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
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

constexpr int32_t kUnbounded = 1 << 30;
constexpr int64_t kMaxLayoutI64 = std::numeric_limits<int32_t>::max();

inline int64_t nonnegative_i64(int32_t value) {
    return std::max<int64_t>(0, value);
}

inline int64_t saturating_add_nonnegative(int64_t lhs, int64_t rhs) {
    lhs = std::max<int64_t>(0, lhs);
    rhs = std::max<int64_t>(0, rhs);
    if (lhs > std::numeric_limits<int64_t>::max() - rhs)
        return std::numeric_limits<int64_t>::max();
    return lhs + rhs;
}

inline int64_t saturating_mul_nonnegative(int64_t lhs, int64_t rhs) {
    lhs = std::max<int64_t>(0, lhs);
    rhs = std::max<int64_t>(0, rhs);
    if (lhs == 0 || rhs == 0)
        return 0;
    if (lhs > std::numeric_limits<int64_t>::max() / rhs)
        return std::numeric_limits<int64_t>::max();
    return lhs * rhs;
}

inline int64_t saturating_add_i64(int64_t lhs, int64_t rhs) {
    if (rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs)
        return std::numeric_limits<int64_t>::max();
    if (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs)
        return std::numeric_limits<int64_t>::min();
    return lhs + rhs;
}

inline int32_t clamp_nonnegative_i32(int64_t value) {
    if (value <= 0)
        return 0;
    if (value > kMaxLayoutI64)
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(value);
}

inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

} // namespace

// ─── Node ────────────────────────────────────────────────────────────
//
// The layout tree is a plain owned tree — parent holds unique_ptr to
// children.  A node is either a container (has `layout_mode`) or a
// leaf widget (has `widget_handle`, no children).  Absolute mode
// children override rect via their spec's absolute_x/y.

struct sao_ui_layout_node_s {
    // Identity.
    sao_ui_layout_node_s* parent = nullptr;
    sao_ui_layout_tree_s* tree = nullptr;
    std::vector<std::unique_ptr<sao_ui_layout_node_s>> children;
    // Config.
    int32_t layout_mode = SAO_UI_LAYOUT_VERTICAL;
    SaoUiLayoutSpec spec{};
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
    SaoUiSize measured_available{0, 0};
    bool measure_cache_valid = false;
    SaoUiRect arranged{0, 0, 0, 0};
    bool dirty = true;
    bool paint_dirty = true;
};

struct sao_ui_layout_tree_s {
    std::mutex mu;
    std::unique_ptr<sao_ui_layout_node_s> root;
    // Dirty-rect ledger populated during arrange for nodes that
    // changed rect since the last take.
    std::vector<SaoUiRect> dirty_rects;
};

