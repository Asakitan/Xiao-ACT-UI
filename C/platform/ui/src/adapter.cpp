// SAO Auto — legacy overlay adapter backed by compositor layers.
//
// 1:1 with `sao_auto/python/render/overlay_adapter.py` (317 lines).
//
// Python authoritative behaviour:
//   * `CompositorOverlayWindow` wraps a `CompositorLayer` with the
//     legacy `GpuOverlayWindow` API.  A monotonic ID counter
//     (`_next_layer_id`, guarded by `_id_lock`) uniquifies layer
//     names — `<title>_<n>`.
//   * show() / hide() drive layer visibility, sync input proxy state,
//     and call `compositor.sync_host_input_mode()` — the second call
//     silently swallows exceptions so it never blocks the primary
//     visibility change.
//   * destroy() first tries to detach the input proxy (swallowing
//     exceptions so a failed detach doesn't leak the layer), then
//     destroys the layer, then syncs host input mode.
//   * set_geometry(x, y, w, h) mutates local cache + delegates.
//   * set_click_through(bool) mutates layer.click_through and syncs
//     host input mode.
//   * set_alpha(float) sets layer.alpha and requests a redraw.
//   * raise_to_top() / set_z(int) delegate to compositor.
//   * `CompositorBgraPresenter` stages a BGRA frame; render is a no-op
//     because the master compositor pass does the drawing.
//
// The C++ port delegates the production path to an owned compositor
// layer while retaining an explicitly reported record-only fixture
// when tests pass a null compositor.
// This file is frozen compatibility surface. No new rendering, input,
// theme, capture, or lifecycle authority is added here.

#include "sao/ui/adapter.h"
#include "sao/ui/legacy_webview.h"
#include "sao/ui/scheduler.h"

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" sao_status_t SAO_UI_CALL
sao_ui_compositor_require_owner_thread(sao_ui_compositor_handle_t compositor);
extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_input_policy(
    sao_ui_layer_handle_t layer, bool click_through, bool input_enabled);
extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_disable_input_proxy(
    sao_ui_layer_handle_t layer);

namespace {

// Process-wide monotonic ID counter — the C++ analogue of Python's
// `_next_layer_id` + `_id_lock`.  Keeps the same title_1 / title_2
// naming for callers that inspect layer names.
std::atomic<uint64_t> g_next_layer_id{0};
std::atomic<int32_t> g_next_raised_z{1'000'000};
constexpr sao_status_t kStatusBusy = -102;

enum class HostSyncPhase : size_t {
    region = 0,
    input = 1,
    window = 2,
    count = 3,
};

std::string gen_layer_name(const char* title_utf8) {
    const uint64_t n = g_next_layer_id.fetch_add(1u) + 1u;
    std::string title = (title_utf8 != nullptr && title_utf8[0] != '\0') ? std::string(title_utf8)
                                                                         : std::string("layer");
    title += "_";
    title += std::to_string(n);
    return title;
}

// ── CompositorOverlayWindow ─────────────────────────────────
struct OverlayWindow {
    sao_ui_compositor_handle_t compositor;
    sao_ui_layer_handle_t layer = nullptr;
    SaoUiCompositorOverlayBackingState backing = SAO_UI_COMPOSITOR_OVERLAY_BACKING_FIXTURE;

    // Local snapshot of the layer state — populated by the setters.
    // Mirrors `_w / _h / _x / _y / _click_through / _visible /
    // _destroyed` in the Python class.
    std::string name;
    std::string title;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    bool click_through = true;
    bool vsync = false;
    int32_t target_fps = 0;
    int32_t z_order = 100;
    float alpha = 1.0f;
    bool visible = false;
    bool destroyed = false;
    bool input_proxy_attached = false;
    bool layer_visible = false;
    bool layer_click_through = true;
    bool layer_input_enabled = false;
    bool degraded = false;
    sao_status_t degraded_status = SAO_STATUS_OK;
    sao_ui_layer_render_fn_t render_fn{};
    void* render_user_data{};
    sao_ui_layer_cursor_pos_fn_t cursor_pos_fn{};
    sao_ui_layer_cursor_leave_fn_t cursor_leave_fn{};
    sao_ui_layer_button_fn_t button_fn{};
    sao_ui_layer_scroll_fn_t scroll_fn{};
    void* input_user_data{};
    uint64_t input_generation{1};
    std::array<std::deque<sao_status_t>, static_cast<size_t>(HostSyncPhase::count)>
        host_sync_failures;

    // Diagnostic call log — one entry per public setter invocation.
    // Preserves the Python "did we actually call sync_host_input_mode"
    // semantics for parity tests.
    std::mutex mu;
    std::deque<std::string> call_log;
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    size_t operations_in_flight{};
    size_t callbacks_in_flight{};
    std::unordered_map<uint64_t, size_t> input_callbacks_by_generation;
    bool accepting_operations{true};
    bool destroy_requested{};
    bool finalizing{};
    bool finalized{};

    void log(const std::string& s) noexcept {
        try {
            std::lock_guard<std::mutex> guard(mu);
            call_log.push_back(s);
        } catch (...) {
        }
    }

    ~OverlayWindow() {
        if (layer != nullptr) {
            (void)sao_ui_layer_set_input_enabled(layer, false);
            sao_ui_layer_destroy(layer);
        }
    }
};

// ── CompositorBgraPresenter ─────────────────────────────────
struct BgraPresenter {
    sao_ui_layer_handle_t layer;

