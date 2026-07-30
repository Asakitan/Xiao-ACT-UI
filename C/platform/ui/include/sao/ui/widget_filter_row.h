// SAO Auto — panel search/filter helper row.
//
// Generic filter-chips + search-box row that panels (workshop, plugin
// manager, any SDK-registered panel) instantiate to drive a filtered
// list.  The row owns a search-box text field and a set of toggleable
// filter chips; filter state is exposed via the JSON props pipeline so
// the host panel can read the active query + selected chips without
// owning widget state.
//
// Game-agnostic: the chip labels + ids come from the caller; this widget
// only knows "text query + list of toggle chips".  It emits a
// VALUE_CHANGED event (via the widget_kit event registry) whenever the
// query text or chip selection changes.
//
// Conventions match widget_kit: opaque handle, transactional JSON props,
// theme-token colours, status-code returns.  Paint uses d2d_widgets
// primitives (rounded panel + text + badge-style chips).

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/theme.h"

#ifdef __cplusplus
extern "C"
{
#endif

// Extended widget kind — continues past SAO_UI_WIDGET_ANIMATED_BADGE.
enum sao_ui_widget_filter_kind_e : int32_t {
    SAO_UI_WIDGET_FILTER_ROW = 171,
};

// ── Chip definition ────────────────────────────────────────────────
// chip_id is opaque to this widget; the caller owns semantics.  label
// is the chip's visible text.  selected initial state is set via the
// spec; toggling is driven by sao_ui_filter_row_toggle_chip or props.
struct SaoUiFilterChipSpec {
    const char* label_utf8;
    int32_t     chip_id;
    bool        selected;
    uint8_t     _pad[3];
};

// ── Spec ──────────────────────────────────────────────────────────
struct SaoUiFilterRowSpec {
    const char* placeholder_utf8;    // search-box empty hint
    const SaoUiFilterChipSpec* chips;
    size_t                      chip_count;
    int32_t     search_box_width_px; // 0 → 200
    int32_t     chip_height_px;      // 0 → 26
    int32_t     chip_gap_px;          // 0 → 8
    int32_t     font_size_px;         // 0 → 13
    int32_t     pad_x_px;            // 0 → 10
    int32_t     pad_y_px;            // 0 → 6
    int32_t     radius_px;           // 0 → 8
    uint32_t    bg_argb;             // 0 → SAO_UI_TOKEN_APP_CARD
    uint32_t    fg_argb;             // 0 → SAO_UI_TOKEN_APP_TEXT
    uint32_t    border_argb;         // 0 → SAO_UI_TOKEN_APP_BORDER
    uint32_t    chip_bg_argb;        // 0 → SAO_UI_TOKEN_APP_BG
    uint32_t    chip_selected_argb;  // 0 → SAO_UI_TOKEN_APP_ACCENT
    uint32_t    chip_fg_argb;        // 0 → SAO_UI_TOKEN_APP_TEXT
    uint32_t    chip_selected_fg_argb; // 0 → SAO_UI_TOKEN_WHITE
    SaoUiThemeId theme_override;     // SAO_UI_THEME_COUNT = inherit
    uint8_t     _pad[4];
};

// ── Lifecycle ─────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_filter_row_create(
    void* d3d_device_ptr,
    const SaoUiFilterRowSpec* spec,
    sao_ui_widget_handle_t* out_handle);

// ── Query text ────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_filter_row_set_query(
    sao_ui_widget_handle_t handle,
    const char* text_utf8);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_filter_row_get_query(
    sao_ui_widget_handle_t handle,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_written);

// ── Chip selection ────────────────────────────────────────────────
// Toggle the chip with the given id.  Returns SAO_STATUS_ERR_NOT_FOUND
// if the id is not in the chip set.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_filter_row_toggle_chip(
    sao_ui_widget_handle_t handle,
    int32_t chip_id);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_filter_row_set_chip_selected(
    sao_ui_widget_handle_t handle,
    int32_t chip_id,
    bool selected);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_filter_row_is_chip_selected(
    sao_ui_widget_handle_t handle,
    int32_t chip_id,
    bool* out_selected);

// ── Match helper ──────────────────────────────────────────────────
// Returns true (out_match == true) when candidate_utf8 contains the
// active query (case-insensitive substring).  Empty query → true for
// all.  Chips are not consulted here — this is the text filter only.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_filter_row_matches_text(
    sao_ui_widget_handle_t handle,
    const char* candidate_utf8,
    bool* out_match);

// Returns true when at least one selected chip id is in selected_ids.
// Empty selected_ids → true (no chip filter active).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_filter_row_matches_chips(
    sao_ui_widget_handle_t handle,
    const int32_t* selected_ids,
    size_t selected_count,
    bool* out_match);

// ── Transactional props ────────────────────────────────────────────
// Accepted keys (all optional):
//   { "query": "utf-8", "chips": [{"id": <int>, "selected": <bool>}...],
//     "placeholder": "utf-8", "search_width": <int>, "chip_height": <int>,
//     "chip_gap": <int>, "font_size": <int>, "pad_x": <int>,
//     "pad_y": <int>, "radius": <int>, "bg": "#RRGGBBAA", "fg": "#RRGGBBAA",
//     "border": "#RRGGBBAA", "chip_bg": "#RRGGBBAA",
//     "chip_selected": "#RRGGBBAA", "chip_fg": "#RRGGBBAA",
//     "chip_selected_fg": "#RRGGBBAA", "theme": <int 0..2> }
// Chips list replaces chip selection state by id; unknown ids ignored.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_filter_row_apply_props(
    sao_ui_widget_handle_t handle,
    const uint8_t* props_json_utf8,
    size_t props_len);

// Serialize current filter state as JSON for the host panel.
// Shape: { "query": "utf-8", "chips": [{"id": <int>, "selected": <bool>}...] }
// out_json_utf8 receives the bytes; capacity is the buffer size.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_filter_row_state_json(
    sao_ui_widget_handle_t handle,
    char* out_json_utf8,
    size_t capacity,
    size_t* out_bytes_written);

#ifdef __cplusplus
} // extern "C"
#endif