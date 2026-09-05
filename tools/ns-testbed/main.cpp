#include "logging/log.hpp"
#include "framegraph/compiler/compiler.hpp"
#include "framegraph/resources/frame.hpp"
#include "plugins/shader/manifest.hpp"
#include "profile/profile.hpp"
#include "layer/resource_tracker/analyzer.hpp"
#include "layer/resource_tracker/resource_tracker.hpp"
#include "vulkan/presentation/shader_executor.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

namespace {

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 64;
constexpr VkDeviceSize kByteCount = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;

class VulkanError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void vk_check(VkResult result, std::string_view operation) {
    if (result != VK_SUCCESS) {
        throw VulkanError(std::string(operation) + " failed with VkResult " + std::to_string(result));
    }
}

template <typename Handle, auto Destroy>
class UniqueHandle {
public:
    UniqueHandle() = default;
    UniqueHandle(Handle handle, VkDevice owner) : handle_(handle), owner_(owner) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(other.handle_), owner_(other.owner_) { other.handle_ = VK_NULL_HANDLE; }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            owner_ = other.owner_;
            other.handle_ = VK_NULL_HANDLE;
        }
        return *this;
    }
    ~UniqueHandle() { reset(); }
    [[nodiscard]] Handle get() const noexcept { return handle_; }

private:
    void reset() noexcept {
        if (handle_ != VK_NULL_HANDLE) Destroy(owner_, handle_, nullptr);
        handle_ = VK_NULL_HANDLE;
    }
    Handle handle_{VK_NULL_HANDLE};
    VkDevice owner_{VK_NULL_HANDLE};
};

using UniqueBuffer = UniqueHandle<VkBuffer, vkDestroyBuffer>;
using UniqueImage = UniqueHandle<VkImage, vkDestroyImage>;
using UniqueMemory = UniqueHandle<VkDeviceMemory, vkFreeMemory>;
using UniqueCommandPool = UniqueHandle<VkCommandPool, vkDestroyCommandPool>;
using UniqueFence = UniqueHandle<VkFence, vkDestroyFence>;

struct InstanceOwner {
    VkInstance value{VK_NULL_HANDLE};
    ~InstanceOwner() { if (value != VK_NULL_HANDLE) vkDestroyInstance(value, nullptr); }
};

struct DeviceOwner {
    VkDevice value{VK_NULL_HANDLE};
    ~DeviceOwner() { if (value != VK_NULL_HANDLE) vkDestroyDevice(value, nullptr); }
};

struct DeviceSelection {
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    std::uint32_t queue_family{};
    VkPhysicalDeviceProperties properties{};
};

[[nodiscard]] std::uint32_t find_memory_type(VkPhysicalDevice physical_device,
                                              std::uint32_t allowed,
                                              VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        const auto bit = std::uint32_t{1} << index;
        if ((allowed & bit) != 0 && (properties.memoryTypes[index].propertyFlags & required) == required) {
            return index;
        }
    }
    throw VulkanError("no compatible Vulkan memory type");
}

[[nodiscard]] DeviceSelection select_device(VkInstance instance) {
    std::uint32_t count = 0;
    vk_check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices(count)");
    if (count == 0) throw VulkanError("no Vulkan physical device found");

    std::vector<VkPhysicalDevice> devices(count);
    vk_check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");

    const char* requested = std::getenv("NEUROSHADE_VULKAN_DEVICE");
    DeviceSelection best{};
    int best_score = std::numeric_limits<int>::min();

    for (VkPhysicalDevice physical_device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device, &properties);

        std::uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, queues.data());

        for (std::uint32_t family = 0; family < queue_count; ++family) {
            if ((queues[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) == 0) continue;
            if (requested != nullptr && std::string_view(properties.deviceName).find(requested) == std::string_view::npos) {
                continue;
            }

            int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 0;
            score += properties.vendorID == 0x1002 ? 20 : 0;
            score += (queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 ? 5 : 0;
            if (score > best_score) {
                best = {physical_device, family, properties};
                best_score = score;
            }
        }
    }

    if (best.physical_device == VK_NULL_HANDLE) {
        throw VulkanError(requested == nullptr ? "no usable Vulkan queue found"
                                               : "requested NEUROSHADE_VULKAN_DEVICE was not found");
    }
    return best;
}

[[nodiscard]] std::uint64_t fnv1a(std::span<const std::uint8_t> bytes, std::uint64_t hash) {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= prime;
    }
    return hash;
}

