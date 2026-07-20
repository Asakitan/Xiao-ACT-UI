// SAO Auto — gpu_capture lifecycle gates + BGRA readback / hash helpers.
// 1:1 with `render/gpu_capture.py`.
//
// The WGC session lifecycle remains stubbed (not implemented) —
// the recognition path in the Python auth source relies on the
// `windows_capture` pyo3 module which we have no equivalent for in
// pure C++.  We keep the stubs so callers get an unambiguous
// SAO_STATUS_ERR_NOT_INITIALIZED and fall back to their PrintWindow
// path.
//
// Stateless helpers add the "raw pixels" plumbing that the Python
// auth source doesn't require (it consumes numpy arrays directly):
//   * sao_ui_gpu_capture_bgra_from_texture — D3D11 CopyResource + Map
//   * sao_ui_gpu_capture_premultiply_bgra  — straight → premultiplied
//     BGRA, sRGB-aware branch when alpha_correct=true
//   * sao_ui_gpu_capture_compute_hash      — 128-bit stable digest
//
// The BGRA readback owns a per-device staging texture cache so
// repeated calls at the same size share the D3D11 staging alloc.  The
// cache is intentionally global-static per process; consumers that
// need per-thread isolation should use dxgi_dup's per-handle staging
// texture instead.

#include "sao/ui/gpu_capture.h"

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
#  include <roapi.h>
#  if __has_include(<winrt/Windows.Graphics.Capture.h>) && \
      __has_include(<windows.graphics.capture.interop.h>) && \
      __has_include(<windows.graphics.directx.direct3d11.interop.h>)
#    define SAO_UI_HAS_WGC 1
#    include <winrt/base.h>
#    include <winrt/Windows.Foundation.h>
#    include <winrt/Windows.Graphics.Capture.h>
#    include <winrt/Windows.Graphics.DirectX.h>
#    include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#    include <windows.graphics.capture.interop.h>
#    include <windows.graphics.directx.direct3d11.interop.h>
#  endif
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#if defined(SAO_UI_HAS_WGC)

