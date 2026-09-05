// Qualification harness for M8 true 2x temporal super-resolution.

#include "neural/model/package.hpp"
#include "neural/temporal/temporal_runtime.hpp"
#include "telemetry/profiler.hpp"

#include <hip/hip_runtime_api.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t input_width = 32;
constexpr std::size_t input_height = 32;
constexpr std::size_t output_width = 64;
constexpr std::size_t output_height = 64;
constexpr std::size_t channels = 4;

void hip_check(hipError_t result, const char* operation) {
    if (result != hipSuccess) {
        throw std::runtime_error(std::string(operation) + " failed: " + hipGetErrorString(result));
    }
}

std::uint64_t hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t result = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

std::uint64_t hash_floats(std::span<const float> values) {
    return hash({reinterpret_cast<const std::uint8_t*>(values.data()), values.size_bytes()});
}

void fill_low_res(std::span<float> buffer) {
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        buffer[index] = static_cast<float>((index % 251) + 1) / 257.0F;
    }
}

void fill_motion(std::span<float> buffer, std::uint64_t seed) {
    std::uint64_t salt = seed;
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        salt = 1099511628211ULL * salt ^ (buffer.size() + index);
        buffer[index] = static_cast<float>((salt >> 8) & 0xFF) / 255.0F - 0.5F;
    }
}

void read_back(std::span<float> host, const void* device) {
    hip_check(hipMemcpy(host.data(), device, host.size_bytes(), hipMemcpyDeviceToHost),
              "hipMemcpy(readback)");
}

float input_at(std::span<const float> input, std::size_t channel,
               std::size_t output_y, std::size_t output_x) {
    const std::size_t index = channel * input_width * input_height +
                              (output_y / 2) * input_width + output_x / 2;
    return input[index];
}

void verify_frame(std::span<const float> actual, std::span<const float> low_res,
                  std::span<const float> motion, std::span<const float> history,
                  const char* frame_name) {
    for (std::size_t channel = 0; channel < channels; ++channel) {
        for (std::size_t y = 0; y < output_height; ++y) {
            for (std::size_t x = 0; x < output_width; ++x) {
                const std::size_t output_index =
                    channel * output_width * output_height + y * output_width + x;
                const float expected = 0.5F * input_at(low_res, channel, y, x) +
                                       0.1F * input_at(motion, channel, y, x) +
                                       history[output_index];
                if (std::abs(actual[output_index] - expected) > 2.0e-5F) {
                    throw std::runtime_error(std::string(frame_name) +
                                             " numerical mismatch at output index " +
                                             std::to_string(output_index) + " expected=" +
                                             std::to_string(expected) + " actual=" +
                                             std::to_string(actual[output_index]));
                }
            }
        }
    }
}

