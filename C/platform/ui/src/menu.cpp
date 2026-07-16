// SAO Auto — SAO main menu implementation (Wave 3, G3.2 first slice).
//
// This slice owns the *state machine* + *ring geometry* half of the menu.
// It does NOT touch the D2D compose path, the frosted-glass shader, or
// the child-menu row list — those land in later Wave 3 slices.  What
// this file gives us today:
//
//   * Header-declared entry points (create / destroy / show / hide /
//     get_phase / hit_test / set_hover / activate / set_items / …)
//     implemented for the three menu modes documented in menu.h.
//   * Ring geometry math for RING mode (button 0 at 12 o'clock, buttons
//     distributed evenly clockwise around outer_radius).
//   * Vertical geometry math for VERTICAL_STRIP mode (SE-anchor stack,
//     matches the Python SAOMenuBar default pack layout).
//   * Cascade geometry math for CASCADE mode (horizontal strip anchored
//     near screen top).
//   * A wave3-only helper family, exported for tests + future compose
//     path (compute_button_layout / tick / set_button_state /
//     dispatch_event).  Their prototypes live at the bottom of this
//     file — they are ABI-exported so the test binary can see them,
//     but they are not part of menu.h and therefore not part of the
//     public plugin ABI yet.
//
// State machine (mirrors SAOPopUpMenu.play_enter_animation / close):
//
//   CLOSED  --show-->  OPENING  --tick 450ms-->  OPEN
//   OPEN    --hide-->  CLOSING  --tick 300ms-->  CLOSED
//   OPEN    --child open  -->  CHILD_OPENING --200ms--> CHILD_OPEN
//   CHILD_OPEN --child close--> CHILD_CLOSING --200ms--> OPEN
//
// The top-level 450 ms open / 300 ms close durations come from the
// production popup fade authority. Child timings remain on the generic
// row-transition track until the child renderer consumes the asymmetric
// 160 ms fadeout / 220 ms fadein contract.
//
// UTF-8 no BOM.

#include "sao/ui/menu.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(sizeof(sao_ui_menu_handle_t) == sizeof(void*), "menu handle must be pointer-width");
static_assert(SAO_UI_MENU_MODE_VERTICAL_STRIP == 0, "menu mode enum drifted from header");
static_assert(SAO_UI_MENU_MODE_RING == 1, "menu mode enum drifted from header");
static_assert(SAO_UI_MENU_MODE_CASCADE == 2, "menu mode enum drifted from header");
static_assert(SAO_UI_MENU_PHASE_CLOSED == 0, "menu phase enum drifted from header");
static_assert(SAO_UI_MENU_PHASE_CLOSING == 6, "menu phase enum drifted from header");
static_assert(SAO_UI_MENU_BTN_IDLE == 0, "menu button state enum drifted from header");
static_assert(SAO_UI_MENU_BTN_DISABLED == 3, "menu button state enum drifted from header");

// ---------------------------------------------------------------------------
// Constants — animation timings + layout defaults.
// ---------------------------------------------------------------------------

namespace {

constexpr int32_t kMenuOpenMs = 450;
constexpr int32_t kMenuCloseMs = 300;
constexpr int32_t kChildOpenMs = 200;
constexpr int32_t kChildCloseMs = 200;

// Default metrics — from menu.h banner + theme.h metrics table:
//   button_size     = 54  (SAOCircleButton.SIZE)
//   button_max_size = 70  (fisheye peak)
//   slot_size       = 70  (SAOMenuBar._SLOT)
//   max_visible     = 9
constexpr int32_t kDefaultButtonSize = 54;
constexpr int32_t kDefaultButtonMaxSize = 70;
constexpr int32_t kDefaultSlotSize = 70;
constexpr int32_t kDefaultMaxVisible = 9;

// Ring defaults: outer_radius large enough that a 70-px button doesn't
// clip the NerveGear centre (which owns a 36-px inner disc).
constexpr int32_t kDefaultInnerRadius = 60;
constexpr int32_t kDefaultOuterRadius = 160;
constexpr int32_t kDefaultChildRadius = 240;

// π convenience.  We use double for the math to keep radial hits
// exact enough that the 12-o'clock case doesn't drift due to float
// rounding when angle_step is small.
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

} // namespace

