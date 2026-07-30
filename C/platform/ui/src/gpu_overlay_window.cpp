#include "sao/ui/gpu_overlay_window.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct sao_ui_gpu_overlay_window_s {
    uint64_t serial = 0;
};

namespace {

std::mutex& wgl_serialize_mutex() {
    static std::mutex mutex;
    return mutex;
}

struct GpuOverlayWindow {
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    size_t active_operations = 0;
    size_t active_callbacks = 0;
    bool destroying = false;
    bool finalization_started = false;
    bool finalized = false;

    std::mutex state_mutex;
    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_layer_handle_t layer = nullptr;
    void* hwnd = nullptr;
    sao_ui_gpu_overlay_window_handle_t handle = nullptr;
    uint64_t serial = 0;
    sao_ui_layer_render_fn_t fn = nullptr;
    void* user_data = nullptr;
    std::atomic_bool binding_visible{false};
    SaoGpuOverlayWindowState state{};
};

struct GpuOverlayRegistry {
    std::mutex mutex;
    std::unordered_map<sao_ui_gpu_overlay_window_handle_t, std::shared_ptr<GpuOverlayWindow>>
        active;
    std::vector<std::unique_ptr<sao_ui_gpu_overlay_window_s>> handle_shells;
};

struct PumpTraceState {
    std::mutex mutex;
    uint32_t remaining = 0;
    uint64_t generation = 0;
    uint64_t emitted = 0;
};

GpuOverlayRegistry& gpu_overlay_registry() {
    static GpuOverlayRegistry registry;
    return registry;
}

PumpTraceState& pump_trace_state() {
    static PumpTraceState state;
    return state;
}

uint64_t next_window_serial() {
    static std::atomic<uint64_t> sequence{1};
    return sequence.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<int32_t>& create_failure_point() {
    static std::atomic<int32_t> point{0};
    return point;
}

bool create_failure_requested(int32_t point) {
    return create_failure_point().load(std::memory_order_acquire) == point;
}

void complete_finalization(const std::shared_ptr<GpuOverlayWindow>& window) noexcept {
    sao_ui_layer_destroy(window->layer);
    {
        std::lock_guard<std::mutex> lock(window->lifecycle_mutex);
        window->finalized = true;
    }
    window->lifecycle_cv.notify_all();
    auto& registry = gpu_overlay_registry();
    std::lock_guard<std::mutex> registry_lock(registry.mutex);
    const auto it = registry.active.find(window->handle);
    if (it != registry.active.end() && it->second.get() == window.get())
        registry.active.erase(it);
}

enum class LeaseKind : uint8_t {
    kOperation,
    kCallback,
};

void release_lease(const std::shared_ptr<GpuOverlayWindow>& window, LeaseKind kind) noexcept {
    bool finalize = false;
    {
        std::lock_guard<std::mutex> lock(window->lifecycle_mutex);
        size_t& count =
            kind == LeaseKind::kOperation ? window->active_operations : window->active_callbacks;
        if (count != 0)
            --count;
        if (window->destroying && window->active_operations == 0 && window->active_callbacks == 0 &&
            !window->finalization_started) {
            window->finalization_started = true;
            finalize = true;
        }
    }
    window->lifecycle_cv.notify_all();
    if (finalize)
        complete_finalization(window);
}

class WindowLease {
  public:
    WindowLease() = default;

    WindowLease(std::shared_ptr<GpuOverlayWindow> window, LeaseKind kind)
        : window_(std::move(window)), kind_(kind) {}

    ~WindowLease() {
        reset();
    }

    WindowLease(const WindowLease&) = delete;
    WindowLease& operator=(const WindowLease&) = delete;

    WindowLease(WindowLease&& other) noexcept
        : window_(std::move(other.window_)), kind_(other.kind_) {}

    WindowLease& operator=(WindowLease&& other) noexcept {
        if (this != &other) {
            reset();
            window_ = std::move(other.window_);
            kind_ = other.kind_;
        }
        return *this;
    }

    explicit operator bool() const noexcept {
        return window_ != nullptr;
    }

    GpuOverlayWindow* operator->() const noexcept {
        return window_.get();
    }

  private:
    void reset() noexcept {
        if (window_ != nullptr) {
            release_lease(window_, kind_);
            window_.reset();
        }
    }

    std::shared_ptr<GpuOverlayWindow> window_;
    LeaseKind kind_ = LeaseKind::kOperation;
};

WindowLease acquire_window_lease(sao_ui_gpu_overlay_window_handle_t handle, LeaseKind kind) {
    if (handle == nullptr)
        return {};
    auto& registry = gpu_overlay_registry();
    std::lock_guard<std::mutex> registry_lock(registry.mutex);
    const auto it = registry.active.find(handle);
    if (it == registry.active.end())
        return {};
    const auto window = it->second;
    std::lock_guard<std::mutex> lifecycle_lock(window->lifecycle_mutex);
    if (window->destroying)
        return {};
    size_t& count =
        kind == LeaseKind::kOperation ? window->active_operations : window->active_callbacks;
    ++count;
    return WindowLease(window, kind);
}

WindowLease acquire_operation(sao_ui_gpu_overlay_window_handle_t handle) {
    return acquire_window_lease(handle, LeaseKind::kOperation);
}

WindowLease acquire_callback(sao_ui_gpu_overlay_window_handle_t handle) {
    return acquire_window_lease(handle, LeaseKind::kCallback);
}

sao_ui_gpu_overlay_window_handle_t
register_window(const std::shared_ptr<GpuOverlayWindow>& window) {
    auto shell = std::make_unique<sao_ui_gpu_overlay_window_s>();
    shell->serial = window->serial;
    auto* handle = shell.get();
    auto& registry = gpu_overlay_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.active.emplace(handle, window);
    struct ActiveRegistrationRollback {
        GpuOverlayRegistry& registry;
        sao_ui_gpu_overlay_window_handle_t handle;
        bool committed = false;

        ~ActiveRegistrationRollback() {
            if (!committed)
                registry.active.erase(handle);
        }
    } rollback{registry, handle};
    registry.handle_shells.push_back(std::move(shell));
    rollback.committed = true;
    window->handle = handle;
    return handle;
}

thread_local sao_ui_gpu_overlay_window_handle_t current_callback_handle = nullptr;

class CallbackHandleScope {
  public:
    explicit CallbackHandleScope(sao_ui_gpu_overlay_window_handle_t handle)
        : previous_(current_callback_handle) {
        current_callback_handle = handle;
    }

    ~CallbackHandleScope() {
        current_callback_handle = previous_;
    }

    CallbackHandleScope(const CallbackHandleScope&) = delete;
    CallbackHandleScope& operator=(const CallbackHandleScope&) = delete;

  private:
    sao_ui_gpu_overlay_window_handle_t previous_;
};

void destroy_window_impl(sao_ui_gpu_overlay_window_handle_t handle) noexcept {
    if (handle == nullptr)
        return;

    std::shared_ptr<GpuOverlayWindow> window;
    try {
        auto& registry = gpu_overlay_registry();
        {
            std::lock_guard<std::mutex> registry_lock(registry.mutex);
            const auto it = registry.active.find(handle);
            if (it == registry.active.end())
                return;
            window = it->second;
            {
                std::lock_guard<std::mutex> lifecycle_lock(window->lifecycle_mutex);
                window->destroying = true;
            }
        }
        {
            std::lock_guard<std::mutex> state_lock(window->state_mutex);
            window->binding_visible.store(false, std::memory_order_release);
            window->state.visible = false;
            window->state.destroyed = true;
        }

        if (current_callback_handle == handle)
            return;

        bool finalize = false;
        std::unique_lock<std::mutex> lock(window->lifecycle_mutex);
        while (!window->finalized) {
            if (window->active_operations == 0 && window->active_callbacks == 0 &&
                !window->finalization_started) {
                window->finalization_started = true;
                finalize = true;
                break;
            }
            window->lifecycle_cv.wait(lock);
        }
        lock.unlock();
        if (finalize)
            complete_finalization(window);
    } catch (...) {
    }
}

void emit_pump_trace_tick() {
    uint32_t tick = 0;
    uint64_t generation = 0;
    {
        auto& state = pump_trace_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.remaining == 0)
            return;
        tick = 9u - state.remaining;
        generation = state.generation;
        --state.remaining;
        ++state.emitted;
    }

    char marker[128]{};
#if defined(_WIN32)
    const unsigned long thread_id = static_cast<unsigned long>(::GetCurrentThreadId());
#else
    const unsigned long thread_id = 0;
#endif
    std::snprintf(marker, sizeof(marker),
                  "[SAO_UI_PUMP_TRACE] generation=%llu tick=%u/8 thread=%lu\n",
                  static_cast<unsigned long long>(generation), tick, thread_id);
    std::fputs(marker, stderr);
    std::fflush(stderr);
#if defined(_WIN32)
    ::OutputDebugStringA(marker);
#endif
}

void SAO_UI_CALL gpu_overlay_render_tick(void* gl_ctx, float time_sec, void* user_data) {
    auto* handle = static_cast<sao_ui_gpu_overlay_window_handle_t>(user_data);
    WindowLease lease;
    try {
        lease = acquire_callback(handle);
    } catch (...) {
        return;
    }
    if (!lease)
        return;

    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_layer_render_fn_t render_fn = nullptr;
    void* render_user_data = nullptr;
    uint64_t serial = 0;
    {
        std::lock_guard<std::mutex> state_lock(lease->state_mutex);
        if (!lease->state.visible || !lease->binding_visible.load(std::memory_order_acquire)) {
            return;
        }
        compositor = lease->compositor;
        render_fn = lease->fn;
        render_user_data = lease->user_data;
        serial = lease->serial;
    }

    bool trace_authority = false;
    {
        auto& registry = gpu_overlay_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        uint64_t authority = 0;
        for (const auto& [candidate_handle, candidate] : registry.active) {
            (void)candidate_handle;
            if (candidate->compositor == compositor &&
                candidate->binding_visible.load(std::memory_order_acquire) &&
                (authority == 0 || candidate->serial < authority)) {
                authority = candidate->serial;
            }
        }
        trace_authority = authority == serial;
    }
    if (trace_authority)
        emit_pump_trace_tick();
    if (render_fn == nullptr)
        return;

    const CallbackHandleScope callback_scope(handle);
    render_fn(gl_ctx, time_sec, render_user_data);
}

std::string next_layer_name(const char* title_utf8) {
    static std::atomic<uint64_t> sequence{0};
    std::string name = title_utf8 == nullptr || title_utf8[0] == '\0' ? "gpu_overlay" : title_utf8;
    name += "_gpu_";
    name += std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return name;
}

} // namespace

