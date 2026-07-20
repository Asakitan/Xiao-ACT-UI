// SAO Auto — Direct2D widget kit.
//
// Python authoritative source:
//   `sao_auto/python/sao_panel_components.py` — action_button /
//     rounded_panel / sao_scrollbar / status_badge
//   `sao_auto/python/sao_gui/*.py` — all the specific panels
//   `sao_auto/python/gui_modules/*.py` — widget primitives
//   memory `面板组件库支持颜色覆盖` (component library color overrides)
//
// The native replacement for the Python `gui_modules/*.py` widget set.
// Widgets draw directly to a Direct2D-backed layer surface via a
// paint context.  Composition into panels happens through UI spec
// normalization in `engine/ui_spec.h`.
//
// ── Rendering pipeline ──────────────────────────────────────
//   Widget owns a persistent state (colors, sub-widget layout cache).
//   `paint(ctx, x, y, w, h)` draws into the D2D context — retained-
//   mode for internal caches (font layout, gradient stops, path
//   geometry) but immediate-mode drawing on each frame.  Caches
//   invalidate on `apply_props` when the relevant key changes.
//
// ── Sub-pixel rendering ─────────────────────────────────────
//   Widget positions accept float coordinates.  Slow tweens (HP drains,
//   caption drift, fisheye breathing) are visibly stair-stepped when
//   snapped to a 1-px grid.  Mirrors `render/overlay_subpixel.py`
//   semantics: fractional-offset bilinear blit for typical 100-500 px
//   sprites, sub-pixel bar width for progressive fill.  See
//   `subpixel.h` for the utility.
//
// ── Color override (memory 面板组件库支持颜色覆盖) ─────────
//   Every widget accepts optional fill/border/fg/canvas_bg color
//   overrides via apply_props.  Not-set → inherit theme token.  Set
//   → override even during theme swap.
//
// ── Active toggle (action_button.set_active) ────────────────
//   action_button supports an `active` state (memory: batch a80adbf).
//   When active=true the button paints in accent color instead of
//   base.  set_active() must invalidate the paint cache.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_widget_s* sao_ui_widget_handle_t;
typedef struct sao_ui_paint_ctx_s* sao_ui_paint_ctx_handle_t;
typedef struct sao_ui_offscreen_raster_s* sao_ui_offscreen_raster_handle_t;

struct SaoUiOffscreenRasterDesc {
    uint32_t width_px;
    uint32_t height_px;
    uint32_t clear_argb;
};

// Widget kinds — deliberately fixed.  Rich custom widgets live in
// plugins as canvas-op sequences (via `scripting/plugin_ui_render`).
enum sao_ui_widget_kind_e : int32_t {
    SAO_UI_WIDGET_ROUNDED_PANEL = 0,
    SAO_UI_WIDGET_ACTION_BUTTON = 1,
    SAO_UI_WIDGET_STATUS_BADGE  = 2,
    SAO_UI_WIDGET_SCROLLBAR     = 3,
    SAO_UI_WIDGET_TEXT          = 4,
    SAO_UI_WIDGET_BAR           = 5,   // progress / HP / duration
    SAO_UI_WIDGET_DIVIDER       = 6,
    SAO_UI_WIDGET_INPUT         = 7,
    SAO_UI_WIDGET_SLIDER        = 8,
    SAO_UI_WIDGET_TABLE         = 9,
    SAO_UI_WIDGET_DROPDOWN_BUTTON = 10,  // memory batch 38bafcf
    SAO_UI_WIDGET_TOOLTIP       = 11,
    SAO_UI_WIDGET_MORE_INDICATOR = 12,   // "▾" affordance for aggregated buttons
    SAO_UI_WIDGET_CHECKBOX      = 13,
    SAO_UI_WIDGET_RADIO         = 14,
    SAO_UI_WIDGET_ICON          = 15,
};

// Text alignment (bar labels, table cells).  Mirrors Tk's anchor semantics.
enum sao_ui_text_align_e : int32_t {
    SAO_UI_TEXT_ALIGN_LEFT      = 0,
    SAO_UI_TEXT_ALIGN_CENTER    = 1,
    SAO_UI_TEXT_ALIGN_RIGHT     = 2,
};

// Create a widget bound to a d3d11 device.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_create(
    int32_t widget_kind,
    void* d3d_device_ptr,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_widget_destroy(sao_ui_widget_handle_t handle);

// Apply a normalized property dict (UTF-8 JSON object) to the widget.
// Supported values are validated before the replacement state is committed;
// malformed JSON, non-object JSON, or an invalid supported value returns
// SAO_STATUS_ERR_INVALID_ARGUMENT without changing the current state.  The
// widget picks out the keys it cares about and ignores the rest.
// Standard keys accepted by all widget kinds:
//   { "fill": "#RRGGBB|#RRGGBBAA", "border": "#RRGGBB|#RRGGBBAA",
//     "fg": "#RRGGBB|#RRGGBBAA", "accent": "#RRGGBB|#RRGGBBAA",
//     "canvas_bg": "#RRGGBB|#RRGGBBAA", "radius": <number>, "padding": <int>,
//     "enabled": <bool>, "active": <bool>, "tooltip": "utf-8 str" }
// Six-digit colors are opaque.  Eight-digit colors place the trailing AA byte
// into the internal 0xAARRGGBB representation.
// Widget-specific keys documented in each widget's paint_hint.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_apply_props(
    sao_ui_widget_handle_t handle,
    const uint8_t* props_json_utf8,
    size_t props_len);

