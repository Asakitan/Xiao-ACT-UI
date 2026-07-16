// SAO Auto — shared D3D11 device + immediate context.
//
// Python authoritative source: implicit in `render/dcomp_bridge.py`
// and `render/gpu_renderer.py`.  There the device is created inline
// per module; the C++ port centralises it so the DComp bridge,
// shared-texture producers, and plugin renderers all share ONE
// device per process (feature level 11.1).
//
// Owned by the platform for the lifetime of the overlay host.
//
// ── TDR handling ─────────────────────────────────────────────
//   A GPU TDR / driver re-init (which a display-mode switch or a
//   power transition can trigger) surfaces as DXGI_ERROR_DEVICE_*
//   HRESULTs, after which every D3D11/DXGI COM pointer we hold is
//   dangling.  Client code that observed DEVICE_LOST calls
//   `sao_ui_d3d11_device_recreate` — this releases the old objects
//   under lock, creates a new device+context, and invalidates all
//   borrowed pointers.  Clients holding borrowed refs MUST re-fetch
//   after any recreate.
//
// ── Feature levels ───────────────────────────────────────────
//   Requests {11_1, 11_0, 10_1, 10_0}, takes the first supported.
//   BGRA_SUPPORT flag (0x20) required — DComp swapchains only accept
//   B8G8R8A8_UNORM.  Debug layer opt-in via env SAO_D3D_DEBUG=1.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_d3d11_device_s* sao_ui_d3d11_device_handle_t;

struct SaoD3d11DeviceConfig {
    // Enable the D3D11 debug layer.  Overhead of ~2ms/frame; only for
    // dev sessions.  Mirrors env SAO_D3D_DEBUG=1.
    bool        enable_debug_layer;

    // Prefer WARP (software) driver.  Test-only; production is
    // D3D_DRIVER_TYPE_HARDWARE.
    bool        prefer_warp;

    // Adapter selection.  0 → default adapter.  Non-zero picks the
    // Nth entry from IDXGIFactory::EnumAdapters (multi-GPU laptops).
    uint32_t    adapter_index;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_d3d11_device_create(
    const SaoD3d11DeviceConfig* config,
    sao_ui_d3d11_device_handle_t* out_handle);

// Destroy on the creating render thread after dependents have released their
// borrowed interfaces and composition resources.
SAO_UI_API void SAO_UI_CALL sao_ui_d3d11_device_destroy(
    sao_ui_d3d11_device_handle_t handle);

// Borrowed pointers.  Do NOT Release — the module owns the ref.
// Cast to `ID3D11Device*` / `ID3D11DeviceContext*` at the call site.
SAO_UI_API void* SAO_UI_CALL sao_ui_d3d11_device_ptr(
    sao_ui_d3d11_device_handle_t handle);

SAO_UI_API void* SAO_UI_CALL sao_ui_d3d11_device_context_ptr(
    sao_ui_d3d11_device_handle_t handle);

// The associated DXGI factory (IDXGIFactory2 minimum, for
// CreateSwapChainForComposition).
SAO_UI_API void* SAO_UI_CALL sao_ui_d3d11_device_dxgi_factory(
    sao_ui_d3d11_device_handle_t handle);

// The chosen adapter (IDXGIAdapter1).  For multi-GPU introspection.
SAO_UI_API void* SAO_UI_CALL sao_ui_d3d11_device_dxgi_adapter(
    sao_ui_d3d11_device_handle_t handle);

// Actual feature level in use (0xB000..0xB100 = 11.0..11.1).
SAO_UI_API uint32_t SAO_UI_CALL sao_ui_d3d11_device_feature_level(
    sao_ui_d3d11_device_handle_t handle);

// TDR handling — a client that observed DEVICE_LOST should ask the
// module to recreate.  All borrowed pointers become invalid on success.
// Returns the DXGI HRESULT reason in out_reason (unmasked).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_d3d11_device_recreate(
    sao_ui_d3d11_device_handle_t handle,
    uint32_t* out_last_removed_reason);

// Non-blocking device-alive check.  `ID3D11Device::GetDeviceRemoved-
// Reason()` wrapper.  Returns SAO_STATUS_OK if the device is alive.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_d3d11_device_check_alive(
    sao_ui_d3d11_device_handle_t handle);

// Subscribe to device-lost notifications.  Called on the render
// thread the moment device_removed becomes non-zero.
typedef void (SAO_UI_CALL* sao_ui_d3d11_device_lost_fn_t)(
    uint32_t reason, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_d3d11_device_on_lost(
    sao_ui_d3d11_device_handle_t handle,
    sao_ui_d3d11_device_lost_fn_t fn, void* user_data);

struct SaoD3d11DeviceState {
    uint32_t feature_level;
    uint32_t adapter_index;
    uint32_t owner_thread_id;
    uint32_t last_removed_reason;
    bool device_ready;
    bool context_ready;
    bool factory_ready;
    bool adapter_ready;
    bool loss_notified;
    uint8_t _pad[3];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_d3d11_device_get_state(
    sao_ui_d3d11_device_handle_t handle,
    SaoD3d11DeviceState* out_state);

#ifdef __cplusplus
}  // extern "C"
#endif
