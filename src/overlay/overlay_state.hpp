#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neuroshade::overlay {

enum class RuntimeMode {
    pass_through,
    shader_only,
    neural_spatial,
    neural_temporal,
    temporal_super_resolution,
};

enum class Tab { overview, pipeline, resources, models, profiler, compatibility, logs };

struct PassRow {
    std::string id;
    bool enabled{true};
    double average_ms{};
    double strength{1.0};
    std::string model;
    bool default_enabled{true};
    double default_strength{1.0};
    std::string default_model;
};

struct ResourceRow {
    std::string semantic;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t format{};
    float confidence{};
    bool bound{};
};

struct Snapshot {
    std::string game;
    std::string gpu;
    std::string gfx;
    std::string rocm;
    std::string vulkan_driver;
    std::string interop_mode;
    std::string compatibility_class;
    std::string active_profile;
    RuntimeMode mode{RuntimeMode::pass_through};
    double total_average_ms{};
    std::uint64_t vram_bytes{};
    std::vector<PassRow> passes;
    std::vector<ResourceRow> resources;
    std::vector<std::string> models;
    std::vector<std::string> log_lines;
};

class OverlayState {
public:
    // The platform input adapter calls this on a Home key press. Keeping
    // input handling outside the state object makes it testable and avoids
    // global hooks in the game process.
    void on_home_pressed() noexcept;
    [[nodiscard]] bool visible() const noexcept;
    void select_tab(Tab tab) noexcept;
    [[nodiscard]] Tab selected_tab() const noexcept;

    void update(Snapshot snapshot);
    [[nodiscard]] const Snapshot& snapshot() const noexcept;
    [[nodiscard]] bool set_pass_enabled(std::string_view id, bool enabled) noexcept;
    [[nodiscard]] bool set_pass_strength(std::string_view id, double strength) noexcept;
    [[nodiscard]] bool select_model(std::string_view id, std::string_view model) noexcept;
    [[nodiscard]] bool reorder_pass(std::size_t from, std::size_t to) noexcept;
    void update_neural_timing(double average_ms) noexcept;
    void set_resources(std::vector<ResourceRow> resources);
    void restore_defaults() noexcept;
    [[nodiscard]] std::string accessible_text() const;

private:
    bool visible_{};
    Tab selected_{Tab::overview};
    Snapshot snapshot_{};
};

[[nodiscard]] std::string_view to_string(RuntimeMode mode) noexcept;
[[nodiscard]] std::string_view to_string(Tab tab) noexcept;

}  // namespace neuroshade::overlay