namespace {

bool valid_layout_mode(int32_t mode) {
    return mode >= SAO_UI_LAYOUT_VERTICAL && mode <= SAO_UI_LAYOUT_DOCK;
}

const char* layout_mode_name(int32_t mode) {
    switch (mode) {
    case SAO_UI_LAYOUT_VERTICAL:
        return "vertical";
    case SAO_UI_LAYOUT_HORIZONTAL:
        return "horizontal";
    case SAO_UI_LAYOUT_GRID:
        return "grid";
    case SAO_UI_LAYOUT_ABSOLUTE:
        return "absolute";
    case SAO_UI_LAYOUT_FLEX:
        return "flex";
    case SAO_UI_LAYOUT_DOCK:
        return "dock";
    default:
        return "unknown";
    }
}

nlohmann::json layout_node_json(const sao_ui_layout_node_s& node) {
    nlohmann::json children = nlohmann::json::array();
    for (const auto& child : node.children)
        children.push_back(layout_node_json(*child));
    return {
        {"mode", layout_mode_name(node.layout_mode)},
        {"widget", reinterpret_cast<uintptr_t>(node.widget)},
        {"dirty", node.dirty},
        {"measured",
         {
             {"width", node.measured.width_px},
             {"height", node.measured.height_px},
         }},
        {"rect",
         {
             {"x", node.arranged.x_px},
             {"y", node.arranged.y_px},
             {"width", node.arranged.width_px},
             {"height", node.arranged.height_px},
         }},
        {"spec",
         {
             {"padding",
              {node.spec.pad_top_px, node.spec.pad_right_px, node.spec.pad_bottom_px,
               node.spec.pad_left_px}},
             {"margin",
              {node.spec.margin_top_px, node.spec.margin_right_px, node.spec.margin_bottom_px,
               node.spec.margin_left_px}},
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

} // namespace

// ─── helpers ─────────────────────────────────────────────────────────

static void mark_dirty_up(sao_ui_layout_node_s* node) {
    while (node != nullptr) {
        node->dirty = true;
        node->paint_dirty = true;
        node->measure_cache_valid = false;
        node = node->parent;
    }
}

static bool rect_has_area(const SaoUiRect& rect) {
    return rect.width_px > 0 && rect.height_px > 0;
}

static bool rect_equal(const SaoUiRect& left, const SaoUiRect& right) {
    return left.x_px == right.x_px && left.y_px == right.y_px && left.width_px == right.width_px &&
           left.height_px == right.height_px;
}

static void record_dirty_rect(sao_ui_layout_node_s* node, const SaoUiRect& rect) {
    if (node->tree != nullptr && rect_has_area(rect))
        node->tree->dirty_rects.push_back(rect);
}

static SaoUiSize widget_size_hint(const sao_ui_layout_node_s* node, SaoUiSize available) {
    int64_t width = 100;
    int64_t height = 24;
    if (node->widget != nullptr) {
        SaoUiWidgetSizeHint widget_hint{};
        if (sao_ui_widget_get_size_hint(node->widget, std::max(0, available.width_px),
                                        std::max(0, available.height_px),
                                        &widget_hint) == SAO_STATUS_OK) {
            width = std::max(nonnegative_i64(widget_hint.min_width_px),
                             nonnegative_i64(widget_hint.preferred_width_px));
            height = std::max(nonnegative_i64(widget_hint.min_height_px),
                              nonnegative_i64(widget_hint.preferred_height_px));
            if (widget_hint.max_width_px > 0)
                width = std::min(width, nonnegative_i64(widget_hint.max_width_px));
            if (widget_hint.max_height_px > 0)
                height = std::min(height, nonnegative_i64(widget_hint.max_height_px));
        }
    }
    if (node->spec.fixed_width_px > 0)
        width = node->spec.fixed_width_px;
    if (node->spec.fixed_height_px > 0)
        height = node->spec.fixed_height_px;
    return {clamp_nonnegative_i32(width), clamp_nonnegative_i32(height)};
}

static SaoUiSize apply_clamp(const SaoUiLayoutSpec& spec, SaoUiSize s) {
    int64_t width = std::max<int64_t>(0, s.width_px);
    int64_t height = std::max<int64_t>(0, s.height_px);
    if (spec.min_width_px > 0)
        width = std::max(width, nonnegative_i64(spec.min_width_px));
    if (spec.min_height_px > 0)
        height = std::max(height, nonnegative_i64(spec.min_height_px));
    if (spec.max_width_px > 0)
        width = std::min(width, nonnegative_i64(spec.max_width_px));
    if (spec.max_height_px > 0)
        height = std::min(height, nonnegative_i64(spec.max_height_px));
    return {clamp_nonnegative_i32(width), clamp_nonnegative_i32(height)};
}

static void insets_of(const SaoUiLayoutSpec& spec, int64_t& l, int64_t& t, int64_t& r, int64_t& b) {
    l = saturating_add_nonnegative(nonnegative_i64(spec.margin_left_px),
                                   nonnegative_i64(spec.pad_left_px));
    t = saturating_add_nonnegative(nonnegative_i64(spec.margin_top_px),
                                   nonnegative_i64(spec.pad_top_px));
    r = saturating_add_nonnegative(nonnegative_i64(spec.margin_right_px),
                                   nonnegative_i64(spec.pad_right_px));
    b = saturating_add_nonnegative(nonnegative_i64(spec.margin_bottom_px),
                                   nonnegative_i64(spec.pad_bottom_px));
}

inline bool finite_positive_weight(float value) noexcept {
    return std::isfinite(value) && value > 0.0F;
}

static std::vector<int32_t> allocate_integer_weight_shares(int32_t total,
                                                           const std::vector<double>& weights) {
    const int32_t budget = std::max(0, total);
    std::vector<int32_t> shares(weights.size(), 0);
    double weight_scale = 0.0;
    for (const double weight : weights) {
        if (std::isfinite(weight) && weight > weight_scale)
            weight_scale = weight;
    }
    if (!(weight_scale > 0.0) || budget == 0)
        return shares;

    double weight_sum = 0.0;
    for (const double weight : weights) {
        if (std::isfinite(weight) && weight > 0.0)
            weight_sum += weight / weight_scale;
    }
    if (!(weight_sum > 0.0) || !std::isfinite(weight_sum))
        return shares;

    std::vector<double> remainders(weights.size(), -1.0);
    int64_t assigned = 0;
    for (size_t index = 0; index < weights.size(); ++index) {
        const double weight = weights[index];
        if (!std::isfinite(weight) || weight <= 0.0)
            continue;
        const double exact = static_cast<double>(budget) * (weight / weight_scale) / weight_sum;
        const int32_t share = static_cast<int32_t>(std::floor(exact));
        shares[index] = share;
        assigned += share;
        remainders[index] = exact - static_cast<double>(share);
    }

    int32_t leftover = budget - clamp_nonnegative_i32(assigned);
    while (leftover > 0) {
        size_t best = weights.size();
        for (size_t index = 0; index < remainders.size(); ++index) {
            if (remainders[index] < 0.0)
                continue;
            if (best == weights.size() || remainders[index] > remainders[best])
                best = index;
        }
        if (best == weights.size())
            break;
        ++shares[best];
        remainders[best] = -1.0;
        --leftover;
    }
    return shares;
}

static void compress_sizes_to_budget(std::vector<int32_t>& sizes, int32_t total) {
    const int32_t budget = std::max(0, total);
    int64_t size_sum = 0;
    for (const int32_t size : sizes)
        size_sum = saturating_add_nonnegative(size_sum, size);
    if (size_sum <= budget || size_sum == 0)
        return;

    std::vector<int64_t> remainders(sizes.size(), 0);
    int64_t assigned = 0;
    for (size_t index = 0; index < sizes.size(); ++index) {
        const int64_t numerator = static_cast<int64_t>(std::max(0, sizes[index])) * budget;
        sizes[index] = static_cast<int32_t>(numerator / size_sum);
        remainders[index] = numerator % size_sum;
        assigned += sizes[index];
    }
    int32_t leftover = budget - clamp_nonnegative_i32(assigned);
    while (leftover > 0 && !remainders.empty()) {
        size_t best = 0;
        for (size_t index = 1; index < remainders.size(); ++index) {
            if (remainders[index] > remainders[best])
                best = index;
        }
        ++sizes[best];
        remainders[best] = -1;
        --leftover;
    }
}

static std::vector<int32_t>
allocate_weighted_main_sizes(const std::vector<std::unique_ptr<sao_ui_layout_node_s>>& children,
                             int32_t available, int32_t requested_gap, bool vertical,
                             int32_t* out_gap) {
    const int32_t count = static_cast<int32_t>(children.size());
    const int32_t nonnegative_available = std::max(0, available);
    const int32_t gap = count > 1 ? std::min(std::max(0, requested_gap),
                                             nonnegative_available / std::max(1, count - 1))
                                  : 0;
    if (out_gap != nullptr)
        *out_gap = gap;

    std::vector<int32_t> sizes(static_cast<size_t>(count), 0);
    std::vector<int32_t> maximums(static_cast<size_t>(count), kUnbounded);
    std::vector<double> weights(static_cast<size_t>(count), 0.0);
    int64_t base_sum = 0;
    for (int32_t index = 0; index < count; ++index) {
        const auto& child = children[static_cast<size_t>(index)];
        const auto& spec = child->spec;
        const int32_t measured = vertical ? child->measured.height_px : child->measured.width_px;
        const int32_t minimum = std::max(0, vertical ? spec.min_height_px : spec.min_width_px);
        const int32_t maximum =
            (vertical ? spec.max_height_px : spec.max_width_px) > 0
                ? std::max(minimum, vertical ? spec.max_height_px : spec.max_width_px)
                : kUnbounded;
        maximums[static_cast<size_t>(index)] = maximum;
        const int32_t fixed = vertical ? spec.fixed_height_px : spec.fixed_width_px;
        const bool weighted = finite_positive_weight(spec.weight) && fixed <= 0;
        weights[static_cast<size_t>(index)] = weighted ? static_cast<double>(spec.weight) : 0.0;
        const int32_t base = fixed > 0  ? clamp_i32(fixed, minimum, maximum)
                             : weighted ? minimum
                                        : clamp_i32(measured, minimum, maximum);
        sizes[static_cast<size_t>(index)] = base;
        base_sum += base;
    }

    const int64_t gap_total = static_cast<int64_t>(gap) * std::max(0, count - 1);
    const int32_t budget = static_cast<int32_t>(
        std::max<int64_t>(0, static_cast<int64_t>(nonnegative_available) - gap_total));
    if (base_sum > budget && base_sum > 0) {
        int64_t compressed_sum = 0;
        std::vector<int64_t> remainders(static_cast<size_t>(count), 0);
        for (int32_t index = 0; index < count; ++index) {
            const int64_t numerator = static_cast<int64_t>(sizes[static_cast<size_t>(index)]) *
                                      static_cast<int64_t>(budget);
            sizes[static_cast<size_t>(index)] = static_cast<int32_t>(numerator / base_sum);
            remainders[static_cast<size_t>(index)] = numerator % base_sum;
            compressed_sum += sizes[static_cast<size_t>(index)];
        }
        int32_t leftover = budget - static_cast<int32_t>(compressed_sum);
        while (leftover > 0) {
            size_t best = 0;
            for (size_t index = 1; index < remainders.size(); ++index) {
                if (remainders[index] > remainders[best])
                    best = index;
            }
            ++sizes[best];
            remainders[best] = -1;
            --leftover;
        }
        return sizes;
    }

    int32_t remaining = budget - static_cast<int32_t>(base_sum);
    std::vector<size_t> active;
    for (size_t index = 0; index < sizes.size(); ++index) {
        if (weights[index] > 0.0 && sizes[index] < maximums[index])
            active.push_back(index);
    }
    while (remaining > 0 && !active.empty()) {
        double weight_sum = 0.0;
        for (const size_t index : active)
            weight_sum += weights[index];
        if (!(weight_sum > 0.0))
            break;

        bool saturated = false;
        for (auto it = active.begin(); it != active.end();) {
            const size_t index = *it;
            const int32_t capacity = maximums[index] - sizes[index];
            const int32_t share = static_cast<int32_t>(
                std::floor(static_cast<double>(remaining) * weights[index] / weight_sum));
            if (capacity <= share && capacity >= 0) {
                sizes[index] += capacity;
                remaining -= capacity;
                it = active.erase(it);
                saturated = true;
            } else {
                ++it;
            }
        }
        if (saturated)
            continue;

        std::vector<double> remainders(sizes.size(), -1.0);
        int32_t assigned = 0;
        for (const size_t index : active) {
            const double exact = static_cast<double>(remaining) * weights[index] / weight_sum;
            const int32_t share = static_cast<int32_t>(std::floor(exact));
            sizes[index] += share;
            assigned += share;
            remainders[index] = exact - static_cast<double>(share);
        }
        int32_t leftover = remaining - assigned;
        while (leftover > 0) {
            size_t best = active.front();
            for (const size_t index : active) {
                if (remainders[index] > remainders[best])
                    best = index;
            }
            ++sizes[best];
            remainders[best] = -1.0;
            --leftover;
        }
        remaining = 0;
    }
    return sizes;
}

static int32_t flex_basis_for_child(const sao_ui_layout_node_s& child, bool row) {
    const int32_t measured = std::max(0, row ? child.measured.width_px : child.measured.height_px);
    int64_t basis = measured;
    if (std::isfinite(child.spec.flex_basis) && child.spec.flex_basis > 0.0F)
        basis = static_cast<int64_t>(std::llround(child.spec.flex_basis));
    const int32_t minimum = std::max(0, row ? child.spec.min_width_px : child.spec.min_height_px);
    const int32_t maximum =
        (row ? child.spec.max_width_px : child.spec.max_height_px) > 0
            ? std::max(minimum, row ? child.spec.max_width_px : child.spec.max_height_px)
            : kUnbounded;
    return clamp_i32(clamp_nonnegative_i32(basis), minimum, maximum);
}

static int32_t track_capped_size(const SaoUiTrackSize& track, int32_t size) {
    const int32_t nonnegative = std::max(0, size);
    return track.max_px > 0 ? std::min(nonnegative, track.max_px) : nonnegative;
}

// ─── measure recursion ──────────────────────────────────────────────

static SaoUiSize measure_node(sao_ui_layout_node_s* node, SaoUiSize available) {
    if (!node->dirty && node->measure_cache_valid &&
        node->measured_available.width_px == available.width_px &&
        node->measured_available.height_px == available.height_px) {
        return node->measured;
    }
    if (node->widget != nullptr || node->children.empty()) {
        SaoUiSize s = widget_size_hint(node, available);
        if (node->spec.fixed_width_px > 0)
            s.width_px = node->spec.fixed_width_px;
        if (node->spec.fixed_height_px > 0)
            s.height_px = node->spec.fixed_height_px;
        s = apply_clamp(node->spec, s);
        node->measured = s;
        node->measured_available = available;
        node->measure_cache_valid = true;
        node->dirty = false;
        return s;
    }

    int64_t ml, mt, mr, mb;
    insets_of(node->spec, ml, mt, mr, mb);
    const int64_t inner_w = std::max<int64_t>(0, nonnegative_i64(available.width_px) - ml - mr);
    const int64_t inner_h = std::max<int64_t>(0, nonnegative_i64(available.height_px) - mt - mb);
    const SaoUiSize child_avail{clamp_nonnegative_i32(inner_w), clamp_nonnegative_i32(inner_h)};
    int64_t total_w = 0;
    int64_t total_h = 0;
    const int64_t gap = nonnegative_i64(node->spec.gap_px);

    switch (node->layout_mode) {
    case SAO_UI_LAYOUT_VERTICAL: {
        int64_t max_child_w = 0;
        int64_t sum_h = 0;
        size_t count = 0;
        for (auto& c : node->children) {
            const SaoUiSize s = measure_node(c.get(), child_avail);
            max_child_w = std::max(max_child_w, static_cast<int64_t>(s.width_px));
            sum_h = saturating_add_nonnegative(sum_h, s.height_px);
            ++count;
        }
        if (count > 1) {
            sum_h = saturating_add_nonnegative(
                sum_h, saturating_mul_nonnegative(gap, static_cast<int64_t>(count - 1)));
        }
        total_w = max_child_w;
        total_h = sum_h;
        break;
    }
    case SAO_UI_LAYOUT_HORIZONTAL: {
        int64_t sum_w = 0;
        int64_t max_child_h = 0;
        size_t count = 0;
        for (auto& c : node->children) {
            const SaoUiSize s = measure_node(c.get(), child_avail);
            sum_w = saturating_add_nonnegative(sum_w, s.width_px);
            max_child_h = std::max(max_child_h, static_cast<int64_t>(s.height_px));
            ++count;
        }
        if (count > 1) {
            sum_w = saturating_add_nonnegative(
                sum_w, saturating_mul_nonnegative(gap, static_cast<int64_t>(count - 1)));
        }
        total_w = sum_w;
        total_h = max_child_h;
        break;
    }
    case SAO_UI_LAYOUT_GRID: {
        const auto track_nominal = [](const SaoUiTrackSize& track, int64_t auto_px) {
            if (track.kind == SAO_UI_TRACK_FIXED || track.kind == SAO_UI_TRACK_MIN)
                return track_capped_size(track, clamp_nonnegative_i32(track.fixed_px));
            if (track.kind == SAO_UI_TRACK_FLEX) {
                if (!std::isfinite(track.flex_weight) || track.flex_weight <= 0.0f)
                    return track_capped_size(track, 0);
                const long double scaled = static_cast<long double>(track.flex_weight) * 20.0L;
                if (scaled >= static_cast<long double>(std::numeric_limits<int64_t>::max()))
                    return track_capped_size(track, std::numeric_limits<int32_t>::max());
                return track_capped_size(track,
                                         clamp_nonnegative_i32(static_cast<int64_t>(scaled)));
            }
            return track_capped_size(track, clamp_nonnegative_i32(auto_px));
        };
        int64_t sum_cols = 0;
        for (const SaoUiTrackSize& track : node->grid_cols_owned)
            sum_cols = saturating_add_nonnegative(sum_cols, track_nominal(track, 40));
        int64_t sum_rows = 0;
        for (const SaoUiTrackSize& track : node->grid_rows_owned)
            sum_rows = saturating_add_nonnegative(sum_rows, track_nominal(track, 24));
        const size_t ncols = node->grid_cols_owned.size();
        const size_t nrows = node->grid_rows_owned.size();
        if (ncols > 1) {
            sum_cols = saturating_add_nonnegative(
                sum_cols, saturating_mul_nonnegative(nonnegative_i64(node->grid.col_gap_px),
                                                     static_cast<int64_t>(ncols - 1)));
        }
        if (nrows > 1) {
            sum_rows = saturating_add_nonnegative(
                sum_rows, saturating_mul_nonnegative(nonnegative_i64(node->grid.row_gap_px),
                                                     static_cast<int64_t>(nrows - 1)));
        }
        for (auto& c : node->children)
            measure_node(c.get(), child_avail);
        total_w = sum_cols;
        total_h = sum_rows;
        break;
    }
    case SAO_UI_LAYOUT_ABSOLUTE: {
        int64_t max_r = 0;
        int64_t max_b = 0;
        for (auto& c : node->children) {
            const SaoUiSize s = measure_node(c.get(), child_avail);
            max_r = std::max(max_r, saturating_add_i64(c->spec.absolute_x_px, s.width_px));
            max_b = std::max(max_b, saturating_add_i64(c->spec.absolute_y_px, s.height_px));
        }
        total_w = max_r;
        total_h = max_b;
        break;
    }
    case SAO_UI_LAYOUT_FLEX: {
        const bool row = node->flex.direction == SAO_UI_FLEX_ROW ||
                         node->flex.direction == SAO_UI_FLEX_ROW_REVERSE;
        const bool wrap = node->flex.wrap != SAO_UI_FLEX_NOWRAP;
        const int32_t available_main =
            std::max(0, row ? child_avail.width_px : child_avail.height_px);
        const int32_t main_gap =
            node->flex.gap_main_px > 0 ? node->flex.gap_main_px : clamp_nonnegative_i32(gap);
        int64_t measured_main = 0;
        int64_t measured_cross = 0;
        int64_t line_main = 0;
        int64_t line_cross = 0;
        size_t line_count = 0;
        for (auto& c : node->children) {
            measure_node(c.get(), child_avail);
            const int32_t basis = flex_basis_for_child(*c, row);
            const int32_t cross = std::max(0, row ? c->measured.height_px : c->measured.width_px);
            const int64_t candidate = line_main + (line_count == 0 ? 0 : main_gap) + basis;
            if (wrap && line_count > 0 && candidate > available_main) {
                measured_main = std::max(measured_main, line_main);
                measured_cross += line_cross;
                if (measured_cross > 0)
                    measured_cross += std::max(0, node->flex.gap_cross_px);
                line_main = basis;
                line_cross = cross;
                line_count = 1;
            } else {
                line_main = candidate;
                line_cross = std::max(line_cross, static_cast<int64_t>(cross));
                ++line_count;
            }
        }
        if (line_count > 0) {
            measured_main = std::max(measured_main, line_main);
            measured_cross += line_cross;
        }
        if (wrap && row) {
            total_w = measured_main;
            total_h = measured_cross;
        } else if (wrap) {
            total_w = measured_cross;
            total_h = measured_main;
        } else if (row) {
            total_w = line_main;
            total_h = line_cross;
        } else {
            total_w = line_cross;
            total_h = line_main;
        }
        break;
    }
    case SAO_UI_LAYOUT_DOCK: {
        int64_t consumed_w = 0;
        int64_t consumed_h = 0;
        int64_t max_top_bottom_w = 0;
        int64_t max_left_right_h = 0;
        int64_t center_w = 0;
        int64_t center_h = 0;
        int64_t remaining_w = inner_w;
        int64_t remaining_h = inner_h;
        for (auto& c : node->children) {
            const SaoUiSize s = measure_node(
                c.get(), {clamp_nonnegative_i32(remaining_w), clamp_nonnegative_i32(remaining_h)});
            switch (c->spec.dock_side) {
            case SAO_UI_DOCK_TOP:
            case SAO_UI_DOCK_BOTTOM:
                consumed_h = saturating_add_nonnegative(consumed_h, s.height_px);
                max_top_bottom_w = std::max(max_top_bottom_w, static_cast<int64_t>(s.width_px));
                remaining_h = std::max<int64_t>(0, remaining_h - s.height_px);
                break;
            case SAO_UI_DOCK_LEFT:
            case SAO_UI_DOCK_RIGHT:
                consumed_w = saturating_add_nonnegative(consumed_w, s.width_px);
                max_left_right_h = std::max(max_left_right_h, static_cast<int64_t>(s.height_px));
                remaining_w = std::max<int64_t>(0, remaining_w - s.width_px);
                break;
            case SAO_UI_DOCK_CENTER:
                center_w = std::max(center_w, static_cast<int64_t>(s.width_px));
                center_h = std::max(center_h, static_cast<int64_t>(s.height_px));
                break;
            default:
                break;
            }
        }
        total_w = std::max(max_top_bottom_w, saturating_add_nonnegative(consumed_w, center_w));
        total_h = std::max(max_left_right_h, saturating_add_nonnegative(consumed_h, center_h));
        break;
    }
    default:
        break;
    }

    total_w = saturating_add_nonnegative(total_w, ml);
    total_w = saturating_add_nonnegative(total_w, mr);
    total_h = saturating_add_nonnegative(total_h, mt);
    total_h = saturating_add_nonnegative(total_h, mb);
    if (node->spec.fixed_width_px > 0)
        total_w = nonnegative_i64(node->spec.fixed_width_px);
    if (node->spec.fixed_height_px > 0)
        total_h = nonnegative_i64(node->spec.fixed_height_px);
    SaoUiSize s{clamp_nonnegative_i32(total_w), clamp_nonnegative_i32(total_h)};
    s = apply_clamp(node->spec, s);
    node->measured = s;
    node->measured_available = available;
    node->measure_cache_valid = true;
    node->dirty = false;
    return s;
}
// ─── arrange recursion ─────────────────────────────────────────────

static SaoUiRect make_nonnegative_rect(int64_t x, int64_t y, int64_t width, int64_t height) {
    return {clamp_nonnegative_i32(x), clamp_nonnegative_i32(y), clamp_nonnegative_i32(width),
            clamp_nonnegative_i32(height)};
}

static SaoUiRect make_rect_within(const SaoUiRect& bounds, int64_t x, int64_t y, int64_t width,
                                  int64_t height) {
    const int64_t bounds_x = bounds.x_px;
    const int64_t bounds_y = bounds.y_px;
    const int64_t bounds_w = nonnegative_i64(bounds.width_px);
    const int64_t bounds_h = nonnegative_i64(bounds.height_px);
    const int64_t bounds_right = saturating_add_i64(bounds_x, bounds_w);
    const int64_t bounds_bottom = saturating_add_i64(bounds_y, bounds_h);
    const int64_t clamped_x = std::min(std::max(x, bounds_x), bounds_right);
    const int64_t clamped_y = std::min(std::max(y, bounds_y), bounds_bottom);
    const int64_t clamped_width = std::min(std::max<int64_t>(0, width), bounds_right - clamped_x);
    const int64_t clamped_height =
        std::min(std::max<int64_t>(0, height), bounds_bottom - clamped_y);
    return make_nonnegative_rect(clamped_x, clamped_y, clamped_width, clamped_height);
}

static void arrange_node(sao_ui_layout_node_s* node, SaoUiRect rect);

static void arrange_vertical(sao_ui_layout_node_s* node, SaoUiRect inner) {
    int32_t gap = 0;
    auto sizes = allocate_weighted_main_sizes(node->children, inner.height_px, node->spec.gap_px,
                                              true, &gap);
    if (node->vertical.fill_available && !sizes.empty()) {
        int64_t used = static_cast<int64_t>(gap) *
                       static_cast<int64_t>(sizes.size() > 1 ? sizes.size() - 1U : 0U);
        for (const int32_t size : sizes)
            used += size;
        sizes.back() += clamp_nonnegative_i32(std::max<int64_t>(0, inner.height_px - used));
    }
    int64_t cursor =
        node->vertical.reverse ? static_cast<int64_t>(inner.y_px) + inner.height_px : inner.y_px;
    for (size_t index = 0; index < node->children.size(); ++index) {
        auto& child = node->children[index];
        const int32_t height = std::max(0, sizes[index]);
        int32_t width = inner.width_px;
        if (child->spec.fixed_width_px > 0)
            width = child->spec.fixed_width_px;
        width = std::min(clamp_i32(width, std::max(0, child->spec.min_width_px),
                                   child->spec.max_width_px > 0 ? std::max(child->spec.min_width_px,
                                                                           child->spec.max_width_px)
                                                                : kUnbounded),
                         std::max(0, inner.width_px));
        int64_t x = inner.x_px;
        if (child->spec.align_h == SAO_UI_JUSTIFY_CENTER)
            x += (static_cast<int64_t>(inner.width_px) - width) / 2;
        else if (child->spec.align_h == SAO_UI_JUSTIFY_END)
            x += static_cast<int64_t>(inner.width_px) - width;
        int64_t y = node->vertical.reverse ? cursor - height : cursor;
        arrange_node(child.get(), make_rect_within(inner, x, y, width, height));
        cursor += node->vertical.reverse ? -static_cast<int64_t>(height + gap)
                                         : static_cast<int64_t>(height + gap);
    }
}
static void arrange_horizontal(sao_ui_layout_node_s* node, SaoUiRect inner) {
    int32_t gap = 0;
    auto sizes = allocate_weighted_main_sizes(node->children, inner.width_px, node->spec.gap_px,
                                              false, &gap);
    if (node->horizontal.fill_available && !sizes.empty()) {
        int64_t used = static_cast<int64_t>(gap) *
                       static_cast<int64_t>(sizes.size() > 1 ? sizes.size() - 1U : 0U);
        for (const int32_t size : sizes)
            used += size;
        sizes.back() += clamp_nonnegative_i32(std::max<int64_t>(0, inner.width_px - used));
    }
    int64_t cursor =
        node->horizontal.reverse ? static_cast<int64_t>(inner.x_px) + inner.width_px : inner.x_px;
    for (size_t index = 0; index < node->children.size(); ++index) {
        auto& child = node->children[index];
        const int32_t width = std::max(0, sizes[index]);
        int32_t height = inner.height_px;
        if (child->spec.fixed_height_px > 0)
            height = child->spec.fixed_height_px;
        height =
            std::min(clamp_i32(height, std::max(0, child->spec.min_height_px),
                               child->spec.max_height_px > 0
                                   ? std::max(child->spec.min_height_px, child->spec.max_height_px)
                                   : kUnbounded),
                     std::max(0, inner.height_px));
        int64_t y = inner.y_px;
        if (child->spec.align_v == SAO_UI_ALIGN_AXIS_CENTER)
            y += (static_cast<int64_t>(inner.height_px) - height) / 2;
        else if (child->spec.align_v == SAO_UI_ALIGN_AXIS_END)
            y += static_cast<int64_t>(inner.height_px) - height;
        int64_t x = node->horizontal.reverse ? cursor - width : cursor;
        arrange_node(child.get(), make_rect_within(inner, x, y, width, height));
        cursor += node->horizontal.reverse ? -static_cast<int64_t>(width + gap)
                                           : static_cast<int64_t>(width + gap);
    }
}
static void arrange_grid(sao_ui_layout_node_s* node, SaoUiRect inner) {
    const int32_t nrows = static_cast<int32_t>(node->grid_rows_owned.size());
    const int32_t ncols = static_cast<int32_t>(node->grid_cols_owned.size());
    if (nrows <= 0 || ncols <= 0)
        return;
    std::vector<int32_t> col_w(ncols, 0), row_h(nrows, 0);
    std::vector<double> col_weights(static_cast<size_t>(ncols), 0.0),
        row_weights(static_cast<size_t>(nrows), 0.0);
    const int32_t col_gap = ncols > 1 ? clamp_nonnegative_i32(std::min<int64_t>(
                                            nonnegative_i64(node->grid.col_gap_px),
                                            std::max<int64_t>(0, inner.width_px) / (ncols - 1)))
                                      : 0;
    const int32_t row_gap = nrows > 1 ? clamp_nonnegative_i32(std::min<int64_t>(
                                            nonnegative_i64(node->grid.row_gap_px),
                                            std::max<int64_t>(0, inner.height_px) / (nrows - 1)))
                                      : 0;
    int64_t fixed_cw = 0;
    for (int32_t i = 0; i < ncols; ++i) {
        const auto& track = node->grid_cols_owned[static_cast<size_t>(i)];
        if (track.kind == SAO_UI_TRACK_FLEX) {
            if (finite_positive_weight(track.flex_weight))
                col_weights[static_cast<size_t>(i)] = track.flex_weight;
        } else {
            col_w[static_cast<size_t>(i)] =
                track_capped_size(track, track.kind == SAO_UI_TRACK_AUTO ? 40 : track.fixed_px);
            fixed_cw += col_w[static_cast<size_t>(i)];
        }
    }
    const int32_t col_budget = clamp_nonnegative_i32(std::max<int64_t>(
        0, static_cast<int64_t>(inner.width_px) - static_cast<int64_t>(col_gap) * (ncols - 1)));
    if (fixed_cw > col_budget)
        compress_sizes_to_budget(col_w, col_budget);
    else {
        const auto shares = allocate_integer_weight_shares(
            std::max(0, col_budget - static_cast<int32_t>(fixed_cw)), col_weights);
        for (int32_t i = 0; i < ncols; ++i)
            if (node->grid_cols_owned[static_cast<size_t>(i)].kind == SAO_UI_TRACK_FLEX)
                col_w[static_cast<size_t>(i)] = track_capped_size(
                    node->grid_cols_owned[static_cast<size_t>(i)], shares[static_cast<size_t>(i)]);
    }
    int64_t fixed_rh = 0;
    for (int32_t i = 0; i < nrows; ++i) {
        const auto& track = node->grid_rows_owned[static_cast<size_t>(i)];
        if (track.kind == SAO_UI_TRACK_FLEX) {
            if (finite_positive_weight(track.flex_weight))
                row_weights[static_cast<size_t>(i)] = track.flex_weight;
        } else {
            row_h[static_cast<size_t>(i)] =
                track_capped_size(track, track.kind == SAO_UI_TRACK_AUTO ? 24 : track.fixed_px);
            fixed_rh += row_h[static_cast<size_t>(i)];
        }
    }
    const int32_t row_budget = clamp_nonnegative_i32(std::max<int64_t>(
        0, static_cast<int64_t>(inner.height_px) - static_cast<int64_t>(row_gap) * (nrows - 1)));
    if (fixed_rh > row_budget)
        compress_sizes_to_budget(row_h, row_budget);
    else {
        const auto shares = allocate_integer_weight_shares(
            std::max(0, row_budget - static_cast<int32_t>(fixed_rh)), row_weights);
        for (int32_t i = 0; i < nrows; ++i)
            if (node->grid_rows_owned[static_cast<size_t>(i)].kind == SAO_UI_TRACK_FLEX)
                row_h[static_cast<size_t>(i)] = track_capped_size(
                    node->grid_rows_owned[static_cast<size_t>(i)], shares[static_cast<size_t>(i)]);
    }
    std::vector<int64_t> col_x(static_cast<size_t>(ncols) + 1U, inner.x_px),
        row_y(static_cast<size_t>(nrows) + 1U, inner.y_px);
    for (int32_t i = 0; i < ncols; ++i)
        col_x[static_cast<size_t>(i + 1)] = col_x[static_cast<size_t>(i)] +
                                            col_w[static_cast<size_t>(i)] +
                                            (i + 1 < ncols ? col_gap : 0);
    for (int32_t i = 0; i < nrows; ++i)
        row_y[static_cast<size_t>(i + 1)] = row_y[static_cast<size_t>(i)] +
                                            row_h[static_cast<size_t>(i)] +
                                            (i + 1 < nrows ? row_gap : 0);
    std::vector<bool> occupied(static_cast<size_t>(nrows) * static_cast<size_t>(ncols), false);
    size_t cursor = 0;
    // Layout nodes expose no placement metadata; spans remain unsupported and placement is
    // single-cell.
    for (auto& child : node->children) {
        while (node->grid.dense_packing && cursor < occupied.size() && occupied[cursor])
            ++cursor;
        const size_t cell = std::min(cursor, occupied.size() - 1U);
        occupied[cell] = true;
        const int32_t row = static_cast<int32_t>(cell / static_cast<size_t>(ncols));
        const int32_t col = static_cast<int32_t>(cell % static_cast<size_t>(ncols));
        arrange_node(child.get(), make_rect_within(inner, col_x[static_cast<size_t>(col)],
                                                   row_y[static_cast<size_t>(row)],
                                                   col_w[static_cast<size_t>(col)],
                                                   row_h[static_cast<size_t>(row)]));
        ++cursor;
    }
}

static void arrange_absolute(sao_ui_layout_node_s* node, SaoUiRect inner) {
    for (auto& child : node->children) {
        const int32_t width =
            child->spec.fixed_width_px > 0 ? child->spec.fixed_width_px : child->measured.width_px;
        const int32_t height = child->spec.fixed_height_px > 0 ? child->spec.fixed_height_px
                                                               : child->measured.height_px;
        arrange_node(child.get(),
                     make_rect_within(inner, inner.x_px + child->spec.absolute_x_px,
                                      inner.y_px + child->spec.absolute_y_px, width, height));
    }
}

static void arrange_flex(sao_ui_layout_node_s* node, SaoUiRect inner) {
    const bool row =
        node->flex.direction == SAO_UI_FLEX_ROW || node->flex.direction == SAO_UI_FLEX_ROW_REVERSE;
    const bool reverse = node->flex.direction == SAO_UI_FLEX_ROW_REVERSE ||
                         node->flex.direction == SAO_UI_FLEX_COLUMN_REVERSE;
    const bool wrap = node->flex.wrap != SAO_UI_FLEX_NOWRAP;
    const bool wrap_reverse = node->flex.wrap == SAO_UI_FLEX_WRAP_REVERSE;
    const int32_t available_main = std::max(0, row ? inner.width_px : inner.height_px);
    const int32_t available_cross = std::max(0, row ? inner.height_px : inner.width_px);
    const int32_t gap =
        std::max(0, node->flex.gap_main_px > 0 ? node->flex.gap_main_px : node->spec.gap_px);
    struct flex_line {
        std::vector<size_t> indices;
        std::vector<int32_t> sizes;
        int32_t cross = 0;
    };
    std::vector<flex_line> lines;
    for (size_t index = 0; index < node->children.size(); ++index) {
        const int32_t basis = flex_basis_for_child(*node->children[index], row);
        if (lines.empty())
            lines.emplace_back();
        flex_line& line = lines.back();
        int64_t occupied = basis + (line.indices.empty() ? 0 : gap);
        for (const size_t item : line.indices)
            occupied += flex_basis_for_child(*node->children[item], row);
        if (wrap && !line.indices.empty() && occupied > available_main) {
            lines.emplace_back();
        }
        flex_line& target = lines.back();
        target.indices.push_back(index);
        target.cross = std::max(target.cross, row ? node->children[index]->measured.height_px
                                                  : node->children[index]->measured.width_px);
    }
    // STRETCH: every line spans the full cross axis.  Applied before the
    // align_content bookkeeping so no cross-extra remains to distribute.
    if (node->flex.align_items == SAO_UI_ALIGN_AXIS_STRETCH)
        for (auto& line : lines)
            line.cross = available_cross;
    for (auto& line : lines) {
        const int32_t line_gap =
            line.indices.size() > 1
                ? std::min(gap, available_main / static_cast<int32_t>(line.indices.size() - 1U))
                : 0;
        int64_t base_sum = 0;
        std::vector<double> shrink;
        std::vector<double> grow;
        for (const size_t index : line.indices) {
            line.sizes.push_back(flex_basis_for_child(*node->children[index], row));
            base_sum += line.sizes.back();
            const double factor = std::isfinite(node->children[index]->spec.flex_shrink) &&
                                          node->children[index]->spec.flex_shrink > 0.0F
                                      ? node->children[index]->spec.flex_shrink
                                      : 0.0;
            shrink.push_back(static_cast<double>(line.sizes.back()) * factor);
            grow.push_back(finite_positive_weight(node->children[index]->spec.weight)
                               ? node->children[index]->spec.weight
                               : 0.0);
        }
        const int32_t budget = clamp_nonnegative_i32(std::max<int64_t>(
            0, static_cast<int64_t>(available_main) -
                   static_cast<int64_t>(line_gap) *
                       (line.indices.size() > 1 ? line.indices.size() - 1U : 0U)));
        if (base_sum > budget) {
            const int32_t cut = clamp_nonnegative_i32(base_sum - budget);
            const auto shares = allocate_integer_weight_shares(cut, shrink);
            for (size_t i = 0; i < line.sizes.size(); ++i)
                line.sizes[i] = std::max(0, line.sizes[i] - shares[i]);

        } else if (base_sum < budget) {
            const auto shares =
                allocate_integer_weight_shares(clamp_nonnegative_i32(budget - base_sum), grow);
            for (size_t i = 0; i < line.sizes.size(); ++i)
                line.sizes[i] += shares[i];
        }
    }
    int64_t cross_total = 0;
    const int32_t cross_gap =
        lines.size() > 1 ? std::min(std::max(0, node->flex.gap_cross_px),
                                    available_cross / static_cast<int32_t>(lines.size() - 1U))
                         : 0;
    for (size_t i = 0; i < lines.size(); ++i)
        cross_total += lines[i].cross + (i + 1U < lines.size() ? cross_gap : 0);
    int32_t cross_extra =
        clamp_nonnegative_i32(std::max<int64_t>(0, available_cross - cross_total));
    int32_t cross_lead = 0;
    int32_t cross_step = cross_gap;
    if (node->flex.align_content == SAO_UI_ALIGN_AXIS_END)
        cross_lead = cross_extra;
    else if (node->flex.align_content == SAO_UI_ALIGN_AXIS_CENTER)
        cross_lead = cross_extra / 2;
    else if (node->flex.align_content == SAO_UI_JUSTIFY_SPACE_BETWEEN && lines.size() > 1)
        cross_step += cross_extra / static_cast<int32_t>(lines.size() - 1U);
    else if (node->flex.align_content == SAO_UI_JUSTIFY_SPACE_AROUND && !lines.empty()) {
        cross_lead = cross_extra / static_cast<int32_t>(lines.size() * 2U);
        cross_step += cross_extra / static_cast<int32_t>(lines.size());
    }
    else if (node->flex.align_content == SAO_UI_JUSTIFY_SPACE_EVENLY && !lines.empty()) {
        cross_lead = cross_extra / static_cast<int32_t>(lines.size() + 1U);
        cross_step += cross_extra / static_cast<int32_t>(lines.size() + 1U);
    } else if (node->flex.align_content == SAO_UI_ALIGN_AXIS_STRETCH && !lines.empty()) {
        const auto shares =
            allocate_integer_weight_shares(cross_extra, std::vector<double>(lines.size(), 1.0));
        for (size_t i = 0; i < lines.size(); ++i)
            lines[i].cross += shares[i];
    }
    int64_t cross_cursor = cross_lead;
    for (auto& line : lines) {
        const int64_t cross_position =
            wrap_reverse ? available_cross - cross_cursor - line.cross : cross_cursor;
        int64_t main_extra = available_main;
        const int32_t line_gap =
            line.indices.size() > 1
                ? std::min(gap, available_main / static_cast<int32_t>(line.indices.size() - 1U))
                : 0;
        for (const int32_t size : line.sizes)
            main_extra -= size;
        main_extra -= static_cast<int64_t>(line_gap) *
                      (line.indices.size() > 1 ? line.indices.size() - 1U : 0U);
        int32_t lead = 0;
        int32_t step = line_gap;
        if (node->flex.justify == SAO_UI_JUSTIFY_END)
            lead = clamp_nonnegative_i32(main_extra);
        else if (node->flex.justify == SAO_UI_JUSTIFY_CENTER)
            lead = clamp_nonnegative_i32(main_extra / 2);
        else if (node->flex.justify == SAO_UI_JUSTIFY_SPACE_BETWEEN && line.indices.size() > 1)
            step +=
                clamp_nonnegative_i32(main_extra / static_cast<int32_t>(line.indices.size() - 1U));
        else if (node->flex.justify == SAO_UI_JUSTIFY_SPACE_AROUND && !line.indices.empty()) {
            lead = clamp_nonnegative_i32(main_extra / static_cast<int32_t>(line.indices.size() * 2U));
            step += clamp_nonnegative_i32(main_extra / static_cast<int32_t>(line.indices.size()));
        }
        else if (node->flex.justify == SAO_UI_JUSTIFY_SPACE_EVENLY && !line.indices.empty()) {
            lead =
                clamp_nonnegative_i32(main_extra / static_cast<int32_t>(line.indices.size() + 1U));
            step +=
                clamp_nonnegative_i32(main_extra / static_cast<int32_t>(line.indices.size() + 1U));
        }
        int64_t cursor_main = lead;
        for (size_t i = 0; i < line.indices.size(); ++i) {
            auto& child = node->children[line.indices[i]];
            const int32_t main_size = line.sizes[i];
            int32_t cross_size = std::min(line.cross, std::max(0, row ? child->measured.height_px
                                                                      : child->measured.width_px));
            int64_t cross_offset = 0;
            if (node->flex.align_items == SAO_UI_ALIGN_AXIS_STRETCH)
                cross_size = line.cross;
            else if (node->flex.align_items == SAO_UI_ALIGN_AXIS_END)
                cross_offset = line.cross - cross_size;
            else if (node->flex.align_items == SAO_UI_ALIGN_AXIS_CENTER)
                cross_offset = (line.cross - cross_size) / 2;
            const int64_t main_position =
                reverse ? available_main - cursor_main - main_size : cursor_main;
            if (row)
                arrange_node(child.get(),
                             make_rect_within(inner, inner.x_px + main_position,
                                              inner.y_px + cross_position + cross_offset, main_size,
                                              cross_size));
            else
                arrange_node(child.get(),
                             make_rect_within(inner, inner.x_px + cross_position + cross_offset,
                                              inner.y_px + main_position, cross_size, main_size));
            cursor_main += main_size + step;
        }
        cross_cursor += line.cross + cross_step;
    }
}
static void arrange_dock(sao_ui_layout_node_s* node, SaoUiRect inner) {
    SaoUiRect remaining = inner;
    std::vector<sao_ui_layout_node_s*> centers;
    for (auto& child : node->children) {
        if (child->spec.dock_side == SAO_UI_DOCK_CENTER) {
            centers.push_back(child.get());
            continue;
        }
        const int32_t width = std::max(0, child->measured.width_px);
        const int32_t height = std::max(0, child->measured.height_px);
        SaoUiRect rect{};
        switch (child->spec.dock_side) {
        case SAO_UI_DOCK_TOP:
            rect = make_rect_within(remaining, remaining.x_px, remaining.y_px, remaining.width_px,
                                    height);
            remaining.y_px += height;
            remaining.height_px = std::max(0, remaining.height_px - height);
            break;
        case SAO_UI_DOCK_BOTTOM:
            rect = make_rect_within(remaining, remaining.x_px,
                                    remaining.y_px + remaining.height_px - height,
                                    remaining.width_px, height);
            remaining.height_px = std::max(0, remaining.height_px - height);
            break;
        case SAO_UI_DOCK_LEFT:
            rect = make_rect_within(remaining, remaining.x_px, remaining.y_px, width,
                                    remaining.height_px);
            remaining.x_px += width;
            remaining.width_px = std::max(0, remaining.width_px - width);
            break;
        case SAO_UI_DOCK_RIGHT:
            rect = make_rect_within(remaining, remaining.x_px + remaining.width_px - width,
                                    remaining.y_px, width, remaining.height_px);
            remaining.width_px = std::max(0, remaining.width_px - width);
            break;
        default:
            rect = remaining;
            break;
        }
        arrange_node(child.get(), rect);
    }
    // Center children share the remaining region in insertion order; the last remains topmost.
    for (size_t index = 0; index < centers.size(); ++index) {
        sao_ui_layout_node_s* child = centers[index];
        const bool fills = node->dock.last_child_fills && index + 1U == centers.size();
        const SaoUiRect rect = fills ? remaining : make_rect_within(remaining, remaining.x_px, remaining.y_px, child->measured.width_px, child->measured.height_px);
        arrange_node(child, rect);
    }
}

static void arrange_node(sao_ui_layout_node_s* node, SaoUiRect rect) {
    rect = make_nonnegative_rect(rect.x_px, rect.y_px, rect.width_px, rect.height_px);
    const SaoUiRect previous = node->arranged;
    const bool changed = !rect_equal(previous, rect);
    if (node->paint_dirty || node->spec.force_dirty || changed) {
        if (changed)
            record_dirty_rect(node, previous);
        record_dirty_rect(node, rect);
    }
    node->arranged = rect;
    node->paint_dirty = false;
    if (node->widget != nullptr || node->children.empty())
        return;
    int64_t ml, mt, mr, mb;
    insets_of(node->spec, ml, mt, mr, mb);
    const int64_t inner_w = std::max<int64_t>(0, static_cast<int64_t>(rect.width_px) - ml - mr);
    const int64_t inner_h = std::max<int64_t>(0, static_cast<int64_t>(rect.height_px) - mt - mb);
    const SaoUiRect inner = make_nonnegative_rect(
        saturating_add_i64(rect.x_px, ml), saturating_add_i64(rect.y_px, mt), inner_w, inner_h);
    switch (node->layout_mode) {
    case SAO_UI_LAYOUT_VERTICAL:
        arrange_vertical(node, inner);
        break;
    case SAO_UI_LAYOUT_HORIZONTAL:
        arrange_horizontal(node, inner);
        break;
    case SAO_UI_LAYOUT_GRID:
        arrange_grid(node, inner);
        break;
    case SAO_UI_LAYOUT_ABSOLUTE:
        arrange_absolute(node, inner);
        break;
    case SAO_UI_LAYOUT_FLEX:
        arrange_flex(node, inner);
        break;
    case SAO_UI_LAYOUT_DOCK:
        arrange_dock(node, inner);
        break;
    default:
        break;
    }
}
// ─── hit test recursion ────────────────────────────────────────────

// ─── API implementations ────────────────────────────────────────────

extern "C" void SAO_UI_CALL sao_ui_layout_spec_defaults(SaoUiLayoutSpec* out_spec) {
    if (out_spec == nullptr)
        return;
    std::memset(out_spec, 0, sizeof(*out_spec));
    out_spec->weight = 0.0f;
    out_spec->flex_shrink = 1.0f;
    out_spec->flex_basis = 0.0f;
    out_spec->align_h = SAO_UI_JUSTIFY_START;
    out_spec->align_v = SAO_UI_ALIGN_AXIS_STRETCH;
    out_spec->dock_side = SAO_UI_DOCK_FILL;
    out_spec->hit_testable = true;
    out_spec->clip_children = false;
    out_spec->force_dirty = false;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_layout_tree_create(sao_ui_layout_tree_handle_t* out_tree) {
    if (out_tree == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_tree = nullptr;
    try {
        auto* tree = new (std::nothrow) sao_ui_layout_tree_s;
        if (tree == nullptr)
            return SAO_STATUS_ERR_UNKNOWN;
        *out_tree = tree;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_layout_tree_destroy(sao_ui_layout_tree_handle_t tree) {
    if (tree == nullptr) return;
    std::unique_lock<std::mutex> lock(tree->mu);
    tree->root.reset();
    tree->dirty_rects.clear();
    lock.unlock();
    delete tree;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_layout_tree_set_root(sao_ui_layout_tree_handle_t tree, int32_t layout_mode,
                            const SaoUiLayoutSpec* spec, sao_ui_layout_node_handle_t* out_root) {
    if (tree == nullptr || spec == nullptr || out_root == nullptr ||
        !valid_layout_mode(layout_mode)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_root = nullptr;
    try {
        std::lock_guard<std::mutex> lock(tree->mu);
        if (tree->root)
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        auto node = std::make_unique<sao_ui_layout_node_s>();
        node->tree = tree;
        node->layout_mode = layout_mode;
        node->spec = *spec;
        sao_ui_layout_node_handle_t root = node.get();
        tree->root = std::move(node);
        *out_root = root;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_add_container(
    sao_ui_layout_node_handle_t parent, int32_t layout_mode, const SaoUiLayoutSpec* spec,
    sao_ui_layout_node_handle_t* out_child) {
    if (parent == nullptr || spec == nullptr || out_child == nullptr ||
        !valid_layout_mode(layout_mode)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_child = nullptr;
    std::lock_guard<std::mutex> lock(parent->tree->mu);
    try {
        auto node = std::make_unique<sao_ui_layout_node_s>();
        node->parent = parent;
        node->tree = parent->tree;
        node->layout_mode = layout_mode;
        node->spec = *spec;
        sao_ui_layout_node_handle_t child = node.get();
        parent->children.push_back(std::move(node));
        *out_child = child;
        mark_dirty_up(parent);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_layout_node_add_widget(sao_ui_layout_node_handle_t parent, sao_ui_widget_handle_t widget,
                              const SaoUiLayoutSpec* spec, sao_ui_layout_node_handle_t* out_leaf) {
    if (parent == nullptr || widget == nullptr || spec == nullptr || out_leaf == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_leaf = nullptr;
    std::lock_guard<std::mutex> lock(parent->tree->mu);
    try {
        auto node = std::make_unique<sao_ui_layout_node_s>();
        node->parent = parent;
        node->tree = parent->tree;
        node->widget = widget;
        node->spec = *spec;
        sao_ui_layout_node_handle_t leaf = node.get();
        parent->children.push_back(std::move(node));
        *out_leaf = leaf;
        mark_dirty_up(parent);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_remove(sao_ui_layout_node_handle_t node) {
    if (node == nullptr || node->parent == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(node->tree->mu);
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

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_set_spec(sao_ui_layout_node_handle_t node,
                                                                const SaoUiLayoutSpec* spec) {
    if (node == nullptr || spec == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(node->tree->mu);
    node->spec = *spec;
    mark_dirty_up(node);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_layout_node_set_mode_config(sao_ui_layout_node_handle_t container, const void* mode_config) {
    if (container == nullptr || mode_config == nullptr || container->widget != nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(container->tree->mu);
    try {
        switch (container->layout_mode) {
        case SAO_UI_LAYOUT_VERTICAL:
            container->vertical = *static_cast<const SaoUiVerticalMode*>(mode_config);
            break;
        case SAO_UI_LAYOUT_HORIZONTAL:
            container->horizontal = *static_cast<const SaoUiHorizontalMode*>(mode_config);
            break;
        case SAO_UI_LAYOUT_GRID: {
            const SaoUiGridMode* grid = static_cast<const SaoUiGridMode*>(mode_config);
            constexpr size_t kMaxTrackCount = 1U << 16U;
            if ((grid->row_count != 0U && grid->rows == nullptr) ||
                (grid->col_count != 0U && grid->cols == nullptr) ||
                grid->row_count > kMaxTrackCount || grid->col_count > kMaxTrackCount) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            std::vector<SaoUiTrackSize> rows;
            std::vector<SaoUiTrackSize> cols;
            if (grid->row_count != 0U)
                rows.assign(grid->rows, grid->rows + grid->row_count);
            if (grid->col_count != 0U)
                cols.assign(grid->cols, grid->cols + grid->col_count);
            container->grid = *grid;
            container->grid.rows = nullptr;
            container->grid.cols = nullptr;
            container->grid_rows_owned = std::move(rows);
            container->grid_cols_owned = std::move(cols);
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
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_reorder(sao_ui_layout_node_handle_t child,
                                                               int32_t new_index) {
    if (child == nullptr || child->parent == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(child->tree->mu);
    try {
        auto& siblings = child->parent->children;
        auto it = std::find_if(
            siblings.begin(), siblings.end(),
            [child](const std::unique_ptr<sao_ui_layout_node_s>& p) { return p.get() == child; });
        if (it == siblings.end())
            return SAO_STATUS_ERR_NOT_FOUND;
        // Extract, then insert at clamped index.
        auto owned = std::move(*it);
        siblings.erase(it);
        const int32_t clamped = clamp_i32(new_index, 0, static_cast<int32_t>(siblings.size()));
        siblings.insert(siblings.begin() + clamped, std::move(owned));
        mark_dirty_up(child->parent);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_measure(sao_ui_layout_node_handle_t root,
                                                          SaoUiSize available,
                                                          SaoUiSize* out_preferred) {
    if (root == nullptr || out_preferred == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_preferred = {};
    try {
        if (root->tree != nullptr) {
            std::lock_guard<std::mutex> lock(root->tree->mu);
            *out_preferred = measure_node(root, available);
        } else {
            *out_preferred = measure_node(root, available);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_arrange(sao_ui_layout_node_handle_t root,
                                                          SaoUiRect rect) {
    if (root == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        if (root->tree != nullptr) {
            std::lock_guard<std::mutex> lock(root->tree->mu);
            arrange_node(root, rect);
        } else {
            arrange_node(root, rect);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_node_get_rect(sao_ui_layout_node_handle_t node,
                                                                SaoUiRect* out_rect) {
    if (node == nullptr || out_rect == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(node->tree->mu);
    *out_rect = node->arranged;
    return SAO_STATUS_OK;
}

static sao_ui_layout_node_s* hit_test_walk(sao_ui_layout_node_s* node, int32_t x, int32_t y,
                                           sao_ui_layout_node_s* /*caller_best*/) {
    if (node == nullptr || !node->spec.hit_testable)
        return nullptr;
    const SaoUiRect& rect = node->arranged;
    if (x < rect.x_px || y < rect.y_px || x >= rect.x_px + rect.width_px ||
        y >= rect.y_px + rect.height_px)
        return nullptr;
    // Children later in the list draw on top — test them first.
    for (size_t index = node->children.size(); index-- > 0;) {
        sao_ui_layout_node_s* const child_hit =
            hit_test_walk(node->children[index].get(), x, y, nullptr);
        if (child_hit != nullptr)
            return child_hit;
    }
    return node;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_hit_test(sao_ui_layout_node_handle_t root,
                                                           int32_t x, int32_t y,
                                                           SaoUiHitResult* out_hit) {
    if (root == nullptr || out_hit == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(root->tree->mu);
    out_hit->node = nullptr;
    out_hit->widget = nullptr;
    out_hit->local_x_px = 0;
    out_hit->local_y_px = 0;
    out_hit->depth = 0;
    sao_ui_layout_node_s* hit = hit_test_walk(root, x, y, nullptr);
    if (hit == nullptr)
        return SAO_STATUS_ERR_NOT_FOUND;
    out_hit->node = hit;
    out_hit->widget = hit->widget;
    out_hit->local_x_px = x - hit->arranged.x_px;
    out_hit->local_y_px = y - hit->arranged.y_px;
    // Depth: walk parent chain.
    int32_t d = 0;
    for (sao_ui_layout_node_s* n = hit; n->parent != nullptr; n = n->parent)
        ++d;
    out_hit->depth = d;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_get_hit_path(
    sao_ui_layout_node_handle_t hit_node, sao_ui_layout_node_handle_t* out_path, size_t capacity,
    size_t* out_written) {
    if (hit_node == nullptr || out_written == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(hit_node->tree->mu);
    size_t depth = 0;
    for (sao_ui_layout_node_s* n = hit_node; n != nullptr; n = n->parent)
        ++depth;
    *out_written = depth;
    if (out_path == nullptr)
        return SAO_STATUS_OK;
    size_t i = 0;
    for (sao_ui_layout_node_s* n = hit_node; n != nullptr && i < capacity; n = n->parent) {
        out_path[i++] = n;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_layout_node_invalidate(sao_ui_layout_node_handle_t node) {
    if (node == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(node->tree->mu);
    mark_dirty_up(node);
    return SAO_STATUS_OK;
}

static void mark_subtree_dirty(sao_ui_layout_node_s* node) {
    if (node == nullptr)
        return;
    node->dirty = true;
    node->paint_dirty = true;
    node->measure_cache_valid = false;
    for (auto& c : node->children)
        mark_subtree_dirty(c.get());
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_layout_node_invalidate_subtree(sao_ui_layout_node_handle_t node) {
    if (node == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(node->tree->mu);
    mark_subtree_dirty(node);
    // Also propagate up so measure() picks up the invalidation.
    if (node->parent != nullptr)
        mark_dirty_up(node->parent);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_layout_tree_invalidate_all(sao_ui_layout_tree_handle_t tree) {
    if (tree == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(tree->mu);
    if (tree->root)
        mark_subtree_dirty(tree->root.get());
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_take_dirty_rects(sao_ui_layout_tree_handle_t tree,
                                                                   SaoUiRect* out_rects,
                                                                   size_t capacity,
                                                                   size_t* out_written) {
    if (tree == nullptr || out_written == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(tree->mu);
    const size_t n = tree->dirty_rects.size();
    *out_written = n;
    if (out_rects != nullptr) {
        const size_t copy_n = std::min(n, capacity);
        for (size_t i = 0; i < copy_n; ++i)
            out_rects[i] = tree->dirty_rects[i];
    }
    tree->dirty_rects.clear();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layout_dump_json(sao_ui_layout_tree_handle_t tree,
                                                            uint8_t* out_utf8_buffer,
                                                            size_t capacity,
                                                            size_t* out_bytes_written) {
    if (tree == nullptr || out_bytes_written == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> lock(tree->mu);
        nlohmann::json snapshot{{"has_root", tree->root != nullptr}};
        if (tree->root != nullptr)
            snapshot["root"] = layout_node_json(*tree->root);
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
