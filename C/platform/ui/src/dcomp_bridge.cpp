// SAO Auto — D3D11/DXGI/DirectComposition presentation bridge.
//
// The bridge owns a composition swap chain, dynamic BGRA upload texture, and
// DComp visual tree. Creation and render calls are bound to the creating
// render thread. Teardown reverses the tree attachment before releasing the
// swap chain, DComp objects, DXGI interfaces, immediate context, and device.

#include "sao/ui/dcomp_bridge.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <d3d11.h>
#  include <dxgi1_2.h>
#  include <dcomp.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <new>

namespace {

#if defined(_WIN32)

// dcomp.dll is late-loaded intentionally.  Some Windows headers/toolchains
// export `dcomp.lib` with the DCompositionCreateDevice symbol, but the SDK
// build posture across the tree is that late-load is safer against older
// headers.  We resolve at process-first-use and cache the address.
using DCompositionCreateDevice_t = HRESULT (WINAPI*)(IDXGIDevice*, REFIID, void**);

std::once_flag g_dcomp_once;
HMODULE g_dcomp_module = nullptr;
DCompositionCreateDevice_t g_pDCompositionCreateDevice = nullptr;

void resolve_dcomp() {
    std::call_once(g_dcomp_once, []() {
        g_dcomp_module = ::LoadLibraryW(L"dcomp.dll");
        if (g_dcomp_module != nullptr) {
            g_pDCompositionCreateDevice =
                reinterpret_cast<DCompositionCreateDevice_t>(
                    ::GetProcAddress(g_dcomp_module,
                                     "DCompositionCreateDevice"));
        }
    });
}

// Reverse-order Release helper for a COM pointer.  Matches Python's
// _release(): tolerates null, never throws.
template <typename T>
inline void safe_release(T** p) {
    if (p != nullptr && *p != nullptr) {
        (*p)->Release();
        *p = nullptr;
    }
}

#endif  // _WIN32

// Static COM-lifetime counter for the leak-check tests (see memory
// notes: "挂机卡死=Win32句柄泄漏清单").  Bumps on create success,
// decrements on destroy.  A leak-check test asserts create/destroy
// symmetry after N iterations by observing this returns to zero.
std::atomic<int64_t> g_bridge_live_count{0};

}  // namespace

// ── bridge state ────────────────────────────────────────────────────

struct sao_ui_dcomp_bridge_s {
#if defined(_WIN32)
    HWND                    hwnd = nullptr;
    DWORD                   owner_thread = 0;
    ID3D11Device*           d3d_dev = nullptr;
    ID3D11DeviceContext*    d3d_ctx = nullptr;
    IDXGIDevice*            dxgi_dev = nullptr;
    IDXGIAdapter*           dxgi_adp = nullptr;
    IDXGIFactory2*          dxgi_fac = nullptr;
    IDCompositionDevice*    dc_dev = nullptr;
    IDCompositionTarget*    dc_tgt = nullptr;
    IDCompositionVisual*    dc_vis = nullptr;
    IDXGISwapChain1*        swap = nullptr;
    ID3D11Texture2D*        upload_texture = nullptr;
    D3D_FEATURE_LEVEL       feature_level = D3D_FEATURE_LEVEL_11_0;
    uint32_t                width = 1;
    uint32_t                height = 1;
    uint32_t                alpha_mode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    uint32_t                buffer_count = 2;
    uint32_t                last_removed_reason = 0;
    bool                    attached = false;
    bool                    alive = false;
#else
    void*                   _unused = nullptr;
#endif
};

#if defined(_WIN32)

