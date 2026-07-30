// SAO Auto — animated badge widget.
//
// Small badge widget (count / status dot) that can pulse.  Reuses the
// shared animator (animator.h) so the pulse costs no per-badge thread.
// Generic: menu items / panel headers / list rows instantiate it to
// show live counts (notifications, queue depth, connection state).
//
// Conventions match widget_data.h (SaoUiStatusBadgeSpec parity):
//   * widget_create / apply_props / paint / hit_test
//   * transactional JSON props (transactional restore via
//     widget_typed_internal apply/restore helpers)
//   * theme-token colour resolution with 0 → inherit
//
// Pulse drives a 1.0 → 1.18 → 1.0 scale envelope over pulse_ms with
// SAO_UI_CURVE_EASE_IN_OUT, applied to the dot radius.  The badge text
// (if any) is painted centred beside the dot.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/theme.h"

#ifdef __cplusplus
extern "C" {
#endif

// Extended widget kind — continues past SAO_UI_WIDGET_SPARKLINE.
enum sao_ui_widget_badge_kind_e : int32_t {
    SAO_UI_WIDGET_ANIMATED_BADGE = 170,
};

// ── Spec ──────────────────────────────────────────────────────────
// count >= 0 → numeric badge (renders count text).  count < 0 → status
// dot only (no text).  pulse_ms == 0 → static (no animation).
struct SaoUiAnimatedBadgeSpec {
    int32_t     count;          // < 0 → dot only
    int32_t     dot_radius_px;  // 0 → 6
    int32_t     pulse_ms;       // 0 → 900 default
    int32_t     font_size_px;   // 0 → 12
    int32_t     pad_x_px;       // 0 → 6
    int32_t     pad_y_px;       // 0 → 3
    int32_t     radius_px;      // corner radius for text pill; 0 → 8
    uint32_t    fill_argb;      // 0 → SAO_UI_TOKEN_APP_ACCENT
    uint32_t    fg_argb;         // 0 → SAO_UI_TOKEN_WHITE
    uint32_t    border_argb;    // 0 → SAO_UI_TOKEN_APP_BORDER
    uint32_t    pulse_argb;      // 0 → fill lightened 30%
    SaoUiThemeId theme_override; // SAO_UI_THEME_COUNT = inherit
    uint8_t     _pad[4];
};

// ── Lifecycle ─────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animated_badge_create(
    void* d3d_device_ptr,
    const SaoUiAnimatedBadgeSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animated_badge_set_count(
    sao_ui_widget_handle_t handle,
    int32_t count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animated_badge_get_count(
    sao_ui_widget_handle_t handle,
    int32_t* out_count);

// Start / stop the pulse animation.  pulse_ms == 0 stops it.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animated_badge_set_pulse(
    sao_ui_widget_handle_t handle,
    int32_t pulse_ms);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animated_badge_is_pulsing(
    sao_ui_widget_handle_t handle,
    bool* out_pulsing);

// ── Transactional props ────────────────────────────────────────────
// Accepted keys (all optional):
//   { "count": <int>, "dot_radius": <int>, "pulse_ms": <int>,
//     "font_size": <int>, "pad_x": <int>, "pad_y": <int>,
//     "radius": <int>, "fill": "#RRGGBBAA", "fg": "#RRGGBBAA",
//     "border": "#RRGGBBAA", "pulse": "#RRGGBBAA", "theme": <int 0..2> }
// Invalid values leave the current state untouched.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animated_badge_apply_props(
    sao_ui_widget_handle_t handle,
    const uint8_t* props_json_utf8,
    size_t props_len);

// Advance the pulse animation.  Called by the overlay scheduler pump.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animated_badge_tick(
    sao_ui_widget_handle_t handle,
    int32_t dt_ms);

// Resolve the current pulse scale (0..1 phase → 1.0..1.18).  Test hook.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animated_badge_pulse_scale(
    sao_ui_widget_handle_t handle,
    float* out_scale);

#ifdef __cplusplus
} // extern "C"
#endif