// ---------------------------------------------------------------------------
// Internal types.
// ---------------------------------------------------------------------------

struct SaoUiMenuButtonRect {
    int32_t x; // top-left of button bounding box
    int32_t y;
    int32_t w;
    int32_t h;
};

namespace {

// Local mirror of SaoUiMenuItem, so we own the strings — the header
// POD holds only borrowed pointers.
struct MenuItem {
    std::string name;
    std::string icon;
    int32_t action_id{-1};
    bool can_activate{false};
    SaoUiMenuBtnState state{SAO_UI_MENU_BTN_IDLE};
};

struct ChildMenu {
    std::string parent_name;
    std::vector<MenuItem> items;
};

} // namespace

struct sao_ui_menu_s {
    sao_ui_compositor_handle_t compositor{nullptr};
    sao_ui_theme_handle_t theme{nullptr};
    SaoUiMenuMode mode{SAO_UI_MENU_MODE_RING};
    SaoUiMenuLayout layout{};
    std::vector<MenuItem> items;
    std::vector<ChildMenu> children;

    // Anchor + phase.
    int32_t anchor_x{0};
    int32_t anchor_y{0};
    SaoUiMenuPhase phase{SAO_UI_MENU_PHASE_CLOSED};

    // ms since phase entered.  Used by tick() to progress OPENING→OPEN etc.
    int32_t phase_elapsed_ms{0};

    // Hover / active idx.
    int32_t hover_idx{-1};
    int32_t active_idx{-1};

    // Callback.
    sao_ui_menu_event_callback_t callback{nullptr};
    void* callback_user_data{nullptr};

    // HUD bounds cache — populated on show; not the whole compose path.
    SaoUiMenuHudBounds hud_bounds{};

    mutable std::mutex mtx;
};