[[nodiscard]] std::uint32_t parse_frames(int argc, char** argv) {
    std::uint32_t frames = 4;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            std::cout << "Usage: ns-testbed [--frames N]\n"
                         "Environment: NEUROSHADE_VULKAN_DEVICE=<device-name-substring>\n";
            std::exit(0);
        }
        if (argument == "--frames" && index + 1 < argc) {
            const std::string_view value(argv[++index]);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), frames);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || frames == 0) {
                throw std::invalid_argument("--frames requires a positive integer");
            }
            continue;
        }
        throw std::invalid_argument("unknown argument: " + std::string(argument));
    }
    return frames;
}

[[nodiscard]] std::vector<neuroshade::vulkan::ShaderEffect> configured_effects() {
    if (const char* profile_path = std::getenv("NEUROSHADE_PROFILE")) {
        const auto loaded = neuroshade::profile::load(profile_path);
        if (!loaded.valid()) throw std::invalid_argument("unable to load NEUROSHADE_PROFILE: " + loaded.errors.front());
        std::vector<neuroshade::vulkan::ShaderEffect> effects;
        for (const auto& effect : loaded.profile.pipeline) {
            if (!effect.enabled) continue;
            const auto separator = effect.plugin.rfind('.');
            effects.push_back({separator == std::string::npos ? effect.plugin : effect.plugin.substr(separator + 1),
                               effect.strength});
        }
        return effects;
    }
    const char* configured = std::getenv("NEUROSHADE_TEST_EFFECTS");
    if (configured == nullptr || *configured == '\0') return {};
    std::vector<neuroshade::vulkan::ShaderEffect> effects;
    std::string_view remaining(configured);
    while (!remaining.empty()) {
        const auto separator = remaining.find(',');
        const std::string name(remaining.substr(0, separator));
        if (name != "copy" && name != "sharpen" && name != "color_adjust") {
            throw std::invalid_argument("unsupported NEUROSHADE_TEST_EFFECTS entry: " + name);
        }
        effects.push_back({name, name == "sharpen" ? 0.8F : 1.0F});
        if (separator == std::string_view::npos) break;
        remaining.remove_prefix(separator + 1);
    }
    return effects;
}

[[nodiscard]] std::uint32_t toggle_frame() {
    const char* configured = std::getenv("NEUROSHADE_TEST_TOGGLE_FRAME");
    if (configured == nullptr) return 0;
    std::uint32_t value{};
    const std::string_view text(configured);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::invalid_argument("NEUROSHADE_TEST_TOGGLE_FRAME must be an unsigned integer");
    }
    return value;
}

[[nodiscard]] std::filesystem::path shader_directory() {
    if (const char* configured = std::getenv("NEUROSHADE_SHADER_DIR")) return configured;
    std::error_code error;
    const auto executable = std::filesystem::canonical("/proc/self/exe", error);
    if (error) throw std::runtime_error("unable to locate ns-testbed executable");
    return executable.parent_path().parent_path() / "share/neuroshade/shaders";
}