// Set / override a theme token by name.  argb_value follows Direct2D
// convention (0xAARRGGBB).  When set, this OVERRIDES the shared
// theme lookup even during theme swaps — matches the "color override
// stays across theme switch" memory note.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_set_theme_token(
    sao_ui_widget_handle_t handle,
    const char* token_key_utf8,
    uint32_t argb_value);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_clear_theme_token(
    sao_ui_widget_handle_t handle,
    const char* token_key_utf8);

// Render into a paint context.  Fractional coordinates are supported
// (see subpixel.h) — the widget picks integer alignment for its
// interior lines but positions its bounding rect at the requested
// fractional origin.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_paint(
    sao_ui_widget_handle_t handle,
    sao_ui_paint_ctx_handle_t ctx,
    float x, float y, float width, float height);

// Hit test — returns true if the widget accepts a click at the given
// local coord.  Used by the input router.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_hit_test(
    sao_ui_widget_handle_t handle,
    float local_x, float local_y,
    bool* out_hit);

// Active state (memory: action_button.active, batch a80adbf).
// When active=true, the widget paints in "pressed"/accent color.
// Invalidates the paint cache.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_set_active(
    sao_ui_widget_handle_t handle, bool active);

// ── Paint context ────────────────────────────────────────────

// A paint context wraps an ID2D1RenderTarget + typography sources
// (IDWriteFactory / IDWriteTextFormat cache).  Widgets take one
// during their paint() call.  Created by the compositor per-layer,
// released between layer draws.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_create(
    void* d2d_render_target,      // ID2D1RenderTarget*
    void* dwrite_factory,         // IDWriteFactory*
    sao_ui_paint_ctx_handle_t* out_ctx);

SAO_UI_API void SAO_UI_CALL sao_ui_paint_ctx_destroy(
    sao_ui_paint_ctx_handle_t ctx);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_begin_frame(
    sao_ui_paint_ctx_handle_t ctx);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_end_frame(
    sao_ui_paint_ctx_handle_t ctx);

// Push a rectangular clip (widgets use this for their bounds).
// Nested pushes are stacked; each push MUST be matched by a pop.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_push_clip(
    sao_ui_paint_ctx_handle_t ctx,
    float x, float y, float width, float height);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_pop_clip(
    sao_ui_paint_ctx_handle_t ctx);

// Multiplies alpha for subsequent primitives while preserving
// premultiplied-BGRA invariants.  Nested pushes multiply; every successful
// push must be matched by a pop.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_push_opacity(
    sao_ui_paint_ctx_handle_t ctx,
    float opacity_0_to_1);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_pop_opacity(
    sao_ui_paint_ctx_handle_t ctx);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_offscreen_raster_create(
    const SaoUiOffscreenRasterDesc* desc,
    sao_ui_offscreen_raster_handle_t* out_raster);

SAO_UI_API void SAO_UI_CALL sao_ui_offscreen_raster_destroy(
    sao_ui_offscreen_raster_handle_t raster);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_offscreen_raster_snapshot(
    sao_ui_offscreen_raster_handle_t raster,
    uint8_t* out_bgra_premultiplied,
    size_t capacity,
    size_t* out_bytes_written,
    uint32_t* out_width_px,
    uint32_t* out_height_px,
    uint32_t* out_stride_bytes);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_create_offscreen(
    sao_ui_offscreen_raster_handle_t raster,
    sao_ui_paint_ctx_handle_t* out_ctx);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_fill_rect(
    sao_ui_paint_ctx_handle_t ctx,
    float x, float y, float width, float height,
    uint32_t argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_stroke_line(
    sao_ui_paint_ctx_handle_t ctx,
    float x1, float y1, float x2, float y2,
    float width, uint32_t argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_fill_ellipse(
    sao_ui_paint_ctx_handle_t ctx,
    float x, float y, float width, float height,
    uint32_t argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_fill_polygon(
    sao_ui_paint_ctx_handle_t ctx,
    const int32_t* points_xy,
    size_t point_count,
    uint32_t argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_draw_utf8(
    sao_ui_paint_ctx_handle_t ctx,
    float x, float y,
    const char* text_utf8,
    float size_px,
    uint32_t argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_paint_ctx_blit_premultiplied_bgra(
    sao_ui_paint_ctx_handle_t ctx,
    const uint8_t* bgra_pixels,
    uint32_t source_width_px,
    uint32_t source_height_px,
    uint32_t source_stride_bytes,
    float x, float y, float width, float height);

#ifdef __cplusplus
}  // extern "C"
#endif
