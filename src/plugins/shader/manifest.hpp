#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace neuroshade::plugins {

struct ShaderPassManifest {
    std::string id;
    std::string backend;
    std::string shader;
    std::vector<std::string> inputs;
    std::string output;
};

struct PluginManifest {
    std::uint32_t schema{};
    std::string id;
    std::string name;
    std::string version;
    std::uint32_t api{};
    std::vector<ShaderPassManifest> passes;
};

struct ManifestResult {
    PluginManifest manifest;
    std::vector<std::string> errors;
    [[nodiscard]] bool valid() const noexcept { return errors.empty(); }
};

[[nodiscard]] ManifestResult parse_shader_manifest(std::string_view source);
[[nodiscard]] ManifestResult load_shader_manifest(const std::filesystem::path& path);

}  // namespace neuroshade::plugins
