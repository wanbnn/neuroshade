#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace neuroshade::interop {

inline constexpr std::uint32_t kCanonicalAbiVersion = 1;

struct alignas(8) NSHalf4 {
    std::uint16_t r;
    std::uint16_t g;
    std::uint16_t b;
    std::uint16_t a;
};

struct alignas(4) NSHalf2 {
    std::uint16_t x;
    std::uint16_t y;
};

using NSDepth = float;

static_assert(sizeof(NSHalf4) == 8);
static_assert(alignof(NSHalf4) == 8);
static_assert(sizeof(NSHalf2) == 4);
static_assert(std::is_trivially_copyable_v<NSHalf4>);

[[nodiscard]] constexpr std::size_t color_byte_count(std::uint32_t width,
                                                      std::uint32_t height) noexcept {
    return static_cast<std::size_t>(width) * height * sizeof(NSHalf4);
}

[[nodiscard]] constexpr std::size_t depth_byte_count(std::uint32_t width,
                                                      std::uint32_t height) noexcept {
    return static_cast<std::size_t>(width) * height * sizeof(NSDepth);
}

[[nodiscard]] constexpr std::size_t motion_byte_count(std::uint32_t width,
                                                       std::uint32_t height) noexcept {
    return static_cast<std::size_t>(width) * height * sizeof(NSHalf2);
}

} // namespace neuroshade::interop
