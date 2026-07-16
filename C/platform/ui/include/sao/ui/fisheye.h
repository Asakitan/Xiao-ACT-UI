// SAO Auto — fisheye focus (hover-magnification).
//
// Python source of truth:
//   - `sao_auto/python/gui_modules/sao_gui_fisheye_mixin.py`
//     (SAOPlayerGUIFisheyeMixin — hit layer + z-order fights around
//      hover magnification, unified-compositor plumbing)
//   - `sao_auto/python/sao_theme/menu_bar.py`  (_on_fisheye /
//     _off_fisheye + _tick_float — the actual size lerp per button)
//   - `sao_auto/python/sao_theme/circle_button.py`  (SAOCircleButton
//     — the SIZE→MAX_SIZE growth curve, subpixel composite)
//   - `sao_auto/python/gui_modules/sao_menu_hud.py`
//     (MenuCircleButtonRenderer.render — the actual sprite the
//      fisheye scales into)
//
// The fisheye = a hover cursor magnifies the nearest button; buttons
// on either side inherit a smaller boost.  Mirrors Mac OS Dock.  In
// SAO context it's the primary interaction feedback on the ring
// menu: no click-target ambiguity, no tiny hit boxes.
//
// This module encodes the *math* — a config + apply function.  The
// actual rendering is done via popup.h / menu.h.  Keeping them
// separate lets other panels (plugin lists, damage rows) reuse the
// same lens with their own hit rectangles.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Config ────────────────────────────────────────────────────────
// Mirrors the constants in menu_bar_layout.py + circle_button.py.
// falloff_neighbors: how many buttons to either side of the hovered
//                    one receive a partial boost.  Default 2.
// scale_curve_gamma: exponent applied to distance falloff.  1.0 =
//                    linear falloff; 2.0 = quadratic (default).
struct SaoUiFisheyeConfig {
    int32_t base_size;             // idle diameter (default 54)
    int32_t max_size;              // fully-focused diameter (default 70)
    int32_t slot_size;             // spacing box (default 70)
    int32_t falloff_neighbors;     // default 2
    float   scale_curve_gamma;     // default 2.0
    float   grow_speed_lerp;       // per-tick blend factor (0..1);
                                   //   0.28 default (SIZE_LERP)
    float   grow_epsilon;          // when |size-target|<eps stop
                                   //   animating (default 0.18)
    float   hover_ease_ms;         // hover_t transition duration
                                   //   (default 200ms)
    bool    subpixel_snap;         // 0.25 px quantize (memory note
                                   //   [Tk Label图片Configure反馈环]
                                   //   for why float _size is worth it)
    bool    _pad[3];
};

SAO_UI_API const SaoUiFisheyeConfig* SAO_UI_CALL
    sao_ui_fisheye_default_config(void);

// ── Per-button state ──────────────────────────────────────────────
// Filled in by sao_ui_fisheye_apply for each visible button.  The
// caller allocates a buffer of these; the fisheye math writes them.
struct SaoUiFisheyeButton {
    // Input (client fills these)
    int32_t index;                 // 0..n
    int32_t slot_center_x;         // ring / column position (screen)
    int32_t slot_center_y;
    // Output (fisheye writes these)
    float   current_size;          // px, blended toward target
    float   target_size;
    float   hover_t;               // 0=idle, 1=focused
    int32_t sprite_center_x;       // screen coord for sprite paste
    int32_t sprite_center_y;
    bool    is_animating;          // still lerping toward target
    bool    _pad[3];
};

// ── Apply ─────────────────────────────────────────────────────────
// hover_target_idx: index of the currently-hovered button, or -1
// if none.  now_seconds: wall clock (for hover_t interpolation).
//
// This is called once per frame per menu.  Buttons whose is_animating
// stays true keep the render loop alive; the compositor uses that
// bit to decide whether to keep asking for redraws.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_apply(
    const SaoUiFisheyeConfig* config,
    SaoUiFisheyeButton* buttons,       // in/out
    size_t button_count,
    int32_t hover_target_idx,
    double now_seconds);

// Convenience: returns true iff ANY button in the array is still
// animating.  Compositors use this to gate the redraw request.
SAO_UI_API bool SAO_UI_CALL sao_ui_fisheye_any_animating(
    const SaoUiFisheyeButton* buttons,
    size_t button_count);

// ── Ring layout helper ────────────────────────────────────────────
// Positions n buttons evenly around a circle.  Fills slot_center_x/y
// on each SaoUiFisheyeButton.  Angle starts at start_angle_rad
// (0 = east); positive angles rotate counter-clockwise.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_ring_layout(
    int32_t center_x, int32_t center_y,
    int32_t radius,
    float start_angle_rad,
    SaoUiFisheyeButton* buttons,
    size_t button_count);

// ── Column layout helper ──────────────────────────────────────────
// Stacks n buttons vertically at column_x, starting at top_y.  Slot
// spacing = config->slot_size.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_column_layout(
    int32_t column_x, int32_t top_y,
    int32_t slot_size,
    SaoUiFisheyeButton* buttons,
    size_t button_count);

// ── Hit-test ──────────────────────────────────────────────────────
// Returns the button index whose current sprite contains (x, y), or
// -1 if none.  Uses each button's current_size (not the slot) so hit
// boxes grow with the fisheye — the way SAO ring menus actually feel.
SAO_UI_API int32_t SAO_UI_CALL sao_ui_fisheye_hit_test(
    const SaoUiFisheyeButton* buttons,
    size_t button_count,
    int32_t x, int32_t y);

// ── Wave 4 stateful lens (G3.6 first slice) ───────────────────────
// The pure-function API above is fine when the caller owns the animation
// clock (e.g. reads its own perf timer, feeds now_seconds).  For lens
// consumers that only speak in dt_ms deltas (unit tests, script bindings)
// a stateful lens is easier — sao_ui_fisheye_create() gives back a handle
// that owns the config + the last hover state; sao_ui_fisheye_animate
// blends from the last hover to the target over dt_ms.
typedef struct sao_ui_fisheye_s* sao_ui_fisheye_handle_t;

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_create(
    const SaoUiFisheyeConfig* config,
    sao_ui_fisheye_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_fisheye_destroy(
    sao_ui_fisheye_handle_t handle);

// Compute target sizes for the visible items given hover_target_index
// (or -1 for no hover).  distance metric: absolute index difference,
// matching the column layout used by SAOMenuBar.  Items whose distance
// from hover exceeds config.falloff_neighbors keep base_size; the
// hovered item snaps to max_size; interior neighbors scale down along
// a gamma-shaped falloff (see fisheye.cpp for the formula).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_apply_column_layout(
    sao_ui_fisheye_handle_t handle,
    SaoUiFisheyeButton* buttons,
    size_t button_count,
    int32_t hover_target_index);

// Blend from a previous hover target to a new one over dt_ms.  Writes
// the interpolated current_size and hover_t values into buttons[].
// If from_hover == to_hover, this is just a settle-toward-target step.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_animate(
    sao_ui_fisheye_handle_t handle,
    SaoUiFisheyeButton* buttons,
    size_t button_count,
    int32_t from_hover,
    int32_t to_hover,
    int32_t dt_ms);

#ifdef __cplusplus
}  // extern "C"
#endif
