// SAO Auto — sub-pixel compose / bar-width helpers.
//
// Python authoritative source: `sao_auto/python/render/overlay_subpixel.py` (120 lines)
//
// PIL's `Image.alpha_composite` only accepts integer offsets, and
// `int(round(...))` on a width snaps every animation to a 1-pixel grid.
// Slow tweens (HP drains, caption drift, fisheye breathing) are then
// visibly stair-stepped because the on-screen position only updates
// 1 px every several frames.  These helpers let those animations
// move smoothly between integer pixels.
//
// ── subpixel_alpha_composite ─────────────────────────────────
//   Shifts `src` by the fractional remainder of (x, y) using
//   bilinear affine transform, then composites at the integer base.
//   Single transform + composite, ~0.3-1 ms for typical 100-500 px
//   sprites.  When the fractional part is smaller than `eps` (default
//   1/512 px), falls back to plain integer composite (cached-layout
//   optimization).
//
// ── subpixel_bar_width ───────────────────────────────────────
//   Trim `bar_img` to a fractional width.  Returns image of
//   `ceil(frac_w)` px width whose FINAL column has its alpha
//   multiplied by the fractional remainder — so a bar growing from
//   100.0 → 100.99 px visibly fades the 101st column in instead of
//   jumping at 100.5.
//
// ── Cython accelerator paths ─────────────────────────────────
//   Python has _sao_cy_pixels.{subpixel_shift_rgba_bytes,
//   fade_last_column_alpha_rgba_bytes} fast paths.  C++ port uses
//   pixel kernels in `platform/core/pixel_kernels.h` — same
//   semantic, native perf.  MUST match floor semantics:
//   `(alpha * frac255) // 255`.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Composite `src` (RGBA 8-bit) into `dst` (RGBA 8-bit) at fractional
// (x, y).  dst is modified in place.  Falls back to integer composite
// when fractional shift is smaller than `eps_px`.
// Recommended eps_px:
//   0.002 (=1/512) for small sprites (button ~20 px)
//   0.150         for large sprites (>200 px) — perceptual limit
//                 hides the snap, and the transform cost of a big
//                 sprite is ~30 ms.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_subpixel_composite_rgba(
    uint8_t* dst_rgba, int32_t dst_w, int32_t dst_h,
    const uint8_t* src_rgba, int32_t src_w, int32_t src_h,
    float x, float y, float eps_px);

// Trim `bar_rgba` (RGBA 8-bit) to a fractional width; write the
// result to `out_rgba` which must be at least `ceil(frac_w) * height *
// 4` bytes.  Returns SAO_STATUS_ERR_INVALID_ARGUMENT if frac_w <= 0
// (no bar to draw).
// out_width is set to `ceil(frac_w)`; frac_w > bar_w clamps to
// integer bar_w.  Semantics: final column's alpha = alpha *
// (frac_w - floor(frac_w)) using floor multiplication.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_subpixel_bar_width_rgba(
    const uint8_t* bar_rgba, int32_t bar_w, int32_t bar_h,
    float frac_w,
    uint8_t* out_rgba, int32_t* out_width);

// Round-to-integer with epsilon threshold — same as the Python
// helper's snap-to-int corner-case logic.  Used by callers that
// implement their own composite path.
SAO_UI_API int32_t SAO_UI_CALL sao_ui_subpixel_snap_or_floor(
    float value, float eps_px);

#ifdef __cplusplus
}  // extern "C"
#endif
