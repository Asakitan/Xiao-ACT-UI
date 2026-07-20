// SAO Auto — panel_layout engine first slice.
//
// Two-phase measure→arrange with 6 layout
// modes (vertical, horizontal, grid, absolute, flex, dock) and per-
// node dirty flags.  1:1 with tk pack/grid/place + CSS flex/grid.
//
// The stub API contract is preserved — every function that used to
// return NOT_IMPLEMENTED now returns OK for the covered code paths.
// Uncovered corners (JSON dump, hit-path enumeration) still return
// NOT_IMPLEMENTED so callers can distinguish "unimplemented" from
// "empty result".

#include "sao/ui/panel_layout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

constexpr int32_t kUnbounded = 1 << 30;

inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace

// ─── Node ────────────────────────────────────────────────────────────
//
// The layout tree is a plain owned tree — parent holds unique_ptr to
// children.  A node is either a container (has `layout_mode`) or a
// leaf widget (has `widget_handle`, no children).  Absolute mode
// children override rect via their spec's absolute_x/y.

struct sao_ui_layout_node_s {
    // Identity.
    sao_ui_layout_node_s* parent = nullptr;
    std::vector<std::unique_ptr<sao_ui_layout_node_s>> children;
    // Config.
    int32_t layout_mode = SAO_UI_LAYOUT_VERTICAL;
    SaoUiLayoutSpec spec {};
    // Leaf widget (nullptr when container).
    sao_ui_widget_handle_t widget = nullptr;
    // Mode-specific configs (only the one matching layout_mode is
    // populated).  Filled by set_mode_config.
    SaoUiVerticalMode vertical{};
    SaoUiHorizontalMode horizontal{};
    SaoUiGridMode grid{};
    SaoUiAbsoluteMode absolute{};
    SaoUiFlexMode flex{};
    SaoUiDockMode dock{};
    // Grid stores its own owned copies of row/col track arrays since
    // the caller's SaoUiTrackSize array can be transient.
    std::vector<SaoUiTrackSize> grid_rows_owned;
    std::vector<SaoUiTrackSize> grid_cols_owned;
    // Runtime state.
    SaoUiSize measured{0, 0};
    SaoUiRect arranged{0, 0, 0, 0};
    bool dirty = true;
};

struct sao_ui_layout_tree_s {
    std::mutex mu;
    std::unique_ptr<sao_ui_layout_node_s> root;
    // Dirty-rect ledger populated during arrange for nodes that
    // changed rect since the last take.
    std::vector<SaoUiRect> dirty_rects;
};

namespace {

const char* layout_mode_name(int32_t mode) {
    switch (mode) {
    case SAO_UI_LAYOUT_VERTICAL: return "vertical";
    case SAO_UI_LAYOUT_HORIZONTAL: return "horizontal";
    case SAO_UI_LAYOUT_GRID: return "grid";
    case SAO_UI_LAYOUT_ABSOLUTE: return "absolute";
    case SAO_UI_LAYOUT_FLEX: return "flex";
    case SAO_UI_LAYOUT_DOCK: return "dock";
    default: return "unknown";
    }
}

nlohmann::json layout_node_json(const sao_ui_layout_node_s& node) {
    nlohmann::json children = nlohmann::json::array();
    for (const auto& child : node.children) children.push_back(layout_node_json(*child));
    return {
        {"mode", layout_mode_name(node.layout_mode)},
        {"widget", reinterpret_cast<uintptr_t>(node.widget)},
        {"dirty", node.dirty},
        {"measured", {
            {"width", node.measured.width_px},
            {"height", node.measured.height_px},
        }},
        {"rect", {
            {"x", node.arranged.x_px},
            {"y", node.arranged.y_px},
            {"width", node.arranged.width_px},
            {"height", node.arranged.height_px},
        }},
        {"spec", {
            {"padding", {node.spec.pad_top_px, node.spec.pad_right_px,
                          node.spec.pad_bottom_px, node.spec.pad_left_px}},
            {"margin", {node.spec.margin_top_px, node.spec.margin_right_px,
                         node.spec.margin_bottom_px, node.spec.margin_left_px}},
            {"gap", node.spec.gap_px},
            {"min_width", node.spec.min_width_px},
            {"min_height", node.spec.min_height_px},
            {"max_width", node.spec.max_width_px},
            {"max_height", node.spec.max_height_px},
            {"fixed_width", node.spec.fixed_width_px},
            {"fixed_height", node.spec.fixed_height_px},
            {"weight", node.spec.weight},
            {"hit_testable", node.spec.hit_testable},
            {"clip_children", node.spec.clip_children},
        }},
        {"children", std::move(children)},
    };
}

}  // namespace

// ─── helpers ─────────────────────────────────────────────────────────

