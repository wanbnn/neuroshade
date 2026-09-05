#include "neural/runtime/spatial_runtime.hpp"

#include <hip/hip_runtime_api.h>
#include <migraphx/migraphx.hpp>
#include <migraphx/version.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>

namespace neuroshade::neural {
namespace {

void hip_check(hipError_t result, const char* operation) {
    if (result != hipSuccess) {
        throw std::runtime_error(std::string(operation) + " failed: " + hipGetErrorString(result));
    }
}

std::filesystem::path cache_root() {
    if (const char* configured = std::getenv("XDG_CACHE_HOME")) return configured;
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home) / ".cache";
    return std::filesystem::temp_directory_path();
}

std::string runtime_version() {
    return std::to_string(MIGRAPHX_VERSION_MAJOR) + "." +
           std::to_string(MIGRAPHX_VERSION_MINOR) + "." +
           std::to_string(MIGRAPHX_VERSION_PATCH);
}

bool is_output_parameter(std::string_view name) {
    return name.find("#output") != std::string_view::npos;
}

} // namespace

std::string active_hip_architecture() {
    int device = 0;
    hip_check(hipGetDevice(&device), "hipGetDevice");
    hipDeviceProp_t properties{};
    hip_check(hipGetDeviceProperties(&properties, device), "hipGetDeviceProperties");
    return properties.gcnArchName;
}

class SpatialRuntime::Impl {
public:
    Impl(ModelPackage model_package, std::string gfx_architecture)
        : package(std::move(model_package)) {
        cache_file = cache_root() / "neuroshade" / "models" / package.content_hash /
                     gfx_architecture / runtime_version() / "program.mxr";
        try {
            compile_or_load();
            allocate_tensors();
            warm_up();
        } catch (...) {
            release_tensors();
            throw;
        }
    }

    ~Impl() { release_tensors(); }

    void release_tensors() noexcept {
        for (auto& [name, allocation] : allocations) {
            (void)name;
            if (allocation.pointer != nullptr) {
                (void)hipFree(allocation.pointer);
                allocation.pointer = nullptr;
            }
        }
    }

    struct Allocation {
        void* pointer{};
        std::size_t bytes{};
        migraphx::shape shape;
        migraphx::argument argument;
    };

    void compile_or_load() {
        if (std::filesystem::is_regular_file(cache_file)) {
            try {
                program = migraphx::load(cache_file.c_str());
                loaded_from_cache = true;
                return;
            } catch (...) {
                std::error_code ignored;
                std::filesystem::remove(cache_file, ignored);
            }
        }

        program = migraphx::parse_onnx(package.onnx_path.c_str());
        migraphx::compile_options options;
        options.set_offload_copy(false);
        program.compile(migraphx::target("gpu"), options);
        std::error_code error;
        std::filesystem::create_directories(cache_file.parent_path(), error);
        if (!error) migraphx::save(program, cache_file.c_str());
    }

    void allocate_tensors() {
        const auto shapes = program.get_parameter_shapes();
        for (const char* raw_name : shapes.names()) {
            const std::string name(raw_name);
            const auto shape = shapes[raw_name];
            Allocation allocation{};
            allocation.bytes = shape.bytes();
            allocation.shape = shape;
            hip_check(hipMalloc(&allocation.pointer, allocation.bytes), "hipMalloc(neural tensor)");
            allocation.argument = migraphx::argument(shape, allocation.pointer);
            hip_check(hipMemset(allocation.pointer, 0, allocation.bytes), "hipMemset(neural tensor)");
            parameters.add(name.c_str(), allocation.argument);
            const bool output = is_output_parameter(name);
            plan.push_back({name, shape.lengths(), allocation.bytes, output});
            auto [position, inserted] = allocations.emplace(name, std::move(allocation));
            if (!inserted) throw std::runtime_error("duplicate MIGraphX parameter: " + name);
            if (output) {
                output_pointer = position->second.pointer;
                output_byte_count = position->second.bytes;
            }
        }
        if (output_pointer == nullptr) throw std::runtime_error("compiled model has no external output tensor");
        for (const auto& input : package.manifest.inputs) {
            if (!input.optional && allocations.find(input.tensor) == allocations.end()) {
                throw std::runtime_error("model input missing after compile: " + input.tensor);
            }
        }
    }

