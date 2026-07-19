// SAO Auto — Compositor-only GPU overlay window (compat shim).
//
// Python authoritative source: `sao_auto/python/render/gpu_overlay_window.py` (748 lines)
//
// GpuOverlayWindow (Python) is a legacy adapter kept so existing
// callers can transition to the unified DWM compositor without
// touching every call site.  In the C++ port, this header exposes
// the SAME public API as the Python class, but every method
// delegates to `compositor.h` under the hood — matches
// `overlay_adapter.py::CompositorOverlayWindow`.
//
// ── Public API surface (matches Python) ─────────────────────
//   glfw_supported() → bool
//   get_glfw_pump(root)   [dropped in C++ port — no Tk pump]
//   GpuOverlayWindow(pump, w, h, x, y, render_fn=None, click_through=True)
//     .show() / .hide() / .destroy()
//     .set_geometry(x, y, w, h)
//     .set_render_fn(fn)
//     .request_redraw()
//     .set_click_through(bool)
//     .set_input_callbacks(...)
//     .set_alpha(float)
//     .start_fade(target, duration, done_fn)
//     .raise_to_top()
//     .set_z(int)
//     .ctx      → GL context handle
//     .hwnd     → HWND handle
//
// ── Legacy behaviour vs unified ─────────────────────────────
//   Python has a `_USE_UNIFIED` toggle (default true).  The C++ port
//   is unified-only.  The name is preserved for source-level
//   compatibility with plugin loaders.
//
// ── WGL context serialize lock ──────────────────────────────
//   Every wglCreateContext / wglMakeCurrent across the whole process
//   must go through the process-wide serialize lock exposed by
//   `get_wgl_serialize_lock`.  Without this, two threads racing
//   wglCreateContext on the same GPU driver corrupt each other's
//   current-context state and surface as "WGL: Failed to clear
//   current context: handle invalid" errors.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_gpu_overlay_window_s* sao_ui_gpu_overlay_window_handle_t;

struct SaoGpuOverlayWindowConfig {
    // Position + size on the host.  Coordinates are host-window-local.
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;

    // Whether the layer receives mouse events.  false → the compositor's
    // click-through RGN skips this layer, mouse passes through.
    bool click_through;

    // Title (mainly for debug diagnostics).  UTF-8, NULL → empty.
    const char* title_utf8;

    // Vsync — if true, target_fps is set to the display refresh rate.
    bool vsync;

    // Z-order.  Default 100.  See `compositor.h::sao_ui_layer_set_z_order`.
    int32_t z_order;

    // Optional render callback.  NULL → BGRA upload path only.
    void* render_fn; // sao_ui_layer_render_fn_t*
    void* render_fn_user_data;
};

// Query legacy WGL availability.  The D3D/DComp production build returns false.
SAO_UI_API bool SAO_UI_CALL sao_ui_gpu_overlay_supported(void);

// Get the process-wide WGL serialize lock handle.  Every
// wglCreateContext / wglMakeCurrent MUST wrap its call in
// `lock_acquire`/`lock_release`.  Never manually lock this;
// use the RAII wrapper.  Handle is process-lifetime.
SAO_UI_API void* SAO_UI_CALL sao_ui_get_wgl_serialize_lock(void);
SAO_UI_API void SAO_UI_CALL sao_ui_wgl_serialize_lock_acquire(void* lock_handle);
SAO_UI_API void SAO_UI_CALL sao_ui_wgl_serialize_lock_release(void* lock_handle);

// Create a GPU overlay window (delegate to compositor). The returned opaque
// handle may be called from any thread. Mutators/getters are serialized with
// destroy and either complete before destroy returns or reject the retired
// handle with SAO_STATUS_ERR_HANDLE_INVALID.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_create(
    sao_ui_compositor_handle_t compositor, const SaoGpuOverlayWindowConfig* config,
    sao_ui_gpu_overlay_window_handle_t* out_handle);

// Removes the handle from the active registry and waits for in-flight render
// callbacks and API calls before returning. If called by this window's render
// callback, teardown is deferred until that callback returns so the callback
// never waits for itself. Repeated/stale-handle destroy calls are safe no-ops.
SAO_UI_API void SAO_UI_CALL
sao_ui_gpu_overlay_window_destroy(sao_ui_gpu_overlay_window_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_show(sao_ui_gpu_overlay_window_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_hide(sao_ui_gpu_overlay_window_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_set_geometry(
    sao_ui_gpu_overlay_window_handle_t handle, int32_t x, int32_t y, int32_t width, int32_t height);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_move(sao_ui_gpu_overlay_window_handle_t handle, int32_t x, int32_t y);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_set_click_through(
    sao_ui_gpu_overlay_window_handle_t handle, bool click_through);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_set_alpha(sao_ui_gpu_overlay_window_handle_t handle, float alpha);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_raise_to_top(sao_ui_gpu_overlay_window_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_set_z(sao_ui_gpu_overlay_window_handle_t handle, int32_t z);

// Get underlying compositor layer (delegate target).
SAO_UI_API sao_ui_layer_handle_t SAO_UI_CALL
sao_ui_gpu_overlay_window_layer(sao_ui_gpu_overlay_window_handle_t handle);

// Get the compositor host's HWND (for D3D interop).
SAO_UI_API void* SAO_UI_CALL
sao_ui_gpu_overlay_window_hwnd(sao_ui_gpu_overlay_window_handle_t handle);

// Get the legacy GL context.  Returns NULL in the D3D/DComp production build.
SAO_UI_API void* SAO_UI_CALL
sao_ui_gpu_overlay_window_gl_ctx(sao_ui_gpu_overlay_window_handle_t handle);

struct SaoGpuOverlayWindowState {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t z_order;
    float alpha;
    bool visible;
    bool click_through;
    bool destroyed;
    uint8_t _pad;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_get_state(
    sao_ui_gpu_overlay_window_handle_t handle, SaoGpuOverlayWindowState* out_state);

// Arm diagnostic trace markers for the next 8 actual compositor render ticks
// observed by a visible GPU overlay layer. Calls that do not execute a render
// tick do not consume the counter. Re-arming restarts the count at 8.
SAO_UI_API void SAO_UI_CALL sao_ui_arm_pump_trace(void);

#ifdef __cplusplus
} // extern "C"
#endif
