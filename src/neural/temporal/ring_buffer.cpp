#include "neural/temporal/ring_buffer.hpp"

#include "neural/runtime/spatial_runtime.hpp"  // TensorAllocation

#include <hip/hip_runtime_api.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace neuroshade::neural {
namespace {

void hip_check(hipError_t result, const char* operation) {
    if (result != hipSuccess) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                  hipGetErrorString(result));
    }
}

} // namespace

struct TemporalRingBuffer::Impl {
    struct TensorSlots {
        std::string name;
        std::size_t bytes{};
        std::vector<void*> slots;
    };

    std::vector<TensorSlots> tensors_;

    Impl(std::vector<TensorAllocation> tensor_plan, std::size_t slot_count) {
        if (slot_count == 0) throw std::runtime_error("temporal ring requires at least 1 slot");
        tensors_.reserve(tensor_plan.size());
        for (auto& tensor : tensor_plan) {
            TensorSlots entry;
            entry.name = std::move(tensor.name);
            entry.bytes = tensor.byte_count;
            entry.slots.resize(slot_count, nullptr);
            try {
                for (std::size_t index = 0; index < slot_count; ++index) {
                    hip_check(hipMalloc(&entry.slots[index], entry.bytes),
                              "hipMalloc(temporal ring slot)");
                    hip_check(hipMemset(entry.slots[index], 0, entry.bytes),
                              "hipMemset(temporal ring slot)");
                }
            } catch (...) {
                release_all(entry);
                throw;
            }
            tensors_.push_back(std::move(entry));
        }
    }

    ~Impl() {
        for (auto& tensor : tensors_) release_all(tensor);
    }

    static void release_all(TensorSlots& tensor) noexcept {
        for (auto& slot : tensor.slots) {
            if (slot != nullptr) {
                (void)hipFree(slot);
                slot = nullptr;
            }
        }
    }

    [[nodiscard]] TensorSlots& find(std::string_view name) {
        for (auto& tensor : tensors_) {
            if (tensor.name == name) return tensor;
        }
        throw std::out_of_range("temporal ring buffer unknown tensor: " + std::string(name));
    }

    [[nodiscard]] const TensorSlots& find(std::string_view name) const {
        for (const auto& tensor : tensors_) {
            if (tensor.name == name) return tensor;
        }
        throw std::out_of_range("temporal ring buffer unknown tensor: " + std::string(name));
    }
};

TemporalRingBuffer::TemporalRingBuffer(std::vector<TensorAllocation> tensors, std::size_t slots)
    : impl_(std::make_unique<Impl>(std::move(tensors), slots)) {}

TemporalRingBuffer::~TemporalRingBuffer() = default;

void* TemporalRingBuffer::slot(std::string_view tensor, std::size_t index) const {
    const auto& entry = impl_->find(tensor);
    if (index >= entry.slots.size()) {
        throw std::out_of_range("temporal ring slot index out of range");
    }
    return entry.slots[index];
}

std::size_t TemporalRingBuffer::slot_count() const noexcept {
    if (impl_->tensors_.empty()) return 0;
    return impl_->tensors_.front().slots.size();
}

std::size_t TemporalRingBuffer::tensor_bytes(std::string_view tensor) const {
    return impl_->find(tensor).bytes;
}

std::vector<std::string> TemporalRingBuffer::tensor_names() const {
    std::vector<std::string> names;
    names.reserve(impl_->tensors_.size());
    for (const auto& tensor : impl_->tensors_) names.push_back(tensor.name);
    return names;
}

void TemporalRingBuffer::record_into(std::string_view tensor, std::size_t index,
                                     const void* src_hip_ptr, std::size_t bytes) {
    auto& entry = impl_->find(tensor);
    if (index >= entry.slots.size()) {
        throw std::out_of_range("temporal ring slot index out of range");
    }
    const auto copy_bytes = bytes == 0 ? entry.bytes : std::min(bytes, entry.bytes);
    hip_check(hipMemcpy(entry.slots[index], src_hip_ptr, copy_bytes, hipMemcpyDeviceToDevice),
              "hipMemcpy(temporal ring record)");
}

void TemporalRingBuffer::reset() noexcept {
    for (auto& tensor : impl_->tensors_) {
        for (auto& slot : tensor.slots) {
            if (slot != nullptr) {
                (void)hipMemset(slot, 0, tensor.bytes);
            }
        }
    }
}

} // namespace neuroshade::neural
