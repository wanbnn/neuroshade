#pragma once

#include "runtime/pipeline_plan.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neuroshade::layer {

struct PresentDispatch {
    PFN_vkGetMemoryFdKHR get_memory_fd{};
    unsigned char device_pci[VK_UUID_SIZE]{};
    PFN_vkCreateQueryPool create_query_pool{};
    PFN_vkDestroyQueryPool destroy_query_pool{};
    PFN_vkCmdResetQueryPool cmd_reset_query_pool{};
    PFN_vkCmdWriteTimestamp cmd_write_timestamp{};
    PFN_vkGetQueryPoolResults get_query_pool_results{};
    double timestamp_period{};
    std::uint32_t timestamp_bits{};
    PFN_vkCreateBuffer create_buffer{};
    PFN_vkDestroyBuffer destroy_buffer{};
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements{};
    PFN_vkAllocateMemory allocate_memory{};
    PFN_vkFreeMemory free_memory{};
    PFN_vkBindBufferMemory bind_buffer_memory{};
    PFN_vkMapMemory map_memory{};
    PFN_vkUnmapMemory unmap_memory{};
    PFN_vkCreateShaderModule create_shader_module{};
    PFN_vkDestroyShaderModule destroy_shader_module{};
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout{};
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout{};
    PFN_vkCreatePipelineLayout create_pipeline_layout{};
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout{};
    PFN_vkCreateDescriptorPool create_descriptor_pool{};
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool{};
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets{};
    PFN_vkUpdateDescriptorSets update_descriptor_sets{};
    PFN_vkCreateComputePipelines create_compute_pipelines{};
    PFN_vkDestroyPipeline destroy_pipeline{};
    PFN_vkCreateCommandPool create_command_pool{};
    PFN_vkDestroyCommandPool destroy_command_pool{};
    PFN_vkAllocateCommandBuffers allocate_command_buffers{};
    PFN_vkBeginCommandBuffer begin_command_buffer{};
    PFN_vkEndCommandBuffer end_command_buffer{};
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier{};
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer{};
    PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image{};
    PFN_vkCmdBindPipeline cmd_bind_pipeline{};
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets{};
    PFN_vkCmdPushConstants cmd_push_constants{};
    PFN_vkCmdDispatch cmd_dispatch{};
    PFN_vkCreateSemaphore create_semaphore{};
    PFN_vkDestroySemaphore destroy_semaphore{};
    PFN_vkCreateFence create_fence{};
    PFN_vkDestroyFence destroy_fence{};
    PFN_vkResetFences reset_fences{};
    PFN_vkWaitForFences wait_for_fences{};
    PFN_vkQueueSubmit queue_submit{};

    [[nodiscard]] bool complete() const noexcept;
};

class PresentProcessor {
public:
    static std::unique_ptr<PresentProcessor> create(
        VkDevice device, const VkPhysicalDeviceMemoryProperties& memory_properties,
        std::uint32_t queue_family, VkExtent2D extent, VkFormat format,
        const std::vector<VkImage>& images,
        const runtime::PipelinePreparation& pipeline, PresentDispatch dispatch,
        const std::string& overlay_shader, std::string& error);

    ~PresentProcessor();
    PresentProcessor(const PresentProcessor&) = delete;
    PresentProcessor& operator=(const PresentProcessor&) = delete;

    [[nodiscard]] VkResult submit(VkQueue queue, std::uint32_t image_index,
                                  std::uint32_t wait_count,
                                  const VkSemaphore* wait_semaphores,
                                  bool overlay_visible,
                                  VkSemaphore& completion,
                                  const std::vector<std::uint32_t>* overlay_pixels = nullptr) noexcept;
    bool update_nr(const runtime::PipelinePreparation& pipeline,std::string& error);
    [[nodiscard]] double average_ms() const noexcept;
    [[nodiscard]] bool profiler_gpu_backed() const noexcept;
    [[nodiscard]] double inference_ms() const noexcept;
    [[nodiscard]] std::string runtime_status() const;

private:
    struct Impl;
    explicit PresentProcessor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool present_format_supported(VkFormat format) noexcept;

}  // namespace neuroshade::layer
