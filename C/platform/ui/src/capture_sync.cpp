// SAO Auto — reentrant render/capture synchronization.
//
// 1:1 with `sao_auto/python/render/render_capture_sync.py` (54 lines).
//
// Python authoritative behaviour:
//   * A process-wide reentrant capture counter guarded by a mutex.
//   * begin_capture() increments the depth and sets an "active" event;
//     clears an "idle" event.
//   * end_capture() decrements the depth, and only when depth reaches 0
//     clears the active event and sets the idle event.
//   * capture_is_active() -> non-blocking snapshot of active event.
//   * wait_until_capture_idle(timeout_s) -> block until idle event set
//     (or timeout).  timeout_s=0 (or fractional non-positive) -> a
//     non-blocking check that returns true only if already idle.
//
// The C++ port keeps the same semantics via std::mutex + std::condition
// _variable — the "event" is expressed as "depth == 0" plus a broadcast
// on end.  All operations are process-wide (no owner handle) so a single
// static instance is used, matching the Python module-level state.

#include "sao/ui/capture_sync.h"
#include "sao/ui/compositor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_compose_snapshot_(
    sao_ui_compositor_handle_t compositor, uint8_t* out_bgra_pixels, size_t capacity,
    uint32_t* out_width, uint32_t* out_height, size_t* out_bytes);

namespace {

// Process-wide singleton: matches the Python module-level state where the
// three primitives (_lock, _capture_active, _capture_idle) are global.
struct CaptureSyncState {
    std::mutex              mu;
    std::condition_variable idle_cv;
    // depth == 0 <=> idle.  Never dips below zero (matches the Python
    // clamp on end_capture()).
    uint32_t                depth = 0;
    // Diagnostic snapshot separate from the mutex-guarded depth so
    // sao_ui_capture_sync_is_active() stays lock-free.  Fenced via
    // release/acquire around mutations of `depth`.
    std::atomic<bool>       active{false};
};

CaptureSyncState& state() {
    static CaptureSyncState s;
    return s;
}

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_capture_sync_begin(void) {
    auto& s = state();
    std::lock_guard<std::mutex> guard(s.mu);
    // Reentrant: nested captures increment depth without waking
    // anything.  Only the transition from 0 -> 1 flips the diagnostic
    // active flag.
    s.depth += 1u;
    if (s.depth == 1u) {
        s.active.store(true, std::memory_order_release);
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_capture_sync_end(void) {
    auto& s = state();
    bool became_idle = false;
    {
        std::lock_guard<std::mutex> guard(s.mu);
        if (s.depth > 0u) {
            s.depth -= 1u;
        }
        if (s.depth == 0u) {
            s.active.store(false, std::memory_order_release);
            became_idle = true;
        }
    }
    if (became_idle) {
        // Multiple waiters may be blocked on wait_until_idle — wake them
        // all so any pending render tick can proceed simultaneously.
        s.idle_cv.notify_all();
    }
    return SAO_STATUS_OK;
}

extern "C" bool SAO_UI_CALL sao_ui_capture_sync_is_active(void) {
    return state().active.load(std::memory_order_acquire);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_capture_sync_wait_until_idle(
    double timeout_sec) {
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mu);
    if (s.depth == 0u) {
        return SAO_STATUS_OK;
    }
    // Non-positive timeout -> non-blocking poll semantics matching the
    // Python `_capture_idle.wait(0.0)` branch which returns immediately.
    if (!(timeout_sec > 0.0)) {
        return SAO_STATUS_ERR_TIMEOUT;
    }
    // Clamp to a sane upper bound so a bogus caller value can't overflow
    // the chrono representation.  24 hours is well beyond any realistic
    // capture window.
    constexpr double kMaxWaitSec = 24.0 * 60.0 * 60.0;
    double bounded = timeout_sec;
    if (bounded > kMaxWaitSec) {
        bounded = kMaxWaitSec;
    }
    const auto us = std::chrono::microseconds(
        static_cast<int64_t>(bounded * 1'000'000.0));
    const bool ok = s.idle_cv.wait_for(
        lock, us, [&s] { return s.depth == 0u; });
    return ok ? SAO_STATUS_OK : SAO_STATUS_ERR_TIMEOUT;
}

extern "C" uint32_t SAO_UI_CALL sao_ui_capture_sync_depth(void) {
    auto& s = state();
    std::lock_guard<std::mutex> guard(s.mu);
    return s.depth;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_snapshot_bgra(
    sao_ui_compositor_handle_t compositor, uint8_t* out_bgra_pixels, size_t capacity,
    uint32_t* out_width, uint32_t* out_height, size_t* out_bytes) {
    // Frame-source invariant: capture_sync owns the capture lease,
    // compositor owns composition, and dcomp_bridge is never involved in a
    // capture. Present and capture therefore consume one compositor result.
    const sao_status_t begin_status = sao_ui_capture_sync_begin();
    if (begin_status != SAO_STATUS_OK)
        return begin_status;
    const sao_status_t compose_status = sao_ui_compositor_compose_snapshot_(
        compositor, out_bgra_pixels, capacity, out_width, out_height, out_bytes);
    const sao_status_t end_status = sao_ui_capture_sync_end();
    return compose_status != SAO_STATUS_OK ? compose_status : end_status;
}
