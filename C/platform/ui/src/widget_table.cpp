// SAO Auto — table widgets, virtual scrolling, hit testing, and sorting.
//
// This slice implements the Table portion of widget_table.h:
//   * sao_ui_table_create / _set_rows / _upsert_row / _remove_row /
//     _clear_rows / _set_sort / _set_filter / _set_row_click_handler
//
// Plus helpers for virtual-scroll visible-range calculation, cell
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
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(SAO_UI_COL_TEXT == 0, "column type enum drifted");
static_assert(SAO_UI_COL_RELTIME == 8, "column type enum drifted");
static_assert(SAO_UI_CELL_STRING == 0, "cell kind enum drifted");
static_assert(SAO_UI_CELL_BOOL == 3, "cell kind enum drifted");

namespace {

constexpr int32_t kTableTag = 145; // aligned with SAO_UI_WIDGET_TABLE_EXT
constexpr int32_t kTreeTag = 146;
constexpr size_t kMaxTableColumns = 16;
constexpr size_t kMaxTableRows = 200;
constexpr size_t kMaxTableCells = kMaxTableColumns * kMaxTableRows;
constexpr size_t kMaxTableStringBytes = 4000;
constexpr size_t kMaxTableUtf8Bytes = 16U * 1024U * 1024U;
constexpr size_t kMaxTreeNodes = 1024;
constexpr size_t kMaxTreeStringBytes = 4096;
constexpr size_t kMaxTreeUtf8Bytes = 64U * 1024U;
constexpr size_t kMaxTreeDepth = 64;

template <typename Function> sao_status_t table_abi_status(Function&& function) noexcept {
    try {
        return function();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

template <typename Function> void table_abi_void(Function&& function) noexcept {
    try {
        function();
    } catch (...) {
    }
}

bool checked_add_to_table_budget(size_t value, size_t limit, size_t* total) {
    if (total == nullptr || *total > limit || value > limit - *total) {
        return false;
    }
    *total += value;
    return true;
}

bool copy_bounded_table_string(const char* source, size_t* total_bytes, std::string* output) {
    if (total_bytes == nullptr || output == nullptr)
        return false;
    if (source == nullptr) {
        output->clear();
        return true;
    }
    size_t length = 0;
    while (length <= kMaxTableStringBytes && source[length] != '\0') {
        ++length;
    }
    if (length > kMaxTableStringBytes ||
        !checked_add_to_table_budget(length, kMaxTableUtf8Bytes, total_bytes)) {
        return false;
    }
    output->assign(source, length);
    return true;
}

bool copy_bounded_tree_string(const char* source, size_t* total_bytes, std::string* output) {
    if (total_bytes == nullptr || output == nullptr)
        return false;
    if (source == nullptr) {
        output->clear();
        return true;
    }
    size_t length = 0;
    while (length <= kMaxTreeStringBytes && source[length] != '\0') {
        ++length;
    }
    if (length > kMaxTreeStringBytes ||
        !checked_add_to_table_budget(length, kMaxTreeUtf8Bytes, total_bytes)) {
        return false;
    }
    output->assign(source, length);
    return true;
}

// Owned mirror of SaoUiTableColumn.  We copy the strings so the caller
// can free their buffers immediately after create() returns.
struct OwnedColumn {
    std::string key;
    std::string title;
    int32_t type{0};
    int32_t align{0};
    int32_t min_width_px{0};
    int32_t max_width_px{0};
    float flex_weight{0.0f};
    bool sortable{false};
    bool filterable{true};
    bool resizable{false};
    bool hidden{false};
    uint32_t header_bg_argb{0};
    uint32_t header_fg_argb{0};
    uint32_t cell_fg_argb{0};
    uint32_t cell_bg_alt_argb{0};
};

// Owned mirror of a single cell — we copy strings so callers can
// hand us stack-allocated const char*.
struct OwnedCell {
    int32_t kind{SAO_UI_CELL_STRING};
    std::string s;
    int64_t i64{0};
    double f64{0.0};
    bool b{false};
    double max_hint{0.0};
    uint32_t fg_argb{0};
    uint32_t bg_argb{0};
};

// Owned mirror of SaoUiTableRow.
struct OwnedRow {
    int64_t row_id{0};
    std::vector<OwnedCell> cells;
    bool highlight{false};
    bool mem_priority_badge{false};
    bool zebra_alt{false};
    bool dim{false};
    uint32_t row_bg_override_argb{0};
    uint32_t row_fg_override_argb{0};
};

template <typename Callback> struct CallbackSlot {
    Callback callback{nullptr};
    void* user_data{nullptr};
    uint64_t generation{1};
    std::unordered_map<uint64_t, size_t> in_flight;
    std::mutex transition_mtx;
};

// Default heights (Python action table uses row_height=22 for
// the DPS panel).
constexpr int32_t kDefaultRowHeight = 22;
constexpr int32_t kDefaultHeaderHeight = 24;

struct TableState {
    int32_t tag{kTableTag};
    SaoUiTableSpec spec{}; // pointer fields NULL after copy-in
    std::vector<OwnedColumn> columns;
    std::vector<OwnedRow> rows_all;
    // Ordered indices into rows_all, post filter + sort.
    std::vector<size_t> rows_view;
    // Sort state.
    std::string sort_key;
    bool sort_desc{false};
    bool sorted{false};
    // Filter state.
    std::string filter_text;
    // Callback wiring.
    CallbackSlot<sao_ui_table_row_click_cb_t> row_click;
    CallbackSlot<sao_ui_table_cell_action_cb_t> cell_action;
    bool callbacks_retired{false};
    std::condition_variable callback_cv;
    mutable std::mutex mtx;
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
    CallbackSlot<sao_ui_tree_select_cb_t> select;
    int64_t selected_node_id{0};
    bool callbacks_retired{false};
    std::condition_variable callback_cv;
    mutable std::mutex mtx;
};

thread_local std::vector<const void*> current_table_callback_states;

class TableCallbackScope {
  public:
    explicit TableCallbackScope(const void* state) : state_(state) {
        current_table_callback_states.push_back(state);
    }

    ~TableCallbackScope() {
        const auto found = std::find(current_table_callback_states.rbegin(),
                                     current_table_callback_states.rend(), state_);
        if (found != current_table_callback_states.rend()) {
            current_table_callback_states.erase(std::next(found).base());
        }
    }

    TableCallbackScope(const TableCallbackScope&) = delete;
    TableCallbackScope& operator=(const TableCallbackScope&) = delete;

  private:
    const void* state_;
};

bool callback_owns_table_state(const void* state) {
    return std::find(current_table_callback_states.begin(), current_table_callback_states.end(),
                     state) != current_table_callback_states.end();
}

uint64_t next_callback_generation(uint64_t generation) {
    ++generation;
    return generation == 0 ? 1 : generation;
}

template <typename State, typename Callback> class TableCallbackLease {
  public:
    TableCallbackLease(std::shared_ptr<State> state, CallbackSlot<Callback>* slot,
                       uint64_t generation)
        : state_(std::move(state)), slot_(slot), generation_(generation) {}

    ~TableCallbackLease() {
        release();
    }

    TableCallbackLease(const TableCallbackLease&) = delete;
    TableCallbackLease& operator=(const TableCallbackLease&) = delete;

  private:
    void release() noexcept {
        if (state_ == nullptr || slot_ == nullptr)
            return;
        try {
            {
                std::lock_guard<std::mutex> lock(state_->mtx);
                const auto found = slot_->in_flight.find(generation_);
                if (found != slot_->in_flight.end()) {
                    if (found->second > 1) {
                        --found->second;
                    } else {
                        slot_->in_flight.erase(found);
                    }
                }
            }
            state_->callback_cv.notify_all();
        } catch (...) {
        }
        state_.reset();
        slot_ = nullptr;
    }

    std::shared_ptr<State> state_;
    CallbackSlot<Callback>* slot_{nullptr};
    uint64_t generation_{0};
};

template <typename State, typename Callback>
sao_status_t replace_callback(const std::shared_ptr<State>& state, CallbackSlot<Callback>& slot,
                              Callback callback, void* user_data) {
    if (callback_owns_table_state(state.get()))
        return SAO_UI_STATUS_ERR_BUSY;
    std::lock_guard<std::mutex> transition_lock(slot.transition_mtx);
    std::unique_lock<std::mutex> lock(state->mtx);
    if (state->callbacks_retired)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    const uint64_t old_generation = slot.generation;
    slot.generation = next_callback_generation(slot.generation);
    slot.callback = callback;
    slot.user_data = user_data;
    state->callback_cv.wait(lock, [&] {
        const auto found = slot.in_flight.find(old_generation);
        return found == slot.in_flight.end() || found->second == 0;
    });
    slot.in_flight.erase(old_generation);
    return state->callbacks_retired ? SAO_STATUS_ERR_HANDLE_INVALID : SAO_STATUS_OK;
}

template <typename State, typename Callback>
bool callback_generation_is_current(const std::shared_ptr<State>& state,
                                    const CallbackSlot<Callback>& slot, uint64_t generation) {
    std::lock_guard<std::mutex> lock(state->mtx);
    return !state->callbacks_retired && slot.generation == generation;
}

template <typename Callback> bool callback_slot_is_idle(const CallbackSlot<Callback>& slot) {
    return std::all_of(slot.in_flight.begin(), slot.in_flight.end(),
                       [](const auto& item) { return item.second == 0; });
}

template <typename State>
std::shared_ptr<State> acquire_table_state(sao_ui_widget_handle_t handle, int32_t kind) {
    return std::static_pointer_cast<State>(sao::ui::detail::acquire_widget_handle(
        handle, sao::ui::detail::WidgetHandleFamily::table, kind));
}

std::shared_ptr<TableState> as_table(sao_ui_widget_handle_t handle) {
    return acquire_table_state<TableState>(handle, kTableTag);
}

std::shared_ptr<TreeState> as_tree(sao_ui_widget_handle_t handle) {
    return acquire_table_state<TreeState>(handle, kTreeTag);
}

template <typename State>
sao_status_t publish_table_state(int32_t kind, std::shared_ptr<State> state,
                                 sao_ui_widget_handle_t* out_handle) {
    void* const handle = sao::ui::detail::register_widget_handle(
        sao::ui::detail::WidgetHandleFamily::table, kind, std::move(state));
    if (handle == nullptr)
        return SAO_STATUS_ERR_UNKNOWN;
    *out_handle = reinterpret_cast<sao_ui_widget_handle_t>(handle);
    return SAO_STATUS_OK;
}

size_t find_tree_node_no_lock(const TreeState& tree, int64_t node_id) {
    for (size_t index = 0; index < tree.nodes.size(); ++index) {
        if (tree.nodes[index].node_id == node_id)
            return index;
    }
    return std::numeric_limits<size_t>::max();
}

void append_visible_tree_children(const std::vector<OwnedTreeNode>& nodes,
                                  std::vector<VisibleTreeNode>* visible, int64_t parent_id,
                                  size_t depth) {
    for (size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        if (node.parent_id != parent_id)
            continue;
        visible->push_back({index, static_cast<int32_t>(depth)});
        if (node.expanded) {
            append_visible_tree_children(nodes, visible, node.node_id, depth + 1);
        }
    }
}

std::vector<VisibleTreeNode> build_tree_visible(const std::vector<OwnedTreeNode>& nodes) {
    std::vector<VisibleTreeNode> visible;
    visible.reserve(nodes.size());
    append_visible_tree_children(nodes, &visible, 0, 0);
    return visible;
}

sao_status_t build_tree_candidate(const SaoUiTreeNode* nodes, size_t node_count,
                                  std::vector<OwnedTreeNode>* owned_out,
                                  std::vector<VisibleTreeNode>* visible_out) {
    if (owned_out == nullptr || visible_out == nullptr ||
        (nodes == nullptr && node_count != 0) || node_count > kMaxTreeNodes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    std::vector<OwnedTreeNode> next;
    next.reserve(node_count);
    std::unordered_map<int64_t, size_t> node_indices;
    node_indices.reserve(node_count);
    size_t total_bytes = 0;
    for (size_t index = 0; index < node_count; ++index) {
        if (nodes[index].node_id == 0)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (node_indices.contains(nodes[index].node_id))
            return SAO_STATUS_ERR_ALREADY_EXISTS;

        OwnedTreeNode owned{};
        owned.node_id = nodes[index].node_id;
        owned.parent_id = nodes[index].parent_id;
        if (!copy_bounded_tree_string(nodes[index].label_utf8, &total_bytes, &owned.label) ||
            !copy_bounded_tree_string(nodes[index].detail_utf8, &total_bytes, &owned.detail)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        owned.icon_slot = nodes[index].icon_slot;
        owned.expanded = nodes[index].expanded_default;
        owned.selectable = nodes[index].selectable;
        owned.fg_argb = nodes[index].fg_argb;
        owned.bg_argb = nodes[index].bg_argb;
        node_indices.emplace(owned.node_id, next.size());
        next.push_back(std::move(owned));
    }

    for (const auto& node : next) {
        int64_t parent_id = node.parent_id;
        size_t depth = 0;
        size_t traversed = 0;
        while (parent_id != 0) {
            const auto parent = node_indices.find(parent_id);
            if (parent == node_indices.end())
                return SAO_STATUS_ERR_NOT_FOUND;
            ++depth;
            ++traversed;
            if (depth > kMaxTreeDepth || traversed > next.size() || parent_id == node.node_id) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            parent_id = next[parent->second].parent_id;
        }
    }

    auto visible = build_tree_visible(next);
    *owned_out = std::move(next);
    *visible_out = std::move(visible);
    return SAO_STATUS_OK;
}

bool copy_column_no_lock(OwnedColumn& out, const SaoUiTableColumn& src, size_t* total_bytes) {
    if (!copy_bounded_table_string(src.key_utf8, total_bytes, &out.key) ||
        !copy_bounded_table_string(src.title_utf8, total_bytes, &out.title)) {
        return false;
    }
    out.type = src.type;
    out.align = src.align;
    out.min_width_px = src.min_width_px;
    out.max_width_px = src.max_width_px;
    out.flex_weight = src.flex_weight;
    out.sortable = src.sortable;
    out.filterable = src.filterable;
    out.resizable = src.resizable;
    out.hidden = src.hidden;
    out.header_bg_argb = src.header_bg_argb;
    out.header_fg_argb = src.header_fg_argb;
    out.cell_fg_argb = src.cell_fg_argb;
    out.cell_bg_alt_argb = src.cell_bg_alt_argb;
    return true;
}

bool copy_cell_no_lock(OwnedCell& out, const SaoUiCellValue& src, size_t* total_bytes) {
    out.kind = src.kind;
    switch (src.kind) {
    case SAO_UI_CELL_STRING:
        if (!copy_bounded_table_string(src.v.s_utf8, total_bytes, &out.s)) {
            return false;
        }
        break;
    case SAO_UI_CELL_INT64:
        out.i64 = src.v.i64;
        break;
    case SAO_UI_CELL_DOUBLE:
        out.f64 = src.v.f64;
        break;
    case SAO_UI_CELL_BOOL:
        out.b = src.v.b;
        break;
    default:
        break;
    }
    out.max_hint = src.max_hint;
    out.fg_argb = src.fg_argb;
    out.bg_argb = src.bg_argb;
    return true;
}

bool copy_row_no_lock(OwnedRow& out, const SaoUiTableRow& src, size_t* total_cells,
                      size_t* total_bytes) {
    if ((src.cells == nullptr && src.cell_count > 0) || src.cell_count > kMaxTableColumns ||
        !checked_add_to_table_budget(src.cell_count, kMaxTableCells, total_cells)) {
        return false;
    }
    OwnedRow candidate;
    candidate.row_id = src.row_id;
    candidate.highlight = src.highlight;
    candidate.mem_priority_badge = src.mem_priority_badge;
    candidate.zebra_alt = src.zebra_alt;
    candidate.dim = src.dim;
    candidate.row_bg_override_argb = src.row_bg_override_argb;
    candidate.row_fg_override_argb = src.row_fg_override_argb;
    candidate.cells.reserve(src.cell_count);
    for (size_t i = 0; i < src.cell_count; ++i) {
        OwnedCell cell;
        if (!copy_cell_no_lock(cell, src.cells[i], total_bytes))
            return false;
        candidate.cells.push_back(std::move(cell));
    }
    out = std::move(candidate);
    return true;
}

sao_status_t build_row_candidate(const SaoUiTableRow* rows, size_t row_count,
                                 std::vector<OwnedRow>* output) {
    if (output == nullptr || (rows == nullptr && row_count > 0) || row_count > kMaxTableRows) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::vector<OwnedRow> candidate;
    candidate.reserve(row_count);
    size_t total_cells = 0;
    size_t total_bytes = 0;
    for (size_t i = 0; i < row_count; ++i) {
        OwnedRow row;
        if (!copy_row_no_lock(row, rows[i], &total_cells, &total_bytes)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        candidate.push_back(std::move(row));
    }
    *output = std::move(candidate);
    return SAO_STATUS_OK;
}

bool rows_fit_table_budget(const std::vector<OwnedRow>& rows, size_t column_count) {
    if (rows.size() > kMaxTableRows || column_count > kMaxTableColumns) {
        return false;
    }
    size_t total_cells = 0;
    size_t total_bytes = 0;
    for (const auto& row : rows) {
        if (row.cells.size() > column_count ||
            !checked_add_to_table_budget(row.cells.size(), kMaxTableCells, &total_cells)) {
            return false;
        }
        for (const auto& cell : row.cells) {
            if (cell.s.size() > kMaxTableStringBytes ||
                !checked_add_to_table_budget(cell.s.size(), kMaxTableUtf8Bytes, &total_bytes)) {
                return false;
            }
        }
    }
    return true;
}

// Find column index by key.  Returns -1 if not found.
int32_t find_column_index_no_lock(const TableState& s, std::string_view key) {
    for (size_t i = 0; i < s.columns.size(); ++i) {
        if (s.columns[i].key == key)
            return static_cast<int32_t>(i);
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
        if (a.i64 < b.i64)
            return -1;
        if (a.i64 > b.i64)
            return 1;
        return 0;
    case SAO_UI_CELL_DOUBLE:
        if (a.f64 < b.f64)
            return -1;
        if (a.f64 > b.f64)
            return 1;
        return 0;
    case SAO_UI_CELL_BOOL:
        if (a.b == b.b)
            return 0;
        return a.b ? 1 : -1;
    default:
        return 0;
    }
}

// Return true if the row should be kept under the current filter.
// Empty filter → keep everything.  Otherwise: case-insensitive substring
// match against any filterable column's stringified cell value.
bool row_passes_filter_no_lock(const TableState& s, const OwnedRow& row) {
    if (s.filter_text.empty())
        return true;
    // Naive case-insensitive substring: lower-case the filter, then
    // compare cell-by-cell.  Only ASCII (matches the Python filter
    // widget which itself is ASCII-only at present).
    std::string needle = s.filter_text;
    for (auto& c : needle)
        c = static_cast<char>(std::tolower(c));
    for (size_t ci = 0; ci < s.columns.size() && ci < row.cells.size(); ++ci) {
        const OwnedColumn& col = s.columns[ci];
        if (!col.filterable)
            continue;
        const OwnedCell& cell = row.cells[ci];
        std::string hay;
        switch (cell.kind) {
        case SAO_UI_CELL_STRING:
            hay = cell.s;
            break;
        case SAO_UI_CELL_INT64:
            hay = std::to_string(cell.i64);
            break;
        case SAO_UI_CELL_DOUBLE:
            hay = std::to_string(cell.f64);
            break;
        case SAO_UI_CELL_BOOL:
            hay = cell.b ? "true" : "false";
            break;
        default:
            break;
        }
        for (auto& c : hay)
            c = static_cast<char>(std::tolower(c));
        if (hay.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// Rebuild rows_view from rows_all under the current sort/filter state.
// stable_sort keeps insertion order among equal keys.
std::vector<size_t> build_view_no_lock(const TableState& s, const std::vector<OwnedRow>& rows) {
    std::vector<size_t> view;
    view.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        if (row_passes_filter_no_lock(s, rows[i])) {
            view.push_back(i);
        }
    }
    if (!s.sorted || s.sort_key.empty())
        return view;
    const int32_t col_idx = find_column_index_no_lock(s, s.sort_key);
    if (col_idx < 0)
        return view;
    const bool desc = s.sort_desc;
    std::stable_sort(view.begin(), view.end(), [&](size_t la, size_t lb) {
        const OwnedRow& ra = rows[la];
        const OwnedRow& rb = rows[lb];
        if (static_cast<size_t>(col_idx) >= ra.cells.size() ||
            static_cast<size_t>(col_idx) >= rb.cells.size()) {
            return false;
        }
        const int cmp = compare_cells(ra.cells[col_idx], rb.cells[col_idx]);
        if (cmp == 0)
            return false;
        return desc ? (cmp > 0) : (cmp < 0);
    });
    return view;
}

void rebuild_view_no_lock(TableState& s) {
    s.rows_view = build_view_no_lock(s, s.rows_all);
}

// Find an existing row by row_id.  Returns iterator (end() if missing).
std::vector<OwnedRow>::iterator find_row_by_id_no_lock(TableState& s, int64_t row_id) {
    return std::find_if(s.rows_all.begin(), s.rows_all.end(),
                        [row_id](const OwnedRow& r) { return r.row_id == row_id; });
}

} // namespace

// ---------------------------------------------------------------------------
// Table ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_create(void* /*d3d_device_ptr*/,
                                                        const SaoUiTableSpec* spec,
                                                        sao_ui_widget_handle_t* out_handle) {
    return table_abi_status([&]() -> sao_status_t {
        if (out_handle != nullptr)
            *out_handle = nullptr;
        if (spec == nullptr || out_handle == nullptr ||
            (spec->columns == nullptr && spec->column_count > 0) ||
            spec->column_count > kMaxTableColumns) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        auto state = std::make_shared<TableState>();
        state->spec = *spec;
        state->spec.columns = nullptr;
        state->spec.column_count = 0;
        state->spec.initial_sort_key_utf8 = nullptr;
        state->spec.initial_filter_utf8 = nullptr;
        size_t total_bytes = 0;
        state->columns.reserve(spec->column_count);
        for (size_t i = 0; i < spec->column_count; ++i) {
            OwnedColumn column;
            if (!copy_column_no_lock(column, spec->columns[i], &total_bytes)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            state->columns.push_back(std::move(column));
        }
        if (!copy_bounded_table_string(spec->initial_sort_key_utf8, &total_bytes,
                                       &state->sort_key) ||
            !copy_bounded_table_string(spec->initial_filter_utf8, &total_bytes,
                                       &state->filter_text)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        state->sort_desc = spec->initial_sort_desc;
        state->sorted = !state->sort_key.empty();
        if (state->spec.row_height_px <= 0) {
            state->spec.row_height_px = kDefaultRowHeight;
        }
        if (state->spec.header_height_px <= 0) {
            state->spec.header_height_px = kDefaultHeaderHeight;
        }
        return publish_table_state(kTableTag, std::move(state), out_handle);
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_set_rows(sao_ui_widget_handle_t handle,
                                                          const SaoUiTableRow* rows,
                                                          size_t row_count) {
    return table_abi_status([&]() -> sao_status_t {
        auto state = as_table(handle);
        if (state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::vector<OwnedRow> next_rows;
        const sao_status_t build_status = build_row_candidate(rows, row_count, &next_rows);
        if (build_status != SAO_STATUS_OK)
            return build_status;
        std::lock_guard<std::mutex> lock(state->mtx);
        if (!rows_fit_table_budget(next_rows, state->columns.size())) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        auto next_view = build_view_no_lock(*state, next_rows);
        state->rows_all = std::move(next_rows);
        state->rows_view = std::move(next_view);
        return SAO_STATUS_OK;
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_upsert_row(sao_ui_widget_handle_t handle,
                                                            const SaoUiTableRow* row) {
    return table_abi_status([&]() -> sao_status_t {
        if (row == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        auto state = as_table(handle);
        if (state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        size_t total_cells = 0;
        size_t total_bytes = 0;
        OwnedRow replacement;
        if (!copy_row_no_lock(replacement, *row, &total_cells, &total_bytes)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(state->mtx);
        std::vector<OwnedRow> next_rows = state->rows_all;
        const auto found =
            std::find_if(next_rows.begin(), next_rows.end(),
                         [row](const OwnedRow& current) { return current.row_id == row->row_id; });
        if (found == next_rows.end()) {
            if (next_rows.size() >= kMaxTableRows) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            next_rows.push_back(std::move(replacement));
        } else {
            *found = std::move(replacement);
        }
        if (!rows_fit_table_budget(next_rows, state->columns.size())) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        auto next_view = build_view_no_lock(*state, next_rows);
        state->rows_all = std::move(next_rows);
        state->rows_view = std::move(next_view);
        return SAO_STATUS_OK;
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_remove_row(sao_ui_widget_handle_t handle,
                                                            int64_t row_id) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(s->mtx);
        auto it = find_row_by_id_no_lock(*s, row_id);
        if (it == s->rows_all.end())
            return SAO_STATUS_ERR_NOT_FOUND;
        s->rows_all.erase(it);
        rebuild_view_no_lock(*s);
        return SAO_STATUS_OK;
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_clear_rows(sao_ui_widget_handle_t handle) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(s->mtx);
        s->rows_all.clear();
        s->rows_view.clear();
        return SAO_STATUS_OK;
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_set_sort(sao_ui_widget_handle_t handle,
                                                          const char* column_key_utf8,
                                                          bool descending) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::string sort_key;
        size_t total_bytes = 0;
        if (!copy_bounded_table_string(column_key_utf8, &total_bytes, &sort_key))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lk(s->mtx);
        s->sort_key = std::move(sort_key);
        s->sort_desc = s->sort_key.empty() ? false : descending;
        s->sorted = !s->sort_key.empty();
        rebuild_view_no_lock(*s);
        return SAO_STATUS_OK;
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_set_filter(sao_ui_widget_handle_t handle,
                                                            const char* filter_utf8) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::string filter;
        size_t total_bytes = 0;
        if (!copy_bounded_table_string(filter_utf8, &total_bytes, &filter))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lk(s->mtx);
        s->filter_text = std::move(filter);
        rebuild_view_no_lock(*s);
        return SAO_STATUS_OK;
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_set_row_click_handler(
    sao_ui_widget_handle_t handle, sao_ui_table_row_click_cb_t callback, void* user_data) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        return replace_callback(s, s->row_click, callback, user_data);
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_table_set_cell_action_handler(
    sao_ui_widget_handle_t handle, sao_ui_table_cell_action_cb_t callback, void* user_data) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        return replace_callback(s, s->cell_action, callback, user_data);
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_create(void*, const SaoUiTreeViewSpec* spec,
                                                            sao_ui_widget_handle_t* out) {
    return table_abi_status([&]() -> sao_status_t {
        if (out != nullptr)
            *out = nullptr;
        if (out == nullptr || spec == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        auto tree = std::make_shared<TreeState>();
        tree->spec = *spec;
        if (tree->spec.row_height_px <= 0)
            tree->spec.row_height_px = kDefaultRowHeight;
        if (tree->spec.indent_px <= 0)
            tree->spec.indent_px = 14;
        if (tree->spec.caret_width_px <= 0)
            tree->spec.caret_width_px = 10;
        return publish_table_state(kTreeTag, std::move(tree), out);
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_set_nodes(sao_ui_widget_handle_t handle,
                                                               const SaoUiTreeNode* nodes,
                                                               size_t node_count) {
    return table_abi_status([&]() -> sao_status_t {
        auto tree = as_tree(handle);
        if (tree == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::vector<OwnedTreeNode> next_nodes;
        std::vector<VisibleTreeNode> next_visible;
        const sao_status_t build_status =
            build_tree_candidate(nodes, node_count, &next_nodes, &next_visible);
        if (build_status != SAO_STATUS_OK)
            return build_status;
        std::lock_guard<std::mutex> lock(tree->mtx);
        tree->nodes = std::move(next_nodes);
        tree->visible = std::move(next_visible);
        tree->selected_node_id = 0;
        return SAO_STATUS_OK;
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_expand_node(sao_ui_widget_handle_t handle,
                                                                 int64_t node_id, bool expanded) {
    return table_abi_status([&]() -> sao_status_t {
        auto tree = as_tree(handle);
        if (tree == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(tree->mtx);
        const size_t index = find_tree_node_no_lock(*tree, node_id);
        if (index == std::numeric_limits<size_t>::max())
            return SAO_STATUS_ERR_NOT_FOUND;
        std::vector<OwnedTreeNode> next_nodes = tree->nodes;
        next_nodes[index].expanded = expanded;
        auto next_visible = build_tree_visible(next_nodes);
        tree->nodes = std::move(next_nodes);
        tree->visible = std::move(next_visible);
        return SAO_STATUS_OK;
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_set_select_handler(
    sao_ui_widget_handle_t handle, sao_ui_tree_select_cb_t callback, void* user_data) {
    return table_abi_status([&]() -> sao_status_t {
        auto tree = as_tree(handle);
        if (tree == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        return replace_callback(tree, tree->select, callback, user_data);
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_select_node(sao_ui_widget_handle_t handle,
                                                                 int64_t node_id) {
    return table_abi_status([&]() -> sao_status_t {
        auto tree = as_tree(handle);
        if (tree == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_tree_select_cb_t callback = nullptr;
        void* user_data = nullptr;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(tree->mtx);
            if (tree->callbacks_retired)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            const size_t index = find_tree_node_no_lock(*tree, node_id);
            if (index == std::numeric_limits<size_t>::max())
                return SAO_STATUS_ERR_NOT_FOUND;
            if (!tree->nodes[index].selectable)
                return SAO_STATUS_ERR_ACCESS_DENIED;
            const bool visible =
                std::any_of(tree->visible.begin(), tree->visible.end(), [index](const auto& item) {
                    return item.node_index == index;
                });
            if (!visible)
                return SAO_STATUS_ERR_NOT_FOUND;
            tree->selected_node_id = node_id;
            callback = tree->select.callback;
            user_data = tree->select.user_data;
            if (callback != nullptr) {
                generation = tree->select.generation;
                ++tree->select.in_flight[generation];
            }
        }
        if (callback == nullptr)
            return SAO_STATUS_OK;

        TableCallbackLease<TreeState, sao_ui_tree_select_cb_t> lease(tree, &tree->select,
                                                                     generation);
        TableCallbackScope callback_scope(tree.get());
        try {
            callback(node_id, user_data);
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        return callback_generation_is_current(tree, tree->select, generation)
                   ? SAO_STATUS_OK
                   : SAO_UI_STATUS_ERR_BUSY;
    });
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_tree_view_get_visible_count(sao_ui_widget_handle_t handle, size_t* out_count) {
    return table_abi_status([&]() -> sao_status_t {
        auto tree = as_tree(handle);
        if (tree == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (out_count == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lock(tree->mtx);
        *out_count = tree->visible.size();
        return SAO_STATUS_OK;
    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tree_view_get_visible_node(sao_ui_widget_handle_t handle,
                                                                      size_t visible_index,
                                                                      int64_t* out_node_id,
                                                                      int32_t* out_depth) {
    return table_abi_status([&]() -> sao_status_t {
        auto tree = as_tree(handle);
        if (tree == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (out_node_id == nullptr || out_depth == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lock(tree->mtx);
        if (visible_index >= tree->visible.size())
            return SAO_STATUS_ERR_NOT_FOUND;
        const VisibleTreeNode item = tree->visible[visible_index];
        *out_node_id = tree->nodes[item.node_index].node_id;
        *out_depth = item.depth;
        return SAO_STATUS_OK;
    });
}

// ---------------------------------------------------------------------------
// Table helper API — visible-range for virtual scroll + cell hit test
// + row-id enumeration for tests.
// ---------------------------------------------------------------------------

struct SaoUiPointF {
    float x;
    float y;
};

// Number of visible rows post-filter (rows_view size).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_get_visible_row_count(sao_ui_widget_handle_t handle, size_t* out_count) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(s->mtx);
        if (out_count)
            *out_count = s->rows_view.size();
        return SAO_STATUS_OK;
    });
}

// Row id at a specific position within the post-filter/sort view.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_table_get_row_id_at_view_index(
    sao_ui_widget_handle_t handle, size_t view_index, int64_t* out_row_id) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(s->mtx);
        if (view_index >= s->rows_view.size()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        const size_t master_idx = s->rows_view[view_index];
        if (out_row_id)
            *out_row_id = s->rows_all[master_idx].row_id;
        return SAO_STATUS_OK;
    });
}

// Compute first/last-visible-row indices in the current view under a
// given scroll offset + viewport height.  Both indices are inclusive;
// last is clamped to view size - 1.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_table_get_visible_range(
    sao_ui_widget_handle_t handle, int32_t viewport_h_px, int32_t scroll_offset_px,
    size_t* first_row_out, size_t* last_row_out) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (viewport_h_px <= 0)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lk(s->mtx);
        const int32_t row_h = s->spec.row_height_px;
        if (row_h <= 0)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (s->rows_view.empty()) {
            if (first_row_out)
                *first_row_out = 0;
            if (last_row_out)
                *last_row_out = 0;
            return SAO_STATUS_OK;
        }
        const int32_t body_offset = std::max(int32_t{0}, scroll_offset_px);
        const size_t first = static_cast<size_t>(body_offset / row_h);
        // last inclusive: cover any row that has any pixel visible in the
        // viewport.  Ceiling divide against the trailing edge.
        const size_t last_exclusive =
            static_cast<size_t>((body_offset + viewport_h_px + row_h - 1) / row_h);
        const size_t view_size = s->rows_view.size();
        const size_t clamped_first = std::min(first, view_size > 0 ? view_size - 1 : 0);
        const size_t clamped_last = std::min(last_exclusive == 0 ? 0 : last_exclusive - 1,
                                             view_size > 0 ? view_size - 1 : 0);
        if (first_row_out)
            *first_row_out = clamped_first;
        if (last_row_out)
            *last_row_out = clamped_last;
        return SAO_STATUS_OK;
    });
}

// Hit test: point is in body-local coords (0,0 = top-left of body area,
// after the header).  Returns row/col in the current view.  When the
// hit is on the header, out_row_view_index is set to SIZE_MAX and
// out_col_index still resolves.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_table_hit_test(
    sao_ui_widget_handle_t handle, SaoUiPointF point, int32_t total_width_px,
    int32_t scroll_offset_px, size_t* out_row_view_index, int32_t* out_col_index) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(s->mtx);
        if (out_row_view_index)
            *out_row_view_index = std::numeric_limits<size_t>::max();
        if (out_col_index)
            *out_col_index = -1;
        if (point.x < 0.0f || point.y < 0.0f)
            return SAO_STATUS_OK;
        // Row selection: divide by row_h + scroll offset.
        const int32_t row_h = s->spec.row_height_px;
        if (row_h > 0) {
            const int32_t abs_y =
                static_cast<int32_t>(point.y) + std::max(int32_t{0}, scroll_offset_px);
            const size_t row_idx = static_cast<size_t>(abs_y / row_h);
            if (row_idx < s->rows_view.size()) {
                if (out_row_view_index)
                    *out_row_view_index = row_idx;
            }
        }
        // Column selection: distribute total_width_px across columns by
        // (max(min_width_px, flex_weight * remainder)).  Simple version:
        // start with each column's min_width_px; give any leftover to
        // flex_weight-proportional columns.
        if (s->columns.empty() || total_width_px <= 0)
            return SAO_STATUS_OK;
        std::vector<int32_t> widths(s->columns.size(), 0);
        int32_t used = 0;
        float total_weight = 0.0f;
        for (size_t i = 0; i < s->columns.size(); ++i) {
            if (s->columns[i].hidden) {
                widths[i] = 0;
                continue;
            }
            widths[i] = std::max(int32_t{1}, s->columns[i].min_width_px);
            used += widths[i];
            total_weight += std::max(0.0f, s->columns[i].flex_weight);
        }
        const int32_t remainder = std::max(int32_t{0}, total_width_px - used);
        if (remainder > 0 && total_weight > 0.0f) {
            for (size_t i = 0; i < s->columns.size(); ++i) {
                if (s->columns[i].hidden)
                    continue;
                const float w = std::max(0.0f, s->columns[i].flex_weight);
                widths[i] +=
                    static_cast<int32_t>(static_cast<float>(remainder) * (w / total_weight));
            }
        }
        // Locate x.
        int32_t x_accum = 0;
        for (size_t i = 0; i < widths.size(); ++i) {
            if (s->columns[i].hidden)
                continue;
            const int32_t next = x_accum + widths[i];
            if (point.x >= static_cast<float>(x_accum) && point.x < static_cast<float>(next)) {
                if (out_col_index)
                    *out_col_index = static_cast<int32_t>(i);
                break;
            }
            x_accum = next;
        }
        return SAO_STATUS_OK;
    });
}

// Sort by column index — public wrapper for tests that don't want to
// look up the column key themselves.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_table_sort_by_index(
    sao_ui_widget_handle_t handle, int32_t col_index, bool ascending) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(s->mtx);
        if (col_index < 0 || static_cast<size_t>(col_index) >= s->columns.size()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        s->sort_key = s->columns[static_cast<size_t>(col_index)].key;
        s->sort_desc = !ascending;
        s->sorted = true;
        rebuild_view_no_lock(*s);
        return SAO_STATUS_OK;
    });
}

// Fire row-click callback synchronously (for tests).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_fire_row_click(sao_ui_widget_handle_t handle, size_t view_index) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_table_row_click_cb_t callback = nullptr;
        void* user_data = nullptr;
        int64_t row_id = 0;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(s->mtx);
            if (s->callbacks_retired)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            if (view_index >= s->rows_view.size())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            row_id = s->rows_all[s->rows_view[view_index]].row_id;
            callback = s->row_click.callback;
            user_data = s->row_click.user_data;
            if (callback != nullptr) {
                generation = s->row_click.generation;
                ++s->row_click.in_flight[generation];
            }
        }
        if (callback == nullptr)
            return SAO_STATUS_OK;

        TableCallbackLease<TableState, sao_ui_table_row_click_cb_t> lease(
            s, &s->row_click, generation);
        TableCallbackScope callback_scope(s.get());
        try {
            callback(row_id, user_data);
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        return callback_generation_is_current(s, s->row_click, generation)
                   ? SAO_STATUS_OK
                   : SAO_UI_STATUS_ERR_BUSY;
    });
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_table_fire_cell_action(
    sao_ui_widget_handle_t handle, size_t view_index, int32_t column_index) {
    return table_abi_status([&]() -> sao_status_t {
        auto s = as_table(handle);
        if (s == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_table_cell_action_cb_t callback = nullptr;
        void* user_data = nullptr;
        int64_t row_id = 0;
        std::string column_key;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(s->mtx);
            if (s->callbacks_retired)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            if (view_index >= s->rows_view.size() || column_index < 0 ||
                static_cast<size_t>(column_index) >= s->columns.size()) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            row_id = s->rows_all[s->rows_view[view_index]].row_id;
            column_key = s->columns[static_cast<size_t>(column_index)].key;
            callback = s->cell_action.callback;
            user_data = s->cell_action.user_data;
            if (callback != nullptr) {
                generation = s->cell_action.generation;
                ++s->cell_action.in_flight[generation];
            }
        }
        if (callback == nullptr)
            return SAO_STATUS_OK;

        TableCallbackLease<TableState, sao_ui_table_cell_action_cb_t> lease(
            s, &s->cell_action, generation);
        TableCallbackScope callback_scope(s.get());
        try {
            callback(row_id, column_key.c_str(), user_data);
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        return callback_generation_is_current(s, s->cell_action, generation)
                   ? SAO_STATUS_OK
                   : SAO_UI_STATUS_ERR_BUSY;
    });
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_tree_get_selected_node(sao_ui_widget_handle_t handle, int64_t* out_node_id) {
    return table_abi_status([&]() -> sao_status_t {
        auto tree = as_tree(handle);
        if (tree == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (out_node_id == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lock(tree->mtx);
        *out_node_id = tree->selected_node_id;
        return SAO_STATUS_OK;
    });
}

// Shared destroy helper.
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_table_family_destroy(sao_ui_widget_handle_t handle) {
    table_abi_void([&] {
        if (handle == nullptr)
            return;
        sao::ui::detail::WidgetHandleMetadata metadata{};
        auto state = sao::ui::detail::retire_widget_handle(
            handle, sao::ui::detail::WidgetHandleFamily::table, &metadata);
        if (state == nullptr)
            return;

        if (metadata.kind == kTableTag) {
            auto table = std::static_pointer_cast<TableState>(state);
            {
                std::lock_guard<std::mutex> lock(table->mtx);
                table->callbacks_retired = true;
                table->row_click.generation =
                    next_callback_generation(table->row_click.generation);
                table->cell_action.generation =
                    next_callback_generation(table->cell_action.generation);
                table->row_click.callback = nullptr;
                table->row_click.user_data = nullptr;
                table->cell_action.callback = nullptr;
                table->cell_action.user_data = nullptr;
            }
            table->callback_cv.notify_all();
            uint32_t removed = 0;
            (void)sao::ui::detail::release_widget_event_handlers(handle, &removed);
            if (callback_owns_table_state(table.get()))
                return;
            std::unique_lock<std::mutex> lock(table->mtx);
            table->callback_cv.wait(lock, [&] {
                return callback_slot_is_idle(table->row_click) &&
                       callback_slot_is_idle(table->cell_action);
            });
            table->row_click.in_flight.clear();
            table->cell_action.in_flight.clear();
            return;
        }

        if (metadata.kind == kTreeTag) {
            auto tree = std::static_pointer_cast<TreeState>(state);
            {
                std::lock_guard<std::mutex> lock(tree->mtx);
                tree->callbacks_retired = true;
                tree->select.generation = next_callback_generation(tree->select.generation);
                tree->select.callback = nullptr;
                tree->select.user_data = nullptr;
            }
            tree->callback_cv.notify_all();
            uint32_t removed = 0;
            (void)sao::ui::detail::release_widget_event_handlers(handle, &removed);
            if (callback_owns_table_state(tree.get()))
                return;
            std::unique_lock<std::mutex> lock(tree->mtx);
            tree->callback_cv.wait(lock,
                                   [&] { return callback_slot_is_idle(tree->select); });
            tree->select.in_flight.clear();
            return;
        }

        uint32_t removed = 0;
        (void)sao::ui::detail::release_widget_event_handlers(handle, &removed);
    });
}
