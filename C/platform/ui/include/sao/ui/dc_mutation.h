// SAO Auto — display-context mutation coordinator.
//
// Python authoritative source: `sao_auto/python/render/dc_mutation_coordinator.py`
// (399 lines)
//
// Serialized, generation-aware display-context mutations for overlay
// HWNDs.  The compositor and Tk threads submit work here instead of
// blocking on the rt_io pipe.  Calls with the same (hwnd, generation,
// operation) key are coalesced while an earlier call is in flight.
// No helper/backend import occurs until work is actually submitted.
//
// ── Why serialization matters ────────────────────────────────
//   Every kernel-side tagWND write (exstyle bits, rcWindow) goes
//   through a syscall path via `mem_probe._dc`.  Concurrent writes
//   to overlapping fields can flap the tagWND in ways that user-mode
//   API readbacks won't detect until 500 ms later — a spurious
//   racing SetWindowLongPtrW then overwrites the wanted value.
//   The coordinator serializes all writes per-HWND and coalesces
//   duplicates in the queue.
//
// ── Invalidation barrier ────────────────────────────────────
//   `invalidate(hwnd, timeout)` is the teardown API.  It:
//     1. Marks the HWND as invalidating (blocks new register calls)
//     2. Drains any inflight mutation on the HWND
//     3. Signals a completion barrier when drain succeeds
//   If drain times out, invalidate returns false — caller MUST NOT
//   clear its HWND handle (a worker with a stale HWND that leaked
//   past the timeout can then destroy some other window that
//   inherited the same value).
//
// ── Stale-failure identity check ────────────────────────────
//   Windows kernel reassigns destroyed HWND values to new windows
//   (rare but real).  Simple "failed once, block forever" logic then
//   permanently prevents a legit new window from registering.
//   The C++ port records (pid, tid) identity at failure time; on
//   re-register, compares current identity; mismatch → clear the
//   stale block.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_dc_mutation_coordinator_s* sao_ui_dc_mutation_coordinator_handle_t;
typedef struct sao_ui_dc_mutation_barrier_s* sao_ui_dc_mutation_barrier_handle_t;

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_create(
    sao_ui_dc_mutation_coordinator_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_dc_mutation_coordinator_destroy(
    sao_ui_dc_mutation_coordinator_handle_t handle);

// Register an HWND for coordinated mutations.  Returns a "token" —
// an opaque value used to detect stale registrations after an
// invalidate call took the HWND out of service.  Fails if the HWND
// is already invalidating, or if a stale-failure identity block is
// active on this HWND value.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_register(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    void* hwnd,
    void** out_token);

// Submit a display-context mutation.  operation is a short symbolic
// key ("host-exstyle", "host-rect", "proxy-exstyle") used for
// coalescing.  method_name selects the coordinator's backend method
// ("hide_exstyle", "hide_window_rect", ...) — same as
// `mem_probe._dc.hide_exstyle` in the Python code.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_submit_dc(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    void* hwnd,
    const char* operation_utf8,
    const char* method_name_utf8,
    const uint8_t* args_json_utf8,       // NULL / empty → no args
    size_t args_len);

// Invalidate an HWND — teardown barrier.  Marks the HWND as
// invalidating, drains any inflight mutation on the HWND, and
// signals a completion barrier when drain succeeds.  Returns false
// on timeout — caller MUST NOT clear its HWND handle (see above).
SAO_UI_API bool SAO_UI_CALL sao_ui_dc_mutation_coordinator_invalidate(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    void* hwnd,
    double timeout_sec);

// Explicit stale-failure clear.  Caller knows this HWND value was
// reassigned to a new window; clear the block and reset the epoch
// so register() succeeds from a clean slate.  Returns true if
// something was actually cleared.
SAO_UI_API bool SAO_UI_CALL sao_ui_dc_mutation_coordinator_clear_failed(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    void* hwnd);

// Barrier polling (used by `invalidate` callers that don't want to
// block indefinitely).  wait_ms=0 → non-blocking check.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dc_mutation_barrier_wait(
    sao_ui_dc_mutation_barrier_handle_t barrier,
    uint32_t wait_ms,
    bool* out_confirmed);

SAO_UI_API bool SAO_UI_CALL sao_ui_dc_mutation_barrier_done(
    sao_ui_dc_mutation_barrier_handle_t barrier);

// Diagnostic snapshot.
struct SaoDcMutationStats {
    uint32_t registered_hwnds;
    uint32_t inflight_operations;
    uint32_t queued_operations;
    uint32_t total_invalidations;
    uint32_t total_failed_invalidations;
    uint32_t stale_blocks_active;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_stats(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    SaoDcMutationStats* out_stats);

#ifdef __cplusplus
}  // extern "C"
#endif
