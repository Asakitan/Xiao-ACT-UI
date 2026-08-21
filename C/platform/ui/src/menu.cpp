// SAO Auto — SAO main menu implementation.
//
// Owns root/child state machines, geometry, hit testing, interaction,
// and renderer-facing visual snapshots. The D2D compose path and
// frosted-glass shader remain separate concerns. This file provides:
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
//   * An internal helper family, exported for tests + compose
//     path (compute_button_layout / tick / set_button_state /
//     dispatch_event).  Their prototypes live at the bottom of this
//     file — they are ABI-exported so the test binary can see them,
//     but they are not part of menu.h and therefore not part of the
//     public plugin ABI yet.
//   * Dynamic root items keyed by stable name plus retained child-menu
//     registries, child-row geometry, hit testing, and activation.
//
// State machine (mirrors SAOPopUpMenu.play_enter_animation / close):
//
//   CLOSED  --show-->  OPENING  --tick 450ms-->  OPEN
//   OPEN    --hide-->  CLOSING  --tick 300ms-->  CLOSED
//   OPEN    --child open  -->  CHILD_OPENING --220ms--> CHILD_OPEN
//   CHILD_OPEN --child close--> CHILD_CLOSING --160ms--> OPEN
//
// The top-level 450 ms open / 300 ms close durations come from the
// production popup fade authority. Child fade, staggered slide, and
// hover timings mirror the active Python GPU popup authority.
//
// UTF-8 no BOM.

#include "sao/ui/menu.h"
#include "sao/ui/animator.h"

#include "sao/ui/subpixel.h"

#include "menu_visual_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
constexpr int32_t kChildFadeInMs = 220;
constexpr int32_t kChildFadeOutMs = 160;
constexpr int32_t kChildSlideMs = 320;
constexpr int32_t kChildSlideStaggerMs = 50;
constexpr int32_t kRootHoverInMs = 140;
constexpr int32_t kRootHoverOutMs = 110;
constexpr int32_t kChildHoverInMs = 140;
constexpr int32_t kChildHoverOutMs = 110;
constexpr float kGeometrySnapEpsilon = 1.0F / 512.0F;

// Default metrics — from menu.h banner + theme.h metrics table:
//   button_size     = 54  (SAOCircleButton.SIZE)
//   button_max_size = 70  (fisheye peak)
//   slot_size       = 70  (SAOMenuBar._SLOT)
//   max_visible     = 9
constexpr int32_t kDefaultButtonSize = 54;
constexpr int32_t kDefaultButtonMaxSize = 70;
constexpr int32_t kDefaultSlotSize = 70;
constexpr int32_t kDefaultMaxVisible = 9;

// Active Python GPU child-bar authority.
constexpr int32_t kChildColumnGap = 25;
constexpr int32_t kChildListX = 27;
constexpr int32_t kChildRowHeight = 44;
constexpr int32_t kChildRowStride = 47;
constexpr int32_t kChildTargetRowWidth = 240;

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
    std::vector<int32_t> visible_widths;
    std::vector<float> hover_values;
};

} // namespace

struct sao_ui_menu_s {
    sao_ui_compositor_handle_t compositor{nullptr};
    sao_ui_theme_handle_t theme{nullptr};
    SaoUiMenuMode mode{SAO_UI_MENU_MODE_RING};
    SaoUiMenuLayout layout{};
    std::vector<MenuItem> items;
    std::vector<float> root_hover_values;
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

