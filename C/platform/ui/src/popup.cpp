// SAO Auto — popup keyboard-nav + hierarchical hit-test first slice.
//
// Logic only, no rendering.  Owns the entry
// tree (with submenus), the selection state machine (UP/DOWN/LEFT/
// RIGHT/ENTER/ESC) and the hit-test cursor→entry_id resolver.
//
// 1:1 with `ui_gpu/popup.py` navigation semantics.  Render callbacks
// come from the compositor and are populated by later slices.

#include "sao/ui/popup.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Deep-copied entry node — the caller's SaoUiPopupEntry array is a
// transient POD, we build a heap-owned tree so mutations (open/close
// submenu, hover) are safe past the show() return.
struct EntryNode {
    std::string label;
    std::string icon;
    std::string accelerator;
    int32_t     entry_id = -1;
    bool        enabled = true;
    bool        checked = false;
    bool        is_separator = false;
    // Sub-entries; empty when leaf.
    std::vector<EntryNode> children;
};

// Cache-aware layout: computed once per level open, invalidated on
// refresh_entries.  Mirrors the [连续反馈不对要停止调参数] memory:
// content_w/h and origin must both be recomputed together every tick
// when the anchor shifts.
struct LevelRect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    // Per-visible-row rect list, aligned with `entries` after skipping
    // separators (they get half-height rows).
    std::vector<int32_t> row_top;      // top-y per entry
    std::vector<int32_t> row_bottom;   // bottom-y (exclusive) per entry
};

struct OpenLevel {
    // Path from root down: indices into parent's `children` at each
    // depth.  Level 0 is the root popup itself (index unused).
    int32_t parent_entry_index = -1;   // -1 for root
    int32_t selected_index = -1;       // active hover / arrow-nav row
    LevelRect rect;
};

// Row size heuristics.  These stay compile-time constant per the
// header contract; the trap is caching *some* of them but not the
// anchor.  We only cache row_top/bottom (deterministic from anchor).
constexpr int32_t kRowHeight = 28;
constexpr int32_t kSeparatorHeight = 8;

}  // namespace

struct sao_ui_popup_s {
    std::mutex mu;

    // Configured at create — never mutated.
    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_theme_handle_t theme = nullptr;

    // Set by show().
    bool visible = false;
    SaoUiPopupSpec spec_snapshot {};   // shallow copy; entries deep-copied below.
    EntryNode root;                    // synthetic root whose children are the top-level entries
    sao_ui_popup_result_callback_t callback = nullptr;
    void* user_data = nullptr;

    // Navigation stack.  levels[0] is always the root popup; each
    // deeper level represents an opened submenu.  On LEFT we pop; on
    // RIGHT into an item with children we push.
    std::vector<OpenLevel> levels;
};

// ── helpers (internal) ────────────────────────────────────────────

static void deep_copy_entries(
    const SaoUiPopupEntry* entries, size_t count,
    std::vector<EntryNode>* out) {
    out->clear();
    if (entries == nullptr || count == 0) return;
    out->reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const SaoUiPopupEntry& src = entries[i];
        EntryNode node;
        node.label = (src.label_utf8 != nullptr) ? src.label_utf8 : "";
        node.icon = (src.icon_utf8 != nullptr) ? src.icon_utf8 : "";
        node.accelerator = (src.accelerator_utf8 != nullptr) ? src.accelerator_utf8 : "";
        node.entry_id = src.entry_id;
        node.enabled = src.enabled;
        node.checked = src.checked;
        node.is_separator = src.is_separator;
        if (src.submenu_entries != nullptr && src.submenu_count > 0) {
            deep_copy_entries(src.submenu_entries, src.submenu_count, &node.children);
        }
        out->push_back(std::move(node));
    }
}

// Compute the rect for a level given its anchor.  Root is anchored at
// the show() anchor; a submenu is anchored at the right edge of its
// parent's selected row.
static void compute_level_rect(
    const std::vector<EntryNode>& entries,
    int32_t anchor_x, int32_t anchor_y,
    int32_t width,
    LevelRect* out) {
    out->x = anchor_x;
    out->y = anchor_y;
    out->width = width;
    out->row_top.clear();
    out->row_bottom.clear();
    out->row_top.reserve(entries.size());
    out->row_bottom.reserve(entries.size());
    int32_t cursor_y = anchor_y;
    for (const EntryNode& e : entries) {
        int32_t h = e.is_separator ? kSeparatorHeight : kRowHeight;
        out->row_top.push_back(cursor_y);
        out->row_bottom.push_back(cursor_y + h);
        cursor_y += h;
    }
    out->height = cursor_y - anchor_y;
}

