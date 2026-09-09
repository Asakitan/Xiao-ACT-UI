// SAO Auto — DirectComposition + DXGI presentation bridge.
//
// Python authoritative source: `sao_auto/python/render/dcomp_bridge.py` (1268 lines)
//
// Replaces WGL SwapBuffers for overlay anti-capture: GL rendering
// stays on the existing WGL context, pixel data is read via glReadPixels
// and uploaded to a DXGI swap chain bound to a DirectComposition visual.
// DWM manages DirectComposition content so `SetWindowDisplayAffinity`
// (WDA_EXCLUDEFROMCAPTURE) actually excludes the overlay from
// screen capture — SwapBuffers alone does not.
//
// ── Setup chain ──────────────────────────────────────────────────
//   D3D11CreateDevice
//     → QI IDXGIDevice
//     → GetAdapter
//     → GetParent(IDXGIFactory2)
//     → CreateSwapChainForComposition
//   DCompositionCreateDevice
//     → CreateTargetForHwnd(host_hwnd)
//     → CreateVisual
//     → SetContent(swapchain)
//     → SetRoot(visual)
//     → Commit
//
// ── Per-frame ────────────────────────────────────────────────────
//   glReadPixels(BGRA from GL FBO)
//     → Map staging texture (D3D11_MAP_WRITE)
//     → memcpy + row flip
//     → Unmap
//     → CopyResource(backbuf, staging)
//     → Present(0, 0)
//     → Commit
//
// ── Device-loss handling (§9 of handoff) ────────────────────────
//   `wglDXLockObjectsNV` has NO timeout parameter; against a
//   removed device the driver-defined behaviour is a permanent CPU+
//   GPU spike, not an error return.  EVERY acquire must be preceded
//   by `ID3D11Device::GetDeviceRemovedReason()` (constant time, non-
//   blocking).  When device_removed detected: `_teardown_gl_interop()`
//   + `alive = false`; subsequent frames short-circuit to plain
//   swap_buffers().  There is NO device-recovery; a bounce recreates
//   the entire compositor.  DXGI_STATUS_OCCLUDED (positive success)
//   is NOT device-lost, it means "monitor off, continue as normal".
//
// ── DXGI shared-texture keyed-mutex contract ────────────────────
//   Producer and consumer both AcquireSync/ReleaseSync key 0.  Consumer
//   timeout is 8 ms.  A successful acquire is always paired with
//   ReleaseSync(0), including error/exception paths.  WAIT_TIMEOUT keeps the
//   last complete consumer frame and never falls through to an unlocked copy.
//   Plain shared textures without IDXGIKeyedMutex remain supported.
//
// ── COM lifetime on init failure (§10 of handoff) ───────────────
//   `_init()` chain failures previously leaked any COM objects
//   already acquired because `destroy()` gated cleanup on `_alive`
//   which was still false.  The C++ port RAII-wraps every COM object
//   with `sao_com_ptr`; init failures release everything before
//   returning.  Do not weaken this invariant.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/overlay_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_dcomp_bridge_s* sao_ui_dcomp_bridge_handle_t;

#define SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_KEY 0ull
#define SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_TIMEOUT_MS 8u

// Bridge creation config.
struct SaoDcompBridgeConfig {
    // The host HWND — the DComp target hooks here.  Must be hRender
    // (not hControl decoy) — attaching to hControl would produce a
    // 1x1 visual tree.
    void*       hwnd;

    // Shared D3D11 device (may be null → bridge creates its own with
    // `D3D11CreateDevice` at BGRA-support + hardware driver type).
    // If provided, must have BGRA support flag set (0x20).
    void*       d3d11_device;    // ID3D11Device*

    // Alpha mode — DXGI_ALPHA_MODE_PREMULTIPLIED (1) is what DWM
    // expects for correct DComp blending.  Straight (2) is available
    // for special cases but not routed here.
    uint32_t    alpha_mode;

    // Buffer count — 2 (typical FLIP_SEQUENTIAL) unless a driver
    // workaround demands 3.
    uint32_t    buffer_count;

    // Initial size.  Bridge auto-resizes on `resize()` calls.
    uint32_t    width;
    uint32_t    height;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_create(
    sao_ui_overlay_host_handle_t host,
    const SaoDcompBridgeConfig* config,
    sao_ui_dcomp_bridge_handle_t* out_handle);

// Destroy on the creating render thread. Reverse teardown detaches the visual
// tree before releasing swap-chain, DComp, DXGI, context, and device refs.
SAO_UI_API void SAO_UI_CALL sao_ui_dcomp_bridge_destroy(
    sao_ui_dcomp_bridge_handle_t handle);

// Attach the composition swapchain to the host HWND.  Idempotent.
// Runs the full DCompositionCreateDevice + CreateTargetForHwnd +
// CreateVisual + SetContent + SetRoot + Commit chain.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_attach(
    sao_ui_dcomp_bridge_handle_t handle);

// Detach from HWND without destroying the bridge — used for hot
// re-hosting.  Reciprocal to attach().
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_detach(
    sao_ui_dcomp_bridge_handle_t handle);

