// SAO Auto — the single, real overlay HWND.  1:1 with `render/overlay_host.py`.
//
// Python authoritative source: `sao_auto/python/render/overlay_host.py` (1124 lines)
//
// Direct successor to the legacy `overlay/overlay_host.h` design *and*
// the Python `render/overlay_host.py`.  There is exactly ONE HWND per
// session — every plugin/overlay draws into virtual layers owned by
// the compositor below.  Corresponds to the memory notes:
//   * "统一DWM覆盖合成器"       (unified DWM overlay compositor)
//   * "input proxy逐像素+防焦点偷" (per-pixel input proxy + focus shield)
//   * "挂机卡死=Win32句柄泄漏清单" (idle-crash Win32 handle leak checklist)
//
// ── Window topology ─────────────────────────────────────────────────
//   hRender  (self.hwnd)         — the one full-screen DComp render target.
//   hControl (self.control_hwnd) — hidden 1x1 control helper owned by owner.
//   owner    (self._owner_hwnd)  — hidden owner that suppresses taskbar entry.
//
// hRender is the only window that paints or receives overlay input.  The
// owner/control pair only supplies lifetime and display-affinity topology;
// they never form independent render hosts.
//
// ── Non-negotiable invariants (each one has caused a real bug) ─────
//   1. `SetWindowRgn` is the ONLY reliable cross-process click-through
//      mechanism.  `WS_EX_TRANSPARENT` alone is NOT sufficient (only
//      forwards WM_NCHITTEST within the same thread).  Host RGN is
//      NEVER `NULL` — `NULL` means "clear custom clip", i.e. full
//      opaque rect blocking the game.  (See §4 of the handoff.)
//
//   2. `SetWindowRgn` **must** have `argtypes`/`restype` set to pointer
//      width; ctypes default `c_int` overflows on HRGN values with high
//      bit set → orphan HRGN leak on every dirty layer.  In C++, use
//      the correct `HRGN` (void*) — this is the main-root-cause GDI
//      leak the Python handoff §5 identifies as the single biggest fix.
//
//   3. Ex-style bit flags (`WS_EX_TOPMOST` etc.) — never issue raw
//      `SetWindowPos(HWND_TOPMOST)` here; route ALL topmost mutations
//      through `sao_ui_z_order_manager` (`z_order.h`).  Three known
//      TOPMOST-leak commit hashes: a90adbf / c208715 / 6e1eb20 — do
//      not reintroduce their patterns.
//
//   4. `WM_MOUSEACTIVATE → MA_NOACTIVATE` at WndProc level.  Not just
//      `WS_EX_NOACTIVATE` on the exstyle — Tk / some frameworks still
//      promote to foreground on click; the WndProc-level answer is
//      what actually holds.
//
//   5. `WS_EX_LAYERED` is *not* used — DWM glass via
//      `DwmExtendFrameIntoClientArea(-1,-1,-1,-1)` + `DwmEnableBlur-
//      BehindWindow` provides per-pixel alpha without layered semantics
//      that some GPU drivers render as opaque black.
//
//   6. `WGL_NV_DX_interop2` (wglDXLockObjectsNV) has NO timeout param.
//      Every acquire must be preceded by `ID3D11Device::GetDevice-
//      RemovedReason()` device_removed() check.  See §9 of handoff.
//
//   7. Capture affinity (`WDA_EXCLUDEFROMCAPTURE`) is applied
//      symmetrically to hRender + hControl with rollback on partial
//      failure.  An unavailable platform API returns an explicit
//      unsupported status; it never reports a successful toggle.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_overlay_host_s* sao_ui_overlay_host_handle_t;

// Creation config — deliberately dense so callers can't accidentally
// depend on defaults that differ between debug / release / test builds.
// Mirrors OverlayHost.__init__ in Python (width/height/dc_mutations).
struct SaoOverlayHostConfig {
    // 0 → GetSystemMetrics(SM_CX/CYSCREEN) at create time.  Non-zero
    // → explicit override (test rigs / secondary monitor).
    int32_t width;
    int32_t height;
    int32_t origin_x;
    int32_t origin_y;

    // Optional window-title override for debug tooling.  Not shown to
    // user (host has WS_EX_TOOLWINDOW + owner suppression).  UTF-16.
    // NULL → empty title, matching Python default.
    const wchar_t* title_utf16;

    // Enable diagnostic stderr output (mirrors env SAO_OVERLAY_DIAGNOSTICS=1).
    // Prints masked HWND, capture-mode partial states, exstyle failures.
    bool diagnostics;

    // Enable tagWND snapshot dump (mirrors env SAO_DUMP_TW=1).  Writes
    // to %TEMP%/sao_tw_dump.jsonl.  Very verbose; only for hard bugs
    // where GetWindowLong reports one exstyle and physical memory
    // reports another (anti-cheat exstyle-hook diagnosis).
    bool tagwnd_dump;