// Get pointer to the entry vector for a given level path.  levels[0]
// is root (entries = popup->root.children); deeper levels drill down.
static const std::vector<EntryNode>& level_entries(
    const sao_ui_popup_s* popup, int32_t level_idx) {
    const EntryNode* node = &popup->root;
    for (int32_t i = 1; i <= level_idx; ++i) {
        int32_t parent_idx = popup->levels[static_cast<size_t>(i)].parent_entry_index;
        node = &node->children[static_cast<size_t>(parent_idx)];
    }
    return node->children;
}

// Walk from current selection: skip separators + disabled rows.
// direction = +1 (down) or -1 (up).  Wraps.  Returns new index or -1
// if no eligible entry.
static int32_t advance_selection(
    const std::vector<EntryNode>& entries, int32_t current, int32_t direction) {
    if (entries.empty()) return -1;
    const int32_t n = static_cast<int32_t>(entries.size());
    // Start from current (or -1 → wrap to first candidate).
    int32_t start = current;
    if (start < 0) start = (direction > 0) ? -1 : n;   // pretend "past" boundary
    for (int32_t step = 0; step < n; ++step) {
        int32_t candidate = start + direction * (step + 1);
        // Wrap modulo n.
        candidate = ((candidate % n) + n) % n;
        const EntryNode& e = entries[static_cast<size_t>(candidate)];
        if (!e.is_separator && e.enabled) {
            return candidate;
        }
    }
    return -1;   // nothing selectable
}

// ── layout constants (unchanged from stub) ────────────────────────
extern "C" const SaoUiPopupLayoutConsts* SAO_UI_CALL
sao_ui_popup_layout_constants(void) {
    static constexpr SaoUiPopupLayoutConsts kConsts = {
        /*menu_size=*/54,
        /*menu_max_size=*/70,
        /*menu_slot=*/70,
        /*menu_width=*/70,
        /*menu_max_visible=*/9,
        /*child_width=*/240,
        /*child_row_stride=*/40,
        /*child_slide_ms=*/240,
        /*hud_pad=*/40,
        /*hud_margin=*/12,
        /*gap_menu_child=*/25,
    };
    return &kConsts;
}

