// SAO Auto — UI-side render hooks (frame-clock hook points).
//
// Complement to `engine/render_hook.h`.
//   * engine side  — plugin ui_spec / payload hooks per surface id
//                     (host-agnostic, JSON-only, unit-testable).
//   * ui side (here) — actual GPU render clock: before/after compositor,
//                     before/after present, before/after paint of a
//                     specific layer or panel.
//
// The two systems are complementary:
//   1. During a frame, the compositor first runs the engine hook chain
//      to resolve the final payload for each surface.
//   2. Then it runs the UI hook chain at each render-clock point so
//      plugins can inject extra draws (screen-space overlay text,
//      timers, damage-flash effects) directly into the paint context.
//
// Python source alignment:
//   * `act_platform/render_hooks.py`   — engine side (ui_spec overlays)
//   * `render/overlay_compositor.py`   — this side (per-tick draw callbacks)

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_render_hook_manager_s* sao_ui_render_hook_manager_handle_t;
typedef uint64_t sao_ui_render_hook_token_t;

// ─── Render-clock hook points ────────────────────────────────────────
enum sao_ui_render_hook_point_e : int32_t {
    // Fires once per frame before the compositor walks layers.  Plugins
    // can set global GL/D3D state (viewport, blend mode) here.
    SAO_UI_HOOK_BEFORE_COMPOSITOR = 0,

    // Fires once per frame after the compositor has painted every layer
    // but before Present.  Best point for full-screen overlay text
    // (Boss timer, DPS totals) that shouldn't be clipped by any panel.
    SAO_UI_HOOK_AFTER_COMPOSITOR  = 1,

    // Fires immediately before IDXGISwapChain::Present.  Last chance to
    // amend the framebuffer.  Do not call blocking APIs here.
    SAO_UI_HOOK_BEFORE_PRESENT    = 2,

    // Fires after Present returned successfully.  Post-frame bookkeeping
    // (release textures, flip buffers).  Not for drawing.
    SAO_UI_HOOK_AFTER_PRESENT     = 3,

    // Per-layer/panel hooks — fired around a specific layer's paint.
    // `filter` in `sao_ui_render_hook_register` identifies the target.
    SAO_UI_HOOK_BEFORE_LAYER_PAINT = 4,
    SAO_UI_HOOK_AFTER_LAYER_PAINT  = 5,

    // Per-widget hook — fired around a specific widget's paint.
    SAO_UI_HOOK_BEFORE_WIDGET_PAINT = 6,
    SAO_UI_HOOK_AFTER_WIDGET_PAINT  = 7,
};

// ─── Hook filter — identifies the target layer / panel / widget ──────
enum sao_ui_render_hook_filter_kind_e : int32_t {
    // Wildcard — fires for every layer / widget in per-target hooks.
    SAO_UI_HOOK_FILTER_ANY      = 0,
    // Match by named compositor layer.
    SAO_UI_HOOK_FILTER_LAYER    = 1,
    // Match by panel id.
    SAO_UI_HOOK_FILTER_PANEL_ID = 2,
    // Match by widget kind (all widgets of the given kind).
    SAO_UI_HOOK_FILTER_WIDGET_KIND = 3,
    // Match by specific widget handle.
    SAO_UI_HOOK_FILTER_WIDGET   = 4,
};

struct SaoUiRenderHookFilter {
    int32_t kind;                               // filter kind
    // Union payload — only one field is read per kind.
    sao_ui_layer_handle_t   layer;
    const char*             panel_id_utf8;
    int32_t                 widget_kind;        // sao_ui_widget_kind_ext_e
    sao_ui_widget_handle_t  widget;
    uint8_t                 _pad[4];
};

// ─── Hook payload passed to the callback ─────────────────────────────
struct SaoUiRenderHookPayload {
    // The paint context — direct2d device context / render target.
    // Nullable when the hook point is AFTER_PRESENT (no paint state).
    sao_ui_paint_ctx_handle_t paint_ctx;

    // Frame monotonic clock in microseconds.
    int64_t frame_time_us;

    // Frame index since compositor start (wraps at UINT32_MAX).
    uint32_t frame_index;

    // Delta from previous frame in microseconds (0 on the first frame).
    uint32_t frame_delta_us;

