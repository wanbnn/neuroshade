#include "overlay/overlay_state.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    using namespace neuroshade::overlay;
    OverlayState overlay;
    overlay.update({.game = "test-game",
                    .gpu = "RX 9060 XT",
                    .gfx = "gfx1200",
                    .rocm = "7.2",
                    .vulkan_driver = "RADV",
                    .interop_mode = "ZERO-COPY BUFFER",
                    .compatibility_class = "A",
                    .active_profile = "temporal_sr_2x",
                    .mode = RuntimeMode::temporal_super_resolution,
                    .total_average_ms = 1.25,
                    .vram_bytes = 64 * 1024 * 1024,
                    .passes = {{"capture.color", true, 0.1},
                               {"neural.temporal_sr", true, 0.9, 0.8,
                                "org.neuroshade.bundled.temporal_sr_2x", true, 0.8,
                                "org.neuroshade.bundled.temporal_sr_2x"},
                               {"composite.present", true, 0.25}},
                    .resources = {{"Color.LowRes", 960, 540, 37, 0.95F, true},
                                  {"Motion.Screen", 1920, 1080, 97, 1.0F, true}},
                    .models = {"org.neuroshade.bundled.temporal_sr_2x"},
                    .log_lines = {"runtime ready"}});
    overlay.on_home_pressed();
    overlay.select_tab(Tab::profiler);
    if (!overlay.visible() || overlay.selected_tab() != Tab::profiler ||
        !overlay.set_pass_enabled("neural.temporal_sr", false) ||
        !overlay.set_pass_strength("neural.temporal_sr", 0.5) ||
        overlay.set_pass_strength("neural.temporal_sr", 1.5) ||
        !overlay.select_model("neural.temporal_sr", "org.neuroshade.bundled.temporal_sr_2x") ||
        overlay.select_model("neural.temporal_sr", "not-installed") ||
        !overlay.reorder_pass(2, 1)) {
        throw std::runtime_error("overlay interaction state failed");
    }
    const std::string text = overlay.accessible_text();
    for (const char* expected : {"tab=Profiler", "mode=TEMPORAL_SUPER_RESOLUTION",
                                 "resource=Motion.Screen", "average_ms=0.900",
                                 "strength=0.500", "model=org.neuroshade.bundled.temporal_sr_2x"}) {
        if (text.find(expected) == std::string::npos) {
            std::cerr << text;
            throw std::runtime_error(std::string("overlay output missing ") + expected);
        }
    }
    overlay.restore_defaults();
    const auto restored = overlay.snapshot().passes[2];
    if (!restored.enabled || restored.strength != 0.8 ||
        restored.model != "org.neuroshade.bundled.temporal_sr_2x") {
        throw std::runtime_error("overlay defaults were not restored");
    }
    overlay.on_home_pressed();
    if (overlay.visible()) throw std::runtime_error("Home did not hide overlay");
    std::cout << "overlay_state=pass tabs=7 home_toggle=pass mode_visible=pass\n";
    return 0;
}
