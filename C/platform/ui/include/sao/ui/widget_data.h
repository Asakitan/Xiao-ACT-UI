// SAO Auto — data-display widgets (progress / gauge / badge / tooltip /
// more-indicator).  Table + tree live in the sibling widget_table.h
// (split for header size).
//
// Python source alignment:
//   * progress bar   → SAOHPBar (sao_theme/hp_bar.py), boss HP bar, STA
//                       bar in sao_gui_hp.py; buff uptime bars
//   * gauge          → the circular clock+timer in sao_gui_hp identity plate
//   * status badge   → sao_panel_components.status_badge (fill / border /
//                       fg / bg overrides + kind)
//   * tooltip        → sao_panel_components.attach_tooltip
//   * more indicator → sao_panel_components.more_indicator

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── Progress bar — segmented / gradient / animated (HP bar family) ──
//
// The SAO HP bar is the reference visual: value + max + color ramp
// (green→yellow→red at 50%/25% thresholds) + optional segments (Boss HP
// with cover / trailing decay).  Callers who just want a flat progress
// pass segment_count=1, no color ramp.

enum sao_ui_progress_style_e : int32_t {
    SAO_UI_PROGRESS_FLAT     = 0,           // single colour
    SAO_UI_PROGRESS_HP_RAMP  = 1,           // green→yellow→red per hp_bar.py
    SAO_UI_PROGRESS_HP_TRAIL = 2,           // + lagging trail (~280ms) like BossHP
    SAO_UI_PROGRESS_SEGMENTS = 3,           // segmented (buff uptime by pulses)
};

struct SaoUiProgressSegment {
    float    fraction_start;                // 0..1 fraction of full bar
    float    fraction_end;
    uint32_t fill_argb;
    uint32_t border_argb;
};

