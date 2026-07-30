// SAO Auto — toast notification overlay.
//
// Lightweight transient toast that stacks bottom-right of the overlay,
// auto-dismisses with a fade, and tints by severity (info / success /
// warn / error).  Game-agnostic: any panel can call show() to surface a
// short-lived message without owning a widget.  The toast layer is
// driven by the shared overlay animator (60Hz pump) so fade-in / fade-
// out / auto-dismiss cost no per-toast threads.
//
// Follows popup.cpp / dialog.cpp patterns:
//   * opaque handle, lock-protected state, fire-once dismiss callback
//   * theme-token colour resolution with explicit override == 0
//   * transactional JSON props pipeline (sao_ui_toast_apply_props)
//
// Severity → theme token:
//   info    → SAO_UI_TOKEN_APP_BLUE
//   success → SAO_UI_TOKEN_APP_GREEN
//   warn    → SAO_UI_TOKEN_APP_ORANGE
//   error   → SAO_UI_TOKEN_APP_RED

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/theme.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_toast_s* sao_ui_toast_handle_t;

// ── Severity ──────────────────────────────────────────────────────
enum SaoUiToastSeverity : int32_t {
    SAO_UI_TOAST_INFO    = 0,
    SAO_UI_TOAST_SUCCESS = 1,
    SAO_UI_TOAST_WARN    = 2,
    SAO_UI_TOAST_ERROR   = 3,
};

// ── Spec ──────────────────────────────────────────────────────────
// text_utf8 is copied internally; the caller may free it immediately.
// duration_ms == 0 → use canonical default (3000).  max stacked = 4.
struct SaoUiToastSpec {
    const char* text_utf8;
    int32_t     severity;       // SaoUiToastSeverity
    int32_t     duration_ms;    // 0 → 3000 default
    int32_t     fade_ms;         // 0 → 240 default
    SaoUiThemeId theme_override; // SAO_UI_THEME_COUNT = inherit
    uint32_t    bg_argb;         // 0 → theme token by severity
    uint32_t    fg_argb;          // 0 → SAO_UI_TOKEN_APP_TEXT
    uint32_t    border_argb;     // 0 → severity token
    int32_t     font_size_px;    // 0 → 14
    int32_t     pad_x_px;        // 0 → 12
    int32_t     pad_y_px;        // 0 → 8
    int32_t     radius_px;       // 0 → 8
    uint8_t     _pad[4];
};

// Fired exactly once when the toast auto-dismisses (duration elapsed)
// or is dismissed by sao_ui_toast_dismiss.  cancelled == true when
// dismiss() killed it before the natural duration elapsed.
typedef void (SAO_UI_CALL* sao_ui_toast_dismissed_cb_t)(
    bool cancelled, void* user_data);

// ── Lifecycle ─────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_toast_create(
    sao_ui_theme_handle_t theme,
    sao_ui_toast_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_toast_destroy(
    sao_ui_toast_handle_t handle);

// Show a toast.  Stacks bottom-right; if 4 are already visible the
// oldest is dismissed (cancelled=true) to make room.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_toast_show(
    sao_ui_toast_handle_t handle,
    const SaoUiToastSpec* spec,
    sao_ui_toast_dismissed_cb_t callback,
    void* user_data);

// Manually dismiss the active toast on this handle (fires callback with
// cancelled=true).  No-op if nothing is visible.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_toast_dismiss(
    sao_ui_toast_handle_t handle);

// ── Transactional props ────────────────────────────────────────────
// Accepted keys (all optional):
//   { "text": "utf-8", "severity": <int 0..3>, "duration_ms": <int>,
//     "fade_ms": <int>, "bg": "#RRGGBBAA", "fg": "#RRGGBBAA",
//     "border": "#RRGGBBAA", "font_size": <int>, "pad_x": <int>,
//     "pad_y": <int>, "radius": <int> }
// Invalid values leave the current state untouched.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_toast_apply_props(
    sao_ui_toast_handle_t handle,
    const uint8_t* props_json_utf8,
    size_t props_len);

// ── Stack introspection (test / panel helper) ─────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_toast_visible_count(
    sao_ui_toast_handle_t handle,
    int32_t* out_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_toast_stack_top_text(
    sao_ui_toast_handle_t handle,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_written);

// Advance time for auto-dismiss + fade.  Called by the overlay scheduler
// pump; returns SAO_STATUS_OK when at least one toast is still live.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_toast_tick(
    sao_ui_toast_handle_t handle,
    int32_t dt_ms);

#ifdef __cplusplus
} // extern "C"
#endif