// ── create / destroy ──────────────────────────────────────────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_create(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    sao_ui_popup_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    auto* p = new (std::nothrow) sao_ui_popup_s;
    if (p == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    p->compositor = compositor;
    p->theme = theme;
    *out_handle = p;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_popup_destroy(sao_ui_popup_handle_t handle) {
    delete handle;
}

// ── show / hide ───────────────────────────────────────────────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_show(
    sao_ui_popup_handle_t handle,
    const SaoUiPopupSpec* spec,
    sao_ui_popup_result_callback_t callback,
    void* user_data) {
    if (handle == nullptr || spec == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    // Deep copy entries into our owned tree; the caller's array can go
    // out of scope any time after show() returns.
    deep_copy_entries(spec->entries, spec->entry_count, &handle->root.children);
    handle->spec_snapshot = *spec;
    // Null out the transient entries pointer so nobody accidentally
    // uses the caller's buffer past this point.
    handle->spec_snapshot.entries = nullptr;
    handle->spec_snapshot.entry_count = 0;
    handle->callback = callback;
    handle->user_data = user_data;
    handle->visible = true;
    // Reset navigation stack to just the root.
    handle->levels.clear();
    OpenLevel root_level;
    root_level.parent_entry_index = -1;
    // Anchor width — use spec.anchor_w if non-zero, else the child_width
    // constant (matches popup.py fallback).
    const auto* consts = sao_ui_popup_layout_constants();
    int32_t width = spec->anchor_w > 0 ? spec->anchor_w : consts->child_width;
    compute_level_rect(handle->root.children,
                       spec->anchor_x, spec->anchor_y, width,
                       &root_level.rect);
    // Pre-select first enabled non-separator (matches Python's
    // "keyboard-open lands on first item" behavior).
    root_level.selected_index = advance_selection(handle->root.children, -1, +1);
    handle->levels.push_back(std::move(root_level));
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_hide(sao_ui_popup_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_ui_popup_result_callback_t cb = nullptr;
    void* ud = nullptr;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        if (!handle->visible) return SAO_STATUS_OK;
        handle->visible = false;
        cb = handle->callback;
        ud = handle->user_data;
        handle->callback = nullptr;
        handle->user_data = nullptr;
        handle->levels.clear();
    }
    if (cb != nullptr) {
        cb(/*chosen_entry_id=*/-1, /*dismissed=*/true,
           /*screen_x=*/-1, /*screen_y=*/-1, ud);
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_is_visible(
    sao_ui_popup_handle_t handle, bool* out_visible) {
    if (handle == nullptr || out_visible == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    *out_visible = handle->visible;
    return SAO_STATUS_OK;
}

// ── runtime mutation ──────────────────────────────────────────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_refresh_entries(
    sao_ui_popup_handle_t handle,
    const SaoUiPopupEntry* entries, size_t count) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    deep_copy_entries(entries, count, &handle->root.children);
    // Rebuild root level rect + drop any submenu levels (their paths
    // are invalidated).
    handle->levels.clear();
    OpenLevel root_level;
    root_level.parent_entry_index = -1;
    const auto* consts = sao_ui_popup_layout_constants();
    int32_t width = handle->spec_snapshot.anchor_w > 0
        ? handle->spec_snapshot.anchor_w : consts->child_width;
    compute_level_rect(handle->root.children,
                       handle->spec_snapshot.anchor_x,
                       handle->spec_snapshot.anchor_y,
                       width,
                       &root_level.rect);
    root_level.selected_index = advance_selection(handle->root.children, -1, +1);
    handle->levels.push_back(std::move(root_level));
    return SAO_STATUS_OK;
}

static EntryNode* find_by_id_mut(EntryNode& root, int32_t entry_id) {
    for (EntryNode& child : root.children) {
        if (child.entry_id == entry_id) return &child;
        EntryNode* deep = find_by_id_mut(child, entry_id);
        if (deep != nullptr) return deep;
    }
    return nullptr;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_set_entry_checked(
    sao_ui_popup_handle_t handle, int32_t entry_id, bool checked) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    EntryNode* n = find_by_id_mut(handle->root, entry_id);
    if (n == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    n->checked = checked;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_set_entry_enabled(
    sao_ui_popup_handle_t handle, int32_t entry_id, bool enabled) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    EntryNode* n = find_by_id_mut(handle->root, entry_id);
    if (n == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    n->enabled = enabled;
    return SAO_STATUS_OK;
}

// ── keyboard nav — the state machine ──────────────────────────────
//
// This is the core of G3.4.  All navigation happens on the innermost
// open level (handle->levels.back()); LEFT collapses that level (or
// dismisses if at root); RIGHT pushes a new level when the selected
// entry has children; ENTER fires the callback for the selected entry
// (or expands if it's a container-only row); ESC dismisses regardless
// of depth.
extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_key_press(
    sao_ui_popup_handle_t handle, SaoUiPopupNavKey key) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // ESC path fires the dismiss callback outside the lock.
    sao_ui_popup_result_callback_t dismiss_cb = nullptr;
    void* dismiss_ud = nullptr;
    sao_ui_popup_result_callback_t choose_cb = nullptr;
    void* choose_ud = nullptr;
    int32_t chosen_entry = -1;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        if (!handle->visible || handle->levels.empty()) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        OpenLevel& top = handle->levels.back();
        const int32_t top_depth = static_cast<int32_t>(handle->levels.size()) - 1;
        const std::vector<EntryNode>& entries = level_entries(handle, top_depth);

        switch (key) {
        case SAO_UI_POPUP_KEY_UP: {
            top.selected_index = advance_selection(entries, top.selected_index, -1);
            break;
        }
        case SAO_UI_POPUP_KEY_DOWN: {
            top.selected_index = advance_selection(entries, top.selected_index, +1);
            break;
        }
        case SAO_UI_POPUP_KEY_LEFT: {
            if (handle->levels.size() > 1) {
                handle->levels.pop_back();
            }
            // At root, LEFT is a no-op (matches Windows menu behavior).
            break;
        }
        case SAO_UI_POPUP_KEY_RIGHT: {
            if (top.selected_index < 0 ||
                top.selected_index >= static_cast<int32_t>(entries.size())) {
                break;
            }
            const EntryNode& sel = entries[static_cast<size_t>(top.selected_index)];
            if (sel.enabled && !sel.is_separator && !sel.children.empty()) {
                OpenLevel next;
                next.parent_entry_index = top.selected_index;
                // Anchor at right edge of parent's selected row.
                const auto* consts = sao_ui_popup_layout_constants();
                int32_t sub_x = top.rect.x + top.rect.width + consts->gap_menu_child;
                int32_t sub_y = top.rect.row_top[static_cast<size_t>(top.selected_index)];
                int32_t sub_width = consts->child_width;
                compute_level_rect(sel.children, sub_x, sub_y, sub_width, &next.rect);
                next.selected_index = advance_selection(sel.children, -1, +1);
                handle->levels.push_back(std::move(next));
            }
            break;
        }
        case SAO_UI_POPUP_KEY_ENTER: {
            if (top.selected_index < 0 ||
                top.selected_index >= static_cast<int32_t>(entries.size())) {
                break;
            }
            const EntryNode& sel = entries[static_cast<size_t>(top.selected_index)];
            if (!sel.enabled || sel.is_separator) break;
            if (!sel.children.empty()) {
                // ENTER on a submenu row = expand (same as RIGHT).
                OpenLevel next;
                next.parent_entry_index = top.selected_index;
                const auto* consts = sao_ui_popup_layout_constants();
                int32_t sub_x = top.rect.x + top.rect.width + consts->gap_menu_child;
                int32_t sub_y = top.rect.row_top[static_cast<size_t>(top.selected_index)];
                compute_level_rect(sel.children, sub_x, sub_y, consts->child_width, &next.rect);
                next.selected_index = advance_selection(sel.children, -1, +1);
                handle->levels.push_back(std::move(next));
            } else {
                chosen_entry = sel.entry_id;
                choose_cb = handle->callback;
                choose_ud = handle->user_data;
                handle->visible = false;
                handle->callback = nullptr;
                handle->user_data = nullptr;
                handle->levels.clear();
            }
            break;
        }
        case SAO_UI_POPUP_KEY_ESC: {
            handle->visible = false;
            dismiss_cb = handle->callback;
            dismiss_ud = handle->user_data;
            handle->callback = nullptr;
            handle->user_data = nullptr;
            handle->levels.clear();
            break;
        }
        default:
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    }
    if (choose_cb != nullptr) {
        choose_cb(chosen_entry, /*dismissed=*/false,
                  /*screen_x=*/-1, /*screen_y=*/-1, choose_ud);
    }
    if (dismiss_cb != nullptr) {
        dismiss_cb(/*chosen=*/-1, /*dismissed=*/true, -1, -1, dismiss_ud);
    }
    return SAO_STATUS_OK;
}

// ── hit test (hierarchical) ───────────────────────────────────────
//
// Walk the level stack from deepest → shallowest.  A hit inside the
// deepest submenu wins; otherwise the parent's row we're hovering
// over.  entry_id == -1 when off any entry (e.g. gutter between
// menu column and submenu).
extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_hit_test(
    sao_ui_popup_handle_t handle,
    int32_t x, int32_t y,
    int32_t* out_entry_id, int32_t* out_submenu_depth) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (out_entry_id != nullptr) *out_entry_id = -1;
    if (out_submenu_depth != nullptr) *out_submenu_depth = 0;
    std::lock_guard<std::mutex> lk(handle->mu);
    if (!handle->visible || handle->levels.empty()) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    // Test deepest first (submenu wins over parent).
    for (int32_t depth = static_cast<int32_t>(handle->levels.size()) - 1;
         depth >= 0; --depth) {
        const OpenLevel& lvl = handle->levels[static_cast<size_t>(depth)];
        const LevelRect& r = lvl.rect;
        if (x < r.x || x >= r.x + r.width) continue;
        if (y < r.y || y >= r.y + r.height) continue;
        const std::vector<EntryNode>& entries = level_entries(handle, depth);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (y >= r.row_top[i] && y < r.row_bottom[i]) {
                const EntryNode& e = entries[i];
                if (e.is_separator) {
                    // Separator rows report no entry; still record depth.
                    if (out_submenu_depth != nullptr) *out_submenu_depth = depth;
                    return SAO_STATUS_OK;
                }
                if (out_entry_id != nullptr) *out_entry_id = e.entry_id;
                if (out_submenu_depth != nullptr) *out_submenu_depth = depth;
                return SAO_STATUS_OK;
            }
        }
        // Inside this level's rect but off all rows — treat as "on level, no entry".
        if (out_submenu_depth != nullptr) *out_submenu_depth = depth;
        return SAO_STATUS_OK;
    }
    // Outside all level rects.
    return SAO_STATUS_OK;
}