    // Test/diagnostic mirror only. The compositor layer is the sole
    // production frame source after a successful set_frame call.
    std::vector<uint8_t> frame;
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
    int32_t x = 0;
    int32_t y = 0;
    float alpha = 1.0f;
    uint64_t fade_generation{};
    std::mutex mu;
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    size_t operations_in_flight{};
    bool accepting_operations{true};
    bool destroy_requested{};
    bool finalized{};
};

std::mutex& adapter_registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<OverlayWindow*>& live_windows() {
    static std::unordered_set<OverlayWindow*> values;
    return values;
}

std::vector<std::unique_ptr<OverlayWindow>>& window_storage() {
    static std::vector<std::unique_ptr<OverlayWindow>> values;
    return values;
}

std::unordered_set<BgraPresenter*>& live_presenters() {
    static std::unordered_set<BgraPresenter*> values;
    return values;
}

std::vector<std::unique_ptr<BgraPresenter>>& presenter_storage() {
    static std::vector<std::unique_ptr<BgraPresenter>> values;
    return values;
}

struct ActiveAdapterCallback {
    OverlayWindow* window{};
    uint64_t generation{};
    ActiveAdapterCallback* previous{};
};

thread_local ActiveAdapterCallback* active_adapter_callback = nullptr;

sao_status_t require_host_owner(OverlayWindow* window) noexcept {
    if (window->compositor == nullptr || sao_ui_compositor_host_hwnd(window->compositor) == nullptr)
        return SAO_STATUS_OK;
    return sao_ui_compositor_require_owner_thread(window->compositor);
}

sao_status_t consume_host_sync_failure(OverlayWindow* window, HostSyncPhase phase) noexcept {
    try {
        std::lock_guard lock(window->mu);
        auto& failures = window->host_sync_failures[static_cast<size_t>(phase)];
        if (failures.empty())
            return SAO_STATUS_OK;
        const sao_status_t status = failures.front();
        failures.pop_front();
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t sync_host(OverlayWindow* window, bool region, bool input, bool lift) noexcept {
    if (window->compositor == nullptr || sao_ui_compositor_host_hwnd(window->compositor) == nullptr)
        return SAO_STATUS_OK;
    sao_status_t status = SAO_STATUS_OK;
    if (region) {
        status = consume_host_sync_failure(window, HostSyncPhase::region);
        if (status == SAO_STATUS_OK)
            status = sao_ui_compositor_sync_host_rgn(window->compositor);
    }
    if (status == SAO_STATUS_OK && input) {
        status = consume_host_sync_failure(window, HostSyncPhase::input);
        if (status == SAO_STATUS_OK)
            status = sao_ui_compositor_sync_host_input_mode(window->compositor);
    }
    if (status == SAO_STATUS_OK && lift) {
        status = consume_host_sync_failure(window, HostSyncPhase::window);
        if (status == SAO_STATUS_OK)
            status = sao_ui_compositor_lift_input_proxies(window->compositor);
    }
    return status;
}

struct OverlayPolicyState {
    bool local_visible{};
    bool local_click_through{true};
    bool layer_visible{};
    bool layer_click_through{true};
    bool layer_input_enabled{};
    bool input_proxy_attached{};
    bool lift{};
};

OverlayPolicyState snapshot_policy(OverlayWindow* window) {
    std::lock_guard lock(window->mu);
    return {
        window->visible,
        window->click_through,
        window->layer_visible,
        window->layer_click_through,
        window->layer_input_enabled,
        window->input_proxy_attached,
        window->input_proxy_attached,
    };
}

sao_status_t apply_layer_policy(OverlayWindow* window, const OverlayPolicyState& policy) {
    sao_ui_layer_handle_t layer = nullptr;
    {
        std::lock_guard lock(window->mu);
        layer = window->layer;
    }
    if (layer == nullptr) {
        std::lock_guard lock(window->mu);
        window->layer_visible = policy.layer_visible;
        window->layer_click_through = policy.layer_click_through;
        window->layer_input_enabled = policy.layer_input_enabled;
        window->input_proxy_attached = policy.input_proxy_attached;
        return SAO_STATUS_OK;
    }

    sao_status_t status = policy.input_proxy_attached
        ? sao_ui_layer_enable_input_proxy(layer)
        : sao_ui_layer_disable_input_proxy(layer);
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_input_policy(
            layer, policy.layer_click_through, policy.layer_input_enabled);
    }
    if (status != SAO_STATUS_OK)
        return status;
    {
        std::lock_guard lock(window->mu);
        window->layer_click_through = policy.layer_click_through;
        window->layer_input_enabled = policy.layer_input_enabled;
        window->input_proxy_attached = policy.input_proxy_attached;
    }
    status = sao_ui_layer_set_visible(layer, policy.layer_visible);
    if (status != SAO_STATUS_OK)
        return status;
    {
        std::lock_guard lock(window->mu);
        window->layer_visible = policy.layer_visible;
    }
    return SAO_STATUS_OK;
}

void apply_local_policy(OverlayWindow* window, const OverlayPolicyState& policy) {
    std::lock_guard lock(window->mu);
    window->visible = policy.local_visible;
    window->click_through = policy.local_click_through;
}

void set_degraded(OverlayWindow* window, sao_status_t status) noexcept {
    try {
        std::lock_guard lock(window->mu);
        window->degraded = status != SAO_STATUS_OK;
        window->degraded_status = status;
    } catch (...) {
    }
}

sao_status_t apply_policy_transaction(OverlayWindow* window, const OverlayPolicyState& previous,
                                      const OverlayPolicyState& target) {
    sao_status_t status = apply_layer_policy(window, target);
    if (status == SAO_STATUS_OK) {
        apply_local_policy(window, target);
        status = sync_host(window, true, true, target.lift);
    }
    if (status == SAO_STATUS_OK) {
        set_degraded(window, SAO_STATUS_OK);
        return SAO_STATUS_OK;
    }

    sao_status_t rollback_status = apply_layer_policy(window, previous);
    apply_local_policy(window, previous);
    const sao_status_t host_rollback_status =
        sync_host(window, true, true, previous.lift);
    if (rollback_status == SAO_STATUS_OK)
        rollback_status = host_rollback_status;
    if (rollback_status != SAO_STATUS_OK) {
        set_degraded(window, rollback_status);
        return SAO_STATUS_ERR_SURFACE_INVALID;
    }
    set_degraded(window, SAO_STATUS_OK);
    return status;
}

bool claim_window_finalization(OverlayWindow* window) {
    std::lock_guard lock(window->lifecycle_mutex);
    if (!window->destroy_requested || window->finalizing || window->finalized ||
        window->operations_in_flight != 0 || window->callbacks_in_flight != 0) {
        return false;
    }
    window->finalizing = true;
    return true;
}

void finalize_window(OverlayWindow* window) noexcept {
    try {
        sao_ui_layer_handle_t layer = nullptr;
        {
            std::lock_guard lock(window->mu);
            layer = std::exchange(window->layer, nullptr);
            window->visible = false;
            window->destroyed = true;
            window->backing = SAO_UI_COMPOSITOR_OVERLAY_BACKING_DESTROYED;
            window->cursor_pos_fn = nullptr;
            window->cursor_leave_fn = nullptr;
            window->button_fn = nullptr;
            window->scroll_fn = nullptr;
            window->render_fn = nullptr;
        }
        if (layer != nullptr) {
            (void)sao_ui_layer_set_render_fn(layer, nullptr, nullptr);
            (void)sao_ui_layer_set_input_callbacks(layer, nullptr, nullptr, nullptr, nullptr,
                                                   nullptr);
            (void)sao_ui_layer_set_input_enabled(layer, false);
            sao_ui_layer_destroy(layer);
        }
        (void)sync_host(window, true, true, false);
        window->log("destroy");
    } catch (...) {
    }
    {
        std::lock_guard lock(window->lifecycle_mutex);
        window->finalizing = false;
        window->finalized = true;
    }
    window->lifecycle_cv.notify_all();
}

void finalize_window_if_ready(OverlayWindow* window) noexcept {
    if (claim_window_finalization(window))
        finalize_window(window);
}

class WindowOperation {
  public:
    explicit WindowOperation(OverlayWindow* window) : window_(window) {
        if (window_ == nullptr)
            return;
        try {
            std::lock_guard registry_lock(adapter_registry_mutex());
            if (!live_windows().contains(window_))
                return;
            std::lock_guard lifecycle_lock(window_->lifecycle_mutex);
            if (!window_->accepting_operations)
                return;
            ++window_->operations_in_flight;
            acquired_ = true;
        } catch (...) {
        }
    }

    ~WindowOperation() {
        if (!acquired_)
            return;
        {
            std::lock_guard lock(window_->lifecycle_mutex);
            --window_->operations_in_flight;
        }
        window_->lifecycle_cv.notify_all();
        finalize_window_if_ready(window_);
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

  private:
    OverlayWindow* window_{};
    bool acquired_{};
};

class WindowCallbackLease {
  public:
    WindowCallbackLease(OverlayWindow* window, bool input) : window_(window), input_(input) {
        try {
            std::lock_guard lifecycle_lock(window_->lifecycle_mutex);
            if (!window_->accepting_operations)
                return;
            {
                std::lock_guard state_lock(window_->mu);
                generation_ = input_ ? window_->input_generation : 0;
                render_fn_ = window_->render_fn;
                render_user_data_ = window_->render_user_data;
                cursor_pos_fn_ = window_->cursor_pos_fn;
                cursor_leave_fn_ = window_->cursor_leave_fn;
                button_fn_ = window_->button_fn;
                scroll_fn_ = window_->scroll_fn;
                input_user_data_ = window_->input_user_data;
            }
            ++window_->input_callbacks_by_generation[generation_];
            ++window_->callbacks_in_flight;
            marker_ = {window_, generation_, active_adapter_callback};
            active_adapter_callback = &marker_;
            acquired_ = true;
        } catch (...) {
        }
    }

    ~WindowCallbackLease() {
        if (!acquired_)
            return;
        active_adapter_callback = marker_.previous;
        {
            std::lock_guard lock(window_->lifecycle_mutex);
            auto found = window_->input_callbacks_by_generation.find(generation_);
            if (found != window_->input_callbacks_by_generation.end() && --found->second == 0)
                window_->input_callbacks_by_generation.erase(found);
            --window_->callbacks_in_flight;
        }
        window_->lifecycle_cv.notify_all();
        finalize_window_if_ready(window_);
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }
    sao_ui_layer_render_fn_t render_fn() const noexcept {
        return render_fn_;
    }
    void* render_user_data() const noexcept {
        return render_user_data_;
    }
    sao_ui_layer_cursor_pos_fn_t cursor_pos_fn() const noexcept {
        return cursor_pos_fn_;
    }
    sao_ui_layer_cursor_leave_fn_t cursor_leave_fn() const noexcept {
        return cursor_leave_fn_;
    }
    sao_ui_layer_button_fn_t button_fn() const noexcept {
        return button_fn_;
    }
    sao_ui_layer_scroll_fn_t scroll_fn() const noexcept {
        return scroll_fn_;
    }
    void* input_user_data() const noexcept {
        return input_user_data_;
    }

  private:
    OverlayWindow* window_{};
    bool input_{};
    uint64_t generation_{};
    sao_ui_layer_render_fn_t render_fn_{};
    void* render_user_data_{};
    sao_ui_layer_cursor_pos_fn_t cursor_pos_fn_{};
    sao_ui_layer_cursor_leave_fn_t cursor_leave_fn_{};
    sao_ui_layer_button_fn_t button_fn_{};
    sao_ui_layer_scroll_fn_t scroll_fn_{};
    void* input_user_data_{};
    ActiveAdapterCallback marker_{};
    bool acquired_{};
};

void wait_input_generation(OverlayWindow* window, uint64_t generation) {
    for (const ActiveAdapterCallback* active = active_adapter_callback; active != nullptr;
         active = active->previous) {
        if (active->window == window && active->generation == generation)
            return;
    }
    std::unique_lock lock(window->lifecycle_mutex);
    window->lifecycle_cv.wait(
        lock, [&] { return !window->input_callbacks_by_generation.contains(generation); });
}

void SAO_UI_CALL adapter_render_callback(void* context, float time_sec, void* user_data) {
    try {
        auto* window = static_cast<OverlayWindow*>(user_data);
        WindowCallbackLease callback(window, false);
        if (!callback || callback.render_fn() == nullptr)
            return;
        callback.render_fn()(context, time_sec, callback.render_user_data());
    } catch (...) {
    }
}

void SAO_UI_CALL adapter_cursor_callback(float x, float y, void* user_data) {
    try {
        auto* window = static_cast<OverlayWindow*>(user_data);
        WindowCallbackLease callback(window, true);
        if (callback && callback.cursor_pos_fn() != nullptr)
            callback.cursor_pos_fn()(x, y, callback.input_user_data());
    } catch (...) {
    }
}

void SAO_UI_CALL adapter_cursor_leave_callback(void* user_data) {
    try {
        auto* window = static_cast<OverlayWindow*>(user_data);
        WindowCallbackLease callback(window, true);
        if (callback && callback.cursor_leave_fn() != nullptr)
            callback.cursor_leave_fn()(callback.input_user_data());
    } catch (...) {
    }
}

void SAO_UI_CALL adapter_button_callback(int32_t button, int32_t action, int32_t mods, float x,
                                         float y, void* user_data) {
    try {
        auto* window = static_cast<OverlayWindow*>(user_data);
        WindowCallbackLease callback(window, true);
        if (callback && callback.button_fn() != nullptr)
            callback.button_fn()(button, action, mods, x, y, callback.input_user_data());
    } catch (...) {
    }
}

void SAO_UI_CALL adapter_scroll_callback(float dx, float dy, void* user_data) {
    try {
        auto* window = static_cast<OverlayWindow*>(user_data);
        WindowCallbackLease callback(window, true);
        if (callback && callback.scroll_fn() != nullptr)
            callback.scroll_fn()(dx, dy, callback.input_user_data());
    } catch (...) {
    }
}

class PresenterOperation {
  public:
    explicit PresenterOperation(BgraPresenter* presenter) : presenter_(presenter) {
        if (presenter_ == nullptr)
            return;
        try {
            std::lock_guard registry_lock(adapter_registry_mutex());
            if (!live_presenters().contains(presenter_))
                return;
            std::lock_guard lifecycle_lock(presenter_->lifecycle_mutex);
            if (!presenter_->accepting_operations)
                return;
            ++presenter_->operations_in_flight;
            acquired_ = true;
        } catch (...) {
        }
    }

    ~PresenterOperation() {
        if (!acquired_)
            return;
        {
            std::lock_guard lock(presenter_->lifecycle_mutex);
            --presenter_->operations_in_flight;
            if (presenter_->destroy_requested && presenter_->operations_in_flight == 0)
                presenter_->finalized = true;
        }
        presenter_->lifecycle_cv.notify_all();
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

  private:
    BgraPresenter* presenter_{};
    bool acquired_{};
};

} // namespace

// ── OverlayWindow ABI ───────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_create(
    sao_ui_compositor_handle_t compositor, const SaoGpuOverlayWindowConfig* config,
    sao_ui_compositor_overlay_window_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (!sao_ui_legacy_compat_enabled())
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    if (config == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (config->width <= 0 || config->height <= 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        auto win = std::make_unique<OverlayWindow>();
        win->compositor = compositor;
        win->title =
            (config->title_utf8 != nullptr) ? std::string(config->title_utf8) : std::string();
        win->name = gen_layer_name(config->title_utf8);
        win->x = config->x;
        win->y = config->y;
        win->width = config->width;
        win->height = config->height;
        win->click_through = config->click_through;
        win->layer_click_through = config->click_through;
        win->vsync = config->vsync;
        win->z_order = config->z_order;
        win->render_fn = reinterpret_cast<sao_ui_layer_render_fn_t>(config->render_fn);
        win->render_user_data = config->render_fn_user_data;
        if (compositor != nullptr) {
            SaoLayerConfig layer_config{};
            layer_config.struct_size = sizeof(SaoLayerConfig);
            layer_config.name_utf8 = win->name.c_str();
            layer_config.x = win->x;
            layer_config.y = win->y;
            layer_config.width = win->width;
            layer_config.height = win->height;
            layer_config.z_order = win->z_order;
            layer_config.click_through = win->click_through;
            layer_config.rect_hit = true;
            layer_config.bgra_swizzle = true;
            if (config->vsync) {
                const int32_t refresh_hz = sao_ui_scheduler_detect_refresh_hz();
                layer_config.target_fps = refresh_hz > 0 ? refresh_hz : 60;
                win->target_fps = layer_config.target_fps;
            }
            sao_status_t status = sao_ui_layer_create(compositor, &layer_config, &win->layer);
            if (status == SAO_STATUS_OK && config->render_fn != nullptr) {
                status =
                    sao_ui_layer_set_render_fn(win->layer, &adapter_render_callback, win.get());
            }
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_set_visible(win->layer, false);
            }
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_set_input_policy(
                    win->layer, win->click_through, false);
            }
            if (status != SAO_STATUS_OK)
                return status;
            win->backing = SAO_UI_COMPOSITOR_OVERLAY_BACKING_LAYER;
        }
        win->log("create");
        OverlayWindow* const raw = win.get();
        {
            std::lock_guard registry_lock(adapter_registry_mutex());
            live_windows().insert(raw);
            try {
                window_storage().push_back(std::move(win));
            } catch (...) {
                live_windows().erase(raw);
                throw;
            }
        }
        *out_handle = reinterpret_cast<sao_ui_compositor_overlay_window_handle_t>(raw);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL
sao_ui_compositor_overlay_window_destroy(sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr)
        return;
    try {
        auto* win = reinterpret_cast<OverlayWindow*>(handle);
        {
            std::lock_guard registry_lock(adapter_registry_mutex());
            if (!live_windows().erase(win))
                return;
            std::lock_guard lifecycle_lock(win->lifecycle_mutex);
            win->accepting_operations = false;
            win->destroy_requested = true;
        }
        finalize_window_if_ready(win);
        bool self_callback = false;
        for (const ActiveAdapterCallback* active = active_adapter_callback; active != nullptr;
             active = active->previous) {
            if (active->window == win) {
                self_callback = true;
                break;
            }
        }
        if (self_callback)
            return;
        std::unique_lock lock(win->lifecycle_mutex);
        win->lifecycle_cv.wait(lock, [win] { return win->finalized; });
    } catch (...) {
    }
}

extern "C" sao_ui_layer_handle_t SAO_UI_CALL
sao_ui_compositor_overlay_window_layer(sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr)
        return nullptr;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return nullptr;
    try {
        std::lock_guard lock(win->mu);
        return win->layer;
    } catch (...) {
        return nullptr;
    }
}

extern "C" SaoUiCompositorOverlayBackingState SAO_UI_CALL
sao_ui_compositor_overlay_window_backing_state(sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr)
        return SAO_UI_COMPOSITOR_OVERLAY_BACKING_DESTROYED;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_UI_COMPOSITOR_OVERLAY_BACKING_DESTROYED;
    try {
        std::lock_guard lock(win->mu);
        return win->backing;
    } catch (...) {
        return SAO_UI_COMPOSITOR_OVERLAY_BACKING_DESTROYED;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_compositor_overlay_window_show(sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const sao_status_t owner_status = require_host_owner(win);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        const OverlayPolicyState previous = snapshot_policy(win);
        OverlayPolicyState target = previous;
        target.local_visible = true;
        target.layer_visible = true;
        target.layer_click_through = previous.local_click_through;
        target.layer_input_enabled = !previous.local_click_through;
        target.input_proxy_attached = previous.input_proxy_attached;
        const sao_status_t status = apply_policy_transaction(win, previous, target);
        if (status != SAO_STATUS_OK)
            return status;
        win->log("show");
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_compositor_overlay_window_hide(sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const sao_status_t owner_status = require_host_owner(win);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        const OverlayPolicyState previous = snapshot_policy(win);
        OverlayPolicyState target = previous;
        target.local_visible = false;
        target.layer_visible = false;
        target.layer_click_through = previous.local_click_through;
        target.layer_input_enabled = false;
        target.input_proxy_attached = previous.input_proxy_attached;
        target.lift = false;
        const sao_status_t status = apply_policy_transaction(win, previous, target);
        if (status != SAO_STATUS_OK)
            return status;
        win->log("hide");
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_compositor_overlay_window_set_geometry(sao_ui_compositor_overlay_window_handle_t handle,
                                              int32_t x, int32_t y, int32_t width, int32_t height) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (width <= 0 || height <= 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const sao_status_t owner_status = require_host_owner(win);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        bool lift = false;
        {
            std::lock_guard lock(win->mu);
            if (win->layer != nullptr) {
                const sao_status_t status =
                    sao_ui_layer_set_geometry(win->layer, x, y, width, height);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            win->x = x;
            win->y = y;
            win->width = width;
            win->height = height;
            lift = win->input_proxy_attached;
        }
        const sao_status_t sync_status = sync_host(win, true, true, lift);
        if (sync_status != SAO_STATUS_OK)
            return sync_status;
        win->log("set_geometry");
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_move(
    sao_ui_compositor_overlay_window_handle_t handle, int32_t x, int32_t y) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const sao_status_t owner_status = require_host_owner(win);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        bool lift = false;
        {
            std::lock_guard lock(win->mu);
            if (win->layer != nullptr) {
                const sao_status_t status = sao_ui_layer_set_position(win->layer, x, y);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            win->x = x;
            win->y = y;
            lift = win->input_proxy_attached;
        }
        const sao_status_t sync_status = sync_host(win, true, false, lift);
        if (sync_status != SAO_STATUS_OK)
            return sync_status;
        win->log("move");
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_click_through(
    sao_ui_compositor_overlay_window_handle_t handle, bool click_through) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const sao_status_t owner_status = require_host_owner(win);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        const OverlayPolicyState previous = snapshot_policy(win);
        OverlayPolicyState target = previous;
        target.local_click_through = click_through;
        target.layer_click_through = click_through;
        target.layer_input_enabled = previous.local_visible && !click_through;
        target.input_proxy_attached = previous.input_proxy_attached;
        const sao_status_t status = apply_policy_transaction(win, previous, target);
        if (status != SAO_STATUS_OK)
            return status;
        win->log("set_click_through");
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_input_callbacks(
    sao_ui_compositor_overlay_window_handle_t handle, sao_ui_layer_cursor_pos_fn_t cursor_pos_fn,
    sao_ui_layer_cursor_leave_fn_t cursor_leave_fn, sao_ui_layer_button_fn_t button_fn,
    sao_ui_layer_scroll_fn_t scroll_fn, void* user_data) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const sao_status_t owner_status = require_host_owner(win);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        sao_ui_layer_handle_t layer = nullptr;
        {
            std::lock_guard lock(win->mu);
            layer = win->layer;
        }
        if (layer != nullptr) {
            const sao_status_t status = sao_ui_layer_set_input_callbacks(
                layer, &adapter_cursor_callback, &adapter_cursor_leave_callback,
                &adapter_button_callback, &adapter_scroll_callback, win);
            if (status != SAO_STATUS_OK)
                return status;
        }
        uint64_t previous_generation = 0;
        {
            std::lock_guard lifecycle_lock(win->lifecycle_mutex);
            previous_generation = win->input_generation++;
            std::lock_guard state_lock(win->mu);
            win->cursor_pos_fn = cursor_pos_fn;
            win->cursor_leave_fn = cursor_leave_fn;
            win->button_fn = button_fn;
            win->scroll_fn = scroll_fn;
            win->input_user_data = user_data;
        }
        wait_input_generation(win, previous_generation);
        bool lift = false;
        {
            std::lock_guard lock(win->mu);
            lift = win->input_proxy_attached;
        }
        const sao_status_t sync_status = sync_host(win, true, true, lift);
        if (sync_status != SAO_STATUS_OK)
            return sync_status;
        win->log("set_input_callbacks");
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_alpha(
    sao_ui_compositor_overlay_window_handle_t handle, float alpha) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!std::isfinite(alpha))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // Clamp to [0, 1] — matches PIL alpha semantics; the Python code
    // relies on layer.alpha clamp downstream.
    if (alpha < 0.0f)
        alpha = 0.0f;
    if (alpha > 1.0f)
        alpha = 1.0f;
    try {
        const sao_status_t owner_status = require_host_owner(win);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        sao_ui_layer_handle_t layer = nullptr;
        float previous_alpha = 1.0f;
        bool lift = false;
        {
            std::lock_guard lock(win->mu);
            layer = win->layer;
            previous_alpha = win->alpha;
            lift = win->input_proxy_attached;
        }
        sao_status_t status = SAO_STATUS_OK;
        if (layer != nullptr) {
            status = sao_ui_layer_start_fade(layer, alpha, 0.0F, nullptr, nullptr);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_alpha(layer, alpha);
        }
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(win->mu);
            win->alpha = alpha;
        }
        if (status == SAO_STATUS_OK)
            status = sync_host(win, true, true, lift);
        if (status != SAO_STATUS_OK) {
            sao_status_t rollback_status = SAO_STATUS_OK;
            if (layer != nullptr) {
                rollback_status = sao_ui_layer_start_fade(
                    layer, previous_alpha, 0.0F, nullptr, nullptr);
                if (rollback_status == SAO_STATUS_OK)
                    rollback_status = sao_ui_layer_set_alpha(layer, previous_alpha);
            }
            {
                std::lock_guard lock(win->mu);
                win->alpha = previous_alpha;
            }
            const sao_status_t host_rollback_status = sync_host(win, true, true, lift);
            if (rollback_status == SAO_STATUS_OK)
                rollback_status = host_rollback_status;
            if (rollback_status != SAO_STATUS_OK) {
                set_degraded(win, rollback_status);
                return SAO_STATUS_ERR_SURFACE_INVALID;
            }
            set_degraded(win, SAO_STATUS_OK);
            return status;
        }
        set_degraded(win, SAO_STATUS_OK);
        win->log("set_alpha");
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_enable_input_proxy(
    sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const sao_status_t owner_status = require_host_owner(win);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        const OverlayPolicyState previous = snapshot_policy(win);
        OverlayPolicyState target = previous;
        target.input_proxy_attached = true;
        target.layer_input_enabled = true;
        target.lift = true;
        const sao_status_t status = apply_policy_transaction(win, previous, target);
        if (status != SAO_STATUS_OK)
            return status;
        win->log("enable_input_proxy");
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_compositor_overlay_window_raise_to_top(sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const sao_status_t owner_status = require_host_owner(win);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        const int32_t raised = g_next_raised_z.fetch_add(1, std::memory_order_relaxed);
        if (raised >= std::numeric_limits<int32_t>::max() - 1)
            return kStatusBusy;
        bool lift = false;
        {
            std::lock_guard lock(win->mu);
            if (win->layer != nullptr) {
                const sao_status_t status = sao_ui_layer_set_z_order(win->layer, raised);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            win->z_order = raised;
            lift = win->input_proxy_attached;
        }
        const sao_status_t sync_status = sync_host(win, false, false, lift);
        if (sync_status != SAO_STATUS_OK)
            return sync_status;
        win->log("raise_to_top");
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_z(
    sao_ui_compositor_overlay_window_handle_t handle, int32_t z) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const sao_status_t owner_status = require_host_owner(win);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        bool lift = false;
        {
            std::lock_guard lock(win->mu);
            if (win->layer != nullptr) {
                const sao_status_t status = sao_ui_layer_set_z_order(win->layer, z);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            win->z_order = z;
            lift = win->input_proxy_attached;
        }
        const sao_status_t sync_status = sync_host(win, false, false, lift);
        if (sync_status != SAO_STATUS_OK)
            return sync_status;
        win->log("set_z");
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ── BgraPresenter ABI ───────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_create(
    sao_ui_layer_handle_t layer, sao_ui_compositor_bgra_presenter_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (!sao_ui_legacy_compat_enabled())
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    try {
        auto presenter = std::make_unique<BgraPresenter>();
        presenter->layer = layer;
        BgraPresenter* const raw = presenter.get();
        {
            std::lock_guard registry_lock(adapter_registry_mutex());
            live_presenters().insert(raw);
            try {
                presenter_storage().push_back(std::move(presenter));
            } catch (...) {
                live_presenters().erase(raw);
                throw;
            }
        }
        *out_handle = reinterpret_cast<sao_ui_compositor_bgra_presenter_handle_t>(raw);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL
sao_ui_compositor_bgra_presenter_destroy(sao_ui_compositor_bgra_presenter_handle_t handle) {
    if (handle == nullptr)
        return;
    try {
        auto* presenter = reinterpret_cast<BgraPresenter*>(handle);
        {
            std::lock_guard registry_lock(adapter_registry_mutex());
            if (!live_presenters().erase(presenter))
                return;
            std::lock_guard lifecycle_lock(presenter->lifecycle_mutex);
            presenter->accepting_operations = false;
            presenter->destroy_requested = true;
            if (presenter->operations_in_flight == 0)
                presenter->finalized = true;
        }
        std::unique_lock lock(presenter->lifecycle_mutex);
        presenter->lifecycle_cv.wait(lock, [presenter] { return presenter->finalized; });
    } catch (...) {
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_set_frame(
    sao_ui_compositor_bgra_presenter_handle_t handle, const uint8_t* bgra, uint32_t width,
    uint32_t height, int32_t x, int32_t y) {
    if (handle == nullptr || bgra == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (width == 0u || height == 0u) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    PresenterOperation operation(p);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (width > std::numeric_limits<uint32_t>::max() / 4u ||
        static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / 4u / height) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const size_t n = static_cast<size_t>(width) * height * 4u;
    std::vector<uint8_t> snapshot;
    try {
        snapshot.assign(bgra, bgra + n);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    try {
        std::lock_guard lock(p->mu);
        if (p->layer != nullptr) {
            sao_status_t status = sao_ui_layer_set_position(p->layer, x, y);
            if (status != SAO_STATUS_OK)
                return status;
            status = sao_ui_layer_update_bgra(p->layer, bgra, width, height, width * 4u);
            if (status != SAO_STATUS_OK) {
                (void)sao_ui_layer_set_position(p->layer, p->x, p->y);
                return status;
            }
        }
        p->frame = std::move(snapshot);
        p->frame_width = width;
        p->frame_height = height;
        p->x = x;
        p->y = y;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_start_fade(
    sao_ui_compositor_bgra_presenter_handle_t handle, float target_alpha, float duration_sec) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    PresenterOperation operation(p);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!std::isfinite(target_alpha) || !std::isfinite(duration_sec) || duration_sec < 0.0f)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (target_alpha < 0.0f)
        target_alpha = 0.0f;
    if (target_alpha > 1.0f)
        target_alpha = 1.0f;
    try {
        std::lock_guard lock(p->mu);
        if (p->layer != nullptr) {
            const sao_status_t status =
                sao_ui_layer_start_fade(p->layer, target_alpha, duration_sec, nullptr, nullptr);
            if (status != SAO_STATUS_OK)
                return status;
        }
        ++p->fade_generation;
        p->alpha = target_alpha;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_set_alpha(
    sao_ui_compositor_bgra_presenter_handle_t handle, float alpha) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    PresenterOperation operation(p);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!std::isfinite(alpha))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (alpha < 0.0f)
        alpha = 0.0f;
    if (alpha > 1.0f)
        alpha = 1.0f;
    try {
        std::lock_guard lock(p->mu);
        if (p->layer != nullptr) {
            sao_status_t status = sao_ui_layer_start_fade(p->layer, alpha, 0.0F, nullptr, nullptr);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_alpha(p->layer, alpha);
            if (status != SAO_STATUS_OK)
                return status;
        }
        ++p->fade_generation;
        p->alpha = alpha;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" float SAO_UI_CALL
sao_ui_compositor_bgra_presenter_get_alpha(sao_ui_compositor_bgra_presenter_handle_t handle) {
    if (handle == nullptr)
        return 1.0f;
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    PresenterOperation operation(p);
    if (!operation)
        return 1.0F;
    try {
        std::lock_guard lock(p->mu);
        return p->alpha;
    } catch (...) {
        return 1.0F;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_create_overlay_window(
    sao_ui_compositor_handle_t compositor, const SaoGpuOverlayWindowConfig* config,
    sao_ui_compositor_overlay_window_handle_t* out_handle) {
    // Python factory always chooses CompositorOverlayWindow when
    // `_USE_UNIFIED = True`.  The C++ port hard-codes the unified
    // path (no `_USE_UNIFIED` toggle) so this forwards directly.
    return sao_ui_compositor_overlay_window_create(compositor, config, out_handle);
}

// ── Adapter test-only helpers (SAO_UI_API to export from DLL) ───
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_adapter_test_call_count(sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr)
        return 0;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return 0;
    std::lock_guard<std::mutex> guard(win->mu);
    return win->call_log.size();
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_last_call(
    sao_ui_compositor_overlay_window_handle_t handle, char* out_call, size_t out_call_cap) {
    if (handle == nullptr || out_call == nullptr || out_call_cap == 0u) {
        return false;
    }
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return false;
    std::lock_guard<std::mutex> guard(win->mu);
    if (win->call_log.empty())
        return false;
    const std::string& s = win->call_log.back();
    const size_t n = s.size() < out_call_cap - 1u ? s.size() : out_call_cap - 1u;
    std::memcpy(out_call, s.data(), n);
    out_call[n] = '\0';
    return true;
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_layer_name(
    sao_ui_compositor_overlay_window_handle_t handle, char* out_name, size_t out_name_cap) {
    if (handle == nullptr || out_name == nullptr || out_name_cap == 0u) {
        return false;
    }
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return false;
    std::lock_guard<std::mutex> guard(win->mu);
    const std::string& s = win->name;
    const size_t n = s.size() < out_name_cap - 1u ? s.size() : out_name_cap - 1u;
    std::memcpy(out_name, s.data(), n);
    out_name[n] = '\0';
    return true;
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_snapshot(
    sao_ui_compositor_overlay_window_handle_t handle, int32_t* x, int32_t* y, int32_t* w,
    int32_t* h, bool* visible, bool* destroyed, bool* click_through, bool* input_proxy_attached,
    int32_t* z_order, float* alpha) {
    if (handle == nullptr)
        return false;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(win);
    if (!operation)
        return false;
    std::lock_guard<std::mutex> guard(win->mu);
    if (x)
        *x = win->x;
    if (y)
        *y = win->y;
    if (w)
        *w = win->width;
    if (h)
        *h = win->height;
    if (visible)
        *visible = win->visible;
    if (destroyed)
        *destroyed = win->destroyed;
    if (click_through)
        *click_through = win->click_through;
    if (input_proxy_attached)
        *input_proxy_attached = win->input_proxy_attached;
    if (z_order)
        *z_order = win->z_order;
    if (alpha)
        *alpha = win->alpha;
    return true;
}

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_adapter_test_presenter_frame_bytes(sao_ui_compositor_bgra_presenter_handle_t handle) {
    if (handle == nullptr)
        return 0;
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    PresenterOperation operation(p);
    if (!operation)
        return 0;
    std::lock_guard lock(p->mu);
    return p->frame.size();
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_presenter_size(
    sao_ui_compositor_bgra_presenter_handle_t handle, uint32_t* w, uint32_t* h) {
    if (handle == nullptr)
        return false;
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    PresenterOperation operation(p);
    if (!operation)
        return false;
    std::lock_guard lock(p->mu);
    if (w)
        *w = p->frame_width;
    if (h)
        *h = p->frame_height;
    return true;
}

extern "C" SAO_UI_API int32_t SAO_UI_CALL
sao_ui_adapter_test_target_fps(sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr)
        return 0;
    auto* window = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(window);
    if (!operation)
        return 0;
    std::lock_guard lock(window->mu);
    return window->target_fps;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_adapter_test_dispatch_button(sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* window = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(window);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    adapter_button_callback(0, 1, 0, 1.0F, 1.0F, window);
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_adapter_test_fail_next_host_sync(
    sao_ui_compositor_overlay_window_handle_t handle, int32_t phase, sao_status_t status) {
    if (handle == nullptr || phase < 0 ||
        phase >= static_cast<int32_t>(HostSyncPhase::count) || status == SAO_STATUS_OK) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* window = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(window);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard lock(window->mu);
        window->host_sync_failures[static_cast<size_t>(phase)].push_back(status);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_policy_snapshot(
    sao_ui_compositor_overlay_window_handle_t handle, bool* layer_visible,
    bool* layer_click_through, bool* layer_input_enabled, bool* degraded,
    sao_status_t* degraded_status) {
    if (handle == nullptr)
        return false;
    auto* window = reinterpret_cast<OverlayWindow*>(handle);
    WindowOperation operation(window);
    if (!operation)
        return false;
    try {
        std::lock_guard lock(window->mu);
        if (layer_visible != nullptr)
            *layer_visible = window->layer_visible;
        if (layer_click_through != nullptr)
            *layer_click_through = window->layer_click_through;
        if (layer_input_enabled != nullptr)
            *layer_input_enabled = window->layer_input_enabled;
        if (degraded != nullptr)
            *degraded = window->degraded;
        if (degraded_status != nullptr)
            *degraded_status = window->degraded_status;
        return true;
    } catch (...) {
        return false;
    }
}
