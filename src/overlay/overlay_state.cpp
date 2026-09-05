#include "overlay/overlay_state.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace neuroshade::overlay {

void OverlayState::on_home_pressed() noexcept { visible_ = !visible_; }
bool OverlayState::visible() const noexcept { return visible_; }
void OverlayState::select_tab(Tab tab) noexcept { selected_ = tab; }
Tab OverlayState::selected_tab() const noexcept { return selected_; }
void OverlayState::update(Snapshot snapshot) { snapshot_ = std::move(snapshot); }
const Snapshot& OverlayState::snapshot() const noexcept { return snapshot_; }

bool OverlayState::set_pass_enabled(std::string_view id, bool enabled) noexcept {
    const auto found = std::ranges::find(snapshot_.passes, id, &PassRow::id);
    if (found == snapshot_.passes.end()) return false;
    found->enabled = enabled;
    return true;
}

bool OverlayState::set_pass_strength(std::string_view id, double strength) noexcept {
    const auto found = std::ranges::find(snapshot_.passes, id, &PassRow::id);
    if (found == snapshot_.passes.end() || strength < 0.0 || strength > 1.0) return false;
    found->strength = strength;
    return true;
}

bool OverlayState::select_model(std::string_view id, std::string_view model) noexcept {
    const auto found = std::ranges::find(snapshot_.passes, id, &PassRow::id);
    if (found == snapshot_.passes.end()) return false;
    const auto installed = std::ranges::find(snapshot_.models, model);
    if (installed == snapshot_.models.end()) return false;
    found->model = model;
    return true;
}

bool OverlayState::reorder_pass(std::size_t from, std::size_t to) noexcept {
    if (from >= snapshot_.passes.size() || to >= snapshot_.passes.size()) return false;
    if (from == to) return true;
    auto pass = std::move(snapshot_.passes[from]);
    snapshot_.passes.erase(snapshot_.passes.begin() + static_cast<std::ptrdiff_t>(from));
    snapshot_.passes.insert(snapshot_.passes.begin() + static_cast<std::ptrdiff_t>(to),
                            std::move(pass));
    return true;
}

void OverlayState::update_neural_timing(double average_ms) noexcept {
    snapshot_.total_average_ms = average_ms;
    for (auto& pass : snapshot_.passes) {
        if (pass.enabled && !pass.model.empty()) pass.average_ms = average_ms;
    }
}

void OverlayState::set_resources(std::vector<ResourceRow> resources) {
    snapshot_.resources = std::move(resources);
}

void OverlayState::restore_defaults() noexcept {
    for (auto& pass : snapshot_.passes) {
        pass.enabled = pass.default_enabled;
        pass.strength = pass.default_strength;
        pass.model = pass.default_model;
    }
}

std::string OverlayState::accessible_text() const {
    std::ostringstream output;
    output << "NeuroShade " << (visible_ ? "visible" : "hidden") << '\n'
           << "tab=" << to_string(selected_) << " mode=" << to_string(snapshot_.mode) << '\n'
           << "game=" << snapshot_.game << " gpu=" << snapshot_.gpu
           << " gfx=" << snapshot_.gfx << '\n'
           << "interop=" << snapshot_.interop_mode
           << " compatibility=" << snapshot_.compatibility_class << '\n'
           << "profile=" << snapshot_.active_profile << '\n'
           << std::fixed << std::setprecision(3)
           << "total_average_ms=" << snapshot_.total_average_ms
           << " vram_bytes=" << snapshot_.vram_bytes << '\n';
    for (const auto& pass : snapshot_.passes) {
        output << "pass=" << pass.id << " enabled=" << (pass.enabled ? "yes" : "no")
               << " average_ms=" << pass.average_ms << " strength=" << pass.strength
               << " model=" << (pass.model.empty() ? "none" : pass.model) << '\n';
    }
    for (const auto& resource : snapshot_.resources) {
        output << "resource=" << resource.semantic << " extent=" << resource.width << 'x'
               << resource.height << " format=" << resource.format
               << " score=" << resource.confidence
               << " bound=" << (resource.bound ? "yes" : "no") << '\n';
    }
    return output.str();
}

std::string_view to_string(RuntimeMode mode) noexcept {
    switch (mode) {
        case RuntimeMode::pass_through: return "PASS_THROUGH";
        case RuntimeMode::shader_only: return "SHADER_ONLY";
        case RuntimeMode::neural_spatial: return "NEURAL_SPATIAL";
        case RuntimeMode::neural_temporal: return "NEURAL_TEMPORAL";
        case RuntimeMode::temporal_super_resolution: return "TEMPORAL_SUPER_RESOLUTION";
    }
    return "PASS_THROUGH";
}

std::string_view to_string(Tab tab) noexcept {
    switch (tab) {
        case Tab::overview: return "Overview";
        case Tab::pipeline: return "Pipeline";
        case Tab::resources: return "Resources";
        case Tab::models: return "Models";
        case Tab::profiler: return "Profiler";
        case Tab::compatibility: return "Compatibility";
        case Tab::logs: return "Logs";
    }
    return "Overview";
}

}  // namespace neuroshade::overlay
