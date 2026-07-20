// SAO Auto — overlay compositor streaming-mode flow control.
//
// 1:1 with `sao_auto/python/render/overlay_compositor.py`:
//   * `_streaming_lock` / `_streaming_threads_lock` /
//     `_streaming_threads` / `_streaming_accepting` /
//     `_streaming_generation` init                       L1463-1467
//   * `set_streaming_mode(exclude)`                      L1938
//   * streaming worker `_bg` publish                     L1972-1986
//   * `_stop_streaming_workers(timeout=2.0)`             L1988-2003
//   * teardown 2-phase                                   L2664-2675
//   * startup (`_stop_locked` clears + `run` re-arms)    L1858-1907
//   * SettingsManager streaming_mode gate                L2752-2760
//
// Key porting decisions:
//   * `std::thread::join()` has no timeout; the alive-worker set is a
//     map<handle, WorkerState> with a per-worker `exited` flag and a
//     shared CV that workers signal on exit.  `stop_workers` waits on
//     the CV up to the deadline, checking that all handles are
//     `exited==true` on wake.
//   * Handles are opaque uint64_t counters (never 0 — 0 is the sentinel
//     for "rejected").  This matches the Python thread-identity model
//     while staying test-friendly (tests can synthesize handles without
//     spawning real threads).
//   * `accepting` is std::atomic<bool> AND guarded by `threads_lock`
//     during compound register/stop transactions.  Reads outside a
//     transaction go through the atomic load path (matches Python which
//     reads the flag inside `_streaming_threads_lock`).
//   * `mode_lock` uses timed_mutex so `try_lock_for` gives the 250 ms
//     teardown bound at L2668 in Python.
//   * Persisted setting reading is decoupled via callback so unit tests
//     stay Windows-free; the shipping caller wires this to the real
//     SettingsManager JSON reader.
//
// The anti-screencap module owns the streaming lifecycle.  The small local
// worker map below remains only for ABI-compatible UI worker tokens; it never
// decides admission, generation, teardown, or worker counts.

#include "sao/ui/streaming_flow.h"

#include "sao_security/anti_screencap/streaming_mode.h"
#include "sao_security/anti_screencap/streaming_teardown.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace {

using clock_type = std::chrono::steady_clock;

// Per-worker state held in the alive-worker set.  `exited` is written
// under `threads_lock` and paired with a `notify_all` on the shared
// exit CV so `stop_workers` can wait bounded.
struct WorkerState {
    // Generation the worker was born under.  Used for the "generation
    // still valid" query; must match the current generation for the
    // worker's side-effects to be honoured.
    uint64_t born_generation = 0;
    // Set true by `worker_exit`.  Once true the entry is a candidate
    // for pruning; kept in the map so `stop_workers` can distinguish
    // "worker did signal exit" from "worker was never registered".
    bool     exited = false;
};

struct SaoStreamingFlowState {
    // ── Mode side-effect lock (Python `_streaming_lock`) ──────
    // timed_mutex so `try_lock_for` gives us the 250 ms teardown bound
    // matching Python L2668.
    std::timed_mutex mode_lock;
    // Owner thread id of `mode_lock` when held via the ABI acquire.
    // `optional<thread::id>` keeps the unlocked state distinct without
    // relying on a hash value that may legitimately be zero.
    std::mutex                     mode_lock_owner_mutex;
    std::optional<std::thread::id> mode_lock_owner;

    // ── Worker set lock (Python `_streaming_threads_lock`) ────
    // recursive_mutex so a caller can nest reads inside a transaction,
    // matching Python RLock semantics.
    mutable std::recursive_mutex threads_lock;
    // Compatibility token map.  Security owns all actual streaming workers.
    // Key = handle (assigned monotonically from `next_handle`).
    std::unordered_map<sao_streaming_worker_handle_t, WorkerState> workers;
    sao_streaming_worker_handle_t next_handle = 1u;  // 0 = rejected

