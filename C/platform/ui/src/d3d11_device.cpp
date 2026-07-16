// SAO Auto — process-local D3D11 device and immediate-context owner.

#include "sao/ui/d3d11_device.h"

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
#endif

#include <cstdint>
#include <iterator>
#include <mutex>
#include <new>

namespace {

#if defined(_WIN32)

template <typename T>
void safe_release(T*& value) {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

#endif

}  // namespace

struct sao_ui_d3d11_device_s {
    SaoD3d11DeviceConfig config{};
    std::mutex mutex;

#if defined(_WIN32)
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_9_1;
    DWORD owner_thread = 0;
#endif

    sao_ui_d3d11_device_lost_fn_t lost_fn = nullptr;
    void* lost_user_data = nullptr;
    bool loss_notified = false;
};

#if defined(_WIN32)

namespace {

void release_device_objects(sao_ui_d3d11_device_s* handle) {
    safe_release(handle->factory);
    safe_release(handle->adapter);
    safe_release(handle->dxgi_device);
    safe_release(handle->context);
    safe_release(handle->device);
    handle->feature_level = D3D_FEATURE_LEVEL_9_1;
}

HRESULT create_native_device(IDXGIAdapter1* adapter,
                             D3D_DRIVER_TYPE driver_type,
                             UINT flags,
                             ID3D11Device** out_device,
                             D3D_FEATURE_LEVEL* out_level,
                             ID3D11DeviceContext** out_context) {
    constexpr D3D_FEATURE_LEVEL requested_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = ::D3D11CreateDevice(
        adapter, driver_type, nullptr, flags, requested_levels,
        static_cast<UINT>(std::size(requested_levels)), D3D11_SDK_VERSION,
        out_device, out_level, out_context);

    // The 11.1 request itself is rejected by older D3D runtimes. Retry
    // without it so Windows installations with a 11.0 runtime stay usable.
    if (hr == E_INVALIDARG) {
        constexpr D3D_FEATURE_LEVEL fallback_levels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        hr = ::D3D11CreateDevice(
            adapter, driver_type, nullptr, flags, fallback_levels,
            static_cast<UINT>(std::size(fallback_levels)), D3D11_SDK_VERSION,
            out_device, out_level, out_context);
    }
    return hr;
}

sao_status_t create_device_objects(sao_ui_d3d11_device_s* handle) {
    IDXGIFactory1* enumeration_factory = nullptr;
    IDXGIAdapter1* selected_adapter = nullptr;
    if (handle->config.adapter_index != 0 && !handle->config.prefer_warp) {
        HRESULT hr = ::CreateDXGIFactory1(
            __uuidof(IDXGIFactory1),
            reinterpret_cast<void**>(&enumeration_factory));
        if (FAILED(hr) || enumeration_factory == nullptr) {
            safe_release(enumeration_factory);
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        hr = enumeration_factory->EnumAdapters1(
            handle->config.adapter_index, &selected_adapter);
        safe_release(enumeration_factory);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        if (FAILED(hr) || selected_adapter == nullptr) {
            safe_release(selected_adapter);
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
    }

    const UINT base_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const UINT requested_flags = handle->config.enable_debug_layer
        ? (base_flags | D3D11_CREATE_DEVICE_DEBUG)
        : base_flags;
    const D3D_DRIVER_TYPE primary_driver = handle->config.prefer_warp
        ? D3D_DRIVER_TYPE_WARP
        : (selected_adapter != nullptr
            ? D3D_DRIVER_TYPE_UNKNOWN
            : D3D_DRIVER_TYPE_HARDWARE);
    IDXGIAdapter1* const primary_adapter = handle->config.prefer_warp
        ? nullptr
        : selected_adapter;

    HRESULT hr = create_native_device(
        primary_adapter, primary_driver, requested_flags, &handle->device,
        &handle->feature_level, &handle->context);
    if (FAILED(hr) && handle->config.enable_debug_layer) {
        safe_release(handle->context);
        safe_release(handle->device);
        hr = create_native_device(
            primary_adapter, primary_driver, base_flags, &handle->device,
            &handle->feature_level, &handle->context);
    }
    if (FAILED(hr) && !handle->config.prefer_warp && selected_adapter == nullptr) {
        safe_release(handle->context);
        safe_release(handle->device);
        hr = create_native_device(
            nullptr, D3D_DRIVER_TYPE_WARP, base_flags, &handle->device,
            &handle->feature_level, &handle->context);
    }
    safe_release(selected_adapter);
    if (FAILED(hr) || handle->device == nullptr || handle->context == nullptr) {
        release_device_objects(handle);
        return SAO_STATUS_ERR_DEVICE_LOST;
    }

    hr = handle->device->QueryInterface(
        __uuidof(IDXGIDevice),
        reinterpret_cast<void**>(&handle->dxgi_device));
    if (FAILED(hr) || handle->dxgi_device == nullptr) {
        release_device_objects(handle);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    IDXGIAdapter* base_adapter = nullptr;
    hr = handle->dxgi_device->GetAdapter(&base_adapter);
    if (SUCCEEDED(hr) && base_adapter != nullptr) {
        hr = base_adapter->QueryInterface(
            __uuidof(IDXGIAdapter1),
            reinterpret_cast<void**>(&handle->adapter));
    }
    safe_release(base_adapter);
    if (FAILED(hr) || handle->adapter == nullptr) {
        release_device_objects(handle);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    hr = handle->adapter->GetParent(
        __uuidof(IDXGIFactory2),
        reinterpret_cast<void**>(&handle->factory));
    if (FAILED(hr) || handle->factory == nullptr) {
        release_device_objects(handle);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return SAO_STATUS_OK;
}

bool is_owner_thread(const sao_ui_d3d11_device_s* handle) {
    return handle->owner_thread == ::GetCurrentThreadId();
}

}  // namespace

#endif  // _WIN32

extern "C" sao_status_t SAO_UI_CALL sao_ui_d3d11_device_create(
    const SaoD3d11DeviceConfig* config,
    sao_ui_d3d11_device_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;

#if !defined(_WIN32)
    (void)config;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    auto* handle = new (std::nothrow) sao_ui_d3d11_device_s{};
    if (handle == nullptr) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    if (config != nullptr) {
        handle->config = *config;
    }
    handle->owner_thread = ::GetCurrentThreadId();

    const sao_status_t status = create_device_objects(handle);
    if (status != SAO_STATUS_OK) {
        delete handle;
        return status;
    }
    *out_handle = handle;
    return SAO_STATUS_OK;
#endif
}

extern "C" void SAO_UI_CALL sao_ui_d3d11_device_destroy(
    sao_ui_d3d11_device_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
#if defined(_WIN32)
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        release_device_objects(handle);
    }
#endif
    delete handle;
}

extern "C" void* SAO_UI_CALL sao_ui_d3d11_device_ptr(
    sao_ui_d3d11_device_handle_t handle) {
    if (handle == nullptr) {
        return nullptr;
    }
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(handle->mutex);
    return handle->device;
#else
    return nullptr;
#endif
}

extern "C" void* SAO_UI_CALL sao_ui_d3d11_device_context_ptr(
    sao_ui_d3d11_device_handle_t handle) {
    if (handle == nullptr) {
        return nullptr;
    }
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(handle->mutex);
    return handle->context;
#else
    return nullptr;
#endif
}

extern "C" void* SAO_UI_CALL sao_ui_d3d11_device_dxgi_factory(
    sao_ui_d3d11_device_handle_t handle) {
    if (handle == nullptr) {
        return nullptr;
    }
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(handle->mutex);
    return handle->factory;
#else
    return nullptr;
#endif
}

extern "C" void* SAO_UI_CALL sao_ui_d3d11_device_dxgi_adapter(
    sao_ui_d3d11_device_handle_t handle) {
    if (handle == nullptr) {
        return nullptr;
    }
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(handle->mutex);
    return handle->adapter;
#else
    return nullptr;
#endif
}

extern "C" uint32_t SAO_UI_CALL sao_ui_d3d11_device_feature_level(
    sao_ui_d3d11_device_handle_t handle) {
    if (handle == nullptr) {
        return 0;
    }
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(handle->mutex);
    return static_cast<uint32_t>(handle->feature_level);
#else
    return 0;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_d3d11_device_recreate(
    sao_ui_d3d11_device_handle_t handle,
    uint32_t* out_last_removed_reason) {
    if (out_last_removed_reason != nullptr) {
        *out_last_removed_reason = 0;
    }
    if (handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
#if !defined(_WIN32)
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    if (!is_owner_thread(handle)) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }

    std::lock_guard<std::mutex> lock(handle->mutex);
    if (handle->device != nullptr) {
        const HRESULT reason = handle->device->GetDeviceRemovedReason();
        if (out_last_removed_reason != nullptr) {
            *out_last_removed_reason = static_cast<uint32_t>(reason);
        }
    }
    release_device_objects(handle);
    handle->loss_notified = false;
    return create_device_objects(handle);
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_d3d11_device_check_alive(
    sao_ui_d3d11_device_handle_t handle) {
    if (handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
#if !defined(_WIN32)
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    sao_ui_d3d11_device_lost_fn_t notify = nullptr;
    void* notify_user_data = nullptr;
    uint32_t reason = 0;
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (handle->device == nullptr) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        const HRESULT hr = handle->device->GetDeviceRemovedReason();
        if (SUCCEEDED(hr)) {
            return SAO_STATUS_OK;
        }
        reason = static_cast<uint32_t>(hr);
        if (!handle->loss_notified) {
            handle->loss_notified = true;
            notify = handle->lost_fn;
            notify_user_data = handle->lost_user_data;
        }
    }
    if (notify != nullptr) {
        notify(reason, notify_user_data);
    }
    return SAO_STATUS_ERR_DEVICE_LOST;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_d3d11_device_on_lost(
    sao_ui_d3d11_device_handle_t handle,
    sao_ui_d3d11_device_lost_fn_t fn,
    void* user_data) {
    if (handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    handle->lost_fn = fn;
    handle->lost_user_data = user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_d3d11_device_get_state(
    sao_ui_d3d11_device_handle_t handle,
    SaoD3d11DeviceState* out_state) {
    if (out_state != nullptr) *out_state = {};
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_state == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    out_state->adapter_index = handle->config.adapter_index;
    out_state->loss_notified = handle->loss_notified;
#if defined(_WIN32)
    out_state->feature_level = static_cast<uint32_t>(handle->feature_level);
    out_state->owner_thread_id = handle->owner_thread;
    out_state->device_ready = handle->device != nullptr;
    out_state->context_ready = handle->context != nullptr;
    out_state->factory_ready = handle->factory != nullptr;
    out_state->adapter_ready = handle->adapter != nullptr;
    if (handle->device != nullptr) {
        out_state->last_removed_reason = static_cast<uint32_t>(
            handle->device->GetDeviceRemovedReason());
    }
#endif
    return SAO_STATUS_OK;
}
