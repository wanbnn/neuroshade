#include "hip/interop/interop_runtime.hpp"
#include "vulkan/interop_buffers/canonical.hpp"
#include "vulkan/interop_buffers/interop_pool.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 64;
constexpr std::size_t kPixelCount = static_cast<std::size_t>(kWidth) * kHeight;
constexpr VkDeviceSize kColorBytes = kPixelCount * sizeof(neuroshade::interop::NSHalf4);

void vk_check(VkResult result, std::string_view operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                                 std::to_string(result));
    }
}

struct VulkanContext {
    VulkanContext() = default;
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&& other) noexcept
        : instance(other.instance), physical(other.physical), properties(other.properties),
          device(other.device), queue(other.queue), queue_family(other.queue_family),
          command_pool(other.command_pool), fence(other.fence) {
        other.instance = VK_NULL_HANDLE;
        other.device = VK_NULL_HANDLE;
        other.command_pool = VK_NULL_HANDLE;
        other.fence = VK_NULL_HANDLE;
    }
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkPhysicalDeviceProperties properties{};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queue_family{};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};

    ~VulkanContext() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
        if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }
};

struct Buffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    void* mapped{nullptr};
};

struct Image {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
};

std::uint32_t memory_type(VkPhysicalDevice physical,
                          std::uint32_t allowed,
                          VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        const auto bit = std::uint32_t{1} << index;
        if ((allowed & bit) != 0 &&
            (properties.memoryTypes[index].propertyFlags & required) == required) return index;
    }
    throw std::runtime_error("no compatible Vulkan memory type");
}

