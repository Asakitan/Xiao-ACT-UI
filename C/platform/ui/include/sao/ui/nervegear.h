// SAO Auto — NerveGear button (floating menu trigger).
//
// Python source of truth:
//   - `sao_auto/python/gui_modules/sao_gui_nervegear_button.py`
//     (GpuNerveGearButton — 72×72 draggable GPU disc, per-pixel hit
//      shape driven by alpha silhouette, Tk input proxy in unified
//      compositor mode)
//   - `sao_auto/python/sao_theme/circle_button.py`
//     (SAOCircleButton — the ring-button base class; shares palette
//      + hover_t easing curve)
//   - `sao_auto/python/sao_theme/link_start.py`
//     (SAOLinkStart — the Link Start intro animation; NerveGear
//      transitions through this on first activation)
//
// The NerveGear is the *primary* SAO branding element.  It stays
// visible even when the menu is collapsed, docked to some corner or
// user-chosen spot, and takes clicks to open the menu.  A right-click
// spawns the context menu (theme/quit/settings).  Drag to move.

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

typedef struct sao_ui_nervegear_s* sao_ui_nervegear_handle_t;

// ── Button size (matches Python SIZE = 72) ────────────────────────
// _HALF = 36.  Kept as a compile-time constant so hit-testing
// consumers don't drift.
enum { SAO_UI_NERVEGEAR_SIZE = 72 };

// ── State machine ─────────────────────────────────────────────────
// Mirrors the visual states in render_button(); the LINKING /
// LINKED / LOGOUT extras drive the SAOLinkStart intro animation.
enum SaoUiNerveGearState : int32_t {
    SAO_UI_NG_STATE_IDLE     = 0,
    SAO_UI_NG_STATE_HOVER    = 1,
    SAO_UI_NG_STATE_PRESSED  = 2,   // mouse-down, drag pending
    SAO_UI_NG_STATE_DRAGGING = 3,
    // Link Start intro path:
    SAO_UI_NG_STATE_LINKING  = 4,   // SAOLinkStart Phase 1-3 in flight
    SAO_UI_NG_STATE_LINKED   = 5,   // "SYSTEM >> CONNECTED" hold state
    SAO_UI_NG_STATE_LOGOUT   = 6,   // exit pulse
};

// ── Theme mode ────────────────────────────────────────────────────
// Matches _DARK / _LIGHT palettes in the Python source.  These are
// distinct from theme.h's SaoUiThemeId — the NerveGear disc has its
// own hand-tuned two-tone palette (cyan/gold) that doesn't map onto
// the whole-app theme.  Callers usually mirror the app theme (dark
// mode → NG dark) but can override.
enum SaoUiNerveGearPalette : int32_t {
    SAO_UI_NG_PALETTE_DARK  = 0,
    SAO_UI_NG_PALETTE_LIGHT = 1,
};

// ── Link Start animation timeline ─────────────────────────────────
// Mirrors SAOLinkStart._P1_END .. _P4_FADE_END.  Total ~9.9s.
// Exposed here so the NerveGear button can drive the state machine
// with real timings without duplicating them from Python.
struct SaoUiLinkStartTimeline {
    float startup_prelude;   // 0.72s aperture/scan
    float p1_end;            // 3.5s  colour tunnel end
    float p2_start;          // 3.5s  "Welcome to 咲 ACT UI!" text in
    float p2_end;            // 5.5s
    float p3_start;          // 5.2s  blue tunnel start (overlaps p2 fade)
    float p3_end;            // 7.5s
    float p4_start;          // 7.3s  white flash
    float p4_hold_end;       // 9.2s  SYSTEM >> CONNECTED hold
    float p4_fade_end;       // 9.9s  P4 text fade-out
    float total_duration;    // 10.0s
};

// Get the default timeline (compile-time constant).  Callers can
// tweak fields and hand it back via sao_ui_nervegear_set_timeline.
SAO_UI_API const SaoUiLinkStartTimeline* SAO_UI_CALL
    sao_ui_nervegear_default_timeline(void);