    // Optional DC-mutation-coordinator handle (from
    // `render/dc_mutation_coordinator.py` equivalent). Real on-screen
    // geometry is always published synchronously through USER32 and DWM;
    // when a rect-scrub provider is installed, the coordinator then scrubs
    // the physical rcWindow to a decoy rectangle as the ordered second step.
    // NULL → USER32/DWM geometry only.
    void* dc_mutation_coordinator;
};

// Creates the one real overlay host: registers a random class name,
// creates the invisible owner + 1x1 hControl decoy + full-screen
// hRender, and sets up DWM glass for the D3D11/DirectComposition
// presentation path.  Does NOT call ShowWindow; does NOT pump
// a message loop — the launcher owns both so the UI thread is
// deterministic.
//
// Ordering matters: owner first, hControl next, hRender LAST.  The
// WndProc is shared by all three; must guard "hwnd == self.hwnd"
// before running interactive branches (WM_NCHITTEST / mouse events)
// — the early creation messages of owner/hControl otherwise land in
// the interactive path when self.hwnd==0 (short-circuit-false).  This
// is documented at length in the Python WndProc (line 507-560).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_create(
    const SaoOverlayHostConfig* config, sao_ui_overlay_host_handle_t* out_handle);

// Two-phase destroy (mirrors Python `destroy()`).  Phase 1 drains the
// dc-mutation coordinator; failure → return false (retry expected).
// Phase 2 releases WGL context (under a wgl-serialize lock), releases
// HDC, destroys hRender / hControl / owner in order.  Never partial-
// clears handles — a partial teardown must be retryable.
// Returns false if teardown failed and caller should retry.
SAO_UI_API bool SAO_UI_CALL sao_ui_overlay_host_destroy(sao_ui_overlay_host_handle_t handle);

// Grab the raw HWND for interop with D3D swapchains, DirectComposition,
// input hooks etc.  Do not destroy or reparent — the handle is owned
// by this module.  Returns hRender (α semantics).
SAO_UI_API void* SAO_UI_CALL sao_ui_overlay_host_hwnd(sao_ui_overlay_host_handle_t handle);

// Grab the hidden 1x1 hControl HWND.  Capture affinity is always set on
// BOTH hRender and hControl, or on neither.
SAO_UI_API void* SAO_UI_CALL sao_ui_overlay_host_control_hwnd(sao_ui_overlay_host_handle_t handle);

// Validate that the caller is the Win32 owner thread that created the host.
// Host-bound compositors use this before attaching thread-affine D3D/DComp and
// input state.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_overlay_host_require_owner_thread(sao_ui_overlay_host_handle_t handle);

// Borrowed DC-mutation coordinator configured at host creation, or NULL.
// The host owns only its HWND registration; it does not own the coordinator.
SAO_UI_API void* SAO_UI_CALL
sao_ui_overlay_host_dc_mutation_coordinator(sao_ui_overlay_host_handle_t handle);

// The production host is D3D/DComp based.  WGL interop is deliberately
// unsupported in this build: these accessors return NULL and the three WGL
// operations below return SAO_STATUS_ERR_NOT_IMPLEMENTED.  This keeps the
// legacy ABI honest rather than manufacturing a context that cannot present
// through the DComp render host.
SAO_UI_API void* SAO_UI_CALL sao_ui_overlay_host_hglrc(sao_ui_overlay_host_handle_t handle);
SAO_UI_API void* SAO_UI_CALL sao_ui_overlay_host_hdc(sao_ui_overlay_host_handle_t handle);

// Set the host's on-screen geometry (screen coords, DPI-aware). The ordered
// pair is: synchronous USER32 SetWindowPos(real), cached geometry update,
// DwmFlush, then one asynchronous physical rcWindow scrub when configured.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_bounds(
    sao_ui_overlay_host_handle_t handle, int32_t x, int32_t y, int32_t width, int32_t height);

// Returns the last successfully requested on-screen geometry.  This remains
// stable when an external tagWND rcWindow scrub makes WM_SIZE/WM_MOVE report
// a decoy rectangle, so the z-order authority can re-assert the real DWM
// presentation bounds on its next tick.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_get_desired_bounds(
    sao_ui_overlay_host_handle_t handle, struct SaoOverlayHostClientRect* out_rect);

// Show/hide.  ShowWindow(hRender, SW_SHOWNOACTIVATE) — never
// SW_SHOW/SW_SHOWNORMAL (would activate).  hControl gets the same
// treatment; owner stays hidden.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_overlay_host_set_visible(sao_ui_overlay_host_handle_t handle, bool visible);

