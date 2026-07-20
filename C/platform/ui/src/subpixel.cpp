// SAO Auto — subpixel compositing and fractional bar widths.
//
// 1:1 with `sao_auto/python/render/overlay_subpixel.py` (120 lines).
//
// Python authoritative behaviour:
//   subpixel_alpha_composite(dst, src, x, y, eps=1/512)
//     ix = floor(x); iy = floor(y)
//     fx = x - ix;   fy = y - iy
//     if snap:
//         if fx > 0.5: ix += 1
//         if fy > 0.5: iy += 1
//         alpha_composite(src, (ix, iy))
//     else:
//         shifted = _sao_cy_pixels.subpixel_shift_rgba_bytes(
//             src, w, h, fx, fy)  # bilinear, zero-OOB
//         alpha_composite(shifted, (ix, iy))
//
//   subpixel_bar_width(bar_img, frac_w)
//     if frac_w <= 0: return None
//     fw_int = ceil(frac_w)
//     frac = frac_w - floor(frac_w)
//     if fw_int > bar_w: fw_int = bar_w; frac = 0
//     cropped = bar[:, 0:fw_int]
//     if frac in eps-band: return cropped
//     else: cropped[:, fw_int-1, A] = (alpha * (frac*255)) // 255  # floor
//     return cropped
//
// C++ port operates on raw RGBA buffers (uint8_t* dst / src / bar / out)
// with straightforward pointer arithmetic — the semantics are identical
// but no PIL / numpy dependency is required.

#include "sao/ui/subpixel.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

// Same 1/512 epsilon as the Python module.
constexpr float kDefaultEps = 1.0f / 512.0f;

// Clamp helper — no <algorithm> because we want the same fixed
// generation on every compiler.
inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// floor(x) that also handles negatives correctly (mirrors Python
// `int(math.floor(x))`).
inline int32_t floor_i32(float x) {
    return static_cast<int32_t>(std::floor(x));
}

inline int32_t ceil_i32(float x) {
    return static_cast<int32_t>(std::ceil(x));
}

// Bilinear RGBA sample of `src` at (sx, sy) with zero-alpha OOB.  Matches
// `_sao_cy_pixels.subpixel_shift_rgba_bytes` semantics from Python:
//   * Weights on the 2×2 neighbourhood (ix0..ix1, iy0..iy1).
//   * Out-of-bounds contributors treated as (0, 0, 0, 0), so the border
//     fades to transparent — same as PIL fillcolor=(0,0,0,0) semantics.
inline void bilinear_sample_rgba(
    const uint8_t* src, int32_t src_w, int32_t src_h,
    float sx, float sy, uint8_t out_rgba[4]) {
    const int32_t ix0 = floor_i32(sx);
    const int32_t iy0 = floor_i32(sy);
    const float   fx  = sx - static_cast<float>(ix0);
    const float   fy  = sy - static_cast<float>(iy0);
    const int32_t ix1 = ix0 + 1;
    const int32_t iy1 = iy0 + 1;

    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 = fx          * (1.0f - fy);
    const float w01 = (1.0f - fx) * fy;
    const float w11 = fx          * fy;

    float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    auto tap = [&](int32_t x, int32_t y, float w) {
        if (x < 0 || x >= src_w || y < 0 || y >= src_h) {
            // OOB → transparent (matches Python fill=(0,0,0,0)).
            return;
        }
        const uint8_t* p = src + (static_cast<size_t>(y) * src_w + x) * 4u;
        acc[0] += p[0] * w;
        acc[1] += p[1] * w;
        acc[2] += p[2] * w;
        acc[3] += p[3] * w;
    };
    tap(ix0, iy0, w00);
    tap(ix1, iy0, w10);
    tap(ix0, iy1, w01);
    tap(ix1, iy1, w11);

    for (int i = 0; i < 4; ++i) {
        const float v = clampf(acc[i], 0.0f, 255.0f);
        out_rgba[i] = static_cast<uint8_t>(v + 0.5f);
    }
}

