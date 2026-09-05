#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace neuroshade::resources {

struct ImageInfo {
    std::uint64_t id{};
    VkImage image{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent3D extent{};
    VkImageUsageFlags usage{};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    std::uint64_t memory_id{};
    VkDeviceSize memory_offset{};
    std::uint64_t create_frame{};
    std::uint64_t last_read_frame{};
    std::uint64_t last_write_frame{};
    std::uint32_t write_count{};
    std::uint32_t read_count{};
    std::uint32_t attachment_writes{};
    bool swapchain_image{};
};

class ResourceTracker {
public:
    [[nodiscard]] std::uint64_t track_image(VkImage image, const VkImageCreateInfo& info,
                                            std::uint64_t frame, bool swapchain = false);
    void destroy_image(VkImage image);
    [[nodiscard]] std::uint64_t track_memory(VkDeviceMemory memory);
    void destroy_memory(VkDeviceMemory memory);
    void bind_image(VkImage image, VkDeviceMemory memory, VkDeviceSize offset);
    void track_view(VkImageView view, VkImage image);
    void destroy_view(VkImageView view);
    void observe_view_write(VkImageView view, std::uint64_t frame, bool attachment = true);
    void observe_image_read(VkImage image, std::uint64_t frame);
    [[nodiscard]] std::optional<ImageInfo> image(VkImage image) const;
    [[nodiscard]] std::vector<ImageInfo> images() const;
    [[nodiscard]] std::size_t image_count() const;

private:
    mutable std::mutex mutex_;
    std::uint64_t next_image_id_{1};
    std::uint64_t next_memory_id_{1};
    std::unordered_map<VkImage, ImageInfo> images_;
    std::unordered_map<VkImageView, VkImage> views_;
    std::unordered_map<VkDeviceMemory, std::uint64_t> memories_;
};

}  // namespace neuroshade::resources