namespace {

bool is_owner_thread(const sao_ui_dcomp_bridge_s* bridge) {
    return bridge->owner_thread == ::GetCurrentThreadId();
}

bool is_device_lost_hresult(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED ||
           hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DEVICE_RESET;
}

sao_status_t check_device_removed(sao_ui_dcomp_bridge_s* bridge) {
    if (bridge->d3d_dev == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    const HRESULT hr = bridge->d3d_dev->GetDeviceRemovedReason();
    bridge->last_removed_reason = static_cast<uint32_t>(hr);
    if (SUCCEEDED(hr)) {
        bridge->last_removed_reason = 0;
        return SAO_STATUS_OK;
    }
    bridge->alive = false;
    return SAO_STATUS_ERR_DEVICE_LOST;
}

sao_status_t status_from_hresult(sao_ui_dcomp_bridge_s* bridge, HRESULT hr) {
    if (is_device_lost_hresult(hr) ||
        check_device_removed(bridge) == SAO_STATUS_ERR_DEVICE_LOST) {
        return SAO_STATUS_ERR_DEVICE_LOST;
    }
    return SAO_STATUS_ERR_OS_CALL_FAILED;
}

void release_upload_texture(sao_ui_dcomp_bridge_s* bridge) {
    safe_release(&bridge->upload_texture);
}

sao_status_t create_composition_swapchain(sao_ui_dcomp_bridge_s* bridge) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = bridge->width;
    desc.Height = bridge->height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = bridge->buffer_count;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = static_cast<DXGI_ALPHA_MODE>(bridge->alpha_mode);

    const HRESULT hr = bridge->dxgi_fac->CreateSwapChainForComposition(
        bridge->d3d_dev, &desc, nullptr, &bridge->swap);
    if (FAILED(hr) || bridge->swap == nullptr) {
        return status_from_hresult(bridge, hr);
    }
    return SAO_STATUS_OK;
}

sao_status_t attach_visual_tree(sao_ui_dcomp_bridge_s* bridge) {
    HRESULT hr = bridge->dc_vis->SetContent(bridge->swap);
    if (SUCCEEDED(hr)) {
        hr = bridge->dc_tgt->SetRoot(bridge->dc_vis);
    }
    if (SUCCEEDED(hr)) {
        hr = bridge->dc_dev->Commit();
    }
    if (FAILED(hr)) {
        bridge->dc_tgt->SetRoot(nullptr);
        bridge->dc_vis->SetContent(nullptr);
        return status_from_hresult(bridge, hr);
    }
    bridge->attached = true;
    return SAO_STATUS_OK;
}

}  // namespace

#endif  // _WIN32

// ── create ──────────────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_create(
    sao_ui_overlay_host_handle_t /*host*/,
    const SaoDcompBridgeConfig* config,
    sao_ui_dcomp_bridge_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;