int run(std::uint32_t frame_count) {
    const auto effects = configured_effects();
    const auto enable_at_frame = toggle_frame();
    if (!effects.empty()) {
        std::vector<neuroshade::framegraph::PassDescriptor> passes;
        std::string input(neuroshade::framegraph::semantic::color_final);
        for (std::size_t index = 0; index < effects.size(); ++index) {
            const auto manifest_path = shader_directory().parent_path() / "plugins" / effects[index].name / "manifest.toml";
            const auto manifest = neuroshade::plugins::load_shader_manifest(manifest_path);
            if (!manifest.valid()) throw std::runtime_error(manifest.errors.front());
            if (manifest.manifest.passes.front().shader != effects[index].name + ".spv") {
                throw std::runtime_error("plugin manifest shader does not match bundled artifact");
            }
            const std::string output = index + 1 == effects.size()
                                           ? std::string(neuroshade::framegraph::semantic::output_color)
                                           : "Work." + std::to_string(index);
            passes.push_back({"plugin." + effects[index].name, neuroshade::framegraph::PassType::shader,
                              {{input, neuroshade::framegraph::ResourceFormat::rgba8_uint, true}},
                              {{output, neuroshade::framegraph::ResourceFormat::rgba8_uint}},
                              neuroshade::framegraph::QueuePreference::compute, 1});
            input = output;
        }
        const auto compiled = neuroshade::framegraph::Compiler{}.compile(
            passes, {{std::string(neuroshade::framegraph::semantic::color_final),
                      neuroshade::framegraph::ResourceFormat::rgba8_uint}});
        if (!compiled.valid()) throw std::runtime_error("configured shader FrameGraph did not compile");
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "ns-testbed";
    application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application.pEngineName = "NeuroShade";
    application.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    InstanceOwner instance;
    vk_check(vkCreateInstance(&instance_info, nullptr, &instance.value), "vkCreateInstance");

    const DeviceSelection selected = select_device(instance.value);
    neuroshade::logging::write(neuroshade::logging::Level::info,
                              std::string("Vulkan device: ") + selected.properties.deviceName);

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = selected.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    DeviceOwner device;
    vk_check(vkCreateDevice(selected.physical_device, &device_info, nullptr, &device.value), "vkCreateDevice");

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device.value, selected.queue_family, 0, &queue);

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = kByteCount;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer_handle = VK_NULL_HANDLE;
    vk_check(vkCreateBuffer(device.value, &buffer_info, nullptr, &buffer_handle), "vkCreateBuffer");
    UniqueBuffer buffer(buffer_handle, device.value);

    VkMemoryRequirements buffer_requirements{};
    vkGetBufferMemoryRequirements(device.value, buffer.get(), &buffer_requirements);
    VkMemoryAllocateInfo buffer_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    buffer_allocation.allocationSize = buffer_requirements.size;
    buffer_allocation.memoryTypeIndex = find_memory_type(
        selected.physical_device, buffer_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory buffer_memory_handle = VK_NULL_HANDLE;
    vk_check(vkAllocateMemory(device.value, &buffer_allocation, nullptr, &buffer_memory_handle),
             "vkAllocateMemory(buffer)");
    UniqueMemory buffer_memory(buffer_memory_handle, device.value);
    vk_check(vkBindBufferMemory(device.value, buffer.get(), buffer_memory.get(), 0), "vkBindBufferMemory");

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UINT;
    image_info.extent = {kWidth, kHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image_handle = VK_NULL_HANDLE;
    vk_check(vkCreateImage(device.value, &image_info, nullptr, &image_handle), "vkCreateImage");
    UniqueImage image(image_handle, device.value);

    neuroshade::resources::ResourceTracker resource_tracker;
    VkImageCreateInfo depth_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depth_info.imageType = VK_IMAGE_TYPE_2D;
    depth_info.format = VK_FORMAT_D32_SFLOAT;
    depth_info.extent = {kWidth, kHeight, 1};
    depth_info.mipLevels = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    depth_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkImage depth_handle = VK_NULL_HANDLE;
    vk_check(vkCreateImage(device.value, &depth_info, nullptr, &depth_handle), "vkCreateImage(depth fixture)");
    UniqueImage depth_image(depth_handle, device.value);
    const auto depth_id = resource_tracker.track_image(depth_handle, depth_info, 0);

    VkImageCreateInfo low_res_info = image_info;
    low_res_info.extent = {kWidth / 2, kHeight / 2, 1};
    low_res_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage low_res_handle = VK_NULL_HANDLE;
    vk_check(vkCreateImage(device.value, &low_res_info, nullptr, &low_res_handle), "vkCreateImage(low-res fixture)");
    UniqueImage low_res_image(low_res_handle, device.value);
    const auto low_res_id = resource_tracker.track_image(low_res_handle, low_res_info, 0);

    const auto depth_candidates = neuroshade::resources::rank_candidates(
        resource_tracker.images(), neuroshade::resources::CandidateRole::depth, {kWidth, kHeight});
    const auto low_res_candidates = neuroshade::resources::rank_candidates(
        resource_tracker.images(), neuroshade::resources::CandidateRole::low_res_color, {kWidth, kHeight});
    if (depth_candidates.empty() || depth_candidates.front().image.id != depth_id ||
        low_res_candidates.empty() || low_res_candidates.front().image.id != low_res_id) {
        throw VulkanError("resource analyzer failed to identify deterministic testbed fixtures");
    }

    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(device.value, image.get(), &image_requirements);
    VkMemoryAllocateInfo image_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    image_allocation.allocationSize = image_requirements.size;
    image_allocation.memoryTypeIndex = find_memory_type(selected.physical_device, image_requirements.memoryTypeBits,
                                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory image_memory_handle = VK_NULL_HANDLE;
    vk_check(vkAllocateMemory(device.value, &image_allocation, nullptr, &image_memory_handle),
             "vkAllocateMemory(image)");
    UniqueMemory image_memory(image_memory_handle, device.value);
    vk_check(vkBindImageMemory(device.value, image.get(), image_memory.get(), 0), "vkBindImageMemory");

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = selected.queue_family;
    VkCommandPool pool_handle = VK_NULL_HANDLE;
    vk_check(vkCreateCommandPool(device.value, &pool_info, nullptr, &pool_handle), "vkCreateCommandPool");
    UniqueCommandPool pool(pool_handle, device.value);

    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = pool.get();
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    vk_check(vkAllocateCommandBuffers(device.value, &command_info, &command), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence_handle = VK_NULL_HANDLE;
    vk_check(vkCreateFence(device.value, &fence_info, nullptr, &fence_handle), "vkCreateFence");
    UniqueFence fence(fence_handle, device.value);

    neuroshade::vulkan::ShaderExecutor shader_executor(
        selected.physical_device, device.value, queue, selected.queue_family, buffer.get(), image.get(), kWidth, kHeight,
        shader_directory(), effects);

    constexpr std::array<std::array<std::uint8_t, 4>, 4> palette{{
        {{0x11, 0x22, 0x33, 0xff}}, {{0x44, 0x55, 0x66, 0xff}},
        {{0x77, 0x88, 0x99, 0xff}}, {{0xaa, 0xbb, 0xcc, 0xff}},
    }};
    std::uint64_t checksum = 14695981039346656037ULL;

    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        vk_check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(command, &begin_info), "vkBeginCommandBuffer");

        VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_transfer.srcAccessMask = frame == 0 ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = frame == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = image.get();
        to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, frame == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

        const auto& color = palette[frame % palette.size()];
        VkClearColorValue clear{};
        for (std::size_t channel = 0; channel < color.size(); ++channel) clear.uint32[channel] = color[channel];
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

        VkImageMemoryBarrier to_source{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_source.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_source.image = image.get();
        to_source.subresourceRange = range;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_source);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(command, image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.get(), 1, &copy);

        VkMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host_barrier, 0, nullptr, 0, nullptr);
        vk_check(vkEndCommandBuffer(command), "vkEndCommandBuffer");

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        vk_check(vkQueueSubmit(queue, 1, &submit, fence.get()), "vkQueueSubmit");
        vk_check(vkWaitForFences(device.value, 1, &fence_handle, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        vk_check(vkResetFences(device.value, 1, &fence_handle), "vkResetFences");

        void* mapped = nullptr;
        vk_check(vkMapMemory(device.value, buffer_memory.get(), 0, kByteCount, 0, &mapped), "vkMapMemory");
        const auto pixels = std::span(static_cast<const std::uint8_t*>(mapped), static_cast<std::size_t>(kByteCount));
        bool valid = true;
        for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
            valid = valid && std::memcmp(pixels.data() + offset, color.data(), color.size()) == 0;
        }
        vkUnmapMemory(device.value, buffer_memory.get());
        if (!valid) throw VulkanError("deterministic frame verification failed at frame " + std::to_string(frame));

        if (!shader_executor.empty() && frame >= enable_at_frame) {
            const auto processed = shader_executor.execute();
            void* composite_mapping = nullptr;
            vk_check(vkMapMemory(device.value, buffer_memory.get(), 0, kByteCount, 0, &composite_mapping),
                     "vkMapMemory(composite)");
            const bool composite_matches =
                std::memcmp(composite_mapping, processed.data(), processed.size()) == 0;
            vkUnmapMemory(device.value, buffer_memory.get());
            if (!composite_matches) throw VulkanError("Vulkan output composite verification failed");
            checksum = fnv1a(processed, checksum);
        } else {
            void* checksum_mapping = nullptr;
            vk_check(vkMapMemory(device.value, buffer_memory.get(), 0, kByteCount, 0, &checksum_mapping),
                     "vkMapMemory(checksum)");
            checksum = fnv1a(std::span(static_cast<const std::uint8_t*>(checksum_mapping),
                                       static_cast<std::size_t>(kByteCount)), checksum);
            vkUnmapMemory(device.value, buffer_memory.get());
        }
    }

    vk_check(vkDeviceWaitIdle(device.value), "vkDeviceWaitIdle");
    std::cout << "ns-testbed: PASS frames=" << frame_count << " size=" << kWidth << 'x' << kHeight
              << " effects=" << effects.size() << " checksum=0x" << std::hex << checksum << std::dec << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_frames(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "ns-testbed: FAIL: " << error.what() << '\n';
        return 1;
    }
}
