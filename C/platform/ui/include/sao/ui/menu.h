// SAO Auto — SAO main menu (fisheye ring, NerveGear centre, child bar).
//
// Python source of truth:
//   - `sao_auto/python/gui_modules/sao_gui_menu_hud.py`  (MenuHudOverlay
//     — the GPU-required layered HUD host)
//   - `sao_auto/python/gui_modules/sao_gui_menu_mixin.py`  (build/refresh
//     menu items + child menus)
//   - `sao_auto/python/gui_modules/sao_menu_hud.py`
//     (MenuHudSpriteRenderer / MenuCircleButtonRenderer /
//      MenuLeftInfoRenderer sprite pipeline)
//   - `sao_auto/python/sao_theme/menu_bar.py`  (SAOMenuBar,
//     fisheye column, 9 slots, GPU painter dispatch)
//   - `sao_auto/python/sao_theme/child_bar.py`  (SAOChildBar,
//     row list, slide-in animation)
//   - `sao_auto/python/sao_theme/popup_menu.py`  (legacy chroma-key
//     shell; the GPU path lives in ui_gpu/popup.py — see popup.h)
//
// This header owns the *layout math* and *state machine* of the SAO
// menu: where the ring buttons sit, when the child bar opens, how
// hover propagates.  The pixel pipeline is popup.h (GPU compose) +
// theme.h (colour tokens) + animator.h (easing curves).
//
// Coordinate convention: all positions are screen-relative and int32.
// This is deliberate — the Python side had a bug (memory note
// [连续反馈不对要停止调参数]) where content_w/h were cached but
// left/top were read live, so the child bar's slide-in de-synced the
// menu HUD backdrop.  Here the layout struct exposes BOTH — never
// mix a cached bound with a live one.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"
#include "sao/ui/theme.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_menu_s* sao_ui_menu_handle_t;

// ── Menu item POD ─────────────────────────────────────────────────
// Icon glyph is a UTF-8 string (Python uses '●', '⚔', '♪' etc.).
// The compose path renders it through the font atlas — no image slot.
// action_id is what OnActivate returns; -1 means there is no action token.
struct SaoUiMenuItem {
    const char* name_utf8; // display + child-menu registry key
    const char* icon_utf8; // glyph (1-2 chars typical)
    int32_t action_id;
    bool can_activate; // false means the row is disabled and click is a no-op
    bool _pad[3];
};

// ── Layout descriptor ─────────────────────────────────────────────
// Mirrors SAOMenuBar._SLOT = 70, SAOCircleButton.SIZE = 54,
// SAOCircleButton.MAX_SIZE = 70 (fisheye max grow), _MAX_VISIBLE = 9.
// These come from theme.h metrics tokens by default; callers can
// override via sao_ui_menu_set_layout.
struct SaoUiMenuLayout {
    int32_t center_x; // screen coord of the ring's centre
    int32_t center_y;
    int32_t inner_radius;      // NerveGear button lives here
    int32_t outer_radius;      // button-icon centres sit at this radius
    int32_t child_ring_radius; // second-tier expanded child buttons
    int32_t button_size;       // idle circle diameter (default 54)
    int32_t button_max_size;   // fisheye peak diameter (default 70)
    int32_t slot_size;         // spacing box (default 70)
    int32_t max_visible;       // clamp on visible count (default 9)
};

// ── Menu opening mode ─────────────────────────────────────────────
enum SaoUiMenuMode : int32_t {
    // Legacy vertical column anchored SE of a floating button.
    // Matches SAOMenuBar's default pack layout.
    SAO_UI_MENU_MODE_VERTICAL_STRIP = 0,

    // Full ring around the NerveGear centre (SAO NerveGear look).
    // Buttons distributed evenly around outer_radius.
    SAO_UI_MENU_MODE_RING = 1,

    // Apple-style slide-down from screen top (popup.py cascade_mode).
    SAO_UI_MENU_MODE_CASCADE = 2,
};

// ── Menu button state ─────────────────────────────────────────────
enum SaoUiMenuBtnState : int32_t {
    SAO_UI_MENU_BTN_IDLE = 0,
    SAO_UI_MENU_BTN_HOVER = 1,
    SAO_UI_MENU_BTN_ACTIVE = 2, // sticky-selected (child bar open)
    SAO_UI_MENU_BTN_DISABLED = 3,
};

// ── Menu-open animation phase ─────────────────────────────────────
// Mirrors SAOMenuBar.play_enter_animation / SAOChildBar row animation.
enum SaoUiMenuPhase : int32_t {
    SAO_UI_MENU_PHASE_CLOSED = 0,
    SAO_UI_MENU_PHASE_OPENING = 1, // fade-in + button drop
    SAO_UI_MENU_PHASE_OPEN = 2,
    SAO_UI_MENU_PHASE_CHILD_OPENING = 3,
    SAO_UI_MENU_PHASE_CHILD_OPEN = 4,
    SAO_UI_MENU_PHASE_CHILD_CLOSING = 5,
    SAO_UI_MENU_PHASE_CLOSING = 6, // fade-out + slide-up
};

// ── Menu event callback ───────────────────────────────────────────
enum SaoUiMenuEvent : int32_t {
    SAO_UI_MENU_EV_OPENED = 0,
    SAO_UI_MENU_EV_CLOSED = 1,
    SAO_UI_MENU_EV_ITEM_ACTIVATED = 2,   // top-level ring/strip button
    SAO_UI_MENU_EV_CHILD_SELECTED = 3,   // child bar row click
    SAO_UI_MENU_EV_BACKGROUND_CLICK = 4, // click outside content bbox
    SAO_UI_MENU_EV_HOVER_CHANGED = 5,    // hover_idx changed
};