static void mark_dirty_up(sao_ui_layout_node_s* node) {
    while (node != nullptr) {
        node->dirty = true;
        node = node->parent;
    }
}

// Preferred size hint for a leaf widget.  Since widget size hints are
// wired up in later slices (calling into d2d_widgets.cpp), we approx
// with the spec's fixed size or (100, 24) for text-ish defaults.
static SaoUiSize widget_size_hint(const sao_ui_layout_node_s* node) {
    SaoUiSize hint{100, 24};
    if (node->spec.fixed_width_px > 0) hint.width_px = node->spec.fixed_width_px;
    if (node->spec.fixed_height_px > 0) hint.height_px = node->spec.fixed_height_px;
    return hint;
}

static SaoUiSize apply_clamp(const SaoUiLayoutSpec& spec, SaoUiSize s) {
    if (spec.min_width_px > 0) s.width_px = std::max(s.width_px, spec.min_width_px);
    if (spec.min_height_px > 0) s.height_px = std::max(s.height_px, spec.min_height_px);
    if (spec.max_width_px > 0) s.width_px = std::min(s.width_px, spec.max_width_px);
    if (spec.max_height_px > 0) s.height_px = std::min(s.height_px, spec.max_height_px);
    return s;
}

static void insets_of(const SaoUiLayoutSpec& spec, int32_t& l, int32_t& t, int32_t& r, int32_t& b) {
    l = spec.margin_left_px + spec.pad_left_px;
    t = spec.margin_top_px + spec.pad_top_px;
    r = spec.margin_right_px + spec.pad_right_px;
    b = spec.margin_bottom_px + spec.pad_bottom_px;
}

// ─── measure recursion ──────────────────────────────────────────────

