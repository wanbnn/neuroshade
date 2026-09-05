#include "neural/temporal/temporal_runtime.hpp"

#include "neural/temporal/ring_buffer.hpp"

#include <hip/hip_runtime_api.h>
#include <migraphx/migraphx.hpp>
#include <migraphx/version.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neuroshade::neural {
namespace {

void hip_check(hipError_t result, const char* operation) {
    if (result != hipSuccess) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                  hipGetErrorString(result));
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

bool is_history_semantic(std::string_view semantic) {
    return semantic.starts_with("History.");
}

bool is_output_parameter(std::string_view name) {
    return name.find("#output") != std::string_view::npos;
}

} // namespace

class TemporalRuntime::Impl {
public:
    Impl(ModelPackage model_package, std::string gfx_architecture, std::size_t ring_slots)
        : package(std::move(model_package)), ring_slots_(ring_slots) {
        if (package.manifest.history == 0) {
            throw std::runtime_error("model requires temporal runtime (manifest.history=0)");
        }
        cache_file = cache_root() / "neuroshade" / "models" / package.content_hash /
                     gfx_architecture / runtime_version() / "temporal_program.mxr";
        classify_inputs();

        layout_.output_tensor = package.manifest.output.tensor;
        if (layout_.non_history_inputs.empty()) {
            throw std::runtime_error("temporal model must declare at least one non-history input");
        }
        layout_.primary_non_history_tensor = layout_.non_history_inputs.front().tensor;
        for (const auto& input : layout_.history_inputs) {
            history_tensors_.push_back(input.tensor);
        }

        try {
            compile_or_load();
            build_plan();
            allocate_static_tensors();
            allocate_ring_buffer();
            // Static parameters are populated lazily by
            // rebuild_parameters_for_step() during warm_up(); binding
            // them twice via bind_program_parameters() would double-add.
            warm_up();
        } catch (...) {
            release_static_tensors();
            throw;
        }
    }

    ~Impl() { release_static_tensors(); }

    void classify_inputs() {
        for (const auto& input : package.manifest.inputs) {
            if (input.tensor == package.manifest.output.tensor) continue;
            if (is_history_semantic(input.semantic)) {
                layout_.history_inputs.push_back(input);
            } else {
                layout_.non_history_inputs.push_back(input);
            }
        }
    }

    struct StaticAllocation {
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

    void build_plan() {
        const auto shapes = program.get_parameter_shapes();
        for (const char* raw_name : shapes.names()) {
            const std::string name(raw_name);
            const auto shape = shapes[raw_name];
            const bool output = is_output_parameter(name);
            plan_.push_back({name, shape.lengths(), shape.bytes(), output});
            if (output) {
                output_byte_count = shape.bytes();
            }
        }
        if (output_byte_count == 0) {
            throw std::runtime_error("compiled model has no external output tensor");
        }
    }

    void allocate_static_tensors() {
        const auto shapes = program.get_parameter_shapes();
        for (const char* raw_name : shapes.names()) {
            const std::string name(raw_name);
            const auto shape = shapes[raw_name];
            if (is_output_parameter(name)) {
                StaticAllocation alloc{};
                alloc.bytes = output_byte_count;
                alloc.shape = shape;
                hip_check(hipMalloc(&alloc.pointer, alloc.bytes),
                          "hipMalloc(temporal output)");
                hip_check(hipMemset(alloc.pointer, 0, alloc.bytes),
                          "hipMemset(temporal output)");
                alloc.argument = migraphx::argument(shape, alloc.pointer);
                auto [_, inserted] = static_allocations.emplace(name, std::move(alloc));
                if (!inserted) {
                    throw std::runtime_error("duplicate MIGraphX parameter: " + name);
                }
                output_pointer = static_allocations.at(name).pointer;
                continue;
            }
            // History tensors are bound to the ring buffer at evaluation time,
            // not here. Reserve their place in the plan but skip the
            // hipMalloc.
            if (is_history_input(name)) continue;

            StaticAllocation alloc{};
            alloc.bytes = shape.bytes();
            alloc.shape = shape;
            hip_check(hipMalloc(&alloc.pointer, alloc.bytes),
                      "hipMalloc(temporal static tensor)");
            hip_check(hipMemset(alloc.pointer, 0, alloc.bytes),
                      "hipMemset(temporal static tensor)");
            alloc.argument = migraphx::argument(shape, alloc.pointer);
            // Note: parameters binding happens in bind_program_parameters()
            // so we only add each name once regardless of warm-up calls.
            auto [_, inserted] = static_allocations.emplace(name, std::move(alloc));
            if (!inserted) {
                throw std::runtime_error("duplicate MIGraphX parameter: " + name);
            }
        }
    }

