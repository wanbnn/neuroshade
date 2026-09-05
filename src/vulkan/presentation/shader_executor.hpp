#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace neuroshade::vulkan {

struct ShaderEffect {
    std::string name;
    float strength{1.0F};
};

class ShaderExecutor {
public:
    ShaderExecutor(VkPhysicalDevice physical_device,
                   VkDevice device,
                   VkQueue queue,
                   std::uint32_t queue_family,
                   VkBuffer input,
                   VkImage target_image,
                   std::uint32_t width,
                   std::uint32_t height,
                   const std::filesystem::path& shader_directory,
                   std::vector<ShaderEffect> effects);
    ~ShaderExecutor();
    ShaderExecutor(const ShaderExecutor&) = delete;
    ShaderExecutor& operator=(const ShaderExecutor&) = delete;
    ShaderExecutor(ShaderExecutor&&) noexcept;
    ShaderExecutor& operator=(ShaderExecutor&&) noexcept;

    [[nodiscard]] std::vector<std::uint8_t> execute();
    [[nodiscard]] bool empty() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neuroshade::vulkan
