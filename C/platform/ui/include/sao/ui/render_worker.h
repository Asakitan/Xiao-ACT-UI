// SAO Auto — off-thread frame composition (ULW / DComp).
//
// Python authoritative source: `sao_auto/python/render/overlay_render_worker.py` (758 lines)
//
// The main-thread scheduler calls tick → advance (animate) → render.
// Previously `render` did compose_frame (PIL/numpy heavy, ~3-8 ms) +
// ulw_update (premultiply + Win32 commit, ~1-2 ms) all on the Tk
// thread, which blocked the event loop and caused visible tearing/jank.
//
// This module provides `AsyncFrameWorker`-equivalent handles backed
// by shared fixed render lanes.  Each overlay worker is pinned to
// one background thread, so thread-affine resources (standalone GL
// contexts) remain stable while multiple overlays render in parallel
// across CPU cores.
//
// ── Architecture ─────────────────────────────────────────────
//   Main thread                    Render lanes / CPU task pool
//   ──────────                     ────────────────────────────
//   advance(now)
//   submit_compose(fn, now)  → pinned lane: fn(now) → premultiply
//                              → store result
//   ⋮ (returns immediately)
//   if result ready:
//     ulw_commit(hwnd, result) ← (GDI only, <0.3 ms)
//
// PIL and NumPy release the GIL in their C routines, so background
// jobs genuinely run on other cores.  In C++, use `std::async` or
// `WorkerPool` — the C++ equivalents don't need GIL relief but the
// same lane-affinity constraint applies for standalone GL contexts.
//
// ── Lane count heuristic ─────────────────────────────────────
//   cpu_total <= 2  → 1 lane
//   cpu_total <= 4  → 2 lanes
//   cpu_total <= 6  → 3 lanes
//   cpu_total <= 8  → 4 lanes
//   cpu_total >= 12 → min(6, cpu_total - 2) lanes
//   High-core (>= 12c) systems get up to 6 lanes so 5-6 panels + menu
//   compose in parallel instead of serializing on 4 lanes.
//
// ── Capture sync ─────────────────────────────────────────────
//   Wraps `wait_until_capture_idle` (see `capture_sync.h`).  Frame
//   commits deferred until capture section (PrintWindow inside
//   recognition) is idle.
//
// ── Premultiply pipeline ────────────────────────────────────
//   RGBA (from PIL / D2D) → premultiplied BGRA (for UpdateLayered-
//   Window).  Cython-accelerated in Python (_sao_cy_pixels); C++
//   port uses direct SIMD kernel via `abi/pixel_kernels.h`.
//   Cache: if input image sets `_sao_premult_safe` + matching
//   `_sao_content_version`, the previous BGRA output is reused.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_render_worker_s* sao_ui_render_worker_handle_t;
typedef struct sao_ui_render_lane_s* sao_ui_render_lane_handle_t;
typedef struct sao_ui_frame_buffer_s* sao_ui_frame_buffer_handle_t;

#ifndef SAO_UI_STATUS_ERR_BUSY
#define SAO_UI_STATUS_ERR_BUSY ((sao_status_t) - 102)
#endif

struct SaoRenderWorkerConfig {
    // 0 → auto per the heuristic above.  Non-zero override.
    int32_t     lane_count;

    // CPU task pool size (for background compose work not needing a
    // render lane).  0 → auto.
    int32_t     task_pool_size;

    // Frame-buffer allocator hint: max concurrent frame buffers.
    // Sizing: (lane_count + task_pool_size) * 2.
    int32_t     frame_buffer_pool_size;

    // If true, dropped frames (submitted while a lane is busy) queue
    // as pending; if false, they're discarded.  Default true.
    bool        queue_pending;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_worker_create(
    const SaoRenderWorkerConfig* config,
    sao_ui_render_worker_handle_t* out_handle);

// Retires the public handle before draining accepted API operations and jobs.
// A callback running on this worker receives SAO_UI_STATUS_ERR_BUSY rather
// than waiting for or joining its own thread.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_worker_destroy(
    sao_ui_render_worker_handle_t handle);

