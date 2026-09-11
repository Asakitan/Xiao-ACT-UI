// SAO Auto - shared fisheye glass backdrop service.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_fisheye_backdrop_s* sao_ui_fisheye_backdrop_handle_t;

enum SaoUiFisheyeBackdropMode : int32_t {
    SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL = 0,
    SAO_UI_FISHEYE_BACKDROP_MODE_LIVE = 1,
};

struct SaoUiFisheyeBackdropRect {
    // Compositor host-local pixels. (0, 0) is the overlay host client
    // origin, not the virtual-desktop or monitor origin.
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct SaoUiFisheyeBackdropGeometry {
    SaoUiFisheyeBackdropRect rect;
    int32_t z_order;
};

struct SaoUiFisheyeBackdropState {
    bool visible;
    SaoUiFisheyeBackdropMode mode;
    SaoUiFisheyeBackdropGeometry geometry;
    bool layer_present;
    bool live_available;
    sao_status_t last_status;
    uint64_t frame_generation;
    // Actual live-path lifecycle, independent from the desired mode/visibility.
    // These fields are appended to preserve the existing field order.
    bool live_worker_running;
    bool live_resources_active;
};

// The compositor is borrowed and must outlive the service. Passing NULL creates
// a headless service that can still use the procedural pixel helper.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_create(
    sao_ui_compositor_handle_t compositor, sao_ui_fisheye_backdrop_handle_t* out_handle);

// Owner-thread retryable teardown. A failed call preserves the handle.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_try_destroy(sao_ui_fisheye_backdrop_handle_t handle);

// Compatibility wrapper. Owners should prefer try_destroy before clearing the
// handle so an owner-thread error remains observable.
SAO_UI_API void SAO_UI_CALL
sao_ui_fisheye_backdrop_destroy(sao_ui_fisheye_backdrop_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_set_mode(
    sao_ui_fisheye_backdrop_handle_t handle, SaoUiFisheyeBackdropMode mode);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_get_mode(
    sao_ui_fisheye_backdrop_handle_t handle, SaoUiFisheyeBackdropMode* out_mode);

// Publish the target foreground surface. rect is compositor host-local;
// repeated calls reuse the existing layer and only update geometry/z-order
// during the next owner-thread tick.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_show(
    sao_ui_fisheye_backdrop_handle_t handle, const SaoUiFisheyeBackdropRect* rect, int32_t z_order);

// Hiding wakes the live worker immediately. Owner-thread ticks reap the worker
// and fade procedural content out over 400ms while preserving one-layer reuse.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_hide(sao_ui_fisheye_backdrop_handle_t handle);

// Apply mode, geometry, visibility, and worker reports on the owner thread.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_tick(sao_ui_fisheye_backdrop_handle_t handle);

// Deterministic owner-thread step, 0..1000ms. Tick/service derive the same step
// from steady_clock. Procedural show fades in over 500ms (instant in reduced motion).
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_advance(sao_ui_fisheye_backdrop_handle_t handle, uint32_t delta_ms);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_service(sao_ui_fisheye_backdrop_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_get_state(
    sao_ui_fisheye_backdrop_handle_t handle, SaoUiFisheyeBackdropState* out_state);

// Generate a deterministic premultiplied BGRA SAO glass frame. Passing NULL
// with capacity 0 queries the required byte count. Row padding is zero-filled.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_render_procedural_bgra(
    uint32_t width, uint32_t height, uint32_t stride, uint8_t* out_bgra, size_t capacity,
    size_t* out_required_bytes);

#ifdef __cplusplus
} // extern "C"
#endif
