// ns-temporal-test — qualification harness for the M6 temporal neural runtime.
//
// Mirrors tools/ns-neural-test/main.cpp but exercises:
//   * pre-allocated history ring (TMP-001)
//   * first-frame behavior (TMP-003)
//   * reset_history and invalidate_history (TMP-002)
//   * per-pass profiler (SPEC §23, §49)
//   * injected failure contains via copy fallback (NNR-004)
//
// No Python process is required. The test only depends on the bundled
// `temporal_blend.nsmodel` package installed alongside the binary.

#include "neural/model/package.hpp"
#include "neural/temporal/temporal_runtime.hpp"
#include "telemetry/profiler.hpp"

#include <hip/hip_runtime_api.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void hip_check(hipError_t result, const char* operation) {
    if (result != hipSuccess) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                  hipGetErrorString(result));
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

void fill_test_pattern(std::span<float> buffer) {
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        buffer[index] = 0.5F * static_cast<float>((index % 257) + 1) / 257.0F;
    }
}

void read_back(float* host, const float* device, std::size_t count) {
    hip_check(hipMemcpy(host, device, count * sizeof(float), hipMemcpyDeviceToHost),
              "hipMemcpy(test readback)");
}

std::string gpu_name(int device) {
    hipDeviceProp_t properties{};
    hip_check(hipGetDeviceProperties(&properties, device), "hipGetDeviceProperties");
    return std::string(properties.name);
}