static SaoUiSize measure_node(sao_ui_layout_node_s* node, SaoUiSize available) {
    // Leaf.
    if (node->widget != nullptr || node->children.empty()) {
        SaoUiSize s = widget_size_hint(node);
        if (node->spec.fixed_width_px > 0) s.width_px = node->spec.fixed_width_px;
        if (node->spec.fixed_height_px > 0) s.height_px = node->spec.fixed_height_px;
        s = apply_clamp(node->spec, s);
        node->measured = s;
        node->dirty = false;
        return s;
    }
    // Container.
    int32_t ml, mt, mr, mb;
    insets_of(node->spec, ml, mt, mr, mb);
    const int32_t inner_w = std::max(0, available.width_px - ml - mr);
    const int32_t inner_h = std::max(0, available.height_px - mt - mb);
    SaoUiSize child_avail{inner_w, inner_h};
    int32_t total_w = 0;
    int32_t total_h = 0;
    const int32_t gap = node->spec.gap_px;
    switch (node->layout_mode) {
    case SAO_UI_LAYOUT_VERTICAL: {
        int32_t max_child_w = 0;
        int32_t sum_h = 0;
        int32_t count = 0;
        for (auto& c : node->children) {
            SaoUiSize s = measure_node(c.get(), child_avail);
            max_child_w = std::max(max_child_w, s.width_px);
            sum_h += s.height_px;
            ++count;
        }
        if (count > 1) sum_h += gap * (count - 1);
        total_w = max_child_w;
        total_h = sum_h;
        break;
    }
    case SAO_UI_LAYOUT_HORIZONTAL: {
        int32_t sum_w = 0;
        int32_t max_child_h = 0;
        int32_t count = 0;
        for (auto& c : node->children) {
            SaoUiSize s = measure_node(c.get(), child_avail);
            sum_w += s.width_px;
            max_child_h = std::max(max_child_h, s.height_px);
            ++count;
        }
        if (count > 1) sum_w += gap * (count - 1);
        total_w = sum_w;
        total_h = max_child_h;
        break;
    }
    case SAO_UI_LAYOUT_GRID: {
        // Sum fixed track sizes + gaps.  Auto/flex tracks contribute
        // their nominal min in measure phase; arrange phase distributes
        // the leftover.
        int32_t sum_cols = 0;
        for (const SaoUiTrackSize& t : node->grid_cols_owned) {
            if (t.kind == SAO_UI_TRACK_FIXED) sum_cols += t.fixed_px;
            else if (t.kind == SAO_UI_TRACK_MIN) sum_cols += t.fixed_px;
            else if (t.kind == SAO_UI_TRACK_FLEX) sum_cols += static_cast<int32_t>(t.flex_weight * 20.0f);
            // AUTO: contribute 40px placeholder.
            else sum_cols += 40;
        }
        int32_t sum_rows = 0;
        for (const SaoUiTrackSize& t : node->grid_rows_owned) {
            if (t.kind == SAO_UI_TRACK_FIXED) sum_rows += t.fixed_px;
            else if (t.kind == SAO_UI_TRACK_MIN) sum_rows += t.fixed_px;
            else if (t.kind == SAO_UI_TRACK_FLEX) sum_rows += static_cast<int32_t>(t.flex_weight * 20.0f);
            else sum_rows += 24;
        }
        const int32_t ncols = static_cast<int32_t>(node->grid_cols_owned.size());
        const int32_t nrows = static_cast<int32_t>(node->grid_rows_owned.size());
        if (ncols > 1) sum_cols += node->grid.col_gap_px * (ncols - 1);
        if (nrows > 1) sum_rows += node->grid.row_gap_px * (nrows - 1);
        // Still probe children so their measure caches populate.
        for (auto& c : node->children) measure_node(c.get(), child_avail);
        total_w = sum_cols;
        total_h = sum_rows;
        break;
    }
    case SAO_UI_LAYOUT_ABSOLUTE: {
        // Union rect of absolutely positioned children.
        int32_t max_r = 0;
        int32_t max_b = 0;
        for (auto& c : node->children) {
            SaoUiSize s = measure_node(c.get(), child_avail);
            max_r = std::max(max_r, c->spec.absolute_x_px + s.width_px);
            max_b = std::max(max_b, c->spec.absolute_y_px + s.height_px);
        }
        total_w = max_r;
        total_h = max_b;
        break;
    }
    case SAO_UI_LAYOUT_FLEX: {
        const bool row = (node->flex.direction == SAO_UI_FLEX_ROW ||
                          node->flex.direction == SAO_UI_FLEX_ROW_REVERSE);
        int32_t sum_main = 0;
        int32_t max_cross = 0;
        int32_t count = 0;
        for (auto& c : node->children) {
            SaoUiSize s = measure_node(c.get(), child_avail);
            if (row) {
                sum_main += s.width_px;
                max_cross = std::max(max_cross, s.height_px);
            } else {
                sum_main += s.height_px;
                max_cross = std::max(max_cross, s.width_px);
            }
            ++count;
        }
        const int32_t main_gap = node->flex.gap_main_px > 0
            ? node->flex.gap_main_px : gap;
        if (count > 1) sum_main += main_gap * (count - 1);
        if (row) {
            total_w = sum_main;
            total_h = max_cross;
        } else {
            total_w = max_cross;
            total_h = sum_main;
        }
        break;
    }
    case SAO_UI_LAYOUT_DOCK: {
        // Dock: each child consumes from its side; remaining is the
        // center.  We approximate preferred size as available.
        int32_t consumed_w = 0;
        int32_t consumed_h = 0;
        for (auto& c : node->children) {
            SaoUiSize s = measure_node(c.get(), child_avail);
            if (c->spec.dock_side == SAO_UI_DOCK_TOP ||
                c->spec.dock_side == SAO_UI_DOCK_BOTTOM) {
                consumed_h += s.height_px;
            } else if (c->spec.dock_side == SAO_UI_DOCK_LEFT ||
                       c->spec.dock_side == SAO_UI_DOCK_RIGHT) {
                consumed_w += s.width_px;
            }
        }
        total_w = consumed_w;
        total_h = consumed_h;
        break;
    }
    default:
        break;
    }
    // Add back insets.
    total_w += ml + mr;
    total_h += mt + mb;
    if (node->spec.fixed_width_px > 0) total_w = node->spec.fixed_width_px;
    if (node->spec.fixed_height_px > 0) total_h = node->spec.fixed_height_px;
    SaoUiSize s{total_w, total_h};
    s = apply_clamp(node->spec, s);
    node->measured = s;
    node->dirty = false;
    return s;
}

// ─── arrange recursion ─────────────────────────────────────────────

static void arrange_node(sao_ui_layout_node_s* node, SaoUiRect rect);

static void arrange_vertical(sao_ui_layout_node_s* node, SaoUiRect inner) {
    // Sum weights and fixed heights.
    float sum_weight = 0.0f;
    int32_t fixed_h = 0;
    int32_t count = 0;
    for (auto& c : node->children) {
        if (c->spec.weight > 0.0f) sum_weight += c->spec.weight;
        else fixed_h += c->measured.height_px;
        ++count;
    }
    const int32_t gap = node->spec.gap_px;
    if (count > 1) fixed_h += gap * (count - 1);
    const int32_t flex_h = std::max(0, inner.height_px - fixed_h);
    int32_t y = inner.y_px;
    for (auto& c : node->children) {
        int32_t h = c->measured.height_px;
        if (c->spec.weight > 0.0f && sum_weight > 0.0f) {
            h = static_cast<int32_t>(
                std::lround((c->spec.weight / sum_weight) * flex_h));
        }
        int32_t w = inner.width_px;
        if (c->spec.fixed_width_px > 0) w = c->spec.fixed_width_px;
        int32_t x = inner.x_px;
        // Horizontal align inside strip.
        if (c->spec.fixed_width_px > 0 || c->spec.align_h != SAO_UI_JUSTIFY_START) {
            switch (c->spec.align_h) {
            case SAO_UI_JUSTIFY_CENTER:
                x = inner.x_px + (inner.width_px - w) / 2;
                break;
            case SAO_UI_JUSTIFY_END:
                x = inner.x_px + inner.width_px - w;
                break;
            default:
                break;
            }
        }
        arrange_node(c.get(), {x, y, w, h});
        y += h + gap;
    }
}

