// SAO Auto — overlay compositor streaming-mode flow control.
//
// Python authoritative source: `sao_auto/python/render/overlay_compositor.py`
//   * `_streaming_lock`              L1463  (threading.Lock)
//   * `_streaming_threads_lock`      L1464  (threading.RLock)
//   * `_streaming_threads`           L1465  (set[Thread])
//   * `_streaming_accepting`         L1466  (bool)
//   * `_streaming_generation`        L1467  (int)
//   * `set_streaming_mode(exclude)`  L1938
//   * streaming worker publish       L1972-1986
//   * `_stop_streaming_workers`      L1988-2003
//   * teardown 2-phase               L2664-2675
//   * startup init                   L1885-1907
//   * SettingsManager gate           L2752-2760
//
// ── Why this exists ─────────────────────────────────────────
//   `capture_sync.cpp` owns the render / capture
//   contention path.  This module is the ORTHOGONAL lifecycle path for
//   the streaming-mode toggle — the state that manages the background
//   worker set that flips the host into "excluded from screen capture".
//
//   The two subsystems share no state and must not be merged.  Merging
//   would make it impossible to keep the reentrant capture depth counter
//   separate from the generation-based streaming worker gate.
//
// ── Concurrency contract ────────────────────────────────────
//   * `mode_lock` (mutex)              — serializes streaming mode side-
//     effects (capture-mode toggle + vfence start/stop).  Analogue of
//     Python `_streaming_lock` (threading.Lock).
//   * `threads_lock` (recursive_mutex) — guards the worker set and the
//     accepting flag; recursive so the caller can nest reads inside a
//     transaction.  Analogue of Python `_streaming_threads_lock` (RLock).
//   * `accepting` (atomic bool)        — false blocks new worker
//     registration.  Analogue of Python `_streaming_accepting`.
//   * `generation` (atomic uint64_t)   — monotonically increases on each
//     `set_mode` and each `stop_workers`.  Workers born with an old
//     generation early-exit.  Analogue of Python `_streaming_generation`.
//
//   `stop_workers` sets accepting=false BEFORE draining alive workers so
//   the drain is monotone: no new worker can enter the set after the
//   snapshot.  This mirrors the Python order at L1990-1993.
//
// ── Timeout semantics ───────────────────────────────────────
//   `std::thread::join()` has no timeout, so we track worker liveness
//   with a per-worker atomic status flag + a condition variable that
//   the worker signals on exit.  `stop_workers` waits on the CV up to
//   the deadline, checks alive workers on wake, and prunes exited ones.
//
//   Callers must not hold `threads_lock` across the CV wait — the CV is
//   configured against `threads_lock`, so waiting requires releasing it.

#pragma once

#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque worker handle.  Returned by `register_worker`; the caller uses
// it to signal completion via `worker_exit`.  Value 0 is the sentinel
// for "worker was rejected" (not accepting, or generation mismatch).
typedef uint64_t sao_streaming_worker_handle_t;

// Snapshot of the current state for diagnostics.  All fields are
// samples captured under `threads_lock`.
typedef struct sao_streaming_flow_snapshot_s {
    bool     accepting;
    bool     mode_exclude;
    uint64_t generation;
    uint32_t worker_count;
    uint32_t alive_worker_count;
} sao_streaming_flow_snapshot_t;

// ── Startup / shutdown ─────────────────────────────────────
//
// `startup` clears the state to "fresh generation, accepting=true".
// Mirrors Python `_stop_locked`'s L1858-1860 block that sets
// `_streaming_accepting = True` at start().  Also waits for any
// previous generation's workers to finish, matching the invariant
// that a new compositor generation must not observe stale workers.
SAO_UI_API sao_status_t SAO_UI_CALL sao_streaming_flow_startup(
    double previous_generation_drain_timeout_sec);

