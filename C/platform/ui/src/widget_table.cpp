// SAO Auto — table widgets first slice (Wave 4 / Agent d, G3.8).
//
// This slice implements the Table portion of widget_table.h:
//   * sao_ui_table_create / _set_rows / _upsert_row / _remove_row /
//     _clear_rows / _set_sort / _set_filter / _set_row_click_handler
//
// Plus wave4 helpers for virtual-scroll visible-range calculation, cell
// hit-test, and stable sort ordering.  TreeView is stubbed for a later
// slice.
//
// Storage model:
//   * columns    = owned copies of caller-supplied SaoUiTableColumn
//                  (including UTF-8 keys/titles).
//   * rows_all   = master, insertion-ordered list of rows.
//   * rows_view  = filtered + sorted view (indices into rows_all).
//     rebuild_view is idempotent and called from set_rows / set_sort /
//     set_filter.
//
// UTF-8 no BOM.

#include "sao/ui/widget_table.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(SAO_UI_COL_TEXT     == 0, "column type enum drifted");
static_assert(SAO_UI_COL_RELTIME  == 8, "column type enum drifted");
static_assert(SAO_UI_CELL_STRING  == 0, "cell kind enum drifted");
static_assert(SAO_UI_CELL_BOOL    == 3, "cell kind enum drifted");