typedef void(SAO_UI_CALL* sao_ui_menu_event_callback_t)(
    SaoUiMenuEvent event,
    int32_t primary_idx,   // menu item idx for ITEM_ACTIVATED,
                           // child row idx for CHILD_SELECTED,
                           // -1 for OPENED/CLOSED
    int32_t secondary_idx, // parent menu idx for CHILD_SELECTED
    int32_t action_id,     // resolved action from item POD
    void* user_data);

// ── Lifecycle ─────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_create(sao_ui_compositor_handle_t compositor,
                                                       sao_ui_theme_handle_t theme,
                                                       SaoUiMenuMode mode,
                                                       sao_ui_menu_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_menu_destroy(sao_ui_menu_handle_t handle);

// ── Content ───────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_set_items(sao_ui_menu_handle_t handle,
                                                          const SaoUiMenuItem* items,
                                                          size_t item_count);

// Register a child menu for one parent item.  parent_name_utf8 must
// match a previously-registered SaoUiMenuItem.name_utf8.  Child items
// use the same POD as top-level items and may include disabled rows.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_set_children(sao_ui_menu_handle_t handle,
                                                             const char* parent_name_utf8,
                                                             const SaoUiMenuItem* items,
                                                             size_t item_count);

// Layout override.  Passing zeros for any field keeps the metrics-
// table default from theme.h.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_set_layout(sao_ui_menu_handle_t handle,
                                                           const SaoUiMenuLayout* layout);

// ── Show / hide ───────────────────────────────────────────────────
// anchor_x/y: for VERTICAL_STRIP mode this is the SE anchor (matches
// the Python floating-button anchor pattern).  For RING mode it's the
// centre.  For CASCADE mode it's ignored (centred at screen top).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_show(sao_ui_menu_handle_t handle, int32_t anchor_x,
                                                     int32_t anchor_y);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_hide(sao_ui_menu_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_is_visible(sao_ui_menu_handle_t handle,
                                                           bool* out_visible);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_get_phase(sao_ui_menu_handle_t handle,
                                                          SaoUiMenuPhase* out_phase);

// ── Query / interaction ───────────────────────────────────────────
// Ray-cast a cursor position to (menu_idx, child_idx).  Uses the
// "ring hit test = radial band + angular sweep" fast path so we don't
// walk every button POD.  menu_idx == -1 when the point is off any
// button; child_idx == -1 when we're only over a top-level button.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_hit_test(sao_ui_menu_handle_t handle, int32_t x,
                                                         int32_t y, int32_t* out_menu_idx,
                                                         int32_t* out_child_idx);

// Set hover manually (for keyboard nav).  idx == -1 clears.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_set_hover(sao_ui_menu_handle_t handle,
                                                          int32_t menu_idx);

// Trigger the same activation path that a mouse click would.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_activate(sao_ui_menu_handle_t handle,
                                                         int32_t menu_idx);

// Update one child row's current slide-in width.  The accepted range is
// 0..240 px; hit testing follows max(1, visible_width_px), matching the
// active GPU popup while its first animation frame is still at width 0.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_menu_set_child_row_visible_width(sao_ui_menu_handle_t handle, int32_t parent_menu_idx,
                                        int32_t child_idx, int32_t visible_width_px);

// Query the active-GPU child-row rectangle.  The child column starts 25 px
// to the right of the 70 px root column, rows start at LIST_X=27, and each
// row occupies 44 px in a 47 px stride.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_compute_child_layout(
    sao_ui_menu_handle_t handle, int32_t parent_menu_idx, int32_t child_idx, int32_t* out_x,
    int32_t* out_y, int32_t* out_w, int32_t* out_h);

// Activate a visible child row.  The parent must be the active root item.
// Emits CHILD_SELECTED with primary=child_idx, secondary=parent_menu_idx,
// and action_id resolved from the child item.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_activate_child(sao_ui_menu_handle_t handle,
                                                               int32_t parent_menu_idx,
                                                               int32_t child_idx);

// ── HUD backdrop ──────────────────────────────────────────────────
// The frosted-glass menu backdrop with brackets, rails, and scan
// line.  Owned by the menu, but exposed here so plugins can query
// the current sprite pad / brackets when composing custom overlays.
//
// content_w/h: the inner bounding box that content sits in.
// sprite_off_x/y: negative padding — the HUD sprite starts before
//                 the content anchor.  Matches
//                 MenuHudSpriteRenderer._sprite_off.
// gpu_pad: the frame's outer margin around content.
struct SaoUiMenuHudBounds {
    int32_t anchor_x; // screen coord of content top-left
    int32_t anchor_y;
    int32_t content_w;
    int32_t content_h;
    int32_t sprite_off_x; // usually -gpu_pad
    int32_t sprite_off_y;
    int32_t gpu_pad;
    int32_t breath_dx; // current breathing offset
    int32_t breath_dy;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_query_hud_bounds(sao_ui_menu_handle_t handle,
                                                                 SaoUiMenuHudBounds* out_bounds);

// ── Events ────────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_set_event_callback(
    sao_ui_menu_handle_t handle, sao_ui_menu_event_callback_t callback, void* user_data);

#ifdef __cplusplus
} // extern "C"
#endif