VulkanContext create_context() {
    VulkanContext context{};
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "ns-interop-test";
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    vk_check(vkCreateInstance(&instance_info, nullptr, &context.instance), "vkCreateInstance");

    std::uint32_t count = 0;
    vk_check(vkEnumeratePhysicalDevices(context.instance, &count, nullptr),
             "vkEnumeratePhysicalDevices(count)");
    std::vector<VkPhysicalDevice> devices(count);
    vk_check(vkEnumeratePhysicalDevices(context.instance, &count, devices.data()),
             "vkEnumeratePhysicalDevices");
    const char* requested = std::getenv("NEUROSHADE_VULKAN_DEVICE");
    int best_score = std::numeric_limits<int>::min();
    for (const auto physical : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (requested != nullptr &&
            std::string_view(properties.deviceName).find(requested) == std::string_view::npos) continue;
        std::uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues.data());
        for (std::uint32_t family = 0; family < queue_count; ++family) {
            if ((queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
            int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 0;
            score += properties.vendorID == 0x1002 ? 20 : 0;
            if (score > best_score) {
                context.physical = physical;
                context.properties = properties;
                context.queue_family = family;
                best_score = score;
            }
        }
    }
    if (context.physical == VK_NULL_HANDLE) throw std::runtime_error("no matching Vulkan GPU");

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = context.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    constexpr std::array extensions{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = extensions.size();
    device_info.ppEnabledExtensionNames = extensions.data();
    vk_check(vkCreateDevice(context.physical, &device_info, nullptr, &context.device),
             "vkCreateDevice");
    vkGetDeviceQueue(context.device, context.queue_family, 0, &context.queue);

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context.queue_family;
    vk_check(vkCreateCommandPool(context.device, &pool_info, nullptr, &context.command_pool),
             "vkCreateCommandPool");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vk_check(vkCreateFence(context.device, &fence_info, nullptr, &context.fence), "vkCreateFence");
    return context;
}

Buffer create_host_buffer(const VulkanContext& context, VkDeviceSize size) {
    Buffer result{};
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vkCreateBuffer(context.device, &info, nullptr, &result.buffer), "vkCreateBuffer(staging)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device, result.buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(
        context.physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_check(vkAllocateMemory(context.device, &allocation, nullptr, &result.memory),
             "vkAllocateMemory(staging)");
    vk_check(vkBindBufferMemory(context.device, result.buffer, result.memory, 0),
             "vkBindBufferMemory(staging)");
    vk_check(vkMapMemory(context.device, result.memory, 0, size, 0, &result.mapped),
             "vkMapMemory(staging)");
    return result;
}

Image create_color_image(const VulkanContext& context) {
    Image result{};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.extent = {kWidth, kHeight, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(vkCreateImage(context.device, &info, nullptr, &result.image), "vkCreateImage(canonical)");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context.device, result.image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(context.physical, requirements.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(context.device, &allocation, nullptr, &result.memory),
             "vkAllocateMemory(image)");
    vk_check(vkBindImageMemory(context.device, result.image, result.memory, 0),
             "vkBindImageMemory");
    return result;
}

void destroy(const VulkanContext& context, Buffer& buffer) {
    if (buffer.mapped != nullptr) vkUnmapMemory(context.device, buffer.memory);
    if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(context.device, buffer.buffer, nullptr);
    if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(context.device, buffer.memory, nullptr);
    buffer = {};
}

void destroy(const VulkanContext& context, Image& image) {
    if (image.image != VK_NULL_HANDLE) vkDestroyImage(context.device, image.image, nullptr);
    if (image.memory != VK_NULL_HANDLE) vkFreeMemory(context.device, image.memory, nullptr);
    image = {};
}

VkCommandBuffer begin_commands(const VulkanContext& context) {
    vk_check(vkResetFences(context.device, 1, &context.fence), "vkResetFences");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = context.command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    vk_check(vkAllocateCommandBuffers(context.device, &allocation, &commands),
             "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
    return commands;
}

void submit_and_wait(const VulkanContext& context, VkCommandBuffer commands) {
    vk_check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    vk_check(vkQueueSubmit(context.queue, 1, &submit, context.fence), "vkQueueSubmit");
    vk_check(vkWaitForFences(context.device, 1, &context.fence, VK_TRUE, 5'000'000'000ULL),
             "vkWaitForFences");
    vkFreeCommandBuffers(context.device, context.command_pool, 1, &commands);
}

void image_barrier(VkCommandBuffer commands,
                   VkImage image,
                   VkImageLayout old_layout,
                   VkImageLayout new_layout,
                   VkAccessFlags source_access,
                   VkAccessFlags destination_access) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void external_barrier(VkCommandBuffer commands,
                      VkBuffer buffer,
                      VkDeviceSize size,
                      std::uint32_t source_family,
                      std::uint32_t destination_family,
                      VkAccessFlags source_access,
                      VkAccessFlags destination_access) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    barrier.srcQueueFamilyIndex = source_family;
    barrier.dstQueueFamilyIndex = destination_family;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = size;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
}

std::vector<neuroshade::interop::NSHalf4> make_pattern() {
    constexpr std::array<std::uint16_t, 5> half_values{0x0000, 0x3400, 0x3800, 0x3a00, 0x3c00};
    std::vector<neuroshade::interop::NSHalf4> pixels(kPixelCount);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            pixels[static_cast<std::size_t>(y) * kWidth + x] = {
                half_values[(x / 8) % half_values.size()],
                half_values[(y / 8 + 1) % half_values.size()],
                half_values[((x + y) / 8 + 2) % half_values.size()],
                0x3c00};
        }
    }
    return pixels;
}

std::vector<neuroshade::interop::NSHalf4> expected_effect(
    std::span<const neuroshade::interop::NSHalf4> input) {
    std::vector<neuroshade::interop::NSHalf4> output(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        output[index] = {input[index].b, input[index].r, input[index].g, input[index].a};
    }
    return output;
}

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint64_t result = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

std::span<const std::uint8_t> bytes(std::span<const neuroshade::interop::NSHalf4> pixels) {
    return {reinterpret_cast<const std::uint8_t*>(pixels.data()), pixels.size_bytes()};
}

std::filesystem::path cache_path(const VulkanContext& context,
                                 const neuroshade::hip::HipDeviceInfo& hip) {
    std::ostringstream key;
    key << context.properties.vendorID << ':' << context.properties.deviceID << ':'
        << context.properties.driverVersion << ':' << hip.architecture << ':' << hip.runtime_version;
    const auto key_text = key.str();
    const auto hash = fnv1a({reinterpret_cast<const std::uint8_t*>(key_text.data()), key_text.size()});
    std::filesystem::path root;
    if (const char* configured = std::getenv("XDG_CACHE_HOME")) root = configured;
    else if (const char* home = std::getenv("HOME")) root = std::filesystem::path(home) / ".cache";
    else root = std::filesystem::temp_directory_path();
    std::ostringstream filename;
    filename << std::hex << hash << ".result";
    return root / "neuroshade" / "interop" / filename.str();
}

void write_cache(const std::filesystem::path& path, std::string_view mode) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    std::ofstream output(path, std::ios::trunc);
    if (output) output << "abi=" << neuroshade::interop::kCanonicalAbiVersion
                       << "\nresult=pass\nmode=" << mode << '\n';
}

int run() {
    auto context = create_context();
    const auto hip_device = neuroshade::hip::select_device();
    const bool force_fallback = std::getenv("NEUROSHADE_FORCE_HOST_STAGING") != nullptr;

    neuroshade::interop::InteropBufferPool pool(context.physical, context.device, kWidth, kHeight);
    if (pool.allocation_count() != 13) throw std::runtime_error("interop pool allocation invariant failed");
    const auto& input_external = pool.slots()[0].input_color;
    const auto& output_external = pool.slots()[0].output_color;
    Buffer upload = create_host_buffer(context, kColorBytes);
    Buffer readback = create_host_buffer(context, kColorBytes);
    Image input_image = create_color_image(context);
    Image output_image = create_color_image(context);
    std::unique_ptr<neuroshade::hip::HostStagingWorkspace> staging_workspace;
    if (force_fallback) {
        staging_workspace = std::make_unique<neuroshade::hip::HostStagingWorkspace>(kColorBytes);
    }

    try {
        const auto input = make_pattern();
        const auto expected = expected_effect(input);
        std::memcpy(upload.mapped, input.data(), kColorBytes);

        auto commands = begin_commands(context);
        image_barrier(commands, input_image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        neuroshade::interop::record_canonical_buffer_to_image(
            commands, upload.buffer, input_image.image, kWidth, kHeight);
        image_barrier(commands, input_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT);
        neuroshade::interop::record_image_to_canonical_buffer(
            commands, input_image.image, input_external.buffer, kWidth, kHeight);

        if (!force_fallback) {
            external_barrier(commands, input_external.buffer, input_external.data_size,
                             context.queue_family, VK_QUEUE_FAMILY_EXTERNAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, 0);
            external_barrier(commands, output_external.buffer, output_external.data_size,
                             context.queue_family, VK_QUEUE_FAMILY_EXTERNAL, 0, 0);
        } else {
            external_barrier(commands, input_external.buffer, input_external.data_size,
                             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferCopy copy{0, 0, kColorBytes};
            vkCmdCopyBuffer(commands, input_external.buffer, readback.buffer, 1, &copy);
        }
        submit_and_wait(context, commands);

        const auto started = std::chrono::steady_clock::now();
        std::uint64_t hip_input_hash = 0;
        if (!force_fallback) {
            if (std::getenv("NEUROSHADE_TEST_FAIL_ZERO_COPY") != nullptr) {
                throw std::runtime_error("injected zero-copy self-test failure");
            }
            std::vector<std::unique_ptr<neuroshade::hip::ImportedBuffer>> imported;
            imported.reserve(pool.allocation_count());
            auto import = [&](const neuroshade::interop::ExportableBuffer& allocation) {
                imported.push_back(std::make_unique<neuroshade::hip::ImportedBuffer>(
                    pool.export_fd(allocation), allocation.allocation_size, allocation.data_size));
            };
            for (const auto& slot : pool.slots()) {
                import(slot.input_color);
                import(slot.depth);
                import(slot.motion);
                import(slot.output_color);
            }
            for (const auto& allocation : pool.scratch()) import(allocation);

            hip_input_hash = neuroshade::hip::hash_bytes(imported[0]->data(), kColorBytes);
            neuroshade::hip::run_color_effect(imported[0]->data(), imported[3]->data(), kPixelCount);

            commands = begin_commands(context);
            external_barrier(commands, output_external.buffer, output_external.data_size,
                             VK_QUEUE_FAMILY_EXTERNAL, context.queue_family, 0,
                             VK_ACCESS_TRANSFER_READ_BIT);
            image_barrier(commands, output_image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            neuroshade::interop::record_canonical_buffer_to_image(
                commands, output_external.buffer, output_image.image, kWidth, kHeight);
            image_barrier(commands, output_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT);
            neuroshade::interop::record_image_to_canonical_buffer(
                commands, output_image.image, readback.buffer, kWidth, kHeight);
            submit_and_wait(context, commands);
        } else {
            hip_input_hash = fnv1a({static_cast<const std::uint8_t*>(readback.mapped), kColorBytes});
            staging_workspace->process(readback.mapped, upload.mapped);
            commands = begin_commands(context);
            image_barrier(commands, output_image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            neuroshade::interop::record_canonical_buffer_to_image(
                commands, upload.buffer, output_image.image, kWidth, kHeight);
            image_barrier(commands, output_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT);
            neuroshade::interop::record_image_to_canonical_buffer(
                commands, output_image.image, readback.buffer, kWidth, kHeight);
            submit_and_wait(context, commands);
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();

        const auto input_hash = fnv1a(bytes(input));
        const auto expected_hash = fnv1a(bytes(expected));
        const auto actual_hash = fnv1a({static_cast<const std::uint8_t*>(readback.mapped), kColorBytes});
        if (hip_input_hash != input_hash) throw std::runtime_error("HIP read/hash mismatch");
        if (actual_hash != expected_hash) throw std::runtime_error("Vulkan readback of HIP output mismatch");
        if (actual_hash == input_hash) throw std::runtime_error("HIP color effect did not change output");

        const std::string mode = force_fallback ? "HOST-STAGING FALLBACK" : "ZERO-COPY BUFFER";
        const auto cache = cache_path(context, hip_device);
        write_cache(cache, mode);
        std::cout << "Interop: " << mode << '\n'
                  << "vulkan_gpu=" << context.properties.deviceName << '\n'
                  << "hip_gpu=" << hip_device.name << " arch=" << hip_device.architecture << '\n'
                  << "canonical_abi=" << neuroshade::interop::kCanonicalAbiVersion
                  << " pool_allocations=" << pool.allocation_count() << '\n'
                  << "vulkan_to_hip_hash=0x" << std::hex << hip_input_hash << '\n'
                  << "hip_to_vulkan_hash=0x" << actual_hash << std::dec << '\n'
                  << "color_effect=visible checksum_changed=yes\n"
                  << "steady_state_cpu_frame_readback=" << (force_fallback ? "yes" : "no") << '\n'
                  << "transfer_cost_ms=" << std::fixed << std::setprecision(3) << elapsed << '\n'
                  << "self_test_cache=" << cache << '\n';
    } catch (...) {
        destroy(context, output_image);
        destroy(context, input_image);
        destroy(context, readback);
        destroy(context, upload);
        throw;
    }

    destroy(context, output_image);
    destroy(context, input_image);
    destroy(context, readback);
    destroy(context, upload);
    return 0;
}

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& error) {
        if (std::getenv("NEUROSHADE_FORCE_HOST_STAGING") == nullptr) {
            std::cerr << "ns-interop-test: zero-copy self-test failed: " << error.what()
                      << "\nInterop: selecting HOST-STAGING FALLBACK\n";
            if (setenv("NEUROSHADE_FORCE_HOST_STAGING", "1", 1) == 0) {
                try {
                    return run();
                } catch (const std::exception& fallback_error) {
                    std::cerr << "ns-interop-test: fallback failed: " << fallback_error.what() << '\n';
                    return 1;
                }
            }
        }
        std::cerr << "ns-interop-test: " << error.what() << '\n';
        return 1;
    }
}
