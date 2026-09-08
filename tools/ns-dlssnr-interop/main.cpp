#include "vulkan/interop_buffers/interop_pool.hpp"
#include <vulkan/vulkan.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
extern "C" {
void* ns_nr_create(const char*,const char*,const char*,const char*);
void ns_nr_destroy(void*);
const char* ns_nr_error();
int ns_nr_run(void*,const unsigned char*,unsigned char*,std::size_t,int);
int ns_nr_reset(void*);
int ns_nr_import_frame(void*,int,std::size_t,std::size_t);
int ns_nr_run_shared(void*,unsigned,int,unsigned);
}
namespace {
void vk_check(VkResult r,std::string_view op){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(op)+" VkResult="+std::to_string(r));}
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

void destroy(const VulkanContext& context, Buffer& buffer) {
    if (buffer.mapped != nullptr) vkUnmapMemory(context.device, buffer.memory);
    if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(context.device, buffer.buffer, nullptr);
    if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(context.device, buffer.memory, nullptr);
    buffer = {};
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

}
int main(int argc,char**argv){try{
 if(argc!=2)throw std::runtime_error("usage: ns-dlssnr-interop MODEL.nsmodel");
 const char* runtime=std::getenv("XDG_RUNTIME_DIR");
 auto lockpath=std::filesystem::path(runtime?runtime:"/tmp")/("neuroshade-gpu-qualification-"+std::to_string(getuid())+".lock");
 int lock=open(lockpath.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
 if(lock<0||flock(lock,LOCK_EX|LOCK_NB))throw std::runtime_error("GPU qualification lock unavailable");
 auto context=create_context();
 const std::size_t bytes=1920*1080*4;
 neuroshade::interop::InteropBufferPool pool(context.physical,context.device,1920,1080,0);
 auto staging=create_host_buffer(context,bytes);
 struct Cleanup {const VulkanContext& context;Buffer& buffer;~Cleanup(){destroy(context,buffer);}} cleanup{context,staging};
 auto model=std::filesystem::absolute(argv[1]);
 std::unique_ptr<void,decltype(&ns_nr_destroy)> engine(ns_nr_create((model/"kernels.hsaco").c_str(),(model/"graph.bin").c_str(),(model/"weights.bin").c_str(),(model/"lookup.bin").c_str()),ns_nr_destroy);
 if(!engine)throw std::runtime_error(ns_nr_error());
 const auto& shared=pool.slots()[0].input_color;
 int fd=pool.export_fd(shared);int slot=ns_nr_import_frame(engine.get(),fd,shared.allocation_size,bytes);close(fd);
 if(slot<0)throw std::runtime_error(ns_nr_error());
 std::vector<unsigned char> source(bytes),reference(bytes),actual(bytes);std::uint32_t state=17;
 for(std::size_t i=0;i<bytes;++i){state=state*1664525u+1013904223u;source[i]=state>>24;}
 bool owned=true;
 for(int bgra:{0,1})for(unsigned strength:{0u,128u,255u}){
  if(ns_nr_reset(engine.get())||ns_nr_run(engine.get(),source.data(),reference.data(),bytes,bgra))throw std::runtime_error(ns_nr_error());
  for(std::size_t i=0;i<bytes;++i)reference[i]=i%4==3?source[i]:static_cast<unsigned char>((source[i]*(255-strength)+reference[i]*strength+127)/255);
  if(ns_nr_reset(engine.get()))throw std::runtime_error(ns_nr_error());
  std::memcpy(staging.mapped,source.data(),bytes);
  auto commands=begin_commands(context);
  if(!owned)external_barrier(commands,shared.buffer,bytes,VK_QUEUE_FAMILY_EXTERNAL,context.queue_family,0,VK_ACCESS_TRANSFER_WRITE_BIT);
  VkBufferCopy copy{0,0,bytes};vkCmdCopyBuffer(commands,staging.buffer,shared.buffer,1,&copy);
  external_barrier(commands,shared.buffer,bytes,context.queue_family,VK_QUEUE_FAMILY_EXTERNAL,VK_ACCESS_TRANSFER_WRITE_BIT,0);
  submit_and_wait(context,commands);
  const auto start=std::chrono::steady_clock::now();
  if(ns_nr_run_shared(engine.get(),static_cast<unsigned>(slot),bgra,strength))throw std::runtime_error(ns_nr_error());
  const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  commands=begin_commands(context);
  external_barrier(commands,shared.buffer,bytes,VK_QUEUE_FAMILY_EXTERNAL,context.queue_family,0,VK_ACCESS_TRANSFER_READ_BIT);
  vkCmdCopyBuffer(commands,shared.buffer,staging.buffer,1,&copy);
  submit_and_wait(context,commands);owned=true;
  std::memcpy(actual.data(),staging.mapped,bytes);
  if(actual!=reference)throw std::runtime_error("shared frame differs from reference");
  std::cout<<"shared_frame=pass bgra="<<bgra<<" strength="<<strength<<" gpu_path_ms="<<ms<<" output_bitwise_equal=yes"<<std::endl;
 }
 std::cout<<"interop=pass cases=6 resolution=1920x1080"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"ns-dlssnr-interop: "<<e.what()<<std::endl;return 1;}}
