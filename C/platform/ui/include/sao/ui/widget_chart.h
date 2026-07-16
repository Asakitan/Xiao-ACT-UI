// SAO Auto — chart widgets (time series / bar / line).
//
// Python source alignment:
//   * time series → sao_gui_graph_timeseries.py (four metrics: damage /
//                    heal / event_count / target_hp_pct; sparse point
//                    arrays; zoom / filter; peak + latest overlay)
//   * bar chart   → dps bar rows are a stripped bar chart; boss break
//                    is a bar chart with the "current segment" style
//   * line chart  → threshold plots (buff coverage, sao_gui_action_log)
//
// Charts are widget-native (Direct2D primitives) rather than plugin
// canvas ops — they know how to auto-tick, cache the static grid, and
// re-render only the changed suffix of a time series without a full
// spec resubmission.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── Shared axis descriptor ──────────────────────────────────────────
enum sao_ui_axis_scale_e : int32_t {
    SAO_UI_AXIS_LINEAR = 0,
    SAO_UI_AXIS_LOG10  = 1,
    SAO_UI_AXIS_TIME   = 2,   // ticks labelled via fmt_clock/fmt_dur
};

enum sao_ui_axis_time_fmt_e : int32_t {
    // Only used when scale == SAO_UI_AXIS_TIME.  Matches memory
    // [ACT时间显示三类分开] — never guess time semantics from magnitude,
    // callers declare intent so the widget picks the right formatter.
    SAO_UI_AXIS_TIME_CLOCK    = 0,   // fmt_clock (absolute epoch)
    SAO_UI_AXIS_TIME_DURATION = 1,   // fmt_dur   (non-negative duration)
    SAO_UI_AXIS_TIME_RELATIVE = 2,   // fmt_rel   (signed delta)
};

struct SaoUiAxisSpec {
    int32_t     scale;                      // sao_ui_axis_scale_e
    int32_t     time_fmt;                   // sao_ui_axis_time_fmt_e
    double      min_value;                  // NaN → auto from data
    double      max_value;                  // NaN → auto
    int32_t     desired_tick_count;         // 0 → auto (~5)
    bool        auto_snap;                  // round ticks to nice values
    bool        show_grid;
    bool        show_label;                 // draw axis title
    uint8_t     _pad[5];
    const char* title_utf8;                 // may be NULL
    uint32_t    grid_argb;
    uint32_t    tick_argb;
    uint32_t    label_argb;
};

// ─── Time series — one or many lanes (multi-metric graph) ────────────
struct SaoUiTimePoint {
    int64_t time_ms;                        // absolute epoch ms
    double  value;
};

struct SaoUiTimeSeriesLane {
    const char*             lane_id_utf8;   // "damage" / "heal" / ...
    const char*             label_utf8;
    const SaoUiTimePoint*   points;
    size_t                  point_count;
    uint32_t                fill_argb;      // primary colour
    uint32_t                area_fill_argb; // 0 → no area shading below line
    float                   line_width_px;
    bool                    show_points;    // dot markers on data points
    bool                    interpolate;    // smoothed vs stepped
    uint8_t                 _pad[6];
    double                  peak_hint;      // 0 → auto
};