static void arrange_horizontal(sao_ui_layout_node_s* node, SaoUiRect inner) {
    float sum_weight = 0.0f;
    int32_t fixed_w = 0;
    int32_t count = 0;
    for (auto& c : node->children) {
        if (c->spec.weight > 0.0f) sum_weight += c->spec.weight;
        else fixed_w += c->measured.width_px;
        ++count;
    }
    const int32_t gap = node->spec.gap_px;
    if (count > 1) fixed_w += gap * (count - 1);
    const int32_t flex_w = std::max(0, inner.width_px - fixed_w);
    int32_t x = inner.x_px;
    for (auto& c : node->children) {
        int32_t w = c->measured.width_px;
        if (c->spec.weight > 0.0f && sum_weight > 0.0f) {
            w = static_cast<int32_t>(
                std::lround((c->spec.weight / sum_weight) * flex_w));
        }
        int32_t h = inner.height_px;
        if (c->spec.fixed_height_px > 0) h = c->spec.fixed_height_px;
        int32_t y = inner.y_px;
        switch (c->spec.align_v) {
        case SAO_UI_ALIGN_AXIS_CENTER:
            y = inner.y_px + (inner.height_px - h) / 2;
            break;
        case SAO_UI_ALIGN_AXIS_END:
            y = inner.y_px + inner.height_px - h;
            break;
        default:
            break;
        }
        arrange_node(c.get(), {x, y, w, h});
        x += w + gap;
    }
}

static void arrange_grid(sao_ui_layout_node_s* node, SaoUiRect inner) {
    const int32_t nrows = static_cast<int32_t>(node->grid_rows_owned.size());
    const int32_t ncols = static_cast<int32_t>(node->grid_cols_owned.size());
    if (nrows <= 0 || ncols <= 0) return;
    // Column widths.
    std::vector<int32_t> col_w(ncols, 0);
    std::vector<int32_t> row_h(nrows, 0);
    const int32_t col_gap = node->grid.col_gap_px;
    const int32_t row_gap = node->grid.row_gap_px;
    int32_t fixed_cw = 0;
    float sum_col_flex = 0.0f;
    for (int32_t i = 0; i < ncols; ++i) {
        const SaoUiTrackSize& t = node->grid_cols_owned[static_cast<size_t>(i)];
        if (t.kind == SAO_UI_TRACK_FIXED || t.kind == SAO_UI_TRACK_MIN) {
            col_w[static_cast<size_t>(i)] = t.fixed_px;
            fixed_cw += t.fixed_px;
        } else if (t.kind == SAO_UI_TRACK_FLEX) {
            sum_col_flex += t.flex_weight;
        } else {
            col_w[static_cast<size_t>(i)] = 40;   // AUTO placeholder
            fixed_cw += 40;
        }
    }
    if (ncols > 1) fixed_cw += col_gap * (ncols - 1);
    const int32_t flex_cw = std::max(0, inner.width_px - fixed_cw);
    for (int32_t i = 0; i < ncols; ++i) {
        const SaoUiTrackSize& t = node->grid_cols_owned[static_cast<size_t>(i)];
        if (t.kind == SAO_UI_TRACK_FLEX && sum_col_flex > 0.0f) {
            col_w[static_cast<size_t>(i)] = static_cast<int32_t>(
                std::lround((t.flex_weight / sum_col_flex) * flex_cw));
        }
    }
    // Row heights.
    int32_t fixed_rh = 0;
    float sum_row_flex = 0.0f;
    for (int32_t i = 0; i < nrows; ++i) {
        const SaoUiTrackSize& t = node->grid_rows_owned[static_cast<size_t>(i)];
        if (t.kind == SAO_UI_TRACK_FIXED || t.kind == SAO_UI_TRACK_MIN) {
            row_h[static_cast<size_t>(i)] = t.fixed_px;
            fixed_rh += t.fixed_px;
        } else if (t.kind == SAO_UI_TRACK_FLEX) {
            sum_row_flex += t.flex_weight;
        } else {
            row_h[static_cast<size_t>(i)] = 24;
            fixed_rh += 24;
        }
    }
    if (nrows > 1) fixed_rh += row_gap * (nrows - 1);
    const int32_t flex_rh = std::max(0, inner.height_px - fixed_rh);
    for (int32_t i = 0; i < nrows; ++i) {
        const SaoUiTrackSize& t = node->grid_rows_owned[static_cast<size_t>(i)];
        if (t.kind == SAO_UI_TRACK_FLEX && sum_row_flex > 0.0f) {
            row_h[static_cast<size_t>(i)] = static_cast<int32_t>(
                std::lround((t.flex_weight / sum_row_flex) * flex_rh));
        }
    }
    // Compute cumulative offsets.
    std::vector<int32_t> col_x(ncols + 1, 0);
    col_x[0] = inner.x_px;
    for (int32_t i = 0; i < ncols; ++i) {
        col_x[static_cast<size_t>(i + 1)] =
            col_x[static_cast<size_t>(i)] +
            col_w[static_cast<size_t>(i)] +
            (i + 1 < ncols ? col_gap : 0);
    }
    std::vector<int32_t> row_y(nrows + 1, 0);
    row_y[0] = inner.y_px;
    for (int32_t i = 0; i < nrows; ++i) {
        row_y[static_cast<size_t>(i + 1)] =
            row_y[static_cast<size_t>(i)] +
            row_h[static_cast<size_t>(i)] +
            (i + 1 < nrows ? row_gap : 0);
    }
    // Place children in row-major order.  If the child count exceeds
    // rows*cols, extras are placed at (0, 0) — matches CSS "grid item
    // overflow" fallback.
    int32_t idx = 0;
    for (auto& c : node->children) {
        const int32_t r = std::min(idx / ncols, nrows - 1);
        const int32_t co = std::min(idx % ncols, ncols - 1);
        SaoUiRect cell{
            col_x[static_cast<size_t>(co)],
            row_y[static_cast<size_t>(r)],
            col_w[static_cast<size_t>(co)],
            row_h[static_cast<size_t>(r)],
        };
        arrange_node(c.get(), cell);
        ++idx;
    }
}

