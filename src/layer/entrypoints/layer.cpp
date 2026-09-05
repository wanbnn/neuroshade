#include "logging/log.hpp"
#include "layer/presentation/present_processor.hpp"
#include "layer/resource_tracker/analyzer.hpp"
#include "layer/resource_tracker/resource_tracker.hpp"
#include "overlay/home_key_input.hpp"
#include "overlay/overlay_state.hpp"
#include "profile/profile.hpp"
#include "runtime/pipeline_plan.hpp"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define NS_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define NS_LAYER_EXPORT
#endif

extern "C" {
NS_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
NS_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*);
NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance, std::uint32_t*, VkPhysicalDevice*);
}

namespace {

namespace nslog = neuroshade::logging;

template <typename Handle>
[[nodiscard]] void* dispatch_key(Handle handle) noexcept {
    if (handle == VK_NULL_HANDLE) return nullptr;
    return *reinterpret_cast<void**>(handle);
}

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    PFN_vkDestroyInstance destroy_instance{};
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices{};
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties{};
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_surface_capabilities{};
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    PFN_vkDestroyDevice destroy_device{};
    PFN_vkGetDeviceQueue get_device_queue{};
    PFN_vkGetDeviceQueue2 get_device_queue2{};
    PFN_vkCreateSwapchainKHR create_swapchain{};
    PFN_vkDestroySwapchainKHR destroy_swapchain{};
    PFN_vkGetSwapchainImagesKHR get_swapchain_images{};
    PFN_vkAcquireNextImageKHR acquire_next_image{};
    PFN_vkAcquireNextImage2KHR acquire_next_image2{};
    PFN_vkQueuePresentKHR queue_present{};
    PFN_vkCreateImage create_image{};
    PFN_vkDestroyImage destroy_image{};
    PFN_vkBindImageMemory bind_image_memory{};
    PFN_vkBindImageMemory2 bind_image_memory2{};
    PFN_vkCreateImageView create_image_view{};
    PFN_vkDestroyImageView destroy_image_view{};
    PFN_vkAllocateMemory allocate_memory{};
    PFN_vkFreeMemory free_memory{};
    PFN_vkCmdBeginRendering cmd_begin_rendering{};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    neuroshade::layer::PresentDispatch present_dispatch{};
    std::shared_ptr<neuroshade::resources::ResourceTracker> tracker;
    std::shared_ptr<std::atomic<std::uint64_t>> frame;
    std::shared_ptr<const neuroshade::runtime::PipelinePreparation> pipeline;
    std::shared_ptr<neuroshade::overlay::OverlayState> overlay;
    std::shared_ptr<neuroshade::overlay::HomeKeyInput> home_key;
    std::shared_ptr<std::mutex> overlay_mutex;
    std::shared_ptr<bool> overlay_resources_initialized;
    std::shared_ptr<const std::string> overlay_shader;
};

struct SwapchainState {
    VkDevice device{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::uint64_t acquire_count{};
    std::uint64_t present_count{};
    VkImageUsageFlags usage{};
    bool processing_eligible{};
    bool processing_attempted{};
    std::uint32_t processing_queue_family{VK_QUEUE_FAMILY_IGNORED};
    std::shared_ptr<neuroshade::layer::PresentProcessor> processor;
};

std::mutex state_mutex;
std::unordered_map<void*, InstanceDispatch> instance_dispatches;
std::unordered_map<void*, void*> physical_device_instances;
std::unordered_map<void*, DeviceDispatch> device_dispatches;
std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;
std::unordered_map<VkQueue, std::uint32_t> queue_families;
std::atomic<std::uint64_t> instance_count{};
std::atomic<std::uint64_t> device_count{};

[[nodiscard]] VkLayerInstanceCreateInfo* instance_chain_info(const VkInstanceCreateInfo* create_info) {
    auto* current = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(create_info->pNext));
    while (current != nullptr) {
        if (current->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            current->function == VK_LAYER_LINK_INFO) {
            return current;
        }
        current = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(current->pNext));
    }
    return nullptr;
}

[[nodiscard]] VkLayerDeviceCreateInfo* device_chain_info(const VkDeviceCreateInfo* create_info) {
    auto* current = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(create_info->pNext));
    while (current != nullptr) {
        if (current->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            current->function == VK_LAYER_LINK_INFO) {
            return current;
        }
        current = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(current->pNext));
    }
    return nullptr;
}

template <typename Function>
[[nodiscard]] Function load_instance(PFN_vkGetInstanceProcAddr get_proc_addr,
                                     VkInstance instance,
                                     const char* name) {
    return reinterpret_cast<Function>(get_proc_addr(instance, name));
}

template <typename Function>
[[nodiscard]] Function load_device(PFN_vkGetDeviceProcAddr get_proc_addr,
                                   VkDevice device,
                                   const char* name) {
    return reinterpret_cast<Function>(get_proc_addr(device, name));
}

template <typename Handle>
[[nodiscard]] std::optional<InstanceDispatch> find_instance(Handle instance) {
    std::scoped_lock lock(state_mutex);
    void* key = dispatch_key(instance);
    if (const auto physical = physical_device_instances.find(key);
        physical != physical_device_instances.end()) {
        key = physical->second;
    }
    const auto found = instance_dispatches.find(key);
    if (found == instance_dispatches.end()) return std::nullopt;
    return found->second;
}