    // UI diagnostic of the last requested mode.  Capture application and
    // effective mode policy remain in security.
    std::atomic<bool>       mode_exclude{false};

    // Persisted-settings reader (installed via
    // `set_persisted_settings_reader`).  Guarded by `settings_mu`.
    std::mutex                                  settings_mu;
    sao_streaming_flow_settings_reader_fn       settings_reader = nullptr;
    void*                                       settings_user   = nullptr;
};

SaoStreamingFlowState& state() {
    static SaoStreamingFlowState s;
    return s;
}

// Convert a floating-point seconds count to a chrono duration used by
// the timed primitives.  Clamps below at zero (matches Python
// `max(0.0, ...)` at L1989/L1998) and above at 24h (matches the
// capture_sync bound to keep the chrono ticks from overflowing).
std::chrono::microseconds seconds_to_micros(double sec) {
    if (!(sec > 0.0)) {
        return std::chrono::microseconds{0};
    }
    constexpr double kMaxWaitSec = 24.0 * 60.0 * 60.0;
    if (sec > kMaxWaitSec) {
        sec = kMaxWaitSec;
    }
    return std::chrono::microseconds(
        static_cast<int64_t>(sec * 1'000'000.0));
}

// Drop entries where `exited==true` from the map.  Caller must hold
// `threads_lock`.  Returns count remaining after pruning.
uint32_t prune_exited_locked(SaoStreamingFlowState& s) {
    for (auto it = s.workers.begin(); it != s.workers.end();) {
        if (it->second.exited) {
            it = s.workers.erase(it);
        } else {
            ++it;
        }
    }
    return static_cast<uint32_t>(s.workers.size());
}

}  // namespace

// ── Startup / shutdown ─────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_streaming_flow_startup(
    double previous_generation_drain_timeout_sec) {
    auto& s = state();
    (void)previous_generation_drain_timeout_sec;
    {
        std::lock_guard<std::recursive_mutex> lock(s.threads_lock);
        (void)prune_exited_locked(s);
    }
    const int32_t rc = sao_anti_screencap_streaming_mode_init(nullptr, nullptr);
    return rc == SAO_STATUS_OK ? SAO_STATUS_OK : rc;
}

extern "C" sao_status_t SAO_UI_CALL sao_streaming_flow_teardown(
    double stop_workers_timeout_sec,
    double mode_lock_acquire_timeout_sec) {
    auto& s = state();

    // Phase 1: stop workers (Python L2664-2666).  ERR_TIMEOUT on
    // failure means alive workers remain — the caller's teardown
    // must fail and retry.
    const uint32_t worker_timeout_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            seconds_to_micros(stop_workers_timeout_sec)).count());
    const uint32_t mode_timeout_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            seconds_to_micros(mode_lock_acquire_timeout_sec)).count());
    const int32_t security_rc = sao_anti_screencap_streaming_teardown(
        worker_timeout_ms, mode_timeout_ms, nullptr, nullptr);
    if (security_rc != SAO_STATUS_OK) return security_rc;

    const sao_status_t stop_st = sao_streaming_flow_stop_workers(
        stop_workers_timeout_sec);
    if (stop_st != SAO_STATUS_OK) return stop_st;

    // Phase 2: acquire mode_lock so no in-flight mode toggle can race
    // the rest of teardown (Python L2667-2669).  Release immediately —
    // ownership was only needed to fence the toggle from continuing.
    const auto us = seconds_to_micros(mode_lock_acquire_timeout_sec);
    const bool acquired = s.mode_lock.try_lock_for(us);
    if (!acquired) {
        return SAO_STATUS_ERR_TIMEOUT;
    }
    s.mode_lock.unlock();
    return SAO_STATUS_OK;
}

// ── Mode ────────────────────────────────────────────────────