typedef void (SAO_UI_CALL* sao_ui_render_worker_task_fn_t)(void* user_data);

// Submits non-thread-affine CPU work to the shared fan-out pool.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_worker_submit(
    sao_ui_render_worker_handle_t handle,
    sao_ui_render_worker_task_fn_t task_fn,
    void* user_data);

// Waits until every fan-out and lane job accepted before quiescence has
// completed.  A callback running on this worker returns BUSY.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_worker_flush(
    sao_ui_render_worker_handle_t handle);

// Get / create a lane for a specific overlay id.  Lanes are pinned
// to specific background threads so thread-affine resources stay
// stable.  The lane is owned by the worker; do not destroy separately.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_worker_get_lane(
    sao_ui_render_worker_handle_t handle,
    const char* overlay_id_utf8,
    sao_ui_render_lane_handle_t* out_lane);

// Submit a compose job to a lane.  fn runs on the lane's background
// thread with `now_sec` as the timestamp.  On completion, the frame
// buffer is stored on the lane and can be picked up by `try_take_frame`.
// If the lane is busy with an earlier job, this returns
// SAO_STATUS_ERR_ALREADY_EXISTS unless `queue_pending` was set at
// construction.
typedef sao_ui_frame_buffer_handle_t (SAO_UI_CALL* sao_ui_compose_fn_t)(
    double now_sec, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_lane_submit_compose(
    sao_ui_render_lane_handle_t lane,
    sao_ui_compose_fn_t fn,
    void* user_data,
    double now_sec);

// Non-blocking check for a completed frame.  On success, ownership
// of the frame buffer transfers to the caller (must call
// `sao_ui_frame_buffer_release` after use).  Returns SAO_STATUS_ERR_
// NOT_FOUND if no completed frame is ready.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_lane_try_take_frame(
    sao_ui_render_lane_handle_t lane,
    sao_ui_frame_buffer_handle_t* out_frame);

// Frame-buffer inspection.
struct SaoFrameBufferView {
    const uint8_t* bgra_bytes;   // premultiplied BGRA, stride = w*4
    uint32_t       width;
    uint32_t       height;
    int32_t        x;            // target screen coord
    int32_t        y;
};

// Copies one tightly packed premultiplied BGRA frame into an owned buffer.
// bgra_size must equal width * height * 4 exactly.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_frame_buffer_create_bgra(
    const uint8_t* bgra_bytes,
    size_t bgra_size,
    uint32_t width,
    uint32_t height,
    int32_t x,
    int32_t y,
    sao_ui_frame_buffer_handle_t* out_frame);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_frame_buffer_view(
    sao_ui_frame_buffer_handle_t handle,
    SaoFrameBufferView* out_view);

SAO_UI_API void SAO_UI_CALL sao_ui_frame_buffer_release(
    sao_ui_frame_buffer_handle_t handle);

// UpdateLayeredWindow commit path.  Wraps the entire GDI commit
// sequence (CreateCompatibleDC, CreateDIBSection, SelectObject,
// UpdateLayeredWindowIndirect).  Fails on device-lost equivalents.
// Called from the main thread AFTER the lane produced a frame.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_worker_ulw_commit(
    void* hwnd,
    sao_ui_frame_buffer_handle_t frame);

// Recent peak worker wall time in ms for the given lane over a
// window.  Used by the scheduler pressure floor.  Returns 0.0 when
// no samples in window.
SAO_UI_API double SAO_UI_CALL sao_ui_render_worker_peak_wall_ms(
    sao_ui_render_worker_handle_t handle,
    double window_sec);

// Standalone premultiply helper — RGBA bytes → premultiplied BGRA
// bytes.  For compat use in isolation from a lane (test tools).
// Output is heap-allocated; caller frees with `sao_core_free`.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_worker_premultiply_rgba_to_bgra(
    const uint8_t* rgba_in, uint32_t width, uint32_t height,
    uint8_t** out_bgra, size_t* out_size);

#ifdef __cplusplus
}  // extern "C"
#endif
