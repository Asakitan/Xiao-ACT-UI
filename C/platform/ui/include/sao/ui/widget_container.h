// SAO Auto — container widgets (panel / scroll / tab / grid).
//
// A container owns a set of child widget handles and delegates layout to
// the `panel_layout.h` engine.  Containers are widgets themselves so a
// panel-inside-a-scroll-inside-a-panel is naturally expressible without
// a separate scene-graph type.
//
// Python source alignment:
//   * rounded_panel   → sao_panel_components.rounded_panel (fill / border /
//                       rail / rail_w / pad / height / canvas_bg / shadow)
//   * scroll view     → sao_panel_components._SaoScroll + bind_canvas_mousewheel
//   * tab view        → SAO tabs (sao_gui_dps DAMAGE|HEALING; buffmon light|dark)
//   * grid            → tk.Grid + web CSS-grid dual

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── Panel — rounded_panel with fill / border / rail / shadow ────────
struct SaoUiPanelWidgetSpec {
    uint32_t bg_argb;                       // 0 → theme 'card_bg'
    uint32_t border_argb;                   // 0 → theme 'border'
    uint32_t rail_argb;                     // 0 → no left rail
    uint32_t canvas_bg_argb;                // 0 → theme 'body_bg'
    int32_t  radius_px;
    int32_t  rail_width_px;                 // typical 3px
    int32_t  pad_px;                        // inner content inset
    int32_t  fixed_height_px;               // 0 → auto-grow to content
    bool     shadow;                        // flat two-layer drop shadow
    bool     clip_children;                 // scissor to rounded rect
    uint8_t  _pad[6];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_widget_create(
    void* d3d_device_ptr,
    const SaoUiPanelWidgetSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_widget_add_child(
    sao_ui_widget_handle_t parent,
    sao_ui_widget_handle_t child);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_widget_remove_child(
    sao_ui_widget_handle_t parent,
    sao_ui_widget_handle_t child);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_widget_clear(
    sao_ui_widget_handle_t parent);

// ─── Scroll view — vertical/horizontal scroll with slim rounded thumb ─
enum sao_ui_scroll_orientation_e : int32_t {
    SAO_UI_SCROLL_VERTICAL   = 0,
    SAO_UI_SCROLL_HORIZONTAL = 1,
    SAO_UI_SCROLL_BOTH       = 2,
};

struct SaoUiScrollViewSpec {
    int32_t  orientation;                   // sao_ui_scroll_orientation_e
    int32_t  thumb_width_px;                // matches Python _SaoScroll default 9
    uint32_t track_bg_argb;                 // 0 → theme 'body_bg'
    uint32_t thumb_argb;                    // 0 → theme 'border'
    uint32_t thumb_hover_argb;
    bool     auto_hide_thumb;               // hide when everything fits
    bool     mousewheel_enabled;
    bool     kinetic_flick;                 // touch-style momentum after drag
    uint8_t  _pad[5];
    // Scroll region — total content extent.  Set via
    // sao_ui_scroll_view_set_content_size when children change.
    int32_t  initial_content_width_px;
    int32_t  initial_content_height_px;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scroll_view_create(
    void* d3d_device_ptr,
    const SaoUiScrollViewSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scroll_view_add_child(
    sao_ui_widget_handle_t view,
    sao_ui_widget_handle_t child,
    int32_t child_x, int32_t child_y);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scroll_view_set_content_size(
    sao_ui_widget_handle_t view,
    int32_t width_px, int32_t height_px);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scroll_view_scroll_to(
    sao_ui_widget_handle_t view,
    float x_fraction_0_to_1,
    float y_fraction_0_to_1);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scroll_view_get_scroll(
    sao_ui_widget_handle_t view,
    float* out_x_fraction,
    float* out_y_fraction);

// ─── Tab view — horizontal tab bar + swappable panes ─────────────────
struct SaoUiTabSpec {
    const char* label_utf8;
    int32_t     tab_id;
    bool        disabled;
    uint8_t     _pad[3];
};

struct SaoUiTabViewSpec {
    const SaoUiTabSpec* tabs;
    size_t              tab_count;
    int32_t             initial_tab_id;
    int32_t             tab_bar_height_px;
    uint32_t            tab_bg_argb;
    uint32_t            tab_active_bg_argb;
    uint32_t            tab_fg_argb;
    uint32_t            tab_active_fg_argb;
    uint32_t            underline_argb;     // active tab underline (cyan/gold)
    int32_t             underline_thickness_px;
    bool                allow_close_button; // per-tab close 'x'
    bool                scroll_when_overflow;
    uint8_t             _pad[6];
};

typedef void (SAO_UI_CALL* sao_ui_tab_change_cb_t)(
    int32_t new_tab_id, void* user_data);
typedef void (SAO_UI_CALL* sao_ui_tab_close_cb_t)(
    int32_t tab_id, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tab_view_create(
    void* d3d_device_ptr,
    const SaoUiTabViewSpec* spec,
    sao_ui_widget_handle_t* out_handle);

// Attach a widget as the pane body for a given tab id.  The tab view
// shows exactly one pane at a time; the previously visible pane is
// hidden but not destroyed.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tab_view_set_pane(
    sao_ui_widget_handle_t handle,
    int32_t tab_id,
    sao_ui_widget_handle_t pane);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tab_view_select_tab(
    sao_ui_widget_handle_t handle, int32_t tab_id);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tab_view_set_change_handler(
    sao_ui_widget_handle_t handle,
    sao_ui_tab_change_cb_t callback, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tab_view_set_close_handler(
    sao_ui_widget_handle_t handle,
    sao_ui_tab_close_cb_t callback, void* user_data);

// ─── Grid — CSS-grid-style layout container ──────────────────────────
//
// A grid is a container with row/column tracks.  Children are placed by
// (row_start, col_start, row_span, col_span).  Track sizes use the same
// SaoUiTrackSize discriminated union as panel_layout.h so a grid can be
// used standalone or nested inside a flex/dock parent.
enum sao_ui_track_kind_e : int32_t {
    SAO_UI_TRACK_AUTO  = 0,   // size to content
    SAO_UI_TRACK_FIXED = 1,   // exact pixels
    SAO_UI_TRACK_FLEX  = 2,   // fractional (weight)
    SAO_UI_TRACK_MIN   = 3,   // minmax(min, auto)
};

struct SaoUiTrackSize {
    int32_t  kind;                          // sao_ui_track_kind_e
    int32_t  fixed_px;                      // for FIXED / MIN
    float    flex_weight;                   // for FLEX (1.0f = 1fr)
    int32_t  max_px;                        // 0 → unbounded
    uint8_t  _pad[4];
};

struct SaoUiGridSpec {
    const SaoUiTrackSize* rows;
    size_t                row_count;
    const SaoUiTrackSize* cols;
    size_t                col_count;
    int32_t               row_gap_px;
    int32_t               col_gap_px;
    int32_t               pad_px;
    uint32_t              bg_argb;
    uint8_t               _pad[4];
};

struct SaoUiGridPlacement {
    int32_t row_start;
    int32_t col_start;
    int32_t row_span;                       // ≥1
    int32_t col_span;                       // ≥1
    int32_t align_h;                        // sao_ui_text_align_e
    int32_t align_v;                        // sao_ui_text_align_e
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_grid_create(
    void* d3d_device_ptr,
    const SaoUiGridSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_grid_place_child(
    sao_ui_widget_handle_t grid,
    sao_ui_widget_handle_t child,
    const SaoUiGridPlacement* placement);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_grid_remove_child(
    sao_ui_widget_handle_t grid,
    sao_ui_widget_handle_t child);

#ifdef __cplusplus
}  // extern "C"
#endif