int run(const char* package_path, neuroshade::telemetry::PassProfiler* profiler) {
    using neuroshade::neural::TemporalRuntime;
    using neuroshade::neural::load_model_package;

    const auto loaded = load_model_package(package_path);
    if (!loaded.valid()) throw std::runtime_error(loaded.errors.front());
    if (loaded.package.manifest.scale_x != 2.0F ||
        loaded.package.manifest.scale_y != 2.0F ||
        loaded.package.manifest.missing_motion != "spatial") {
        throw std::runtime_error("temporal SR manifest policy mismatch");
    }

    int device = 0;
    hip_check(hipGetDevice(&device), "hipGetDevice");
    hipDeviceProp_t properties{};
    hip_check(hipGetDeviceProperties(&properties, device), "hipGetDeviceProperties");

    profiler->begin_pass("neural.temporal_sr.construct");
    TemporalRuntime runtime(loaded.package, properties.gcnArchName, 2);
    profiler->end_pass("neural.temporal_sr.construct");
    if (!runtime.warmed_up() || !runtime.active()) {
        throw std::runtime_error("temporal_sr_2x model did not warm up");
    }

    const std::size_t input_elements = channels * input_width * input_height;
    const std::size_t output_elements = channels * output_width * output_height;
    if (runtime.tensor_bytes("inp_low_res") != input_elements * sizeof(float) ||
        runtime.tensor_bytes("inp_motion") != input_elements * sizeof(float) ||
        runtime.tensor_bytes("inp_history") != output_elements * sizeof(float) ||
        runtime.output_bytes() != output_elements * sizeof(float)) {
        throw std::runtime_error("compiled tensor dimensions do not describe a true 2x model");
    }

    std::vector<float> low_res(input_elements);
    std::vector<float> motion_a(input_elements);
    std::vector<float> motion_b(input_elements);
    std::vector<float> zero_motion(input_elements, 0.0F);
    std::vector<float> zero_history(output_elements, 0.0F);
    std::vector<float> output_a(output_elements);
    std::vector<float> output_b(output_elements);
    std::vector<float> output_fallback(output_elements);
    fill_low_res(low_res);
    fill_motion(motion_a, 0x1);
    fill_motion(motion_b, 0xC0FFEE);

    hip_check(hipMemcpy(runtime.current_input("inp_low_res"), low_res.data(),
                        low_res.size() * sizeof(float), hipMemcpyHostToDevice),
              "hipMemcpy(low_res)");
    hip_check(hipMemcpy(runtime.current_input("inp_motion"), motion_a.data(),
                        motion_a.size() * sizeof(float), hipMemcpyHostToDevice),
              "hipMemcpy(motion A)");
    profiler->begin_pass("neural.temporal_sr.step_a");
    const auto step_a = runtime.step();
    profiler->end_pass("neural.temporal_sr.step_a");
    if (step_a != TemporalRuntime::ExecutionResult::temporal) {
        throw std::runtime_error("temporal SR frame A did not run");
    }
    read_back(output_a, runtime.output_data());
    verify_frame(output_a, low_res, motion_a, zero_history, "frame A");

    hip_check(hipMemcpy(runtime.current_input("inp_motion"), motion_b.data(),
                        motion_b.size() * sizeof(float), hipMemcpyHostToDevice),
              "hipMemcpy(motion B)");
    profiler->begin_pass("neural.temporal_sr.step_b");
    const auto step_b = runtime.step();
    profiler->end_pass("neural.temporal_sr.step_b");
    if (step_b != TemporalRuntime::ExecutionResult::temporal) {
        throw std::runtime_error("temporal SR frame B did not run");
    }
    read_back(output_b, runtime.output_data());
    verify_frame(output_b, low_res, motion_b, output_a, "frame B");

    const auto output_a_hash = hash_floats(output_a);
    const auto output_b_hash = hash_floats(output_b);
    if (output_a_hash == output_b_hash) {
        throw std::runtime_error("motion change did not affect temporal output");
    }

    profiler->begin_pass("neural.temporal_sr.missing_motion");
    const auto fallback = runtime.step(false);
    profiler->end_pass("neural.temporal_sr.missing_motion");
    if (fallback != TemporalRuntime::ExecutionResult::spatial_fallback || !runtime.active()) {
        throw std::runtime_error("missing motion did not select active spatial fallback");
    }
    read_back(output_fallback, runtime.output_data());
    verify_frame(output_fallback, low_res, zero_motion, zero_history, "spatial fallback");

    std::cout << "model=" << loaded.package.manifest.id << '\n'
              << "runtime=MIGraphX gpu=" << properties.name
              << " arch=" << properties.gcnArchName << '\n'
              << "input_extent=32x32 output_extent=64x64 true_2x=yes\n"
              << "output_bytes=" << runtime.output_bytes() << '\n'
              << "history_ring=" << runtime.ring_slot_count() << "_slots\n"
              << "warmup=complete active=yes\n"
              << "motion_propagates_to_output=yes\n"
              << "missing_motion_policy=spatial selected=yes pass_active=yes\n"
              << "step_1_checksum=0x" << std::hex << output_a_hash << '\n'
              << "step_2_checksum=0x" << output_b_hash << std::dec << '\n'
              << "pass_neural.temporal_sr.first_frame_eval="
              << profiler->format_ms(profiler->avg_ms("neural.temporal_sr.step_a"))
              << "ms (avg)\n"
              << "python_runtime=no\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("usage: ns-temporal-sr-test <package.nsmodel>");
        neuroshade::telemetry::PassProfiler profiler;
        return run(argv[1], &profiler);
    } catch (const std::exception& error) {
        std::cerr << "ns-temporal-sr-test: " << error.what() << '\n';
        return 1;
    }
}