    [[nodiscard]] bool is_history_input(std::string_view name) const {
        for (const auto& tensor : history_tensors_) {
            if (tensor == name) return true;
        }
        return false;
    }

    void allocate_ring_buffer() {
        const auto shapes = program.get_parameter_shapes();
        std::vector<TensorAllocation> ring_plan;
        ring_plan.reserve(history_tensors_.size());
        for (const auto& tensor : history_tensors_) {
            const auto shape = shapes[tensor.c_str()];
            history_shapes_[tensor] = shape;
            TensorAllocation entry;
            entry.name = tensor;
            entry.byte_count = shape.bytes();
            ring_plan.push_back(std::move(entry));
        }
        ring_buffer = std::make_unique<TemporalRingBuffer>(std::move(ring_plan), ring_slots_);
    }

    void bind_program_parameters() {
        // Static (non-history) tensors and the output tensor are bound once
        // here, so they are guaranteed to carry a valid argument into the
        // very first MIGraphX eval(). History tensors are bound at
        // rebind_history_to_slot() time because the ring slot pointer
        // changes between frames.
        for (const auto& [name, allocation] : static_allocations) {
            parameters.add(name.c_str(), allocation.argument);
        }
    }

    // Rebuilds `parameters` from the static allocation map and the current
    // ring slot pointers. Called from step() because `migraphx::program_
    // parameters::add` is documented to append; we want to replace any
    // history-tensor binding with the slot pointer that belongs to the
    // current frame.
    void rebuild_parameters_for_step() {
        parameters = migraphx::program_parameters{};
        for (const auto& [name, allocation] : static_allocations) {
            parameters.add(name.c_str(), allocation.argument);
        }
        const auto slot_index =
            history_valid_ ? history_slot_index_ : 0;  // reading zeros when invalid
        for (const auto& tensor : history_tensors_) {
            void* slot_ptr = ring_buffer->slot(tensor, slot_index);
            const auto& shape = history_shapes_.at(tensor);
            parameters.add(tensor.c_str(), migraphx::argument(shape, slot_ptr));
        }
    }

    void rebind_history_to_slot() { /* superseded by rebuild_parameters_for_step() */ }

    void release_static_tensors() noexcept {
        for (auto& [name, allocation] : static_allocations) {
            (void)name;
            if (allocation.pointer != nullptr) {
                (void)hipFree(allocation.pointer);
                allocation.pointer = nullptr;
            }
        }
        output_pointer = nullptr;
        ring_buffer.reset();
    }

    void evaluate() {
        // Output is wired to our persistent output_pointer through
        // bind_program_parameters(), so MIGraphX writes the result in place
        // and eval() returns it as a single entry whose backing memory is
        // our pointer.
        const auto results = program.eval(parameters);
        if (results.size() != 1) {
            throw std::runtime_error("temporal model must produce exactly one output");
        }
        hip_check(hipDeviceSynchronize(), "hipDeviceSynchronize(temporal)");
    }

    void step_warm() {
        rebuild_parameters_for_step();
        evaluate();
        // No history recording during warm-up so first user frame sees an
        // empty buffer; the warm-up itself exercises compile and/or load.
    }

    void warm_up() {
        step_warm();
        step_warm();
        warmed = true;
        enabled = true;
    }

    void record_output_into_current_slot() {
        // For v1's reference temporal models, the next frame's history
        // bindings are derived from the current frame's output. The FrameGraph
        // is responsible for routing specialized history sources (e.g.,
        // multi-frame scene color) into the relevant history slots; at the
        // runtime level we treat every history binding as a snapshot of the
        // output of the previous frame.
        const auto recording_slot = current_slot_index_;
        for (const auto& tensor : history_tensors_) {
            ring_buffer->record_into(tensor, recording_slot, output_pointer);
        }
        hip_check(hipDeviceSynchronize(), "hipDeviceSynchronize(temporal history record)");
    }

