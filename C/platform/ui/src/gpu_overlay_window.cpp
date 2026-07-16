#include "sao/ui/gpu_overlay_window.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <string>

struct sao_ui_gpu_overlay_window_s {
    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_layer_handle_t layer = nullptr;
    void* hwnd = nullptr;
    bool destroyed = false;
    SaoGpuOverlayWindowState state{};
};

namespace {

std::mutex& wgl_serialize_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::string next_layer_name(const char* title_utf8) {
    static std::atomic<uint64_t> sequence{0};
    std::string name = title_utf8 == nullptr || title_utf8[0] == '\0' ? "gpu_overlay" : title_utf8;
    name += "_gpu_";
    name += std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return name;
}

sao_status_t validate(sao_ui_gpu_overlay_window_handle_t handle) {
    return handle == nullptr || handle->destroyed ? SAO_STATUS_ERR_HANDLE_INVALID : SAO_STATUS_OK;
}

}  // namespace

extern "C" bool SAO_UI_CALL sao_ui_gpu_overlay_supported(void) {
    // The production path is D3D/DComp.  WGL interop is not available.
    return false;
}

extern "C" void* SAO_UI_CALL sao_ui_get_wgl_serialize_lock(void) {
    return &wgl_serialize_mutex();
}

extern "C" void SAO_UI_CALL sao_ui_wgl_serialize_lock_acquire(void* lock_handle) {
    if (lock_handle != nullptr) reinterpret_cast<std::mutex*>(lock_handle)->lock();
}

