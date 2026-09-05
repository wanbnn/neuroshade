#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neuroshade::profile {

// Persistent declaration of one resource binding a framegraph effect needs.
// Mirrors the entries persisted by ``BindingStore::save`` in the resource
// layer; the profile schema (v2) carries enough fields to identify the
// resource across game launches via its fingerprint.
struct ResourceBinding {
    std::string semantic;
    std::uint64_t runtime_id{};
    std::uint32_t format{};
    std::uint16_t width_per_mille{};
    std::uint16_t height_per_mille{};
};

struct Effect {
    std::string plugin;
    bool enabled{true};
    float strength{1.0F};
    // Optional neural model attached to a plugin entry. Empty for shader-only
    // plugins, set to a relative ``.nsmodel`` path under the install models
    // root for neural plugins.
    std::string model;
    // Optional per-effect resource bindings. Schematic-only; the project ships
    // a profile-schema-v2 roundtrip test that proves the data flows through
    // save() / parse() unchanged.
    std::vector<ResourceBinding> resource_bindings;
};

struct Profile {
    unsigned schema_version{1};
    std::string executable;
    std::vector<Effect> pipeline;
    // Output extent of the gameplay swapchain in absolute pixels. Optional;
    // the resource analyzer uses it as the denominator when recording per-
    // resource relative extent fingerprints (e.g. Color.LowRes at 50%).
    std::uint32_t output_extent_width{0};
    std::uint32_t output_extent_height{0};
};

struct LoadResult {
    Profile profile;
    std::vector<std::string> errors;
    [[nodiscard]] bool valid() const noexcept { return errors.empty(); }
};

[[nodiscard]] LoadResult parse(std::string_view json);
[[nodiscard]] LoadResult load(const std::filesystem::path& path);
[[nodiscard]] bool save(const Profile& profile, const std::filesystem::path& path, std::string& error);

}  // namespace neuroshade::profile