extern "C" uint64_t SAO_UI_CALL sao_streaming_flow_set_mode(bool exclude) {
    auto& s = state();
    SaoAntiScreencapStreamingSnapshot before{};
    if (sao_anti_screencap_streaming_snapshot(&before) != SAO_STATUS_OK ||
        !before.accepting) {
        return 0u;
    }
    if (sao_anti_screencap_set_streaming_mode(exclude) != SAO_STATUS_OK) {
        return 0u;
    }
    SaoAntiScreencapStreamingSnapshot security_snapshot{};
    if (sao_anti_screencap_streaming_snapshot(&security_snapshot) != SAO_STATUS_OK) {
        return 0u;
    }
    const uint64_t new_gen = security_snapshot.generation;
    s.mode_exclude.store(exclude, std::memory_order_release);
    return new_gen;
}

extern "C" bool SAO_UI_CALL sao_streaming_flow_get_mode(void) {
    return state().mode_exclude.load(std::memory_order_acquire);
}

extern "C" uint64_t SAO_UI_CALL sao_streaming_flow_generation(void) {
    SaoAntiScreencapStreamingSnapshot snapshot{};
    return sao_anti_screencap_streaming_snapshot(&snapshot) == SAO_STATUS_OK
        ? snapshot.generation
        : 0u;
}

// ── Worker lifecycle ───────────────────────────────────────

extern "C" sao_streaming_worker_handle_t SAO_UI_CALL
sao_streaming_flow_register_worker(uint64_t expected_generation) {
    auto& s = state();
    SaoAntiScreencapStreamingSnapshot security_snapshot{};
    if (sao_anti_screencap_streaming_snapshot(&security_snapshot) != SAO_STATUS_OK ||
        !security_snapshot.accepting ||
        expected_generation != security_snapshot.generation) {
        return 0u;
    }
    std::lock_guard<std::recursive_mutex> lock(s.threads_lock);
    const sao_streaming_worker_handle_t handle = s.next_handle++;
    // Wraparound guard — 2^64 handles is unreachable in practice, but
    // 0 is the "rejected" sentinel so we must skip it if we ever wrap.
    if (s.next_handle == 0u) {
        s.next_handle = 1u;
    }
    WorkerState ws;
    ws.born_generation = expected_generation;
    ws.exited = false;
    s.workers.emplace(handle, ws);
    return handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_streaming_flow_worker_exit(
    sao_streaming_worker_handle_t handle) {
    if (handle == 0u) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto& s = state();
    {
        // Python L1971-1973 (finally block):
        //   with self._streaming_threads_lock:
        //       self._streaming_threads.discard(current)
        std::lock_guard<std::recursive_mutex> lock(s.threads_lock);
        auto it = s.workers.find(handle);
        if (it == s.workers.end()) {
            // Idempotent — a worker exiting twice is not an error.
            // Matches `set.discard()` semantics in Python which does
            // nothing if the element is absent.
            return SAO_STATUS_OK;
        }
        if (it->second.exited) {
            return SAO_STATUS_OK;
        }
        it->second.exited = true;
    }
    return SAO_STATUS_OK;
}

extern "C" bool SAO_UI_CALL sao_streaming_flow_worker_generation_still_valid(
    sao_streaming_worker_handle_t handle) {
    if (handle == 0u) {
        return false;
    }
    auto& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.threads_lock);
    auto it = s.workers.find(handle);
    if (it == s.workers.end()) {
        return false;
    }
    if (it->second.exited) {
        return false;
    }
    SaoAntiScreencapStreamingSnapshot security_snapshot{};
    return sao_anti_screencap_streaming_snapshot(&security_snapshot) == SAO_STATUS_OK &&
        security_snapshot.accepting &&
        it->second.born_generation == security_snapshot.generation;
}

// ── Drain ──────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_streaming_flow_stop_workers(
    double timeout_sec) {
    auto& s = state();
    const uint32_t timeout_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            seconds_to_micros(timeout_sec)).count());
    const int32_t security_rc = sao_anti_screencap_stop_streaming_workers(timeout_ms);
    if (security_rc != SAO_STATUS_OK) return security_rc;
    {
        std::lock_guard<std::recursive_mutex> lock(s.threads_lock);
        s.workers.clear();
    }
    return SAO_STATUS_OK;
}

