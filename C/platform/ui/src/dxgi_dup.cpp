// SAO Auto - dxgi_dup first-implementable slice (Wave 3 / G1.4).
//
// 1:1 with `sao_auto/python/render/dxgi_duplication.py` DXGIDuplicator._init.
//
// Scope of THIS slice:
//   - sao_ui_dxgi_dup_create : owns its own D3D11 device (matches Python;
//     Wave 5 refactor lets callers reuse the shared d3d11_device module).
//     Chain: D3D11CreateDevice -> QI IDXGIDevice -> GetAdapter ->
//     EnumOutputs(output_index) -> QI IDXGIOutput1 -> DuplicateOutput ->
//     GetDesc.
//   - sao_ui_dxgi_dup_destroy : reverse-order Release the whole chain.
//   - sao_ui_dxgi_dup_acquire_frame : AcquireNextFrame(timeout) with
//     status mapping (WAIT_TIMEOUT -> ERR_TIMEOUT, ACCESS_LOST ->
//     ERR_SURFACE_INVALID, DEVICE_* -> ERR_DEVICE_LOST).  QI
//     ID3D11Texture2D and hand out a BORROWED pointer to the desktop
//     texture; the CopyResource + Map (RGB tight-pack) is Wave 4.
//   - sao_ui_dxgi_dup_release_frame : IDXGIOutputDuplication::ReleaseFrame
//     with a double-release guard.
//   - sao_ui_dxgi_dup_get_desc : GetDesc snapshot.
//   - sao_ui_dxgi_dup_reinit : Force-rebuild (test aid).
//
// Deliberately NOT in this slice (leaves fields zeroed):
//   - Dirty-rect / move-rect metadata handling
//   - Cursor shape handling
//   - Auto-recover loop wrapped around acquire_frame
//
// Test invariants live in test_dxgi_dup_wave3.cpp.

#include "sao/ui/dxgi_dup.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <d3d11.h>
#  include <dxgi.h>
#  include <dxgi1_2.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <new>

namespace {

#if defined(_WIN32)

template <typename T>
inline void safe_release(T** p) {
    if (p != nullptr && *p != nullptr) {
        (*p)->Release();
        *p = nullptr;
    }
}

#endif

}  // namespace

// ── state ──────────────────────────────────────────────────────────

struct sao_ui_dxgi_dup_s {
#if defined(_WIN32)
    ID3D11Device*            d3d_dev = nullptr;
    ID3D11DeviceContext*     d3d_ctx = nullptr;
    IDXGIDevice*             dxgi_dev = nullptr;
    IDXGIAdapter*            dxgi_adp = nullptr;
    IDXGIOutput1*            output1 = nullptr;
    IDXGIOutputDuplication*  dup = nullptr;

    // Config snapshot (for reinit).
    uint32_t                 output_index = 0;
    uint32_t                 adapter_index = 0;
    uint32_t                 acquire_timeout_ms = 16;
    bool                     auto_recover = true;

    // Metrics returned by acquire_frame.  Wave 4 fills bgra_bytes from
    // the mapped staging texture; this slice hands back nullptr for the
    // pixel pointer and only asserts the acquire/release protocol.
    DXGI_OUTDUPL_DESC        desc = {};
    RECT                     desktop_rect = {};
    DXGI_MODE_ROTATION       rotation = DXGI_MODE_ROTATION_IDENTITY;

    // Held frame guard: AcquireNextFrame must be balanced 1:1 with
    // ReleaseFrame.  Double-release returns SAO_STATUS_ERR_INVALID_ARGUMENT
    // per the CASE dxgi_dup_double_release_returns_error test contract.
    bool                     frame_held = false;
    bool                     alive = false;

    // ── Wave 7 additions ────────────────────────────────────────
    // The currently-held desktop texture (borrowed pointer valid
    // between acquire_frame and release_frame).  Kept as ID3D11-
    // Texture2D so copy_to_staging / cursor_info can operate without
    // an extra QueryInterface round-trip.
    ID3D11Texture2D*         held_texture = nullptr;
    IDXGIResource*           held_resource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO  held_frame_info = {};

