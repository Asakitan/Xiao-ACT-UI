// SAO Auto — Windows.Graphics.Capture-backed HWND grabber.
//
// Python authoritative source: `sao_auto/python/render/gpu_capture.py` (578 lines)
//
// The recognition path uses `PrintWindow` which synchronously asks
// the target window's WndProc to render to a DC.  For the game
// window that costs 30-100 ms per call and blocks the calling
// thread for the entire duration.  Worse, capture is wrapped in
// `capture_section()` (see `capture_sync.h`), so all overlay ULW
// commits wait for the capture lock.  This is why overlay panels
// can feel 1-2 FPS under load even though their tick loops fire
// at 30 Hz.
//
// Switching to Windows.Graphics.Capture (WGC, since Windows 10 1903)
// fixes both issues at once:
//
//   * WGC runs on a free-threaded DirectX frame pool.  The recognition
//     tick polls the latest queued frame — never blocks for capture.
//   * It captures the game window's swap-chain directly, giving a
//     real GPU blit, not a synchronous WndProc round-trip.
//   * The window doesn't need to be foreground or fully visible.
//
// If WGC fails to initialise (Windows < 1903, GPU driver issue,
// target HWND not capturable yet, env SAO_GPU_CAPTURE=0), this
// module returns SAO_STATUS_ERR_NOT_IMPLEMENTED and callers fall back
// to the legacy PrintWindow path.
//
// ── Client-inset compensation ────────────────────────────────
//   WGC hands back the full swap-chain surface.  Recognition
//   consumers usually only want the client area.  `client_inset`
//   returns the (off_x, off_y, client_w, client_h) tuple mapping
//   the client area onto the swap-chain image.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_gpu_capture_s* sao_ui_gpu_capture_handle_t;

struct SaoGpuCaptureConfig {
    // Target HWND (game window).
    void*       hwnd;

    // Preferred output format.  Default DXGI_FORMAT_B8G8R8A8_UNORM (87).
    uint32_t    format;

    // If true, disables cursor drawing in captured frames.  Default true
    // (recognition doesn't care about the cursor).
    bool        disable_cursor;

    // Max frame age (seconds) before `get_latest_bgr` reports stale.
    double      max_frame_age_sec;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_capture_create(
    const SaoGpuCaptureConfig* config,
    sao_ui_gpu_capture_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_gpu_capture_destroy(
    sao_ui_gpu_capture_handle_t handle);

// Ensure a capture session is active for the given HWND.  Returns
// SAO_STATUS_OK on success, SAO_STATUS_ERR_NOT_IMPLEMENTED if WGC is
// unavailable (caller falls back to PrintWindow).  Idempotent —
// safe to call every tick.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_capture_ensure_session(
    sao_ui_gpu_capture_handle_t handle);

// Frame data — BGR (3 bytes) OR BGRA (4 bytes), top-down.
struct SaoGpuCaptureFrame {
    const uint8_t* pixels;
    uint32_t       width;
    uint32_t       height;
    uint32_t       row_pitch;    // bytes
    uint32_t       channels;     // 3 = BGR, 4 = BGRA
    double         capture_time_sec;   // monotonic; 0 if no frame yet
};

// Non-blocking read of the latest frame.  If `max_age_sec` (from
// config) has elapsed, or no frame has arrived yet, returns
// SAO_STATUS_ERR_NOT_FOUND with `out_frame->pixels = nullptr`.
// On success, borrows read access to the internal frame buffer
// until the next call — do NOT hold across ticks.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_capture_get_latest(
    sao_ui_gpu_capture_handle_t handle,
    SaoGpuCaptureFrame* out_frame);

// Query the client-area inset within the captured surface.  Client
// coords: (off_x, off_y, client_w, client_h).  Recognition consumers
// use this to crop out title bar / border.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_capture_client_inset(
    sao_ui_gpu_capture_handle_t handle,
    int32_t* out_off_x, int32_t* out_off_y,
    int32_t* out_client_w, int32_t* out_client_h);

// Stop the session (releases the WGC framePool).  Safe to call
// multiple times.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_capture_stop(
    sao_ui_gpu_capture_handle_t handle);