template <typename Handle>
[[nodiscard]] std::optional<DeviceDispatch> find_device(Handle handle) {
    std::scoped_lock lock(state_mutex);
    const auto found = device_dispatches.find(dispatch_key(handle));
    if (found == device_dispatches.end()) return std::nullopt;
    return found->second;
}

[[nodiscard]] bool name_is(const char* requested, std::string_view expected) noexcept {
    return requested != nullptr && expected == requested;
}

}  // namespace

extern "C" {

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkInstance* instance) {
    if (create_info == nullptr || instance == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    nslog::initialize_from_environment();
    auto* chain = instance_chain_info(create_info);
    if (chain == nullptr || chain->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    const auto next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto next_create = load_instance<PFN_vkCreateInstance>(next_gipa, VK_NULL_HANDLE, "vkCreateInstance");
    if (next_create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    const VkResult result = next_create(create_info, allocator, instance);
    if (result != VK_SUCCESS) return result;

    InstanceDispatch dispatch{};
    dispatch.get_instance_proc_addr = next_gipa;
    dispatch.destroy_instance = load_instance<PFN_vkDestroyInstance>(next_gipa, *instance, "vkDestroyInstance");
    dispatch.enumerate_physical_devices = load_instance<PFN_vkEnumeratePhysicalDevices>(
        next_gipa, *instance, "vkEnumeratePhysicalDevices");
    dispatch.get_memory_properties = load_instance<PFN_vkGetPhysicalDeviceMemoryProperties>(
        next_gipa, *instance, "vkGetPhysicalDeviceMemoryProperties");
    dispatch.get_surface_capabilities =
        load_instance<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            next_gipa, *instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    {
        std::scoped_lock lock(state_mutex);
        instance_dispatches.emplace(dispatch_key(*instance), dispatch);
    }
    ++instance_count;
    nslog::write(nslog::Level::info, "VK_LAYER_NEUROSHADE instance initialized in pass-through mode");
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* allocator) {
    const auto dispatch = find_instance(instance);
    if (!dispatch || dispatch->destroy_instance == nullptr) return;
    void* const key = dispatch_key(instance);
    dispatch->destroy_instance(instance, allocator);
    {
        std::scoped_lock lock(state_mutex);
        instance_dispatches.erase(key);
        std::erase_if(physical_device_instances,
                      [key](const auto& entry) { return entry.second == key; });
    }
    --instance_count;
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance instance, std::uint32_t* count, VkPhysicalDevice* physical_devices) {
    const auto dispatch = find_instance(instance);
    if (!dispatch || !dispatch->enumerate_physical_devices) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result =
        dispatch->enumerate_physical_devices(instance, count, physical_devices);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count && physical_devices) {
        std::scoped_lock lock(state_mutex);
        for (std::uint32_t index = 0; index < *count; ++index) {
            physical_device_instances[dispatch_key(physical_devices[index])] = dispatch_key(instance);
        }
    }
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkDevice* device) {
    if (create_info == nullptr || device == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* chain = device_chain_info(create_info);
    if (chain == nullptr || chain->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    const auto next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto next_create = load_instance<PFN_vkCreateDevice>(next_gipa, VK_NULL_HANDLE, "vkCreateDevice");
    if (next_create == nullptr || next_gdpa == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    const VkResult result = next_create(physical_device, create_info, allocator, device);
    if (result != VK_SUCCESS) return result;

    DeviceDispatch dispatch{};
    dispatch.get_device_proc_addr = next_gdpa;
    dispatch.destroy_device = load_device<PFN_vkDestroyDevice>(next_gdpa, *device, "vkDestroyDevice");
    dispatch.get_device_queue = load_device<PFN_vkGetDeviceQueue>(next_gdpa, *device, "vkGetDeviceQueue");
    dispatch.get_device_queue2 = load_device<PFN_vkGetDeviceQueue2>(next_gdpa, *device, "vkGetDeviceQueue2");
    dispatch.create_swapchain = load_device<PFN_vkCreateSwapchainKHR>(next_gdpa, *device, "vkCreateSwapchainKHR");
    dispatch.destroy_swapchain = load_device<PFN_vkDestroySwapchainKHR>(next_gdpa, *device, "vkDestroySwapchainKHR");
    dispatch.get_swapchain_images = load_device<PFN_vkGetSwapchainImagesKHR>(next_gdpa, *device, "vkGetSwapchainImagesKHR");
    dispatch.acquire_next_image = load_device<PFN_vkAcquireNextImageKHR>(next_gdpa, *device, "vkAcquireNextImageKHR");
    dispatch.acquire_next_image2 = load_device<PFN_vkAcquireNextImage2KHR>(next_gdpa, *device, "vkAcquireNextImage2KHR");
    dispatch.queue_present = load_device<PFN_vkQueuePresentKHR>(next_gdpa, *device, "vkQueuePresentKHR");
    dispatch.create_image = load_device<PFN_vkCreateImage>(next_gdpa, *device, "vkCreateImage");
    dispatch.destroy_image = load_device<PFN_vkDestroyImage>(next_gdpa, *device, "vkDestroyImage");
    dispatch.bind_image_memory = load_device<PFN_vkBindImageMemory>(next_gdpa, *device, "vkBindImageMemory");
    dispatch.bind_image_memory2 = load_device<PFN_vkBindImageMemory2>(next_gdpa, *device, "vkBindImageMemory2");
    dispatch.create_image_view = load_device<PFN_vkCreateImageView>(next_gdpa, *device, "vkCreateImageView");
    dispatch.destroy_image_view = load_device<PFN_vkDestroyImageView>(next_gdpa, *device, "vkDestroyImageView");
    dispatch.allocate_memory = load_device<PFN_vkAllocateMemory>(next_gdpa, *device, "vkAllocateMemory");
    dispatch.free_memory = load_device<PFN_vkFreeMemory>(next_gdpa, *device, "vkFreeMemory");
    dispatch.cmd_begin_rendering = load_device<PFN_vkCmdBeginRendering>(next_gdpa, *device, "vkCmdBeginRendering");
    dispatch.physical_device = physical_device;
    if (const auto instance = find_instance(physical_device);
        instance && instance->get_memory_properties) {
        instance->get_memory_properties(physical_device, &dispatch.memory_properties);
    }
    auto& present = dispatch.present_dispatch;
    present.create_buffer = load_device<PFN_vkCreateBuffer>(next_gdpa, *device, "vkCreateBuffer");
    present.destroy_buffer = load_device<PFN_vkDestroyBuffer>(next_gdpa, *device, "vkDestroyBuffer");
    present.get_buffer_memory_requirements = load_device<PFN_vkGetBufferMemoryRequirements>(next_gdpa, *device, "vkGetBufferMemoryRequirements");
    present.allocate_memory = dispatch.allocate_memory;
    present.free_memory = dispatch.free_memory;
    present.bind_buffer_memory = load_device<PFN_vkBindBufferMemory>(next_gdpa, *device, "vkBindBufferMemory");
    present.map_memory = load_device<PFN_vkMapMemory>(next_gdpa, *device, "vkMapMemory");
    present.unmap_memory = load_device<PFN_vkUnmapMemory>(next_gdpa, *device, "vkUnmapMemory");
    present.create_shader_module = load_device<PFN_vkCreateShaderModule>(next_gdpa, *device, "vkCreateShaderModule");
    present.destroy_shader_module = load_device<PFN_vkDestroyShaderModule>(next_gdpa, *device, "vkDestroyShaderModule");
    present.create_descriptor_set_layout = load_device<PFN_vkCreateDescriptorSetLayout>(next_gdpa, *device, "vkCreateDescriptorSetLayout");
    present.destroy_descriptor_set_layout = load_device<PFN_vkDestroyDescriptorSetLayout>(next_gdpa, *device, "vkDestroyDescriptorSetLayout");
    present.create_pipeline_layout = load_device<PFN_vkCreatePipelineLayout>(next_gdpa, *device, "vkCreatePipelineLayout");
    present.destroy_pipeline_layout = load_device<PFN_vkDestroyPipelineLayout>(next_gdpa, *device, "vkDestroyPipelineLayout");
    present.create_descriptor_pool = load_device<PFN_vkCreateDescriptorPool>(next_gdpa, *device, "vkCreateDescriptorPool");
    present.destroy_descriptor_pool = load_device<PFN_vkDestroyDescriptorPool>(next_gdpa, *device, "vkDestroyDescriptorPool");
    present.allocate_descriptor_sets = load_device<PFN_vkAllocateDescriptorSets>(next_gdpa, *device, "vkAllocateDescriptorSets");
    present.update_descriptor_sets = load_device<PFN_vkUpdateDescriptorSets>(next_gdpa, *device, "vkUpdateDescriptorSets");
    present.create_compute_pipelines = load_device<PFN_vkCreateComputePipelines>(next_gdpa, *device, "vkCreateComputePipelines");
    present.destroy_pipeline = load_device<PFN_vkDestroyPipeline>(next_gdpa, *device, "vkDestroyPipeline");
    present.create_command_pool = load_device<PFN_vkCreateCommandPool>(next_gdpa, *device, "vkCreateCommandPool");
    present.destroy_command_pool = load_device<PFN_vkDestroyCommandPool>(next_gdpa, *device, "vkDestroyCommandPool");
    present.allocate_command_buffers = load_device<PFN_vkAllocateCommandBuffers>(next_gdpa, *device, "vkAllocateCommandBuffers");
    present.begin_command_buffer = load_device<PFN_vkBeginCommandBuffer>(next_gdpa, *device, "vkBeginCommandBuffer");
    present.end_command_buffer = load_device<PFN_vkEndCommandBuffer>(next_gdpa, *device, "vkEndCommandBuffer");
    present.cmd_pipeline_barrier = load_device<PFN_vkCmdPipelineBarrier>(next_gdpa, *device, "vkCmdPipelineBarrier");
    present.cmd_copy_image_to_buffer = load_device<PFN_vkCmdCopyImageToBuffer>(next_gdpa, *device, "vkCmdCopyImageToBuffer");
    present.cmd_copy_buffer_to_image = load_device<PFN_vkCmdCopyBufferToImage>(next_gdpa, *device, "vkCmdCopyBufferToImage");
    present.cmd_bind_pipeline = load_device<PFN_vkCmdBindPipeline>(next_gdpa, *device, "vkCmdBindPipeline");
    present.cmd_bind_descriptor_sets = load_device<PFN_vkCmdBindDescriptorSets>(next_gdpa, *device, "vkCmdBindDescriptorSets");
    present.cmd_push_constants = load_device<PFN_vkCmdPushConstants>(next_gdpa, *device, "vkCmdPushConstants");
    present.cmd_dispatch = load_device<PFN_vkCmdDispatch>(next_gdpa, *device, "vkCmdDispatch");
    present.create_semaphore = load_device<PFN_vkCreateSemaphore>(next_gdpa, *device, "vkCreateSemaphore");
    present.destroy_semaphore = load_device<PFN_vkDestroySemaphore>(next_gdpa, *device, "vkDestroySemaphore");
    present.create_fence = load_device<PFN_vkCreateFence>(next_gdpa, *device, "vkCreateFence");
    present.destroy_fence = load_device<PFN_vkDestroyFence>(next_gdpa, *device, "vkDestroyFence");
    present.reset_fences = load_device<PFN_vkResetFences>(next_gdpa, *device, "vkResetFences");
    present.wait_for_fences = load_device<PFN_vkWaitForFences>(next_gdpa, *device, "vkWaitForFences");
    present.queue_submit = load_device<PFN_vkQueueSubmit>(next_gdpa, *device, "vkQueueSubmit");
    dispatch.tracker = std::make_shared<neuroshade::resources::ResourceTracker>();
    dispatch.frame = std::make_shared<std::atomic<std::uint64_t>>(0);
    if (const char* profile_path = std::getenv("NEUROSHADE_PROFILE");
        profile_path != nullptr && profile_path[0] != '\0') {
        auto loaded = neuroshade::profile::load(profile_path);
        if (loaded.valid()) {
            nslog::write(nslog::Level::info,
                         "profile_load=pass schema=" +
                             std::to_string(loaded.profile.schema_version) +
                             " effects=" + std::to_string(loaded.profile.pipeline.size()));
            const char* root_value = std::getenv("NEUROSHADE_ROOT");
            const std::filesystem::path plugin_root =
                root_value == nullptr || root_value[0] == '\0'
                    ? std::filesystem::path{"share/neuroshade/plugins"}
                    : std::filesystem::path{root_value} / "share/neuroshade/plugins";
            auto prepared = neuroshade::runtime::prepare_pipeline(loaded.profile, plugin_root);
            if (prepared.valid()) {
                nslog::write(nslog::Level::info,
                                 "pipeline_compile=pass mode=" +
                                 std::string(neuroshade::runtime::to_string(prepared.mode)) +
                                 " passes=" + std::to_string(prepared.plan.passes.size()) +
                                 " execution=pending-swapchain");
                dispatch.pipeline =
                    std::make_shared<const neuroshade::runtime::PipelinePreparation>(
                        std::move(prepared));
                dispatch.overlay = std::make_shared<neuroshade::overlay::OverlayState>();
                dispatch.home_key = std::make_shared<neuroshade::overlay::HomeKeyInput>();
                dispatch.overlay_mutex = std::make_shared<std::mutex>();
                dispatch.overlay_resources_initialized = std::make_shared<bool>(false);
                if (std::getenv("NEUROSHADE_OVERLAY_VISIBLE")) {
                    dispatch.overlay->on_home_pressed();
                }
                dispatch.overlay_shader = std::make_shared<const std::string>(
                    (plugin_root.parent_path() / "shaders/overlay.spv").string());
                neuroshade::overlay::Snapshot snapshot{};
                snapshot.game = loaded.profile.executable;
                snapshot.active_profile = profile_path;
                snapshot.interop_mode = dispatch.pipeline->mode ==
                                                neuroshade::runtime::PipelineMode::neural_spatial
                                            ? "HOST-STAGING FALLBACK (present adapter)"
                                            : "VULKAN";
                snapshot.mode = dispatch.pipeline->mode == neuroshade::runtime::PipelineMode::shader_only
                                    ? neuroshade::overlay::RuntimeMode::shader_only
                                : dispatch.pipeline->mode == neuroshade::runtime::PipelineMode::neural_spatial
                                    ? neuroshade::overlay::RuntimeMode::neural_spatial
                                : dispatch.pipeline->mode == neuroshade::runtime::PipelineMode::neural_temporal
                                    ? neuroshade::overlay::RuntimeMode::neural_temporal
                                    : neuroshade::overlay::RuntimeMode::pass_through;
                for (const auto& effect : loaded.profile.pipeline) {
                    snapshot.passes.push_back(
                        {effect.plugin, effect.enabled, 0.0, effect.strength, effect.model,
                         effect.enabled, effect.strength, effect.model});
                }
                dispatch.overlay->update(std::move(snapshot));
            } else {
                nslog::write(nslog::Level::warning,
                             "pipeline_compile=fail fallback=pass-through reason=" +
                                 (prepared.errors.empty() ? std::string{"unknown"}
                                                          : prepared.errors.front()));
            }
        } else {
            nslog::write(nslog::Level::warning,
                         "profile_load=fail fallback=pass-through reason=" +
                             (loaded.errors.empty() ? std::string{"unknown"} : loaded.errors.front()));
        }
    } else {
        nslog::write(nslog::Level::info, "profile_load=none fallback=pass-through");
    }
    {
        std::scoped_lock lock(state_mutex);
        device_dispatches.emplace(dispatch_key(*device), dispatch);
    }
    ++device_count;
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* allocator) {
    const auto dispatch = find_device(device);
    if (!dispatch || dispatch->destroy_device == nullptr) return;
    void* const key = dispatch_key(device);
    std::vector<std::shared_ptr<neuroshade::layer::PresentProcessor>> processors;
    {
        std::scoped_lock lock(state_mutex);
        std::erase_if(swapchains, [&](const auto& entry) {
            if (entry.second.device != device) return false;
            if (entry.second.processor) processors.push_back(entry.second.processor);
            return true;
        });
        std::erase_if(queue_families, [key](const auto& entry) {
            return dispatch_key(entry.first) == key;
        });
    }
    processors.clear();
    dispatch->destroy_device(device, allocator);
    {
        std::scoped_lock lock(state_mutex);
        device_dispatches.erase(key);
    }
    --device_count;
}

NS_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
    VkDevice device,
    uint32_t queue_family_index,
    uint32_t queue_index,
    VkQueue* queue) {
    const auto dispatch = find_device(device);
    if (dispatch && dispatch->get_device_queue) {
        dispatch->get_device_queue(device, queue_family_index, queue_index, queue);
        if (queue && *queue) {
            std::scoped_lock lock(state_mutex);
            queue_families[*queue] = queue_family_index;
        }
    }
}

NS_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device,
    const VkDeviceQueueInfo2* queue_info,
    VkQueue* queue) {
    const auto dispatch = find_device(device);
    if (dispatch && dispatch->get_device_queue2) dispatch->get_device_queue2(device, queue_info, queue);
    if (dispatch && queue_info && queue && *queue) {
        std::scoped_lock lock(state_mutex);
        queue_families[*queue] = queue_info->queueFamilyIndex;
    }
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice device, const VkImageCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImage* image) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->create_image) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = dispatch->create_image(device, create_info, allocator, image);
    if (result == VK_SUCCESS && create_info && image && dispatch->tracker) {
        (void)dispatch->tracker->track_image(*image, *create_info, dispatch->frame->load());
    }
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->destroy_image) return;
    if (dispatch->tracker) dispatch->tracker->destroy_image(image);
    dispatch->destroy_image(device, image, allocator);
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* allocator,
    VkDeviceMemory* memory) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->allocate_memory) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = dispatch->allocate_memory(device, info, allocator, memory);
    if (result == VK_SUCCESS && memory && dispatch->tracker) (void)dispatch->tracker->track_memory(*memory);
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkFreeMemory(
    VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->free_memory) return;
    if (dispatch->tracker) dispatch->tracker->destroy_memory(memory);
    dispatch->free_memory(device, memory, allocator);
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(
    VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->bind_image_memory) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = dispatch->bind_image_memory(device, image, memory, offset);
    if (result == VK_SUCCESS && dispatch->tracker) dispatch->tracker->bind_image(image, memory, offset);
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2(
    VkDevice device, uint32_t count, const VkBindImageMemoryInfo* infos) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->bind_image_memory2) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = dispatch->bind_image_memory2(device, count, infos);
    if (result == VK_SUCCESS && dispatch->tracker) {
        for (uint32_t index = 0; index < count; ++index) {
            dispatch->tracker->bind_image(infos[index].image, infos[index].memory, infos[index].memoryOffset);
        }
    }
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* allocator,
    VkImageView* view) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->create_image_view) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = dispatch->create_image_view(device, info, allocator, view);
    if (result == VK_SUCCESS && info && view && dispatch->tracker) dispatch->tracker->track_view(*view, info->image);
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
    VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->destroy_image_view) return;
    if (dispatch->tracker) dispatch->tracker->destroy_view(view);
    dispatch->destroy_image_view(device, view, allocator);
}