extern "C" bool SAO_UI_CALL sao_ui_gpu_overlay_supported(void) {
    // The production path is D3D/DComp.  WGL interop is not available.
    return false;
}

extern "C" void* SAO_UI_CALL sao_ui_get_wgl_serialize_lock(void) {
    return &wgl_serialize_mutex();
}

extern "C" void SAO_UI_CALL sao_ui_wgl_serialize_lock_acquire(void* lock_handle) {
    if (lock_handle != nullptr)
        reinterpret_cast<std::mutex*>(lock_handle)->lock();
}

extern "C" void SAO_UI_CALL sao_ui_wgl_serialize_lock_release(void* lock_handle) {
    if (lock_handle != nullptr)
        reinterpret_cast<std::mutex*>(lock_handle)->unlock();
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_create(
    sao_ui_compositor_handle_t compositor, const SaoGpuOverlayWindowConfig* config,
    sao_ui_gpu_overlay_window_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (compositor == nullptr || config == nullptr || config->width <= 0 || config->height <= 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    sao_ui_layer_handle_t layer = nullptr;
    sao_ui_gpu_overlay_window_handle_t handle = nullptr;
    bool registered = false;
    try {
        const std::string name = next_layer_name(config->title_utf8);
        if (create_failure_requested(1))
            return SAO_STATUS_ERR_UNKNOWN;

        SaoLayerConfig layer_config{};
        layer_config.struct_size = sizeof(SaoLayerConfig);
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

        sao_status_t status = sao_ui_layer_create(compositor, &layer_config, &layer);
        if (status != SAO_STATUS_OK)
            return status;
        if (create_failure_requested(2)) {
            sao_ui_layer_destroy(layer);
            return SAO_STATUS_ERR_UNKNOWN;
        }

        status = sao_ui_layer_set_visible(layer, false);
        if (status != SAO_STATUS_OK) {
            sao_ui_layer_destroy(layer);
            return status;
        }
        if (create_failure_requested(3)) {
            sao_ui_layer_destroy(layer);
            return SAO_STATUS_ERR_UNKNOWN;
        }

        auto window = std::make_shared<GpuOverlayWindow>();
        window->compositor = compositor;
        window->layer = layer;
        window->hwnd = sao_ui_compositor_host_hwnd(compositor);
        window->serial = next_window_serial();
        window->fn = reinterpret_cast<sao_ui_layer_render_fn_t>(config->render_fn);
        window->user_data = config->render_fn_user_data;
        window->state.x = config->x;
        window->state.y = config->y;
        window->state.width = config->width;
        window->state.height = config->height;
        window->state.z_order = config->z_order;
        window->state.alpha = 1.0F;
        window->state.click_through = config->click_through;

        handle = register_window(window);
        registered = true;
        layer = nullptr;
        if (create_failure_requested(4)) {
            destroy_window_impl(handle);
            return SAO_STATUS_ERR_UNKNOWN;
        }

        status = sao_ui_layer_set_render_fn(window->layer, &gpu_overlay_render_tick, handle);
        if (status != SAO_STATUS_OK) {
            destroy_window_impl(handle);
            return status;
        }
        *out_handle = handle;
        return SAO_STATUS_OK;
    } catch (...) {
        if (registered)
            destroy_window_impl(handle);
        else if (layer != nullptr)
            sao_ui_layer_destroy(layer);
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL
sao_ui_gpu_overlay_window_destroy(sao_ui_gpu_overlay_window_handle_t handle) {
    destroy_window_impl(handle);
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_show(sao_ui_gpu_overlay_window_handle_t handle) {
    try {
        auto lease = acquire_operation(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->state_mutex);
        const sao_status_t status = sao_ui_layer_set_visible(lease->layer, true);
        if (status == SAO_STATUS_OK) {
            lease->state.visible = true;
            lease->binding_visible.store(true, std::memory_order_release);
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_hide(sao_ui_gpu_overlay_window_handle_t handle) {
    try {
        auto lease = acquire_operation(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->state_mutex);
        const sao_status_t status = sao_ui_layer_set_visible(lease->layer, false);
        if (status == SAO_STATUS_OK) {
            lease->binding_visible.store(false, std::memory_order_release);
            lease->state.visible = false;
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_set_geometry(sao_ui_gpu_overlay_window_handle_t handle, int32_t x,
                                       int32_t y, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_operation(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->state_mutex);
        const sao_status_t status = sao_ui_layer_set_geometry(lease->layer, x, y, width, height);
        if (status == SAO_STATUS_OK) {
            lease->state.x = x;
            lease->state.y = y;
            lease->state.width = width;
            lease->state.height = height;
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_move(sao_ui_gpu_overlay_window_handle_t handle, int32_t x, int32_t y) {
    try {
        auto lease = acquire_operation(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->state_mutex);
        const sao_status_t status = sao_ui_layer_set_position(lease->layer, x, y);
        if (status == SAO_STATUS_OK) {
            lease->state.x = x;
            lease->state.y = y;
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_set_click_through(
    sao_ui_gpu_overlay_window_handle_t handle, bool click_through) {
    try {
        auto lease = acquire_operation(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->state_mutex);
        const sao_status_t status =
            sao_ui_layer_set_input_policy(lease->layer, click_through, !click_through);
        if (status == SAO_STATUS_OK)
            lease->state.click_through = click_through;
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_set_alpha(sao_ui_gpu_overlay_window_handle_t handle, float alpha) {
    try {
        auto lease = acquire_operation(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->state_mutex);
        const sao_status_t status = sao_ui_layer_set_alpha(lease->layer, alpha);
        if (status == SAO_STATUS_OK)
            lease->state.alpha = alpha;
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_raise_to_top(sao_ui_gpu_overlay_window_handle_t handle) {
    try {
        auto lease = acquire_operation(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->state_mutex);
        const sao_status_t status =
            sao_ui_layer_set_z_order(lease->layer, std::numeric_limits<int32_t>::max());
        if (status == SAO_STATUS_OK)
            lease->state.z_order = std::numeric_limits<int32_t>::max();
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_gpu_overlay_window_set_z(sao_ui_gpu_overlay_window_handle_t handle, int32_t z) {
    try {
        auto lease = acquire_operation(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->state_mutex);
        const sao_status_t status = sao_ui_layer_set_z_order(lease->layer, z);
        if (status == SAO_STATUS_OK)
            lease->state.z_order = z;
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_ui_layer_handle_t SAO_UI_CALL
sao_ui_gpu_overlay_window_layer(sao_ui_gpu_overlay_window_handle_t handle) {
    try {
        auto lease = acquire_operation(handle);
        return lease ? lease->layer : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void* SAO_UI_CALL
sao_ui_gpu_overlay_window_hwnd(sao_ui_gpu_overlay_window_handle_t handle) {
    try {
        auto lease = acquire_operation(handle);
        return lease ? lease->hwnd : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void* SAO_UI_CALL sao_ui_gpu_overlay_window_gl_ctx(sao_ui_gpu_overlay_window_handle_t) {
    return nullptr;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gpu_overlay_window_get_state(
    sao_ui_gpu_overlay_window_handle_t handle, SaoGpuOverlayWindowState* out_state) {
    if (out_state != nullptr)
        *out_state = {};
    if (out_state == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_operation(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->state_mutex);
        *out_state = lease->state;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_arm_pump_trace(void) {
    auto& state = pump_trace_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.generation;
    state.remaining = 8;
}

extern "C" SAO_UI_API uint32_t SAO_UI_CALL sao_ui_gpu_overlay_test_pump_trace_remaining(void) {
    auto& state = pump_trace_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.remaining;
}

extern "C" SAO_UI_API uint64_t SAO_UI_CALL sao_ui_gpu_overlay_test_pump_trace_emitted(void) {
    auto& state = pump_trace_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.emitted;
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_gpu_overlay_test_set_create_failure_point(int32_t point) {
    create_failure_point().store(point, std::memory_order_release);
}
