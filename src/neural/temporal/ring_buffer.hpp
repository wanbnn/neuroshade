#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neuroshade::neural {

struct TensorAllocation;

// Persistent GPU-resident temporal ring buffer (SPEC §14, TMP-001).
//
// Owns `slot_count` HIP allocations for every named tensor. Each slot is a
// device-side buffer of fixed byte size and is reused across frames. There
// are no per-frame allocations in steady state.
class TemporalRingBuffer {
public:
    TemporalRingBuffer(std::vector<TensorAllocation> tensors, std::size_t slots);
    ~TemporalRingBuffer();

    TemporalRingBuffer(const TemporalRingBuffer&) = delete;
    TemporalRingBuffer& operator=(const TemporalRingBuffer&) = delete;

    // Returns the HIP device pointer for the slot `(tensor, index)`. Throws
    // if either identifier is unknown.
    [[nodiscard]] void* slot(std::string_view tensor, std::size_t index) const;

    [[nodiscard]] std::size_t slot_count() const noexcept;
    [[nodiscard]] std::size_t tensor_bytes(std::string_view tensor) const;
    [[nodiscard]] std::vector<std::string> tensor_names() const;

    // Copies `bytes` from `src_hip_ptr` (device memory) into the named slot.
    // Bytes defaults to the slot capacity. Throws on HIP error.
    void record_into(std::string_view tensor, std::size_t index, const void* src_hip_ptr,
                     std::size_t bytes = 0);

    // Fills every slot for every named tensor with zeros (no allocations).
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neuroshade::neural