static void arrange_absolute(sao_ui_layout_node_s* node, SaoUiRect inner) {
    for (auto& c : node->children) {
        int32_t w = c->spec.fixed_width_px > 0
            ? c->spec.fixed_width_px : c->measured.width_px;
        int32_t h = c->spec.fixed_height_px > 0
            ? c->spec.fixed_height_px : c->measured.height_px;
        arrange_node(c.get(), {
            inner.x_px + c->spec.absolute_x_px,
            inner.y_px + c->spec.absolute_y_px,
            w, h,
        });
    }
}

static void arrange_flex(sao_ui_layout_node_s* node, SaoUiRect inner) {
    const bool row = (node->flex.direction == SAO_UI_FLEX_ROW ||
                      node->flex.direction == SAO_UI_FLEX_ROW_REVERSE);
    // Compute main-axis sizes: fixed basis first, then distribute
    // leftover by flex-grow via `spec.weight`.
    float sum_grow = 0.0f;
    int32_t fixed_main = 0;
    int32_t count = 0;
    for (auto& c : node->children) {
        const int32_t basis = row ? c->measured.width_px : c->measured.height_px;
        if (c->spec.weight > 0.0f) sum_grow += c->spec.weight;
        fixed_main += basis;
        ++count;
    }
    const int32_t main_gap = node->flex.gap_main_px > 0
        ? node->flex.gap_main_px : node->spec.gap_px;
    if (count > 1) fixed_main += main_gap * (count - 1);
    const int32_t avail_main = row ? inner.width_px : inner.height_px;
    const int32_t leftover = std::max(0, avail_main - fixed_main);
    int32_t cursor = row ? inner.x_px : inner.y_px;
    for (auto& c : node->children) {
        int32_t main_size = row ? c->measured.width_px : c->measured.height_px;
        if (c->spec.weight > 0.0f && sum_grow > 0.0f) {
            main_size += static_cast<int32_t>(
                std::lround((c->spec.weight / sum_grow) * leftover));
        }
        int32_t cross_size = row ? inner.height_px : inner.width_px;
        // Cross-axis alignment.
        int32_t cross_off = 0;
        const int32_t measured_cross = row ? c->measured.height_px : c->measured.width_px;
        switch (node->flex.align_items) {
        case SAO_UI_ALIGN_AXIS_START:
            cross_size = measured_cross;
            break;
        case SAO_UI_ALIGN_AXIS_END:
            cross_off = cross_size - measured_cross;
            cross_size = measured_cross;
            break;
        case SAO_UI_ALIGN_AXIS_CENTER:
            cross_off = (cross_size - measured_cross) / 2;
            cross_size = measured_cross;
            break;
        case SAO_UI_ALIGN_AXIS_STRETCH:
        default:
            // Keep full cross size.
            break;
        }
        SaoUiRect r;
        if (row) {
            r = {cursor, inner.y_px + cross_off, main_size, cross_size};
        } else {
            r = {inner.x_px + cross_off, cursor, cross_size, main_size};
        }
        arrange_node(c.get(), r);
        cursor += main_size + main_gap;
    }
}