struct SaoUiProgressBarSpec {
    float    value;                         // 0..max
    float    max_value;
    int32_t  style;                         // sao_ui_progress_style_e
    // Colour scheme — used unless style == SEGMENTS.
    uint32_t bg_argb;
    uint32_t border_argb;
    uint32_t fill_argb;
    uint32_t fill_low_argb;                 // < 25% (only used by HP_RAMP)
    uint32_t fill_mid_argb;                 // < 50%
    uint32_t fill_high_argb;                // >= 50%
    // Segment overrides — used when style == SEGMENTS.
    const SaoUiProgressSegment* segments;
    size_t                      segment_count;
    // Animation: interpolate to new value over duration (ms).
    int32_t  animate_duration_ms;
    // BossHP-style trailing decay lag when style == HP_TRAIL.
    int32_t  trail_lag_ms;
    // Corner radius (0 → square edges).
    int32_t  radius_px;
    // Skewed leading edge (SAO look).  0 → straight edge.
    int32_t  leading_skew_px;
    uint8_t  _pad[4];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_progress_bar_create(
    void* d3d_device_ptr,
    const SaoUiProgressBarSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_progress_bar_set_value(
    sao_ui_widget_handle_t handle,
    float value);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_progress_bar_set_max(
    sao_ui_widget_handle_t handle,
    float max_value);

// ─── Gauge — circular progress / dial (identity plate boss timer) ────
struct SaoUiGaugeSpec {
    float    value;
    float    max_value;
    float    start_angle_deg;               // 0 = 3 o'clock, +CW
    float    sweep_angle_deg;               // 360 → full ring
    int32_t  radius_px;
    int32_t  thickness_px;
    uint32_t track_argb;
    uint32_t fill_argb;
    uint32_t center_argb;                   // gauge face background
    bool     show_value_label;
    bool     tick_marks;                    // small dashes every 30°
    uint8_t  _pad[6];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gauge_create(
    void* d3d_device_ptr,
    const SaoUiGaugeSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gauge_set_value(
    sao_ui_widget_handle_t handle, float value);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gauge_get_ratio(
    sao_ui_widget_handle_t handle, float* out_ratio);

// ─── Status badge — sao_panel_components.status_badge parity ─────────
struct SaoUiStatusBadgeSpec {
    const char* text_utf8;
    int32_t     kind;                       // sao_ui_button_kind_e
    // Explicit overrides — 0 = fall back to theme via `kind`.
    uint32_t    bg_argb;
    uint32_t    fill_argb;
    uint32_t    border_argb;
    uint32_t    fg_argb;
    int32_t     font_size_px;
    int32_t     pad_x_px;                   // default 9
    int32_t     pad_y_px;                   // default 3
    int32_t     radius_px;                  // default min(9, h/2)
    uint8_t     _pad[4];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_status_badge_create(
    void* d3d_device_ptr,
    const SaoUiStatusBadgeSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_status_badge_set_text(
    sao_ui_widget_handle_t handle,
    const char* text_utf8);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_status_badge_get_text(
    sao_ui_widget_handle_t handle,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_written);

// ─── Tooltip — attached to any widget, hover-delayed bubble ──────────
struct SaoUiTooltipSpec {
    const char* text_utf8;
    int32_t     delay_ms;                   // default 450
    int32_t     max_width_px;               // wrap length; default 320
    uint32_t    bg_argb;
    uint32_t    fg_argb;
    uint32_t    border_argb;
    int32_t     font_size_px;
    int32_t     pad_x_px;
    int32_t     pad_y_px;
    bool        follow_cursor;              // move with mouse vs anchor to widget
    uint8_t     _pad[3];
};

// Attach a tooltip to an existing widget.  Returns a handle to the
// tooltip so it can be dismissed / updated independently.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tooltip_attach(
    sao_ui_widget_handle_t target,
    const SaoUiTooltipSpec* spec,
    sao_ui_widget_handle_t* out_tooltip);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tooltip_set_text(
    sao_ui_widget_handle_t tooltip,
    const char* text_utf8);

// Hover state driver.  Call with hovering=true when the pointer enters
// the attached target (cursor_x/cursor_y in host coords), and false on
// exit.  The bubble appears after `delay_ms` and, when follow_cursor is
// set, re-anchors to the latest cursor position.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tooltip_notify_hover(
    sao_ui_widget_handle_t target,
    bool hovering,
    int32_t cursor_x,
    int32_t cursor_y);

// ─── More indicator — '… 还有 N 条' truncation hint ──────────────────
struct SaoUiMoreIndicatorSpec {
    int32_t     hidden_count;               // shown as N in the label
    const char* noun_utf8;                  // "条" default
    uint32_t    fg_argb;
    uint32_t    bg_argb;
    int32_t     font_size_px;
    int32_t     pad_x_px;
    int32_t     pad_y_px;
    uint8_t     _pad[4];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_more_indicator_create(
    void* d3d_device_ptr,
    const SaoUiMoreIndicatorSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_more_indicator_set_count(
    sao_ui_widget_handle_t handle,
    int32_t hidden_count);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_more_indicator_get_count(
    sao_ui_widget_handle_t handle,
    int32_t* out_hidden_count);
    
struct SaoUiMetricSpec {
    const char* label_utf8;
    const char* value_utf8;
    const char* unit_utf8;
    uint32_t    label_argb;
    uint32_t    value_argb;
    uint32_t    unit_argb;
    int32_t     value_font_size_px;
    bool        emphasize;
    uint8_t     _pad[3];
};
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_metric_create(
    void* d3d_device_ptr,
    const SaoUiMetricSpec* spec,
    sao_ui_widget_handle_t* out_handle);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_metric_set_value(
    sao_ui_widget_handle_t handle,
    const char* value_utf8);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_metric_get_value(
    sao_ui_widget_handle_t handle,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_written);
    
struct SaoUiEmptyStateSpec {
    const char* title_utf8;
    const char* detail_utf8;
    const char* action_utf8;
    uint32_t    title_argb;
    uint32_t    detail_argb;
    int32_t     icon_slot;
    bool        action_enabled;
    uint8_t     _pad[3];
};
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_empty_state_create(
    void* d3d_device_ptr,
    const SaoUiEmptyStateSpec* spec,
    sao_ui_widget_handle_t* out_handle);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_empty_state_set_detail(
    sao_ui_widget_handle_t handle,
    const char* detail_utf8);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_empty_state_get_detail(
    sao_ui_widget_handle_t handle,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_written);

#ifdef __cplusplus
}  // extern "C"
#endif