    void evaluate() {
        const auto results = program.eval(parameters);
        if (results.size() != 1) throw std::runtime_error("spatial model must produce exactly one output");
        const auto result = results[0];
        if (result.get_shape().bytes() != output_byte_count) {
            throw std::runtime_error("MIGraphX output byte count changed");
        }
        if (result.data() != output_pointer) {
            hip_check(hipMemcpy(output_pointer, result.data(), output_byte_count,
                                hipMemcpyDeviceToDevice), "hipMemcpy(neural output)");
        }
        hip_check(hipDeviceSynchronize(), "hipDeviceSynchronize(neural)");
    }

    void warm_up() {
        evaluate();
        evaluate();
        warmed = true;
        enabled = true;
    }

    ExecutionResult execute() {
        try {
            if (std::getenv("NEUROSHADE_TEST_NEURAL_FAILURE") != nullptr) {
                throw std::runtime_error("injected neural execution failure");
            }
            evaluate();
            return ExecutionResult::neural;
        } catch (const std::exception& error) {
            enabled = false;
            error_message = error.what();
            const auto& primary_input = allocations.at(package.manifest.inputs.front().tensor);
            const auto copy_bytes = std::min(primary_input.bytes, output_byte_count);
            hip_check(hipMemcpy(output_pointer, primary_input.pointer, copy_bytes,
                                hipMemcpyDeviceToDevice), "hipMemcpy(copy fallback)");
            if (copy_bytes < output_byte_count) {
                hip_check(hipMemset(static_cast<char*>(output_pointer) + copy_bytes, 0,
                                    output_byte_count - copy_bytes), "hipMemset(copy fallback)");
            }
            hip_check(hipDeviceSynchronize(), "hipDeviceSynchronize(copy fallback)");
            return ExecutionResult::copy_fallback;
        }
    }

    ModelPackage package;
    migraphx::program program;
    migraphx::program_parameters parameters;
    std::map<std::string, Allocation> allocations;
    std::vector<TensorAllocation> plan;
    std::filesystem::path cache_file;
    void* output_pointer{};
    std::size_t output_byte_count{};
    bool loaded_from_cache{};
    bool warmed{};
    bool enabled{};
    std::string error_message;
};

SpatialRuntime::SpatialRuntime(ModelPackage package, std::string gfx_architecture)
    : impl_(std::make_unique<Impl>(std::move(package), std::move(gfx_architecture))) {}

SpatialRuntime::~SpatialRuntime() = default;

void* SpatialRuntime::input_data(std::string_view tensor_name) const {
    const auto found = impl_->allocations.find(std::string(tensor_name));
    if (found == impl_->allocations.end() || is_output_parameter(found->first)) {
        throw std::out_of_range("unknown neural input tensor: " + std::string(tensor_name));
    }
    return found->second.pointer;
}

void* SpatialRuntime::output_data() const noexcept { return impl_->output_pointer; }

std::size_t SpatialRuntime::input_bytes(std::string_view tensor_name) const {
    const auto found = impl_->allocations.find(std::string(tensor_name));
    if (found == impl_->allocations.end()) throw std::out_of_range("unknown neural input tensor");
    return found->second.bytes;
}

std::size_t SpatialRuntime::output_bytes() const noexcept { return impl_->output_byte_count; }

const std::vector<TensorAllocation>& SpatialRuntime::tensor_plan() const noexcept { return impl_->plan; }

const std::filesystem::path& SpatialRuntime::cache_path() const noexcept { return impl_->cache_file; }

bool SpatialRuntime::cache_hit() const noexcept { return impl_->loaded_from_cache; }

bool SpatialRuntime::warmed_up() const noexcept { return impl_->warmed; }

bool SpatialRuntime::active() const noexcept { return impl_->enabled; }

const std::string& SpatialRuntime::last_error() const noexcept { return impl_->error_message; }

void SpatialRuntime::upload_input(std::string_view tensor_name,
                                  std::span<const std::byte> bytes) {
    const auto expected = input_bytes(tensor_name);
    if (bytes.size() != expected) throw std::invalid_argument("neural input byte count mismatch");
    hip_check(hipMemcpy(input_data(tensor_name), bytes.data(), bytes.size(),
                        hipMemcpyHostToDevice), "hipMemcpy(neural input upload)");
}

void SpatialRuntime::download_output(std::span<std::byte> bytes) const {
    if (bytes.size() != output_bytes()) {
        throw std::invalid_argument("neural output byte count mismatch");
    }
    hip_check(hipMemcpy(bytes.data(), output_data(), bytes.size(),
                        hipMemcpyDeviceToHost), "hipMemcpy(neural output download)");
}

ExecutionResult SpatialRuntime::execute() { return impl_->execute(); }

} // namespace neuroshade::neural
