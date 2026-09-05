#include "layer/presentation/present_processor.hpp"
#include "logging/log.hpp"
#include "neural/runtime/host_client.hpp"

#if defined(NEUROSHADE_LAYER_NEURAL)
#include "neural/model/package.hpp"
#include "neural/runtime/spatial_runtime.hpp"
#endif
#if defined(NEUROSHADE_LAYER_TEMPORAL)
#include "neural/temporal/temporal_runtime.hpp"
#include "telemetry/profiler.hpp"
#endif

#include <algorithm>
#include <fstream>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>

namespace nslog = neuroshade::logging;

namespace neuroshade::layer {
namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 std::to_string(result));
    }
}

[[nodiscard]] std::vector<std::uint32_t> read_spirv(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("unable to open shader: " + path);
    const auto size = stream.tellg();
    if (size <= 0 || size % 4 != 0) throw std::runtime_error("invalid shader: " + path);
    std::vector<std::uint32_t> code(static_cast<std::size_t>(size) / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(code.data()), size);
    if (!stream) throw std::runtime_error("unable to read shader: " + path);
    return code;
}

[[nodiscard]] std::uint32_t memory_type(
    const VkPhysicalDeviceMemoryProperties& properties, std::uint32_t allowed,
    VkMemoryPropertyFlags required) {
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((allowed & (std::uint32_t{1} << index)) != 0 &&
            (properties.memoryTypes[index].propertyFlags & required) == required) {
            return index;
        }
    }
    throw std::runtime_error("no compatible memory type for present buffers");
}

}  // namespace

bool PresentDispatch::complete() const noexcept {
    return create_buffer && destroy_buffer && get_buffer_memory_requirements &&
           allocate_memory && free_memory && bind_buffer_memory && map_memory && unmap_memory &&
           create_shader_module && destroy_shader_module &&
           create_descriptor_set_layout && destroy_descriptor_set_layout &&
           create_pipeline_layout && destroy_pipeline_layout && create_descriptor_pool &&
           destroy_descriptor_pool && allocate_descriptor_sets && update_descriptor_sets &&
           create_compute_pipelines && destroy_pipeline && create_command_pool &&
           destroy_command_pool && allocate_command_buffers && begin_command_buffer &&
           end_command_buffer && cmd_pipeline_barrier && cmd_copy_image_to_buffer &&
           cmd_copy_buffer_to_image && cmd_bind_pipeline && cmd_bind_descriptor_sets &&
           cmd_push_constants && cmd_dispatch && create_semaphore && destroy_semaphore &&
           create_fence && destroy_fence && reset_fences && wait_for_fences && queue_submit;
}

bool present_format_supported(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return true;
        default:
            return false;
    }
}

struct PresentProcessor::Impl {
    struct Buffer {
        VkBuffer handle{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        void* mapped{};
    };
    struct Stage {
        VkShaderModule module{VK_NULL_HANDLE};
        VkPipeline pipeline{VK_NULL_HANDLE};
        float strength{1.0F};
    };
    struct ImageResources {
        Buffer input;
        Buffer overlay;
        VkQueryPool queries{};
        bool submitted{};
        std::vector<Buffer> outputs;
        std::vector<VkDescriptorSet> descriptors;
        std::array<VkCommandBuffer, 3> commands{};
        VkFence download_fence{};
        VkSemaphore completion{VK_NULL_HANDLE};
        VkFence transfer_fence{VK_NULL_HANDLE};
    };
    struct Push {
        std::uint32_t width;
        std::uint32_t height;
        float strength;
    };