namespace {

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

double monotonic_seconds() {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    ::QueryPerformanceCounter(&counter);
    ::QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart == 0
        ? 0.0
        : static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

bool initialize_winrt(bool* out_uninitialize) {
    const HRESULT hr = ::RoInitialize(RO_INIT_MULTITHREADED);
    *out_uninitialize = hr == S_OK || hr == S_FALSE;
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

bool wgc_runtime_supported() {
    static const bool supported = [] {
        bool uninitialize = false;
        if (!initialize_winrt(&uninitialize)) return false;
        bool result = false;
        try {
            result = GraphicsCaptureSession::IsSupported();
        } catch (...) {
            result = false;
        }
        if (uninitialize) ::RoUninitialize();
        return result;
    }();
    return supported;
}

}  // namespace

struct WgcBackend {
    std::mutex mutex;
    HWND hwnd = nullptr;
    bool disable_cursor = true;
    int32_t state = SAO_UI_GPU_CAPTURE_STOPPED;
    DWORD apartment_thread = 0;
    bool ro_uninitialize = false;
    winrt::com_ptr<ID3D11Device> d3d_device;
    winrt::com_ptr<ID3D11DeviceContext> d3d_context;
    winrt::com_ptr<ID3D11Texture2D> staging;
    IDirect3DDevice winrt_device{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool frame_pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    uint32_t staging_width = 0;
    uint32_t staging_height = 0;
    std::vector<uint8_t> latest_bgra;
    uint32_t latest_width = 0;
    uint32_t latest_height = 0;
    uint32_t latest_pitch = 0;
    double latest_time = 0.0;
};

struct sao_ui_gpu_capture_s {
    SaoGpuCaptureConfig config{};
    std::unique_ptr<WgcBackend> backend;
    std::mutex snapshot_mutex;
    std::vector<uint8_t> snapshot;
    uint32_t snapshot_width = 0;
    uint32_t snapshot_height = 0;
    uint32_t snapshot_pitch = 0;
    double snapshot_time = 0.0;
};

namespace {

void release_wgc_session_no_lock(WgcBackend& backend) {
    if (backend.session != nullptr) backend.session.Close();
    if (backend.frame_pool != nullptr) backend.frame_pool.Close();
    backend.session = nullptr;
    backend.frame_pool = nullptr;
    backend.item = nullptr;
    backend.winrt_device = nullptr;
    backend.staging = nullptr;
    backend.d3d_context = nullptr;
    backend.d3d_device = nullptr;
    backend.staging_width = 0;
    backend.staging_height = 0;
    backend.latest_bgra.clear();
    backend.latest_width = 0;
    backend.latest_height = 0;
    backend.latest_pitch = 0;
    backend.latest_time = 0.0;
    backend.state = SAO_UI_GPU_CAPTURE_STOPPED;
}

void receive_wgc_frame(WgcBackend* backend) {
    if (backend == nullptr) return;
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        if (backend->state != SAO_UI_GPU_CAPTURE_RUNNING || backend->frame_pool == nullptr) return;
        auto frame = backend->frame_pool.TryGetNextFrame();
        if (frame == nullptr) return;
        auto access = frame.Surface().as<
            Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> source;
        winrt::check_hresult(access->GetInterface(
            __uuidof(ID3D11Texture2D), source.put_void()));
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        if (source_desc.Width == 0 || source_desc.Height == 0 ||
            (source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) return;
        if (backend->staging == nullptr ||
            backend->staging_width != source_desc.Width ||
            backend->staging_height != source_desc.Height) {
            D3D11_TEXTURE2D_DESC staging_desc = source_desc;
            staging_desc.MipLevels = 1;
            staging_desc.ArraySize = 1;
            staging_desc.SampleDesc.Count = 1;
            staging_desc.SampleDesc.Quality = 0;
            staging_desc.Usage = D3D11_USAGE_STAGING;
            staging_desc.BindFlags = 0;
            staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_desc.MiscFlags = 0;
            winrt::com_ptr<ID3D11Texture2D> staging;
            winrt::check_hresult(backend->d3d_device->CreateTexture2D(
                &staging_desc, nullptr, staging.put()));
            backend->staging = std::move(staging);
            backend->staging_width = source_desc.Width;
            backend->staging_height = source_desc.Height;
        }
        backend->d3d_context->CopyResource(backend->staging.get(), source.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(backend->d3d_context->Map(
            backend->staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const uint32_t packed_pitch = source_desc.Width * 4u;
        backend->latest_bgra.resize(
            static_cast<size_t>(packed_pitch) * source_desc.Height);
        for (uint32_t row = 0; row < source_desc.Height; ++row) {
            std::memcpy(backend->latest_bgra.data() +
                            static_cast<size_t>(row) * packed_pitch,
                        static_cast<const uint8_t*>(mapped.pData) +
                            static_cast<size_t>(row) * mapped.RowPitch,
                        packed_pitch);
        }
        backend->d3d_context->Unmap(backend->staging.get(), 0);
        backend->latest_width = source_desc.Width;
        backend->latest_height = source_desc.Height;
        backend->latest_pitch = packed_pitch;
        backend->latest_time = monotonic_seconds();
    } catch (...) {
        std::lock_guard<std::mutex> lock(backend->mutex);
        backend->state = SAO_UI_GPU_CAPTURE_FAILED;
    }
}

sao_status_t start_wgc_session(WgcBackend& backend) {
    if (backend.apartment_thread == 0) {
        bool uninitialize = false;
        if (!initialize_winrt(&uninitialize)) return SAO_STATUS_ERR_NOT_IMPLEMENTED;
        backend.ro_uninitialize = uninitialize;
        backend.apartment_thread = ::GetCurrentThreadId();
    } else if (backend.apartment_thread != ::GetCurrentThreadId()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, backend.d3d_device.put(),
        &feature_level, backend.d3d_context.put());
    if (FAILED(hr)) {
        hr = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, backend.d3d_device.put(),
            &feature_level, backend.d3d_context.put());
    }
    if (FAILED(hr)) return SAO_STATUS_ERR_OS_CALL_FAILED;
    auto dxgi_device = backend.d3d_device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(
        dxgi_device.get(), inspectable.put()));
    backend.winrt_device = inspectable.as<IDirect3DDevice>();

    const auto activation = winrt::get_activation_factory<GraphicsCaptureItem>();
    const auto interop = activation.as<IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(
        backend.hwnd, winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(backend.item)));
    const auto size = backend.item.Size();
    if (size.Width <= 0 || size.Height <= 0) return SAO_STATUS_ERR_OS_CALL_FAILED;
    backend.frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        backend.winrt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2, size);
    backend.session = backend.frame_pool.CreateCaptureSession(backend.item);
    try {
        backend.session.IsCursorCaptureEnabled(!backend.disable_cursor);
    } catch (...) {
    }
    backend.session.StartCapture();
    backend.state = SAO_UI_GPU_CAPTURE_RUNNING;
    return SAO_STATUS_OK;
}

}  // namespace

#else

struct sao_ui_gpu_capture_s {};

#endif

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_create(
    const SaoGpuCaptureConfig* config, sao_ui_gpu_capture_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (config == nullptr || config->hwnd == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if !defined(SAO_UI_HAS_WGC)
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    if (!wgc_runtime_supported()) return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    if (!::IsWindow(reinterpret_cast<HWND>(config->hwnd))) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (config->format != 0 && config->format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* capture = new (std::nothrow) sao_ui_gpu_capture_s();
    if (capture == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    capture->config = *config;
    if (capture->config.format == 0) capture->config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (capture->config.max_frame_age_sec <= 0.0) capture->config.max_frame_age_sec = 1.0;
    capture->backend = std::make_unique<WgcBackend>();
    capture->backend->hwnd = reinterpret_cast<HWND>(config->hwnd);
    capture->backend->disable_cursor = config->disable_cursor;
    *out_handle = capture;
    return SAO_STATUS_OK;
#endif
}

extern "C" void SAO_UI_CALL sao_ui_gpu_capture_destroy(
    sao_ui_gpu_capture_handle_t handle) {
    if (handle == nullptr) return;
    (void)sao_ui_gpu_capture_stop(handle);
#if defined(SAO_UI_HAS_WGC)
    if (handle->backend->ro_uninitialize &&
        handle->backend->apartment_thread == ::GetCurrentThreadId()) {
        ::RoUninitialize();
        handle->backend->ro_uninitialize = false;
    }
#endif
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_ensure_session(
    sao_ui_gpu_capture_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if !defined(SAO_UI_HAS_WGC)
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    std::lock_guard<std::mutex> lock(handle->backend->mutex);
    if (handle->backend->state == SAO_UI_GPU_CAPTURE_RUNNING) return SAO_STATUS_OK;
    if (handle->backend->state == SAO_UI_GPU_CAPTURE_UNSUPPORTED) {
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    }
    try {
        const sao_status_t status = start_wgc_session(*handle->backend);
        if (status != SAO_STATUS_OK) handle->backend->state = SAO_UI_GPU_CAPTURE_FAILED;
        return status;
    } catch (const winrt::hresult_error& error) {
        handle->backend->state = error.code() == E_NOTIMPL
            ? SAO_UI_GPU_CAPTURE_UNSUPPORTED
            : SAO_UI_GPU_CAPTURE_FAILED;
        return error.code() == E_NOTIMPL ? SAO_STATUS_ERR_NOT_IMPLEMENTED
                                         : SAO_STATUS_ERR_OS_CALL_FAILED;
    } catch (...) {
        handle->backend->state = SAO_UI_GPU_CAPTURE_FAILED;
        return SAO_STATUS_ERR_UNKNOWN;
    }
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_get_latest(
    sao_ui_gpu_capture_handle_t handle, SaoGpuCaptureFrame* out_frame) {
    if (out_frame == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_frame = {};
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if !defined(SAO_UI_HAS_WGC)
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    receive_wgc_frame(handle->backend.get());
    std::vector<uint8_t> latest;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    double capture_time = 0.0;
    {
        std::lock_guard<std::mutex> lock(handle->backend->mutex);
        if (handle->backend->latest_bgra.empty()) return SAO_STATUS_ERR_NOT_FOUND;
        if (monotonic_seconds() - handle->backend->latest_time >
            handle->config.max_frame_age_sec) return SAO_STATUS_ERR_NOT_FOUND;
        latest = handle->backend->latest_bgra;
        width = handle->backend->latest_width;
        height = handle->backend->latest_height;
        pitch = handle->backend->latest_pitch;
        capture_time = handle->backend->latest_time;
    }
    std::lock_guard<std::mutex> snapshot_lock(handle->snapshot_mutex);
    handle->snapshot = std::move(latest);
    handle->snapshot_width = width;
    handle->snapshot_height = height;
    handle->snapshot_pitch = pitch;
    handle->snapshot_time = capture_time;
    out_frame->pixels = handle->snapshot.data();
    out_frame->width = width;
    out_frame->height = height;
    out_frame->row_pitch = pitch;
    out_frame->channels = 4;
    out_frame->capture_time_sec = capture_time;
    return SAO_STATUS_OK;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_client_inset(
    sao_ui_gpu_capture_handle_t handle,
    int32_t* out_off_x, int32_t* out_off_y,
    int32_t* out_client_w, int32_t* out_client_h) {
    if (out_off_x != nullptr) *out_off_x = 0;
    if (out_off_y != nullptr) *out_off_y = 0;
    if (out_client_w != nullptr) *out_client_w = 0;
    if (out_client_h != nullptr) *out_client_h = 0;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_off_x == nullptr || out_off_y == nullptr ||
        out_client_w == nullptr || out_client_h == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
#if !defined(SAO_UI_HAS_WGC)
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    RECT client{};
    RECT window{};
    POINT origin{};
    const HWND hwnd = reinterpret_cast<HWND>(handle->config.hwnd);
    if (!::GetClientRect(hwnd, &client) || !::GetWindowRect(hwnd, &window) ||
        !::ClientToScreen(hwnd, &origin)) return SAO_STATUS_ERR_OS_CALL_FAILED;
    *out_off_x = origin.x - window.left;
    *out_off_y = origin.y - window.top;
    *out_client_w = client.right - client.left;
    *out_client_h = client.bottom - client.top;
    return SAO_STATUS_OK;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_stop(
    sao_ui_gpu_capture_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if !defined(SAO_UI_HAS_WGC)
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    try {
        std::lock_guard<std::mutex> lock(handle->backend->mutex);
        release_wgc_session_no_lock(*handle->backend);
        return SAO_STATUS_OK;
    } catch (...) {
        handle->backend->state = SAO_UI_GPU_CAPTURE_FAILED;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
#endif
}

extern "C" bool SAO_UI_CALL sao_ui_gpu_capture_supported(void) {
#if defined(SAO_UI_HAS_WGC)
    return wgc_runtime_supported();
#else
    return false;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_get_state(
    sao_ui_gpu_capture_handle_t handle, int32_t* out_state) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_state == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if !defined(SAO_UI_HAS_WGC)
    *out_state = SAO_UI_GPU_CAPTURE_UNSUPPORTED;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    std::lock_guard<std::mutex> lock(handle->backend->mutex);
    *out_state = handle->backend->state;
    return SAO_STATUS_OK;
#endif
}

// ── BGRA readback + premultiply + hash ──────────────────────────

#if defined(_WIN32)

namespace {

// Process-global staging texture cache.  Keyed by (device*, width,
// height).  Simple 4-slot LRU — the recognition tick reuses one size,
// so anything more elaborate would be over-engineering.
struct StagingSlot {
    ID3D11Device*        device = nullptr;
    ID3D11Texture2D*     staging = nullptr;
    uint32_t             width = 0;
    uint32_t             height = 0;
    uint32_t             usage_count = 0;
};

std::mutex& staging_cache_mutex() {
    static std::mutex m;
    return m;
}

constexpr uint32_t kStagingSlotCount = 4;

StagingSlot* staging_cache() {
    static StagingSlot slots[kStagingSlotCount];
    return slots;
}

// Fetch or create a staging texture matching (device, width, height).
// Caller MUST hold staging_cache_mutex().  Returned pointer is
// borrowed by the caller for one Map/Unmap round-trip; the slot
// retains ownership.
ID3D11Texture2D* acquire_staging_locked(ID3D11Device* device,
                                        uint32_t width,
                                        uint32_t height) {
    // Look for exact match first.
    StagingSlot* slots = staging_cache();
    for (uint32_t k = 0; k < kStagingSlotCount; ++k) {
        StagingSlot& s = slots[k];
        if (s.device == device && s.width == width && s.height == height &&
            s.staging != nullptr) {
            s.usage_count += 1;
            return s.staging;
        }
    }

    // Find an empty or LRU slot to reuse.
    StagingSlot* victim = nullptr;
    for (uint32_t k = 0; k < kStagingSlotCount; ++k) {
        if (slots[k].staging == nullptr) {
            victim = &slots[k];
            break;
        }
    }
    if (victim == nullptr) {
        // Evict the least-used slot (LRU by usage_count).
        victim = &slots[0];
        for (uint32_t k = 1; k < kStagingSlotCount; ++k) {
            if (slots[k].usage_count < victim->usage_count) {
                victim = &slots[k];
            }
        }
        if (victim->staging != nullptr) {
            victim->staging->Release();
            victim->staging = nullptr;
        }
        victim->device = nullptr;
        victim->width = 0;
        victim->height = 0;
        victim->usage_count = 0;
    }

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

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr) || tex == nullptr) return nullptr;

    victim->device = device;
    victim->staging = tex;
    victim->width = width;
    victim->height = height;
    victim->usage_count = 1;
    return tex;
}

// sRGB decode / encode (per-channel).  Standard IEC 61966-2-1 curve.
// Only invoked from premultiply_bgra when alpha_correct=true.
float srgb_to_linear(float u) {
    if (u <= 0.04045f) return u / 12.92f;
    return ::powf((u + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float u) {
    if (u <= 0.0031308f) return u * 12.92f;
    return 1.055f * ::powf(u, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_bgra_from_texture(
    void*     texture,
    uint32_t  width,
    uint32_t  height,
    uint8_t*  buf_out,
    uint32_t  buf_capacity,
    uint32_t* stride_out) {
    if (stride_out != nullptr) *stride_out = 0;
    if (texture == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (buf_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (width == 0 || height == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    ID3D11Texture2D* src = reinterpret_cast<ID3D11Texture2D*>(texture);

    // Validate format matches our BGRA expectation and read back the
    // source device so we can create a matching staging texture.
    D3D11_TEXTURE2D_DESC desc = {};
    src->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (desc.Width != width || desc.Height != height) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    ID3D11Device* device = nullptr;
    src->GetDevice(&device);
    if (device == nullptr) return SAO_STATUS_ERR_OS_CALL_FAILED;

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (ctx == nullptr) {
        device->Release();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    ID3D11Texture2D* staging = nullptr;
    {
        std::lock_guard<std::mutex> lk(staging_cache_mutex());
        staging = acquire_staging_locked(device, width, height);
    }
    if (staging == nullptr) {
        ctx->Release();
        device->Release();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    // GPU copy: src → staging.
    ctx->CopyResource(staging, src);

    // Map + memcpy + unmap.
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    sao_status_t rc = SAO_STATUS_OK;
    if (FAILED(hr)) {
        rc = SAO_STATUS_ERR_OS_CALL_FAILED;
    } else {
        const uint32_t pitch = mapped.RowPitch;
        if (stride_out != nullptr) *stride_out = pitch;
        const uint32_t total = pitch * height;
        if (buf_capacity < total) {
            rc = SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        } else {
            std::memcpy(buf_out, mapped.pData, total);
        }
        ctx->Unmap(staging, 0);
    }

    ctx->Release();
    device->Release();
    return rc;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_premultiply_bgra(
    uint8_t*  pixels,
    uint32_t  size,
    bool      alpha_correct) {
    if (pixels == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if ((size & 3u) != 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    const uint32_t px_count = size / 4u;
    if (alpha_correct) {
        // sRGB-linear premultiply — the branch DirectComposition +
        // WGC expect.  Decode to linear, scale by α, re-encode.
        for (uint32_t i = 0; i < px_count; ++i) {
            uint8_t* p = pixels + i * 4u;
            const uint8_t a = p[3];
            if (a == 255) continue;         // opaque — nothing to do
            if (a == 0) {                    // fully transparent
                p[0] = 0; p[1] = 0; p[2] = 0;
                continue;
            }
            const float af = static_cast<float>(a) * (1.0f / 255.0f);
            const float br = srgb_to_linear(p[0] * (1.0f / 255.0f));
            const float gr = srgb_to_linear(p[1] * (1.0f / 255.0f));
            const float rr = srgb_to_linear(p[2] * (1.0f / 255.0f));
            const float bp = linear_to_srgb(br * af);
            const float gp = linear_to_srgb(gr * af);
            const float rp = linear_to_srgb(rr * af);
            p[0] = static_cast<uint8_t>(bp * 255.0f + 0.5f);
            p[1] = static_cast<uint8_t>(gp * 255.0f + 0.5f);
            p[2] = static_cast<uint8_t>(rp * 255.0f + 0.5f);
        }
    } else {
        // Naive gamma-space premultiply — the PIL / GDI+ branch.
        //   C' = (C * α + 127) / 255
        for (uint32_t i = 0; i < px_count; ++i) {
            uint8_t* p = pixels + i * 4u;
            const uint32_t a = p[3];
            if (a == 255) continue;
            if (a == 0) {
                p[0] = 0; p[1] = 0; p[2] = 0;
                continue;
            }
            p[0] = static_cast<uint8_t>((p[0] * a + 127u) / 255u);
            p[1] = static_cast<uint8_t>((p[1] * a + 127u) / 255u);
            p[2] = static_cast<uint8_t>((p[2] * a + 127u) / 255u);
        }
    }
    return SAO_STATUS_OK;
}

// 128-bit hash: mixes a 64-bit high word and 64-bit low word with two
// independent multiplicative constants + a byte-tail finaliser.  The
// exact constants match commonly-used "xxh3-inspired" seeds; the point
// is byte-for-byte determinism, not cryptographic strength.
extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_compute_hash(
    const uint8_t* pixels,
    uint32_t       size,
    uint64_t*      hash_hi_out,
    uint64_t*      hash_lo_out) {
    if (hash_hi_out != nullptr) *hash_hi_out = 0;
    if (hash_lo_out != nullptr) *hash_lo_out = 0;
    if (pixels == nullptr && size > 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (hash_hi_out == nullptr || hash_lo_out == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    // Two independent 64-bit state words with different seeds and
    // multipliers, so the (hi, lo) pair carries substantially more
    // entropy than a single 64-bit accumulator.
    const uint64_t seed_hi = 0x9E3779B97F4A7C15ULL;
    const uint64_t seed_lo = 0xC6BC279692B5C323ULL;
    const uint64_t mul_hi  = 0x9FB21C651E98DF25ULL;
    const uint64_t mul_lo  = 0xBF58476D1CE4E5B9ULL;

    uint64_t hi = seed_hi ^ static_cast<uint64_t>(size);
    uint64_t lo = seed_lo ^ static_cast<uint64_t>(size);

    // Consume 8-byte blocks.
    uint32_t i = 0;
    while (i + 8u <= size) {
        uint64_t w = 0;
        std::memcpy(&w, pixels + i, 8);
        hi ^= w;
        hi = (hi * mul_hi) ^ (hi >> 27);
        lo += w;
        lo = (lo * mul_lo) ^ (lo >> 31);
        i += 8u;
    }
    // Tail bytes (0..7).
    if (i < size) {
        uint8_t tail[8] = {0};
        std::memcpy(tail, pixels + i, size - i);
        uint64_t w = 0;
        std::memcpy(&w, tail, 8);
        hi ^= w;
        hi *= mul_hi;
        lo += w;
        lo *= mul_lo;
    }
    // Finaliser.
    hi ^= (hi >> 33);
    hi *= 0xFF51AFD7ED558CCDULL;
    hi ^= (hi >> 33);
    lo ^= (lo >> 33);
    lo *= 0xC4CEB9FE1A85EC53ULL;
    lo ^= (lo >> 33);

    *hash_hi_out = hi;
    *hash_lo_out = lo;
    return SAO_STATUS_OK;
}

#else  // !_WIN32

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_bgra_from_texture(
    void*, uint32_t, uint32_t, uint8_t*, uint32_t, uint32_t*) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_premultiply_bgra(
    uint8_t*, uint32_t, bool) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_capture_compute_hash(
    const uint8_t*, uint32_t, uint64_t*, uint64_t*) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

#endif  // _WIN32