    void advance_ring() {
        if (history_valid_) {
            history_slot_index_ = current_slot_index_;
        }
        current_slot_index_ = (current_slot_index_ + 1) % ring_slots_;
        if (!history_valid_) {
            history_valid_ = true;
        }
        ++frame_index_;
    }

    void copy_fallback_into_output() {
        const auto& primary_name = layout_.primary_non_history_tensor;
        const auto& primary = static_allocations.at(primary_name);
        const auto copy_bytes = std::min(primary.bytes, output_byte_count);
        hip_check(hipMemcpy(output_pointer, primary.pointer, copy_bytes,
                            hipMemcpyDeviceToDevice), "hipMemcpy(temporal copy fallback)");
        if (copy_bytes < output_byte_count) {
            hip_check(hipMemset(static_cast<char*>(output_pointer) + copy_bytes, 0,
                                output_byte_count - copy_bytes),
                      "hipMemset(temporal copy fallback)");
        }
        hip_check(hipDeviceSynchronize(), "hipDeviceSynchronize(temporal copy fallback)");
    }

    [[nodiscard]] bool has_required_motion_input() const {
        return std::ranges::any_of(layout_.non_history_inputs, [](const TensorBinding& input) {
            return input.semantic.starts_with("Motion.") && !input.optional;
        });
    }

    void clear_motion_inputs() {
        for (const auto& input : layout_.non_history_inputs) {
            if (!input.semantic.starts_with("Motion.")) continue;
            const auto& allocation = static_allocations.at(input.tensor);
            hip_check(hipMemset(allocation.pointer, 0, allocation.bytes),
                      "hipMemset(missing motion)");
        }
        hip_check(hipDeviceSynchronize(), "hipDeviceSynchronize(missing motion)");
    }

    void reset_history_state() {
        ring_buffer->reset();
        hip_check(hipDeviceSynchronize(), "hipDeviceSynchronize(reset history)");
        history_valid_ = false;
        history_slot_index_ = 0;
        current_slot_index_ = 0;
        frame_index_ = 0;
    }

    ExecutionResult step_internal(bool motion_available) {
        try {
            if (std::getenv("NEUROSHADE_TEST_TEMPORAL_FAILURE") != nullptr) {
                throw std::runtime_error("injected temporal execution failure");
            }
            const bool missing_required_motion =
                !motion_available && has_required_motion_input();
            if (missing_required_motion && package.manifest.missing_motion == "copy") {
                error_message = "required motion input unavailable; copy fallback selected";
                copy_fallback_into_output();
                return ExecutionResult::copy_fallback;
            }
            if (missing_required_motion && package.manifest.missing_motion == "reject") {
                throw std::runtime_error("required motion input unavailable");
            }
            if (missing_required_motion) {
                reset_history_state();
                clear_motion_inputs();
            }

            // TMP-003: history absent on the very first frame. Honour the
            // declared first-frame behavior.
            if (!history_valid_) {
                if (package.manifest.first_frame == "reject") {
                    throw std::runtime_error("temporal model rejects first frame without history");
                }
                // `spatial`, `copy`, and `zero-history` all succeed with the
                // zero-filled history slot bound. ring_buffer::reset was
                // already called on construction, so slot 0 holds zeros.
            }
            rebuild_parameters_for_step();
            evaluate();
            record_output_into_current_slot();
            advance_ring();
            return missing_required_motion ? ExecutionResult::spatial_fallback
                                           : ExecutionResult::temporal;
        } catch (const std::exception& error) {
            enabled = false;
            error_message = error.what();
            copy_fallback_into_output();
            return ExecutionResult::copy_fallback;
        }
    }

