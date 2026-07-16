// SAO Auto — panel layout engine.  Two-phase measure→arrange with
// per-node dirty flags; a layout tree is separate from the widget tree
// so a widget can appear more than once (rare) and the compositor sees
// only the mutated strip (see `take_dirty_rects`).  Python duals:
// tk.Frame pack/grid/place + CSS flex/grid.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/widget_container.h"        // SaoUiTrackSize etc.
#include "sao/ui/d2d_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_layout_node_s* sao_ui_layout_node_handle_t;
typedef struct sao_ui_layout_tree_s* sao_ui_layout_tree_handle_t;

// ─── Layout mode enum ────────────────────────────────────────────────
enum sao_ui_layout_mode_e : int32_t {
    // Stacks children top-to-bottom.  Widths default to fill parent.
    SAO_UI_LAYOUT_VERTICAL   = 0,
    // Stacks children left-to-right.  Heights default to fill parent.
    SAO_UI_LAYOUT_HORIZONTAL = 1,
    // CSS-grid style; uses SaoUiTrackSize rows/cols (see widget_container.h).
    SAO_UI_LAYOUT_GRID       = 2,
    // Children pinned by absolute (x, y) inside the container.
    SAO_UI_LAYOUT_ABSOLUTE   = 3,
    // Flexbox — mode + wrap + justify + align (CSS-alike).
    SAO_UI_LAYOUT_FLEX       = 4,
    // Dock — top/right/bottom/left/center regions (WPF-alike).
    SAO_UI_LAYOUT_DOCK       = 5,
};

enum sao_ui_dock_side_e : int32_t {
    SAO_UI_DOCK_TOP    = 0,
    SAO_UI_DOCK_RIGHT  = 1,
    SAO_UI_DOCK_BOTTOM = 2,
    SAO_UI_DOCK_LEFT   = 3,
    SAO_UI_DOCK_CENTER = 4,
    SAO_UI_DOCK_FILL   = 4,                 // alias
};

enum sao_ui_flex_direction_e : int32_t {
    SAO_UI_FLEX_ROW            = 0,
    SAO_UI_FLEX_COLUMN         = 1,
    SAO_UI_FLEX_ROW_REVERSE    = 2,
    SAO_UI_FLEX_COLUMN_REVERSE = 3,
};

enum sao_ui_flex_wrap_e : int32_t {
    SAO_UI_FLEX_NOWRAP       = 0,
    SAO_UI_FLEX_WRAP         = 1,
    SAO_UI_FLEX_WRAP_REVERSE = 2,
};

enum sao_ui_flex_justify_e : int32_t {
    SAO_UI_JUSTIFY_START         = 0,
    SAO_UI_JUSTIFY_END           = 1,
    SAO_UI_JUSTIFY_CENTER        = 2,
    SAO_UI_JUSTIFY_SPACE_BETWEEN = 3,
    SAO_UI_JUSTIFY_SPACE_AROUND  = 4,
    SAO_UI_JUSTIFY_SPACE_EVENLY  = 5,
};

enum sao_ui_align_axis_e : int32_t {
    SAO_UI_ALIGN_AXIS_START    = 0,
    SAO_UI_ALIGN_AXIS_END      = 1,
    SAO_UI_ALIGN_AXIS_CENTER   = 2,
    SAO_UI_ALIGN_AXIS_STRETCH  = 3,
    SAO_UI_ALIGN_AXIS_BASELINE = 4,
};

// ─── Spec shared by every layout node ────────────────────────────────
//
// Padding / margin are (top, right, bottom, left) matching CSS box
// model.  gap is inter-child spacing.  min/max size clamp the measured
// preferred size.  weight is used by vertical/horizontal/flex to divide
// leftover space.  See panel_layout.cpp `sao_ui_layout_measure` for the
// exact resolution order.
struct SaoUiLayoutSpec {
    // Box model.
    int32_t  pad_top_px;
    int32_t  pad_right_px;
    int32_t  pad_bottom_px;
    int32_t  pad_left_px;
    int32_t  margin_top_px;
    int32_t  margin_right_px;
    int32_t  margin_bottom_px;
    int32_t  margin_left_px;
    int32_t  gap_px;