// Present one frame.  Returns SAO_STATUS_ERR_DEVICE_LOST on TDR — the
// caller is expected to tear the bridge down + recreate.  Present0
// (0, 0) uses immediate presentation; the DXGI_STATUS_OCCLUDED
// success code (monitor off) is coerced to SAO_STATUS_OK — the
// device is fine, only the panel is dark.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_present(
    sao_ui_dcomp_bridge_handle_t handle);

// Resize the swapchain back buffers to match the host geometry.
// Internally releases current backbuffer references, calls
// IDXGISwapChain::ResizeBuffers, and re-acquires.  Fails on device-
// lost (caller must recreate).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_resize(
    sao_ui_dcomp_bridge_handle_t handle, uint32_t width, uint32_t height);

// Upload one premultiplied BGRA frame into the composition swapchain's
// current back buffer. The bridge owns the reusable dynamic upload texture;
// callers retain ownership of `premultiplied_bgra` until this call returns.
// `stride` is the source row pitch in bytes and must be at least width * 4.
// Call present() after one or more uploads to flush the frame to DWM.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_upload_bgra(
    sao_ui_dcomp_bridge_handle_t handle,
    const uint8_t* premultiplied_bgra,
    uint32_t width,
    uint32_t height,
    uint32_t stride);

// Copy a GPU-composited BGRA8 texture into the current DirectComposition
// swap-chain buffer without a CPU map/upload. The source must belong to the
// bridge's D3D11 device, match width/height, and contain premultiplied alpha.
// Owner-thread only; call present() afterwards.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dcomp_bridge_copy_texture(sao_ui_dcomp_bridge_handle_t handle,
                                 void* d3d11_texture, // borrowed ID3D11Texture2D*
                                 uint32_t width, uint32_t height);

// ── Interop with WGL_NV_DX_interop2 ─────────────────────────────
//
// Explicit legacy gate: the D3D/DComp production host has no WGL context, so
// these four entry points return SAO_STATUS_ERR_NOT_IMPLEMENTED.
//
// Register a D3D11 texture for WGL interop.  Returns an opaque
// interop handle used by lock_texture/unlock_texture.  Wraps
// wglDXOpenDeviceNV / wglDXRegisterObjectNV.  Failure returns
// nullptr in out_interop_handle.  MUST be called on a thread with
// the WGL context current.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_register_gl_interop(
    sao_ui_dcomp_bridge_handle_t handle,
    void* d3d11_texture,             // ID3D11Texture2D*
    uint32_t gl_texture_name,        // uint from glGenTextures
    void** out_interop_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_unregister_gl_interop(
    sao_ui_dcomp_bridge_handle_t handle,
    void* interop_handle);

// Lock the interop texture for GL rendering.  wraps
// wglDXLockObjectsNV.  MUST be preceded by device_removed() check
// (§9 of handoff).  Failure returns SAO_STATUS_ERR_DEVICE_LOST
// or SAO_STATUS_ERR_TIMEOUT.  Never blocks indefinitely against a
// removed device.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_lock_texture(
    sao_ui_dcomp_bridge_handle_t handle,
    void* interop_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_unlock_texture(
    sao_ui_dcomp_bridge_handle_t handle,
    void* interop_handle);

// Non-blocking device-removed check.  ID3D11Device::GetDeviceRemovedReason
// wrapper.  Returns SAO_STATUS_OK if the device is alive, or
// SAO_STATUS_ERR_DEVICE_LOST with the DXGI HRESULT in `out_reason`
// (DXGI_ERROR_DEVICE_REMOVED / _HUNG / _RESET; DXGI_STATUS_OCCLUDED
// is coerced to OK).  Always safe to call — never acquires a lock.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_device_removed(
    sao_ui_dcomp_bridge_handle_t handle,
    uint32_t* out_reason);

// Grab the underlying interfaces for consumers that need direct COM
// access (shared-texture producer in another module).  Do NOT
// Release — the bridge owns the ref.
SAO_UI_API void* SAO_UI_CALL sao_ui_dcomp_bridge_d3d11_device(
    sao_ui_dcomp_bridge_handle_t handle);
SAO_UI_API void* SAO_UI_CALL sao_ui_dcomp_bridge_d3d11_context(
    sao_ui_dcomp_bridge_handle_t handle);
SAO_UI_API void* SAO_UI_CALL sao_ui_dcomp_bridge_swap_chain(
    sao_ui_dcomp_bridge_handle_t handle);
SAO_UI_API void* SAO_UI_CALL sao_ui_dcomp_bridge_dcomp_device(
    sao_ui_dcomp_bridge_handle_t handle);

struct SaoDcompBridgeState {
    uint32_t width;
    uint32_t height;
    uint32_t alpha_mode;
    uint32_t buffer_count;
    uint32_t owner_thread_id;
    uint32_t last_removed_reason;
    bool attached;
    bool alive;
    bool swap_chain_ready;
    bool upload_texture_ready;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_get_state(
    sao_ui_dcomp_bridge_handle_t handle,
    SaoDcompBridgeState* out_state);

#ifdef __cplusplus
}  // extern "C"
#endif
