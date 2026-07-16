// SAO Auto — DXGI Desktop Duplication (WDA-aware capture).
//
// Python authoritative source: `sao_auto/python/render/dxgi_duplication.py` (580 lines)
//
// ImageGrab/mss use GDI BitBlt: on windows excluded via
// SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) — including our
// compositor host — the whole area comes back black.  The fisheye
// panel would then see an all-black background over the game.
// IDXGIOutputDuplication goes through the DWM composition path
// which actually SKIPS excluded windows, letting the desktop/game
// below show through — this is the design semantic of
// WDA_EXCLUDEFROMCAPTURE.
//
// ── Setup chain ──────────────────────────────────────────────
//   D3D11CreateDevice → QI IDXGIDevice → GetAdapter → EnumOutputs(idx)
//   → QI IDXGIOutput1 → DuplicateOutput → IDXGIOutputDuplication
//
// ── Per-frame ────────────────────────────────────────────────
//   AcquireNextFrame(timeout) → QI ID3D11Texture2D
//   → CopyResource(staging, desktop) → Map(staging, READ) → memcpy
//   → Unmap → ReleaseFrame → BGRA→RGB tight-pack
//
// ── Access-lost recovery ────────────────────────────────────
//   DXGI_ERROR_ACCESS_LOST fires on secure-desktop transition (Ctrl-
//   Alt-Del, UAC prompt), monitor topology change, or fullscreen
//   D3D swap-effect flip.  Consumer MUST re-duplicate: release the
//   IDXGIOutputDuplication and call DuplicateOutput again.  The C++
//   port handles this internally; caller just gets an SAO_STATUS_OK
//   frame after a brief blank period.
//
// ── Device-lost handling ────────────────────────────────────
//   Same DEVICE_REMOVED / _HUNG / _RESET set as dcomp_bridge.  On
//   detect, module returns SAO_STATUS_ERR_DEVICE_LOST from
//   `acquire_frame`; caller destroys + recreates.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_dxgi_dup_s* sao_ui_dxgi_dup_handle_t;

struct SaoDxgiDupConfig {
    // Output index (monitor).  0 = primary; use EnumOutputs to find
    // a specific monitor.
    uint32_t    output_index;

    // Adapter index (multi-GPU).  0 = default.
    uint32_t    adapter_index;

    // Preallocated staging texture size (avoid resize on same-size
    // frames).  Set to expected fullscreen size to save one alloc.
    // 0 → allocate on first frame.
    uint32_t    staging_width;
    uint32_t    staging_height;

    // AcquireNextFrame timeout (ms).  Default 16 (~1 frame at 60 Hz).
    // 0 → non-blocking (poll).
    uint32_t    acquire_timeout_ms;

