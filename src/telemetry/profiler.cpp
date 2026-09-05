#include "telemetry/profiler.hpp"

#include <hip/hip_runtime_api.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace neuroshade::telemetry {
namespace {

class HipEvent {
public:
    HipEvent() noexcept {
        last_error_ = hipEventCreateWithFlags(&event_, hipEventBlockingSync);
    }

    ~HipEvent() noexcept {
        if (event_ != nullptr) {
            (void)hipEventDestroy(event_);
        }
    }

    HipEvent(const HipEvent&) = delete;
    HipEvent& operator=(const HipEvent&) = delete;

    [[nodiscard]] hipEvent_t get() const noexcept { return event_; }
    [[nodiscard]] bool valid() const noexcept { return last_error_ == hipSuccess; }

private:
    hipEvent_t event_{};
    hipError_t last_error_{hipErrorUnknown};
};

void hip_check(hipError_t result, const char* operation) {
    if (result != hipSuccess) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                  hipGetErrorString(result));
    }
}

double milliseconds_between(const hipEvent_t start, const hipEvent_t stop) {
    float ms = 0.0F;
    hip_check(hipEventSynchronize(stop), "hipEventSynchronize");
    hip_check(hipEventElapsedTime(&ms, start, stop), "hipEventElapsedTime");
    return static_cast<double>(ms);
}

struct PassRecord {
    double last_ms{};
    double avg_ms{};
    double max_ms{};
    std::size_t samples{};
};

} // namespace

struct PassProfiler::Impl {
    mutable std::mutex mutex;
    bool gpu_available{};
    bool gpu_check_done{};
    std::unordered_map<std::string, PassRecord> records;

    // Two persistent HIP events reused across begin_pass/end_pass calls so we
    // never allocate per frame. ping_ and pong_ alternate roles to avoid
    // serializing on a single event.
    hipEvent_t ping{};
    hipEvent_t pong{};
    hipEvent_t active_start{};
    std::string active_id;

    // CPU fallback state, used only when HIP events are not available.
    // Using clock_gettime(CLOCK_MONOTONIC) avoids pulling <chrono> into the
    // translation unit, which transitively includes <format> in libstdc++
    // and trips the host compiler's `-Wtemplate-body` diagnostic.
    struct timespec cpu_start{};

    bool ensure_gpu_events() {
        if (gpu_check_done) return gpu_available;
        gpu_check_done = true;
        HipEvent test;
        if (!test.valid()) {
            gpu_available = false;
            return false;
        }
        hipError_t make_ping = hipEventCreateWithFlags(&ping, hipEventBlockingSync);
        hipError_t make_pong = hipEventCreateWithFlags(&pong, hipEventBlockingSync);
        if (make_ping != hipSuccess || make_pong != hipSuccess) {
            if (ping != nullptr) (void)hipEventDestroy(ping);
            if (pong != nullptr) (void)hipEventDestroy(pong);
            ping = nullptr;
            pong = nullptr;
            gpu_available = false;
            return false;
        }
        gpu_available = true;
        return true;
    }

    void teardown_gpu() noexcept {
        if (ping != nullptr) {
            (void)hipEventDestroy(ping);
            ping = nullptr;
        }
        if (pong != nullptr) {
            (void)hipEventDestroy(pong);
            pong = nullptr;
        }
    }

    void begin(std::string_view id) noexcept {
        if (ensure_gpu_events()) {
            (void)hipEventRecord(ping, nullptr);
            active_start = ping;
        } else {
            (void)clock_gettime(CLOCK_MONOTONIC, &cpu_start);
        }
        active_id.assign(id);
    }

    void end(std::string_view id) noexcept {
        if (active_id != id) return;  // unbalanced; silently skip.
        double elapsed_ms = 0.0;
        if (gpu_available && active_start != nullptr) {
            (void)hipEventRecord(pong, nullptr);
            elapsed_ms = milliseconds_between(active_start, pong);
        } else {
            struct timespec now{};
            (void)clock_gettime(CLOCK_MONOTONIC, &now);
            const double start_s = static_cast<double>(cpu_start.tv_sec) +
                                    static_cast<double>(cpu_start.tv_nsec) * 1.0e-9;
            const double end_s = static_cast<double>(now.tv_sec) +
                                  static_cast<double>(now.tv_nsec) * 1.0e-9;
            elapsed_ms = (end_s - start_s) * 1000.0;
        }
        active_start = nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        auto& record = records[std::string(id)];
        record.last_ms = elapsed_ms;
        if (record.samples == 0) {
            record.avg_ms = elapsed_ms;
        } else {
            record.avg_ms = (1.0 - smoothing) * record.avg_ms + smoothing * elapsed_ms;
        }
        record.max_ms = std::max(record.max_ms, elapsed_ms);
        ++record.samples;
    }

    PassRecord snapshot(std::string_view id) const noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = records.find(std::string(id));
        if (found == records.end()) return {};
        return found->second;
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        records.clear();
    }
};

PassProfiler::PassProfiler() : impl_(std::make_unique<Impl>()) {}

PassProfiler::~PassProfiler() {
    if (impl_) impl_->teardown_gpu();
}

void PassProfiler::begin_pass(std::string_view pass_id) noexcept {
    impl_->begin(pass_id);
}

void PassProfiler::end_pass(std::string_view pass_id) noexcept {
    impl_->end(pass_id);
}

double PassProfiler::last_ms(std::string_view pass_id) const noexcept {
    return impl_->snapshot(pass_id).last_ms;
}

double PassProfiler::avg_ms(std::string_view pass_id) const noexcept {
    return impl_->snapshot(pass_id).avg_ms;
}

double PassProfiler::max_ms(std::string_view pass_id) const noexcept {
    return impl_->snapshot(pass_id).max_ms;
}

std::size_t PassProfiler::samples(std::string_view pass_id) const noexcept {
    return impl_->snapshot(pass_id).samples;
}

bool PassProfiler::gpu_backed() const noexcept {
    impl_->ensure_gpu_events();
    return impl_->gpu_available;
}

void PassProfiler::reset() noexcept { impl_->reset(); }

std::string PassProfiler::format_ms(double ms) const {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", ms);
    return std::string(buffer);
}

} // namespace neuroshade::telemetry