    // Preallocated staging texture (lazy, sized on first
    // copy_to_staging call).  Reused across frames.
    ID3D11Texture2D*         staging = nullptr;
    uint32_t                 staging_w = 0;
    uint32_t                 staging_h = 0;

    // Last known cursor shape (DXGI only sends shape when it changes;
    // we cache the previous shape so callers that missed a shape
    // update can still read the "current cursor" metadata).
    DXGI_OUTDUPL_POINTER_SHAPE_INFO cursor_shape_info = {};
    // Raw shape bytes; capacity grows as needed.  Sized to
    // shape_info.Width * shape_info.Pitch, may be padded.
    uint8_t*                 cursor_shape_bytes = nullptr;
    uint32_t                 cursor_shape_bytes_size = 0;
    uint32_t                 cursor_shape_bytes_capacity = 0;
    bool                     has_cursor_shape = false;
#else
    void*                    _unused = nullptr;
#endif
};

// ── create ────────────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_create(
    const SaoDxgiDupConfig* config,
    sao_ui_dxgi_dup_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;

#if !defined(_WIN32)
    (void)config;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    if (config == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if ((config->staging_width == 0) != (config->staging_height == 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    sao_ui_dxgi_dup_s* d = new (std::nothrow) sao_ui_dxgi_dup_s();
    if (d == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    d->output_index = config->output_index;
    d->adapter_index = config->adapter_index;
    d->acquire_timeout_ms =
        (config->acquire_timeout_ms > 0) ? config->acquire_timeout_ms : 16;
    d->auto_recover = config->auto_recover;

    // 1. D3D11CreateDevice (requested adapter, or hardware first with WARP
    // fallback for the default adapter on CI/RDP).
    const D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    IDXGIAdapter1* requested_adapter = nullptr;
    if (config->adapter_index != 0) {
        IDXGIFactory1* factory = nullptr;
        HRESULT factory_hr = ::CreateDXGIFactory1(
            __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
        if (FAILED(factory_hr) || factory == nullptr) {
            safe_release(&factory);
            delete d;
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        factory_hr = factory->EnumAdapters1(config->adapter_index, &requested_adapter);
        safe_release(&factory);
        if (factory_hr == DXGI_ERROR_NOT_FOUND) {
            delete d;
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        if (FAILED(factory_hr) || requested_adapter == nullptr) {
            safe_release(&requested_adapter);
            delete d;
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
    }
    HRESULT hr = ::D3D11CreateDevice(
        requested_adapter,
        requested_adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        requested, static_cast<UINT>(std::size(requested)),
        D3D11_SDK_VERSION,
        &d->d3d_dev, &got, &d->d3d_ctx);
    if (FAILED(hr) && requested_adapter == nullptr) {
        hr = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            requested, static_cast<UINT>(std::size(requested)),
            D3D11_SDK_VERSION,
            &d->d3d_dev, &got, &d->d3d_ctx);
    }
            safe_release(&requested_adapter);
    if (FAILED(hr) || d->d3d_dev == nullptr) {
        delete d;
        return SAO_STATUS_ERR_DEVICE_LOST;
    }

    // 2. QI IDXGIDevice -> GetAdapter.
    hr = d->d3d_dev->QueryInterface(
        __uuidof(IDXGIDevice),
        reinterpret_cast<void**>(&d->dxgi_dev));
    if (FAILED(hr) || d->dxgi_dev == nullptr) {
        safe_release(&d->d3d_ctx);
        safe_release(&d->d3d_dev);
        delete d;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    hr = d->dxgi_dev->GetAdapter(&d->dxgi_adp);
    if (FAILED(hr) || d->dxgi_adp == nullptr) {
        safe_release(&d->dxgi_dev);
        safe_release(&d->d3d_ctx);
        safe_release(&d->d3d_dev);
        delete d;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    // 3. EnumOutputs(output_index) -> QI IDXGIOutput1.
    IDXGIOutput* output_raw = nullptr;
    hr = d->dxgi_adp->EnumOutputs(config->output_index, &output_raw);
    if (FAILED(hr) || output_raw == nullptr) {
        safe_release(&d->dxgi_adp);
        safe_release(&d->dxgi_dev);
        safe_release(&d->d3d_ctx);
        safe_release(&d->d3d_dev);
        delete d;
        // Map DXGI_ERROR_NOT_FOUND -> NOT_FOUND so the invalid-output-
        // index test can assert against a distinct error code.
        if (hr == DXGI_ERROR_NOT_FOUND) return SAO_STATUS_ERR_NOT_FOUND;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    hr = output_raw->QueryInterface(
        __uuidof(IDXGIOutput1),
        reinterpret_cast<void**>(&d->output1));
    // Cache desktop_rect for downstream origin_x/y reporting.
    DXGI_OUTPUT_DESC out_desc = {};
    output_raw->GetDesc(&out_desc);
    d->desktop_rect = out_desc.DesktopCoordinates;
    d->rotation = out_desc.Rotation;
    output_raw->Release();
    if (FAILED(hr) || d->output1 == nullptr) {
        safe_release(&d->dxgi_adp);
        safe_release(&d->dxgi_dev);
        safe_release(&d->d3d_ctx);
        safe_release(&d->d3d_dev);
        delete d;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    // 4. DuplicateOutput.  DXGI_ERROR_UNSUPPORTED here means the current
    // session can't take a duplication (e.g. session0, or session lock).
    hr = d->output1->DuplicateOutput(d->d3d_dev, &d->dup);
    if (FAILED(hr) || d->dup == nullptr) {
        safe_release(&d->output1);
        safe_release(&d->dxgi_adp);
        safe_release(&d->dxgi_dev);
        safe_release(&d->d3d_ctx);
        safe_release(&d->d3d_dev);
        delete d;
        if (hr == DXGI_ERROR_UNSUPPORTED) return SAO_STATUS_ERR_ACCESS_DENIED;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    // 5. GetDesc snapshot.
    d->dup->GetDesc(&d->desc);

    if (config->staging_width != 0) {
        D3D11_TEXTURE2D_DESC staging_desc{};
        staging_desc.Width = config->staging_width;
        staging_desc.Height = config->staging_height;
        staging_desc.MipLevels = 1;
        staging_desc.ArraySize = 1;
        staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        staging_desc.SampleDesc.Count = 1;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = d->d3d_dev->CreateTexture2D(&staging_desc, nullptr, &d->staging);
        if (FAILED(hr) || d->staging == nullptr) {
            safe_release(&d->dup);
            safe_release(&d->output1);
            safe_release(&d->dxgi_adp);
            safe_release(&d->dxgi_dev);
            safe_release(&d->d3d_ctx);
            safe_release(&d->d3d_dev);
            delete d;
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        d->staging_w = config->staging_width;
        d->staging_h = config->staging_height;
    }

    d->alive = true;
    *out_handle = d;
    return SAO_STATUS_OK;
#endif
}

// ── destroy ──────────────────────────────────────────────────────

extern "C" void SAO_UI_CALL sao_ui_dxgi_dup_destroy(
    sao_ui_dxgi_dup_handle_t handle) {
    if (handle == nullptr) return;
#if defined(_WIN32)
    sao_ui_dxgi_dup_s* d = handle;
    // Ensure any held frame is released before destroying the dup.
    if (d->frame_held && d->dup != nullptr) {
        safe_release(&d->held_texture);
        safe_release(&d->held_resource);
        d->dup->ReleaseFrame();
        d->frame_held = false;
    }
    // Wave 7: staging texture + cursor shape cache.
    safe_release(&d->staging);
    if (d->cursor_shape_bytes != nullptr) {
        delete[] d->cursor_shape_bytes;
        d->cursor_shape_bytes = nullptr;
        d->cursor_shape_bytes_size = 0;
        d->cursor_shape_bytes_capacity = 0;
    }
    safe_release(&d->dup);
    safe_release(&d->output1);
    safe_release(&d->dxgi_adp);
    safe_release(&d->dxgi_dev);
    safe_release(&d->d3d_ctx);
    safe_release(&d->d3d_dev);
    d->alive = false;
    delete d;
#endif
}

// ── acquire_frame ────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_acquire_frame(
    sao_ui_dxgi_dup_handle_t handle, SaoDxgiDupFrame* out_frame) {
    if (out_frame != nullptr) {
        std::memset(out_frame, 0, sizeof(*out_frame));
    }
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    sao_ui_dxgi_dup_s* d = handle;
    if (!d->alive || d->dup == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (d->frame_held) {
        // Contract: caller must release the previous frame first.
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    DXGI_OUTDUPL_FRAME_INFO info = {};
    IDXGIResource* resource = nullptr;
    HRESULT hr = d->dup->AcquireNextFrame(
        d->acquire_timeout_ms, &info, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return SAO_STATUS_ERR_TIMEOUT;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        if (d->auto_recover) {
            (void)sao_ui_dxgi_dup_reinit(handle);
        }
        return SAO_STATUS_ERR_SURFACE_INVALID;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_HUNG   ||
        hr == DXGI_ERROR_DEVICE_RESET) {
        d->alive = false;
        return SAO_STATUS_ERR_DEVICE_LOST;
    }
    if (FAILED(hr) || resource == nullptr) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    // QI ID3D11Texture2D from the resource.  Wave 7 keeps both the
    // resource + texture pointers alive for the duration of the held
    // frame (so copy_to_staging / cursor_info can operate without an
    // extra Acquire).  The resource is released in release_frame.
    ID3D11Texture2D* tex = nullptr;
    HRESULT qi_hr = resource->QueryInterface(
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&tex));
    if (FAILED(qi_hr) || tex == nullptr) {
        resource->Release();
        // Even on QI failure we must ReleaseFrame — the dup surface
        // has already handed us a lease.
        d->dup->ReleaseFrame();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    d->held_resource = resource;   // owning ref transferred
    d->held_texture  = tex;         // owning ref (from QI)
    d->held_frame_info = info;
    d->frame_held = true;

    if (out_frame != nullptr) {
        out_frame->width       = d->desc.ModeDesc.Width;
        out_frame->height      = d->desc.ModeDesc.Height;
        out_frame->origin_x    = d->desktop_rect.left;
        out_frame->origin_y    = d->desktop_rect.top;
        out_frame->rotation    = static_cast<uint32_t>(d->rotation);
        out_frame->row_pitch   = 0;  // Wave 4/7 fills via copy_to_staging.
        out_frame->bgra_bytes  = nullptr;  // Wave 4/7 via copy_to_staging.
        out_frame->last_present_time =
            static_cast<uint64_t>(info.LastPresentTime.QuadPart);
        out_frame->accumulated_frames = info.AccumulatedFrames;
        out_frame->protected_content_masked_out =
            info.ProtectedContentMaskedOut != FALSE;
    }
    return SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

// ── release_frame ────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_release_frame(
    sao_ui_dxgi_dup_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    sao_ui_dxgi_dup_s* d = handle;
    if (!d->alive || d->dup == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (!d->frame_held) {
        // Double-release: no frame is currently held.  Distinct error
        // per the CASE dxgi_dup_double_release_returns_error contract.
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    // Release the retained texture + resource pointers acquired
    // during Wave 7's acquire_frame.  Order does not matter (both are
    // reference-counted) but reverse-of-acquire keeps the intent clear.
    safe_release(&d->held_texture);
    safe_release(&d->held_resource);
    const HRESULT hr = d->dup->ReleaseFrame();
    d->frame_held = false;
    std::memset(&d->held_frame_info, 0, sizeof(d->held_frame_info));
    return SUCCEEDED(hr) ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

// ── get_desc ────────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_get_desc(
    sao_ui_dxgi_dup_handle_t handle, SaoDxgiDupDesc* out_desc) {
    if (out_desc != nullptr) {
        std::memset(out_desc, 0, sizeof(*out_desc));
    }
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    sao_ui_dxgi_dup_s* d = handle;
    if (!d->alive) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (out_desc == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    out_desc->width            = d->desc.ModeDesc.Width;
    out_desc->height           = d->desc.ModeDesc.Height;
    out_desc->refresh_rate_num = d->desc.ModeDesc.RefreshRate.Numerator;
    out_desc->refresh_rate_den = d->desc.ModeDesc.RefreshRate.Denominator;
    out_desc->format           = static_cast<uint32_t>(d->desc.ModeDesc.Format);
    out_desc->rotation         = static_cast<uint32_t>(d->desc.Rotation);
    out_desc->desktop_image_in_system_memory =
        d->desc.DesktopImageInSystemMemory != FALSE;
    return SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

// ── reinit (test aid) ───────────────────────────────────────────
// Mirrors DXGIDuplicator._rebuild_duplication: release only the
// duplication + output1 while keeping the d3d device + adapter alive,
// then re-open.

extern "C" sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_reinit(
    sao_ui_dxgi_dup_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    sao_ui_dxgi_dup_s* d = handle;
    if (d->d3d_dev == nullptr || d->dxgi_adp == nullptr) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    if (d->frame_held && d->dup != nullptr) {
        safe_release(&d->held_texture);
        safe_release(&d->held_resource);
        d->dup->ReleaseFrame();
        d->frame_held = false;
    }
    // Staging texture is monitor-size-specific; invalidate on reinit.
    safe_release(&d->staging);
    d->staging_w = 0;
    d->staging_h = 0;
    safe_release(&d->dup);
    safe_release(&d->output1);

    IDXGIOutput* output_raw = nullptr;
    HRESULT hr = d->dxgi_adp->EnumOutputs(d->output_index, &output_raw);
    if (FAILED(hr) || output_raw == nullptr) {
        d->alive = false;
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    hr = output_raw->QueryInterface(
        __uuidof(IDXGIOutput1),
        reinterpret_cast<void**>(&d->output1));
    DXGI_OUTPUT_DESC out_desc = {};
    output_raw->GetDesc(&out_desc);
    d->desktop_rect = out_desc.DesktopCoordinates;
    d->rotation = out_desc.Rotation;
    output_raw->Release();
    if (FAILED(hr) || d->output1 == nullptr) {
        d->alive = false;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    hr = d->output1->DuplicateOutput(d->d3d_dev, &d->dup);
    if (FAILED(hr) || d->dup == nullptr) {
        d->alive = false;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    d->dup->GetDesc(&d->desc);
    d->alive = true;
    return SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

// ── Wave 7: dirty-rects / move-rects / cursor / staging copy ────

extern "C" sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_get_dirty_rects(
    sao_ui_dxgi_dup_handle_t handle,
    SaoDxgiDupRect* rects_out,
    uint32_t         capacity,
    uint32_t*        count_out) {
    if (count_out != nullptr) *count_out = 0;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    sao_ui_dxgi_dup_s* d = handle;
    if (!d->alive || d->dup == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (!d->frame_held) return SAO_STATUS_ERR_NOT_INITIALIZED;

    // Fast path when caller wants data: call GetFrameDirtyRects with
    // their buffer directly.  DXGI ships filled_bytes on both S_OK
    // and DXGI_ERROR_MORE_DATA — S_OK means the caller's buffer was
    // enough, MORE_DATA means the required size is in filled_bytes
    // and we should retry with a larger buffer (or report BUFFER_TOO_
    // SMALL).  Zero-count returns S_OK with filled_bytes=0.
    //
    // Note: SaoDxgiDupRect is layout-compatible with RECT (four
    // consecutive int32 members with the same names), asserted
    // statically so we can hand DXGI the caller's buffer directly.
    static_assert(sizeof(SaoDxgiDupRect) == sizeof(RECT),
                  "SaoDxgiDupRect must be RECT-compatible");
    RECT stack_scratch[16];
    RECT* buf = stack_scratch;
    UINT  buf_bytes = sizeof(stack_scratch);
    if (rects_out != nullptr && capacity > 0) {
        buf = reinterpret_cast<RECT*>(rects_out);
        buf_bytes = capacity * static_cast<UINT>(sizeof(RECT));
    }
    UINT filled = 0;
    HRESULT hr = d->dup->GetFrameDirtyRects(buf_bytes, buf, &filled);
    if (hr == S_OK) {
        const uint32_t written = filled / sizeof(RECT);
        if (count_out != nullptr) *count_out = written;
        return SAO_STATUS_OK;
    }
    if (hr != DXGI_ERROR_MORE_DATA) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const uint32_t needed = filled / sizeof(RECT);
    if (count_out != nullptr) *count_out = needed;
    if (rects_out == nullptr) return SAO_STATUS_OK;
    if (capacity < needed) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    // Retry with the caller buffer sized appropriately (we now know
    // it fits — capacity >= needed).
    hr = d->dup->GetFrameDirtyRects(
        capacity * static_cast<UINT>(sizeof(RECT)),
        reinterpret_cast<RECT*>(rects_out), &filled);
    if (FAILED(hr)) return SAO_STATUS_ERR_OS_CALL_FAILED;
    if (count_out != nullptr) *count_out = filled / sizeof(RECT);
    return SAO_STATUS_OK;
#else
    (void)rects_out; (void)capacity;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_get_move_rects(
    sao_ui_dxgi_dup_handle_t handle,
    SaoDxgiDupMoveRect* rects_out,
    uint32_t             capacity,
    uint32_t*            count_out) {
    if (count_out != nullptr) *count_out = 0;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    sao_ui_dxgi_dup_s* d = handle;
    if (!d->alive || d->dup == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (!d->frame_held) return SAO_STATUS_ERR_NOT_INITIALIZED;

    // Same two-pass pattern as get_dirty_rects: hand DXGI the caller's
    // buffer directly (if provided) and check for MORE_DATA to detect
    // an undersized capacity.  Move rects are ABI-incompatible with
    // our SaoDxgiDupMoveRect (nested Point + Rect vs. flat src_x/y +
    // dst rect), so we always translate — use a stack scratch for
    // the count-only probe.
    DXGI_OUTDUPL_MOVE_RECT stack_scratch[16];
    DXGI_OUTDUPL_MOVE_RECT* scratch = stack_scratch;
    UINT scratch_bytes = sizeof(stack_scratch);
    uint32_t scratch_cap =
        sizeof(stack_scratch) / sizeof(DXGI_OUTDUPL_MOVE_RECT);
    UINT filled = 0;
    HRESULT hr = d->dup->GetFrameMoveRects(
        scratch_bytes, scratch, &filled);
    if (hr != S_OK && hr != DXGI_ERROR_MORE_DATA) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    uint32_t written = 0;
    if (hr == S_OK) {
        written = filled / static_cast<UINT>(sizeof(DXGI_OUTDUPL_MOVE_RECT));
    } else {
        // MORE_DATA: filled = required bytes.  Allocate to fit and
        // retry.
        const uint32_t needed = filled /
            static_cast<uint32_t>(sizeof(DXGI_OUTDUPL_MOVE_RECT));
        if (count_out != nullptr) *count_out = needed;
        if (rects_out == nullptr) return SAO_STATUS_OK;
        if (capacity < needed) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        DXGI_OUTDUPL_MOVE_RECT* heap =
            new (std::nothrow) DXGI_OUTDUPL_MOVE_RECT[needed];
        if (heap == nullptr) return SAO_STATUS_ERR_UNKNOWN;
        UINT big_bytes = needed *
            static_cast<UINT>(sizeof(DXGI_OUTDUPL_MOVE_RECT));
        UINT big_filled = 0;
        hr = d->dup->GetFrameMoveRects(big_bytes, heap, &big_filled);
        if (FAILED(hr)) {
            delete[] heap;
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        written = big_filled /
            static_cast<uint32_t>(sizeof(DXGI_OUTDUPL_MOVE_RECT));
        for (uint32_t i = 0; i < written; ++i) {
            rects_out[i].src_x = heap[i].SourcePoint.x;
            rects_out[i].src_y = heap[i].SourcePoint.y;
            rects_out[i].dst.left   = heap[i].DestinationRect.left;
            rects_out[i].dst.top    = heap[i].DestinationRect.top;
            rects_out[i].dst.right  = heap[i].DestinationRect.right;
            rects_out[i].dst.bottom = heap[i].DestinationRect.bottom;
        }
        if (count_out != nullptr) *count_out = written;
        delete[] heap;
        return SAO_STATUS_OK;
    }
    // S_OK path: everything fit in the stack scratch.
    if (count_out != nullptr) *count_out = written;
    if (written == 0 || rects_out == nullptr) return SAO_STATUS_OK;
    if (capacity < written) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    for (uint32_t i = 0; i < written; ++i) {
        rects_out[i].src_x = scratch[i].SourcePoint.x;
        rects_out[i].src_y = scratch[i].SourcePoint.y;
        rects_out[i].dst.left   = scratch[i].DestinationRect.left;
        rects_out[i].dst.top    = scratch[i].DestinationRect.top;
        rects_out[i].dst.right  = scratch[i].DestinationRect.right;
        rects_out[i].dst.bottom = scratch[i].DestinationRect.bottom;
    }
    (void)scratch_cap;  // reserved for future capacity paths
    return SAO_STATUS_OK;
#else
    (void)rects_out; (void)capacity;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_get_cursor_info(
    sao_ui_dxgi_dup_handle_t handle,
    SaoDxgiDupCursorInfo*    info_out,
    uint8_t*                 shape_out,
    uint32_t                 shape_capacity,
    uint32_t*                shape_bytes_out) {
    if (info_out != nullptr) std::memset(info_out, 0, sizeof(*info_out));
    if (shape_bytes_out != nullptr) *shape_bytes_out = 0;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (info_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    sao_ui_dxgi_dup_s* d = handle;
    if (!d->alive || d->dup == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (!d->frame_held) return SAO_STATUS_ERR_NOT_INITIALIZED;

    // Position always reflects the current frame (DXGI populates it
    // via AcquireNextFrame's DXGI_OUTDUPL_FRAME_INFO).
    const auto& fi = d->held_frame_info;
    info_out->position_x = fi.PointerPosition.Position.x;
    info_out->position_y = fi.PointerPosition.Position.y;
    info_out->visible    = (fi.PointerPosition.Visible != FALSE);
    info_out->position_updated = (fi.LastMouseUpdateTime.QuadPart != 0);

    // Shape only updates when PointerShapeBufferSize > 0.  When it
    // doesn't, we fall back to the previously-cached shape (if any).
    info_out->shape_updated = false;
    if (fi.PointerShapeBufferSize > 0) {
        // Reallocate cache if needed.
        const uint32_t needed = fi.PointerShapeBufferSize;
        if (d->cursor_shape_bytes_capacity < needed) {
            if (d->cursor_shape_bytes != nullptr) {
                delete[] d->cursor_shape_bytes;
                d->cursor_shape_bytes = nullptr;
            }
            d->cursor_shape_bytes = new (std::nothrow) uint8_t[needed];
            if (d->cursor_shape_bytes == nullptr) {
                d->cursor_shape_bytes_capacity = 0;
                d->cursor_shape_bytes_size = 0;
                return SAO_STATUS_ERR_UNKNOWN;
            }
            d->cursor_shape_bytes_capacity = needed;
        }
        UINT written = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info{};
        HRESULT hr = d->dup->GetFramePointerShape(
            d->cursor_shape_bytes_capacity,
            d->cursor_shape_bytes,
            &written,
            &shape_info);
        if (SUCCEEDED(hr)) {
            d->cursor_shape_bytes_size = written;
            d->cursor_shape_info = shape_info;
            d->has_cursor_shape = true;
            info_out->shape_updated = true;
        }
        // FAILED(hr) → keep last cached shape.  Rare; treated as "no
        // update this frame".
    }

    if (d->has_cursor_shape) {
        info_out->shape_type      = d->cursor_shape_info.Type;
        info_out->shape_width     = d->cursor_shape_info.Width;
        info_out->shape_height    = d->cursor_shape_info.Height;
        info_out->shape_pitch     = d->cursor_shape_info.Pitch;
        info_out->shape_hotspot_x = d->cursor_shape_info.HotSpot.x;
        info_out->shape_hotspot_y = d->cursor_shape_info.HotSpot.y;
    }

    // Copy shape bytes out on request.  Only meaningful when we
    // actually have a cached shape.
    if (shape_out != nullptr && d->has_cursor_shape) {
        if (shape_capacity < d->cursor_shape_bytes_size) {
            if (shape_bytes_out != nullptr) {
                *shape_bytes_out = d->cursor_shape_bytes_size;
            }
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        std::memcpy(shape_out, d->cursor_shape_bytes,
                    d->cursor_shape_bytes_size);
        if (shape_bytes_out != nullptr) {
            *shape_bytes_out = d->cursor_shape_bytes_size;
        }
    } else if (shape_out == nullptr && shape_capacity != 0) {
        // Header contract: NULL buffer must pair with zero capacity.
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
#else
    (void)shape_out; (void)shape_capacity;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_copy_to_staging(
    sao_ui_dxgi_dup_handle_t handle,
    uint8_t*                 bytes_out,
    uint32_t                 bytes_capacity,
    uint32_t*                stride_out,
    uint32_t*                bytes_written_out) {
    if (stride_out != nullptr) *stride_out = 0;
    if (bytes_written_out != nullptr) *bytes_written_out = 0;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (bytes_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    sao_ui_dxgi_dup_s* d = handle;
    if (!d->alive || d->dup == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (!d->frame_held || d->held_texture == nullptr) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    const uint32_t width = d->desc.ModeDesc.Width;
    const uint32_t height = d->desc.ModeDesc.Height;
    if (width == 0 || height == 0) return SAO_STATUS_ERR_NOT_INITIALIZED;

    // Lazy-allocate the staging texture at the current monitor size.
    // Reallocate on size change (e.g. after reinit into a new mode).
    if (d->staging == nullptr ||
        d->staging_w != width || d->staging_h != height) {
        safe_release(&d->staging);
        d->staging_w = 0;
        d->staging_h = 0;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;

        HRESULT hr = d->d3d_dev->CreateTexture2D(
            &td, nullptr, &d->staging);
        if (FAILED(hr) || d->staging == nullptr) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        d->staging_w = width;
        d->staging_h = height;
    }

    // GPU-side copy: desktop → staging.
    d->d3d_ctx->CopyResource(d->staging, d->held_texture);

    // Map staging for READ; extract RowPitch + memcpy into caller buf.
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = d->d3d_ctx->Map(
        d->staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return SAO_STATUS_ERR_OS_CALL_FAILED;

    const uint32_t pitch = mapped.RowPitch;
    const uint32_t total = pitch * height;
    if (stride_out != nullptr) *stride_out = pitch;
    if (bytes_capacity < total) {
        d->d3d_ctx->Unmap(d->staging, 0);
        if (bytes_written_out != nullptr) *bytes_written_out = total;
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(bytes_out, mapped.pData, total);
    d->d3d_ctx->Unmap(d->staging, 0);
    if (bytes_written_out != nullptr) *bytes_written_out = total;
    return SAO_STATUS_OK;
#else
    (void)bytes_out; (void)bytes_capacity;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dxgi_dup_get_state(
    sao_ui_dxgi_dup_handle_t handle,
    SaoDxgiDupState* out_state) {
    if (out_state != nullptr) *out_state = {};
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_state == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    out_state->output_index = handle->output_index;
    out_state->adapter_index = handle->adapter_index;
    out_state->acquire_timeout_ms = handle->acquire_timeout_ms;
    out_state->staging_width = handle->staging_w;
    out_state->staging_height = handle->staging_h;
    out_state->auto_recover = handle->auto_recover;
    out_state->alive = handle->alive;
    out_state->frame_held = handle->frame_held;
    out_state->cursor_shape_cached = handle->has_cursor_shape;
#endif
    return SAO_STATUS_OK;
}