    // Auto-recover on ACCESS_LOST.  True by default.
    bool        auto_recover;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_create(
    const SaoDxgiDupConfig* config,
    sao_ui_dxgi_dup_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_dxgi_dup_destroy(
    sao_ui_dxgi_dup_handle_t handle);

// Frame data — BGRA rows, top-down.
struct SaoDxgiDupFrame {
    const uint8_t* bgra_bytes;
    uint32_t       width;
    uint32_t       height;
    uint32_t       row_pitch;    // bytes; may be > width*4 due to alignment
    int32_t        origin_x;     // monitor origin in virtual-desktop coords
    int32_t        origin_y;
    uint32_t       rotation;     // DXGI_MODE_ROTATION (0-4)
    uint64_t       last_present_time;
    uint32_t       accumulated_frames;
    bool           protected_content_masked_out;
};

// Acquire the latest frame.  Blocks up to `acquire_timeout_ms` (from
// config) waiting for a new frame.  Fills out_frame with BORROWED
// pointers valid until `release_frame` is called.  Returns:
//   SAO_STATUS_OK                — new frame
//   SAO_STATUS_ERR_TIMEOUT      — no new frame in window
//   SAO_STATUS_ERR_DEVICE_LOST  — device removed, caller must recreate
//   SAO_STATUS_ERR_SURFACE_INVALID — access lost; if auto_recover,
//                                     handled internally, caller retries
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_acquire_frame(
    sao_ui_dxgi_dup_handle_t handle,
    SaoDxgiDupFrame* out_frame);

// Release the frame.  MUST be called after every successful acquire.
// Unmaps the staging texture and releases the DXGI frame lease.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_release_frame(
    sao_ui_dxgi_dup_handle_t handle);

// Query duplication descriptor (monitor mode, rotation, DesktopImage-
// InSystemMemory flag).
struct SaoDxgiDupDesc {
    uint32_t    width;
    uint32_t    height;
    uint32_t    refresh_rate_num;
    uint32_t    refresh_rate_den;
    uint32_t    format;                    // DXGI_FORMAT
    uint32_t    rotation;
    bool        desktop_image_in_system_memory;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_get_desc(
    sao_ui_dxgi_dup_handle_t handle,
    SaoDxgiDupDesc* out_desc);

// Force re-initialization (test aid).  Same effect as ACCESS_LOST
// recovery.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_reinit(
    sao_ui_dxgi_dup_handle_t handle);

// ── Wave 7 additions (Phase 6 overlay live parity depth) ────────
//
// Dirty-rect / move-rect / cursor / staging-copy APIs.  These are the
// "full pipeline" pieces the Python authoritative source doesn't
// exercise (fisheye only wants tight-packed RGB) but the C ABI must
// expose because non-fisheye consumers (overlay differential redraw,
// screencap parity oracles) rely on them.
//
// All Wave 7 APIs require an actively-held frame (i.e. between a
// successful `acquire_frame` and its `release_frame`).  Calling
// against no held frame returns SAO_STATUS_ERR_NOT_INITIALIZED.  This
// matches IDXGIOutputDuplication semantics — DXGI itself validates
// that GetFrame* is only legal while a frame lease is live.

// A rectangle in monitor pixel coordinates.  Matches DXGI's RECT +/-
// origin (0,0) at the top-left of the duplication surface.
struct SaoDxgiDupRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

// Dirty rectangles for the currently-held frame.  Rects_out may be
// NULL to query the count only; capacity is the number of rects the
// caller can accept.  If capacity < required, returns
// SAO_STATUS_ERR_BUFFER_TOO_SMALL and *count_out is set to the actual
// required count.  Backed by IDXGIOutputDuplication::GetFrameDirty-
// Rects().
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_get_dirty_rects(
    sao_ui_dxgi_dup_handle_t handle,
    SaoDxgiDupRect* rects_out,
    uint32_t         capacity,
    uint32_t*        count_out);

// Frame-move pairs — the DXGI compositor's "this rect just slid from
// there to here" hint.  Same capacity semantics as dirty rects.  Every
// move descriptor carries both the source point (before the slide)
// and the destination rect (after the slide) so a differential-blit
// consumer can `CopyResource` a strip rather than re-sample the whole
// surface.  Backed by IDXGIOutputDuplication::GetFrameMoveRects().
struct SaoDxgiDupMoveRect {
    int32_t         src_x;
    int32_t         src_y;
    SaoDxgiDupRect  dst;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_get_move_rects(
    sao_ui_dxgi_dup_handle_t handle,
    SaoDxgiDupMoveRect* rects_out,
    uint32_t             capacity,
    uint32_t*            count_out);

// Cursor position + latest shape metadata for the held frame.  Shape
// pixels are copied into `shape_out` (BGRA row-major, size limited by
// `shape_capacity` bytes).  If the frame has no cursor update at all
// (info.PointerShapeBufferSize == 0) then `shape_bytes_out` is 0 and
// the shape struct fields describe the LAST known shape (per DXGI's
// "shape only updates when it changes" semantics).  Position always
// reflects the current frame.
struct SaoDxgiDupCursorInfo {
    // Absolute monitor coordinates of the cursor's hotspot.
    int32_t   position_x;
    int32_t   position_y;
    // True if the cursor is currently visible.  Mirrors
    // DXGI_OUTDUPL_POINTER_POSITION.Visible.
    bool      visible;
    // Shape metadata (DXGI_OUTDUPL_POINTER_SHAPE_INFO).
    uint32_t  shape_type;          // 1=MONOCHROME, 2=COLOR, 4=MASKED_COLOR
    uint32_t  shape_width;
    uint32_t  shape_height;
    uint32_t  shape_pitch;         // bytes per row
    int32_t   shape_hotspot_x;
    int32_t   shape_hotspot_y;
    // Non-zero if this frame's cursor position updated at all.
    bool      position_updated;
    // Non-zero if this frame's cursor shape updated (shape_out was
    // populated during this call).  When false the shape metadata may
    // still be non-zero from a previous update; the caller can reuse
    // the last shape it received.
    bool      shape_updated;
};

// `shape_out` may be NULL when the caller only wants position; in that
// case `shape_capacity` must be zero.  On success, `*shape_bytes_out`
// is set to the number of bytes actually written (0 if the frame had
// no shape update).  BUFFER_TOO_SMALL if `shape_capacity` is smaller
// than the shape payload; the caller should re-issue with a larger
// buffer.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_get_cursor_info(
    sao_ui_dxgi_dup_handle_t handle,
    SaoDxgiDupCursorInfo*    info_out,
    uint8_t*                 shape_out,
    uint32_t                 shape_capacity,
    uint32_t*                shape_bytes_out);

// Copy the currently-held desktop texture into a caller-provided
// staging buffer and map the internal staging texture for READ.  The
// bytes returned are BGRA rows padded to `stride_out` bytes per row
// (matches D3D11_MAPPED_SUBRESOURCE.RowPitch).  On success the caller
// owns the returned bytes; the internal staging texture is Unmapped
// before this call returns so subsequent acquires are unaffected.
//
// This is the "gimme raw pixels" convenience that mirrors the Python
// _bgra_pitched_to_rgb_bytes path but WITHOUT the BGRA→RGB conversion
// (that's a separate concern; recognition consumers want BGRA).
//
// Returns:
//   SAO_STATUS_OK               — copy succeeded, out params valid
//   SAO_STATUS_ERR_NOT_INITIALIZED — no frame currently held
//   SAO_STATUS_ERR_BUFFER_TOO_SMALL — `bytes_capacity` < height*stride
//   SAO_STATUS_ERR_OS_CALL_FAILED — CopyResource/Map failure
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_copy_to_staging(
    sao_ui_dxgi_dup_handle_t handle,
    uint8_t*                 bytes_out,
    uint32_t                 bytes_capacity,
    uint32_t*                stride_out,
    uint32_t*                bytes_written_out);

struct SaoDxgiDupState {
    uint32_t output_index;
    uint32_t adapter_index;
    uint32_t acquire_timeout_ms;
    uint32_t staging_width;
    uint32_t staging_height;
    bool auto_recover;
    bool alive;
    bool frame_held;
    bool cursor_shape_cached;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_get_state(
    sao_ui_dxgi_dup_handle_t handle,
    SaoDxgiDupState* out_state);

#ifdef __cplusplus
}  // extern "C"
#endif