// ── Events ────────────────────────────────────────────────────────
enum SaoUiNerveGearEvent : int32_t {
    SAO_UI_NG_EV_LEFT_CLICK    = 0,   // open/close menu
    SAO_UI_NG_EV_RIGHT_CLICK   = 1,   // spawn context popup
    SAO_UI_NG_EV_DRAG_START    = 2,
    SAO_UI_NG_EV_DRAG_END      = 3,   // final x/y in event coords
    SAO_UI_NG_EV_HOVER_ENTER   = 4,
    SAO_UI_NG_EV_HOVER_LEAVE   = 5,
    SAO_UI_NG_EV_LINK_STARTED  = 6,
    SAO_UI_NG_EV_LINK_DONE     = 7,   // fires at p4_fade_end
    SAO_UI_NG_EV_LOGOUT_DONE   = 8,
};

typedef void (SAO_UI_CALL* sao_ui_nervegear_event_callback_t)(
    SaoUiNerveGearEvent event,
    int32_t x, int32_t y,           // screen coords at event time
    void* user_data);

// ── Lifecycle ─────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_create(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    int32_t initial_x, int32_t initial_y,
    SaoUiNerveGearPalette palette,
    sao_ui_nervegear_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_nervegear_destroy(
    sao_ui_nervegear_handle_t handle);

// ── Visibility / z-order ──────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_show(
    sao_ui_nervegear_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_hide(
    sao_ui_nervegear_handle_t handle);

// Raise this button above every other overlay layer.  Called after
// the menu closes so the disc stays clickable.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_raise_topmost(
    sao_ui_nervegear_handle_t handle);

// ── Position + palette ────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_set_position(
    sao_ui_nervegear_handle_t handle,
    int32_t x, int32_t y);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_get_position(
    sao_ui_nervegear_handle_t handle,
    int32_t* out_x, int32_t* out_y);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_set_palette(
    sao_ui_nervegear_handle_t handle,
    SaoUiNerveGearPalette palette);

// ── State transitions ─────────────────────────────────────────────
// Read-only view of the current state.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_get_state(
    sao_ui_nervegear_handle_t handle,
    SaoUiNerveGearState* out_state);

// Force a state transition.  Legal targets:
//   IDLE → LINKING              (start Link Start intro)
//   LINKED → LOGOUT             (start exit pulse)
//   any → IDLE                  (abort animation)
// Other transitions are driven internally by mouse events.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_transition(
    sao_ui_nervegear_handle_t handle,
    SaoUiNerveGearState target_state);

// Set the Link Start intro timeline (nullptr = restore defaults).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_set_timeline(
    sao_ui_nervegear_handle_t handle,
    const SaoUiLinkStartTimeline* timeline);

// ── Animation properties ──────────────────────────────────────────
// Master alpha (0..1).  Used by the intro/outro fade.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_set_alpha(
    sao_ui_nervegear_handle_t handle, float alpha);

// Manual glow phase override — normally the compositor ticks this
// automatically at ~2π/8 rad/s to produce the pulse.  Used by the
// intro sequence to sync the ring throb with the tunnel colour swap.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_set_glow_phase(
    sao_ui_nervegear_handle_t handle, float glow_phase);

// ── Hit shape ─────────────────────────────────────────────────────
// The visible disc is a circle inside the 72×72 sprite.  Corners of
// the sprite bounding box must fall through to the game process.
// The compositor's SetWindowRgn accumulation queries this — see
// input.h + z_order.h + [input proxy逐像素+防焦点偷] memory note.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_get_hit_shape(
    sao_ui_nervegear_handle_t handle,
    int32_t* out_center_x,
    int32_t* out_center_y,
    int32_t* out_radius);

// ── Events ────────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_set_event_callback(
    sao_ui_nervegear_handle_t handle,
    sao_ui_nervegear_event_callback_t callback,
    void* user_data);

#ifdef __cplusplus
}  // extern "C"
#endif