extern "C" uint32_t SAO_UI_CALL sao_streaming_flow_prune_dead_workers(void) {
    auto& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.threads_lock);
    return prune_exited_locked(s);
}

// ── Mode-lock RAII helpers ─────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_streaming_flow_mode_lock_acquire(
    double timeout_sec) {
    auto& s = state();
    // For a zero/negative timeout, Python's `Lock.acquire(timeout=0.0)`
    // is a non-blocking try; mirror that with `try_lock`.
    if (!(timeout_sec > 0.0)) {
        if (s.mode_lock.try_lock()) {
            std::lock_guard<std::mutex> owner_lock(s.mode_lock_owner_mutex);
            s.mode_lock_owner = std::this_thread::get_id();
            return SAO_STATUS_OK;
        }
        return SAO_STATUS_ERR_TIMEOUT;
    }
    const auto us = seconds_to_micros(timeout_sec);
    if (!s.mode_lock.try_lock_for(us)) {
        return SAO_STATUS_ERR_TIMEOUT;
    }
    std::lock_guard<std::mutex> owner_lock(s.mode_lock_owner_mutex);
    s.mode_lock_owner = std::this_thread::get_id();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_streaming_flow_mode_lock_release(void) {
    auto& s = state();
    std::lock_guard<std::mutex> owner_lock(s.mode_lock_owner_mutex);
    if (!s.mode_lock_owner.has_value()) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    if (*s.mode_lock_owner != std::this_thread::get_id()) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    s.mode_lock.unlock();
    s.mode_lock_owner.reset();
    return SAO_STATUS_OK;
}

// ── Persisted setting ──────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL
sao_streaming_flow_set_persisted_settings_reader(
    sao_streaming_flow_settings_reader_fn reader, void* user) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.settings_mu);
    s.settings_reader = reader;
    s.settings_user   = user;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_streaming_flow_load_persisted_setting(
    bool default_value, bool* out_value) {
    if (out_value == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto& s = state();
    sao_streaming_flow_settings_reader_fn reader = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.settings_mu);
        reader = s.settings_reader;
        user   = s.settings_user;
    }
    // Python L2752-2760:
    //   from config import SettingsManager
    //   if SettingsManager().get('streaming_mode', False):
    //       ...
    // The default surfaces via the `False` default arg to `get`; we
    // expose it as `default_value` for the callback to honour.
    *out_value = default_value;
    if (reader == nullptr) {
        return SAO_STATUS_OK;
    }
    bool read_value = default_value;
    const bool found = reader("streaming_mode", &read_value, user);
    if (found) {
        *out_value = read_value;
    }
    return SAO_STATUS_OK;
}

// ── Diagnostics ────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_streaming_flow_snapshot(
    sao_streaming_flow_snapshot_t* out) {
    if (out == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    SaoAntiScreencapStreamingSnapshot security_snapshot{};
    if (sao_anti_screencap_streaming_snapshot(&security_snapshot) != SAO_STATUS_OK) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    auto& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.threads_lock);
    out->accepting = security_snapshot.accepting;
    out->mode_exclude = s.mode_exclude.load(std::memory_order_acquire);
    out->generation = security_snapshot.generation;
    out->worker_count = security_snapshot.registered_threads;
    out->alive_worker_count = security_snapshot.registered_threads;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_streaming_flow_reset_for_tests(void) {
    auto& s = state();
    // Guard against reset while workers are alive — otherwise a stray
    // `worker_exit` after reset would corrupt the map.
    {
        std::lock_guard<std::recursive_mutex> lock(s.threads_lock);
        s.workers.clear();
        s.next_handle = 1u;
        s.mode_exclude.store(false, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(s.settings_mu);
        s.settings_reader = nullptr;
        s.settings_user = nullptr;
    }
    return SAO_STATUS_OK;
}
