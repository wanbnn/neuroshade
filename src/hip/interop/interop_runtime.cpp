#include "hip/interop/interop_runtime.hpp"

#include <hip/hip_runtime_api.h>

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace neuroshade::hip {
namespace {

void check(hipError_t result, const char* operation) {
    if (result != hipSuccess) {
        throw std::runtime_error(std::string(operation) + " failed: " + hipGetErrorString(result));
    }
}

extern "C" hipError_t ns_launch_hash(const void*, std::size_t, std::uint64_t*);
extern "C" hipError_t ns_launch_color_effect(const void*, void*, std::size_t);

} // namespace

ImportedBuffer::ImportedBuffer(int opaque_fd,
                               std::size_t allocation_size,
                               std::size_t data_size)
    : data_size_(data_size) {
    hipExternalMemoryHandleDesc handle{};
    handle.type = hipExternalMemoryHandleTypeOpaqueFd;
    handle.handle.fd = opaque_fd;
    handle.size = allocation_size;
    hipExternalMemory_t imported = nullptr;
    const auto import_result = hipImportExternalMemory(&imported, &handle);
    if (import_result != hipSuccess) {
        close(opaque_fd);
        check(import_result, "hipImportExternalMemory");
    }
    memory_ = imported;

    hipExternalMemoryBufferDesc mapping{};
    mapping.offset = 0;
    mapping.size = data_size;
    const auto map_result = hipExternalMemoryGetMappedBuffer(&data_, imported, &mapping);
    if (map_result != hipSuccess) {
        (void)hipDestroyExternalMemory(imported);
        memory_ = nullptr;
        check(map_result, "hipExternalMemoryGetMappedBuffer");
    }
}

ImportedBuffer::~ImportedBuffer() {
    if (data_ != nullptr) (void)hipFree(data_);
    if (memory_ != nullptr) {
        (void)hipDestroyExternalMemory(static_cast<hipExternalMemory_t>(memory_));
    }
}

HipDeviceInfo select_device() {
    int count = 0;
    check(hipGetDeviceCount(&count), "hipGetDeviceCount");
    if (count == 0) throw std::runtime_error("HIP found no GPU");

    int requested_ordinal = -1;
    if (const char* configured = std::getenv("NEUROSHADE_HIP_DEVICE")) {
        char* end = nullptr;
        const long parsed = std::strtol(configured, &end, 10);
        if (end != configured && *end == '\0' && parsed >= 0 && parsed < count) {
            requested_ordinal = static_cast<int>(parsed);
        }
    }

    int best = requested_ordinal;
    int best_score = std::numeric_limits<int>::min();
    for (int ordinal = 0; ordinal < count && requested_ordinal < 0; ++ordinal) {
        hipDeviceProp_t properties{};
        check(hipGetDeviceProperties(&properties, ordinal), "hipGetDeviceProperties");
        int score = properties.integrated ? 0 : 100;
        const std::string_view architecture(properties.gcnArchName);
        if (architecture.starts_with("gfx12")) score += 50;
        if (score > best_score) {
            best = ordinal;
            best_score = score;
        }
    }
    if (best < 0) throw std::runtime_error("NEUROSHADE_HIP_DEVICE is invalid");
    check(hipSetDevice(best), "hipSetDevice");

    hipDeviceProp_t properties{};
    check(hipGetDeviceProperties(&properties, best), "hipGetDeviceProperties(selected)");
    int runtime_version = 0;
    check(hipRuntimeGetVersion(&runtime_version), "hipRuntimeGetVersion");
    return {best, properties.name, properties.gcnArchName, runtime_version};
}

std::uint64_t hash_bytes(const void* device_data, std::size_t byte_count) {
    std::uint64_t* device_hash = nullptr;
    check(hipMalloc(reinterpret_cast<void**>(&device_hash), sizeof(*device_hash)), "hipMalloc(hash)");
    const auto launch = ns_launch_hash(device_data, byte_count, device_hash);
    if (launch != hipSuccess) {
        (void)hipFree(device_hash);
        check(launch, "ns_launch_hash");
    }
    std::uint64_t result = 0;
    const auto copy = hipMemcpy(&result, device_hash, sizeof(result), hipMemcpyDeviceToHost);
    (void)hipFree(device_hash);
    check(copy, "hipMemcpy(hash result)");
    return result;
}

void run_color_effect(const void* input, void* output, std::size_t pixel_count) {
    check(ns_launch_color_effect(input, output, pixel_count), "ns_launch_color_effect");
    check(hipDeviceSynchronize(), "hipDeviceSynchronize(color effect)");
}

HostStagingWorkspace::HostStagingWorkspace(std::size_t byte_count) : byte_count_(byte_count) {
    try {
        check(hipHostMalloc(&pinned_input_, byte_count_), "hipHostMalloc(input)");
        check(hipHostMalloc(&pinned_output_, byte_count_), "hipHostMalloc(output)");
        check(hipMalloc(&device_input_, byte_count_), "hipMalloc(input)");
        check(hipMalloc(&device_output_, byte_count_), "hipMalloc(output)");
    } catch (...) {
        if (device_output_ != nullptr) (void)hipFree(device_output_);
        if (device_input_ != nullptr) (void)hipFree(device_input_);
        if (pinned_output_ != nullptr) (void)hipHostFree(pinned_output_);
        if (pinned_input_ != nullptr) (void)hipHostFree(pinned_input_);
        throw;
    }
}

HostStagingWorkspace::~HostStagingWorkspace() {
    if (device_output_ != nullptr) (void)hipFree(device_output_);
    if (device_input_ != nullptr) (void)hipFree(device_input_);
    if (pinned_output_ != nullptr) (void)hipHostFree(pinned_output_);
    if (pinned_input_ != nullptr) (void)hipHostFree(pinned_input_);
}

void HostStagingWorkspace::process(const void* host_input, void* host_output) {
    std::memcpy(pinned_input_, host_input, byte_count_);
    check(hipMemcpy(device_input_, pinned_input_, byte_count_, hipMemcpyHostToDevice),
          "hipMemcpy(staging upload)");
    run_color_effect(device_input_, device_output_, byte_count_ / 8);
    check(hipMemcpy(pinned_output_, device_output_, byte_count_, hipMemcpyDeviceToHost),
          "hipMemcpy(staging download)");
    std::memcpy(host_output, pinned_output_, byte_count_);
}

} // namespace neuroshade::hip
