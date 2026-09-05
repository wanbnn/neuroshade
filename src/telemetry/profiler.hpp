#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace neuroshade::telemetry {

// Stable per-pass timing record backed by HIP events when available, falling
// back to a CPU steady_clock measurement when HIP event creation is not
// possible. Moving averages suppress flicker (SPEC §23, UI-004).
class PassProfiler {
public:
    // Empirically chosen EMA smoothing factor. Higher values react faster to
    // outlier frames; lower values read smoother. 0.10 keeps a ~10-frame tail
    // visible while still absorbing single-frame spikes.
    static constexpr double smoothing = 0.10;

    PassProfiler();
    ~PassProfiler();

    PassProfiler(const PassProfiler&) = delete;
    PassProfiler& operator=(const PassProfiler&) = delete;

    void begin_pass(std::string_view pass_id) noexcept;
    void end_pass(std::string_view pass_id) noexcept;

    [[nodiscard]] double last_ms(std::string_view pass_id) const noexcept;
    [[nodiscard]] double avg_ms(std::string_view pass_id) const noexcept;
    [[nodiscard]] double max_ms(std::string_view pass_id) const noexcept;
    [[nodiscard]] std::size_t samples(std::string_view pass_id) const noexcept;

    // True when the underlying timer is implemented with HIP events. False
    // indicates the CPU fallback is in use; either way, callers can rely on
    // last_ms / avg_ms returning a millisecond value.
    [[nodiscard]] bool gpu_backed() const noexcept;

    // Wipes every recorded pass timing. Tests and the explicit user reset
    // hook use this to start a clean window.
    void reset() noexcept;

    // Convenience: formats "<ms>" with three decimals. Used by tests.
    [[nodiscard]] std::string format_ms(double ms) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neuroshade::telemetry
