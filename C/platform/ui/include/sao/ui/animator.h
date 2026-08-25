// SAO Auto — animation curve library + scheduler.
//
// Python source of truth:
//   - `sao_auto/python/sao_theme/animator.py`  (Animator class —
//     time-driven via `after()` with easing callback)
//   - `sao_auto/python/sao_theme/utils.py`  (ease_out / ease_in /
//     ease_in_out / ease_out_back_lite / lerp / lerp_color)
//   - `_sao_cy_uihelpers.pyx` (cython kernel — lerp_hex_color, plus
//     ease_out_cubic / lerp_clamped exposed to link_start.py)
//   - `sao_auto/python/render/overlay_scheduler.py`  (shared 60Hz
//     driver — animations register their tick_fn + still_animating
//     predicate, get called from a single pump)
//
// Every SAO animation uses time-driven progress (t=0..1) with an
// easing curve applied on top.  The Python side threads a single
// scheduler through every panel; this header mirrors that pattern
// while exposing individual curve functions for direct math.
//
// Coupled with scheduler.h — sao_ui_animator
// registers on the shared 60Hz overlay pump so we never spawn
// per-animation threads.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_animator_s* sao_ui_animator_handle_t;
typedef uint64_t sao_ui_animation_id_t;

// ── Curve identifiers ─────────────────────────────────────────────
// Ordered to match `utils.py` file order for grep-ability.
enum SaoUiCurve : int32_t {
    SAO_UI_CURVE_LINEAR              = 0,
    SAO_UI_CURVE_EASE_OUT            = 1,   // 1 - (1-t)^3
    SAO_UI_CURVE_EASE_IN             = 2,   // t^3
    SAO_UI_CURVE_EASE_IN_OUT         = 3,   // 3t^2 - 2t^3
    SAO_UI_CURVE_EASE_OUT_BACK_LITE  = 4,   // Cad-noob back cubic, clamped
    SAO_UI_CURVE_EASE_OUT_CUBIC      = 5,   // matches cython _cy_ease_out_cubic
    SAO_UI_CURVE_SPRING              = 6,   // popup.py's spring pop
    SAO_UI_CURVE_BOUNCE              = 7,
    SAO_UI_CURVE_STEP                = 8,   // discrete: 0 for t<1, 1 at t>=1
    SAO_UI_CURVE_CUBIC_BEZIER        = 9,   // custom p1x/p1y/p2x/p2y (SAOLinkStart camera)
};

// Bezier control points for SAO_UI_CURVE_CUBIC_BEZIER.  Defaults
// match SAOLinkStart._cam_z's (0.8, 0.1, 0.9, 0.8) — front-loaded
// slow start, back-loaded acceleration.
struct SaoUiBezierParams {
    float p1x;
    float p1y;
    float p2x;
    float p2y;
};

// ── Direct curve evaluators ───────────────────────────────────────
// Pure math, no state.  Clamps t to [0, 1].
SAO_UI_API float SAO_UI_CALL sao_ui_curve_evaluate(
    SaoUiCurve curve, float t);

SAO_UI_API float SAO_UI_CALL sao_ui_curve_evaluate_bezier(
    const SaoUiBezierParams* params, float t);

// Common linear interpolations (matches Python lerp / lerp_color).
SAO_UI_API float SAO_UI_CALL sao_ui_lerp_f32(
    float a, float b, float t);

SAO_UI_API int32_t SAO_UI_CALL sao_ui_lerp_i32(
    int32_t a, int32_t b, float t);

// ARGB colour lerp with alpha-preservation semantics.  Mirrors
// _sao_cy_uihelpers.lerp_hex_color which is the hot-path colour
// blend used by every hover fade and menu palette shift.
SAO_UI_API uint32_t SAO_UI_CALL sao_ui_lerp_argb(
    uint32_t argb_a, uint32_t argb_b, float t);

// ── Animation lifecycle ───────────────────────────────────────────
// Animation callback: t is the *eased* progress (0..1).  ud is the
// user_data passed at animate() time.
typedef void (SAO_UI_CALL* sao_ui_animation_tick_callback_t)(
    float eased_t,
    float raw_t,           // linear time progress before easing
    void* user_data);

// Completion callback: fires exactly once per animation, after the
// last tick.  cancelled == true if killed via cancel().
typedef void (SAO_UI_CALL* sao_ui_animation_done_callback_t)(
    bool cancelled,
    void* user_data);

struct SaoUiAnimationSpec {
    // Total duration in milliseconds.
    int32_t duration_ms;
    // Easing curve.  CUBIC_BEZIER requires bezier != nullptr.
    SaoUiCurve curve;
    const SaoUiBezierParams* bezier;   // nullptr except for CUBIC_BEZIER

    // Delay before first tick (default 0).
    int32_t delay_ms;

    // Callbacks.  on_tick MUST be non-null.  on_done is optional.
    sao_ui_animation_tick_callback_t on_tick;
    sao_ui_animation_done_callback_t on_done;
    void* user_data;

    // Optional dedup key.  Non-empty strings replace any in-flight
    // animation with the same key on the same handle (matches
    // Animator.animate('hover', ...) behavior).
    const char* dedup_key_utf8;
};

// ── Lifecycle ─────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animator_create(
    sao_ui_animator_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_animator_destroy(
    sao_ui_animator_handle_t handle);

// Kick off a new animation.  Returns an id you can use for cancel/
// query.  The animation runs on the overlay scheduler pump (60Hz);
// the tick callback fires on that thread.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animator_animate(
    sao_ui_animator_handle_t handle,
    const SaoUiAnimationSpec* spec,
    sao_ui_animation_id_t* out_id);

// Cancel a specific animation.  Fires on_done with cancelled=true.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animator_cancel(
    sao_ui_animator_handle_t handle,
    sao_ui_animation_id_t id);

// Cancel all animations on this handle (e.g. on widget teardown).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animator_cancel_all(
    sao_ui_animator_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animator_is_running(
    sao_ui_animator_handle_t handle,
    sao_ui_animation_id_t id,
    bool* out_running);

// ── Driver ────────────────────────────────────────────────────────
// Called by the overlay scheduler each 60Hz tick.  Advances every
// active animation, fires tick callbacks, drops completed ones.
// dt_seconds is the wall-clock delta since last call.  When
// integrating with the compositor scheduler, this is the single
// entry point per handle.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_animator_tick(
    sao_ui_animator_handle_t handle,
    double now_seconds);

// Returns true iff there is at least one active animation — the
// scheduler uses this to decide whether to keep the 60Hz pump busy
// (matches Python's still_animating() predicate).
SAO_UI_API bool SAO_UI_CALL sao_ui_animator_has_active(
    sao_ui_animator_handle_t handle);

// Continuous spring evaluator used by the Entity/Menu visual compositor.
SAO_UI_API float SAO_UI_CALL sao_ui_curve_evaluate_spring_continuous(float t);
SAO_UI_API bool SAO_UI_CALL sao_ui_reduced_motion_enabled(void);
SAO_UI_API int32_t SAO_UI_CALL sao_ui_animation_duration_ms(int32_t duration_ms);

#ifdef __cplusplus
}  // extern "C"
#endif
