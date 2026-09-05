#pragma once

#include <memory>
#include <vector>
#include <cstdint>

namespace neuroshade::overlay {

struct UiInput { int x{-1}; int y{-1}; bool click{}; std::uint32_t key{}; };

class HomeKeyInput {
public:
    HomeKeyInput();
    ~HomeKeyInput();
    HomeKeyInput(const HomeKeyInput&) = delete;
    HomeKeyInput& operator=(const HomeKeyInput&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool pressed_edge() noexcept;
    void capture(bool visible) noexcept;
    [[nodiscard]] std::vector<UiInput> take_events();
    [[nodiscard]] bool mouse_captured() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neuroshade::overlay
