#include "sao/engine/runtime.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>

struct sao_engine_runtime_s {
    mutable std::shared_mutex mutex;
    sao_engine_event_bus_handle_t event_bus = nullptr;
    sao_engine_state_handle_t state = nullptr;
    sao_engine_render_hook_registry_handle_t render_hooks = nullptr;
    uint64_t last_tick_ns = 0;
    std::atomic<bool> active{true};
};

namespace {

struct RuntimeDeleter {
    void operator()(sao_engine_runtime_s* runtime) const noexcept {
        if (runtime == nullptr) {
            return;
        }
        sao_engine_render_hook_registry_destroy(runtime->render_hooks);
        sao_engine_state_destroy(runtime->state);
        sao_engine_event_bus_destroy(runtime->event_bus);
        delete runtime;
    }
};

}  // namespace

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_runtime_create(
    sao_engine_runtime_handle_t* out_handle) {
    if (out_handle != nullptr) {
        *out_handle = nullptr;
    }
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::unique_ptr<sao_engine_runtime_s, RuntimeDeleter> runtime(
            new sao_engine_runtime_s());
        sao_status_t status = sao_engine_event_bus_create(0, 0.0F,
                                                          &runtime->event_bus);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        status = sao_engine_state_create(&runtime->state);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        status = sao_engine_render_hook_registry_create(
            &runtime->render_hooks);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        *out_handle = runtime.release();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_ENGINE_CALL sao_engine_runtime_destroy(
    sao_engine_runtime_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
    try {
        std::unique_lock lock(handle->mutex);
        handle->active.store(false, std::memory_order_release);
    } catch (...) {
        handle->active.store(false, std::memory_order_release);
    }
    RuntimeDeleter{}(handle);
}

extern "C" sao_engine_event_bus_handle_t SAO_ENGINE_CALL
sao_engine_runtime_event_bus(sao_engine_runtime_handle_t handle) {
    if (handle == nullptr) {
        return nullptr;
    }
    try {
        std::shared_lock lock(handle->mutex);
        return handle->active.load(std::memory_order_acquire)
                   ? handle->event_bus
                   : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" sao_engine_state_handle_t SAO_ENGINE_CALL
sao_engine_runtime_state(sao_engine_runtime_handle_t handle) {
    if (handle == nullptr) {
        return nullptr;
    }
    try {
        std::shared_lock lock(handle->mutex);
        return handle->active.load(std::memory_order_acquire) ? handle->state
                                                              : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" sao_engine_render_hook_registry_handle_t SAO_ENGINE_CALL
sao_engine_runtime_render_hooks(sao_engine_runtime_handle_t handle) {
    if (handle == nullptr) {
        return nullptr;
    }
    try {
        std::shared_lock lock(handle->mutex);
        return handle->active.load(std::memory_order_acquire)
                   ? handle->render_hooks
                   : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_runtime_tick(
    sao_engine_runtime_handle_t handle,
    uint64_t ts_ns) {
    if (handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::unique_lock lock(handle->mutex);
        if (!handle->active.load(std::memory_order_acquire)) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        if (ts_ns < handle->last_tick_ns) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        handle->last_tick_ns = ts_ns;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_runtime_render_dispatch(
    sao_engine_runtime_handle_t handle,
    const char* surface_id_utf8,
    int32_t hook_point,
    uint64_t monotonic_time_ns,
    int32_t viewport_x_px,
    int32_t viewport_y_px,
    int32_t viewport_width_px,
    int32_t viewport_height_px,
    uint32_t dispatch_flags) {
    if (handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    sao_engine_render_hook_registry_handle_t render_hooks = nullptr;
    try {
        std::shared_lock lock(handle->mutex);
        if (!handle->active.load(std::memory_order_acquire)) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        render_hooks = handle->render_hooks;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return sao_engine_render_clock_dispatch(
        render_hooks, surface_id_utf8, hook_point, monotonic_time_ns,
        viewport_x_px, viewport_y_px, viewport_width_px, viewport_height_px,
        dispatch_flags);
}
