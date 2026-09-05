#pragma once

#include <memory>

namespace neuroshade::overlay {

class HomeKeyInput {
public:
    HomeKeyInput();
    ~HomeKeyInput();
    HomeKeyInput(const HomeKeyInput&) = delete;
    HomeKeyInput& operator=(const HomeKeyInput&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool pressed_edge() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neuroshade::overlay