    VkDevice device{VK_NULL_HANDLE};
    PresentDispatch dispatch{};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkDeviceSize byte_count{};
    VkDescriptorSetLayout descriptor_layout{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    std::vector<Stage> stages;
    std::vector<ImageResources> image_resources;
    std::size_t base_stage_count{};
    double gpu_ms{};
    bool neural_mode{};
    bool external_mode{};
    std::unique_ptr<neural::HostClient> host;
    float host_strength{1.f};
    bool host_logged{},host_failed{};
    bool temporal_mode{};
#if defined(NEUROSHADE_LAYER_NEURAL)
    std::unique_ptr<neural::SpatialRuntime> neural_runtime;
    std::string neural_input_name;
    std::vector<float> neural_input;
    std::vector<float> neural_output;
    float neural_strength{1.0F};
    bool neural_result_logged{};
    std::uint64_t neural_frame_count{};
#endif
#if defined(NEUROSHADE_LAYER_TEMPORAL)
    std::unique_ptr<neural::TemporalRuntime> temporal_runtime;
    std::unique_ptr<telemetry::PassProfiler> profiler;
#endif

    Impl(VkDevice logical_device,
         const VkPhysicalDeviceMemoryProperties& memory_properties,
         std::uint32_t queue_family, VkExtent2D image_extent,
         const std::vector<VkImage>& images,
         const runtime::PipelinePreparation& pipeline, PresentDispatch functions,
         const std::string& overlay_shader, VkFormat image_format)
        : device(logical_device), dispatch(functions), extent(image_extent), format(image_format),
          byte_count(static_cast<VkDeviceSize>(extent.width) * extent.height * 4) {
        if (!dispatch.complete()) throw std::runtime_error("incomplete present dispatch");
        if (images.empty() || (pipeline.effects.empty() && overlay_shader.empty())) {
            throw std::runtime_error("present pipeline has no images or effects");
        }
        neural_mode = pipeline.mode == runtime::PipelineMode::neural_spatial ||
                      pipeline.mode == runtime::PipelineMode::neural_temporal;
        temporal_mode = pipeline.mode == runtime::PipelineMode::neural_temporal;
        external_mode=neural_mode && !temporal_mode;
#if defined(NEUROSHADE_LAYER_NEURAL)
        if(external_mode && sizeof(void*)==8) {
            const auto loaded=neural::load_model_package(pipeline.effects.front().artifact);
            if(loaded.valid() && loaded.package.manifest.runtime=="migraphx") external_mode=false;
        }
#endif
        if(external_mode) {
            if(pipeline.effects.empty() || pipeline.effects.front().backend!=runtime::EffectBackend::neural ||
               std::count_if(pipeline.effects.begin(),pipeline.effects.end(),[](const auto& e){return e.backend==runtime::EffectBackend::neural;})!=1)
                throw std::runtime_error("host64 requires one neural effect first, followed by shaders");
            host=std::make_unique<neural::HostClient>(pipeline.effects.front().artifact,extent.width,extent.height);
            host_strength=pipeline.effects.front().strength;
            neural_mode=false;
            nslog::write(nslog::Level::info,"host64_connected="+host->description());
        }
        try {
            create_command_pool(queue_family);
            if (neural_mode) {
                create_neural(pipeline, images, memory_properties);
            } else {
                for (const auto& effect : pipeline.effects) {
                    if (effect.backend != runtime::EffectBackend::shader && !external_mode) {
                        throw std::runtime_error("mixed shader/neural present pipelines are unsupported");
                    }
                }
                create_layouts(images.size() *
                               (pipeline.effects.size() + (overlay_shader.empty() ? 0U : 1U)));
                create_stages(pipeline);
                base_stage_count = stages.size();
                if (!overlay_shader.empty()) create_stage(overlay_shader, (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) ? 1.0F : -1.0F);
                create_images(images, memory_properties, external_mode);
                update_descriptors();
                record_commands(images);
            }
        } catch (...) {
            destroy();
            throw;
        }
    }

    void create_layouts(std::size_t descriptor_count) {
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo descriptor_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptor_info.bindingCount = 3;
        descriptor_info.pBindings = bindings;
        check(dispatch.create_descriptor_set_layout(device, &descriptor_info, nullptr,
                                                     &descriptor_layout),
              "vkCreateDescriptorSetLayout(present)");
        const VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pipeline_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_info.setLayoutCount = 1;
        pipeline_info.pSetLayouts = &descriptor_layout;
        pipeline_info.pushConstantRangeCount = 1;
        pipeline_info.pPushConstantRanges = &push_range;
        check(dispatch.create_pipeline_layout(device, &pipeline_info, nullptr, &pipeline_layout),
              "vkCreatePipelineLayout(present)");
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       static_cast<std::uint32_t>(descriptor_count * 3)};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = static_cast<std::uint32_t>(descriptor_count);
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        check(dispatch.create_descriptor_pool(device, &pool_info, nullptr, &descriptor_pool),
              "vkCreateDescriptorPool(present)");
    }

