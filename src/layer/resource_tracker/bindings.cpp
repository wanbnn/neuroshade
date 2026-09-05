#include "layer/resource_tracker/bindings.hpp"

#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace neuroshade::resources {

void BindingStore::bind(std::string semantic, const ImageInfo& image, VkExtent2D output_extent) {
    bindings_[std::move(semantic)] = {image.id, fingerprint(image, output_extent)};
}

std::optional<Binding> BindingStore::get(std::string_view semantic) const {
    const auto found = bindings_.find(std::string(semantic));
    if (found == bindings_.end()) return std::nullopt;
    return found->second;
}

std::optional<std::pair<std::uint64_t, float>> BindingStore::rebind(
    std::string_view semantic, const std::vector<ImageInfo>& candidates, VkExtent2D output_extent) const {
    const auto saved = get(semantic);
    if (!saved || candidates.empty()) return std::nullopt;
    std::pair<std::uint64_t, float> best{};
    for (const auto& candidate : candidates) {
        const float confidence = fingerprint_confidence(saved->fingerprint, fingerprint(candidate, output_extent));
        if (confidence > best.second) best = {candidate.id, confidence};
    }
    return best;
}

bool BindingStore::save(const std::filesystem::path& path, std::string& error) const {
    std::error_code file_error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), file_error);
    const auto temporary = path.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::trunc);
    if (!stream) { error = "unable to create binding profile"; return false; }
    stream << "{\n  \"schema_version\": 1,\n  \"bindings\": [\n";
    std::size_t index = 0;
    for (const auto& [semantic, binding] : bindings_) {
        const auto& f = binding.fingerprint;
        stream << "    {\"semantic\":\"" << semantic << "\",\"runtime_id\":" << binding.runtime_id
               << ",\"format\":" << static_cast<int>(f.format) << ",\"width\":" << f.width_per_mille
               << ",\"height\":" << f.height_per_mille << ",\"usage\":" << f.usage
               << ",\"attachment\":" << unsigned(f.attachment_bucket) << ",\"creation\":" << unsigned(f.creation_bucket)
               << ",\"writes\":" << unsigned(f.write_bucket) << ",\"reads\":" << unsigned(f.read_bucket) << "}"
               << (++index == bindings_.size() ? "\n" : ",\n");
    }
    stream << "  ]\n}\n";
    stream.close();
    std::filesystem::rename(temporary, path, file_error);
    if (file_error) { std::filesystem::remove(temporary); error = file_error.message(); return false; }
    return true;
}

bool BindingStore::load(const std::filesystem::path& path, std::string& error) {
    std::ifstream stream(path);
    if (!stream) { error = "unable to open binding profile"; return false; }
    std::ostringstream contents; contents << stream.rdbuf();
    const std::string source = contents.str();
    if (source.find("\"schema_version\": 1") == std::string::npos) {
        error = "unsupported or missing binding schema_version"; return false;
    }
    const std::regex entry(
        R"re(\{"semantic":"([^"]+)","runtime_id":([0-9]+),"format":([0-9]+),"width":([0-9]+),"height":([0-9]+),"usage":([0-9]+),"attachment":([0-9]+),"creation":([0-9]+),"writes":([0-9]+),"reads":([0-9]+)\})re");
    std::unordered_map<std::string, Binding> loaded;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), entry); it != std::sregex_iterator(); ++it) {
        Fingerprint f{static_cast<VkFormat>(std::stoi((*it)[3].str())),
                      static_cast<std::uint16_t>(std::stoul((*it)[4].str())),
                      static_cast<std::uint16_t>(std::stoul((*it)[5].str())),
                      static_cast<VkImageUsageFlags>(std::stoul((*it)[6].str())),
                      static_cast<std::uint8_t>(std::stoul((*it)[7].str())),
                      static_cast<std::uint8_t>(std::stoul((*it)[8].str())),
                      static_cast<std::uint8_t>(std::stoul((*it)[9].str())),
                      static_cast<std::uint8_t>(std::stoul((*it)[10].str()))};
        loaded[(*it)[1].str()] = {std::stoull((*it)[2].str()), f};
    }
    bindings_ = std::move(loaded);
    return true;
}

}  // namespace neuroshade::resources
