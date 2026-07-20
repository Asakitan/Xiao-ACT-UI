// SAO Auto — table / tree data widgets (split from widget_data.h).
//
// Reference use case: sao_gui_dps entity table.  Columns are declared
// once; rows are updated in batches via SetRows/UpdateRow keyed by
// stable row id.  Sorting / filtering / row highlighting is a purely
// declarative attribute — the widget owns the state machine.
//
// Python source alignment:
//   * table  → sao_gui_dps entity rows: sortable columns, batched row
//              updates, highlight-row (is_self / mem_priority yellow
//              badge from memory [DPS Tk三条造行路径白名单])
//   * tree   → plugin manager tree + drilldown ancestors

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"
// SaoUiTableColumn::align is documented as sao_ui_text_align_e; the enum
// itself lives in <sao/ui/widget_text.h>.  Pull it in so callers only need
// this single header to talk to the table widget.
#include "sao/ui/widget_text.h"

#ifdef __cplusplus
extern "C" {
#endif

// A callback changed or retired the widget while an outer dispatch was in
// progress. The outer operation stops and callers may retry a live handle.
#ifndef SAO_UI_STATUS_ERR_BUSY
#define SAO_UI_STATUS_ERR_BUSY ((sao_status_t)-102)
#endif

// ─── Table — sortable / filterable / batched-update grid of rows ─────
enum sao_ui_column_type_e : int32_t {
    SAO_UI_COL_TEXT     = 0,
    SAO_UI_COL_NUMBER   = 1,
    SAO_UI_COL_PROGRESS = 2,                // bar rendered in the cell
    SAO_UI_COL_BADGE    = 3,                // status_badge in the cell
    SAO_UI_COL_ICON     = 4,
    SAO_UI_COL_ACTION   = 5,                // button in the cell
    SAO_UI_COL_CLOCK    = 6,                // fmt_clock (epoch_ms in value.i64)
    SAO_UI_COL_DURATION = 7,                // fmt_dur   (duration_ms)
    SAO_UI_COL_RELTIME  = 8,                // fmt_rel   (epoch_ms + row.base_ms)
};

struct SaoUiTableColumn {
    const char* key_utf8;                   // row dict key
    const char* title_utf8;
    int32_t     type;                       // sao_ui_column_type_e
    int32_t     align;                      // sao_ui_text_align_e
    int32_t     min_width_px;
    int32_t     max_width_px;               // 0 → unbounded
    float       flex_weight;                // for auto-fit sizing
    bool        sortable;
    bool        filterable;
    bool        resizable;
    bool        hidden;
    // Column-scoped colours (0 → theme).
    uint32_t    header_bg_argb;
    uint32_t    header_fg_argb;
    uint32_t    cell_fg_argb;
    uint32_t    cell_bg_alt_argb;           // zebra stripe
};

// A single cell value — a discriminated union.  Keeps table rows
// pointer-free so callers can push contiguous arrays without extra
// allocations.
enum sao_ui_cell_kind_e : int32_t {
    SAO_UI_CELL_STRING = 0,
    SAO_UI_CELL_INT64  = 1,
    SAO_UI_CELL_DOUBLE = 2,
    SAO_UI_CELL_BOOL   = 3,
};

struct SaoUiCellValue {
    int32_t kind;                           // sao_ui_cell_kind_e
    uint8_t _pad[4];
    union {
        const char* s_utf8;                 // owned by caller until commit
        int64_t     i64;
        double      f64;
        bool        b;
    } v;
    // For progress-column cells: value / max are in v.f64 / max_hint.
    double      max_hint;
    // Cell-scoped override colours (0 = column defaults).
    uint32_t    fg_argb;
    uint32_t    bg_argb;
};

struct SaoUiTableRow {
    int64_t              row_id;            // stable key across updates
    const SaoUiCellValue* cells;            // aligned with columns[]
    size_t               cell_count;
    bool                 highlight;         // 'is_self' style row
    bool                 mem_priority_badge; // yellow MEM badge (memory [DPS Tk三条造行路径白名单])
    bool                 zebra_alt;
    bool                 dim;                // muted (dead / left encounter)
    uint32_t             row_bg_override_argb;
    uint32_t             row_fg_override_argb;
};

struct SaoUiTableSpec {
    const SaoUiTableColumn* columns;
    size_t                  column_count;
    int32_t                 row_height_px;
    int32_t                 header_height_px;
    bool                    show_header;
    bool                    zebra_stripes;
    bool                    row_hover_highlight;
    bool                    multi_select;
    uint8_t                 _pad[4];
    uint32_t                grid_line_argb;
    uint32_t                header_bg_argb;
    uint32_t                header_fg_argb;
    uint32_t                body_bg_argb;
    // Sort/filter state.
    const char*             initial_sort_key_utf8;   // "" → unsorted
    bool                    initial_sort_desc;
    uint8_t                 _pad2[7];
    const char*             initial_filter_utf8;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_table_create(
    void* d3d_device_ptr,
    const SaoUiTableSpec* spec,
    sao_ui_widget_handle_t* out_handle);

// Wholesale row replacement (batched).  For per-row deltas use
// sao_ui_table_upsert_row.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_table_set_rows(
    sao_ui_widget_handle_t handle,
    const SaoUiTableRow* rows,
    size_t row_count);

// Add or update a single row by row_id.  Missing → insert.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_table_upsert_row(
    sao_ui_widget_handle_t handle,
    const SaoUiTableRow* row);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_table_remove_row(
    sao_ui_widget_handle_t handle,
    int64_t row_id);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_table_clear_rows(
    sao_ui_widget_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_table_set_sort(
    sao_ui_widget_handle_t handle,
    const char* column_key_utf8,             // NULL → unsorted
    bool descending);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_table_set_filter(
    sao_ui_widget_handle_t handle,
    const char* filter_utf8);

typedef void (SAO_UI_CALL* sao_ui_table_row_click_cb_t)(
    int64_t row_id, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_table_set_row_click_handler(
    sao_ui_widget_handle_t handle,
    sao_ui_table_row_click_cb_t callback,
    void* user_data);

// Cell-action click — for SAO_UI_COL_ACTION columns.
typedef void (SAO_UI_CALL* sao_ui_table_cell_action_cb_t)(
    int64_t row_id,
    const char* column_key_utf8,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_table_set_cell_action_handler(
    sao_ui_widget_handle_t handle,
    sao_ui_table_cell_action_cb_t callback,
    void* user_data);

// ─── Tree view — hierarchical rows with expand/collapse ──────────────
struct SaoUiTreeNode {
    int64_t             node_id;
    int64_t             parent_id;          // 0 → root
    const char*         label_utf8;
    const char*         detail_utf8;        // secondary line (dimmed)
    int32_t             icon_slot;          // -1 → no icon
    bool                expanded_default;
    bool                selectable;
    uint8_t             _pad[6];
    uint32_t            fg_argb;
    uint32_t            bg_argb;
};

struct SaoUiTreeViewSpec {
    int32_t     row_height_px;
    int32_t     indent_px;                  // per-level indent
    int32_t     caret_width_px;
    bool        show_lines;                 // connector lines like child_bar
    bool        multi_select;
    uint8_t     _pad[6];
    uint32_t    body_bg_argb;
    uint32_t    connector_argb;
    uint32_t    caret_argb;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tree_view_create(
    void* d3d_device_ptr,
    const SaoUiTreeViewSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tree_view_set_nodes(
    sao_ui_widget_handle_t handle,
    const SaoUiTreeNode* nodes,
    size_t node_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tree_view_expand_node(
    sao_ui_widget_handle_t handle,
    int64_t node_id,
    bool expanded);

typedef void (SAO_UI_CALL* sao_ui_tree_select_cb_t)(
    int64_t node_id, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tree_view_set_select_handler(
    sao_ui_widget_handle_t handle,
    sao_ui_tree_select_cb_t callback,
    void* user_data);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tree_view_select_node(
    sao_ui_widget_handle_t handle,
    int64_t node_id);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tree_view_get_visible_count(
    sao_ui_widget_handle_t handle,
    size_t* out_count);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_tree_view_get_visible_node(
    sao_ui_widget_handle_t handle,
    size_t visible_index,
    int64_t* out_node_id,
    int32_t* out_depth);

#ifdef __cplusplus
}  // extern "C"
#endif