static void arrange_dock(sao_ui_layout_node_s* node, SaoUiRect inner) {
    // Consume from sides in order.  Last child (or explicit CENTER)
    // fills what's left.  Matches WPF DockPanel.LastChildFill.
    SaoUiRect remaining = inner;
    sao_ui_layout_node_s* center_child = nullptr;
    for (auto& c : node->children) {
        if (c->spec.dock_side == SAO_UI_DOCK_CENTER) {
            center_child = c.get();
            continue;
        }
        const int32_t cw = c->measured.width_px;
        const int32_t ch = c->measured.height_px;
        SaoUiRect r;
        switch (c->spec.dock_side) {
        case SAO_UI_DOCK_TOP:
            r = {remaining.x_px, remaining.y_px, remaining.width_px, ch};
            remaining.y_px += ch;
            remaining.height_px -= ch;
            break;
        case SAO_UI_DOCK_BOTTOM:
            r = {remaining.x_px, remaining.y_px + remaining.height_px - ch,
                 remaining.width_px, ch};
            remaining.height_px -= ch;
            break;
        case SAO_UI_DOCK_LEFT:
            r = {remaining.x_px, remaining.y_px, cw, remaining.height_px};
            remaining.x_px += cw;
            remaining.width_px -= cw;
            break;
        case SAO_UI_DOCK_RIGHT:
            r = {remaining.x_px + remaining.width_px - cw, remaining.y_px,
                 cw, remaining.height_px};
            remaining.width_px -= cw;
            break;
        default:
            r = remaining;
            break;
        }
        if (remaining.width_px < 0) remaining.width_px = 0;
        if (remaining.height_px < 0) remaining.height_px = 0;
        arrange_node(c.get(), r);
    }
    if (center_child != nullptr) {
        arrange_node(center_child, remaining);
    } else if (node->dock.last_child_fills && !node->children.empty()) {
        // No explicit CENTER — nothing more to arrange (already done above).
        // The last-child-fills behavior is handled when a child has
        // no explicit side (defaulting to CENTER via spec default).
    }
}

static void arrange_node(sao_ui_layout_node_s* node, SaoUiRect rect) {
    // Persist arranged rect.
    node->arranged = rect;
    // Leaf: done.
    if (node->widget != nullptr || node->children.empty()) return;
    // Compute inner rect after insets.
    int32_t ml, mt, mr, mb;
    insets_of(node->spec, ml, mt, mr, mb);
    SaoUiRect inner{
        rect.x_px + ml,
        rect.y_px + mt,
        std::max(0, rect.width_px - ml - mr),
        std::max(0, rect.height_px - mt - mb),
    };
    switch (node->layout_mode) {
    case SAO_UI_LAYOUT_VERTICAL:   arrange_vertical(node, inner);   break;
    case SAO_UI_LAYOUT_HORIZONTAL: arrange_horizontal(node, inner); break;
    case SAO_UI_LAYOUT_GRID:       arrange_grid(node, inner);       break;
    case SAO_UI_LAYOUT_ABSOLUTE:   arrange_absolute(node, inner);   break;
    case SAO_UI_LAYOUT_FLEX:       arrange_flex(node, inner);       break;
    case SAO_UI_LAYOUT_DOCK:       arrange_dock(node, inner);       break;
    default: break;
    }
}

// ─── hit test recursion ────────────────────────────────────────────

static sao_ui_layout_node_s* hit_test_walk(
    sao_ui_layout_node_s* node, int32_t x, int32_t y) {
    if (node == nullptr) return nullptr;
    if (!node->spec.hit_testable) return nullptr;
    const SaoUiRect& r = node->arranged;
    if (x < r.x_px || x >= r.x_px + r.width_px) return nullptr;
    if (y < r.y_px || y >= r.y_px + r.height_px) return nullptr;
    // Test children in reverse (topmost first).
    for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
        sao_ui_layout_node_s* hit = hit_test_walk(it->get(), x, y);
        if (hit != nullptr) return hit;
    }
    return node;
}

// ─── API implementations ────────────────────────────────────────────