struct SaoUiTimeSeriesSpec {
    const SaoUiTimeSeriesLane* lanes;
    size_t                     lane_count;
    SaoUiAxisSpec              x_axis;      // usually TIME + DURATION or CLOCK
    SaoUiAxisSpec              y_axis;
    int32_t                    max_visible_points;   // sliding-window cap
    int32_t                    left_pad_px;
    int32_t                    right_pad_px;
    int32_t                    top_pad_px;
    int32_t                    bottom_pad_px;
    uint32_t                   bg_argb;
    uint32_t                   frame_argb;
    bool                       show_legend;
    bool                       show_latest_value;
    bool                       show_peak;
    bool                       stack_lanes;         // stacked areas
    uint8_t                    _pad[4];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_time_series_chart_create(
    void* d3d_device_ptr,
    const SaoUiTimeSeriesSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_time_series_chart_set_lanes(
    sao_ui_widget_handle_t handle,
    const SaoUiTimeSeriesLane* lanes,
    size_t lane_count);

// Append points to an existing lane (streaming updates).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_time_series_chart_append(
    sao_ui_widget_handle_t handle,
    const char* lane_id_utf8,
    const SaoUiTimePoint* points,
    size_t point_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_time_series_chart_set_zoom(
    sao_ui_widget_handle_t handle,
    int64_t window_ms);                     // 0 → autoscale

// ─── Bar chart — categorical bars (top-N damage, buff durations) ─────
struct SaoUiBarChartBar {
    const char* label_utf8;
    double      value;
    uint32_t    fill_argb;
    uint32_t    border_argb;
    // Optional secondary segment (mem_priority overlay etc.).
    double      overlay_value;
    uint32_t    overlay_fill_argb;
};

struct SaoUiBarChartSpec {
    const SaoUiBarChartBar* bars;
    size_t                  bar_count;
    SaoUiAxisSpec           value_axis;
    bool                    horizontal;     // true → labels on Y
    bool                    sort_desc;      // sort bars by value
    bool                    show_value_labels;
    bool                    show_percent;
    uint8_t                 _pad[4];
    int32_t                 bar_gap_px;
    int32_t                 bar_thickness_px;
    uint32_t                bg_argb;
    uint32_t                label_argb;
    int32_t                 max_visible_bars;  // beyond this → more_indicator
    uint8_t                 _pad2[4];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_bar_chart_create(
    void* d3d_device_ptr,
    const SaoUiBarChartSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_bar_chart_set_bars(
    sao_ui_widget_handle_t handle,
    const SaoUiBarChartBar* bars,
    size_t bar_count);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_bar_chart_get_bar_count(
    sao_ui_widget_handle_t handle,
    size_t* out_count,
    size_t* out_hidden_count);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_bar_chart_get_bar(
    sao_ui_widget_handle_t handle,
    size_t index,
    SaoUiBarChartBar* out_bar);

// ─── Line chart — free-form (X, Y) — scatter/threshold plots ─────────
struct SaoUiLinePoint {
    double x;
    double y;
};

struct SaoUiLineChartSeries {
    const char*           series_id_utf8;
    const char*           label_utf8;
    const SaoUiLinePoint* points;
    size_t                point_count;
    uint32_t              line_argb;
    uint32_t              marker_argb;
    float                 line_width_px;
    bool                  dashed;
    bool                  show_markers;
    uint8_t               _pad[2];
    double                threshold_y;      // NaN → no threshold line
    uint32_t              threshold_argb;
};

struct SaoUiLineChartSpec {
    const SaoUiLineChartSeries* series;
    size_t                      series_count;
    SaoUiAxisSpec               x_axis;
    SaoUiAxisSpec               y_axis;
    uint32_t                    bg_argb;
    uint32_t                    frame_argb;
    bool                        show_legend;
    bool                        crosshair_on_hover;
    uint8_t                     _pad[2];
    int32_t                     left_pad_px;
    int32_t                     right_pad_px;
    int32_t                     top_pad_px;
    int32_t                     bottom_pad_px;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_line_chart_create(
    void* d3d_device_ptr,
    const SaoUiLineChartSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_line_chart_set_series(
    sao_ui_widget_handle_t handle,
    const SaoUiLineChartSeries* series,
    size_t series_count);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_line_chart_get_series_count(
    sao_ui_widget_handle_t handle,
    size_t* out_count);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_line_chart_get_point(
    sao_ui_widget_handle_t handle,
    size_t series_index,
    size_t point_index,
    SaoUiLinePoint* out_point);
    
struct SaoUiSparklineSpec {
    const double* values;
    size_t        value_count;
    size_t        max_points;
    uint32_t      line_argb;
    uint32_t      fill_argb;
    float         line_width_px;
    bool          area_fill;
    uint8_t       _pad[3];
};
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sparkline_create(
    void* d3d_device_ptr,
    const SaoUiSparklineSpec* spec,
    sao_ui_widget_handle_t* out_handle);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sparkline_set_values(
    sao_ui_widget_handle_t handle,
    const double* values,
    size_t value_count);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sparkline_append(
    sao_ui_widget_handle_t handle,
    double value);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sparkline_get_range(
    sao_ui_widget_handle_t handle,
    double* out_min,
    double* out_max,
    size_t* out_count);

#ifdef __cplusplus
}  // extern "C"
#endif
