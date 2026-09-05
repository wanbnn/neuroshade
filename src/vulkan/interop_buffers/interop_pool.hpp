#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace neuroshade::interop {

inline constexpr std::size_t kFramesInFlight = 3;

struct ExportableBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize data_size{};
    VkDeviceSize allocation_size{};
};

struct InteropSlot {
    ExportableBuffer input_color;
    ExportableBuffer depth;
    ExportableBuffer motion;
    ExportableBuffer output_color;
};

class InteropBufferPool {
public:
    InteropBufferPool(VkPhysicalDevice physical_device,
                      VkDevice device,
                      std::uint32_t width,
                      std::uint32_t height,
                      std::size_t scratch_count = 1);
    ~InteropBufferPool();

    InteropBufferPool(const InteropBufferPool&) = delete;
    InteropBufferPool& operator=(const InteropBufferPool&) = delete;

    [[nodiscard]] const std::array<InteropSlot, kFramesInFlight>& slots() const noexcept {
        return slots_;
    }
    [[nodiscard]] const std::vector<ExportableBuffer>& scratch() const noexcept { return scratch_; }
    [[nodiscard]] int export_fd(const ExportableBuffer& allocation) const;
    [[nodiscard]] std::size_t allocation_count() const noexcept;

private:
    [[nodiscard]] ExportableBuffer create(VkDeviceSize byte_count);
    void destroy(ExportableBuffer& allocation) noexcept;

    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    PFN_vkGetMemoryFdKHR get_memory_fd_{nullptr};
    std::array<InteropSlot, kFramesInFlight> slots_{};
    std::vector<ExportableBuffer> scratch_;
};

void record_image_to_canonical_buffer(VkCommandBuffer command_buffer,
                                      VkImage source,
                                      VkBuffer destination,
                                      std::uint32_t width,
                                      std::uint32_t height);

void record_canonical_buffer_to_image(VkCommandBuffer command_buffer,
                                      VkBuffer source,
                                      VkImage destination,
                                      std::uint32_t width,
                                      std::uint32_t height);

} // namespace neuroshade::interop
