#include "vulkan/interop_buffers/interop_pool.hpp"

#include "vulkan/interop_buffers/canonical.hpp"

#include <stdexcept>
#include <string>

namespace neuroshade::interop {
namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                                 std::to_string(result));
    }
}

std::uint32_t device_local_memory_type(VkPhysicalDevice physical_device,
                                       std::uint32_t allowed_types) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        const std::uint32_t bit = std::uint32_t{1} << index;
        if ((allowed_types & bit) != 0 &&
            (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            return index;
        }
    }
    throw std::runtime_error("no device-local memory type for external Vulkan buffer");
}

} // namespace

InteropBufferPool::InteropBufferPool(VkPhysicalDevice physical_device,
                                     VkDevice device,
                                     std::uint32_t width,
                                     std::uint32_t height,
                                     std::size_t scratch_count)
    : physical_device_(physical_device), device_(device) {
    get_memory_fd_ = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
    if (get_memory_fd_ == nullptr) throw std::runtime_error("vkGetMemoryFdKHR is unavailable");

    VkPhysicalDeviceExternalBufferInfo query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    query.flags = 0;
    query.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalBufferProperties external{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(physical_device_, &query, &external);
    constexpr auto required = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    if ((external.externalMemoryProperties.externalMemoryFeatures & required) != required) {
        throw std::runtime_error("Vulkan opaque-fd buffers are not exportable and importable");
    }

    try {
        const auto color_size = static_cast<VkDeviceSize>(color_byte_count(width, height));
        const auto depth_size = static_cast<VkDeviceSize>(depth_byte_count(width, height));
        const auto motion_size = static_cast<VkDeviceSize>(motion_byte_count(width, height));
        for (auto& slot : slots_) {
            slot.input_color = create(color_size);
            slot.depth = create(depth_size);
            slot.motion = create(motion_size);
            slot.output_color = create(color_size);
        }
        scratch_.reserve(scratch_count);
        for (std::size_t index = 0; index < scratch_count; ++index) {
            scratch_.push_back(create(color_size));
        }
    } catch (...) {
        for (auto& slot : slots_) {
            destroy(slot.input_color);
            destroy(slot.depth);
            destroy(slot.motion);
            destroy(slot.output_color);
        }
        for (auto& allocation : scratch_) destroy(allocation);
        throw;
    }
}

InteropBufferPool::~InteropBufferPool() {
    for (auto& slot : slots_) {
        destroy(slot.input_color);
        destroy(slot.depth);
        destroy(slot.motion);
        destroy(slot.output_color);
    }
    for (auto& allocation : scratch_) destroy(allocation);
}

ExportableBuffer InteropBufferPool::create(VkDeviceSize byte_count) {
    ExportableBuffer result{};
    result.data_size = byte_count;

    VkExternalMemoryBufferCreateInfo external_create{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external_create.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkBufferCreateInfo create_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create_info.pNext = &external_create;
    create_info.size = byte_count;
    create_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &create_info, nullptr, &result.buffer), "vkCreateBuffer(external)");

    try {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, result.buffer, &requirements);
        result.allocation_size = requirements.size;

        VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate_info.pNext = &export_info;
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = device_local_memory_type(physical_device_, requirements.memoryTypeBits);
        check(vkAllocateMemory(device_, &allocate_info, nullptr, &result.memory),
              "vkAllocateMemory(external)");
        check(vkBindBufferMemory(device_, result.buffer, result.memory, 0),
              "vkBindBufferMemory(external)");
    } catch (...) {
        if (result.memory != VK_NULL_HANDLE) vkFreeMemory(device_, result.memory, nullptr);
        vkDestroyBuffer(device_, result.buffer, nullptr);
        throw;
    }
    return result;
}

void InteropBufferPool::destroy(ExportableBuffer& allocation) noexcept {
    if (allocation.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, allocation.buffer, nullptr);
    if (allocation.memory != VK_NULL_HANDLE) vkFreeMemory(device_, allocation.memory, nullptr);
    allocation = {};
}

int InteropBufferPool::export_fd(const ExportableBuffer& allocation) const {
    VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    info.memory = allocation.memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    check(get_memory_fd_(device_, &info, &fd), "vkGetMemoryFdKHR");
    return fd;
}

std::size_t InteropBufferPool::allocation_count() const noexcept {
    return slots_.size() * 4 + scratch_.size();
}

void record_image_to_canonical_buffer(VkCommandBuffer command_buffer,
                                      VkImage source,
                                      VkBuffer destination,
                                      std::uint32_t width,
                                      std::uint32_t height) {
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(command_buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           destination, 1, &region);
}

void record_canonical_buffer_to_image(VkCommandBuffer command_buffer,
                                      VkBuffer source,
                                      VkImage destination,
                                      std::uint32_t width,
                                      std::uint32_t height) {
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(command_buffer, source, destination,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

} // namespace neuroshade::interop
