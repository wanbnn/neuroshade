#pragma once

#include "layer/resource_tracker/analyzer.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace neuroshade::resources {

struct Binding {
    std::uint64_t runtime_id{};
    Fingerprint fingerprint{};
};

class BindingStore {
public:
    void bind(std::string semantic, const ImageInfo& image, VkExtent2D output_extent);
    [[nodiscard]] std::optional<Binding> get(std::string_view semantic) const;
    [[nodiscard]] std::optional<std::pair<std::uint64_t, float>> rebind(
        std::string_view semantic, const std::vector<ImageInfo>& candidates, VkExtent2D output_extent) const;
    [[nodiscard]] bool save(const std::filesystem::path& path, std::string& error) const;
    [[nodiscard]] bool load(const std::filesystem::path& path, std::string& error);

private:
    std::unordered_map<std::string, Binding> bindings_;
};

}  // namespace neuroshade::resources