int run(const char* package_path, neuroshade::telemetry::PassProfiler* profiler) {
    using neuroshade::neural::ModelPackage;
    using neuroshade::neural::TemporalRuntime;
    using neuroshade::neural::load_model_package;

    const auto loaded = load_model_package(package_path);
    if (!loaded.valid()) throw std::runtime_error(loaded.errors.front());
    if (load_model_package(std::string(package_path) + ".missing").valid()) {
        throw std::runtime_error("invalid model package was accepted");
    }

    int device = 0;
    hip_check(hipGetDevice(&device), "hipGetDevice");
    hipDeviceProp_t properties{};
    hip_check(hipGetDeviceProperties(&properties, device), "hipGetDeviceProperties");

    profiler->begin_pass("neural.temporal.construct");
    TemporalRuntime runtime(loaded.package, properties.gcnArchName, /*ring_slots=*/4);
    if (!runtime.warmed_up() || !runtime.active()) {
        throw std::runtime_error("temporal model did not warm up");
    }
    profiler->end_pass("neural.temporal.construct");

    const auto element_count = runtime.tensor_bytes("color") / sizeof(float);
    if (runtime.tensor_bytes("history_color") != runtime.tensor_bytes("color")) {
        throw std::runtime_error("history_color allocation mismatch");
    }

    std::vector<float> frame_a(element_count);
    std::vector<float> frame_b(element_count);
    std::vector<float> output(element_count);
    fill_test_pattern(frame_a);
    // Use a different pattern for step 2 to make history depend visibly.
    for (std::size_t index = 0; index < frame_b.size(); ++index) {
        frame_b[index] = 0.5F * static_cast<float>(((index * 31) % 251) + 7) / 257.0F;
    }

    if (runtime.history_valid()) {
        throw std::runtime_error("history must be invalid before first step");
    }

    profiler->begin_pass("neural.temporal.write_inputs_a");
    hip_check(hipMemcpy(runtime.current_input("color"), frame_a.data(),
                        runtime.tensor_bytes("color"), hipMemcpyHostToDevice),
              "hipMemcpy(frame_a)");
    profiler->end_pass("neural.temporal.write_inputs_a");

    profiler->begin_pass("neural.temporal.first_frame_eval");
    const auto first_result = runtime.step();
    profiler->end_pass("neural.temporal.first_frame_eval");
    if (first_result != TemporalRuntime::ExecutionResult::temporal) {
        throw std::runtime_error("first temporal frame did not run");
    }
    if (!runtime.history_valid()) {
        throw std::runtime_error("history should be valid after first step");
    }
    if (runtime.frame_index() != 1) {
        throw std::runtime_error("frame index did not advance after first step");
    }

    read_back(output.data(), static_cast<const float*>(runtime.output_data()),
              element_count);
    const auto first_frame_hash = hash(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(output.data()),
        output.size() * sizeof(float)));
    // First-frame history is zero, so output should equal color + 0
    // (with inputs already scaled to 0.5 in fill_test_pattern).
    for (std::size_t index = 0; index < element_count; ++index) {
        const float expected = frame_a[index];
        const float diff = std::abs(output[index] - expected);
        if (diff > 1.0e-5F) {
            throw std::runtime_error("first frame output mismatch at " +
                                     std::to_string(index));
        }
    }

    profiler->begin_pass("neural.temporal.write_inputs_b");
    hip_check(hipMemcpy(runtime.current_input("color"), frame_b.data(),
                        runtime.tensor_bytes("color"), hipMemcpyHostToDevice),
              "hipMemcpy(frame_b)");
    profiler->end_pass("neural.temporal.write_inputs_b");

    profiler->begin_pass("neural.temporal.second_frame_eval");
    const auto second_result = runtime.step();
    profiler->end_pass("neural.temporal.second_frame_eval");
    if (second_result != TemporalRuntime::ExecutionResult::temporal) {
        throw std::runtime_error("second temporal frame did not run");
    }
    if (runtime.frame_index() != 2) {
        throw std::runtime_error("frame index did not advance after second step");
    }

    read_back(output.data(), static_cast<const float*>(runtime.output_data()),
              element_count);
    const auto second_frame_hash = hash(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(output.data()),
        output.size() * sizeof(float)));
    if (second_frame_hash == first_frame_hash) {
        throw std::runtime_error("history was not consumed on the second frame");
    }
    // The second frame observed a non-zero history_color equal to the
    // first-frame output (= frame_a under the simplified blend model).
    for (std::size_t index = 0; index < element_count; ++index) {
        const float expected = frame_b[index] + frame_a[index];
        const float diff = std::abs(output[index] - expected);
        if (diff > 1.0e-5F) {
            throw std::runtime_error("second frame index=" + std::to_string(index) +
                                     " got=" + std::to_string(output[index]) +
                                     " expected=" + std::to_string(expected));
        }
    }

    // Explicit reset replays the first-frame hash exactly. SPEC TMP-002.
    profiler->begin_pass("neural.temporal.reset");
    runtime.reset_history();
    profiler->end_pass("neural.temporal.reset");
    if (runtime.history_valid() || runtime.frame_index() != 0) {
        throw std::runtime_error("reset_history did not reset state");
    }
    hip_check(hipMemcpy(runtime.current_input("color"), frame_a.data(),
                        runtime.tensor_bytes("color"), hipMemcpyHostToDevice),
              "hipMemcpy(post-reset)");
    profiler->begin_pass("neural.temporal.post_reset_eval");
    const auto post_reset_result = runtime.step();
    profiler->end_pass("neural.temporal.post_reset_eval");
    if (post_reset_result != TemporalRuntime::ExecutionResult::temporal) {
        throw std::runtime_error("post-reset frame did not run");
    }
    read_back(output.data(), static_cast<const float*>(runtime.output_data()),
              element_count);
    const auto post_reset_hash = hash(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(output.data()),
        output.size() * sizeof(float)));
    if (post_reset_hash != first_frame_hash) {
        throw std::runtime_error("history reset did not restore first-frame behavior");
    }

    // Invalidate also returns to first-frame behavior. SPEC TMP-002 covers
    // swapchain recreation and resolution change by name.
    profiler->begin_pass("neural.temporal.invalidate");
    runtime.invalidate_history("swapchain_recreate");
    profiler->end_pass("neural.temporal.invalidate");
    if (runtime.history_valid()) {
        throw std::runtime_error("invalidate_history did not invalidate state");
    }
    hip_check(hipMemcpy(runtime.current_input("color"), frame_a.data(),
                        runtime.tensor_bytes("color"), hipMemcpyHostToDevice),
              "hipMemcpy(post-invalidate)");
    profiler->begin_pass("neural.temporal.post_invalidate_eval");
    const auto post_invalidate_result = runtime.step();
    profiler->end_pass("neural.temporal.post_invalidate_eval");
    if (post_invalidate_result != TemporalRuntime::ExecutionResult::temporal) {
        throw std::runtime_error("post-invalidate frame did not run");
    }
    read_back(output.data(), static_cast<const float*>(runtime.output_data()),
              element_count);
    const auto post_invalidate_hash = hash(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(output.data()),
        output.size() * sizeof(float)));
    if (post_invalidate_hash != first_frame_hash) {
        throw std::runtime_error(
            "invalidate_history did not return to first-frame behavior");
    }

    // Compiled-model cache must be reused on a second instance (NNR-003).
    profiler->begin_pass("neural.temporal.cache_reload");
    TemporalRuntime cached(loaded.package, properties.gcnArchName, /*ring_slots=*/4);
    profiler->end_pass("neural.temporal.cache_reload");
    if (!cached.cache_hit()) {
        throw std::runtime_error("temporal compiled model cache was not reused");
    }

    // Injected failure contains via the copy fallback path (NNR-004).
    hip_check(hipMemcpy(cached.current_input("color"), frame_a.data(),
                        cached.tensor_bytes("color"), hipMemcpyHostToDevice),
              "hipMemcpy(fallback test input)");
    if (setenv("NEUROSHADE_TEST_TEMPORAL_FAILURE", "1", 1) != 0) {
        throw std::runtime_error("unable to inject temporal failure");
    }
    profiler->begin_pass("neural.temporal.fallback_eval");
    const auto fallback_result = cached.step();
    profiler->end_pass("neural.temporal.fallback_eval");
    unsetenv("NEUROSHADE_TEST_TEMPORAL_FAILURE");
    if (fallback_result != TemporalRuntime::ExecutionResult::copy_fallback ||
        cached.active() || cached.last_error().empty()) {
        throw std::runtime_error("temporal failure was not contained");
    }
    read_back(output.data(), static_cast<const float*>(cached.output_data()),
              element_count);
    if (std::memcmp(output.data(), frame_a.data(), frame_a.size() * sizeof(float)) != 0) {
        throw std::runtime_error("copy fallback did not preserve current frame input");
    }

    // Re-enabling is not automatic; the cached runtime must stay disabled.
    if (cached.active()) {
        throw std::runtime_error("runtime should remain disabled after failure");
    }

    // Format profiler timings.
    const auto fmt = [&](const char* pass) {
        const double avg = profiler->avg_ms(pass);
        const double last = profiler->last_ms(pass);
        const std::size_t n = profiler->samples(pass);
        std::ostringstream out;
        out << pass << " ms (avg=" << std::fixed << std::setprecision(3) << avg
            << ", last=" << last << ", samples=" << n << ")";
        return out.str();
    };

    std::cout << "model=" << loaded.package.manifest.id << '\n'
              << "runtime=MIGraphX gpu=" << gpu_name(device)
              << " arch=" << properties.gcnArchName << '\n'
              << "tensor_allocations=" << runtime.tensor_plan().size()
              << " persistent=tensors_and_history\n"
              << "history_ring=" << runtime.ring_slot_count() << "_slots\n"
              << "warmup=complete active_before_first_frame=yes\n"
              << "compiled_cache=hit path=" << runtime.cache_path() << '\n'
              << "first_frame=spatial history_valid=no\n"
              << "step_1_uses_history=yes frame_index=" << runtime.frame_index() << '\n'
              << "step_1_checksum=0x" << std::hex << first_frame_hash << std::dec << '\n'
              << "step_2_checksum=0x" << std::hex << second_frame_hash << std::dec
              << " differs_from_step_1=yes\n"
              << "reset_replays_first_frame=yes checksum=0x" << std::hex << post_reset_hash
              << std::dec << '\n'
              << "invalidate_replays_first_frame=yes checksum=0x" << std::hex
              << post_invalidate_hash << std::dec << '\n'
              << "python_runtime=no\n"
              << "failure_fallback=copy pass_disabled=yes last_error=\""
              << cached.last_error() << "\"\n"
              << "gpu_profiler=" << (profiler->gpu_backed() ? "hip_events" : "cpu_clock")
              << '\n'
              << "pass_neural.temporal.first_frame_eval="
              << profiler->format_ms(profiler->avg_ms("neural.temporal.first_frame_eval"))
              << "ms\n"
              << "pass_neural.temporal.second_frame_eval="
              << profiler->format_ms(profiler->avg_ms("neural.temporal.second_frame_eval"))
              << "ms\n"
              << "pass_neural.temporal.history_record="
              << profiler->format_ms(
                     profiler->avg_ms("neural.temporal.history_record"))
              << "ms (informational)\n"
              << "summary: " << fmt("neural.temporal.first_frame_eval") << '\n';
    (void)second_frame_hash;  // already printed above
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::invalid_argument("usage: ns-temporal-test <package.nsmodel>");
        }
        neuroshade::telemetry::PassProfiler profiler;
        return run(argv[1], &profiler);
    } catch (const std::exception& error) {
        std::cerr << "ns-temporal-test: " << error.what() << '\n';
        return 1;
    }
}