    void create_stage(const std::string& artifact, float strength) {
        Stage stage{};
        stage.strength = strength;
        try {
                const auto code = read_spirv(artifact);
                VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                module_info.codeSize = code.size() * sizeof(std::uint32_t);
                module_info.pCode = code.data();
                check(dispatch.create_shader_module(device, &module_info, nullptr, &stage.module),
                      "vkCreateShaderModule(present)");
                VkPipelineShaderStageCreateInfo shader_info{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                shader_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                shader_info.module = stage.module;
                shader_info.pName = "main";
                VkComputePipelineCreateInfo pipeline_info{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                pipeline_info.stage = shader_info;
                pipeline_info.layout = pipeline_layout;
                check(dispatch.create_compute_pipelines(device, VK_NULL_HANDLE, 1,
                                                         &pipeline_info, nullptr,
                                                         &stage.pipeline),
                      "vkCreateComputePipelines(present)");
                stages.push_back(stage);
        } catch (...) {
            if (stage.pipeline) dispatch.destroy_pipeline(device, stage.pipeline, nullptr);
            if (stage.module) dispatch.destroy_shader_module(device, stage.module, nullptr);
            throw;
        }
    }

    void create_stages(const runtime::PipelinePreparation& pipeline) {
        stages.reserve(pipeline.effects.size() + 1);
        for (const auto& effect : pipeline.effects) {
            if(external_mode && effect.backend==runtime::EffectBackend::neural) continue;
            create_stage(effect.artifact, effect.strength);
        }
    }

    void create_command_pool(std::uint32_t queue_family) {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.queueFamilyIndex = queue_family;
        check(dispatch.create_command_pool(device, &info, nullptr, &command_pool),
              "vkCreateCommandPool(present)");
    }

    Buffer create_buffer(const VkPhysicalDeviceMemoryProperties& properties,
                         bool host_visible) {
        Buffer buffer{};
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = byte_count;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(dispatch.create_buffer(device, &info, nullptr, &buffer.handle),
              "vkCreateBuffer(present)");
        try {
            VkMemoryRequirements requirements{};
            dispatch.get_buffer_memory_requirements(device, buffer.handle, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            const VkMemoryPropertyFlags required = host_visible
                ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            allocation.memoryTypeIndex = memory_type(properties, requirements.memoryTypeBits,
                                                      required);
            check(dispatch.allocate_memory(device, &allocation, nullptr, &buffer.memory),
                  "vkAllocateMemory(present)");
            check(dispatch.bind_buffer_memory(device, buffer.handle, buffer.memory, 0),
                  "vkBindBufferMemory(present)");
            if (host_visible) {
                check(dispatch.map_memory(device, buffer.memory, 0, byte_count, 0,
                                          &buffer.mapped),
                      "vkMapMemory(present)");
            }
        } catch (...) {
            if (buffer.memory) dispatch.free_memory(device, buffer.memory, nullptr);
            dispatch.destroy_buffer(device, buffer.handle, nullptr);
            throw;
        }
        return buffer;
    }

    void create_images(const std::vector<VkImage>& images,
                       const VkPhysicalDeviceMemoryProperties& properties,
                       bool host_visible) {
        std::vector<VkDescriptorSetLayout> layouts(stages.size(), descriptor_layout);
        image_resources.resize(images.size());
        for (auto& resources : image_resources) {
            resources.input = create_buffer(properties, host_visible);
            if (!neural_mode) resources.overlay = create_buffer(properties, true);
            const std::size_t output_count = neural_mode ? 1 : stages.size();
            resources.outputs.reserve(output_count);
            for (std::size_t index = 0; index < output_count; ++index) {
                resources.outputs.push_back(create_buffer(properties, host_visible && neural_mode));
            }
            if (!neural_mode) {
                resources.descriptors.resize(stages.size());
                VkDescriptorSetAllocateInfo allocation{
                    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                allocation.descriptorPool = descriptor_pool;
                allocation.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
                allocation.pSetLayouts = layouts.data();
                check(dispatch.allocate_descriptor_sets(device, &allocation,
                                                         resources.descriptors.data()),
                      "vkAllocateDescriptorSets(present)");
            }
            VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(dispatch.create_semaphore(device, &semaphore_info, nullptr,
                                            &resources.completion),
                  "vkCreateSemaphore(present)");
            {
                VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                if (!neural_mode) fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                check(dispatch.create_fence(device, &fence_info, nullptr,
                                            &resources.transfer_fence),
                      "vkCreateFence(present neural)");
                if (!neural_mode && dispatch.timestamp_bits && dispatch.timestamp_period > 0 &&
                    dispatch.create_query_pool && dispatch.destroy_query_pool &&
                    dispatch.cmd_reset_query_pool && dispatch.cmd_write_timestamp && dispatch.get_query_pool_results) {
                    VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                    query.queryType = VK_QUERY_TYPE_TIMESTAMP;
                    query.queryCount = 2;
                    if (dispatch.create_query_pool(device, &query, nullptr, &resources.queries) != VK_SUCCESS)
                        resources.queries = VK_NULL_HANDLE;
                }
            }
        }
            if(external_mode) {
                // Separate download completion from the final shader submission.
                for(auto& resources:image_resources) {
                    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                    check(dispatch.create_fence(device,&fence,nullptr,&resources.download_fence),"vkCreateFence(host64)");
                }
            }
        std::vector<VkCommandBuffer> commands(images.size() * 3);
        VkCommandBufferAllocateInfo command_info{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_info.commandPool = command_pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = static_cast<std::uint32_t>(commands.size());
        check(dispatch.allocate_command_buffers(device, &command_info, commands.data()),
              "vkAllocateCommandBuffers(present)");
        for (std::size_t index = 0; index < commands.size(); ++index) {
            image_resources[index / 3].commands[index % 3] = commands[index];
        }
    }

    void record_neural_transfer(VkCommandBuffer command, VkImage image,
                                VkBuffer buffer, bool download) {
        const VkBufferImageCopy copy{{}, 0, 0,
                                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     {0, 0, 0}, {extent.width, extent.height, 1}};
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        check(dispatch.begin_command_buffer(command, &begin),
              "vkBeginCommandBuffer(present neural)");
        if (download) {
            image_barrier(command, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
            dispatch.cmd_copy_image_to_buffer(command, image,
                                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                              buffer, 1, &copy);
            buffer_barrier(command,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);
            image_barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        } else {
            image_barrier(command, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
            dispatch.cmd_copy_buffer_to_image(command, buffer,
                                              image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              1, &copy);
            image_barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        check(dispatch.end_command_buffer(command),
              "vkEndCommandBuffer(present neural)");
    }

    void create_neural(const runtime::PipelinePreparation& pipeline,
                       const std::vector<VkImage>& images,
                       const VkPhysicalDeviceMemoryProperties& properties) {
#if defined(NEUROSHADE_LAYER_NEURAL)
        if (pipeline.effects.size() != 1 ||
            pipeline.effects.front().backend != runtime::EffectBackend::neural) {
            throw std::runtime_error("neural present requires exactly one neural effect");
        }
        auto loaded = neural::load_model_package(pipeline.effects.front().artifact);
        if (!loaded.valid()) throw std::runtime_error("neural present model is invalid");
        const auto& manifest = loaded.package.manifest;
        if (manifest.inputs.empty() || manifest.inputs.front().dtype != "fp32" ||
            manifest.inputs.front().layout != "NCHW" ||
            manifest.output.dtype != "fp32" || manifest.output.layout != "NCHW" ||
            manifest.buckets.size() != 1) {
            throw std::runtime_error("neural present supports one fixed spatial FP32 NCHW model");
        }
        const auto& bucket = manifest.buckets.front();
        if (bucket.input.size() != 4 || bucket.output.size() != 4 ||
            bucket.input[0] != 1 || bucket.input[1] != 4 ||
            bucket.output[0] != 1 || bucket.output[1] != 4 ||
            bucket.input[2] != extent.height || bucket.input[3] != extent.width ||
            bucket.output[2] != extent.height || bucket.output[3] != extent.width) {
            throw std::runtime_error("swapchain extent does not match neural model bucket");
        }
        neural_strength = pipeline.effects.front().strength;
        const auto architecture = neural::active_hip_architecture();
#if defined(NEUROSHADE_LAYER_TEMPORAL)
        profiler = std::make_unique<telemetry::PassProfiler>();
#endif
        if (temporal_mode) {
#if defined(NEUROSHADE_LAYER_TEMPORAL)
            if (manifest.history == 0) {
                throw std::runtime_error("temporal present model declares no history");
            }
            temporal_runtime = std::make_unique<neural::TemporalRuntime>(
                std::move(loaded.package), architecture);
            if (temporal_runtime->layout().non_history_inputs.size() != 1) {
                throw std::runtime_error("temporal present requires one current-frame input");
            }
            neural_input_name = temporal_runtime->layout().primary_non_history_tensor;
#else
            throw std::runtime_error("temporal present runtime was not built");
#endif
        } else {
            if (manifest.history != 0 || manifest.inputs.size() != 1) {
                throw std::runtime_error("spatial present requires one input and no history");
            }
            neural_input_name = manifest.inputs.front().tensor;
            neural_runtime = std::make_unique<neural::SpatialRuntime>(
                std::move(loaded.package), architecture);
        }
        const std::size_t elements = static_cast<std::size_t>(extent.width) * extent.height * 4;
        neural_input.resize(elements);
        neural_output.resize(elements);
        create_images(images, properties, true);
        for (std::size_t index = 0; index < images.size(); ++index) {
            record_neural_transfer(image_resources[index].commands[0], images[index],
                                   image_resources[index].input.handle, true);
            record_neural_transfer(image_resources[index].commands[1], images[index],
                                   image_resources[index].outputs.front().handle, false);
        }
#else
        (void)pipeline;
        (void)images;
        (void)properties;
        throw std::runtime_error("neural present runtime was not built");
#endif
    }

    void process_neural(ImageResources& resources) noexcept {
#if defined(NEUROSHADE_LAYER_NEURAL)
        auto* input = static_cast<const std::uint8_t*>(resources.input.mapped);
        auto* output = static_cast<std::uint8_t*>(resources.outputs.front().mapped);
        try {
#if defined(NEUROSHADE_LAYER_TEMPORAL)
            profiler->begin_pass("present.neural");
#endif
            const std::size_t pixels = static_cast<std::size_t>(extent.width) * extent.height;
            const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM ||
                              format == VK_FORMAT_B8G8R8A8_SRGB;
            for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
                const std::size_t raw = pixel * 4;
                neural_input[pixel] = input[raw + (bgra ? 2 : 0)] / 255.0F;
                neural_input[pixels + pixel] = input[raw + 1] / 255.0F;
                neural_input[pixels * 2 + pixel] = input[raw + (bgra ? 0 : 2)] / 255.0F;
                neural_input[pixels * 3 + pixel] = input[raw + 3] / 255.0F;
            }
            const auto input_bytes =
                std::as_bytes(std::span<const float>(neural_input.data(), neural_input.size()));
            const auto output_bytes =
                std::as_writable_bytes(std::span<float>(neural_output.data(),
                                                         neural_output.size()));
            bool executed = false;
            if (temporal_mode) {
#if defined(NEUROSHADE_LAYER_TEMPORAL)
                temporal_runtime->upload_current(neural_input_name, input_bytes);
                const auto result = temporal_runtime->step(false);
                executed = result != neural::TemporalRuntime::ExecutionResult::copy_fallback;
                temporal_runtime->download_output(output_bytes);
#endif
            } else {
                neural_runtime->upload_input(neural_input_name, input_bytes);
                executed = neural_runtime->execute() == neural::ExecutionResult::neural;
                neural_runtime->download_output(output_bytes);
            }
#if defined(NEUROSHADE_LAYER_TEMPORAL)
            profiler->end_pass("present.neural");
#endif
            const float strength = std::clamp(neural_strength, 0.0F, 1.0F);
            const auto channel = [&](std::size_t plane, std::size_t pixel,
                                     std::uint8_t original) {
                const float inferred = std::clamp(neural_output[plane * pixels + pixel],
                                                  0.0F, 1.0F);
                const float blended = (original / 255.0F) * (1.0F - strength) +
                                      inferred * strength;
                return static_cast<std::uint8_t>(std::lround(std::clamp(blended, 0.0F, 1.0F) *
                                                             255.0F));
            };
            for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
                const std::size_t raw = pixel * 4;
                output[raw + (bgra ? 2 : 0)] = channel(0, pixel, input[raw + (bgra ? 2 : 0)]);
                output[raw + 1] = channel(1, pixel, input[raw + 1]);
                output[raw + (bgra ? 0 : 2)] = channel(2, pixel, input[raw + (bgra ? 0 : 2)]);
                output[raw + 3] = channel(3, pixel, input[raw + 3]);
            }
            const bool output_changed =
                std::memcmp(input, output, static_cast<std::size_t>(byte_count)) != 0;
            if (!neural_result_logged) {
                nslog::write(executed ? nslog::Level::info : nslog::Level::warning,
                             std::string("neural_present=") +
                                 (executed ? "executed" : "copy-fallback") +
                                 " runtime=MIGraphX mode=" +
                                 (temporal_mode ? "temporal" : "spatial") +
                                 " interop=HOST-STAGING-FALLBACK output_changed=" +
                                 (output_changed ? "yes" : "no") +
                                 " profiler=" +
                                 (profiler->gpu_backed() ? "HIP_EVENTS" : "CPU_CLOCK") +
                                 " average_ms=" + profiler->format_ms(
                                     profiler->avg_ms("present.neural")));
                neural_result_logged = true;
            }
            ++neural_frame_count;
#if defined(NEUROSHADE_LAYER_TEMPORAL)
            if (temporal_mode && neural_frame_count == 2) {
                nslog::write(nslog::Level::info,
                             std::string("temporal_present=executed frames=2 history=") +
                                 (temporal_runtime->history_valid() ? "valid" : "invalid") +
                                 " output_changed=" + (output_changed ? "yes" : "no"));
            }
#endif
        } catch (...) {
            std::memcpy(output, input, static_cast<std::size_t>(byte_count));
            if (!neural_result_logged) {
                nslog::write(nslog::Level::warning,
                             "neural_present=copy-fallback reason=execution-exception");
                neural_result_logged = true;
            }
        }
#else
        (void)resources;
#endif
    }

    void update_descriptors() {
        for (auto& resources : image_resources) {
            for (std::size_t index = 0; index < stages.size(); ++index) {
                const VkBuffer source = index == 0 ? resources.input.handle
                                                   : resources.outputs[index - 1].handle;
                const VkDescriptorBufferInfo input{source, 0, byte_count};
                const VkDescriptorBufferInfo output{resources.outputs[index].handle, 0,
                                                     byte_count};
                const VkDescriptorBufferInfo overlay{resources.overlay.handle, 0, byte_count};
                const VkWriteDescriptorSet writes[] = {
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     resources.descriptors[index], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &input, nullptr},
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     resources.descriptors[index], 1, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &output, nullptr},
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     resources.descriptors[index], 2, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &overlay, nullptr},
                };
                dispatch.update_descriptor_sets(device, 3, writes, 0, nullptr);
            }
        }
    }

    void buffer_barrier(VkCommandBuffer command, VkAccessFlags source_access,
                        VkAccessFlags destination_access, VkPipelineStageFlags source_stage,
                        VkPipelineStageFlags destination_stage) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        dispatch.cmd_pipeline_barrier(command, source_stage, destination_stage, 0, 1,
                                      &barrier, 0, nullptr, 0, nullptr);
    }

    void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout,
                       VkImageLayout new_layout, VkAccessFlags source_access,
                       VkAccessFlags destination_access, VkPipelineStageFlags source_stage,
                       VkPipelineStageFlags destination_stage) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dispatch.cmd_pipeline_barrier(command, source_stage, destination_stage, 0, 0,
                                      nullptr, 0, nullptr, 1, &barrier);
    }