#if !defined(_WIN32)
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    if (config == nullptr || config->hwnd == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!::IsWindow(reinterpret_cast<HWND>(config->hwnd))) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    resolve_dcomp();
    if (g_pDCompositionCreateDevice == nullptr) {
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    }

    sao_ui_dcomp_bridge_s* b = new (std::nothrow) sao_ui_dcomp_bridge_s();
    if (b == nullptr) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    b->hwnd = reinterpret_cast<HWND>(config->hwnd);
    b->owner_thread = ::GetCurrentThreadId();
    b->width = config->width == 0 ? 1 : config->width;
    b->height = config->height == 0 ? 1 : config->height;
    b->buffer_count = config->buffer_count < 2 ? 2 : config->buffer_count;
    b->alpha_mode = config->alpha_mode == 0
        ? static_cast<uint32_t>(DXGI_ALPHA_MODE_PREMULTIPLIED)
        : config->alpha_mode;
    if (b->alpha_mode != DXGI_ALPHA_MODE_PREMULTIPLIED) {
        delete b;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    HRESULT hr = S_OK;
    if (config->d3d11_device != nullptr) {
        b->d3d_dev = reinterpret_cast<ID3D11Device*>(config->d3d11_device);
        b->d3d_dev->AddRef();
        b->d3d_dev->GetImmediateContext(&b->d3d_ctx);
        b->feature_level = b->d3d_dev->GetFeatureLevel();
        if (b->d3d_ctx == nullptr) {
            safe_release(&b->d3d_dev);
            delete b;
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
    } else {
        const D3D_FEATURE_LEVEL requested[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            requested, static_cast<UINT>(std::size(requested)),
            D3D11_SDK_VERSION, &b->d3d_dev, &b->feature_level, &b->d3d_ctx);
        if (FAILED(hr)) {
            hr = ::D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                requested, static_cast<UINT>(std::size(requested)),
                D3D11_SDK_VERSION,
                &b->d3d_dev, &b->feature_level, &b->d3d_ctx);
        }
        if (FAILED(hr) || b->d3d_dev == nullptr || b->d3d_ctx == nullptr) {
            safe_release(&b->d3d_ctx);
            safe_release(&b->d3d_dev);
            delete b;
            return SAO_STATUS_ERR_DEVICE_LOST;
        }
    }

    // ── 2. QI IDXGIDevice ────────────────────────────────────────
    hr = b->d3d_dev->QueryInterface(
        __uuidof(IDXGIDevice),
        reinterpret_cast<void**>(&b->dxgi_dev));
    if (FAILED(hr) || b->dxgi_dev == nullptr) {
        safe_release(&b->d3d_ctx);
        safe_release(&b->d3d_dev);
        delete b;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    // ── 3. GetAdapter → IDXGIFactory2 ────────────────────────────
    hr = b->dxgi_dev->GetAdapter(&b->dxgi_adp);
    if (FAILED(hr) || b->dxgi_adp == nullptr) {
        safe_release(&b->dxgi_dev);
        safe_release(&b->d3d_ctx);
        safe_release(&b->d3d_dev);
        delete b;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    hr = b->dxgi_adp->GetParent(
        __uuidof(IDXGIFactory2),
        reinterpret_cast<void**>(&b->dxgi_fac));
    if (FAILED(hr) || b->dxgi_fac == nullptr) {
        safe_release(&b->dxgi_adp);
        safe_release(&b->dxgi_dev);
        safe_release(&b->d3d_ctx);
        safe_release(&b->d3d_dev);
        delete b;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    // ── 4. DCompositionCreateDevice + CreateTargetForHwnd + CreateVisual
    IUnknown* dc_dev_iunk = nullptr;
    hr = g_pDCompositionCreateDevice(
        b->dxgi_dev,
        __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(&dc_dev_iunk));
    if (FAILED(hr) || dc_dev_iunk == nullptr) {
        safe_release(&b->dxgi_fac);
        safe_release(&b->dxgi_adp);
        safe_release(&b->dxgi_dev);
        safe_release(&b->d3d_ctx);
        safe_release(&b->d3d_dev);
        delete b;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    b->dc_dev = reinterpret_cast<IDCompositionDevice*>(dc_dev_iunk);

    // topmost=TRUE per header line 111 (CreateTargetForHwnd second arg).
    hr = b->dc_dev->CreateTargetForHwnd(b->hwnd, TRUE, &b->dc_tgt);
    if (FAILED(hr) || b->dc_tgt == nullptr) {
        safe_release(&b->dc_dev);
        safe_release(&b->dxgi_fac);
        safe_release(&b->dxgi_adp);
        safe_release(&b->dxgi_dev);
        safe_release(&b->d3d_ctx);
        safe_release(&b->d3d_dev);
        delete b;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    hr = b->dc_dev->CreateVisual(&b->dc_vis);
    if (FAILED(hr) || b->dc_vis == nullptr) {
        safe_release(&b->dc_tgt);
        safe_release(&b->dc_dev);
        safe_release(&b->dxgi_fac);
        safe_release(&b->dxgi_adp);
        safe_release(&b->dxgi_dev);
        safe_release(&b->d3d_ctx);
        safe_release(&b->d3d_dev);
        delete b;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    b->alive = true;
    sao_status_t attach_status = create_composition_swapchain(b);
    if (attach_status == SAO_STATUS_OK) {
        attach_status = attach_visual_tree(b);
    }
    if (attach_status != SAO_STATUS_OK) {
        safe_release(&b->upload_texture);
        safe_release(&b->swap);
        safe_release(&b->dc_vis);
        safe_release(&b->dc_tgt);
        safe_release(&b->dc_dev);
        safe_release(&b->dxgi_fac);
        safe_release(&b->dxgi_adp);
        safe_release(&b->dxgi_dev);
        safe_release(&b->d3d_ctx);
        safe_release(&b->d3d_dev);
        delete b;
        return attach_status;
    }

    g_bridge_live_count.fetch_add(1, std::memory_order_relaxed);
    *out_handle = b;
    return SAO_STATUS_OK;
#endif
}

// ── destroy ─────────────────────────────────────────────────────────

extern "C" void SAO_UI_CALL sao_ui_dcomp_bridge_destroy(
    sao_ui_dcomp_bridge_handle_t handle) {
    if (handle == nullptr) return;
#if defined(_WIN32)
    sao_ui_dcomp_bridge_s* b = handle;
    if (b->alive) {
        g_bridge_live_count.fetch_sub(1, std::memory_order_relaxed);
    }
    if (b->attached && b->dc_tgt != nullptr && b->dc_vis != nullptr &&
        b->dc_dev != nullptr) {
        b->dc_tgt->SetRoot(nullptr);
        b->dc_vis->SetContent(nullptr);
        b->dc_dev->Commit();
    }
    release_upload_texture(b);
    safe_release(&b->swap);
    safe_release(&b->dc_vis);
    safe_release(&b->dc_tgt);
    safe_release(&b->dc_dev);
    safe_release(&b->dxgi_fac);
    safe_release(&b->dxgi_adp);
    safe_release(&b->dxgi_dev);
    safe_release(&b->d3d_ctx);
    safe_release(&b->d3d_dev);
    b->alive = false;
    delete b;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_attach(
    sao_ui_dcomp_bridge_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    if (!is_owner_thread(handle)) return SAO_STATUS_ERR_ACCESS_DENIED;
    if (!handle->alive || handle->swap == nullptr) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    if (handle->attached) return SAO_STATUS_OK;
    return attach_visual_tree(handle);
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_detach(
    sao_ui_dcomp_bridge_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    if (!is_owner_thread(handle)) return SAO_STATUS_ERR_ACCESS_DENIED;
    if (!handle->alive) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (!handle->attached) return SAO_STATUS_OK;
    HRESULT hr = handle->dc_tgt->SetRoot(nullptr);
    if (SUCCEEDED(hr)) hr = handle->dc_vis->SetContent(nullptr);
    if (SUCCEEDED(hr)) hr = handle->dc_dev->Commit();
    if (FAILED(hr)) return status_from_hresult(handle, hr);
    handle->attached = false;
    return SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_present(
    sao_ui_dcomp_bridge_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    sao_ui_dcomp_bridge_s* b = handle;
    if (!is_owner_thread(b)) return SAO_STATUS_ERR_ACCESS_DENIED;
    if (!b->alive || !b->attached || b->swap == nullptr || b->dc_dev == nullptr) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t status = check_device_removed(b);
    if (status != SAO_STATUS_OK) return status;
    HRESULT hr = b->swap->Present(0, 0);
    if (FAILED(hr)) return status_from_hresult(b, hr);
    hr = b->dc_dev->Commit();
    return FAILED(hr) ? status_from_hresult(b, hr) : SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_resize(
    sao_ui_dcomp_bridge_handle_t handle, uint32_t width, uint32_t height) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (width == 0 || height == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    if (!is_owner_thread(handle)) return SAO_STATUS_ERR_ACCESS_DENIED;
    if (!handle->alive || handle->swap == nullptr) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    if (width == handle->width && height == handle->height) return SAO_STATUS_OK;
    sao_status_t status = check_device_removed(handle);
    if (status != SAO_STATUS_OK) return status;
    release_upload_texture(handle);
    const HRESULT hr = handle->swap->ResizeBuffers(
        handle->buffer_count, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) return status_from_hresult(handle, hr);
    handle->width = width;
    handle->height = height;
    return SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_upload_bgra(
    sao_ui_dcomp_bridge_handle_t handle,
    const uint8_t* premultiplied_bgra,
    uint32_t width,
    uint32_t height,
    uint32_t stride) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (premultiplied_bgra == nullptr || width == 0 || height == 0 ||
        stride < width * 4u) return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    if (!is_owner_thread(handle)) return SAO_STATUS_ERR_ACCESS_DENIED;
    if (!handle->alive || !handle->attached || handle->swap == nullptr) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t status = check_device_removed(handle);
    if (status != SAO_STATUS_OK) return status;
    if (width != handle->width || height != handle->height) {
        status = sao_ui_dcomp_bridge_resize(handle, width, height);
        if (status != SAO_STATUS_OK) return status;
    }
    if (handle->upload_texture == nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT create_hr = handle->d3d_dev->CreateTexture2D(
            &desc, nullptr, &handle->upload_texture);
        if (FAILED(create_hr) || handle->upload_texture == nullptr) {
            return status_from_hresult(handle, create_hr);
        }
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = handle->d3d_ctx->Map(
        handle->upload_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return status_from_hresult(handle, hr);
    const size_t row_bytes = static_cast<size_t>(width) * 4u;
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(static_cast<uint8_t*>(mapped.pData) +
                        static_cast<size_t>(row) * mapped.RowPitch,
                    premultiplied_bgra + static_cast<size_t>(row) * stride,
                    row_bytes);
    }
    handle->d3d_ctx->Unmap(handle->upload_texture, 0);

    ID3D11Texture2D* back_buffer = nullptr;
    hr = handle->swap->GetBuffer(
        0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer));
    if (FAILED(hr) || back_buffer == nullptr) {
        safe_release(&back_buffer);
        return status_from_hresult(handle, hr);
    }
    handle->d3d_ctx->CopyResource(back_buffer, handle->upload_texture);
    safe_release(&back_buffer);
    return check_device_removed(handle);
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

// ── GL interop / keyed mutex compatibility gates ─────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_register_gl_interop(
    sao_ui_dcomp_bridge_handle_t, void*, uint32_t,
    void** out_interop_handle) {
    if (out_interop_handle != nullptr) *out_interop_handle = nullptr;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_unregister_gl_interop(
    sao_ui_dcomp_bridge_handle_t, void*) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_lock_texture(
    sao_ui_dcomp_bridge_handle_t, void*) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_unlock_texture(
    sao_ui_dcomp_bridge_handle_t, void*) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

// ── device_removed ──────────────────────────────────────────────
// Non-blocking wrapper around ID3D11Device::GetDeviceRemovedReason.
// Header contract line 165-171: OCCLUDED is coerced to OK.

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_device_removed(
    sao_ui_dcomp_bridge_handle_t handle, uint32_t* out_reason) {
    if (out_reason != nullptr) *out_reason = 0;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    sao_ui_dcomp_bridge_s* b = handle;
    const sao_status_t status = check_device_removed(b);
    if (out_reason != nullptr) *out_reason = b->last_removed_reason;
    return status;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

// ── borrowed getters ────────────────────────────────────────────
// Do NOT Release — the bridge owns the ref.  Match header line 173-183.

extern "C" void* SAO_UI_CALL sao_ui_dcomp_bridge_d3d11_device(
    sao_ui_dcomp_bridge_handle_t handle) {
    if (handle == nullptr) return nullptr;
#if defined(_WIN32)
    return handle->d3d_dev;
#else
    return nullptr;
#endif
}

extern "C" void* SAO_UI_CALL sao_ui_dcomp_bridge_d3d11_context(
    sao_ui_dcomp_bridge_handle_t handle) {
    if (handle == nullptr) return nullptr;
#if defined(_WIN32)
    return handle->d3d_ctx;
#else
    return nullptr;
#endif
}

extern "C" void* SAO_UI_CALL sao_ui_dcomp_bridge_swap_chain(
    sao_ui_dcomp_bridge_handle_t handle) {
    if (handle == nullptr) return nullptr;
#if defined(_WIN32)
    return handle->swap;
#else
    return nullptr;
#endif
}

extern "C" void* SAO_UI_CALL sao_ui_dcomp_bridge_dcomp_device(
    sao_ui_dcomp_bridge_handle_t handle) {
    if (handle == nullptr) return nullptr;
#if defined(_WIN32)
    return handle->dc_dev;
#else
    return nullptr;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dcomp_bridge_get_state(
    sao_ui_dcomp_bridge_handle_t handle,
    SaoDcompBridgeState* out_state) {
    if (out_state != nullptr) *out_state = {};
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_state == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    out_state->width = handle->width;
    out_state->height = handle->height;
    out_state->alpha_mode = handle->alpha_mode;
    out_state->buffer_count = handle->buffer_count;
    out_state->owner_thread_id = handle->owner_thread;
    out_state->last_removed_reason = handle->last_removed_reason;
    out_state->attached = handle->attached;
    out_state->alive = handle->alive;
    out_state->swap_chain_ready = handle->swap != nullptr;
    out_state->upload_texture_ready = handle->upload_texture != nullptr;
#endif
    return SAO_STATUS_OK;
}

// ── test hook (internal) ────────────────────────────────────────
// Not part of the public ABI header - used by test_dcomp_bridge.cpp
// to verify create/destroy symmetry after N iterations without pulling
// in a real COM leak detector.  Declared as a plain extern "C" symbol
// so it can be exported without a header change; the test declares the
// same signature locally.
extern "C" SAO_UI_API int64_t SAO_UI_CALL
sao_ui_dcomp_bridge_live_count_for_test(void) {
    return g_bridge_live_count.load(std::memory_order_relaxed);
}