extern "C" void SAO_UI_CALL sao_ui_layout_spec_defaults(SaoUiLayoutSpec* out_spec) {
    if (out_spec == nullptr) return;
    std::memset(out_spec, 0, sizeof(*out_spec));
    out_spec->weight        = 0.0f;
    out_spec->flex_shrink   = 1.0f;
    out_spec->flex_basis    = 0.0f;
    out_spec->align_h       = SAO_UI_JUSTIFY_START;
    out_spec->align_v       = SAO_UI_ALIGN_AXIS_STRETCH;
    out_spec->dock_side     = SAO_UI_DOCK_FILL;
    out_spec->hit_testable  = true;
    out_spec->clip_children = false;
    out_spec->force_dirty   = false;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_tree_create(
    sao_ui_layout_tree_handle_t* out_tree) {
    if (out_tree == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_tree = nullptr;
    auto* t = new (std::nothrow) sao_ui_layout_tree_s;
    if (t == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    *out_tree = t;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_layout_tree_destroy(sao_ui_layout_tree_handle_t tree) {
    delete tree;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_tree_set_root(
    sao_ui_layout_tree_handle_t tree,
    int32_t layout_mode, const SaoUiLayoutSpec* spec,
    sao_ui_layout_node_handle_t* out_root) {
    if (tree == nullptr || spec == nullptr || out_root == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_root = nullptr;
    std::lock_guard<std::mutex> lk(tree->mu);
    if (tree->root) return SAO_STATUS_ERR_ALREADY_EXISTS;
    auto node = std::make_unique<sao_ui_layout_node_s>();
    node->layout_mode = layout_mode;
    node->spec = *spec;
    node->dirty = true;
    *out_root = node.get();
    tree->root = std::move(node);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_add_container(
    sao_ui_layout_node_handle_t parent,
    int32_t layout_mode, const SaoUiLayoutSpec* spec,
    sao_ui_layout_node_handle_t* out_child) {
    if (parent == nullptr || spec == nullptr || out_child == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_child = nullptr;
    auto node = std::make_unique<sao_ui_layout_node_s>();
    node->parent = parent;
    node->layout_mode = layout_mode;
    node->spec = *spec;
    node->dirty = true;
    *out_child = node.get();
    parent->children.push_back(std::move(node));
    mark_dirty_up(parent);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_add_widget(
    sao_ui_layout_node_handle_t parent, sao_ui_widget_handle_t widget,
    const SaoUiLayoutSpec* spec, sao_ui_layout_node_handle_t* out_leaf) {
    if (parent == nullptr || spec == nullptr || out_leaf == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_leaf = nullptr;
    auto node = std::make_unique<sao_ui_layout_node_s>();
    node->parent = parent;
    node->widget = widget;
    node->spec = *spec;
    node->dirty = true;
    *out_leaf = node.get();
    parent->children.push_back(std::move(node));
    mark_dirty_up(parent);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_remove(
    sao_ui_layout_node_handle_t node) {
    if (node == nullptr || node->parent == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto& siblings = node->parent->children;
    for (auto it = siblings.begin(); it != siblings.end(); ++it) {
        if (it->get() == node) {
            sao_ui_layout_node_s* p = node->parent;
            siblings.erase(it);
            mark_dirty_up(p);
            return SAO_STATUS_OK;
        }
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_set_spec(
    sao_ui_layout_node_handle_t node, const SaoUiLayoutSpec* spec) {
    if (node == nullptr || spec == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    node->spec = *spec;
    mark_dirty_up(node);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_set_mode_config(
    sao_ui_layout_node_handle_t container, const void* mode_config) {
    if (container == nullptr || mode_config == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    switch (container->layout_mode) {
    case SAO_UI_LAYOUT_VERTICAL:
        container->vertical = *static_cast<const SaoUiVerticalMode*>(mode_config);
        break;
    case SAO_UI_LAYOUT_HORIZONTAL:
        container->horizontal = *static_cast<const SaoUiHorizontalMode*>(mode_config);
        break;
    case SAO_UI_LAYOUT_GRID: {
        const SaoUiGridMode* gm = static_cast<const SaoUiGridMode*>(mode_config);
        container->grid = *gm;
        // Deep-copy the row/col track arrays so the caller's buffer
        // can go out of scope.
        container->grid_rows_owned.assign(gm->rows, gm->rows + gm->row_count);
        container->grid_cols_owned.assign(gm->cols, gm->cols + gm->col_count);
        // Null out the alias pointers in the owned copy to make
        // accidental use obvious.
        container->grid.rows = nullptr;
        container->grid.cols = nullptr;
        break;
    }
    case SAO_UI_LAYOUT_ABSOLUTE:
        container->absolute = *static_cast<const SaoUiAbsoluteMode*>(mode_config);
        break;
    case SAO_UI_LAYOUT_FLEX:
        container->flex = *static_cast<const SaoUiFlexMode*>(mode_config);
        break;
    case SAO_UI_LAYOUT_DOCK:
        container->dock = *static_cast<const SaoUiDockMode*>(mode_config);
        break;
    default:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    mark_dirty_up(container);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_reorder(
    sao_ui_layout_node_handle_t child, int32_t new_index) {
    if (child == nullptr || child->parent == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto& siblings = child->parent->children;
    auto it = std::find_if(siblings.begin(), siblings.end(),
        [child](const std::unique_ptr<sao_ui_layout_node_s>& p) { return p.get() == child; });
    if (it == siblings.end()) return SAO_STATUS_ERR_NOT_FOUND;
    // Extract, then insert at clamped index.
    auto owned = std::move(*it);
    siblings.erase(it);
    const int32_t clamped = clamp_i32(new_index, 0, static_cast<int32_t>(siblings.size()));
    siblings.insert(siblings.begin() + clamped, std::move(owned));
    mark_dirty_up(child->parent);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_measure(
    sao_ui_layout_node_handle_t root, SaoUiSize available,
    SaoUiSize* out_preferred) {
    if (root == nullptr || out_preferred == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_preferred = measure_node(root, available);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_arrange(
    sao_ui_layout_node_handle_t root, SaoUiRect rect) {
    if (root == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    arrange_node(root, rect);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_get_rect(
    sao_ui_layout_node_handle_t node, SaoUiRect* out_rect) {
    if (node == nullptr || out_rect == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_rect = node->arranged;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_hit_test(
    sao_ui_layout_node_handle_t root, int32_t x, int32_t y,
    SaoUiHitResult* out_hit) {
    if (root == nullptr || out_hit == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    out_hit->node = nullptr;
    out_hit->widget = nullptr;
    out_hit->local_x_px = 0;
    out_hit->local_y_px = 0;
    out_hit->depth = 0;
    sao_ui_layout_node_s* hit = hit_test_walk(root, x, y);
    if (hit == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    out_hit->node = hit;
    out_hit->widget = hit->widget;
    out_hit->local_x_px = x - hit->arranged.x_px;
    out_hit->local_y_px = y - hit->arranged.y_px;
    // Depth: walk parent chain.
    int32_t d = 0;
    for (sao_ui_layout_node_s* n = hit; n->parent != nullptr; n = n->parent) ++d;
    out_hit->depth = d;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_get_hit_path(
    sao_ui_layout_node_handle_t hit_node,
    sao_ui_layout_node_handle_t* out_path, size_t capacity,
    size_t* out_written) {
    if (hit_node == nullptr || out_written == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    size_t depth = 0;
    for (sao_ui_layout_node_s* n = hit_node; n != nullptr; n = n->parent) ++depth;
    *out_written = depth;
    if (out_path == nullptr) return SAO_STATUS_OK;
    size_t i = 0;
    for (sao_ui_layout_node_s* n = hit_node; n != nullptr && i < capacity; n = n->parent) {
        out_path[i++] = n;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_invalidate(
    sao_ui_layout_node_handle_t node) {
    if (node == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    mark_dirty_up(node);
    return SAO_STATUS_OK;
}

static void mark_subtree_dirty(sao_ui_layout_node_s* node) {
    if (node == nullptr) return;
    node->dirty = true;
    for (auto& c : node->children) mark_subtree_dirty(c.get());
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_invalidate_subtree(
    sao_ui_layout_node_handle_t node) {
    if (node == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    mark_subtree_dirty(node);
    // Also propagate up so measure() picks up the invalidation.
    if (node->parent != nullptr) mark_dirty_up(node->parent);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_tree_invalidate_all(
    sao_ui_layout_tree_handle_t tree) {
    if (tree == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(tree->mu);
    if (tree->root) mark_subtree_dirty(tree->root.get());
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_take_dirty_rects(
    sao_ui_layout_tree_handle_t tree,
    SaoUiRect* out_rects, size_t capacity, size_t* out_written) {
    if (tree == nullptr || out_written == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(tree->mu);
    const size_t n = tree->dirty_rects.size();
    *out_written = n;
    if (out_rects != nullptr) {
        const size_t copy_n = std::min(n, capacity);
        for (size_t i = 0; i < copy_n; ++i) out_rects[i] = tree->dirty_rects[i];
    }
    tree->dirty_rects.clear();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_dump_json(
    sao_ui_layout_tree_handle_t tree, uint8_t* out_utf8_buffer,
    size_t capacity, size_t* out_bytes_written) {
    if (tree == nullptr || out_bytes_written == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> lock(tree->mu);
        nlohmann::json snapshot{{"has_root", tree->root != nullptr}};
        if (tree->root != nullptr) snapshot["root"] = layout_node_json(*tree->root);
        const std::string payload = snapshot.dump(2);
        const size_t required = payload.size() + 1;
        *out_bytes_written = required;
        if (out_utf8_buffer == nullptr || capacity < required) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        std::memcpy(out_utf8_buffer, payload.c_str(), required);
        return SAO_STATUS_OK;
    } catch (...) {
        *out_bytes_written = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
