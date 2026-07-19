// SAO Auto — legacy overlay adapter (drop-in GpuOverlayWindow replacement).
//
// Python authoritative source: `sao_auto/python/render/overlay_adapter.py` (317 lines)
//
// `CompositorOverlayWindow` has the same public API as
// `GpuOverlayWindow` so existing code can switch with minimal changes.
// This header wraps a compositor layer with the older-shaped API for
// migration; new code SHOULD use `compositor.h` directly.
//
// ── Migration example ────────────────────────────────────────
//   OLD:   pump = get_glfw_pump(root);
//          win  = GpuOverlayWindow(pump, w=300, h=400, ...)
//
//   NEW:   overlay = get_unified_overlay(root);
//          win     = CompositorOverlayWindow(overlay, w=300, h=400, ...)
//
//   Both expose:  show(), hide(), destroy(), set_geometry(),
//                 set_render_fn(), request_redraw(), set_click_through(),
//                 set_input_callbacks(), hwnd, ctx.
//
// ── BgraPresenter compat ────────────────────────────────────
//   `CompositorBgraPresenter` is a BgraPresenter-compatible wrapper
//   that feeds a CompositorLayer.  Drop-in replacement — instead of
//   owning a GL texture and shader program, this simply forwards
//   BGRA bytes to the underlying layer.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"
#include "sao/ui/gpu_overlay_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_compositor_overlay_window_s* sao_ui_compositor_overlay_window_handle_t;
typedef struct sao_ui_compositor_bgra_presenter_s* sao_ui_compositor_bgra_presenter_handle_t;

typedef enum SaoUiCompositorOverlayBackingState {
    SAO_UI_COMPOSITOR_OVERLAY_BACKING_FIXTURE = 0,
    SAO_UI_COMPOSITOR_OVERLAY_BACKING_LAYER = 1,
    SAO_UI_COMPOSITOR_OVERLAY_BACKING_DESTROYED = 2,
} SaoUiCompositorOverlayBackingState;

// ── CompositorOverlayWindow (drop-in for GpuOverlayWindow) ───

// Same config as `SaoGpuOverlayWindowConfig` — deliberately alias-able.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_create(
    sao_ui_compositor_handle_t compositor, const SaoGpuOverlayWindowConfig* config,
    sao_ui_compositor_overlay_window_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL
sao_ui_compositor_overlay_window_destroy(sao_ui_compositor_overlay_window_handle_t handle);

// Borrowed layer handle for production-backed windows; NULL for explicit
// compatibility fixtures created with a NULL compositor.
SAO_UI_API sao_ui_layer_handle_t SAO_UI_CALL
sao_ui_compositor_overlay_window_layer(sao_ui_compositor_overlay_window_handle_t handle);

SAO_UI_API SaoUiCompositorOverlayBackingState SAO_UI_CALL
sao_ui_compositor_overlay_window_backing_state(sao_ui_compositor_overlay_window_handle_t handle);

// All GpuOverlayWindow methods delegated:
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_compositor_overlay_window_show(sao_ui_compositor_overlay_window_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_compositor_overlay_window_hide(sao_ui_compositor_overlay_window_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_compositor_overlay_window_set_geometry(sao_ui_compositor_overlay_window_handle_t handle,
                                              int32_t x, int32_t y, int32_t width, int32_t height);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_move(
    sao_ui_compositor_overlay_window_handle_t handle, int32_t x, int32_t y);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_click_through(
    sao_ui_compositor_overlay_window_handle_t handle, bool click_through);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_input_callbacks(
    sao_ui_compositor_overlay_window_handle_t handle, sao_ui_layer_cursor_pos_fn_t cursor_pos_fn,
    sao_ui_layer_cursor_leave_fn_t cursor_leave_fn, sao_ui_layer_button_fn_t button_fn,
    sao_ui_layer_scroll_fn_t scroll_fn, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_alpha(
    sao_ui_compositor_overlay_window_handle_t handle, float alpha);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_enable_input_proxy(
    sao_ui_compositor_overlay_window_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_compositor_overlay_window_raise_to_top(sao_ui_compositor_overlay_window_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_compositor_overlay_window_set_z(sao_ui_compositor_overlay_window_handle_t handle, int32_t z);

// ── CompositorBgraPresenter (drop-in for BgraPresenter) ──────

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_create(
    sao_ui_layer_handle_t layer, sao_ui_compositor_bgra_presenter_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL
sao_ui_compositor_bgra_presenter_destroy(sao_ui_compositor_bgra_presenter_handle_t handle);

// Stage a new BGRA frame + optional position update.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_set_frame(
    sao_ui_compositor_bgra_presenter_handle_t handle, const uint8_t* bgra, uint32_t width,
    uint32_t height, int32_t x, int32_t y);

// Animate alpha towards a target.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_start_fade(
    sao_ui_compositor_bgra_presenter_handle_t handle, float target_alpha, float duration_sec);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_set_alpha(
    sao_ui_compositor_bgra_presenter_handle_t handle, float alpha);

SAO_UI_API float SAO_UI_CALL
sao_ui_compositor_bgra_presenter_get_alpha(sao_ui_compositor_bgra_presenter_handle_t handle);

// ── Factory (matches Python create_overlay_window) ────────

// One-line create — chooses unified compositor (only mode in C++).
// Kept for source-parity with plugin code that imports the factory.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_create_overlay_window(
    sao_ui_compositor_handle_t compositor, const SaoGpuOverlayWindowConfig* config,
    sao_ui_compositor_overlay_window_handle_t* out_handle);

#ifdef __cplusplus
} // extern "C"
#endif