    // Explicit size clamps.  0 → unbounded.
    int32_t  min_width_px;
    int32_t  min_height_px;
    int32_t  max_width_px;
    int32_t  max_height_px;
    int32_t  fixed_width_px;                // > 0 → overrides preferred
    int32_t  fixed_height_px;

    // Distribution weight (vertical / horizontal / flex-grow).
    float    weight;                        // 0 → shrink to preferred
    float    flex_shrink;                   // 0 → refuse to shrink
    float    flex_basis;                    // 0 → use preferred size

    // Alignment.
    int32_t  align_h;                       // sao_ui_flex_justify_e
    int32_t  align_v;                       // sao_ui_align_axis_e

    // Absolute mode overrides.
    int32_t  absolute_x_px;                 // relative to parent client
    int32_t  absolute_y_px;
    int32_t  absolute_z;                    // stacking order within absolute parent

    // Dock mode side.
    int32_t  dock_side;                     // sao_ui_dock_side_e

    // Whether this subtree participates in hit testing.
    bool     hit_testable;
    // Whether to clip children to this node's rect.
    bool     clip_children;
    // Whether the layout for this subtree is invalidated (dirty).
    bool     force_dirty;
    uint8_t  _pad[5];
};

// Fill a spec with sensible defaults matching `tk.Frame`'s default
// pack(fill='x') behaviour.  Callers modify what they need.
SAO_UI_API void SAO_UI_CALL sao_ui_layout_spec_defaults(
    SaoUiLayoutSpec* out_spec);

// ─── Container-mode configs ──────────────────────────────────────────
//
// Passed to `sao_ui_layout_node_set_mode()` when the node is a container.
// A leaf widget node never uses these.

struct SaoUiVerticalMode {
    bool  reverse;                          // stack from bottom
    bool  fill_available;                   // last child expands
    uint8_t _pad[6];
};

struct SaoUiHorizontalMode {
    bool  reverse;
    bool  fill_available;
    uint8_t _pad[6];
};

struct SaoUiGridMode {
    // Row/column track descriptors — same union as widget_container.h.
    const SaoUiTrackSize* rows;
    size_t                row_count;
    const SaoUiTrackSize* cols;
    size_t                col_count;
    int32_t               row_gap_px;
    int32_t               col_gap_px;
    bool                  dense_packing;    // CSS grid-auto-flow: dense
    uint8_t               _pad[7];
};

struct SaoUiAbsoluteMode {
    // No extra fields — children specify (absolute_x, y, z) in their
    // SaoUiLayoutSpec.  Container size follows fixed_* or the union rect
    // of children.
    bool  fit_children_bbox;
    uint8_t _pad[7];
};

struct SaoUiFlexMode {
    int32_t direction;                      // sao_ui_flex_direction_e
    int32_t wrap;                           // sao_ui_flex_wrap_e
    int32_t justify;                        // sao_ui_flex_justify_e
    int32_t align_items;                    // sao_ui_align_axis_e (cross axis)
    int32_t align_content;                  // multi-line align
    int32_t gap_main_px;
    int32_t gap_cross_px;
    uint8_t _pad[4];
};

struct SaoUiDockMode {
    // Whether the last-added child fills the remaining space.
    bool  last_child_fills;
    uint8_t _pad[7];
};

// ─── Tree lifecycle ──────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_tree_create(
    sao_ui_layout_tree_handle_t* out_tree);

SAO_UI_API void SAO_UI_CALL sao_ui_layout_tree_destroy(
    sao_ui_layout_tree_handle_t tree);

// Create the root container node.  Every tree has exactly one root; a
// second call is SAO_STATUS_ERR_ALREADY_EXISTS.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_tree_set_root(
    sao_ui_layout_tree_handle_t tree,
    int32_t layout_mode,                    // sao_ui_layout_mode_e
    const SaoUiLayoutSpec* spec,
    sao_ui_layout_node_handle_t* out_root);

// Add a container child.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_node_add_container(
    sao_ui_layout_node_handle_t parent,
    int32_t layout_mode,
    const SaoUiLayoutSpec* spec,
    sao_ui_layout_node_handle_t* out_child);