    ModelPackage package;
    TemporalInputLayout layout_{};
    migraphx::program program;
    migraphx::program_parameters parameters;
    std::map<std::string, StaticAllocation> static_allocations;
    std::map<std::string, migraphx::shape> history_shapes_;
    std::unique_ptr<TemporalRingBuffer> ring_buffer;
    std::vector<std::string> history_tensors_;
    std::vector<TensorAllocation> plan_;
    std::filesystem::path cache_file;
    void* output_pointer{};
    std::size_t output_byte_count{};
    std::size_t ring_slots_{};
    std::size_t history_slot_index_{};
    std::size_t current_slot_index_{};
    bool loaded_from_cache{};
    bool warmed{};
    bool enabled{};
    bool history_valid_{};
    std::uint64_t frame_index_{};
    std::string error_message;
};

TemporalRuntime::TemporalRuntime(ModelPackage package, std::string gfx_architecture,
                                 std::size_t ring_slots)
    : impl_(std::make_unique<Impl>(std::move(package), std::move(gfx_architecture), ring_slots)) {}

TemporalRuntime::~TemporalRuntime() = default;

const TemporalInputLayout& TemporalRuntime::layout() const noexcept { return impl_->layout_; }

void* TemporalRuntime::current_input(std::string_view tensor) const {
    const auto found = impl_->static_allocations.find(std::string(tensor));
    if (found == impl_->static_allocations.end()) {
        throw std::out_of_range("unknown temporal input tensor: " + std::string(tensor));
    }
    return found->second.pointer;
}

std::size_t TemporalRuntime::tensor_bytes(std::string_view tensor) const {
    const auto found = impl_->static_allocations.find(std::string(tensor));
    if (found != impl_->static_allocations.end()) return found->second.bytes;
    if (impl_->ring_buffer != nullptr) {
        return impl_->ring_buffer->tensor_bytes(tensor);
    }
    throw std::out_of_range("unknown temporal tensor: " + std::string(tensor));
}

void* TemporalRuntime::output_data() const noexcept { return impl_->output_pointer; }

std::size_t TemporalRuntime::output_bytes() const noexcept { return impl_->output_byte_count; }

const std::vector<TensorAllocation>& TemporalRuntime::tensor_plan() const noexcept {
    return impl_->plan_;
}

const std::filesystem::path& TemporalRuntime::cache_path() const noexcept {
    return impl_->cache_file;
}

std::size_t TemporalRuntime::ring_slot_count() const noexcept { return impl_->ring_slots_; }

bool TemporalRuntime::cache_hit() const noexcept { return impl_->loaded_from_cache; }

bool TemporalRuntime::warmed_up() const noexcept { return impl_->warmed; }

bool TemporalRuntime::active() const noexcept { return impl_->enabled; }

bool TemporalRuntime::history_valid() const noexcept { return impl_->history_valid_; }

std::uint64_t TemporalRuntime::frame_index() const noexcept { return impl_->frame_index_; }

const std::string& TemporalRuntime::last_error() const noexcept { return impl_->error_message; }

void TemporalRuntime::upload_current(std::string_view tensor,
                                     std::span<const std::byte> bytes) {
    const auto expected = tensor_bytes(tensor);
    if (bytes.size() != expected) throw std::invalid_argument("temporal input byte count mismatch");
    hip_check(hipMemcpy(current_input(tensor), bytes.data(), bytes.size(),
                        hipMemcpyHostToDevice), "hipMemcpy(temporal input upload)");
}

void TemporalRuntime::download_output(std::span<std::byte> bytes) const {
    if (bytes.size() != output_bytes()) {
        throw std::invalid_argument("temporal output byte count mismatch");
    }
    hip_check(hipMemcpy(bytes.data(), output_data(), bytes.size(),
                        hipMemcpyDeviceToHost), "hipMemcpy(temporal output download)");
}

TemporalRuntime::ExecutionResult TemporalRuntime::step(bool motion_available) {
    return impl_->step_internal(motion_available);
}

void TemporalRuntime::reset_history() {
    impl_->reset_history_state();
}

void TemporalRuntime::invalidate_history(std::string reason) {
    // Same data path as reset_history(); the reason is accepted as a hook for
    // future logging/diagnostics (SPEC §49 crash markers) but is intentionally
    // not surfaced through last_error() because invalidation itself is not
    // an error condition.
    (void)reason;
    reset_history();
}

} // namespace neuroshade::neural