namespace {

constexpr int32_t kTableTag = 145;   // aligned with SAO_UI_WIDGET_TABLE_EXT
constexpr int32_t kTreeTag = 146;

// Owned mirror of SaoUiTableColumn.  We copy the strings so the caller
// can free their buffers immediately after create() returns.
struct OwnedColumn {
    std::string key;
    std::string title;
    int32_t     type{0};
    int32_t     align{0};
    int32_t     min_width_px{0};
    int32_t     max_width_px{0};
    float       flex_weight{0.0f};
    bool        sortable{false};
    bool        filterable{true};
    bool        resizable{false};
    bool        hidden{false};
    uint32_t    header_bg_argb{0};
    uint32_t    header_fg_argb{0};
    uint32_t    cell_fg_argb{0};
    uint32_t    cell_bg_alt_argb{0};
};

// Owned mirror of a single cell — we copy strings so callers can
// hand us stack-allocated const char*.
struct OwnedCell {
    int32_t     kind{SAO_UI_CELL_STRING};
    std::string s;
    int64_t     i64{0};
    double      f64{0.0};
    bool        b{false};
    double      max_hint{0.0};
    uint32_t    fg_argb{0};
    uint32_t    bg_argb{0};
};

// Owned mirror of SaoUiTableRow.
struct OwnedRow {
    int64_t                row_id{0};
    std::vector<OwnedCell> cells;
    bool                   highlight{false};
    bool                   mem_priority_badge{false};
    bool                   zebra_alt{false};
    bool                   dim{false};
    uint32_t               row_bg_override_argb{0};
    uint32_t               row_fg_override_argb{0};
};

// Wave 4 default heights (Python action table uses row_height=22 for
// the DPS panel).
constexpr int32_t kDefaultRowHeight    = 22;
constexpr int32_t kDefaultHeaderHeight = 24;

struct TableState {
    int32_t                     tag{kTableTag};
    SaoUiTableSpec              spec{};   // pointer fields NULL after copy-in
    std::vector<OwnedColumn>    columns;
    std::vector<OwnedRow>       rows_all;
    // Ordered indices into rows_all, post filter + sort.
    std::vector<size_t>         rows_view;
    // Sort state.
    std::string                 sort_key;
    bool                        sort_desc{false};
    bool                        sorted{false};
    // Filter state.
    std::string                 filter_text;
    // Callback wiring.
    sao_ui_table_row_click_cb_t         row_click_cb{nullptr};
    void*                               row_click_user{nullptr};
    sao_ui_table_cell_action_cb_t       cell_action_cb{nullptr};
    void*                               cell_action_user{nullptr};
    mutable std::mutex          mtx;
};

struct OwnedTreeNode {
    int64_t node_id{0};
    int64_t parent_id{0};
    std::string label;
    std::string detail;
    int32_t icon_slot{-1};
    bool expanded{false};
    bool selectable{true};
    uint32_t fg_argb{0};
    uint32_t bg_argb{0};
};

struct VisibleTreeNode {
    size_t node_index{0};
    int32_t depth{0};
};

struct TreeState {
    int32_t tag{kTreeTag};
    SaoUiTreeViewSpec spec{};
    std::vector<OwnedTreeNode> nodes;
    std::vector<VisibleTreeNode> visible;
    sao_ui_tree_select_cb_t select_cb{nullptr};
    void* select_user{nullptr};
    int64_t selected_node_id{0};
    mutable std::mutex mtx;
};

int32_t peek_tag(sao_ui_widget_handle_t h) {
    if (h == nullptr) return -1;
    return *reinterpret_cast<const int32_t*>(h);
}

TableState* as_table(sao_ui_widget_handle_t h) {
    if (peek_tag(h) != kTableTag) return nullptr;
    return reinterpret_cast<TableState*>(h);
}

TreeState* as_tree(sao_ui_widget_handle_t h) {
    if (peek_tag(h) != kTreeTag) return nullptr;
    return reinterpret_cast<TreeState*>(h);
}

size_t find_tree_node_no_lock(const TreeState& tree, int64_t node_id) {
    for (size_t index = 0; index < tree.nodes.size(); ++index) {
        if (tree.nodes[index].node_id == node_id) return index;
    }
    return std::numeric_limits<size_t>::max();
}

void append_visible_children_no_lock(
    TreeState& tree, int64_t parent_id, int32_t depth) {
    for (size_t index = 0; index < tree.nodes.size(); ++index) {
        const auto& node = tree.nodes[index];
        if (node.parent_id != parent_id) continue;
        tree.visible.push_back({index, depth});
        if (node.expanded) {
            append_visible_children_no_lock(tree, node.node_id, depth + 1);
        }
    }
}

void rebuild_tree_visible_no_lock(TreeState& tree) {
    tree.visible.clear();
    append_visible_children_no_lock(tree, 0, 0);
}

void copy_column_no_lock(OwnedColumn& out, const SaoUiTableColumn& src) {
    out.key   = src.key_utf8   ? src.key_utf8   : "";
    out.title = src.title_utf8 ? src.title_utf8 : "";
    out.type  = src.type;
    out.align = src.align;
    out.min_width_px = src.min_width_px;
    out.max_width_px = src.max_width_px;
    out.flex_weight  = src.flex_weight;
    out.sortable     = src.sortable;
    out.filterable   = src.filterable;
    out.resizable    = src.resizable;
    out.hidden       = src.hidden;
    out.header_bg_argb   = src.header_bg_argb;
    out.header_fg_argb   = src.header_fg_argb;
    out.cell_fg_argb     = src.cell_fg_argb;
    out.cell_bg_alt_argb = src.cell_bg_alt_argb;
}

void copy_cell_no_lock(OwnedCell& out, const SaoUiCellValue& src) {
    out.kind = src.kind;
    switch (src.kind) {
        case SAO_UI_CELL_STRING:
            out.s = src.v.s_utf8 ? src.v.s_utf8 : "";
            break;
        case SAO_UI_CELL_INT64:  out.i64 = src.v.i64; break;
        case SAO_UI_CELL_DOUBLE: out.f64 = src.v.f64; break;
        case SAO_UI_CELL_BOOL:   out.b   = src.v.b;   break;
        default: break;
    }
    out.max_hint = src.max_hint;
    out.fg_argb  = src.fg_argb;
    out.bg_argb  = src.bg_argb;
}

void copy_row_no_lock(OwnedRow& out, const SaoUiTableRow& src) {
    out.row_id = src.row_id;
    out.highlight          = src.highlight;
    out.mem_priority_badge = src.mem_priority_badge;
    out.zebra_alt          = src.zebra_alt;
    out.dim                = src.dim;
    out.row_bg_override_argb = src.row_bg_override_argb;
    out.row_fg_override_argb = src.row_fg_override_argb;
    out.cells.clear();
    out.cells.reserve(src.cell_count);
    for (size_t i = 0; i < src.cell_count; ++i) {
        OwnedCell c;
        copy_cell_no_lock(c, src.cells[i]);
        out.cells.push_back(std::move(c));
    }
}

// Find column index by key.  Returns -1 if not found.
int32_t find_column_index_no_lock(const TableState& s,
                                   std::string_view key) {
    for (size_t i = 0; i < s.columns.size(); ++i) {
        if (s.columns[i].key == key) return static_cast<int32_t>(i);
    }
    return -1;
}

// Comparator between two OwnedCell values under a column type.
// Returns <0/=0/>0.
int compare_cells(const OwnedCell& a, const OwnedCell& b) {
    if (a.kind != b.kind) {
        // Different kinds compared by kind ordinal (deterministic).
        return a.kind < b.kind ? -1 : 1;
    }
    switch (a.kind) {
        case SAO_UI_CELL_STRING:
            return a.s.compare(b.s);
        case SAO_UI_CELL_INT64:
            if (a.i64 < b.i64) return -1;
            if (a.i64 > b.i64) return  1;
            return 0;
        case SAO_UI_CELL_DOUBLE:
            if (a.f64 < b.f64) return -1;
            if (a.f64 > b.f64) return  1;
            return 0;
        case SAO_UI_CELL_BOOL:
            if (a.b == b.b) return 0;
            return a.b ? 1 : -1;
        default:
            return 0;
    }
}

// Return true if the row should be kept under the current filter.
// Empty filter → keep everything.  Otherwise: case-insensitive substring
// match against any filterable column's stringified cell value.
bool row_passes_filter_no_lock(const TableState& s, const OwnedRow& row) {
    if (s.filter_text.empty()) return true;
    // Naive case-insensitive substring: lower-case the filter, then
    // compare cell-by-cell.  Only ASCII (matches the Python filter
    // widget which itself is ASCII-only at present).
    std::string needle = s.filter_text;
    for (auto& c : needle) c = static_cast<char>(std::tolower(c));
    for (size_t ci = 0; ci < s.columns.size() && ci < row.cells.size(); ++ci) {
        const OwnedColumn& col = s.columns[ci];
        if (!col.filterable) continue;
        const OwnedCell& cell = row.cells[ci];
        std::string hay;
        switch (cell.kind) {
            case SAO_UI_CELL_STRING: hay = cell.s; break;
            case SAO_UI_CELL_INT64:  hay = std::to_string(cell.i64); break;
            case SAO_UI_CELL_DOUBLE: hay = std::to_string(cell.f64); break;
            case SAO_UI_CELL_BOOL:   hay = cell.b ? "true" : "false"; break;
            default: break;
        }
        for (auto& c : hay) c = static_cast<char>(std::tolower(c));
        if (hay.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Rebuild rows_view from rows_all under the current sort/filter state.
// stable_sort keeps insertion order among equal keys.
void rebuild_view_no_lock(TableState& s) {
    s.rows_view.clear();
    s.rows_view.reserve(s.rows_all.size());
    for (size_t i = 0; i < s.rows_all.size(); ++i) {
        if (row_passes_filter_no_lock(s, s.rows_all[i])) {
            s.rows_view.push_back(i);
        }
    }
    if (!s.sorted || s.sort_key.empty()) return;
    const int32_t col_idx = find_column_index_no_lock(s, s.sort_key);
    if (col_idx < 0) return;
    const bool desc = s.sort_desc;
    std::stable_sort(s.rows_view.begin(), s.rows_view.end(),
                     [&](size_t la, size_t lb) {
        const OwnedRow& ra = s.rows_all[la];
        const OwnedRow& rb = s.rows_all[lb];
        if (static_cast<size_t>(col_idx) >= ra.cells.size() ||
            static_cast<size_t>(col_idx) >= rb.cells.size()) {
            return false;
        }
        const int cmp = compare_cells(ra.cells[col_idx], rb.cells[col_idx]);
        if (cmp == 0) return false;
        return desc ? (cmp > 0) : (cmp < 0);
    });
}

// Find an existing row by row_id.  Returns iterator (end() if missing).
std::vector<OwnedRow>::iterator find_row_by_id_no_lock(TableState& s,
                                                       int64_t row_id) {
    return std::find_if(s.rows_all.begin(), s.rows_all.end(),
        [row_id](const OwnedRow& r) { return r.row_id == row_id; });
}

}  // namespace

// ---------------------------------------------------------------------------
// Table ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_create(
    void* /*d3d_device_ptr*/,
    const SaoUiTableSpec* spec,
    sao_ui_widget_handle_t* out_handle) {
    if (spec == nullptr || out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* s = new TableState();
    s->spec = *spec;
    // Sever borrowed pointers on the stored spec.
    s->spec.columns = nullptr;
    s->spec.column_count = 0;
    s->spec.initial_sort_key_utf8 = nullptr;
    s->spec.initial_filter_utf8 = nullptr;
    s->columns.reserve(spec->column_count);
    for (size_t i = 0; i < spec->column_count; ++i) {
        OwnedColumn c;
        copy_column_no_lock(c, spec->columns[i]);
        s->columns.push_back(std::move(c));
    }
    if (spec->initial_sort_key_utf8 &&
        spec->initial_sort_key_utf8[0] != '\0') {
        s->sort_key  = spec->initial_sort_key_utf8;
        s->sort_desc = spec->initial_sort_desc;
        s->sorted    = true;
    }
    if (spec->initial_filter_utf8 &&
        spec->initial_filter_utf8[0] != '\0') {
        s->filter_text = spec->initial_filter_utf8;
    }
    if (s->spec.row_height_px    <= 0) s->spec.row_height_px    = kDefaultRowHeight;
    if (s->spec.header_height_px <= 0) s->spec.header_height_px = kDefaultHeaderHeight;
    *out_handle = reinterpret_cast<sao_ui_widget_handle_t>(s);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_set_rows(
    sao_ui_widget_handle_t handle,
    const SaoUiTableRow* rows,
    size_t row_count) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (rows == nullptr && row_count > 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    s->rows_all.clear();
    s->rows_all.reserve(row_count);
    for (size_t i = 0; i < row_count; ++i) {
        OwnedRow r;
        copy_row_no_lock(r, rows[i]);
        s->rows_all.push_back(std::move(r));
    }
    rebuild_view_no_lock(*s);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_upsert_row(
    sao_ui_widget_handle_t handle,
    const SaoUiTableRow* row) {
    TableState* s = as_table(handle);
    if (s == nullptr || row == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    auto it = find_row_by_id_no_lock(*s, row->row_id);
    if (it == s->rows_all.end()) {
        OwnedRow r;
        copy_row_no_lock(r, *row);
        s->rows_all.push_back(std::move(r));
    } else {
        copy_row_no_lock(*it, *row);
    }
    rebuild_view_no_lock(*s);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_remove_row(
    sao_ui_widget_handle_t handle,
    int64_t row_id) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    auto it = find_row_by_id_no_lock(*s, row_id);
    if (it == s->rows_all.end()) return SAO_STATUS_ERR_NOT_FOUND;
    s->rows_all.erase(it);
    rebuild_view_no_lock(*s);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_clear_rows(
    sao_ui_widget_handle_t handle) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->rows_all.clear();
    s->rows_view.clear();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_set_sort(
    sao_ui_widget_handle_t handle,
    const char* column_key_utf8,
    bool descending) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (column_key_utf8 == nullptr || column_key_utf8[0] == '\0') {
        s->sort_key.clear();
        s->sort_desc = false;
        s->sorted    = false;
    } else {
        s->sort_key  = column_key_utf8;
        s->sort_desc = descending;
        s->sorted    = true;
    }
    rebuild_view_no_lock(*s);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_set_filter(
    sao_ui_widget_handle_t handle,
    const char* filter_utf8) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->filter_text = (filter_utf8 && filter_utf8[0] != '\0') ? filter_utf8 : "";
    rebuild_view_no_lock(*s);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_set_row_click_handler(
    sao_ui_widget_handle_t handle,
    sao_ui_table_row_click_cb_t callback,
    void* user_data) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->row_click_cb = callback;
    s->row_click_user = user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_set_cell_action_handler(
    sao_ui_widget_handle_t handle,
    sao_ui_table_cell_action_cb_t callback,
    void* user_data) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->cell_action_cb = callback;
    s->cell_action_user = user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_create(
    void*, const SaoUiTreeViewSpec* spec, sao_ui_widget_handle_t* out) {
    if (out == nullptr || spec == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto* tree = new (std::nothrow) TreeState();
    if (tree == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    tree->spec = *spec;
    if (tree->spec.row_height_px <= 0) tree->spec.row_height_px = kDefaultRowHeight;
    if (tree->spec.indent_px <= 0) tree->spec.indent_px = 14;
    if (tree->spec.caret_width_px <= 0) tree->spec.caret_width_px = 10;
    *out = reinterpret_cast<sao_ui_widget_handle_t>(tree);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_set_nodes(
    sao_ui_widget_handle_t handle, const SaoUiTreeNode* nodes, size_t node_count) {
    TreeState* tree = as_tree(handle);
    if (tree == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (nodes == nullptr && node_count != 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::vector<OwnedTreeNode> next;
    next.reserve(node_count);
    for (size_t index = 0; index < node_count; ++index) {
        if (nodes[index].node_id == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (std::any_of(next.begin(), next.end(), [&](const OwnedTreeNode& item) {
                return item.node_id == nodes[index].node_id;
            })) return SAO_STATUS_ERR_ALREADY_EXISTS;
        OwnedTreeNode owned{};
        owned.node_id = nodes[index].node_id;
        owned.parent_id = nodes[index].parent_id;
        owned.label = nodes[index].label_utf8 == nullptr ? "" : nodes[index].label_utf8;
        owned.detail = nodes[index].detail_utf8 == nullptr ? "" : nodes[index].detail_utf8;
        owned.icon_slot = nodes[index].icon_slot;
        owned.expanded = nodes[index].expanded_default;
        owned.selectable = nodes[index].selectable;
        owned.fg_argb = nodes[index].fg_argb;
        owned.bg_argb = nodes[index].bg_argb;
        next.push_back(std::move(owned));
    }
    for (const auto& node : next) {
        if (node.parent_id == 0) continue;
        if (std::none_of(next.begin(), next.end(), [&](const OwnedTreeNode& candidate) {
                return candidate.node_id == node.parent_id;
            })) return SAO_STATUS_ERR_NOT_FOUND;
        int64_t cursor = node.parent_id;
        for (size_t depth = 0; depth <= next.size(); ++depth) {
            if (cursor == node.node_id) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            const auto parent = std::find_if(next.begin(), next.end(), [&](const OwnedTreeNode& candidate) {
                return candidate.node_id == cursor;
            });
            if (parent == next.end() || parent->parent_id == 0) break;
            cursor = parent->parent_id;
        }
    }
    std::lock_guard<std::mutex> lock(tree->mtx);
    tree->nodes = std::move(next);
    tree->selected_node_id = 0;
    rebuild_tree_visible_no_lock(*tree);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_expand_node(
    sao_ui_widget_handle_t handle, int64_t node_id, bool expanded) {
    TreeState* tree = as_tree(handle);
    if (tree == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(tree->mtx);
    const size_t index = find_tree_node_no_lock(*tree, node_id);
    if (index == std::numeric_limits<size_t>::max()) return SAO_STATUS_ERR_NOT_FOUND;
    tree->nodes[index].expanded = expanded;
    rebuild_tree_visible_no_lock(*tree);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_set_select_handler(
    sao_ui_widget_handle_t handle, sao_ui_tree_select_cb_t callback, void* user_data) {
    TreeState* tree = as_tree(handle);
    if (tree == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(tree->mtx);
    tree->select_cb = callback;
    tree->select_user = user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_select_node(
    sao_ui_widget_handle_t handle, int64_t node_id) {
    TreeState* tree = as_tree(handle);
    if (tree == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    sao_ui_tree_select_cb_t callback = nullptr;
    void* user_data = nullptr;
    {
        std::lock_guard<std::mutex> lock(tree->mtx);
        const size_t index = find_tree_node_no_lock(*tree, node_id);
        if (index == std::numeric_limits<size_t>::max()) return SAO_STATUS_ERR_NOT_FOUND;
        if (!tree->nodes[index].selectable) return SAO_STATUS_ERR_ACCESS_DENIED;
        const bool visible = std::any_of(tree->visible.begin(), tree->visible.end(),
            [index](const VisibleTreeNode& item) { return item.node_index == index; });
        if (!visible) return SAO_STATUS_ERR_NOT_FOUND;
        tree->selected_node_id = node_id;
        callback = tree->select_cb;
        user_data = tree->select_user;
    }
    if (callback != nullptr) callback(node_id, user_data);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_get_visible_count(
    sao_ui_widget_handle_t handle, size_t* out_count) {
    TreeState* tree = as_tree(handle);
    if (tree == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_count == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(tree->mtx);
    *out_count = tree->visible.size();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_get_visible_node(
    sao_ui_widget_handle_t handle, size_t visible_index,
    int64_t* out_node_id, int32_t* out_depth) {
    TreeState* tree = as_tree(handle);
    if (tree == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_node_id == nullptr || out_depth == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(tree->mtx);
    if (visible_index >= tree->visible.size()) return SAO_STATUS_ERR_NOT_FOUND;
    const VisibleTreeNode item = tree->visible[visible_index];
    *out_node_id = tree->nodes[item.node_index].node_id;
    *out_depth = item.depth;
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Wave 4 helper API — visible-range for virtual scroll + cell hit test
// + row-id enumeration for tests.
// ---------------------------------------------------------------------------

struct SaoUiPointF {
    float x;
    float y;
};

// Number of visible rows post-filter (rows_view size).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_get_visible_row_count(
    sao_ui_widget_handle_t handle,
    size_t* out_count) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (out_count) *out_count = s->rows_view.size();
    return SAO_STATUS_OK;
}

// Row id at a specific position within the post-filter/sort view.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_get_row_id_at_view_index(
    sao_ui_widget_handle_t handle,
    size_t view_index,
    int64_t* out_row_id) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (view_index >= s->rows_view.size()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const size_t master_idx = s->rows_view[view_index];
    if (out_row_id) *out_row_id = s->rows_all[master_idx].row_id;
    return SAO_STATUS_OK;
}

// Compute first/last-visible-row indices in the current view under a
// given scroll offset + viewport height.  Both indices are inclusive;
// last is clamped to view size - 1.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_get_visible_range(
    sao_ui_widget_handle_t handle,
    int32_t viewport_h_px,
    int32_t scroll_offset_px,
    size_t* first_row_out,
    size_t* last_row_out) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (viewport_h_px <= 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(s->mtx);
    const int32_t row_h = s->spec.row_height_px;
    if (row_h <= 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (s->rows_view.empty()) {
        if (first_row_out) *first_row_out = 0;
        if (last_row_out)  *last_row_out  = 0;
        return SAO_STATUS_OK;
    }
    const int32_t body_offset = std::max(int32_t{0}, scroll_offset_px);
    const size_t first = static_cast<size_t>(body_offset / row_h);
    // last inclusive: cover any row that has any pixel visible in the
    // viewport.  Ceiling divide against the trailing edge.
    const size_t last_exclusive = static_cast<size_t>(
        (body_offset + viewport_h_px + row_h - 1) / row_h);
    const size_t view_size = s->rows_view.size();
    const size_t clamped_first = std::min(first, view_size > 0 ? view_size - 1 : 0);
    const size_t clamped_last = std::min(
        last_exclusive == 0 ? 0 : last_exclusive - 1,
        view_size > 0 ? view_size - 1 : 0);
    if (first_row_out) *first_row_out = clamped_first;
    if (last_row_out)  *last_row_out  = clamped_last;
    return SAO_STATUS_OK;
}

// Hit test: point is in body-local coords (0,0 = top-left of body area,
// after the header).  Returns row/col in the current view.  When the
// hit is on the header, out_row_view_index is set to SIZE_MAX and
// out_col_index still resolves.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_hit_test(
    sao_ui_widget_handle_t handle,
    SaoUiPointF point,
    int32_t total_width_px,
    int32_t scroll_offset_px,
    size_t* out_row_view_index,
    int32_t* out_col_index) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (out_row_view_index) *out_row_view_index = std::numeric_limits<size_t>::max();
    if (out_col_index)      *out_col_index      = -1;
    if (point.x < 0.0f || point.y < 0.0f) return SAO_STATUS_OK;
    // Row selection: divide by row_h + scroll offset.
    const int32_t row_h = s->spec.row_height_px;
    if (row_h > 0) {
        const int32_t abs_y = static_cast<int32_t>(point.y) +
                              std::max(int32_t{0}, scroll_offset_px);
        const size_t row_idx = static_cast<size_t>(abs_y / row_h);
        if (row_idx < s->rows_view.size()) {
            if (out_row_view_index) *out_row_view_index = row_idx;
        }
    }
    // Column selection: distribute total_width_px across columns by
    // (max(min_width_px, flex_weight * remainder)).  Simple version:
    // start with each column's min_width_px; give any leftover to
    // flex_weight-proportional columns.
    if (s->columns.empty() || total_width_px <= 0) return SAO_STATUS_OK;
    std::vector<int32_t> widths(s->columns.size(), 0);
    int32_t used = 0;
    float total_weight = 0.0f;
    for (size_t i = 0; i < s->columns.size(); ++i) {
        if (s->columns[i].hidden) { widths[i] = 0; continue; }
        widths[i] = std::max(int32_t{1}, s->columns[i].min_width_px);
        used += widths[i];
        total_weight += std::max(0.0f, s->columns[i].flex_weight);
    }
    const int32_t remainder = std::max(int32_t{0}, total_width_px - used);
    if (remainder > 0 && total_weight > 0.0f) {
        for (size_t i = 0; i < s->columns.size(); ++i) {
            if (s->columns[i].hidden) continue;
            const float w = std::max(0.0f, s->columns[i].flex_weight);
            widths[i] += static_cast<int32_t>(
                static_cast<float>(remainder) * (w / total_weight));
        }
    }
    // Locate x.
    int32_t x_accum = 0;
    for (size_t i = 0; i < widths.size(); ++i) {
        if (s->columns[i].hidden) continue;
        const int32_t next = x_accum + widths[i];
        if (point.x >= static_cast<float>(x_accum) &&
            point.x <  static_cast<float>(next)) {
            if (out_col_index) *out_col_index = static_cast<int32_t>(i);
            break;
        }
        x_accum = next;
    }
    return SAO_STATUS_OK;
}

// Sort by column index — public wrapper for tests that don't want to
// look up the column key themselves.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_sort_by_index(
    sao_ui_widget_handle_t handle,
    int32_t col_index,
    bool ascending) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (col_index < 0 ||
        static_cast<size_t>(col_index) >= s->columns.size()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    s->sort_key  = s->columns[static_cast<size_t>(col_index)].key;
    s->sort_desc = !ascending;
    s->sorted    = true;
    rebuild_view_no_lock(*s);
    return SAO_STATUS_OK;
}

// Fire row-click callback synchronously (for tests).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_fire_row_click(
    sao_ui_widget_handle_t handle,
    size_t view_index) {
    TableState* s = as_table(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    sao_ui_table_row_click_cb_t cb = nullptr;
    void* user = nullptr;
    int64_t row_id = 0;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (view_index >= s->rows_view.size()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        row_id = s->rows_all[s->rows_view[view_index]].row_id;
        cb = s->row_click_cb;
        user = s->row_click_user;
    }
    if (cb != nullptr) cb(row_id, user);
    return SAO_STATUS_OK;
}

// Shared destroy helper.
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_table_family_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr) return;
    uint32_t removed = 0;
    (void)sao_ui_widget_release_event_handlers(handle, &removed);
    if (peek_tag(handle) == kTableTag) {
        delete reinterpret_cast<TableState*>(handle);
    } else if (peek_tag(handle) == kTreeTag) {
        delete reinterpret_cast<TreeState*>(handle);
    }
}
