#include <hip/hip_runtime.h>

#include <cstddef>
#include <cstdint>

namespace {

struct alignas(8) Half4 {
    std::uint16_t r;
    std::uint16_t g;
    std::uint16_t b;
    std::uint16_t a;
};

__global__ void hash_kernel(const std::uint8_t* bytes,
                            std::size_t byte_count,
                            std::uint64_t* result) {
    if (blockIdx.x != 0 || threadIdx.x != 0) return;
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t index = 0; index < byte_count; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    *result = hash;
}

__global__ void color_effect_kernel(const Half4* input,
                                    Half4* output,
                                    std::size_t pixel_count) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= pixel_count) return;
    const Half4 source = input[index];
    output[index] = Half4{source.b, source.r, source.g, source.a};
}

} // namespace

extern "C" hipError_t ns_launch_hash(const void* bytes,
                                      std::size_t byte_count,
                                      std::uint64_t* result) {
    hipLaunchKernelGGL(hash_kernel, dim3(1), dim3(1), 0, nullptr,
                       static_cast<const std::uint8_t*>(bytes), byte_count, result);
    return hipGetLastError();
}

extern "C" hipError_t ns_launch_color_effect(const void* input,
                                              void* output,
                                              std::size_t pixel_count) {
    constexpr unsigned int block_size = 256;
    const auto blocks = static_cast<unsigned int>((pixel_count + block_size - 1) / block_size);
    hipLaunchKernelGGL(color_effect_kernel, dim3(blocks), dim3(block_size), 0, nullptr,
                       static_cast<const Half4*>(input), static_cast<Half4*>(output), pixel_count);
    return hipGetLastError();
}