// Toggle `WS_EX_TRANSPARENT` for cross-process click passthrough.
// See §4 of the handoff — `WM_NCHITTEST → HTTRANSPARENT` only works
// within the same thread; `WS_EX_TRANSPARENT` is required for the
// bit to reach the game process.  Verifies via GetWindowLongPtr
// readback; on mismatch (anti-cheat hook / access denied), keeps
// the previous internal state and returns SAO_STATUS_ERR_ACCESS_DENIED.
// Owner-thread-affine; cross-thread calls return ACCESS_DENIED.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_overlay_host_set_input_passthrough(sao_ui_overlay_host_handle_t handle, bool passthrough);

SAO_UI_API bool SAO_UI_CALL
sao_ui_overlay_host_input_passthrough(sao_ui_overlay_host_handle_t handle);

// Interactive rectangles are expressed in hRender client coordinates.
// The host owns no widget geometry; input_router supplies the current union
// after each layout pass.  Each update applies current U previous, then stores
// current as the previous list for the next update.  The empty-list contract is
// therefore intentionally stateful:
//   * initial empty -> empty RGN + passthrough immediately;
//   * A -> empty -> empty applies A, then A once more, then empty.
// The retained post-nonempty empty cycle remains interactive: SetWindowRgn,
// WS_EX_TRANSPARENT, and WM_NCHITTEST are committed as one logical update.
// This prevents a moving/reshaped control from exposing a stale click-through
// hole between rendered frames. Updates are owner-thread-affine.
struct SaoOverlayHostInputRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_input_region(
    sao_ui_overlay_host_handle_t handle, const SaoOverlayHostInputRect* rects, size_t rect_count);

// An input update is transactional.  If SetWindowRgn succeeds but the
// passthrough style update fails, the host restores both the prior region and
// prior style before returning the failure.  A rollback OS call can itself
// fail; the same applies to a direct set_input_passthrough style rollback.
// Either condition is never reported as success and is exposed as PARTIAL
// until a later successful set_input_region call re-synchronizes both sides.
enum SaoOverlayHostInputSyncState : uint32_t {
    SAO_UI_OVERLAY_INPUT_SYNCHRONIZED = 0,
    SAO_UI_OVERLAY_INPUT_PARTIAL = 1,
};

SAO_UI_API uint32_t SAO_UI_CALL
sao_ui_overlay_host_input_sync_state(sao_ui_overlay_host_handle_t handle);

// SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) toggle.  Applies
// symmetrically to hRender + hControl with rollback on partial failure.
// If the OS does not support the requested affinity, returns
// SAO_STATUS_ERR_NOT_IMPLEMENTED and preserves the prior state.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_overlay_host_set_capture_mode(sao_ui_overlay_host_handle_t handle, bool exclude);

SAO_UI_API bool SAO_UI_CALL
sao_ui_overlay_host_capture_excluded(sao_ui_overlay_host_handle_t handle);

// Legacy WGL operations.  See the WGL accessor contract above: all three
// return SAO_STATUS_ERR_NOT_IMPLEMENTED in the D3D/DComp production path.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_overlay_host_make_current(sao_ui_overlay_host_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_overlay_host_release_current(sao_ui_overlay_host_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_overlay_host_swap_buffers(sao_ui_overlay_host_handle_t handle);

// Drain pending Win32 messages for the overlay HWND (up to 128 per call).
// PeekMessageW/TranslateMessage/DispatchMessageW loop — do NOT use
// GetMessage (blocks the render thread indefinitely if the game
// stops feeding input).
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_overlay_host_pump_messages(sao_ui_overlay_host_handle_t handle);

// Wait for a pending message OR timeout, whichever comes first.
// Wraps MsgWaitForMultipleObjectsEx(QS_ALLINPUT).  Without this, a
// naive sleep inside the render thread blocks WM_NCHITTEST for the
// full sleep window — the cursor becomes unresponsive during long
// idle frames.  Called from compositor sleep path.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_overlay_host_msg_wait(sao_ui_overlay_host_handle_t handle, uint32_t timeout_ms);