extern "C" void SAO_UI_CALL sao_ui_wgl_serialize_lock_release(void* lock_handle) {
    if (lock_handle != nullptr) reinterpret_cast<std::mutex*>(lock_handle)->unlock();
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_create(
    sao_ui_compositor_handle_t compositor, const SaoGpuOverlayWindowConfig* config,
    sao_ui_gpu_overlay_window_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (compositor == nullptr || config == nullptr || config->width <= 0 || config->height <= 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    SaoLayerConfig layer_config{};
    const std::string name = next_layer_name(config->title_utf8);
    layer_config.name_utf8 = name.c_str();
    layer_config.x = config->x;
    layer_config.y = config->y;
    layer_config.width = config->width;
    layer_config.height = config->height;
    layer_config.z_order = config->z_order;
    layer_config.click_through = config->click_through;
    layer_config.rect_hit = !config->click_through;
    layer_config.bgra_swizzle = true;
    layer_config.target_fps = config->vsync ? 0 : 60;

    sao_ui_layer_handle_t layer = nullptr;
    sao_status_t status = sao_ui_layer_create(compositor, &layer_config, &layer);
    if (status != SAO_STATUS_OK) return status;
    status = sao_ui_layer_set_visible(layer, false);
    if (status != SAO_STATUS_OK) {
        sao_ui_layer_destroy(layer);
        return status;
    }
    if (config->render_fn != nullptr) {
        status = sao_ui_layer_set_render_fn(
            layer,
            reinterpret_cast<sao_ui_layer_render_fn_t>(config->render_fn),
            config->render_fn_user_data);
        if (status != SAO_STATUS_OK) {
            sao_ui_layer_destroy(layer);
            return status;
        }
    }

    auto* window = new (std::nothrow) sao_ui_gpu_overlay_window_s();
    if (window == nullptr) {
        sao_ui_layer_destroy(layer);
        return SAO_STATUS_ERR_UNKNOWN;
    }
    window->compositor = compositor;
    window->layer = layer;
    window->hwnd = sao_ui_compositor_host_hwnd(compositor);
    window->state.x = config->x;
    window->state.y = config->y;
    window->state.width = config->width;
    window->state.height = config->height;
    window->state.z_order = config->z_order;
    window->state.alpha = 1.0F;
    window->state.click_through = config->click_through;
    *out_handle = window;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_gpu_overlay_window_destroy(
    sao_ui_gpu_overlay_window_handle_t handle) {
    if (handle == nullptr) return;
    if (!handle->destroyed) {
        sao_ui_layer_destroy(handle->layer);
        handle->layer = nullptr;
        handle->destroyed = true;
        handle->state.destroyed = true;
    }
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_show(
    sao_ui_gpu_overlay_window_handle_t handle) {
    const sao_status_t status = validate(handle);
    if (status != SAO_STATUS_OK) return status;
    const sao_status_t set_status = sao_ui_layer_set_visible(handle->layer, true);
    if (set_status == SAO_STATUS_OK) handle->state.visible = true;
    return set_status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_hide(
    sao_ui_gpu_overlay_window_handle_t handle) {
    const sao_status_t status = validate(handle);
    if (status != SAO_STATUS_OK) return status;
    const sao_status_t set_status = sao_ui_layer_set_visible(handle->layer, false);
    if (set_status == SAO_STATUS_OK) handle->state.visible = false;
    return set_status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_set_geometry(
    sao_ui_gpu_overlay_window_handle_t handle, int32_t x, int32_t y, int32_t width, int32_t height) {
    const sao_status_t status = validate(handle);
    if (status != SAO_STATUS_OK) return status;
    if (width <= 0 || height <= 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const sao_status_t set_status = sao_ui_layer_set_geometry(handle->layer, x, y, width, height);
    if (set_status == SAO_STATUS_OK) {
        handle->state.x = x;
        handle->state.y = y;
        handle->state.width = width;
        handle->state.height = height;
    }
    return set_status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_move(
    sao_ui_gpu_overlay_window_handle_t handle, int32_t x, int32_t y) {
    const sao_status_t status = validate(handle);
    if (status != SAO_STATUS_OK) return status;
    const sao_status_t set_status = sao_ui_layer_set_position(handle->layer, x, y);
    if (set_status == SAO_STATUS_OK) {
        handle->state.x = x;
        handle->state.y = y;
    }
    return set_status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_set_click_through(
    sao_ui_gpu_overlay_window_handle_t handle, bool click_through) {
    const sao_status_t status = validate(handle);
    if (status != SAO_STATUS_OK) return status;
    const sao_status_t set_status = sao_ui_layer_set_input_enabled(handle->layer, !click_through);
    if (set_status == SAO_STATUS_OK) handle->state.click_through = click_through;
    return set_status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_set_alpha(
    sao_ui_gpu_overlay_window_handle_t handle, float alpha) {
    const sao_status_t status = validate(handle);
    if (status != SAO_STATUS_OK) return status;
    const sao_status_t set_status = sao_ui_layer_set_alpha(handle->layer, alpha);
    if (set_status == SAO_STATUS_OK) handle->state.alpha = alpha;
    return set_status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_raise_to_top(
    sao_ui_gpu_overlay_window_handle_t handle) {
    const sao_status_t status = validate(handle);
    if (status != SAO_STATUS_OK) return status;
    const sao_status_t set_status = sao_ui_layer_set_z_order(
        handle->layer, std::numeric_limits<int32_t>::max());
    if (set_status == SAO_STATUS_OK) {
        handle->state.z_order = std::numeric_limits<int32_t>::max();
    }
    return set_status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_set_z(
    sao_ui_gpu_overlay_window_handle_t handle, int32_t z) {
    const sao_status_t status = validate(handle);
    if (status != SAO_STATUS_OK) return status;
    const sao_status_t set_status = sao_ui_layer_set_z_order(handle->layer, z);
    if (set_status == SAO_STATUS_OK) handle->state.z_order = z;
    return set_status;
}

extern "C" sao_ui_layer_handle_t SAO_UI_CALL sao_ui_gpu_overlay_window_layer(
    sao_ui_gpu_overlay_window_handle_t handle) {
    return validate(handle) == SAO_STATUS_OK ? handle->layer : nullptr;
}

extern "C" void* SAO_UI_CALL sao_ui_gpu_overlay_window_hwnd(
    sao_ui_gpu_overlay_window_handle_t handle) {
    return validate(handle) == SAO_STATUS_OK ? handle->hwnd : nullptr;
}

extern "C" void* SAO_UI_CALL sao_ui_gpu_overlay_window_gl_ctx(
    sao_ui_gpu_overlay_window_handle_t) {
    return nullptr;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_get_state(
    sao_ui_gpu_overlay_window_handle_t handle,
    SaoGpuOverlayWindowState* out_state) {
    if (out_state != nullptr) *out_state = {};
    const sao_status_t status = validate(handle);
    if (status != SAO_STATUS_OK) return status;
    if (out_state == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_state = handle->state;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_arm_pump_trace(void) {}