NS_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(
    VkCommandBuffer command, const VkRenderingInfo* info) {
    const auto dispatch = find_device(command);
    if (!dispatch || !dispatch->cmd_begin_rendering) return;
    if (info && dispatch->tracker) {
        for (uint32_t index = 0; index < info->colorAttachmentCount; ++index) {
            dispatch->tracker->observe_view_write(info->pColorAttachments[index].imageView, dispatch->frame->load());
        }
        if (info->pDepthAttachment) dispatch->tracker->observe_view_write(info->pDepthAttachment->imageView, dispatch->frame->load());
        if (info->pStencilAttachment) dispatch->tracker->observe_view_write(info->pStencilAttachment->imageView, dispatch->frame->load());
    }
    dispatch->cmd_begin_rendering(command, info);
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* create_info,
    const VkAllocationCallbacks* allocator,
    VkSwapchainKHR* swapchain) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->create_swapchain) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!create_info) return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainCreateInfoKHR effective = *create_info;
    bool processing_eligible = false;
    const bool processing_requested = dispatch->pipeline &&
        (dispatch->pipeline->mode == neuroshade::runtime::PipelineMode::shader_only ||
         dispatch->pipeline->mode == neuroshade::runtime::PipelineMode::neural_spatial ||
         dispatch->pipeline->mode == neuroshade::runtime::PipelineMode::neural_temporal) &&
        std::getenv("NEUROSHADE_ENABLED") != nullptr;
    if (processing_requested && neuroshade::layer::present_format_supported(create_info->imageFormat)) {
        const auto instance = find_instance(dispatch->physical_device);
        VkSurfaceCapabilitiesKHR capabilities{};
        if (instance && instance->get_surface_capabilities &&
            instance->get_surface_capabilities(dispatch->physical_device, create_info->surface,
                                               &capabilities) == VK_SUCCESS) {
            constexpr VkImageUsageFlags required = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if ((capabilities.supportedUsageFlags & required) == required) {
                effective.imageUsage |= required;
                processing_eligible = true;
            } else {
                nslog::write(nslog::Level::warning,
                             "present_processing=disabled reason=surface-transfer-unsupported");
            }
        } else {
            nslog::write(nslog::Level::warning,
                         "present_processing=disabled reason=surface-capabilities-unavailable");
        }
    } else if (processing_requested) {
        nslog::write(nslog::Level::warning,
                     "present_processing=disabled reason=swapchain-format-unsupported");
    }
    const VkResult result = dispatch->create_swapchain(device, &effective, allocator, swapchain);
    if (result == VK_SUCCESS && create_info != nullptr && swapchain != nullptr) {
        std::scoped_lock lock(state_mutex);
        SwapchainState state{};
        state.device = device;
        state.format = effective.imageFormat;
        state.extent = effective.imageExtent;
        state.usage = effective.imageUsage;
        state.processing_eligible = processing_eligible;
        swapchains[*swapchain] = std::move(state);
        nslog::write(nslog::Level::info,
                     "tracking swapchain " + std::to_string(create_info->imageExtent.width) + "x" +
                         std::to_string(create_info->imageExtent.height));
    }
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->destroy_swapchain) return;
    std::vector<VkImage> tracked_images;
    std::shared_ptr<neuroshade::layer::PresentProcessor> processor;
    {
        std::scoped_lock lock(state_mutex);
        if (const auto found = swapchains.find(swapchain); found != swapchains.end()) {
            tracked_images = found->second.images;
            processor = std::move(found->second.processor);
        }
        swapchains.erase(swapchain);
    }
    processor.reset();
    if (dispatch->tracker) {
        for (const auto image : tracked_images) dispatch->tracker->destroy_image(image);
    }
    dispatch->destroy_swapchain(device, swapchain, allocator);
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* count,
    VkImage* images) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->get_swapchain_images) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = dispatch->get_swapchain_images(device, swapchain, count, images);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count != nullptr && images != nullptr) {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        std::scoped_lock lock(state_mutex);
        const auto found = swapchains.find(swapchain);
        if (found != swapchains.end()) {
            found->second.images.assign(images, images + *count);
            format = found->second.format;
            extent = found->second.extent;
        }
        if (dispatch->tracker && format != VK_FORMAT_UNDEFINED) {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = {extent.width, extent.height, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = found->second.usage;
            for (uint32_t index = 0; index < *count; ++index) {
                if (!dispatch->tracker->image(images[index])) {
                    (void)dispatch->tracker->track_image(images[index], info, dispatch->frame->load(), true);
                }
            }
        }
    }
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* image_index) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->acquire_next_image) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = dispatch->acquire_next_image(device, swapchain, timeout, semaphore, fence, image_index);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        std::scoped_lock lock(state_mutex);
        if (const auto found = swapchains.find(swapchain); found != swapchains.end()) ++found->second.acquire_count;
    }
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* acquire_info,
    uint32_t* image_index) {
    const auto dispatch = find_device(device);
    if (!dispatch || !dispatch->acquire_next_image2) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = dispatch->acquire_next_image2(device, acquire_info, image_index);
    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && acquire_info != nullptr) {
        std::scoped_lock lock(state_mutex);
        if (const auto found = swapchains.find(acquire_info->swapchain); found != swapchains.end()) {
            ++found->second.acquire_count;
        }
    }
    return result;
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* present_info) {
    const auto dispatch = find_device(queue);
    if (!dispatch || !dispatch->queue_present) return VK_ERROR_INITIALIZATION_FAILED;
    const auto current_frame = dispatch->frame->fetch_add(1) + 1;
    bool overlay_visible = false;
    if (dispatch->home_key && dispatch->overlay && dispatch->overlay_mutex) {
        std::scoped_lock overlay_lock(*dispatch->overlay_mutex);
        if (dispatch->home_key->pressed_edge()) {
            dispatch->overlay->on_home_pressed();
            nslog::write(nslog::Level::info,
                         std::string("overlay=") +
                             (dispatch->overlay->visible() ? "visible" : "hidden") +
                             " input=Home");
        }
        overlay_visible = dispatch->overlay->visible();
    }
    VkPresentInfoKHR effective{};
    VkSemaphore completion = VK_NULL_HANDLE;
    const VkPresentInfoKHR* forwarded = present_info;
    if (present_info != nullptr) {
        if (present_info->swapchainCount == 1 && present_info->pSwapchains &&
            present_info->pImageIndices && dispatch->pipeline) {
            const VkSwapchainKHR swapchain = present_info->pSwapchains[0];
            const std::uint32_t image_index = present_info->pImageIndices[0];
            std::shared_ptr<neuroshade::layer::PresentProcessor> processor;
            bool initialize = false;
            VkExtent2D extent{};
            VkFormat format = VK_FORMAT_UNDEFINED;
            std::vector<VkImage> images;
            VkDevice logical_device = VK_NULL_HANDLE;
            std::uint32_t queue_family = VK_QUEUE_FAMILY_IGNORED;
            {
                std::scoped_lock lock(state_mutex);
                const auto family = queue_families.find(queue);
                const auto found = swapchains.find(swapchain);
                if (family != queue_families.end() && found != swapchains.end() &&
                    found->second.processing_eligible) {
                    queue_family = family->second;
                    extent = found->second.extent;
                    if (found->second.processing_queue_family == VK_QUEUE_FAMILY_IGNORED ||
                        found->second.processing_queue_family == queue_family) {
                        processor = found->second.processor;
                    }
                    if (!processor && !found->second.processing_attempted &&
                        !found->second.images.empty()) {
                        found->second.processing_attempted = true;
                        found->second.processing_queue_family = queue_family;
                        logical_device = found->second.device;
                        extent = found->second.extent;
                        format = found->second.format;
                        images = found->second.images;
                        initialize = true;
                    }
                }
            }
            if (initialize) {
                std::string error;
                auto created = neuroshade::layer::PresentProcessor::create(
                    logical_device,
                    dispatch->memory_properties, queue_family, extent, format, images,
                    *dispatch->pipeline, dispatch->present_dispatch,
                    dispatch->overlay_shader ? *dispatch->overlay_shader : std::string{}, error);
                if (created) {
                    processor = std::shared_ptr<neuroshade::layer::PresentProcessor>(
                        std::move(created));
                    std::scoped_lock lock(state_mutex);
                    if (const auto found = swapchains.find(swapchain);
                        found != swapchains.end()) {
                        found->second.processor = processor;
                    }
                    const bool neural = dispatch->pipeline->mode !=
                                        neuroshade::runtime::PipelineMode::shader_only;
                    nslog::write(nslog::Level::info,
                                 "present_processing=active backend=" +
                                     std::string(neural ? "neural host_staging" : "shader") +
                                     " extent=" +
                                     std::to_string(extent.width) + "x" +
                                     std::to_string(extent.height) + " overlay=" +
                                     (overlay_visible ? "visible" : "hidden"));
                } else {
                    std::scoped_lock lock(state_mutex);
                    if (const auto found = swapchains.find(swapchain);
                        found != swapchains.end()) {
                        found->second.processing_eligible = false;
                    }
                    nslog::write(nslog::Level::warning,
                                 "present_processing=disabled fallback=pass-through reason=" +
                                     error);
                }
            }
            if (processor) {
                const VkResult submitted = processor->submit(
                    queue, image_index, present_info->waitSemaphoreCount,
                    present_info->pWaitSemaphores,
                    overlay_visible, completion);
                if (submitted == VK_SUCCESS) {
                    effective = *present_info;
                    effective.waitSemaphoreCount = 1;
                    effective.pWaitSemaphores = &completion;
                    forwarded = &effective;
                    if (dispatch->overlay && dispatch->overlay_mutex) {
                        std::scoped_lock overlay_lock(*dispatch->overlay_mutex);
                        if (processor->average_ms() > 0.0) {
                            dispatch->overlay->update_neural_timing(processor->average_ms());
                        }
                        if (dispatch->overlay_resources_initialized &&
                            !*dispatch->overlay_resources_initialized && dispatch->tracker) {
                            const auto tracked = dispatch->tracker->images();
                            std::vector<neuroshade::overlay::ResourceRow> resources;
                            const auto append_candidate = [&](std::string semantic,
                                                              neuroshade::resources::CandidateRole role) {
                                const auto candidates = neuroshade::resources::rank_candidates(
                                    tracked, role, extent);
                                if (candidates.empty()) return;
                                const auto& candidate = candidates.front();
                                resources.push_back(
                                    {std::move(semantic), candidate.image.extent.width,
                                     candidate.image.extent.height,
                                     static_cast<std::uint32_t>(candidate.image.format),
                                     candidate.confidence, false});
                            };
                            append_candidate("Depth.Screen",
                                             neuroshade::resources::CandidateRole::depth);
                            append_candidate("Motion.Screen",
                                             neuroshade::resources::CandidateRole::motion);
                            append_candidate("Color.Scene",
                                             neuroshade::resources::CandidateRole::scene_color);
                            append_candidate("Color.LowRes",
                                             neuroshade::resources::CandidateRole::low_res_color);
                            dispatch->overlay->set_resources(std::move(resources));
                            *dispatch->overlay_resources_initialized = true;
                        }
                    }
                } else {
                    std::scoped_lock lock(state_mutex);
                    if (const auto found = swapchains.find(swapchain);
                        found != swapchains.end()) {
                        found->second.processing_eligible = false;
                    }
                    nslog::write(nslog::Level::error,
                                 "present_processing=submit-fail fallback=pass-through result=" +
                                     std::to_string(submitted));
                }
            }
        }
        std::scoped_lock lock(state_mutex);
        for (uint32_t index = 0; index < present_info->swapchainCount; ++index) {
            if (const auto found = swapchains.find(present_info->pSwapchains[index]); found != swapchains.end()) {
                ++found->second.present_count;
                if (dispatch->tracker && present_info->pImageIndices &&
                    present_info->pImageIndices[index] < found->second.images.size()) {
                    dispatch->tracker->observe_image_read(found->second.images[present_info->pImageIndices[index]], current_frame);
                }
            }
        }
    }
    return dispatch->queue_present(queue, forwarded);
}