// Add a leaf widget.  The layout engine calls
// `sao_ui_widget_get_size_hint()` during measure.  The widget's own
// paint is invoked via `sao_ui_widget_paint_at()` at arrange time.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_node_add_widget(
    sao_ui_layout_node_handle_t parent,
    sao_ui_widget_handle_t widget,
    const SaoUiLayoutSpec* spec,
    sao_ui_layout_node_handle_t* out_leaf);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_node_remove(
    sao_ui_layout_node_handle_t node);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_node_set_spec(
    sao_ui_layout_node_handle_t node,
    const SaoUiLayoutSpec* spec);

// Set the mode-specific config on a container node.  The struct pointed
// to must match the node's mode (vertical → SaoUiVerticalMode, etc.).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_node_set_mode_config(
    sao_ui_layout_node_handle_t container,
    const void* mode_config);

// Reorder children.  new_index is clamped to [0, sibling_count].
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_node_reorder(
    sao_ui_layout_node_handle_t child,
    int32_t new_index);

// ─── Measure / arrange ───────────────────────────────────────────────
struct SaoUiSize {
    int32_t width_px;
    int32_t height_px;
};

struct SaoUiRect {
    int32_t x_px;
    int32_t y_px;
    int32_t width_px;
    int32_t height_px;
};

// Measure the subtree; returns the resolved preferred size.  Repeated
// calls with unchanged inputs short-circuit via cached measurements.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_measure(
    sao_ui_layout_node_handle_t root,
    SaoUiSize available,
    SaoUiSize* out_preferred);

// Arrange (position) the subtree inside the given rect.  Must follow
// a measure pass whose `available` >= arrange rect size.  Populates the
// per-node final rect (readable via sao_ui_layout_node_get_rect).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_arrange(
    sao_ui_layout_node_handle_t root,
    SaoUiRect rect);

// Query the final arranged rect (after arrange).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_node_get_rect(
    sao_ui_layout_node_handle_t node,
    SaoUiRect* out_rect);

// ─── Hit testing ─────────────────────────────────────────────────────
struct SaoUiHitResult {
    sao_ui_layout_node_handle_t node;
    sao_ui_widget_handle_t      widget;     // NULL when node is a container
    int32_t                     local_x_px;
    int32_t                     local_y_px;
    // Ancestors from hit → root.  NULL-terminated up to `path_len`.
    // A caller passing `NULL` in `out_path` gets the depth only.
    int32_t                     depth;
};

// Hit test a point (root-local pixels).  Only nodes with
// `hit_testable == true` are considered.  Returns the leaf-most hit
// whose subtree contains the point.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_hit_test(
    sao_ui_layout_node_handle_t root,
    int32_t x_px, int32_t y_px,
    SaoUiHitResult* out_hit);

// Enumerate the ancestor path from hit → root (root is last).  Copies
// up to `capacity` handles; total depth via `out_written`.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_get_hit_path(
    sao_ui_layout_node_handle_t hit_node,
    sao_ui_layout_node_handle_t* out_path,
    size_t capacity,
    size_t* out_written);

// ─── Dirty flag / invalidation ───────────────────────────────────────
//
// The layout engine tracks per-node dirty flags.  Setting the flag on a
// container invalidates its measure cache; ancestors are propagated as
// needed.  The compositor calls `sao_ui_layout_take_dirty_rects()` after
// arrange to know which regions to redraw.

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_node_invalidate(
    sao_ui_layout_node_handle_t node);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_node_invalidate_subtree(
    sao_ui_layout_node_handle_t node);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_tree_invalidate_all(
    sao_ui_layout_tree_handle_t tree);

// Snapshot dirty rects after arrange.  Rects are in root-local pixels
// and represent the union of areas whose contents changed since the
// last take.  Calling this clears the internal dirty set.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_take_dirty_rects(
    sao_ui_layout_tree_handle_t tree,
    SaoUiRect* out_rects,
    size_t capacity,
    size_t* out_written);

// ─── Snapshot / debug ────────────────────────────────────────────────
//
// Emit a JSON tree snapshot (indented, UTF-8) for panel debugging / unit
// tests.  Every node's mode + spec + rect + widget handle is included.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layout_dump_json(
    sao_ui_layout_tree_handle_t tree,
    uint8_t* out_utf8_buffer,
    size_t capacity,
    size_t* out_bytes_written);

#ifdef __cplusplus
}  // extern "C"
#endif
