// SAO Auto — display-context mutation coordinator.
//
// Python authoritative source: `sao_auto/python/render/dc_mutation_coordinator.py`
// (399 lines)
//
// Serialized, generation-aware display-context mutations for overlay
// HWNDs. Calls with the same (hwnd, generation, operation) key are
// admitted after a fixed 16 ms frame window and coalesced to the latest
// payload before dispatch. Legacy USER32 mutations run on the HWND owner
// thread. Typed tagWND providers run directly on the coordinator worker
// after the real USER32/DWM state is published.
//
// ── Why serialization matters ────────────────────────────────
//   Real on-screen bounds are always published through USER32 and DWM first.
//   A provider may then scrub the physical tagWND rcWindow to a decoy rect.
//   The coordinator serializes these ordered mutations per HWND and keeps
//   rect scrubs in a lane separate from owner-thread USER32 bounds updates.
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

typedef struct SaoUiDcMutationRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} SaoUiDcMutationRect;

typedef sao_status_t(SAO_UI_CALL* sao_ui_dc_mutation_hide_window_rect_fn_t)(
    void* user_data, void* hwnd, const SaoUiDcMutationRect* fake_rect, uint32_t settle_ms,
    uint32_t timeout_ms);

typedef sao_status_t(SAO_UI_CALL* sao_ui_dc_mutation_hide_exstyle_fn_t)(void* user_data, void* hwnd,
                                                                        uint32_t mask,
                                                                        uint32_t timeout_ms);

typedef struct SaoUiDcMutationProvider {
    sao_ui_dc_mutation_hide_window_rect_fn_t hide_window_rect;
    void* user_data;
} SaoUiDcMutationProvider;

// Versioned provider surface. V2 adds physical tagWND ExStyle mutation while
// keeping the original provider layout and create_ex() entry point intact.
// struct_size must be sizeof(SaoUiDcMutationProviderV2); reserved must be 0.
typedef struct SaoUiDcMutationProviderV2 {
    uint32_t struct_size;
    uint32_t reserved;
    sao_ui_dc_mutation_hide_window_rect_fn_t hide_window_rect;
    sao_ui_dc_mutation_hide_exstyle_fn_t hide_exstyle;
    void* user_data;
} SaoUiDcMutationProviderV2;

// Copies the provider table by value. provider->user_data remains borrowed and
// must stay valid until destroy() returns. NULL installs no rect-scrub provider.
// A provider callback must not re-enter or destroy the same coordinator.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_create_ex(
    const SaoUiDcMutationProvider* provider, sao_ui_dc_mutation_coordinator_handle_t* out_handle);

// Copies the complete V2 provider table by value. provider->user_data remains
// borrowed and must stay valid until destroy() returns. NULL installs no
// physical providers; hide_exstyle and hide_window_rect then fail closed with
// NOT_INITIALIZED. Provider callbacks must not re-enter or destroy the same
// coordinator.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_create_ex_v2(
    const SaoUiDcMutationProviderV2* provider, sao_ui_dc_mutation_coordinator_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dc_mutation_coordinator_create(sao_ui_dc_mutation_coordinator_handle_t* out_handle);

// Stops new admission, cancels queued/coalesced work, and joins the worker.
// A single already-executing owner-thread transaction or rect-provider callback
// is allowed to finish before destroy returns. Owner-thread dispatch is bounded
// by the coordinator's OS timeout; providers must honor their supplied timeout.
// The caller must first prevent new API entries, wait for external users to
// quiesce, and call destroy exactly once; the opaque handle itself is not a
// concurrent reference-counted object.
SAO_UI_API void SAO_UI_CALL
sao_ui_dc_mutation_coordinator_destroy(sao_ui_dc_mutation_coordinator_handle_t handle);

// Register an HWND for coordinated mutations.  Returns a "token" —
// an opaque value used to detect stale registrations after an
// invalidate call took the HWND out of service.  Fails if the HWND
// is already invalidating, or if a stale-failure identity block is
// active on this HWND value.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_register(
    sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd, void** out_token);

// Submit a display-context mutation.  operation is a short symbolic
// key ("host-exstyle", "host-rect", "proxy-exstyle") used for
// coalescing. Supported production methods are:
//   host-rect + set_window_rect/set_bounds, with x/y/width/height JSON;
//   host-exstyle/proxy-exstyle + hide_exstyle, with mask JSON and a mandatory
//   V2 physical ExStyle provider.
// Unknown pairs return SAO_STATUS_ERR_NOT_IMPLEMENTED and are never queued.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dc_mutation_coordinator_submit_dc(sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd,
                                         const char* operation_utf8, const char* method_name_utf8,
                                         const uint8_t* args_json_utf8, // NULL / empty → no args
                                         size_t args_len);

// Typed physical rcWindow scrub. The lane is fixed to operation
// "host-rect-scrub" and method "hide_window_rect" and does not parse JSON.
// fake_rect must have right > left and bottom > top. A registered HWND with no
// provider returns NOT_INITIALIZED; revoked generations remain ACCESS_DENIED.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_submit_hide_window_rect(
    sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd,
    const SaoUiDcMutationRect* fake_rect, uint32_t settle_ms, uint32_t timeout_ms);

// Invalidate an HWND — teardown barrier.  Marks the HWND as
// invalidating, drains any inflight mutation on the HWND, and
// signals a completion barrier when drain succeeds.  Returns false
// on timeout — caller MUST NOT clear its HWND handle (see above).
SAO_UI_API bool SAO_UI_CALL sao_ui_dc_mutation_coordinator_invalidate(
    sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd, double timeout_sec);

// Explicit stale-failure clear. Caller knows this HWND value was reassigned to
// a new window; clear the block and reset the epoch so register() succeeds from
// a clean slate. An invalidate timeout creates a generation tombstone:
// clear_failed() returns false and register() remains blocked until every
// admitted operation from that generation has exited. Returns true only when a
// quiescent tombstone was actually cleared.
SAO_UI_API bool SAO_UI_CALL sao_ui_dc_mutation_coordinator_clear_failed(
    sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd);

// Barrier polling (used by `invalidate` callers that don't want to
// block indefinitely).  wait_ms=0 → non-blocking check.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dc_mutation_barrier_wait(
    sao_ui_dc_mutation_barrier_handle_t barrier, uint32_t wait_ms, bool* out_confirmed);

SAO_UI_API bool SAO_UI_CALL
sao_ui_dc_mutation_barrier_done(sao_ui_dc_mutation_barrier_handle_t barrier);

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
    sao_ui_dc_mutation_coordinator_handle_t handle, SaoDcMutationStats* out_stats);

#ifdef __cplusplus
} // extern "C"
#endif
