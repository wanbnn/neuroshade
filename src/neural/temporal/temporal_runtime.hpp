#pragma once

#include "neural/model/package.hpp"
#include "neural/runtime/spatial_runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace neuroshade::neural {

// Optional view into a temporal input. `non_history_inputs` are the
// non-tensor-table tensor bindings of the model. `history_inputs` are the
// tensor bindings declared with semantics `History.Color.*` /
// `History.Depth.*` / `History.Motion.*` in the model manifest and are
// expected to be fed from a persistent ring buffer.
struct TemporalInputLayout {
    std::vector<TensorBinding> non_history_inputs;
    std::vector<TensorBinding> history_inputs;
    std::string output_tensor;
    std::string primary_non_history_tensor;  // used for the copy fallback path
};

// Temporal neural runtime (SPEC §14, §20). Owns the compiled MIGraphX program,
// the static non-history tensor allocations, the persistent temporal ring
// buffer, and the per-frame execution state. Equivalent in spirit to
// `SpatialRuntime` but adds the ring buffer, history validity flag, and
// invalidation hooks.
class TemporalRuntime {
public:
    TemporalRuntime(ModelPackage package, std::string gfx_architecture,
                    std::size_t ring_slots = 4);
    ~TemporalRuntime();

    TemporalRuntime(const TemporalRuntime&) = delete;
    TemporalRuntime& operator=(const TemporalRuntime&) = delete;

    // Reports the layout the runtime discovered. The test uses this to wire
    // inputs into the correct HIP buffers.
    [[nodiscard]] const TemporalInputLayout& layout() const noexcept;

    // HIP device pointer for the named non-history input (current frame).
    [[nodiscard]] void* current_input(std::string_view tensor) const;

    // Returns the byte count for the named input, history or non-history.
    [[nodiscard]] std::size_t tensor_bytes(std::string_view tensor) const;

    // Runtime read-only state.
    [[nodiscard]] void* output_data() const noexcept;
    [[nodiscard]] std::size_t output_bytes() const noexcept;
    [[nodiscard]] const std::vector<TensorAllocation>& tensor_plan() const noexcept;
    [[nodiscard]] const std::filesystem::path& cache_path() const noexcept;
    [[nodiscard]] std::size_t ring_slot_count() const noexcept;
    [[nodiscard]] bool cache_hit() const noexcept;
    [[nodiscard]] bool warmed_up() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool history_valid() const noexcept;
    [[nodiscard]] std::uint64_t frame_index() const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept;

    void upload_current(std::string_view tensor, std::span<const std::byte> bytes);
    void download_output(std::span<std::byte> bytes) const;

    enum class ExecutionResult { temporal, spatial_fallback, copy_fallback };

    // Advances the ring by one frame, evaluates the model, and records the
    // output into the next history slot. `step()` is the only entry point
    // that mutates execution state.
    // `motion_available=false` applies the package's `missing_motion`
    // policy. A spatial fallback clears temporal history, zeros every
    // required Motion.* tensor, and evaluates the spatial branch without
    // disabling the pass.
    ExecutionResult step(bool motion_available = true);

    // Explicit user-driven history reset (TMP-002). Clears validity and the
    // next-to-read slot. The next `step()` will use the model's declared
    // first-frame behavior.
    void reset_history();

    // Programmatic invalidation (TMP-002). Behaves identically to
    // `reset_history()` from the data flow perspective; the `reason` is
    // accepted as a hook for future logging/diagnostics but is intentionally
    // not surfaced through `last_error()` because invalidation itself is
    // not an error condition. Resize, swapchain recreation, model change,
    // topology change, large frame discontinuity, and missing motion all
    // funnel through here.
    void invalidate_history(std::string reason);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neuroshade::neural
