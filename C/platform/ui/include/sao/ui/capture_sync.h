// SAO Auto — capture / present synchronization.
//
// Python authoritative source: `sao_auto/python/render/render_capture_sync.py`
//
// The recognition path uses `PrintWindow` which synchronously drives
// the target WndProc to render into a DC — a call that BLOCKS for 30-
// 100 ms on the game window.  If overlay ULW commits interleave, they
// wait for the capture lock, which is the "1-2 FPS under recognition
// load" symptom.
//
// This module provides a reference-counted capture section:
//   begin_capture() / end_capture() bracket a synchronous capture;
//   render worker frame commits can `wait_until_capture_idle` for a
//   bounded timeout before falling back to committing anyway.
//
// ── Concurrency contract ────────────────────────────────────
//   begin_capture() is reentrant — nested captures increment depth.
//   end_capture() decrements; only depth==0 signals idle.
//   capture_idle event is initially set; begin_capture clears it and
//   end_capture (at depth==0) sets it.  Multiple waiters are all
//   released simultaneously.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Begin a capture section.  Reentrant — nested calls increment depth.
// PrintWindow (or equivalent capture path) wraps its blocking call
// in begin_capture()/end_capture().
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_capture_sync_begin(void);

// End a capture section.  At depth==0, signals idle to any waiter
// blocked in `wait_until_idle`.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_capture_sync_end(void);

// Non-blocking query: is any capture in progress?
SAO_UI_API bool SAO_UI_CALL sao_ui_capture_sync_is_active(void);

// Block until capture idle OR timeout.  timeout_sec=0 → non-blocking
// poll (equivalent to `is_active()`).  Returns SAO_STATUS_OK on idle,
// SAO_STATUS_ERR_TIMEOUT on timeout.  Never blocks indefinitely.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_capture_sync_wait_until_idle(
    double timeout_sec);

// Current depth (for diagnostics).
SAO_UI_API uint32_t SAO_UI_CALL sao_ui_capture_sync_depth(void);

#ifdef __cplusplus
}  // extern "C"
#endif

// Scoped RAII helper (C++ only) — thin wrapper around begin/end.
// Must live OUTSIDE the extern "C" block — namespaces + classes with
// mangled ctor/dtor cannot appear inside extern "C".
#ifdef __cplusplus
namespace sao::ui {
class CaptureSection {
public:
    CaptureSection() { sao_ui_capture_sync_begin(); }
    ~CaptureSection() { sao_ui_capture_sync_end(); }
    CaptureSection(const CaptureSection&) = delete;
    CaptureSection& operator=(const CaptureSection&) = delete;
};
}  // namespace sao::ui
#endif
