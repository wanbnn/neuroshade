#include "neural/model/package.hpp"
#include "neural/runtime/spatial_runtime.hpp"

#include <hip/hip_runtime_api.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void hip_check(hipError_t result, const char* operation) {
    if (result != hipSuccess) {
        throw std::runtime_error(std::string(operation) + " failed: " + hipGetErrorString(result));
    }
}

std::uint64_t hash(std::span<const float> values) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
    const auto byte_count = values.size_bytes();
    std::uint64_t result = 14695981039346656037ULL;
    for (std::size_t index = 0; index < byte_count; ++index) {
        result ^= bytes[index];
        result *= 1099511628211ULL;
    }
    return result;
}

void verify_gain(std::span<const float> input, std::span<const float> output) {
    if (input.size() != output.size()) throw std::runtime_error("neural output size mismatch");
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (std::abs(output[index] - input[index] * 0.75F) > 1.0e-6F) {
            throw std::runtime_error("neural spatial output mismatch at element " +
                                     std::to_string(index));
        }
    }
}

int run(const char* package_path) {
    const auto loaded = neuroshade::neural::load_model_package(package_path);
    if (!loaded.valid()) throw std::runtime_error(loaded.errors.front());
    if (neuroshade::neural::load_model_package(std::string(package_path) + ".missing").valid()) {
        throw std::runtime_error("invalid model package was accepted");
    }

    int device = 0;
    hip_check(hipGetDevice(&device), "hipGetDevice");
    hipDeviceProp_t properties{};
    hip_check(hipGetDeviceProperties(&properties, device), "hipGetDeviceProperties");

    neuroshade::neural::SpatialRuntime runtime(loaded.package, properties.gcnArchName);
    if (!runtime.warmed_up() || !runtime.active()) throw std::runtime_error("model did not warm up");
    if (runtime.input_bytes("color") != runtime.output_bytes()) {
        throw std::runtime_error("reference model input/output allocation mismatch");
    }

    const std::size_t element_count = runtime.input_bytes("color") / sizeof(float);
    std::vector<float> input(element_count);
    std::vector<float> output(element_count);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<float>((index % 257) + 1) / 257.0F;
    }
    hip_check(hipMemcpy(runtime.input_data("color"), input.data(), runtime.input_bytes("color"),
                        hipMemcpyHostToDevice), "hipMemcpy(neural test input)");
    if (runtime.execute() != neuroshade::neural::ExecutionResult::neural) {
        throw std::runtime_error("neural execution unexpectedly selected fallback");
    }
    hip_check(hipMemcpy(output.data(), runtime.output_data(), runtime.output_bytes(),
                        hipMemcpyDeviceToHost), "hipMemcpy(neural test output)");
    verify_gain(input, output);
    const auto input_hash = hash(input);
    const auto neural_hash = hash(output);
    if (input_hash == neural_hash) throw std::runtime_error("bundled model did not change output");

    neuroshade::neural::SpatialRuntime cached_runtime(loaded.package, properties.gcnArchName);
    if (!cached_runtime.cache_hit()) throw std::runtime_error("compiled model cache was not reused");

    hip_check(hipMemcpy(cached_runtime.input_data("color"), input.data(),
                        cached_runtime.input_bytes("color"), hipMemcpyHostToDevice),
              "hipMemcpy(fallback test input)");
    if (setenv("NEUROSHADE_TEST_NEURAL_FAILURE", "1", 1) != 0) {
        throw std::runtime_error("unable to inject neural failure");
    }
    const auto fallback_result = cached_runtime.execute();
    unsetenv("NEUROSHADE_TEST_NEURAL_FAILURE");
    if (fallback_result != neuroshade::neural::ExecutionResult::copy_fallback ||
        cached_runtime.active() || cached_runtime.last_error().empty()) {
        throw std::runtime_error("neural failure was not contained");
    }
    hip_check(hipMemcpy(output.data(), cached_runtime.output_data(), cached_runtime.output_bytes(),
                        hipMemcpyDeviceToHost), "hipMemcpy(fallback output)");
    if (std::memcmp(input.data(), output.data(), input.size() * sizeof(float)) != 0) {
        throw std::runtime_error("copy fallback did not preserve input");
    }

    std::cout << "model=" << loaded.package.manifest.id << '\n'
              << "runtime=MIGraphX gpu=" << properties.name
              << " arch=" << properties.gcnArchName << '\n'
              << "tensor_allocations=" << runtime.tensor_plan().size()
              << " persistent=yes\n"
              << "warmup=complete active_before_first_frame=yes\n"
              << "compiled_cache=hit path=" << runtime.cache_path() << '\n'
              << "input_hash=0x" << std::hex << input_hash << '\n'
              << "neural_hash=0x" << neural_hash << std::dec << '\n'
              << "bundled_model_changed_output=yes\n"
              << "python_runtime=no\n"
              << "failure_fallback=copy pass_disabled=yes error=\""
              << cached_runtime.last_error() << "\"\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("usage: ns-neural-test <package.nsmodel>");
        return run(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << "ns-neural-test: " << error.what() << '\n';
        return 1;
    }
}