    // Screen rect the hook may draw into (paint context coordinates).
    int32_t viewport_x_px;
    int32_t viewport_y_px;
    int32_t viewport_width_px;
    int32_t viewport_height_px;

    // For per-layer / per-widget hooks: the target that triggered the
    // dispatch.  NULL when the hook point is frame-global.
    sao_ui_layer_handle_t  target_layer;
    sao_ui_panel_handle_t  target_panel;
    sao_ui_widget_handle_t target_widget;
};

// Hook callback.  Return non-zero to signal "handled, skip default"
// (only meaningful for BEFORE_* points — AFTER_* points always run
// through every hook regardless).
typedef sao_status_t (SAO_UI_CALL* sao_ui_render_hook_cb_t)(
    int32_t hook_point,                     // sao_ui_render_hook_point_e
    const SaoUiRenderHookPayload* payload,
    bool* out_skip_default,                 // set true to suppress host draw
    void* user_data);

// ─── Manager lifecycle ───────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_hook_manager_create(
    sao_ui_compositor_handle_t compositor,
    sao_ui_render_hook_manager_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_render_hook_manager_destroy(
    sao_ui_render_hook_manager_handle_t handle);

// ─── Registration ────────────────────────────────────────────────────
//
// Priority follows the same convention as engine/render_hook.h — higher
// priority fires first.  Callbacks with equal priority fire in
// registration order (stable).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_hook_register(
    sao_ui_render_hook_manager_handle_t handle,
    const char* plugin_id_utf8,
    int32_t hook_point,                     // sao_ui_render_hook_point_e
    const SaoUiRenderHookFilter* filter,    // NULL → ANY
    float priority,
    sao_ui_render_hook_cb_t callback,
    void* user_data,
    sao_ui_render_hook_token_t* out_token);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_hook_unregister(
    sao_ui_render_hook_manager_handle_t handle,
    sao_ui_render_hook_token_t token);

// Remove every hook registered by the given plugin — called during
// plugin unload so a crashed plugin can't leak hooks into the next
// frame.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_hook_unregister_plugin(
    sao_ui_render_hook_manager_handle_t handle,
    const char* plugin_id_utf8);

// ─── Overlay attach — declarative screen-space widget overlay ────────
//
// Convenience for the common "draw this widget on top of frame after
// compositor" pattern.  The manager keeps the widget alive, positions
// it at `x/y`, and paints via `sao_ui_widget_paint_at()` after the
// compositor pass.  Overlay handles can be moved / removed later.
struct SaoUiOverlayAttach {
    const char*             plugin_id_utf8;
    sao_ui_widget_handle_t  widget;
    int32_t                 x_px;
    int32_t                 y_px;
    int32_t                 z;              // higher = drawn later
    float                   opacity;
    bool                    absorb_input;   // true → widget receives mouse
    bool                    world_space;    // false → screen-space (default)
    uint8_t                 _pad[2];
};

typedef struct sao_ui_overlay_s* sao_ui_overlay_handle_t;

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_hook_attach_overlay(
    sao_ui_render_hook_manager_handle_t handle,
    const SaoUiOverlayAttach* attach,
    sao_ui_overlay_handle_t* out_overlay);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_hook_detach_overlay(
    sao_ui_render_hook_manager_handle_t handle,
    sao_ui_overlay_handle_t overlay);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_hook_move_overlay(
    sao_ui_render_hook_manager_handle_t handle,
    sao_ui_overlay_handle_t overlay,
    int32_t x_px, int32_t y_px);

// ─── Request redraw / diagnostics ────────────────────────────────────
//
// Nudge the compositor to run a frame.  Useful when a plugin state
// change wouldn't otherwise trigger a redraw (idle backoff).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_hook_request_redraw(
    sao_ui_render_hook_manager_handle_t handle);

struct SaoUiRenderHookStats {
    uint64_t total_calls;
    uint64_t total_failures;
    uint64_t slow_call_count;
    double   avg_us_per_call;
    double   max_us_per_call;
};

// Aggregated timing for the given plugin's hooks — used by the plugin
// manager to name-and-shame plugins that overrun the frame budget.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_hook_get_stats(
    sao_ui_render_hook_manager_handle_t handle,
    const char* plugin_id_utf8,
    SaoUiRenderHookStats* out_stats);

#ifdef __cplusplus
}  // extern "C"
#endif