// Teardown = 2-phase shutdown matching Python
// `_teardown_render_thread_once` L2664-2675:
//   phase 1: stop_workers(timeout)
//   phase 2: acquire mode_lock(timeout) so no in-flight mode toggle
//            can race the rest of the render-thread teardown.
// Returns OK when both phases succeeded, ERR_TIMEOUT if either failed.
SAO_UI_API sao_status_t SAO_UI_CALL sao_streaming_flow_teardown(
    double stop_workers_timeout_sec,
    double mode_lock_acquire_timeout_sec);

// ── Mode ────────────────────────────────────────────────────
//
// Set streaming (screen-capture-excluded) mode.  Bumps the generation
// so late-starting workers born on the old generation early-exit.
// Returns the NEW generation, or 0 if the state is not accepting.
SAO_UI_API uint64_t SAO_UI_CALL sao_streaming_flow_set_mode(bool exclude);

// Read the current desired mode.  Never blocks.
SAO_UI_API bool SAO_UI_CALL sao_streaming_flow_get_mode(void);

// Read the current generation.  Never blocks.  Monotonically increasing.
SAO_UI_API uint64_t SAO_UI_CALL sao_streaming_flow_generation(void);

// ── Worker lifecycle ───────────────────────────────────────
//
// Called by the worker thread body BEFORE it starts doing streaming-
// mode side-effects.  Behaviour:
//   * If `accepting` is false, returns 0 (worker must early-exit).
//   * If `expected_generation != current generation`, returns 0.
//   * Otherwise, allocates a handle, adds it to the worker set, and
//     returns the handle.
// Analogue of Python L1976-1986 (the atomic add+start pair).
SAO_UI_API sao_streaming_worker_handle_t SAO_UI_CALL
sao_streaming_flow_register_worker(uint64_t expected_generation);

// Called by the worker thread body IN A finally-equivalent BLOCK when
// it is exiting (either normally or via error).  Marks the worker
// dead, removes it from the alive set, and wakes any thread blocked
// in `stop_workers`.  Analogue of Python L1971-1973.
SAO_UI_API sao_status_t SAO_UI_CALL sao_streaming_flow_worker_exit(
    sao_streaming_worker_handle_t handle);

// Non-blocking check inside a worker body: "is my generation still the
// current generation".  A worker that observes a change should stop
// making streaming-mode side-effects and exit ASAP.
SAO_UI_API bool SAO_UI_CALL sao_streaming_flow_worker_generation_still_valid(
    sao_streaming_worker_handle_t handle);

// ── Drain ──────────────────────────────────────────────────
//
// Sets `accepting=false`, bumps generation, then waits up to
// `timeout_sec` for every alive worker to signal exit.  Returns OK if
// the set is empty on return, ERR_TIMEOUT otherwise (leaves the state
// in accepting=false).  Analogue of Python L1988-2003.
SAO_UI_API sao_status_t SAO_UI_CALL sao_streaming_flow_stop_workers(
    double timeout_sec);

// Prune already-terminated workers without waiting.  Idempotent.
// Analogue of Python L1999-2002.  Returns the number of workers
// remaining in the alive set.
SAO_UI_API uint32_t SAO_UI_CALL sao_streaming_flow_prune_dead_workers(void);

// ── Mode-lock RAII helpers ─────────────────────────────────
//
// A mode transaction: acquires `mode_lock` (returns ERR_TIMEOUT on
// failure), then the caller performs the host.set_capture_mode +
// vfence start/stop pair, then releases.  Serializes with the
// teardown-phase-2 acquire so no in-flight toggle can outrace teardown.
SAO_UI_API sao_status_t SAO_UI_CALL sao_streaming_flow_mode_lock_acquire(
    double timeout_sec);

// Must be called by the thread that acquired the mode lock.  Returns
// ERR_ACCESS_DENIED for a non-owner and ERR_NOT_INITIALIZED after the
// lock has already been released; neither error unlocks the mutex.
SAO_UI_API sao_status_t SAO_UI_CALL sao_streaming_flow_mode_lock_release(
    void);