// Straight alpha_composite of an RGBA source onto an RGBA destination at
// integer offset (ix, iy).  Only writes within the destination bounds.
// Matches PIL's `Image.alpha_composite` in "over" operation.
inline void alpha_composite_rgba(
    uint8_t* dst, int32_t dst_w, int32_t dst_h,
    const uint8_t* src, int32_t src_w, int32_t src_h,
    int32_t ix, int32_t iy) {
    for (int32_t sy = 0; sy < src_h; ++sy) {
        const int32_t dy = iy + sy;
        if (dy < 0 || dy >= dst_h) continue;
        for (int32_t sx = 0; sx < src_w; ++sx) {
            const int32_t dx = ix + sx;
            if (dx < 0 || dx >= dst_w) continue;
            const uint8_t* sp = src + (static_cast<size_t>(sy) * src_w + sx) * 4u;
            uint8_t*       dp = dst + (static_cast<size_t>(dy) * dst_w + dx) * 4u;
            const uint32_t sa = sp[3];
            if (sa == 0u) continue;
            if (sa == 255u) {
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
                dp[3] = 255u;
                continue;
            }
            // Straight-alpha "over" (source is NOT premultiplied — this
            // matches PIL RGBA semantics that the Python code operates
            // on).  Uses 8-bit integer math with rounding via +127.
            const uint32_t da  = dp[3];
            const uint32_t inv = 255u - sa;
            const uint32_t out_a = sa + (da * inv + 127u) / 255u;
            if (out_a == 0u) {
                dp[0] = dp[1] = dp[2] = dp[3] = 0u;
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                const uint32_t s  = sp[c] * sa;
                const uint32_t d  = dp[c] * (da * inv / 255u);
                const uint32_t v  = (s + d + out_a / 2u) / out_a;
                dp[c] = static_cast<uint8_t>(v > 255u ? 255u : v);
            }
            dp[3] = static_cast<uint8_t>(out_a);
        }
    }
}

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_subpixel_composite_rgba(
    uint8_t* dst_rgba, int32_t dst_w, int32_t dst_h,
    const uint8_t* src_rgba, int32_t src_w, int32_t src_h,
    float x, float y, float eps_px) {
    if (dst_rgba == nullptr || src_rgba == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (dst_w <= 0 || dst_h <= 0 || src_w < 0 || src_h < 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (src_w == 0 || src_h == 0) {
        // Nothing to composite — matches Python early-return on w<=0.
        return SAO_STATUS_OK;
    }

    float eps = eps_px;
    if (!(eps > 0.0f)) {
        eps = kDefaultEps;
    }

    int32_t ix = floor_i32(x);
    int32_t iy = floor_i32(y);
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);

    // Snap-to-int corner: matches Python's `(fx < eps or fx > 1-eps) and
    // (fy < eps or fy > 1-eps)` short-circuit.  The extra +1 on rounding
    // handles negative fractional shifts landing on a full unit.
    const bool fx_snap = (fx < eps) || (fx > 1.0f - eps);
    const bool fy_snap = (fy < eps) || (fy > 1.0f - eps);
    if (fx_snap && fy_snap) {
        if (fx > 0.5f) ix += 1;
        if (fy > 0.5f) iy += 1;
        alpha_composite_rgba(
            dst_rgba, dst_w, dst_h,
            src_rgba, src_w, src_h,
            ix, iy);
        return SAO_STATUS_OK;
    }

    // Bilinear shift path — resample src at (px + fx, py + fy) per
    // pixel, then composite at (ix, iy).  Zero-OOB sampling gives the
    // 1-pixel padded feather that PIL fillcolor=(0,0,0,0) would produce.
    for (int32_t py = 0; py < src_h; ++py) {
        const int32_t dy = iy + py;
        if (dy < 0 || dy >= dst_h) continue;
        for (int32_t px = 0; px < src_w; ++px) {
            const int32_t dx = ix + px;
            if (dx < 0 || dx >= dst_w) continue;
            uint8_t sample[4];
            bilinear_sample_rgba(
                src_rgba, src_w, src_h,
                static_cast<float>(px) + fx,
                static_cast<float>(py) + fy,
                sample);
            const uint32_t sa = sample[3];
            if (sa == 0u) continue;
            uint8_t* dp = dst_rgba + (static_cast<size_t>(dy) * dst_w + dx) * 4u;
            if (sa == 255u) {
                dp[0] = sample[0];
                dp[1] = sample[1];
                dp[2] = sample[2];
                dp[3] = 255u;
                continue;
            }
            const uint32_t da  = dp[3];
            const uint32_t inv = 255u - sa;
            const uint32_t out_a = sa + (da * inv + 127u) / 255u;
            if (out_a == 0u) {
                dp[0] = dp[1] = dp[2] = dp[3] = 0u;
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                const uint32_t s = sample[c] * sa;
                const uint32_t d = dp[c] * (da * inv / 255u);
                const uint32_t v = (s + d + out_a / 2u) / out_a;
                dp[c] = static_cast<uint8_t>(v > 255u ? 255u : v);
            }
            dp[3] = static_cast<uint8_t>(out_a);
        }
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_subpixel_bar_width_rgba(
    const uint8_t* bar_rgba, int32_t bar_w, int32_t bar_h,
    float frac_w,
    uint8_t* out_rgba, int32_t* out_width) {
    if (out_width != nullptr) *out_width = 0;
    if (bar_rgba == nullptr || out_rgba == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (bar_w <= 0 || bar_h <= 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!(frac_w > 0.0f)) {
        // Python returns None here — for a C API, we distinguish with
        // ERR_INVALID_ARGUMENT since out_width=0 already conveyed the
        // "nothing to draw" state.
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    int32_t fw_int = ceil_i32(frac_w);
    float   frac   = frac_w - std::floor(frac_w);
    if (fw_int > bar_w) {
        fw_int = bar_w;
        frac = 0.0f;
    }
    if (out_width != nullptr) *out_width = fw_int;

    // Copy the crop.
    for (int32_t y = 0; y < bar_h; ++y) {
        const uint8_t* src_row = bar_rgba +
            static_cast<size_t>(y) * bar_w * 4u;
        uint8_t* dst_row = out_rgba +
            static_cast<size_t>(y) * fw_int * 4u;
        std::memcpy(dst_row, src_row, static_cast<size_t>(fw_int) * 4u);
    }

    // Snap band on the fractional part — if within kDefaultEps of 0 or 1,
    // return the plain crop.  Matches the Python `if frac < eps or frac >
    // 1-eps` shortcut.
    if (frac < kDefaultEps || frac > (1.0f - kDefaultEps)) {
        return SAO_STATUS_OK;
    }

    // Multiply the final column's alpha by the fractional remainder
    // using floor semantics: `(alpha * frac255) // 255`.  Matches
    // `_sao_cy_pixels.fade_last_column_alpha_rgba_bytes`.
    const uint32_t frac255 = static_cast<uint32_t>(frac * 255.0f);
    for (int32_t y = 0; y < bar_h; ++y) {
        uint8_t* p = out_rgba +
            (static_cast<size_t>(y) * fw_int + (fw_int - 1)) * 4u;
        p[3] = static_cast<uint8_t>((p[3] * frac255) / 255u);
    }
    return SAO_STATUS_OK;
}

extern "C" int32_t SAO_UI_CALL sao_ui_subpixel_snap_or_floor(
    float value, float eps_px) {
    float eps = eps_px;
    if (!(eps > 0.0f)) {
        eps = kDefaultEps;
    }
    int32_t iv = floor_i32(value);
    const float fv = value - static_cast<float>(iv);
    if (fv < eps) {
        return iv;
    }
    if (fv > 1.0f - eps) {
        return iv + 1;
    }
    // Standard floor when not in the snap band — the caller decides
    // whether to add 1 based on their own rounding preference.
    return iv;
}