// Capability probe — is WGC available on this Windows build?  Cheap;
// caches the answer.
SAO_UI_API bool SAO_UI_CALL sao_ui_gpu_capture_supported(void);
    
enum sao_ui_gpu_capture_state_e : int32_t {
    SAO_UI_GPU_CAPTURE_STOPPED = 0,
    SAO_UI_GPU_CAPTURE_RUNNING = 1,
    SAO_UI_GPU_CAPTURE_UNSUPPORTED = 2,
    SAO_UI_GPU_CAPTURE_FAILED = 3,
};
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_capture_get_state(
    sao_ui_gpu_capture_handle_t handle,
    int32_t* out_state);

// ── Stateless BGRA readback, premultiplication, and hashing ───────
//
// Pixel-plumbing pieces the Python authoritative source doesn't need
// (its recognition loop consumes numpy arrays directly) but that
// non-Python consumers require: BGRA readback from an arbitrary
// D3D11 texture, alpha-correcting premultiplication, and a stable
// hash of a BGRA payload for fixture-parity diffing.
//
// These helpers are STATELESS — they don't touch the capture-session
// singleton or the handle.  They operate on caller-owned pixel buffers
// and (optionally) caller-owned D3D11 texture pointers.

// BGRA readback from a caller-provided ID3D11Texture2D.  Used by the
// "get raw pixels for a specific texture" path — the caller has
// obtained the texture from WGC / DXGI / their own render pass and
// wants the current BGRA content mapped to CPU.
//
// Parameters:
//   texture         — ID3D11Texture2D* (opaque).  Must be a valid
//                     D3D11 texture created on THIS process's device.
//                     Format is expected to be a BGRA-compatible
//                     format (DXGI_FORMAT_B8G8R8A8_UNORM or _SRGB).
//                     Other formats return SAO_STATUS_ERR_INVALID_ARGUMENT.
//   width, height   — Explicit dimensions (caller-known, avoids an
//                     extra GetDesc round-trip).  Must match texture
//                     desc; validated where possible.
//   buf_out         — Destination BGRA buffer.  Must be able to hold
//                     height*stride bytes.
//   buf_capacity    — Bytes available in buf_out.
//   stride_out      — On success, set to the row pitch used (may
//                     exceed width*4 due to GPU pitch alignment).
//
// The internal staging texture is created on demand off the same
// device the source texture belongs to; retained for reuse across
// calls with the same (width, height) pair.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_capture_bgra_from_texture(
    void*     texture,
    uint32_t  width,
    uint32_t  height,
    uint8_t*  buf_out,
    uint32_t  buf_capacity,
    uint32_t* stride_out);

// Premultiply straight-alpha BGRA in place.  `pixels` is a BGRA byte
// stream of `size` bytes; size must be a multiple of 4.  If
// `alpha_correct` is true, uses the sRGB-space branch (linearises
// R/G/B, scales by α, re-encodes).  If false, uses the naive
// (byte-space) multiply matching PIL's default `image.convert("RGBa")`.
//
// The distinction matters for compositor parity: some renderers
// premultiply in linear light (WGC frames, DirectComposition
// callbacks) and some in gamma-encoded 8-bit (Python PIL,
// GDI+AlphaBlend).  Consumers that mix the two see hairline seams on
// alpha edges; pick a mode consistent with your target.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_capture_premultiply_bgra(
    uint8_t*  pixels,
    uint32_t  size,
    bool      alpha_correct);

// Stable 128-bit hash of a BGRA byte payload.  Used for fixture-parity
// diffing (compare captured pixels against a reference).  The hash is
// deterministic, byte-exact, and independent of endianness.  Fast
// enough to run per-frame on a 1080p BGRA payload (~2ms C++, ~5ms
// naive Python) without dominating recognition latency.
//
// The implementation is xxh3-inspired but hand-rolled to avoid a
// dependency on a third-party library — for parity purposes any
// deterministic mixing function suffices; the point is that
// (bytes → hash) is a stable relation across builds.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_gpu_capture_compute_hash(
    const uint8_t* pixels,
    uint32_t       size,
    uint64_t*      hash_hi_out,
    uint64_t*      hash_lo_out);

#ifdef __cplusplus
}  // extern "C"
#endif
