#include "vulkan/presentation/shader_executor.hpp"

#include <cstring>
#include <fstream>
#include <span>
#include <stdexcept>
#include <utility>

namespace neuroshade::vulkan {
namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

std::uint32_t memory_type(VkPhysicalDevice physical_device, std::uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0 && (properties.memoryTypes[index].propertyFlags & flags) == flags) return index;
    }
    throw std::runtime_error("no host-visible memory type for shader output");
}

std::vector<std::uint32_t> read_spirv(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("unable to open SPIR-V shader: " + path.string());
    const auto size = stream.tellg();
    if (size <= 0 || size % 4 != 0) throw std::runtime_error("invalid SPIR-V byte size: " + path.string());
    std::vector<std::uint32_t> code(static_cast<std::size_t>(size) / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(code.data()), size);
    if (!stream) throw std::runtime_error("unable to read SPIR-V shader: " + path.string());
    return code;
}

}  // namespace

struct ShaderExecutor::Impl {
    struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; };
    struct Stage { VkShaderModule module{}; VkPipeline pipeline{}; VkDescriptorSet descriptor{}; float strength{}; };
    struct Push { std::uint32_t width; std::uint32_t height; float strength; };

    VkDevice device{};
    VkPhysicalDevice physical{};
    VkQueue queue{};
    VkBuffer input{};
    VkImage target_image{};
    std::uint32_t width{};
    std::uint32_t height{};
    VkDeviceSize byte_count{};
    VkDescriptorSetLayout descriptor_layout{};
    VkPipelineLayout pipeline_layout{};
    VkDescriptorPool descriptor_pool{};
    VkCommandPool command_pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    std::vector<Buffer> outputs;
    std::vector<Stage> stages;

    Impl(VkPhysicalDevice physical_device, VkDevice logical_device, VkQueue execution_queue,
         std::uint32_t family, VkBuffer input_buffer, VkImage image, std::uint32_t image_width,
         std::uint32_t image_height, const std::filesystem::path& directory,
         const std::vector<ShaderEffect>& effects)
        : device(logical_device), physical(physical_device), queue(execution_queue), input(input_buffer), target_image(image),
          width(image_width), height(image_height), byte_count(VkDeviceSize{width} * height * 4) {
        if (effects.empty()) return;
        try {
            create_layouts(effects.size());
            create_command_resources(family);
            outputs.reserve(effects.size());
            stages.reserve(effects.size());
            for (const auto& effect : effects) {
                outputs.push_back(create_buffer());
                stages.push_back(create_stage(directory / (effect.name + ".spv"), effect.strength));
            }
            update_descriptors();
        } catch (...) {
            destroy();
            throw;
        }
    }

    void create_layouts(std::size_t count) {
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptor_info.bindingCount = 2;
        descriptor_info.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(device, &descriptor_info, nullptr, &descriptor_layout),
              "vkCreateDescriptorSetLayout");
        const VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pipeline_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_info.setLayoutCount = 1;
        pipeline_info.pSetLayouts = &descriptor_layout;
        pipeline_info.pushConstantRangeCount = 1;
        pipeline_info.pPushConstantRanges = &push_range;
        check(vkCreatePipelineLayout(device, &pipeline_info, nullptr, &pipeline_layout), "vkCreatePipelineLayout");
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<std::uint32_t>(count * 2)};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = static_cast<std::uint32_t>(count);
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        check(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool), "vkCreateDescriptorPool");
    }

    void create_command_resources(std::uint32_t family) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool), "vkCreateCommandPool(shader)");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = command_pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &allocation, &command), "vkAllocateCommandBuffers(shader)");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence(shader)");
    }

    Buffer create_buffer() {
        Buffer result{};
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = byte_count;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &info, nullptr, &result.buffer), "vkCreateBuffer(shader)");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &allocation, nullptr, &result.memory), "vkAllocateMemory(shader)");
        check(vkBindBufferMemory(device, result.buffer, result.memory, 0), "vkBindBufferMemory(shader)");
        return result;
    }

    Stage create_stage(const std::filesystem::path& path, float strength) {
        Stage result{};
        result.strength = strength;
        const auto code = read_spirv(path);
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = code.size() * sizeof(std::uint32_t);
        module_info.pCode = code.data();
        check(vkCreateShaderModule(device, &module_info, nullptr, &result.module), "vkCreateShaderModule");
        VkPipelineShaderStageCreateInfo stage_info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = result.module;
        stage_info.pName = "main";
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage = stage_info;
        pipeline_info.layout = pipeline_layout;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &result.pipeline),
              "vkCreateComputePipelines");
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_info.descriptorPool = descriptor_pool;
        set_info.descriptorSetCount = 1;
        set_info.pSetLayouts = &descriptor_layout;
        check(vkAllocateDescriptorSets(device, &set_info, &result.descriptor), "vkAllocateDescriptorSets");
        return result;
    }

    void update_descriptors() {
        for (std::size_t index = 0; index < stages.size(); ++index) {
            const VkDescriptorBufferInfo input_info{index == 0 ? input : outputs[index - 1].buffer, 0, byte_count};
            const VkDescriptorBufferInfo output_info{outputs[index].buffer, 0, byte_count};
            const VkWriteDescriptorSet writes[] = {
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, stages[index].descriptor, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &input_info, nullptr},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, stages[index].descriptor, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &output_info, nullptr},
            };
            vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        }
    }

    std::vector<std::uint8_t> execute() {
        if (stages.empty()) return {};
        check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer(shader)");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer(shader)");
        VkMemoryBarrier initial{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        initial.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
        initial.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &initial, 0, nullptr, 0, nullptr);
        for (std::size_t index = 0; index < stages.size(); ++index) {
            const auto& stage = stages[index];
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, stage.pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                                    &stage.descriptor, 0, nullptr);
            const Push push{width, height, stage.strength};
            vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(command, (width * height + 63U) / 64U, 1, 1);
            if (index + 1 < stages.size()) {
                VkMemoryBarrier between{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                between.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                between.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &between, 0, nullptr, 0, nullptr);
            }
        }
        VkMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &to_transfer, 0, nullptr, 0, nullptr);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier image_to_destination{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image_to_destination.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image_to_destination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        image_to_destination.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image_to_destination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image_to_destination.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_to_destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_to_destination.image = target_image;
        image_to_destination.subresourceRange = range;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &image_to_destination);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(command, outputs.back().buffer, target_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier image_to_source{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image_to_source.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        image_to_source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image_to_source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image_to_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image_to_source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_to_source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_to_source.image = target_image;
        image_to_source.subresourceRange = range;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &image_to_source);
        vkCmdCopyImageToBuffer(command, target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, input, 1, &copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(command), "vkEndCommandBuffer(shader)");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit(shader)");
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(shader)");
        check(vkResetFences(device, 1, &fence), "vkResetFences(shader)");
        void* mapped = nullptr;
        check(vkMapMemory(device, outputs.back().memory, 0, byte_count, 0, &mapped), "vkMapMemory(shader)");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byte_count));
        std::memcpy(bytes.data(), mapped, bytes.size());
        vkUnmapMemory(device, outputs.back().memory);
        return bytes;
    }

    void destroy() noexcept {
        if (device == VK_NULL_HANDLE) return;
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
        for (auto& stage : stages) {
            if (stage.pipeline) vkDestroyPipeline(device, stage.pipeline, nullptr);
            if (stage.module) vkDestroyShaderModule(device, stage.module, nullptr);
        }
        if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (descriptor_layout) vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
        for (auto& output : outputs) {
            if (output.buffer) vkDestroyBuffer(device, output.buffer, nullptr);
            if (output.memory) vkFreeMemory(device, output.memory, nullptr);
        }
    }

    ~Impl() { destroy(); }
};

ShaderExecutor::ShaderExecutor(VkPhysicalDevice physical_device, VkDevice device, VkQueue queue,
                               std::uint32_t queue_family, VkBuffer input, VkImage target_image, std::uint32_t width,
                               std::uint32_t height, const std::filesystem::path& shader_directory,
                               std::vector<ShaderEffect> effects)
    : impl_(std::make_unique<Impl>(physical_device, device, queue, queue_family, input, target_image, width, height,
                                   shader_directory, effects)) {}
ShaderExecutor::~ShaderExecutor() = default;
ShaderExecutor::ShaderExecutor(ShaderExecutor&&) noexcept = default;
ShaderExecutor& ShaderExecutor::operator=(ShaderExecutor&&) noexcept = default;
std::vector<std::uint8_t> ShaderExecutor::execute() { return impl_->execute(); }
bool ShaderExecutor::empty() const noexcept { return impl_->stages.empty(); }

}  // namespace neuroshade::vulkan