    // Child visual state is keyed by stable parent name so root reorder
    // never leaves renderer-facing rows attached to a stale index.
    std::string displayed_parent_name;
    std::string pending_parent_name;
    int32_t child_hover_idx{-1};
    int32_t child_slide_elapsed_ms{0};
    float child_fade_t{1.0F};
    uint64_t visual_revision{1};
    float transition_eased_t{};
    float close_start_t{};
    std::vector<float> close_start_rows;
    float center_diffusion_t{};
    float close_suction_t{};
    int32_t selection_trail_idx{-1};
    float selection_trail_t{};
    int32_t pressed_pulse_idx{-1};
    float pressed_pulse_t{};
    float child_rail_glow_t{};
    float backdrop_lens_t{};
    float open_spark_t{};
    float selection_spark_t{};
    uint32_t open_spark_count{};
    uint32_t selection_spark_count{};
    bool reduced_motion{};
    bool fps_pressure{};
    int32_t menu_open_sound_debounce_ms{};
    int32_t root_select_sound_debounce_ms{};
    int32_t child_activate_sound_debounce_ms{};

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

float spring_progress(float raw) {
    return sao_ui_curve_evaluate_spring_continuous(raw);
}

int32_t visible_item_count_locked(const sao_ui_menu_s* menu) {
    const int32_t limit = menu->layout.max_visible > 0
                              ? menu->layout.max_visible
                              : sao::ui::menu_visual::kVisualMaxVisible;
    return std::min(static_cast<int32_t>(menu->items.size()), limit);
}

float opening_root_stagger_locked(const sao_ui_menu_s* menu, int32_t index) {
    const int32_t stagger_delay = index * 18;
    const int32_t duration = menu->reduced_motion ? 1 : kMenuOpenMs;
    const float raw = std::clamp(
        static_cast<float>(menu->phase_elapsed_ms - stagger_delay) /
            static_cast<float>(duration),
        0.0F, 1.0F);
    return std::clamp(spring_progress(raw), 0.0F, 1.0F);
}

float closing_root_stagger_locked(const sao_ui_menu_s* menu, int32_t index) {
    const float start = index >= 0 && index < static_cast<int32_t>(menu->close_start_rows.size())
                            ? menu->close_start_rows[static_cast<size_t>(index)]
                            : menu->close_start_t;
    const int32_t duration = menu->reduced_motion ? 1 : kMenuCloseMs;
    const float raw = std::clamp(static_cast<float>(menu->phase_elapsed_ms) /
                                     static_cast<float>(duration),
                                 0.0F, 1.0F);
    const float close_t = std::clamp(spring_progress(raw), 0.0F, 1.0F);
    return std::clamp(start * (1.0F - close_t), 0.0F, 1.0F);
}

float root_stagger_locked(const sao_ui_menu_s* menu, int32_t index) {
    if (menu->phase == SAO_UI_MENU_PHASE_OPENING)
        return opening_root_stagger_locked(menu, index);
    if (menu->phase == SAO_UI_MENU_PHASE_CLOSING)
        return closing_root_stagger_locked(menu, index);
    return 1.0F;
}

void capture_close_start_rows_locked(sao_ui_menu_s* menu) {
    menu->close_start_rows.resize(menu->items.size());
    for (size_t index = 0; index < menu->items.size(); ++index) {
        menu->close_start_rows[index] = root_stagger_locked(menu, static_cast<int32_t>(index));
    }
}

int32_t aligned_boundary(float value) {
    return sao_ui_subpixel_snap_or_floor(value, kGeometrySnapEpsilon);
}

SaoUiMenuButtonRect aligned_rect(float left, float top, float right, float bottom) {
    const int32_t x0 = aligned_boundary(left);
    const int32_t y0 = aligned_boundary(top);
    const int32_t x1 = aligned_boundary(right);
    const int32_t y1 = aligned_boundary(bottom);
    return {x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
}

SaoUiMenuButtonRect compute_ring_rect(const SaoUiMenuLayout& layout, int32_t button_index,
                                      int32_t button_count, float focus_t = 0.0F) {
    const int32_t size = sao::ui::menu_visual::visual_button_diameter(layout, focus_t);
    if (button_count <= 0) {
        const float half = static_cast<float>(size) * 0.5F;
        return aligned_rect(static_cast<float>(layout.center_x) - half,
                            static_cast<float>(layout.center_y) - half,
                            static_cast<float>(layout.center_x) + half,
                            static_cast<float>(layout.center_y) + half);
    }
    const double step_rad = kTwoPi / static_cast<double>(button_count);
    // 12 o'clock is -π/2 in screen coordinates (Y grows downward).
    const double theta = -kPi * 0.5 + step_rad * static_cast<double>(button_index);
    const double cx = static_cast<double>(layout.center_x) +
                      static_cast<double>(layout.outer_radius) * std::cos(theta);
    const double cy = static_cast<double>(layout.center_y) +
                      static_cast<double>(layout.outer_radius) * std::sin(theta);
    const float half = static_cast<float>(size) * 0.5F;
    return aligned_rect(static_cast<float>(cx) - half, static_cast<float>(cy) - half,
                        static_cast<float>(cx) + half, static_cast<float>(cy) + half);
}

// ---------------------------------------------------------------------------
// Vertical-strip geometry — SE-anchor stack (matches SAOMenuBar pack).
// ---------------------------------------------------------------------------

SaoUiMenuButtonRect compute_vertical_rect(const SaoUiMenuLayout& layout, int32_t button_index) {
    const int32_t slot = std::max(layout.slot_size > 0 ? layout.slot_size : kDefaultSlotSize,
                                  layout.button_max_size > 0 ? layout.button_max_size : kDefaultButtonMaxSize);
    // Vertical-strip interaction follows the full fisheye slot rather than
    // the settled 54px circle. The 70px authority slot keeps the expanded
    // hover ring interactive at every edge.
    const float half = static_cast<float>(slot) * 0.5F;
    const float top = static_cast<float>(layout.center_y) +
                      static_cast<float>(button_index * slot);
    return aligned_rect(static_cast<float>(layout.center_x) - half, top,
                        static_cast<float>(layout.center_x) + half,
                        top + static_cast<float>(slot));
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
        const float half = static_cast<float>(size) * 0.5F;
        return aligned_rect(static_cast<float>(layout.center_x) - half,
                            static_cast<float>(layout.center_y),
                            static_cast<float>(layout.center_x) + half,
                            static_cast<float>(layout.center_y + size));
    }
    // Total strip width = (N-1) * slot + button_size.  Left edge
    // computed to centre the strip on layout.center_x.
    const float strip_width = static_cast<float>((button_count - 1) * slot + size);
    const float left = static_cast<float>(layout.center_x) - strip_width * 0.5F;
    const float item_left = left + static_cast<float>(button_index * slot);
    const float top = static_cast<float>(layout.center_y);
    return aligned_rect(item_left, top, item_left + static_cast<float>(size),
                        top + static_cast<float>(size));
}

// ---------------------------------------------------------------------------
// Callback dispatch is captured while locked and invoked after unlocking.
// ---------------------------------------------------------------------------

enum class MenuSoundCue : uint8_t { None, MenuOpen, RootSelect, ChildActivate };

struct PendingMenuEvent {
    sao_ui_menu_event_callback_t callback{};
    void* user_data{};
    SaoUiMenuEvent event{};
    int32_t primary{-1};
    int32_t secondary{-1};
    int32_t action_id{};
    MenuSoundCue sound{MenuSoundCue::None};
    bool play_sound{};
};

PendingMenuEvent capture_event_locked(sao_ui_menu_s* menu, SaoUiMenuEvent event, int32_t primary,
                                      int32_t secondary, int32_t action_id) {
    return {menu->callback, menu->callback_user_data, event, primary, secondary, action_id,
            MenuSoundCue::None, false};
}

void emit_menu_sound(MenuSoundCue cue) noexcept {
#if defined(_WIN32)
    if (cue == MenuSoundCue::MenuOpen)
        (void)MessageBeep(MB_ICONASTERISK);
    else if (cue == MenuSoundCue::RootSelect)
        (void)MessageBeep(MB_OK);
    else if (cue == MenuSoundCue::ChildActivate)
        (void)MessageBeep(MB_ICONEXCLAMATION);
#else
    (void)cue;
#endif
}

bool consume_sound_locked(sao_ui_menu_s* menu, MenuSoundCue cue) {
    if (menu->fps_pressure && cue != MenuSoundCue::MenuOpen)
        return false;
    int32_t* debounce = nullptr;
    if (cue == MenuSoundCue::MenuOpen)
        debounce = &menu->menu_open_sound_debounce_ms;
    else if (cue == MenuSoundCue::RootSelect)
        debounce = &menu->root_select_sound_debounce_ms;
    else if (cue == MenuSoundCue::ChildActivate)
        debounce = &menu->child_activate_sound_debounce_ms;
    if (debounce == nullptr || *debounce > 0)
        return false;
    *debounce = 72;
    return true;
}

PendingMenuEvent capture_sound_event_locked(sao_ui_menu_s* menu, SaoUiMenuEvent event,
                                            int32_t primary, int32_t secondary, int32_t action_id,
                                            MenuSoundCue cue) {
    PendingMenuEvent pending = capture_event_locked(menu, event, primary, secondary, action_id);
    pending.sound = cue;
    pending.play_sound = consume_sound_locked(menu, cue);
    return pending;
}

void dispatch_event_noexcept(const PendingMenuEvent& pending) noexcept {
    if (pending.play_sound)
        emit_menu_sound(pending.sound);
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
int32_t ring_hit_test(const sao_ui_menu_s* menu, int32_t button_count, int32_t px, int32_t py) {
    if (menu == nullptr || button_count <= 0)
        return -1;
    int32_t hit = -1;
    double best_distance = std::numeric_limits<double>::max();
    for (int32_t index = 0; index < button_count; ++index) {
        const float focus = index < static_cast<int32_t>(menu->root_hover_values.size())
                                ? menu->root_hover_values[static_cast<size_t>(index)]
                                : 0.0F;
        const SaoUiMenuButtonRect rect =
            compute_ring_rect(menu->layout, index, button_count, focus);
        const double center_x = static_cast<double>(rect.x) + static_cast<double>(rect.w) * 0.5;
        const double center_y = static_cast<double>(rect.y) + static_cast<double>(rect.h) * 0.5;
        const double dx = static_cast<double>(px) - center_x;
        const double dy = static_cast<double>(py) - center_y;
        const double radius = static_cast<double>(rect.w) * 0.5;
        const double distance_squared = dx * dx + dy * dy;
        if (distance_squared <= radius * radius && distance_squared < best_distance) {
            best_distance = distance_squared;
            hit = index;
        }
    }
    return hit;
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

ChildMenu* find_child_menu_locked(sao_ui_menu_s* menu, const std::string& parent_name) {
    const auto found = std::find_if(menu->children.begin(), menu->children.end(),
                                    [&parent_name](const ChildMenu& child_menu) {
                                        return child_menu.parent_name == parent_name;
                                    });
    return found == menu->children.end() ? nullptr : &*found;
}

const ChildMenu* find_child_menu_locked(const sao_ui_menu_s* menu, const std::string& parent_name) {
    const auto found = std::find_if(menu->children.begin(), menu->children.end(),
                                    [&parent_name](const ChildMenu& child_menu) {
                                        return child_menu.parent_name == parent_name;
                                    });
    return found == menu->children.end() ? nullptr : &*found;
}

bool has_root_name(const std::vector<MenuItem>& items, const std::string& name) {
    return std::any_of(items.begin(), items.end(),
                       [&name](const MenuItem& item) { return item.name == name; });
}

bool item_disabled(const MenuItem& item) {
    return !item.can_activate || item.state == SAO_UI_MENU_BTN_DISABLED;
}

int32_t find_activatable_root(const std::vector<MenuItem>& items, const std::string& name) {
    const auto found = std::find_if(items.begin(), items.end(), [&name](const MenuItem& item) {
        return item.name == name && item.can_activate;
    });
    return found == items.end() ? -1 : static_cast<int32_t>(found - items.begin());
}

int32_t find_root(const std::vector<MenuItem>& items, const std::string& name) {
    const auto found = std::find_if(items.begin(), items.end(),
                                    [&name](const MenuItem& item) { return item.name == name; });
    return found == items.end() ? -1 : static_cast<int32_t>(found - items.begin());
}

bool is_child_phase(SaoUiMenuPhase phase) {
    return phase == SAO_UI_MENU_PHASE_CHILD_OPENING || phase == SAO_UI_MENU_PHASE_CHILD_OPEN ||
           phase == SAO_UI_MENU_PHASE_CHILD_CLOSING;
}

template <size_t Capacity>
void copy_fixed_utf8(std::array<char, Capacity>* destination, const std::string& source) {
    static_assert(Capacity > 0);
    destination->fill('\0');
    size_t byte_count = std::min(source.size(), Capacity - 1);
    if (byte_count < source.size()) {
        while (byte_count > 0 &&
               (static_cast<unsigned char>(source[byte_count]) & 0xC0U) == 0x80U) {
            --byte_count;
        }
    }
    std::memcpy(destination->data(), source.data(), byte_count);
}

void mark_visual_changed_locked(sao_ui_menu_s* menu) {
    ++menu->visual_revision;
}

void clear_child_visual_locked(sao_ui_menu_s* menu) {
    menu->displayed_parent_name.clear();
    menu->pending_parent_name.clear();
    menu->child_hover_idx = -1;
    menu->child_slide_elapsed_ms = 0;
    menu->child_fade_t = 1.0F;
    menu->child_rail_glow_t = 0.0F;
}

void reset_child_rows_locked(ChildMenu* child_menu) {
    if (child_menu == nullptr)
        return;
    child_menu->visible_widths.assign(child_menu->items.size(), 0);
    child_menu->hover_values.assign(child_menu->items.size(), 0.0F);
}

float advance_hover_value(float current, float target, int32_t dt_ms, int32_t duration_ms) {
    if (dt_ms <= 0 || current == target)
        return current;
    if (duration_ms <= 0)
        return target;
    const float step = std::clamp(static_cast<float>(dt_ms) /
                                      static_cast<float>(duration_ms),
                                  0.0F, 1.0F);
    if (target > current)
        return std::min(target, current + step);
    return std::max(target, current - step);
}

void advance_root_hover_locked(sao_ui_menu_s* menu, int32_t dt_ms) {
    bool changed = false;
    if (menu->root_hover_values.size() != menu->items.size()) {
        menu->root_hover_values.resize(menu->items.size(), 0.0F);
        changed = true;
    }
    for (size_t index = 0; index < menu->items.size(); ++index) {
        const float target = static_cast<int32_t>(index) == menu->hover_idx &&
                                     !item_disabled(menu->items[index])
                                 ? 1.0F
                                 : 0.0F;
        const int32_t duration = target > menu->root_hover_values[index] ? kRootHoverInMs
                                                                         : kRootHoverOutMs;
        const float next =
            advance_hover_value(menu->root_hover_values[index], target, dt_ms, duration);
        if (next != menu->root_hover_values[index]) {
            menu->root_hover_values[index] = next;
            changed = true;
        }
    }
    if (changed)
        mark_visual_changed_locked(menu);
}

void begin_child_fadein_locked(sao_ui_menu_s* menu, const std::string& parent_name) {
    auto* child_menu = find_child_menu_locked(menu, parent_name);
    if (child_menu == nullptr || child_menu->items.empty()) {
        clear_child_visual_locked(menu);
        menu->phase = SAO_UI_MENU_PHASE_OPEN;
        menu->phase_elapsed_ms = 0;
        mark_visual_changed_locked(menu);
        return;
    }
    reset_child_rows_locked(child_menu);
    if (menu->reduced_motion) {
        std::fill(child_menu->visible_widths.begin(), child_menu->visible_widths.end(),
                  kChildTargetRowWidth);
        menu->displayed_parent_name = parent_name;
        menu->pending_parent_name.clear();
        menu->child_hover_idx = -1;
        menu->child_slide_elapsed_ms = 0;
        menu->child_fade_t = 0.0F;
        menu->phase = SAO_UI_MENU_PHASE_CHILD_OPEN;
        menu->phase_elapsed_ms = 0;
        mark_visual_changed_locked(menu);
        return;
    }
    menu->displayed_parent_name = parent_name;
    menu->pending_parent_name.clear();
    menu->child_hover_idx = -1;
    menu->child_slide_elapsed_ms = 0;
    menu->child_fade_t = 1.0F;
    menu->phase = SAO_UI_MENU_PHASE_CHILD_OPENING;
    menu->phase_elapsed_ms = 0;
    mark_visual_changed_locked(menu);
}

void close_child_phase_locked(sao_ui_menu_s* menu);

void begin_child_transition_locked(sao_ui_menu_s* menu, const std::string& next_parent_name) {
    if (!menu->displayed_parent_name.empty() && menu->reduced_motion) {
        if (next_parent_name.empty())
            close_child_phase_locked(menu);
        else
            begin_child_fadein_locked(menu, next_parent_name);
        return;
    }
    if (!menu->displayed_parent_name.empty()) {
        menu->pending_parent_name = next_parent_name;
        menu->child_hover_idx = -1;
        menu->phase = SAO_UI_MENU_PHASE_CHILD_CLOSING;
        menu->phase_elapsed_ms = 0;
        mark_visual_changed_locked(menu);
        return;
    }
    if (next_parent_name.empty()) {
        close_child_phase_locked(menu);
        return;
    }
    begin_child_fadein_locked(menu, next_parent_name);
}

void close_child_phase_locked(sao_ui_menu_s* menu) {
    clear_child_visual_locked(menu);
    if (is_child_phase(menu->phase)) {
        menu->phase = SAO_UI_MENU_PHASE_OPEN;
        menu->phase_elapsed_ms = 0;
    }
    mark_visual_changed_locked(menu);
}

int32_t child_slide_width(int32_t elapsed_ms, int32_t child_idx) {
    const int32_t local_elapsed = elapsed_ms - child_idx * kChildSlideStaggerMs;
    const double progress = std::clamp(static_cast<double>(local_elapsed) /
                                           static_cast<double>(kChildSlideMs),
                                       0.0, 1.0);
    const double inverse = 1.0 - progress;
    const double eased = 1.0 - inverse * inverse * inverse;
    return static_cast<int32_t>(
        std::lround(static_cast<double>(kChildTargetRowWidth) * eased));
}

void advance_child_rows_locked(sao_ui_menu_s* menu, int32_t dt_ms) {
    if (menu->displayed_parent_name.empty())
        return;
    auto* child_menu = find_child_menu_locked(menu, menu->displayed_parent_name);
    if (child_menu == nullptr)
        return;
    const int32_t previous_elapsed = menu->child_slide_elapsed_ms;
    const int32_t last_row = std::max(0, static_cast<int32_t>(child_menu->items.size()) - 1);
    const int32_t slide_end = kChildSlideMs + last_row * kChildSlideStaggerMs;
    if (dt_ms > 0) {
        const int64_t next_elapsed =
            static_cast<int64_t>(menu->child_slide_elapsed_ms) + static_cast<int64_t>(dt_ms);
        menu->child_slide_elapsed_ms =
            static_cast<int32_t>(std::min<int64_t>(next_elapsed, slide_end));
    }
    bool changed = previous_elapsed != menu->child_slide_elapsed_ms;
    for (int32_t i = 0; i < static_cast<int32_t>(child_menu->items.size()); ++i) {
        const int32_t width = child_slide_width(menu->child_slide_elapsed_ms, i);
        if (child_menu->visible_widths[static_cast<size_t>(i)] != width) {
            child_menu->visible_widths[static_cast<size_t>(i)] = width;
            changed = true;
        }
        const float target = menu->child_hover_idx == i &&
                                     !item_disabled(child_menu->items[static_cast<size_t>(i)])
                                 ? 1.0F
                                 : 0.0F;
        float& current = child_menu->hover_values[static_cast<size_t>(i)];
        const int32_t duration = target > current ? kChildHoverInMs : kChildHoverOutMs;
        const float next = advance_hover_value(current, target, dt_ms, duration);
        if (next != current) {
            current = next;
            changed = true;
        }
    }
    if (changed)
        mark_visual_changed_locked(menu);
    menu->child_rail_glow_t = advance_hover_value(menu->child_rail_glow_t,
                                                   menu->child_hover_idx >= 0 ? 1.0F : 0.0F,
                                                   dt_ms, 120);
}

SaoUiMenuButtonRect compute_child_rect(const SaoUiMenuLayout& layout, const ChildMenu& child_menu,
                                       int32_t child_idx) {
    const int32_t slot = layout.slot_size > 0 ? layout.slot_size : kDefaultSlotSize;
    const int32_t root_left = aligned_boundary(static_cast<float>(layout.center_x) -
                                               static_cast<float>(slot) * 0.5F);
    const int32_t stored_width = child_menu.visible_widths[child_idx];
    const int32_t visible_width = std::max(1, stored_width);
    const float left = static_cast<float>(sao::ui::menu_visual::visual_child_row_x(
        layout.center_x, slot, layout.child_ring_radius));
    const float top = static_cast<float>(layout.center_y + child_idx * kChildRowStride);
    return aligned_rect(left, top, left + static_cast<float>(visible_width),
                        top + static_cast<float>(kChildRowHeight));
}

int32_t child_hit_test_locked(const sao_ui_menu_s* menu, int32_t px, int32_t py,
                              int32_t* out_parent_idx) {
    if (menu->displayed_parent_name.empty() || !is_child_phase(menu->phase)) {
        return -1;
    }
    const int32_t displayed_parent_idx = find_root(menu->items, menu->displayed_parent_name);
    if (displayed_parent_idx < 0)
        return -1;
    const auto* child_menu = find_child_menu_locked(menu, menu->displayed_parent_name);
    if (child_menu == nullptr) {
        return -1;
    }
    for (int32_t i = 0; i < static_cast<int32_t>(child_menu->items.size()); ++i) {
        if (item_disabled(child_menu->items[static_cast<size_t>(i)]))
            continue;
        const auto rect = compute_child_rect(menu->layout, *child_menu, i);
        if (px >= rect.x && px < rect.x + rect.w && py >= rect.y && py < rect.y + rect.h) {
            *out_parent_idx = displayed_parent_idx;
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
    try {
        std::vector<MenuItem> replacement;
        replacement.reserve(item_count);
        for (size_t i = 0; i < item_count; ++i) {
            MenuItem item{};
            item.name = items[i].name_utf8 ? items[i].name_utf8 : "";
            item.icon = items[i].icon_utf8 ? items[i].icon_utf8 : "";
            item.action_id = items[i].action_id;
            item.can_activate = items[i].can_activate;
            item.state = item.can_activate ? SAO_UI_MENU_BTN_IDLE : SAO_UI_MENU_BTN_DISABLED;
            replacement.push_back(std::move(item));
        }

        std::lock_guard<std::mutex> lock(handle->mtx);
        int32_t next_active_idx = -1;
        if (handle->active_idx >= 0 &&
            handle->active_idx < static_cast<int32_t>(handle->items.size())) {
            next_active_idx =
                find_activatable_root(replacement, handle->items[handle->active_idx].name);
        }
        handle->children.erase(std::remove_if(handle->children.begin(), handle->children.end(),
                                              [&replacement](const ChildMenu& child_menu) {
                                                  return !has_root_name(replacement,
                                                                        child_menu.parent_name);
                                              }),
                               handle->children.end());
        handle->items = std::move(replacement);
        handle->root_hover_values.assign(handle->items.size(), 0.0F);
        handle->close_start_rows.clear();
        handle->active_idx = next_active_idx;
        handle->hover_idx = -1;
        if (next_active_idx >= 0) {
            handle->items[next_active_idx].state = SAO_UI_MENU_BTN_ACTIVE;
            auto* child_menu = find_child_menu_locked(handle, handle->items[next_active_idx].name);
            if (child_menu == nullptr || child_menu->items.empty()) {
                close_child_phase_locked(handle);
            } else {
                begin_child_fadein_locked(handle, handle->items[next_active_idx].name);
            }
        } else {
            close_child_phase_locked(handle);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
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
    try {
        ChildMenu replacement{};
        replacement.parent_name = parent_name_utf8;
        replacement.items.reserve(item_count);
        replacement.visible_widths.assign(item_count, 0);
        replacement.hover_values.assign(item_count, 0.0F);
        for (size_t i = 0; i < item_count; ++i) {
            MenuItem item{};
            item.name = items[i].name_utf8 ? items[i].name_utf8 : "";
            item.icon = items[i].icon_utf8 ? items[i].icon_utf8 : "";
            item.action_id = items[i].action_id;
            item.can_activate = items[i].can_activate;
            item.state = item.can_activate ? SAO_UI_MENU_BTN_IDLE : SAO_UI_MENU_BTN_DISABLED;
            replacement.items.push_back(std::move(item));
        }

        std::lock_guard<std::mutex> lock(handle->mtx);
        const int32_t parent_idx = find_root(handle->items, parent_name_utf8);
        if (parent_idx < 0) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        auto* target = find_child_menu_locked(handle, replacement.parent_name);
        if (target == nullptr) {
            handle->children.push_back(std::move(replacement));
            target = &handle->children.back();
        } else {
            *target = std::move(replacement);
        }
        if (handle->active_idx == parent_idx) {
            if (target->items.empty()) {
                close_child_phase_locked(handle);
            } else {
                begin_child_fadein_locked(handle, target->parent_name);
            }
        } else {
            mark_visual_changed_locked(handle);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
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
    PendingMenuEvent pending{};
    {
        std::lock_guard<std::mutex> lock(handle->mtx);
        handle->anchor_x = anchor_x;
        handle->anchor_y = anchor_y;
        if (handle->mode != SAO_UI_MENU_MODE_CASCADE) {
            handle->layout.center_x = anchor_x;
            handle->layout.center_y = anchor_y;
        }
        if (handle->phase == SAO_UI_MENU_PHASE_CLOSED || handle->phase == SAO_UI_MENU_PHASE_CLOSING) {
            handle->close_start_t = 0.0F;
            handle->close_start_rows.clear();
            handle->phase_elapsed_ms = 0;
            handle->transition_eased_t = 0.0F;
            handle->center_diffusion_t = 0.0F;
            handle->backdrop_lens_t = 0.0F;
            handle->open_spark_t = handle->reduced_motion || handle->fps_pressure ? 0.0F : 1.0F;
            handle->open_spark_count = handle->reduced_motion || handle->fps_pressure
                                           ? 0U
                                           : static_cast<uint32_t>(std::min<size_t>(24U, handle->items.size()));
            if (handle->reduced_motion) {
                handle->phase = SAO_UI_MENU_PHASE_OPEN;
                handle->transition_eased_t = 1.0F;
                handle->center_diffusion_t = 1.0F;
                handle->backdrop_lens_t = 1.0F;
                pending = capture_sound_event_locked(handle, SAO_UI_MENU_EV_OPENED, -1, -1, 0,
                                                     MenuSoundCue::MenuOpen);
            } else {
                handle->phase = SAO_UI_MENU_PHASE_OPENING;
            }
            mark_visual_changed_locked(handle);
        }
        handle->hud_bounds = SaoUiMenuHudBounds{};
        handle->hud_bounds.anchor_x = anchor_x;
        handle->hud_bounds.anchor_y = anchor_y;
    }
    dispatch_event_noexcept(pending);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_hide(sao_ui_menu_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    if (handle->phase == SAO_UI_MENU_PHASE_CLOSED)
        return SAO_STATUS_OK;
    capture_close_start_rows_locked(handle);
    handle->close_start_t = std::clamp(handle->transition_eased_t, 0.0F, 1.0F);
    handle->phase = SAO_UI_MENU_PHASE_CLOSING;
    handle->phase_elapsed_ms = 0;
    handle->close_suction_t = 0.0F;
    mark_visual_changed_locked(handle);
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
    int32_t child_parent_idx = -1;
    const int32_t child_idx = child_hit_test_locked(handle, x, y, &child_parent_idx);
    if (child_idx >= 0) {
        *out_menu_idx = child_parent_idx;
        if (out_child_idx != nullptr) {
            *out_child_idx = child_idx;
        }
        return SAO_STATUS_OK;
    }
    const int32_t n = visible_item_count_locked(handle);
    if (n == 0)
        return SAO_STATUS_OK;
    int32_t idx = -1;
    switch (handle->mode) {
    case SAO_UI_MENU_MODE_RING:
        idx = ring_hit_test(handle, n, x, y);
        break;
    case SAO_UI_MENU_MODE_VERTICAL_STRIP:
        idx = vertical_hit_test(handle->layout, n, x, y);
        break;
    case SAO_UI_MENU_MODE_CASCADE:
        idx = cascade_hit_test(handle->layout, n, x, y);
        break;
    }
    if (idx >= 0 && item_disabled(handle->items[static_cast<size_t>(idx)]))
        idx = -1;
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
        const int32_t n = visible_item_count_locked(handle);
        if (menu_idx < -1 || menu_idx >= n) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (menu_idx >= 0 && item_disabled(handle->items[static_cast<size_t>(menu_idx)]))
            menu_idx = -1;
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
        mark_visual_changed_locked(handle);
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
        const int32_t n = visible_item_count_locked(handle);
        if (menu_idx < 0 || menu_idx >= n) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        auto& it = handle->items[menu_idx];
        if (item_disabled(it))
            return SAO_STATUS_OK;
        const bool toggle_off = handle->active_idx == menu_idx;
        if (toggle_off) {
            it.state = handle->hover_idx == menu_idx ? SAO_UI_MENU_BTN_HOVER
                                                     : SAO_UI_MENU_BTN_IDLE;
            handle->active_idx = -1;
            begin_child_transition_locked(handle, "");
        } else {
            if (handle->active_idx >= 0 && handle->active_idx < n) {
                auto& previous = handle->items[handle->active_idx];
                if (previous.state == SAO_UI_MENU_BTN_ACTIVE) {
                    previous.state = handle->hover_idx == handle->active_idx
                                         ? SAO_UI_MENU_BTN_HOVER
                                         : SAO_UI_MENU_BTN_IDLE;
                }
            }
            handle->active_idx = menu_idx;
            it.state = SAO_UI_MENU_BTN_ACTIVE;
            const auto* child_menu = find_child_menu_locked(handle, it.name);
            begin_child_transition_locked(
                handle, child_menu != nullptr && !child_menu->items.empty() ? it.name : "");
        }
        handle->selection_trail_idx = menu_idx;
        handle->selection_trail_t = 1.0F;
        handle->pressed_pulse_idx = menu_idx;
        handle->pressed_pulse_t = 1.0F;
        handle->selection_spark_t = handle->reduced_motion || handle->fps_pressure ? 0.0F : 1.0F;
        handle->selection_spark_count = handle->reduced_motion || handle->fps_pressure ? 0U : 8U;
        mark_visual_changed_locked(handle);
        pending = capture_sound_event_locked(handle, SAO_UI_MENU_EV_ITEM_ACTIVATED, menu_idx, -1,
                                             it.action_id, MenuSoundCue::RootSelect);
    }
    dispatch_event_noexcept(pending);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_menu_set_child_row_visible_width(sao_ui_menu_handle_t handle, int32_t parent_menu_idx,
                                        int32_t child_idx, int32_t visible_width_px) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (visible_width_px < 0 || visible_width_px > kChildTargetRowWidth)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> lock(handle->mtx);
        if (parent_menu_idx < 0 || parent_menu_idx >= static_cast<int32_t>(handle->items.size())) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        auto* child_menu = find_child_menu_locked(handle, handle->items[parent_menu_idx].name);
        if (child_menu == nullptr)
            return SAO_STATUS_ERR_NOT_FOUND;
        if (child_idx < 0 || child_idx >= static_cast<int32_t>(child_menu->items.size())) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (child_menu->visible_widths[child_idx] != visible_width_px) {
            child_menu->visible_widths[child_idx] = visible_width_px;
            mark_visual_changed_locked(handle);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_compute_child_layout(
    sao_ui_menu_handle_t handle, int32_t parent_menu_idx, int32_t child_idx, int32_t* out_x,
    int32_t* out_y, int32_t* out_w, int32_t* out_h) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_x == nullptr || out_y == nullptr || out_w == nullptr || out_h == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(handle->mtx);
        if (parent_menu_idx < 0 || parent_menu_idx >= static_cast<int32_t>(handle->items.size())) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        const auto* child_menu =
            find_child_menu_locked(handle, handle->items[parent_menu_idx].name);
        if (child_menu == nullptr)
            return SAO_STATUS_ERR_NOT_FOUND;
        if (child_idx < 0 || child_idx >= static_cast<int32_t>(child_menu->items.size())) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        const auto rect = compute_child_rect(handle->layout, *child_menu, child_idx);
        *out_x = rect.x;
        *out_y = rect.y;
        *out_w = rect.w;
        *out_h = rect.h;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_menu_activate_child(sao_ui_menu_handle_t handle,
                                                               int32_t parent_menu_idx,
                                                               int32_t child_idx) {
    bool activated = false;
    int32_t action_id = -1;
    return sao::ui::menu_visual::activate_child(handle, parent_menu_idx, child_idx, &activated,
                                                 &action_id);
}

sao_status_t sao::ui::menu_visual::activate_child(sao_ui_menu_handle_t handle,
                                                   int32_t parent_menu_idx, int32_t child_idx,
                                                   bool* out_activated, int32_t* out_action_id) {
    if (out_activated == nullptr || out_action_id == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_activated = false;
    *out_action_id = -1;
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    PendingMenuEvent pending{};
    try {
        {
            std::lock_guard<std::mutex> lock(handle->mtx);
            if (parent_menu_idx < 0 ||
                parent_menu_idx >= static_cast<int32_t>(handle->items.size()) ||
                parent_menu_idx != handle->active_idx || !is_child_phase(handle->phase) ||
                handle->displayed_parent_name != handle->items[parent_menu_idx].name) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            const auto* child_menu =
                find_child_menu_locked(handle, handle->items[parent_menu_idx].name);
            if (child_menu == nullptr)
                return SAO_STATUS_ERR_NOT_FOUND;
            if (child_idx < 0 || child_idx >= static_cast<int32_t>(child_menu->items.size())) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            const auto& child = child_menu->items[child_idx];
            if (item_disabled(child))
                return SAO_STATUS_OK;
            *out_activated = true;
            *out_action_id = child.action_id;
            handle->selection_trail_idx = parent_menu_idx;
            handle->selection_trail_t = 1.0F;
            handle->pressed_pulse_idx = parent_menu_idx;
            handle->pressed_pulse_t = 1.0F;
            handle->selection_spark_t = handle->reduced_motion || handle->fps_pressure ? 0.0F : 1.0F;
            handle->selection_spark_count = handle->reduced_motion || handle->fps_pressure ? 0U : 8U;
            mark_visual_changed_locked(handle);
            pending = capture_sound_event_locked(handle, SAO_UI_MENU_EV_CHILD_SELECTED, child_idx,
                                                 parent_menu_idx, child.action_id,
                                                 MenuSoundCue::ChildActivate);
        }
        dispatch_event_noexcept(pending);
        return SAO_STATUS_OK;
    } catch (...) {
        *out_activated = false;
        *out_action_id = -1;
        return SAO_STATUS_ERR_UNKNOWN;
    }
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

SAO_UI_API sao_status_t SAO_UI_CALL
sao::ui::menu_visual::set_child_hover(sao_ui_menu_handle_t handle, int32_t parent_menu_idx,
                                      int32_t child_idx) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    if (child_idx == -1) {
        if (handle->child_hover_idx != -1) {
            handle->child_hover_idx = -1;
            mark_visual_changed_locked(handle);
        }
        return SAO_STATUS_OK;
    }
    if (parent_menu_idx < 0 || parent_menu_idx >= static_cast<int32_t>(handle->items.size()) ||
        handle->displayed_parent_name != handle->items[parent_menu_idx].name) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const auto* child_menu = find_child_menu_locked(handle, handle->displayed_parent_name);
    if (child_menu == nullptr)
        return SAO_STATUS_ERR_NOT_FOUND;
    if (child_idx < 0 || child_idx >= static_cast<int32_t>(child_menu->items.size())) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (item_disabled(child_menu->items[static_cast<size_t>(child_idx)]))
        return SAO_STATUS_OK;
    if (handle->child_hover_idx != child_idx) {
        handle->child_hover_idx = child_idx;
        mark_visual_changed_locked(handle);
    }
    return SAO_STATUS_OK;
}

SAO_UI_API sao_status_t SAO_UI_CALL
sao::ui::menu_visual::get_snapshot(sao_ui_menu_handle_t handle, Snapshot* out_snapshot) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        Snapshot snapshot{};
        {
            std::lock_guard<std::mutex> lock(handle->mtx);
            snapshot.active_root_idx = handle->active_idx;
            if (handle->active_idx >= 0 &&
                handle->active_idx < static_cast<int32_t>(handle->items.size())) {
                copy_fixed_utf8(&snapshot.active_root_name_utf8,
                                handle->items[handle->active_idx].name);
            }
            snapshot.displayed_parent_idx =
                find_root(handle->items, handle->displayed_parent_name);
            copy_fixed_utf8(&snapshot.displayed_parent_name_utf8,
                            handle->displayed_parent_name);
            snapshot.child_hover_idx = handle->child_hover_idx;
            snapshot.phase = handle->phase;
            snapshot.fade_t = handle->child_fade_t;
            snapshot.revision = handle->visual_revision;
            snapshot.transition_eased_t = handle->transition_eased_t;
            snapshot.center_diffusion_t = handle->center_diffusion_t;
            snapshot.close_suction_t = handle->close_suction_t;
            snapshot.selection_trail_idx = handle->selection_trail_idx;
            snapshot.selection_trail_t = handle->selection_trail_t;
            snapshot.pressed_pulse_idx = handle->pressed_pulse_idx;
            snapshot.pressed_pulse_t = handle->pressed_pulse_t;
            snapshot.child_rail_glow_t = handle->child_rail_glow_t;
            snapshot.backdrop_lens_t = handle->backdrop_lens_t;
            snapshot.open_spark_t = handle->open_spark_t;
            snapshot.selection_spark_t = handle->selection_spark_t;
            snapshot.open_spark_count = handle->open_spark_count;
            snapshot.selection_spark_count = handle->selection_spark_count;
            snapshot.reduced_motion = handle->reduced_motion;
            snapshot.fps_pressure = handle->fps_pressure;
            snapshot.roots.reserve(handle->items.size());
            for (size_t i = 0; i < handle->items.size(); ++i) {
                RootRowSnapshot row{};
                row.can_activate = handle->items[i].can_activate;
                row.state = handle->items[i].state;
                row.hover_t = i < handle->root_hover_values.size()
                                  ? handle->root_hover_values[i]
                                  : 0.0F;
                const int32_t index = static_cast<int32_t>(i);
                row.stagger_t = root_stagger_locked(handle, index);
                row.radial_t = row.stagger_t;
                row.selection_trail_t = handle->selection_trail_idx == index
                                            ? handle->selection_trail_t
                                            : 0.0F;
                row.pressed_pulse_t = handle->pressed_pulse_idx == index
                                          ? handle->pressed_pulse_t
                                          : 0.0F;
                snapshot.roots.push_back(row);
            }
            const auto* child_menu =
                find_child_menu_locked(handle, handle->displayed_parent_name);
            if (child_menu != nullptr) {
                snapshot.rows.reserve(child_menu->items.size());
                for (size_t i = 0; i < child_menu->items.size(); ++i) {
                    ChildRowSnapshot row{};
                    copy_fixed_utf8(&row.name_utf8, child_menu->items[i].name);
                    copy_fixed_utf8(&row.icon_utf8, child_menu->items[i].icon);
                    row.can_activate = child_menu->items[i].can_activate;
                    row.state = child_menu->items[i].state;
                    row.visible_width_px = child_menu->visible_widths[i];
                    row.hover_t = child_menu->hover_values[i];
                    row.stagger_t = static_cast<float>(child_menu->visible_widths[i]) /
                                    static_cast<float>(kChildTargetRowWidth);
                    row.radial_t = snapshot.child_rail_glow_t;
                    row.selection_trail_t = handle->selection_trail_idx == snapshot.active_root_idx
                                                ? handle->selection_trail_t
                                                : 0.0F;
                    row.pressed_pulse_t = handle->pressed_pulse_idx == snapshot.active_root_idx
                                              ? handle->pressed_pulse_t
                                              : 0.0F;
                    snapshot.rows.push_back(row);
                }
            }
        }
        *out_snapshot = std::move(snapshot);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t sao::ui::menu_visual::children_match(
    sao_ui_menu_handle_t handle, const char* parent_name_utf8,
    const SaoUiMenuItem* items, size_t item_count, bool* out_match) noexcept {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (parent_name_utf8 == nullptr || (items == nullptr && item_count > 0) ||
        out_match == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mtx);
    if (find_root(handle->items, parent_name_utf8) < 0)
        return SAO_STATUS_ERR_NOT_FOUND;
    const auto* child_menu = find_child_menu_locked(handle, parent_name_utf8);
    if (child_menu == nullptr || child_menu->items.size() != item_count) {
        *out_match = false;
        return SAO_STATUS_OK;
    }
    for (size_t index = 0; index < item_count; ++index) {
        const auto& current = child_menu->items[index];
        const char* name = items[index].name_utf8 == nullptr ? "" : items[index].name_utf8;
        const char* icon = items[index].icon_utf8 == nullptr ? "" : items[index].icon_utf8;
        if (current.name != name || current.icon != icon ||
            current.action_id != items[index].action_id ||
            current.can_activate != items[index].can_activate) {
            *out_match = false;
            return SAO_STATUS_OK;
        }
    }
    *out_match = true;
    return SAO_STATUS_OK;
}

sao_status_t sao::ui::menu_visual::get_child_menu_snapshot(
    sao_ui_menu_handle_t handle, const char* parent_name_utf8,
    ChildMenuSnapshot* out_snapshot) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (parent_name_utf8 == nullptr || out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        ChildMenuSnapshot snapshot{};
        {
            std::lock_guard<std::mutex> lock(handle->mtx);
            if (find_root(handle->items, parent_name_utf8) < 0)
                return SAO_STATUS_ERR_NOT_FOUND;
            snapshot.phase = handle->phase;
            snapshot.phase_elapsed_ms = handle->phase_elapsed_ms;
            snapshot.displayed_parent_name = handle->displayed_parent_name;
            snapshot.pending_parent_name = handle->pending_parent_name;
            snapshot.child_hover_idx = handle->child_hover_idx;
            snapshot.child_slide_elapsed_ms = handle->child_slide_elapsed_ms;
            snapshot.child_fade_t = handle->child_fade_t;
            snapshot.visual_revision = handle->visual_revision;
            const auto* child_menu = find_child_menu_locked(handle, parent_name_utf8);
            if (child_menu != nullptr) {
                snapshot.exists = true;
                snapshot.rows.reserve(child_menu->items.size());
                for (size_t index = 0; index < child_menu->items.size(); ++index) {
                    ChildMenuRowSnapshot row{};
                    row.name_utf8 = child_menu->items[index].name;
                    row.icon_utf8 = child_menu->items[index].icon;
                    row.action_id = child_menu->items[index].action_id;
                    row.can_activate = child_menu->items[index].can_activate;
                    row.state = child_menu->items[index].state;
                    row.visible_width_px = child_menu->visible_widths[index];
                    row.hover_t = child_menu->hover_values[index];
                    snapshot.rows.push_back(row);
                }
            }
        }
        *out_snapshot = std::move(snapshot);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t sao::ui::menu_visual::restore_child_menu_snapshot(
    sao_ui_menu_handle_t handle, const char* parent_name_utf8,
    const ChildMenuSnapshot& snapshot) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (parent_name_utf8 == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        ChildMenu replacement{};
        replacement.parent_name = parent_name_utf8;
        if (snapshot.exists) {
            replacement.items.reserve(snapshot.rows.size());
            replacement.visible_widths.reserve(snapshot.rows.size());
            replacement.hover_values.reserve(snapshot.rows.size());
            for (const auto& row : snapshot.rows) {
                MenuItem item{};
                item.name = row.name_utf8;
                item.icon = row.icon_utf8;
                item.action_id = row.action_id;
                item.can_activate = row.can_activate;
                item.state = row.state;
                replacement.items.push_back(std::move(item));
                replacement.visible_widths.push_back(row.visible_width_px);
                replacement.hover_values.push_back(row.hover_t);
            }
        }

        std::lock_guard<std::mutex> lock(handle->mtx);
        const int32_t parent_idx = find_root(handle->items, parent_name_utf8);
        if (parent_idx < 0)
            return SAO_STATUS_ERR_NOT_FOUND;
        auto* target = find_child_menu_locked(handle, parent_name_utf8);
        if (!snapshot.exists) {
            handle->children.erase(
                std::remove_if(handle->children.begin(), handle->children.end(),
                               [parent_name_utf8](const ChildMenu& child_menu) {
                                   return child_menu.parent_name == parent_name_utf8;
                               }),
                handle->children.end());
        } else if (target == nullptr) {
            handle->children.push_back(std::move(replacement));
        } else {
            *target = std::move(replacement);
        }
        handle->phase = snapshot.phase;
        handle->phase_elapsed_ms = snapshot.phase_elapsed_ms;
        handle->displayed_parent_name = snapshot.displayed_parent_name;
        handle->pending_parent_name = snapshot.pending_parent_name;
        handle->child_hover_idx = snapshot.child_hover_idx;
        handle->child_slide_elapsed_ms = snapshot.child_slide_elapsed_ms;
        handle->child_fade_t = snapshot.child_fade_t;
        handle->visual_revision = snapshot.visual_revision;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Menu-layout helper API (exported for tests + future compose path).
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
    const int32_t n = visible_item_count_locked(handle);
    if (button_index < 0 || button_index >= n) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    SaoUiMenuButtonRect r{};
    switch (handle->mode) {
    case SAO_UI_MENU_MODE_RING:
        r = compute_ring_rect(handle->layout, button_index, n,
                              button_index < static_cast<int32_t>(handle->root_hover_values.size())
                                  ? handle->root_hover_values[static_cast<size_t>(button_index)]
                                  : 0.0F);
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

// Advance root and child temporal state by dt_ms.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_tick(sao_ui_menu_handle_t handle,
                                                                int32_t dt_ms) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (dt_ms < 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PendingMenuEvent pending{};
    {
        std::lock_guard<std::mutex> lock(handle->mtx);
        const int64_t next_elapsed =
            static_cast<int64_t>(handle->phase_elapsed_ms) + static_cast<int64_t>(dt_ms);
        handle->phase_elapsed_ms = static_cast<int32_t>(
            std::min<int64_t>(next_elapsed, static_cast<int64_t>(INT32_MAX)));
        handle->menu_open_sound_debounce_ms =
            std::max(0, handle->menu_open_sound_debounce_ms - dt_ms);
        handle->root_select_sound_debounce_ms =
            std::max(0, handle->root_select_sound_debounce_ms - dt_ms);
        handle->child_activate_sound_debounce_ms =
            std::max(0, handle->child_activate_sound_debounce_ms - dt_ms);
        bool transient_changed = false;
        const auto advance_transient = [&](float* value, int32_t duration_ms) {
            const float next = advance_hover_value(*value, 0.0F, dt_ms, duration_ms);
            if (next != *value) {
                *value = next;
                transient_changed = true;
            }
        };
        advance_transient(&handle->selection_trail_t, 220);
        advance_transient(&handle->pressed_pulse_t, 180);
        advance_transient(&handle->selection_spark_t, 260);
        advance_root_hover_locked(handle, dt_ms);
        if (handle->phase == SAO_UI_MENU_PHASE_CHILD_OPENING ||
            handle->phase == SAO_UI_MENU_PHASE_CHILD_OPEN ||
            handle->phase == SAO_UI_MENU_PHASE_CHILD_CLOSING) {
            const float next_rail = advance_hover_value(
                handle->child_rail_glow_t, handle->child_hover_idx >= 0 ? 1.0F : 0.0F,
                dt_ms, 120);
            if (next_rail != handle->child_rail_glow_t) {
                handle->child_rail_glow_t = next_rail;
                transient_changed = true;
            }
        }
        if (transient_changed)
            mark_visual_changed_locked(handle);
        switch (handle->phase) {
        case SAO_UI_MENU_PHASE_OPENING: {
            const int32_t duration = handle->reduced_motion ? 1 : kMenuOpenMs;
            const float raw = std::clamp(static_cast<float>(handle->phase_elapsed_ms) /
                                             static_cast<float>(duration), 0.0F, 1.0F);
            handle->transition_eased_t = spring_progress(raw);
            handle->center_diffusion_t = handle->transition_eased_t;
            handle->backdrop_lens_t = handle->transition_eased_t;
            handle->open_spark_t = handle->reduced_motion || handle->fps_pressure
                                       ? 0.0F
                                       : std::max(0.0F, 1.0F - raw * 1.35F);
            mark_visual_changed_locked(handle);
            if (handle->phase_elapsed_ms >= duration) {
                handle->phase = SAO_UI_MENU_PHASE_OPEN;
                handle->phase_elapsed_ms = 0;
                pending = capture_sound_event_locked(handle, SAO_UI_MENU_EV_OPENED, -1, -1, 0,
                                                      MenuSoundCue::MenuOpen);
            }
            break;
        }
        case SAO_UI_MENU_PHASE_CLOSING: {
            const int32_t duration = handle->reduced_motion ? 1 : kMenuCloseMs;
            const float raw = std::clamp(static_cast<float>(handle->phase_elapsed_ms) /
                                             static_cast<float>(duration), 0.0F, 1.0F);
            handle->close_suction_t = spring_progress(raw);
            handle->transition_eased_t = handle->close_start_t * (1.0F - handle->close_suction_t);
            handle->center_diffusion_t = handle->transition_eased_t;
            handle->backdrop_lens_t = handle->transition_eased_t;
            mark_visual_changed_locked(handle);
            if (handle->phase_elapsed_ms >= duration) {
                handle->phase = SAO_UI_MENU_PHASE_CLOSED;
                handle->phase_elapsed_ms = 0;
                clear_child_visual_locked(handle);
                for (auto& item : handle->items) {
                    if (item.state == SAO_UI_MENU_BTN_ACTIVE ||
                        item.state == SAO_UI_MENU_BTN_HOVER) {
                        item.state = SAO_UI_MENU_BTN_IDLE;
                    }
                }
                handle->active_idx = -1;
                handle->hover_idx = -1;
                std::fill(handle->root_hover_values.begin(), handle->root_hover_values.end(),
                          0.0F);
                handle->close_start_rows.clear();
                mark_visual_changed_locked(handle);
                pending = capture_event_locked(handle, SAO_UI_MENU_EV_CLOSED, -1, -1, 0);
            }
            break;
        }
        case SAO_UI_MENU_PHASE_CHILD_OPENING:
            advance_child_rows_locked(handle, dt_ms);
            {
                const float next_fade = std::clamp(
                    handle->child_fade_t -
                        static_cast<float>(dt_ms) / static_cast<float>(kChildFadeInMs),
                    0.0F, 1.0F);
                if (next_fade != handle->child_fade_t) {
                    handle->child_fade_t = next_fade;
                    mark_visual_changed_locked(handle);
                }
            }
            if (handle->child_fade_t <= 0.001F) {
                handle->phase = SAO_UI_MENU_PHASE_CHILD_OPEN;
                handle->phase_elapsed_ms = 0;
                handle->child_fade_t = 0.0F;
                mark_visual_changed_locked(handle);
            }
            break;
        case SAO_UI_MENU_PHASE_CHILD_OPEN:
            advance_child_rows_locked(handle, dt_ms);
            handle->phase_elapsed_ms = 0;
            break;
        case SAO_UI_MENU_PHASE_CHILD_CLOSING:
            advance_child_rows_locked(handle, dt_ms);
            {
                const float next_fade = std::clamp(
                    handle->child_fade_t +
                        static_cast<float>(dt_ms) / static_cast<float>(kChildFadeOutMs),
                    0.0F, 1.0F);
                if (next_fade != handle->child_fade_t) {
                    handle->child_fade_t = next_fade;
                    mark_visual_changed_locked(handle);
                }
            }
            if (handle->child_fade_t >= 0.999F) {
                const std::string next_parent = handle->pending_parent_name;
                if (next_parent.empty()) {
                    clear_child_visual_locked(handle);
                    handle->phase = SAO_UI_MENU_PHASE_OPEN;
                    handle->phase_elapsed_ms = 0;
                    mark_visual_changed_locked(handle);
                } else {
                    begin_child_fadein_locked(handle, next_parent);
                }
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
        *out_progress = handle->transition_eased_t;
        break;
    case SAO_UI_MENU_PHASE_CLOSING:
        *out_progress = handle->transition_eased_t;
        break;
    case SAO_UI_MENU_PHASE_CHILD_OPENING:
    case SAO_UI_MENU_PHASE_CHILD_CLOSING:
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
    if (state == SAO_UI_MENU_BTN_DISABLED) {
        if (handle->hover_idx == button_index)
            handle->hover_idx = -1;
        if (handle->active_idx == button_index) {
            handle->active_idx = -1;
            begin_child_transition_locked(handle, "");
        }
    } else if (state == SAO_UI_MENU_BTN_HOVER) {
        handle->hover_idx = button_index;
    }
    if (state == SAO_UI_MENU_BTN_ACTIVE)
        handle->active_idx = button_index;
    mark_visual_changed_locked(handle);
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
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_set_visual_budget(
    sao_ui_menu_handle_t handle, bool reduced_motion, bool fps_pressure) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    const bool changed = handle->reduced_motion != reduced_motion || handle->fps_pressure != fps_pressure;
    handle->reduced_motion = reduced_motion;
    handle->fps_pressure = fps_pressure;
    if (reduced_motion || fps_pressure) {
        handle->open_spark_t = 0.0F;
        handle->selection_spark_t = 0.0F;
        handle->open_spark_count = 0;
        handle->selection_spark_count = 0;
    }
    if (reduced_motion) {
        if (handle->phase == SAO_UI_MENU_PHASE_CHILD_OPENING) {
            auto* child_menu = find_child_menu_locked(handle, handle->displayed_parent_name);
            if (child_menu != nullptr) {
                std::fill(child_menu->visible_widths.begin(), child_menu->visible_widths.end(),
                          kChildTargetRowWidth);
            }
            handle->child_fade_t = 0.0F;
            handle->phase = SAO_UI_MENU_PHASE_CHILD_OPEN;
            handle->phase_elapsed_ms = 0;
        } else if (handle->phase == SAO_UI_MENU_PHASE_CHILD_CLOSING) {
            const std::string next_parent = handle->pending_parent_name;
            if (next_parent.empty()) {
                clear_child_visual_locked(handle);
                handle->phase = SAO_UI_MENU_PHASE_OPEN;
                handle->phase_elapsed_ms = 0;
            } else {
                begin_child_fadein_locked(handle, next_parent);
            }
        }
    }
    if (changed)
        mark_visual_changed_locked(handle);
    return SAO_STATUS_OK;
}

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