// ── Persisted setting ──────────────────────────────────────
//
// Read the SettingsManager "streaming_mode" boolean at startup.  Python
// path L2752-2760 wraps this in a paid-user gate; the C++ side only
// exposes the raw read.  `out_value` is untouched on ERR_NOT_FOUND.
//
// The reader is decoupled: the caller injects a JSON-shaped settings
// blob (usually loaded from disk) via `sao_streaming_flow_set_persisted_
// settings_source`.  Deep integration with the C++ SettingsManager is
// deliberately left to the caller so this module stays test-friendly.
typedef bool (SAO_UI_CALL *sao_streaming_flow_settings_reader_fn)(
    const char* key, bool* out_value, void* user);

// Install a reader callback used by `load_persisted_setting`.  Pass
// `reader = nullptr` to clear.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_streaming_flow_set_persisted_settings_reader(
    sao_streaming_flow_settings_reader_fn reader, void* user);

// Query the persisted setting via the installed reader.  If no reader
// is installed OR the reader returns false, the default is used and
// the returned status is OK with *out_value = default_value.
SAO_UI_API sao_status_t SAO_UI_CALL sao_streaming_flow_load_persisted_setting(
    bool default_value, bool* out_value);

// ── Diagnostics ────────────────────────────────────────────
//
// Snapshot for logging / tests.  Never blocks for long; briefly locks
// `threads_lock` to sample the worker set.
SAO_UI_API sao_status_t SAO_UI_CALL sao_streaming_flow_snapshot(
    sao_streaming_flow_snapshot_t* out);

// Test-only: reset state to freshly-constructed values.  Fails if there
// are any alive workers.  Not part of the shipping ABI contract.
SAO_UI_API sao_status_t SAO_UI_CALL sao_streaming_flow_reset_for_tests(
    void);

#ifdef __cplusplus
}  // extern "C"
#endif

// ── C++ RAII helpers ────────────────────────────────────────
#ifdef __cplusplus
namespace sao::ui {

// Scoped worker registration: register in the ctor, signal exit in the
// dtor.  `valid()` returns false if the worker was rejected (accepting
// was false, or generation mismatch), matching the Python `_bg` body
// which returns early in both cases.
class StreamingWorkerScope {
public:
    explicit StreamingWorkerScope(uint64_t expected_generation)
        : handle_(sao_streaming_flow_register_worker(expected_generation)) {}
    ~StreamingWorkerScope() {
        if (handle_ != 0) {
            sao_streaming_flow_worker_exit(handle_);
        }
    }
    StreamingWorkerScope(const StreamingWorkerScope&) = delete;
    StreamingWorkerScope& operator=(const StreamingWorkerScope&) = delete;

    bool valid() const { return handle_ != 0; }
    sao_streaming_worker_handle_t handle() const { return handle_; }
    bool generation_still_valid() const {
        return valid() && sao_streaming_flow_worker_generation_still_valid(
            handle_);
    }

private:
    sao_streaming_worker_handle_t handle_;
};

// Scoped mode-lock guard: fails-open on timeout (`ok()` false) so the
// caller can bail rather than skip the release.
class StreamingModeLockGuard {
public:
    explicit StreamingModeLockGuard(double timeout_sec)
        : ok_(sao_streaming_flow_mode_lock_acquire(timeout_sec)
              == SAO_STATUS_OK) {}
    ~StreamingModeLockGuard() {
        if (ok_) {
            sao_streaming_flow_mode_lock_release();
        }
    }
    StreamingModeLockGuard(const StreamingModeLockGuard&) = delete;
    StreamingModeLockGuard& operator=(const StreamingModeLockGuard&) = delete;

    bool ok() const { return ok_; }

private:
    bool ok_;
};

}  // namespace sao::ui
#endif  // __cplusplus