NS_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char* name) {
    if (name_is(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (name_is(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (name_is(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance);
    if (name_is(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance);
    if (name_is(name, "vkEnumeratePhysicalDevices")) return reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDevices);
    if (name_is(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice);
    if (name_is(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
    if (name_is(name, "vkGetDeviceQueue")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
    if (name_is(name, "vkGetDeviceQueue2")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);
    if (name_is(name, "vkCreateSwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR);
    if (name_is(name, "vkDestroySwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR);
    if (name_is(name, "vkGetSwapchainImagesKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetSwapchainImagesKHR);
    if (name_is(name, "vkAcquireNextImageKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR);
    if (name_is(name, "vkAcquireNextImage2KHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImage2KHR);
    if (name_is(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR);
    if (name_is(name, "vkCreateImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage);
    if (name_is(name, "vkDestroyImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImage);
    if (name_is(name, "vkBindImageMemory")) return reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory);
    if (name_is(name, "vkBindImageMemory2")) return reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory2);
    if (name_is(name, "vkCreateImageView")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImageView);
    if (name_is(name, "vkDestroyImageView")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImageView);
    if (name_is(name, "vkAllocateMemory")) return reinterpret_cast<PFN_vkVoidFunction>(vkAllocateMemory);
    if (name_is(name, "vkFreeMemory")) return reinterpret_cast<PFN_vkVoidFunction>(vkFreeMemory);
    if (name_is(name, "vkCmdBeginRendering")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering);
    if (instance == VK_NULL_HANDLE) return nullptr;
    const auto dispatch = find_instance(instance);
    return dispatch && dispatch->get_instance_proc_addr ? dispatch->get_instance_proc_addr(instance, name) : nullptr;
}

NS_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device,
    const char* name) {
    if (name_is(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (name_is(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
    if (name_is(name, "vkGetDeviceQueue")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
    if (name_is(name, "vkGetDeviceQueue2")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);
    if (name_is(name, "vkCreateSwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR);
    if (name_is(name, "vkDestroySwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR);
    if (name_is(name, "vkGetSwapchainImagesKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetSwapchainImagesKHR);
    if (name_is(name, "vkAcquireNextImageKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR);
    if (name_is(name, "vkAcquireNextImage2KHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImage2KHR);
    if (name_is(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR);
    if (name_is(name, "vkCreateImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage);
    if (name_is(name, "vkDestroyImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImage);
    if (name_is(name, "vkBindImageMemory")) return reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory);
    if (name_is(name, "vkBindImageMemory2")) return reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory2);
    if (name_is(name, "vkCreateImageView")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImageView);
    if (name_is(name, "vkDestroyImageView")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImageView);
    if (name_is(name, "vkAllocateMemory")) return reinterpret_cast<PFN_vkVoidFunction>(vkAllocateMemory);
    if (name_is(name, "vkFreeMemory")) return reinterpret_cast<PFN_vkVoidFunction>(vkFreeMemory);
    if (name_is(name, "vkCmdBeginRendering")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering);
    if (device == VK_NULL_HANDLE) return nullptr;
    const auto dispatch = find_device(device);
    return dispatch && dispatch->get_device_proc_addr ? dispatch->get_device_proc_addr(device, name) : nullptr;
}

NS_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* version) {
    if (version == nullptr || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (version->loaderLayerInterfaceVersion < MIN_SUPPORTED_LOADER_LAYER_INTERFACE_VERSION) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    version->loaderLayerInterfaceVersion =
        std::min(version->loaderLayerInterfaceVersion, static_cast<uint32_t>(CURRENT_LOADER_LAYER_INTERFACE_VERSION));
    version->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = [](VkInstance instance, const char* name) -> PFN_vkVoidFunction {
        if (instance == VK_NULL_HANDLE) return nullptr;
        const auto dispatch = find_instance(instance);
        return dispatch && dispatch->get_instance_proc_addr ? dispatch->get_instance_proc_addr(instance, name) : nullptr;
    };
    return VK_SUCCESS;
}

}  // extern "C"