    void record_command(VkCommandBuffer command, VkImage image,
                        ImageResources& resources, std::size_t stage_count) {
        const VkBufferImageCopy copy{{}, 0, 0,
                                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     {0, 0, 0}, {extent.width, extent.height, 1}};
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
            check(dispatch.begin_command_buffer(command, &begin),
                  "vkBeginCommandBuffer(present)");
            if (resources.queries) {
                dispatch.cmd_reset_query_pool(command, resources.queries, 0, 2);
                dispatch.cmd_write_timestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, resources.queries, 0);
            }
            image_barrier(command, image,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
            if(!external_mode) {
            dispatch.cmd_copy_image_to_buffer(command, image,
                                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                              resources.input.handle, 1, &copy);
            } else {
                buffer_barrier(command,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            }
            buffer_barrier(command, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            for (std::size_t stage_index = 0; stage_index < stage_count; ++stage_index) {
                const auto& stage = stages[stage_index];
                dispatch.cmd_bind_pipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                           stage.pipeline);
                dispatch.cmd_bind_descriptor_sets(
                    command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                    &resources.descriptors[stage_index], 0, nullptr);
                const Push push{extent.width, extent.height, stage.strength};
                dispatch.cmd_push_constants(command, pipeline_layout,
                                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                dispatch.cmd_dispatch(command,
                                      (extent.width * extent.height + 63U) / 64U, 1, 1);
                if (stage_index + 1 < stage_count) {
                    buffer_barrier(command, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                }
            }
            buffer_barrier(command, stage_count ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT,
                           stage_count ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
            image_barrier(command, image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            dispatch.cmd_copy_buffer_to_image(command,
                                              stage_count ? resources.outputs[stage_count - 1].handle : resources.input.handle,
                                              image,
                                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            image_barrier(command, image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            if (resources.queries)
                dispatch.cmd_write_timestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, resources.queries, 1);
            check(dispatch.end_command_buffer(command),
                  "vkEndCommandBuffer(present)");
    }

    void record_commands(const std::vector<VkImage>& images) {
        for (std::size_t image_index = 0; image_index < images.size(); ++image_index) {
            auto& resources = image_resources[image_index];
            record_command(resources.commands[0], images[image_index], resources,
                           base_stage_count);
            record_command(resources.commands[1], images[image_index], resources,
                           stages.size());
            if(external_mode) record_neural_transfer(resources.commands[2],images[image_index],resources.input.handle,true);
        }
    }

    void destroy_buffer(Buffer& buffer) noexcept {
        if (buffer.mapped) dispatch.unmap_memory(device, buffer.memory);
        if (buffer.handle) dispatch.destroy_buffer(device, buffer.handle, nullptr);
        if (buffer.memory) dispatch.free_memory(device, buffer.memory, nullptr);
        buffer = {};
    }

    void destroy() noexcept {
        if (!device) return;
        for (auto& resources : image_resources) {
            if (resources.completion) {
                dispatch.destroy_semaphore(device, resources.completion, nullptr);
            }
            if(resources.download_fence)dispatch.destroy_fence(device,resources.download_fence,nullptr);
            if (resources.transfer_fence) {
                dispatch.destroy_fence(device, resources.transfer_fence, nullptr);
            }
            if (resources.queries) dispatch.destroy_query_pool(device, resources.queries, nullptr);
            destroy_buffer(resources.overlay);
            destroy_buffer(resources.input);
            for (auto& output : resources.outputs) destroy_buffer(output);
        }
        if (command_pool) dispatch.destroy_command_pool(device, command_pool, nullptr);
        for (auto& stage : stages) {
            if (stage.pipeline) dispatch.destroy_pipeline(device, stage.pipeline, nullptr);
            if (stage.module) dispatch.destroy_shader_module(device, stage.module, nullptr);
        }
        if (descriptor_pool) dispatch.destroy_descriptor_pool(device, descriptor_pool, nullptr);
        if (pipeline_layout) dispatch.destroy_pipeline_layout(device, pipeline_layout, nullptr);
        if (descriptor_layout) {
            dispatch.destroy_descriptor_set_layout(device, descriptor_layout, nullptr);
        }
    }

    ~Impl() { destroy(); }
};

PresentProcessor::PresentProcessor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PresentProcessor::~PresentProcessor() = default;

std::unique_ptr<PresentProcessor> PresentProcessor::create(
    VkDevice device, const VkPhysicalDeviceMemoryProperties& memory_properties,
    std::uint32_t queue_family, VkExtent2D extent, VkFormat format,
    const std::vector<VkImage>& images, const runtime::PipelinePreparation& pipeline,
    PresentDispatch dispatch, const std::string& overlay_shader, std::string& error) {
    if (!present_format_supported(format)) {
        error = "unsupported swapchain format " + std::to_string(format);
        return nullptr;
    }
    try {
        return std::unique_ptr<PresentProcessor>(new PresentProcessor(std::make_unique<Impl>(
            device, memory_properties, queue_family, extent, images, pipeline, dispatch,
            overlay_shader, format)));
    } catch (const std::exception& exception) {
        error = exception.what();
        return nullptr;
    }
}

VkResult PresentProcessor::submit(VkQueue queue, std::uint32_t image_index,
                                  std::uint32_t wait_count,
                                  const VkSemaphore* wait_semaphores,
                                  bool overlay_visible,
                                  VkSemaphore& completion,
                                  const std::vector<std::uint32_t>* overlay_pixels) noexcept {
    completion = VK_NULL_HANDLE;
    if (image_index >= impl_->image_resources.size()) return VK_ERROR_UNKNOWN;
    if (wait_count > 16) return VK_ERROR_TOO_MANY_OBJECTS;
    auto& resources = impl_->image_resources[image_index];
    std::array<VkPipelineStageFlags, 16> wait_stages{};
    std::fill_n(wait_stages.begin(), wait_count, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (impl_->neural_mode) {
        VkResult result = impl_->dispatch.reset_fences(impl_->device, 1,
                                                       &resources.transfer_fence);
        if (result != VK_SUCCESS) return result;
        VkSubmitInfo download{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        download.waitSemaphoreCount = wait_count;
        download.pWaitSemaphores = wait_semaphores;
        download.pWaitDstStageMask = wait_count == 0 ? nullptr : wait_stages.data();
        download.commandBufferCount = 1;
        download.pCommandBuffers = &resources.commands[0];
        result = impl_->dispatch.queue_submit(queue, 1, &download,
                                              resources.transfer_fence);
        if (result != VK_SUCCESS) return result;
        result = impl_->dispatch.wait_for_fences(impl_->device, 1,
                                                 &resources.transfer_fence, VK_TRUE,
                                                 5'000'000'000ULL);
        if (result != VK_SUCCESS) return result;
        impl_->process_neural(resources);

        VkSubmitInfo upload{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        upload.commandBufferCount = 1;
        upload.pCommandBuffers = &resources.commands[1];
        upload.signalSemaphoreCount = 1;
        upload.pSignalSemaphores = &resources.completion;
        result = impl_->dispatch.queue_submit(queue, 1, &upload, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) completion = resources.completion;
        return result;
    }
    const VkResult waited = impl_->dispatch.wait_for_fences(impl_->device, 1,
        &resources.transfer_fence, VK_TRUE, 5'000'000'000ULL);
    if (waited != VK_SUCCESS) return waited;
    if (resources.submitted && resources.queries) {
        std::uint64_t times[2]{};
        if (impl_->dispatch.get_query_pool_results(impl_->device, resources.queries, 0, 2,
                sizeof(times), times, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const auto bits = impl_->dispatch.timestamp_bits;
            const std::uint64_t mask = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
            const double ms = ((times[1] - times[0]) & mask) * impl_->dispatch.timestamp_period / 1e6;
            impl_->gpu_ms = impl_->gpu_ms == 0 ? ms : impl_->gpu_ms * 0.9 + ms * 0.1;
        }
    }
    if(impl_->external_mode) {
        auto result=impl_->dispatch.reset_fences(impl_->device,1,&resources.download_fence);
        if(result!=VK_SUCCESS)return result;
        VkSubmitInfo download{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        download.waitSemaphoreCount=wait_count;download.pWaitSemaphores=wait_semaphores;
        download.pWaitDstStageMask=wait_count?wait_stages.data():nullptr;
        download.commandBufferCount=1;download.pCommandBuffers=&resources.commands[2];
        result=impl_->dispatch.queue_submit(queue,1,&download,resources.download_fence);
        if(result!=VK_SUCCESS)return result;
        result=impl_->dispatch.wait_for_fences(impl_->device,1,&resources.download_fence,VK_TRUE,5'000'000'000ULL);
        if(result!=VK_SUCCESS)return result;
        const bool bgra=impl_->format==VK_FORMAT_B8G8R8A8_UNORM || impl_->format==VK_FORMAT_B8G8R8A8_SRGB;
        bool executed=impl_->host->process({static_cast<std::uint8_t*>(resources.input.mapped),static_cast<std::size_t>(impl_->byte_count)},
            impl_->extent.width,impl_->extent.height,bgra,impl_->host_strength);
        if((executed && !impl_->host_logged) || (!executed && !impl_->host_failed)) {
            nslog::write(executed?nslog::Level::info:nslog::Level::warning,
                executed?"neural_present=executed runtime=host64 inference_ms="+std::to_string(impl_->host->average_ms()):
                         "neural_present=copy-fallback reason="+impl_->host->error());
            impl_->host_logged=true;impl_->host_failed=!executed;
        }
    }
    if (overlay_visible && resources.overlay.mapped) {
        if (overlay_pixels && overlay_pixels->size() * sizeof(std::uint32_t) == impl_->byte_count)
            std::memcpy(resources.overlay.mapped, overlay_pixels->data(), static_cast<std::size_t>(impl_->byte_count));
        else std::memset(resources.overlay.mapped, 0, static_cast<std::size_t>(impl_->byte_count));
    }
    const VkResult reset = impl_->dispatch.reset_fences(impl_->device, 1, &resources.transfer_fence);
    if (reset != VK_SUCCESS) return reset;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = impl_->external_mode ? 0 : wait_count;
    submit.pWaitSemaphores = impl_->external_mode ? nullptr : wait_semaphores;
    submit.pWaitDstStageMask = submit.waitSemaphoreCount == 0 ? nullptr : wait_stages.data();
    submit.commandBufferCount = 1;
    const VkCommandBuffer command = resources.commands[overlay_visible ? 1 : 0];
    submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &resources.completion;
    const VkResult result = impl_->dispatch.queue_submit(queue, 1, &submit, resources.transfer_fence);
    resources.submitted = result == VK_SUCCESS;
    if (result == VK_SUCCESS) completion = resources.completion;
    return result;
}

double PresentProcessor::average_ms() const noexcept {
    if (!impl_->neural_mode) return impl_->gpu_ms;
#if defined(NEUROSHADE_LAYER_TEMPORAL)
    return impl_->profiler ? impl_->profiler->avg_ms("present.neural") : 0.0;
#else
    return 0.0;
#endif
}

bool PresentProcessor::profiler_gpu_backed() const noexcept {
    if (!impl_->neural_mode) return !impl_->image_resources.empty() && impl_->image_resources.front().queries;
#if defined(NEUROSHADE_LAYER_TEMPORAL)
    return impl_->profiler && impl_->profiler->gpu_backed();
#else
    return false;
#endif
}

double PresentProcessor::inference_ms() const noexcept {return impl_->host?impl_->host->average_ms():0.0;}
std::string PresentProcessor::runtime_status() const {
    if(!impl_->host)return {};
    return impl_->host_failed ? "FALHA: "+impl_->host->error() : impl_->host->description();
}

}  // namespace neuroshade::layer