namespace {

// ---------------------------------------------------------------------------
// Default-layout seeding.
// ---------------------------------------------------------------------------

SaoUiMenuLayout make_default_layout(int32_t anchor_x, int32_t anchor_y) {
    SaoUiMenuLayout l{};
    l.center_x = anchor_x;
    l.center_y = anchor_y;
    l.inner_radius = kDefaultInnerRadius;
    l.outer_radius = kDefaultOuterRadius;
    l.child_ring_radius = kDefaultChildRadius;
    l.button_size = kDefaultButtonSize;
    l.button_max_size = kDefaultButtonMaxSize;
    l.slot_size = kDefaultSlotSize;
    l.max_visible = kDefaultMaxVisible;
    return l;
}

// Merge a caller-supplied layout with the default: any zero field is
// filled in from the default.  (This mirrors the header contract:
// "Passing zeros for any field keeps the metrics-table default from
// theme.h".)
void merge_layout_overrides(SaoUiMenuLayout* dst, const SaoUiMenuLayout* src) {
    if (dst == nullptr || src == nullptr)
        return;
    if (src->center_x != 0)
        dst->center_x = src->center_x;
    if (src->center_y != 0)
        dst->center_y = src->center_y;
    if (src->inner_radius != 0)
        dst->inner_radius = src->inner_radius;
    if (src->outer_radius != 0)
        dst->outer_radius = src->outer_radius;
    if (src->child_ring_radius != 0)
        dst->child_ring_radius = src->child_ring_radius;
    if (src->button_size != 0)
        dst->button_size = src->button_size;
    if (src->button_max_size != 0)
        dst->button_max_size = src->button_max_size;
    if (src->slot_size != 0)
        dst->slot_size = src->slot_size;
    if (src->max_visible != 0)
        dst->max_visible = src->max_visible;
}

// ---------------------------------------------------------------------------
// Ring geometry — button positions on the outer_radius circle.
// ---------------------------------------------------------------------------
//
// Convention:
//   * Button 0 sits at 12 o'clock (angle = -π/2 in screen-Y-down space).
//   * Subsequent buttons distributed evenly clockwise (positive angle
//     step of 2π / N, still in screen-Y-down space).
//   * Result rect is the button bounding box (top-left corner + w/h).
//
// The button centre lands at (center_x + r*cos(θ), center_y + r*sin(θ));
// the returned bounding box wraps it symmetrically at button_size.

SaoUiMenuButtonRect compute_ring_rect(const SaoUiMenuLayout& layout, int32_t button_index,
                                      int32_t button_count) {
    const int32_t size = layout.button_size > 0 ? layout.button_size : kDefaultButtonSize;
    if (button_count <= 0) {
        return SaoUiMenuButtonRect{layout.center_x - size / 2, layout.center_y - size / 2, size,
                                   size};
    }
    const double step_rad = kTwoPi / static_cast<double>(button_count);
    // 12 o'clock is -π/2 in screen coordinates (Y grows downward).
    const double theta = -kPi * 0.5 + step_rad * static_cast<double>(button_index);
    const double cx = static_cast<double>(layout.center_x) +
                      static_cast<double>(layout.outer_radius) * std::cos(theta);
    const double cy = static_cast<double>(layout.center_y) +
                      static_cast<double>(layout.outer_radius) * std::sin(theta);
    const int32_t half = size / 2;
    const int32_t x = static_cast<int32_t>(std::lround(cx)) - half;
    const int32_t y = static_cast<int32_t>(std::lround(cy)) - half;
    return SaoUiMenuButtonRect{x, y, size, size};
}

// ---------------------------------------------------------------------------
// Vertical-strip geometry — SE-anchor stack (matches SAOMenuBar pack).
// ---------------------------------------------------------------------------

SaoUiMenuButtonRect compute_vertical_rect(const SaoUiMenuLayout& layout, int32_t button_index) {
    const int32_t slot = layout.slot_size > 0 ? layout.slot_size : kDefaultSlotSize;
    // Vertical-strip interaction follows the full fisheye slot rather than
    // the settled 54px circle. The 70px authority slot keeps the expanded
    // hover ring interactive at every edge.
    const int32_t x = layout.center_x - slot / 2;
    const int32_t y = layout.center_y + button_index * slot;
    return SaoUiMenuButtonRect{x, y, slot, slot};
}

// ---------------------------------------------------------------------------
// Cascade geometry — horizontal strip at screen-top-ish anchor.
// ---------------------------------------------------------------------------
//
// SAOPopUpMenu.cascade_mode drops from the top; buttons are laid out
// horizontally, centred around center_x.

SaoUiMenuButtonRect compute_cascade_rect(const SaoUiMenuLayout& layout, int32_t button_index,
                                         int32_t button_count) {
    const int32_t size = layout.button_size > 0 ? layout.button_size : kDefaultButtonSize;
    const int32_t slot = layout.slot_size > 0 ? layout.slot_size : kDefaultSlotSize;
    if (button_count <= 0) {
        return SaoUiMenuButtonRect{layout.center_x - size / 2, layout.center_y, size, size};
    }
    // Total strip width = (N-1) * slot + button_size.  Left edge
    // computed to centre the strip on layout.center_x.
    const int32_t strip_w = (button_count - 1) * slot + size;
    const int32_t left = layout.center_x - strip_w / 2;
    const int32_t x = left + button_index * slot;
    const int32_t y = layout.center_y;
    return SaoUiMenuButtonRect{x, y, size, size};
}

// ---------------------------------------------------------------------------
// Callback dispatch is captured while locked and invoked after unlocking.
// ---------------------------------------------------------------------------

struct PendingMenuEvent {
    sao_ui_menu_event_callback_t callback{};
    void* user_data{};
    SaoUiMenuEvent event{};
    int32_t primary{-1};
    int32_t secondary{-1};
    int32_t action_id{};
};

PendingMenuEvent capture_event_locked(sao_ui_menu_s* menu, SaoUiMenuEvent event, int32_t primary,
                                      int32_t secondary, int32_t action_id) {
    return {menu->callback, menu->callback_user_data, event, primary, secondary, action_id};
}

void dispatch_event_noexcept(const PendingMenuEvent& pending) noexcept {
    if (pending.callback == nullptr)
        return;
    try {
        pending.callback(pending.event, pending.primary, pending.secondary, pending.action_id,
                         pending.user_data);
    } catch (...) {
    }
}

// Ring hit-test using the "radial band + angular sweep" fast path
// described in the header banner.  Returns [0, N) or -1.
int32_t ring_hit_test(const SaoUiMenuLayout& layout, int32_t button_count, int32_t px, int32_t py) {
    if (button_count <= 0)
        return -1;
    const double dx = static_cast<double>(px - layout.center_x);
    const double dy = static_cast<double>(py - layout.center_y);
    const double r = std::sqrt(dx * dx + dy * dy);
    const double size = static_cast<double>(layout.button_size);
    const double outer = static_cast<double>(layout.outer_radius);
    // Radial band: allow ±button_size/2 slop around the outer ring.
    const double lo = outer - size * 0.5;
    const double hi = outer + size * 0.5;
    if (r < lo || r > hi)
        return -1;
    // Angular sweep — recover θ in (-π, π] via atan2(dy, dx), then
    // rotate so 12 o'clock (button 0) sits at 0, and normalise to [0, 2π).
    double theta = std::atan2(dy, dx);
    // Rotate: shift by +π/2 so -π/2 becomes 0.
    theta += kPi * 0.5;
    // Normalise to [0, 2π).
    while (theta < 0.0)
        theta += kTwoPi;
    while (theta >= kTwoPi)
        theta -= kTwoPi;
    const double step = kTwoPi / static_cast<double>(button_count);
    // Snap to nearest button index; clamp because rounding at the seam
    // can push us to N.
    int32_t idx = static_cast<int32_t>(std::floor((theta + step * 0.5) / step));
    if (idx < 0)
        idx = 0;
    if (idx >= button_count)
        idx = 0; // wrap seam: N maps back to 0
    return idx;
}

int32_t vertical_hit_test(const SaoUiMenuLayout& layout, int32_t button_count, int32_t px,
                          int32_t py) {
    for (int32_t i = 0; i < button_count; ++i) {
        auto rect = compute_vertical_rect(layout, i);
        if (px >= rect.x && px < rect.x + rect.w && py >= rect.y && py < rect.y + rect.h) {
            return i;
        }
    }
    return -1;
}

int32_t cascade_hit_test(const SaoUiMenuLayout& layout, int32_t button_count, int32_t px,
                         int32_t py) {
    for (int32_t i = 0; i < button_count; ++i) {
        auto rect = compute_cascade_rect(layout, i, button_count);
        if (px >= rect.x && px < rect.x + rect.w && py >= rect.y && py < rect.y + rect.h) {
            return i;
        }
    }
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// Header-declared entry points.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_create(sao_ui_compositor_handle_t compositor,
                                                       sao_ui_theme_handle_t theme,
                                                       SaoUiMenuMode mode,
                                                       sao_ui_menu_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (mode != SAO_UI_MENU_MODE_VERTICAL_STRIP && mode != SAO_UI_MENU_MODE_RING &&
        mode != SAO_UI_MENU_MODE_CASCADE) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto menu = std::make_unique<sao_ui_menu_s>();
    menu->compositor = compositor;
    menu->theme = theme;
    menu->mode = mode;
    menu->layout = make_default_layout(0, 0);
    menu->phase = SAO_UI_MENU_PHASE_CLOSED;
    *out_handle = menu.release();
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_menu_destroy(sao_ui_menu_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_set_items(sao_ui_menu_handle_t handle,
                                                          const SaoUiMenuItem* items,
                                                          size_t item_count) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (items == nullptr && item_count > 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    handle->items.clear();
    handle->children.clear();
    handle->hover_idx = -1;
    handle->active_idx = -1;
    handle->items.reserve(item_count);
    for (size_t i = 0; i < item_count; ++i) {
        MenuItem it{};
        it.name = items[i].name_utf8 ? items[i].name_utf8 : "";
        it.icon = items[i].icon_utf8 ? items[i].icon_utf8 : "";
        it.action_id = items[i].action_id;
        it.can_activate = items[i].can_activate;
        it.state = SAO_UI_MENU_BTN_IDLE;
        handle->items.push_back(std::move(it));
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_set_children(sao_ui_menu_handle_t handle,
                                                             const char* parent_name_utf8,
                                                             const SaoUiMenuItem* items,
                                                             size_t item_count) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (parent_name_utf8 == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (items == nullptr && item_count > 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    // Reject if parent not registered.
    bool found_parent = false;
    for (const auto& it : handle->items) {
        if (it.name == parent_name_utf8) {
            found_parent = true;
            break;
        }
    }
    if (!found_parent)
        return SAO_STATUS_ERR_NOT_FOUND;
    // Replace-or-insert.
    ChildMenu* target = nullptr;
    for (auto& cm : handle->children) {
        if (cm.parent_name == parent_name_utf8) {
            target = &cm;
            break;
        }
    }
    if (target == nullptr) {
        handle->children.push_back(ChildMenu{});
        target = &handle->children.back();
        target->parent_name = parent_name_utf8;
    }
    target->items.clear();
    target->items.reserve(item_count);
    for (size_t i = 0; i < item_count; ++i) {
        MenuItem it{};
        it.name = items[i].name_utf8 ? items[i].name_utf8 : "";
        it.icon = items[i].icon_utf8 ? items[i].icon_utf8 : "";
        it.action_id = items[i].action_id;
        it.can_activate = items[i].can_activate;
        target->items.push_back(std::move(it));
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_set_layout(sao_ui_menu_handle_t handle,
                                                           const SaoUiMenuLayout* layout) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (layout == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    // The header contract: zero fields keep the default.  Start from
    // whatever the current layout is (already seeded with defaults on
    // create) so we get per-field overrides.
    merge_layout_overrides(&handle->layout, layout);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_show(sao_ui_menu_handle_t handle, int32_t anchor_x,
                                                     int32_t anchor_y) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    handle->anchor_x = anchor_x;
    handle->anchor_y = anchor_y;
    // For RING and VERTICAL_STRIP, the anchor is also the layout centre
    // (RING: ring centre; VERTICAL_STRIP: SE anchor top).  For CASCADE
    // the header says the anchor is ignored — we still store it in
    // anchor_x/y for the compose path but layout.center stays wherever
    // set_layout put it.  This first slice: mirror anchor into layout
    // centre for RING/VERTICAL_STRIP, leave CASCADE alone.
    if (handle->mode != SAO_UI_MENU_MODE_CASCADE) {
        handle->layout.center_x = anchor_x;
        handle->layout.center_y = anchor_y;
    }
    // Transition from any closed-ish phase into OPENING; if we are
    // already OPEN just refresh the anchor without restarting animation.
    if (handle->phase == SAO_UI_MENU_PHASE_CLOSED || handle->phase == SAO_UI_MENU_PHASE_CLOSING) {
        handle->phase = SAO_UI_MENU_PHASE_OPENING;
        handle->phase_elapsed_ms = 0;
    }
    // Seed a minimal hud_bounds struct (the compose slice fills the rest).
    handle->hud_bounds = SaoUiMenuHudBounds{};
    handle->hud_bounds.anchor_x = anchor_x;
    handle->hud_bounds.anchor_y = anchor_y;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_hide(sao_ui_menu_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    if (handle->phase == SAO_UI_MENU_PHASE_CLOSED) {
        return SAO_STATUS_OK;
    }
    // Any not-yet-closed phase collapses to CLOSING; if we were mid-
    // OPENING we don't need to cascade through OPEN first.
    handle->phase = SAO_UI_MENU_PHASE_CLOSING;
    handle->phase_elapsed_ms = 0;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_is_visible(sao_ui_menu_handle_t handle,
                                                           bool* out_visible) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_visible == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    *out_visible = (handle->phase != SAO_UI_MENU_PHASE_CLOSED);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_get_phase(sao_ui_menu_handle_t handle,
                                                          SaoUiMenuPhase* out_phase) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_phase == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    *out_phase = handle->phase;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_hit_test(sao_ui_menu_handle_t handle, int32_t x,
                                                         int32_t y, int32_t* out_menu_idx,
                                                         int32_t* out_child_idx) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_menu_idx == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    *out_menu_idx = -1;
    if (out_child_idx)
        *out_child_idx = -1;
    const int32_t n = static_cast<int32_t>(handle->items.size());
    if (n == 0)
        return SAO_STATUS_OK;
    int32_t idx = -1;
    switch (handle->mode) {
    case SAO_UI_MENU_MODE_RING:
        idx = ring_hit_test(handle->layout, n, x, y);
        break;
    case SAO_UI_MENU_MODE_VERTICAL_STRIP:
        idx = vertical_hit_test(handle->layout, n, x, y);
        break;
    case SAO_UI_MENU_MODE_CASCADE:
        idx = cascade_hit_test(handle->layout, n, x, y);
        break;
    }
    *out_menu_idx = idx;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_set_hover(sao_ui_menu_handle_t handle,
                                                          int32_t menu_idx) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    PendingMenuEvent pending{};
    {
        std::lock_guard<std::mutex> lock(handle->mtx);
        const int32_t n = static_cast<int32_t>(handle->items.size());
        if (menu_idx < -1 || menu_idx >= n) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (handle->hover_idx == menu_idx)
            return SAO_STATUS_OK;
        if (handle->hover_idx >= 0 && handle->hover_idx < n) {
            auto& it = handle->items[handle->hover_idx];
            if (it.state == SAO_UI_MENU_BTN_HOVER) {
                it.state = SAO_UI_MENU_BTN_IDLE;
            }
        }
        handle->hover_idx = menu_idx;
        if (menu_idx >= 0) {
            auto& it = handle->items[menu_idx];
            if (it.state == SAO_UI_MENU_BTN_IDLE) {
                it.state = SAO_UI_MENU_BTN_HOVER;
            }
        }
        pending = capture_event_locked(handle, SAO_UI_MENU_EV_HOVER_CHANGED, menu_idx, -1, 0);
    }
    dispatch_event_noexcept(pending);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_activate(sao_ui_menu_handle_t handle,
                                                         int32_t menu_idx) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    PendingMenuEvent pending{};
    {
        std::lock_guard<std::mutex> lock(handle->mtx);
        const int32_t n = static_cast<int32_t>(handle->items.size());
        if (menu_idx < 0 || menu_idx >= n) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        auto& it = handle->items[menu_idx];
        if (!it.can_activate)
            return SAO_STATUS_OK;
        handle->active_idx = menu_idx;
        it.state = SAO_UI_MENU_BTN_ACTIVE;
        pending =
            capture_event_locked(handle, SAO_UI_MENU_EV_ITEM_ACTIVATED, menu_idx, -1, it.action_id);
    }
    dispatch_event_noexcept(pending);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_query_hud_bounds(sao_ui_menu_handle_t handle,
                                                                 SaoUiMenuHudBounds* out_bounds) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_bounds == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    *out_bounds = handle->hud_bounds;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_set_event_callback(
    sao_ui_menu_handle_t handle, sao_ui_menu_event_callback_t callback, void* user_data) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    handle->callback = callback;
    handle->callback_user_data = user_data;
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Wave3 helper API (exported for tests + future compose path).
//
// These are NOT in menu.h — they are internal helpers that the state-
// machine test suite needs to hit.  Kept extern "C" + SAO_UI_API so
// the test binary can link them across the shared-library boundary.
// When the compose path lands the surface will be reviewed for
// promotion into menu.h.
// ---------------------------------------------------------------------------

// Compute the bounding box of a given button.  Returns error if the
// index is out of range for the current item list.  This is what a
// compose path or hit-test debugger uses to draw a highlight rect.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_menu_compute_button_layout(sao_ui_menu_handle_t handle, int32_t button_index, int32_t* out_x,
                                  int32_t* out_y, int32_t* out_w, int32_t* out_h) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_x == nullptr || out_y == nullptr || out_w == nullptr || out_h == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mtx);
    const int32_t n = static_cast<int32_t>(handle->items.size());
    if (button_index < 0 || button_index >= n) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    SaoUiMenuButtonRect r{};
    switch (handle->mode) {
    case SAO_UI_MENU_MODE_RING:
        r = compute_ring_rect(handle->layout, button_index, n);
        break;
    case SAO_UI_MENU_MODE_VERTICAL_STRIP:
        r = compute_vertical_rect(handle->layout, button_index);
        break;
    case SAO_UI_MENU_MODE_CASCADE:
        r = compute_cascade_rect(handle->layout, button_index, n);
        break;
    }
    *out_x = r.x;
    *out_y = r.y;
    *out_w = r.w;
    *out_h = r.h;
    return SAO_STATUS_OK;
}

// Advance the state machine by dt_ms.  Handles the four transitions
// documented at the top of this file: OPENING→OPEN, CLOSING→CLOSED,
// CHILD_OPENING→CHILD_OPEN, CHILD_CLOSING→OPEN.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_tick(sao_ui_menu_handle_t handle,
                                                                int32_t dt_ms) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (dt_ms < 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PendingMenuEvent pending{};
    {
        std::lock_guard<std::mutex> lock(handle->mtx);
        handle->phase_elapsed_ms += dt_ms;
        switch (handle->phase) {
        case SAO_UI_MENU_PHASE_OPENING:
            if (handle->phase_elapsed_ms >= kMenuOpenMs) {
                handle->phase = SAO_UI_MENU_PHASE_OPEN;
                handle->phase_elapsed_ms = 0;
                pending = capture_event_locked(handle, SAO_UI_MENU_EV_OPENED, -1, -1, 0);
            }
            break;
        case SAO_UI_MENU_PHASE_CLOSING:
            if (handle->phase_elapsed_ms >= kMenuCloseMs) {
                handle->phase = SAO_UI_MENU_PHASE_CLOSED;
                handle->phase_elapsed_ms = 0;
                pending = capture_event_locked(handle, SAO_UI_MENU_EV_CLOSED, -1, -1, 0);
            }
            break;
        case SAO_UI_MENU_PHASE_CHILD_OPENING:
            if (handle->phase_elapsed_ms >= kChildOpenMs) {
                handle->phase = SAO_UI_MENU_PHASE_CHILD_OPEN;
                handle->phase_elapsed_ms = 0;
            }
            break;
        case SAO_UI_MENU_PHASE_CHILD_CLOSING:
            if (handle->phase_elapsed_ms >= kChildCloseMs) {
                handle->phase = SAO_UI_MENU_PHASE_OPEN;
                handle->phase_elapsed_ms = 0;
            }
            break;
        default:
            break;
        }
    }
    dispatch_event_noexcept(pending);
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_menu_get_transition_progress(sao_ui_menu_handle_t handle, float* out_progress) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_progress == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    switch (handle->phase) {
    case SAO_UI_MENU_PHASE_CLOSED:
        *out_progress = 0.0F;
        break;
    case SAO_UI_MENU_PHASE_OPENING:
        *out_progress = std::clamp(static_cast<float>(handle->phase_elapsed_ms) /
                                       static_cast<float>(kMenuOpenMs),
                                   0.0F, 1.0F);
        break;
    case SAO_UI_MENU_PHASE_CLOSING:
        *out_progress = 1.0F - std::clamp(static_cast<float>(handle->phase_elapsed_ms) /
                                              static_cast<float>(kMenuCloseMs),
                                          0.0F, 1.0F);
        break;
    case SAO_UI_MENU_PHASE_CHILD_OPENING:
        *out_progress = std::clamp(static_cast<float>(handle->phase_elapsed_ms) /
                                       static_cast<float>(kChildOpenMs),
                                   0.0F, 1.0F);
        break;
    case SAO_UI_MENU_PHASE_CHILD_CLOSING:
        *out_progress = 1.0F - std::clamp(static_cast<float>(handle->phase_elapsed_ms) /
                                              static_cast<float>(kChildCloseMs),
                                          0.0F, 1.0F);
        break;
    case SAO_UI_MENU_PHASE_OPEN:
    case SAO_UI_MENU_PHASE_CHILD_OPEN:
        *out_progress = 1.0F;
        break;
    }
    return SAO_STATUS_OK;
}

// Direct-write path for a button's visual state (used by keyboard nav
// and by the compose path highlighting the sticky-selected button).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_set_button_state(
    sao_ui_menu_handle_t handle, int32_t button_index, SaoUiMenuBtnState state) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (state < SAO_UI_MENU_BTN_IDLE || state > SAO_UI_MENU_BTN_DISABLED) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mtx);
    const int32_t n = static_cast<int32_t>(handle->items.size());
    if (button_index < 0 || button_index >= n) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    handle->items[button_index].state = state;
    if (state == SAO_UI_MENU_BTN_HOVER)
        handle->hover_idx = button_index;
    if (state == SAO_UI_MENU_BTN_ACTIVE)
        handle->active_idx = button_index;
    return SAO_STATUS_OK;
}

// Query a button's current visual state.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_get_button_state(
    sao_ui_menu_handle_t handle, int32_t button_index, SaoUiMenuBtnState* out_state) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_state == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    const int32_t n = static_cast<int32_t>(handle->items.size());
    if (button_index < 0 || button_index >= n) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_state = handle->items[button_index].state;
    return SAO_STATUS_OK;
}

// External dispatcher — event_type is one of SaoUiMenuEvent.  The
// data_ptr payload interpretation depends on event_type; today we
// only honour a small subset for the state machine bring-up:
//
//   EV_ITEM_ACTIVATED  : data_ptr = const int32_t* button_index
//   EV_HOVER_CHANGED   : data_ptr = const int32_t* button_index (-1 clears)
//   EV_BACKGROUND_CLICK: data_ptr = ignored, hides the menu
//
// Other events pass straight through to the registered callback.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_dispatch_event(
    sao_ui_menu_handle_t handle, SaoUiMenuEvent event_type, const void* data_ptr) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    switch (event_type) {
    case SAO_UI_MENU_EV_ITEM_ACTIVATED: {
        if (data_ptr == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const int32_t idx = *static_cast<const int32_t*>(data_ptr);
        return sao_ui_menu_activate(handle, idx);
    }
    case SAO_UI_MENU_EV_HOVER_CHANGED: {
        if (data_ptr == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const int32_t idx = *static_cast<const int32_t*>(data_ptr);
        return sao_ui_menu_set_hover(handle, idx);
    }
    case SAO_UI_MENU_EV_BACKGROUND_CLICK: {
        PendingMenuEvent pending{};
        {
            std::lock_guard<std::mutex> lock(handle->mtx);
            pending = capture_event_locked(handle, SAO_UI_MENU_EV_BACKGROUND_CLICK, -1, -1, 0);
        }
        dispatch_event_noexcept(pending);
        return sao_ui_menu_hide(handle);
    }
    default: {
        PendingMenuEvent pending{};
        {
            std::lock_guard<std::mutex> lock(handle->mtx);
            pending = capture_event_locked(handle, event_type, -1, -1, 0);
        }
        dispatch_event_noexcept(pending);
        return SAO_STATUS_OK;
    }
    }
}