// Hit test callback — invoked from WndProc's WM_NCHITTEST handler on
// the UI thread.  Return true for "interactive" (HTCLIENT), false for
// "click-through" (HTTRANSPARENT).  Screen coordinates.
typedef bool(SAO_UI_CALL* sao_ui_hit_test_fn_t)(int32_t screen_x, int32_t screen_y,
                                                void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_hit_test(
    sao_ui_overlay_host_handle_t handle, sao_ui_hit_test_fn_t fn, void* user_data);

// Mouse event callback — invoked from WndProc for WM_MOUSEMOVE /
// WM_LBUTTON* / WM_RBUTTON* / WM_MOUSEWHEEL / WM_MOUSELEAVE.
// msg_type is the raw Win32 message id.  Screen coordinates.
// button: 0=left, 1=right, 2=middle (GLFW convention).  wheel_delta
// is signed multiple of WHEEL_DELTA (120) for WM_MOUSEWHEEL, else 0.
// TrackMouseEvent(TME_LEAVE) is auto-armed on WM_MOUSEMOVE so a
// WM_MOUSELEAVE arrives when the cursor exits without a WM_MOUSEMOVE
// (which happens for cross-screen moves).
typedef void(SAO_UI_CALL* sao_ui_mouse_fn_t)(uint32_t msg_type, int32_t screen_x, int32_t screen_y,
                                             int32_t button, int32_t wheel_delta, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_mouse(
    sao_ui_overlay_host_handle_t handle, sao_ui_mouse_fn_t fn, void* user_data);

// ── Window-message completion handlers ───────────────────────────
//
// Additional WM_ handlers beyond the base host lifecycle: WM_SIZE, WM_MOVE,
// WM_ACTIVATE, WM_WINDOWPOSCHANGING, WM_DPICHANGED, WM_DISPLAYCHANGE.
//
// Each handler MAY invoke a caller-supplied callback so consumers
// (compositor, animation clock, DPI-scale layer) can react to the
// underlying Win32 event.  If no callback is registered, the handler
// still performs its default action:
//   * WM_SIZE:            update cached client_w/h; no default return
//   * WM_MOVE:            update cached origin_x/y; no default return
//   * WM_ACTIVATE:        request z-order refresh via z_order manager
//                         (deactivation) or verify topmost intact
//                         (activation)
//   * WM_WINDOWPOSCHANGING: record the request only; z_order.cpp is the
//                         sole authority allowed to mutate HWND z-order
//   * WM_DPICHANGED:      re-scale client rect to the suggested new
//                         RECT; consumer callback decides whether to
//                         redraw layers at the new DPI
//   * WM_DISPLAYCHANGE:   invalidate cached monitor geometry so
//                         compositor recreates its swap chain

// Observability accessors — read the CURRENT cached values that the
// WM_ handlers maintain.  Zero on failure / no handler fired yet.
struct SaoOverlayHostClientRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct SaoOverlayHostState {
    SaoOverlayHostClientRect geometry;
    uint32_t dpi;
    bool visible;
    bool input_passthrough;
    bool capture_excluded;
    uint8_t _pad[1];
};

SAO_UI_API bool SAO_UI_CALL sao_ui_overlay_host_visible(sao_ui_overlay_host_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_overlay_host_get_state(sao_ui_overlay_host_handle_t handle, SaoOverlayHostState* out_state);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_get_client_rect(
    sao_ui_overlay_host_handle_t handle, SaoOverlayHostClientRect* out_rect);

SAO_UI_API uint32_t SAO_UI_CALL
sao_ui_overlay_host_current_dpi(sao_ui_overlay_host_handle_t handle);

// Counters exposed for tests: how many times each WM_ handler fired
// during the host's lifetime.  Monotonic; wraps at UINT32_MAX.
struct SaoOverlayHostWMCounters {
    uint32_t size_events;
    uint32_t move_events;
    uint32_t activate_events;
    uint32_t windowposchanging_events;
    uint32_t windowposchanging_topmost_repairs;
    uint32_t dpichanged_events;
    uint32_t displaychange_events;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_get_wm_counters(
    sao_ui_overlay_host_handle_t handle, SaoOverlayHostWMCounters* out_counters);

// Optional consumer callbacks.  All fire from the WndProc thread —
// callback impl must not block or the host's message pump stalls.
typedef void(SAO_UI_CALL* sao_ui_size_fn_t)(int32_t new_width, int32_t new_height, void* user_data);

typedef void(SAO_UI_CALL* sao_ui_move_fn_t)(int32_t new_x, int32_t new_y, void* user_data);

typedef void(SAO_UI_CALL* sao_ui_activate_fn_t)(bool activated, void* user_data);

typedef void(SAO_UI_CALL* sao_ui_dpi_changed_fn_t)(uint32_t new_dpi, int32_t suggested_x,
                                                   int32_t suggested_y, int32_t suggested_w,
                                                   int32_t suggested_h, void* user_data);

typedef void(SAO_UI_CALL* sao_ui_display_change_fn_t)(uint32_t bit_depth, uint32_t width,
                                                      uint32_t height, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_size_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_size_fn_t fn, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_move_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_move_fn_t fn, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_activate_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_activate_fn_t fn, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_dpi_changed_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_dpi_changed_fn_t fn, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_display_change_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_display_change_fn_t fn, void* user_data);

#ifdef __cplusplus
} // extern "C"
#endif
