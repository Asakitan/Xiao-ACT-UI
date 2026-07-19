#include "sao/ui/abi.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" sao_status_t SAO_UI_CALL
sao_ui_widget_generic_backing_get_kind(sao_ui_widget_handle_t handle, int32_t* out_kind);
extern "C" sao_status_t SAO_UI_CALL
sao_ui_widget_input_get_generation(sao_ui_widget_handle_t handle, uint64_t* out_generation);
extern "C" sao_status_t SAO_UI_CALL
sao_ui_widget_chart_get_generation(sao_ui_widget_handle_t handle, uint64_t* out_generation);
extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_paint_widget(
    sao_ui_widget_handle_t widget, sao_ui_paint_ctx_handle_t context,
    float x, float y, float width, float height);

namespace {

struct WidgetEventHandler {
    uint64_t token{};
    uint64_t generation{};
    int32_t event_type{};
    sao_ui_widget_event_cb_t callback{};
    void* user_data{};
    std::mutex mutex;
    std::condition_variable cv;
    size_t in_flight{};
    bool accepting{true};
};

struct WidgetRendererProvider {
    uint64_t token{};
    int32_t widget_kind{};
    sao_ui_widget_renderer_cb_t callback{};
    void* user_data{};
};

struct WidgetExtensionRegistry {
    std::mutex mutex;
    std::unordered_map<sao_ui_widget_handle_t,
                       std::vector<std::shared_ptr<WidgetEventHandler>>>
        handlers;
    std::unordered_map<int32_t, WidgetRendererProvider> renderers;
    std::unordered_map<uint64_t, int32_t> renderer_kinds;
    std::atomic<uint64_t> next_token{1};
    std::atomic_bool fail_next_renderer_kind_insertion{false};
};

struct WidgetHandleShell {
    int32_t kind{-1};
    uint32_t reserved{};
    uint64_t generation{};
};

struct WidgetHandleRecord {
    sao::ui::detail::WidgetHandleMetadata metadata{};
    std::shared_ptr<void> state;
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    size_t in_flight{};
    bool accepting{true};
    bool retired{};
    bool uses_shell{true};
};

struct WidgetHandleRegistry {
    std::mutex mutex;
    std::unordered_map<void*, std::shared_ptr<WidgetHandleRecord>> active;
    std::vector<std::unique_ptr<WidgetHandleShell>> shells;
    std::atomic<uint64_t> next_generation{1};
};

WidgetExtensionRegistry& extension_registry() {
    static WidgetExtensionRegistry registry;
    return registry;
}

WidgetHandleRegistry& widget_handle_registry() {
    static WidgetHandleRegistry registry;
    return registry;
}

struct ActiveEventHandler {
    const WidgetEventHandler* handler{};
    ActiveEventHandler* previous{};
};

thread_local ActiveEventHandler* active_event_handler = nullptr;

thread_local std::vector<void*> active_widget_lifecycles;

bool widget_lifecycle_is_active(void* handle) {
        return std::find(active_widget_lifecycles.begin(),
                                         active_widget_lifecycles.end(), handle) !=
                     active_widget_lifecycles.end();
}

class WidgetLifecycleLease {
    public:
        explicit WidgetLifecycleLease(void* handle) noexcept : handle_(handle) {
                acquired_ = sao::ui::detail::acquire_widget_lifecycle(handle_);
        }

        ~WidgetLifecycleLease() {
                if (acquired_)
                        sao::ui::detail::release_widget_lifecycle(handle_);
        }

        WidgetLifecycleLease(const WidgetLifecycleLease&) = delete;
        WidgetLifecycleLease& operator=(const WidgetLifecycleLease&) = delete;

        explicit operator bool() const noexcept { return acquired_; }

    private:
        void* handle_{};
        bool acquired_{};
};

bool event_handler_is_active(const WidgetEventHandler* handler) {
    for (const ActiveEventHandler* active = active_event_handler; active != nullptr;
         active = active->previous) {
        if (active->handler == handler)
            return true;
    }
    return false;
}

void retire_event_handler(const std::shared_ptr<WidgetEventHandler>& handler) {
    {
        std::lock_guard lock(handler->mutex);
        handler->accepting = false;
    }
    if (event_handler_is_active(handler.get()))
        return;
    std::unique_lock lock(handler->mutex);
    handler->cv.wait(lock, [&] { return handler->in_flight == 0; });
}

class EventHandlerLease {
  public:
    explicit EventHandlerLease(std::shared_ptr<WidgetEventHandler> handler)
        : handler_(std::move(handler)) {
        std::lock_guard lock(handler_->mutex);
        if (!handler_->accepting)
            return;
        ++handler_->in_flight;
        callback_ = handler_->callback;
        user_data_ = handler_->user_data;
        marker_ = {handler_.get(), active_event_handler};
        active_event_handler = &marker_;
        acquired_ = true;
    }

    ~EventHandlerLease() {
        if (!acquired_)
            return;
        active_event_handler = marker_.previous;
        {
            std::lock_guard lock(handler_->mutex);
            --handler_->in_flight;
        }
        handler_->cv.notify_all();
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

    sao_ui_widget_event_cb_t callback() const noexcept {
        return callback_;
    }

    void* user_data() const noexcept {
        return user_data_;
    }

  private:
    std::shared_ptr<WidgetEventHandler> handler_;
    sao_ui_widget_event_cb_t callback_{};
    void* user_data_{};
    ActiveEventHandler marker_{};
    bool acquired_{};
};

uint64_t allocate_token() {
    auto& registry = extension_registry();
    uint64_t token = registry.next_token.fetch_add(1, std::memory_order_relaxed);
    if (token == 0) {
        token = registry.next_token.fetch_add(1, std::memory_order_relaxed);
    }
    return token;
}

bool valid_widget_kind(int32_t kind) {
    return (kind >= SAO_UI_WIDGET_ROUNDED_PANEL && kind <= SAO_UI_WIDGET_ICON) ||
           (kind >= SAO_UI_WIDGET_LABEL && kind <= SAO_UI_WIDGET_DURATION_LABEL) ||
           (kind >= SAO_UI_WIDGET_BUTTON && kind <= SAO_UI_WIDGET_SLIDER_EXT) ||
           (kind >= SAO_UI_WIDGET_PANEL && kind <= SAO_UI_WIDGET_GRID) ||
           (kind >= SAO_UI_WIDGET_PROGRESS_BAR && kind <= SAO_UI_WIDGET_EMPTY_STATE) ||
           (kind >= SAO_UI_WIDGET_TIME_SERIES_CHART && kind <= SAO_UI_WIDGET_SPARKLINE) ||
           kind == SAO_UI_WIDGET_SCRIPTABLE_CANVAS;
}

bool valid_widget_event_type(int32_t event_type) {
    return event_type >= SAO_UI_EVT_CLICK && event_type <= SAO_UI_EVT_SCROLL;
}

sao_status_t paint_extended_default(
    int32_t kind, sao_ui_paint_ctx_handle_t ctx,
    int32_t x, int32_t y, int32_t width, int32_t height) {
    const float xf = static_cast<float>(x);
    const float yf = static_cast<float>(y);
    const float wf = static_cast<float>(width);
    const float hf = static_cast<float>(height);
    sao_status_t status = sao_ui_paint_ctx_fill_rect(
        ctx, xf, yf, wf, hf, 0xff273447U);
    if (status != SAO_STATUS_OK) return status;

    if (kind >= SAO_UI_WIDGET_TIME_SERIES_CHART &&
        kind <= SAO_UI_WIDGET_SPARKLINE) {
        status = sao_ui_paint_ctx_stroke_line(
            ctx, xf + 2.0F, yf + hf - 3.0F,
            xf + wf * 0.4F, yf + hf * 0.35F, 2.0F, 0xff4ea5ffU);
        if (status == SAO_STATUS_OK) {
            status = sao_ui_paint_ctx_stroke_line(
                ctx, xf + wf * 0.4F, yf + hf * 0.35F,
                xf + wf - 2.0F, yf + hf * 0.55F, 2.0F, 0xff4ea5ffU);
        }
        return status;
    }
    if (kind == SAO_UI_WIDGET_PROGRESS_BAR || kind == SAO_UI_WIDGET_GAUGE) {
        return sao_ui_paint_ctx_fill_rect(
            ctx, xf, yf, wf * 0.5F, hf, 0xff4ea5ffU);
    }
    if (kind >= SAO_UI_WIDGET_LABEL &&
        kind <= SAO_UI_WIDGET_DURATION_LABEL) {
        return sao_ui_paint_ctx_draw_utf8(
            ctx, xf + 2.0F, yf + 2.0F, "widget",
            std::max(5.0F, std::min(14.0F, hf - 4.0F)), 0xfff0f4faU);
    }
    return sao_ui_paint_ctx_fill_rect(
        ctx, xf, yf, std::min(3.0F, wf), hf, 0xff4ea5ffU);
}

void set_size_hint(
    SaoUiWidgetSizeHint& hint, int32_t min_width, int32_t min_height,
    int32_t preferred_width, int32_t preferred_height) {
    hint.min_width_px = min_width;
    hint.min_height_px = min_height;
    hint.preferred_width_px = preferred_width;
    hint.preferred_height_px = preferred_height;
    hint.flex_shrink = 1.0F;
}

}  // namespace

void* sao::ui::detail::register_widget_handle(
    WidgetHandleFamily family, int32_t kind,
    std::shared_ptr<void> state) noexcept {
    if (state == nullptr || !valid_widget_kind(kind)) return nullptr;
    try {
        auto shell = std::make_unique<WidgetHandleShell>();
        auto& registry = widget_handle_registry();
        uint64_t generation = registry.next_generation.fetch_add(
            1, std::memory_order_relaxed);
        if (generation == 0) {
            generation = registry.next_generation.fetch_add(
                1, std::memory_order_relaxed);
        }
        shell->kind = kind;
        shell->generation = generation;
        void* const handle = shell.get();
        std::lock_guard lock(registry.mutex);
        registry.shells.push_back(std::move(shell));
        auto record = std::make_shared<WidgetHandleRecord>();
        const bool inserted = registry.active.emplace(handle, record).second;
        if (!inserted) return nullptr;
        record->metadata = {family, kind, generation};
        record->state = std::move(state);
        record->uses_shell = true;
        return handle;
    } catch (...) {
        return nullptr;
    }
}

bool sao::ui::detail::register_external_widget_handle(
    void* handle, WidgetHandleFamily family, int32_t kind,
    uint64_t* out_generation) noexcept {
    if (handle == nullptr) return false;
    try {
        auto& registry = widget_handle_registry();
        uint64_t generation = registry.next_generation.fetch_add(
            1, std::memory_order_relaxed);
        if (generation == 0) {
            generation = registry.next_generation.fetch_add(
                1, std::memory_order_relaxed);
        }
        std::lock_guard lock(registry.mutex);
        auto record = std::make_shared<WidgetHandleRecord>();
        const bool inserted = registry.active.emplace(handle, record).second;
        if (!inserted) return false;
        record->metadata = {family, kind, generation};
        record->state.reset();
        record->uses_shell = false;
        if (out_generation != nullptr) *out_generation = generation;
        return true;
    } catch (...) {
        return false;
    }
}

bool sao::ui::detail::register_widget_lifecycle(
    void* handle, WidgetHandleFamily family, int32_t kind,
    uint64_t generation) noexcept {
    if (handle == nullptr) return false;
    if (generation == 0)
        return register_external_widget_handle(handle, family, kind, nullptr);
    try {
        auto& registry = widget_handle_registry();
        std::lock_guard lock(registry.mutex);
        auto record = std::make_shared<WidgetHandleRecord>();
        const bool inserted = registry.active.emplace(handle, record).second;
        if (!inserted) return false;
        record->metadata = {family, kind, generation};
        record->state.reset();
        record->uses_shell = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool sao::ui::detail::acquire_widget_lifecycle(void* handle) noexcept {
    if (handle == nullptr) return false;
    try {
        WidgetHandleRecord* record = nullptr;
        {
            auto& registry = widget_handle_registry();
            std::lock_guard registry_lock(registry.mutex);
            const auto found = registry.active.find(handle);
            if (found == registry.active.end()) return false;
            record = found->second.get();
            std::lock_guard lifecycle_lock(record->lifecycle_mutex);
            if (!record->accepting || record->retired) return false;
            ++record->in_flight;
        }
        try {
            active_widget_lifecycles.push_back(handle);
        } catch (...) {
            std::lock_guard lifecycle_lock(record->lifecycle_mutex);
            --record->in_flight;
            record->lifecycle_cv.notify_all();
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void sao::ui::detail::release_widget_lifecycle(void* handle) noexcept {
    if (handle == nullptr) return;
    try {
        const auto found = std::find(active_widget_lifecycles.rbegin(),
                                     active_widget_lifecycles.rend(), handle);
        if (found != active_widget_lifecycles.rend())
            active_widget_lifecycles.erase(std::next(found).base());

        WidgetHandleRecord* record = nullptr;
        auto& registry = widget_handle_registry();
        std::lock_guard registry_lock(registry.mutex);
        const auto record_it = registry.active.find(handle);
        if (record_it == registry.active.end()) return;
        record = record_it->second.get();
        std::lock_guard lifecycle_lock(record->lifecycle_mutex);
        if (record->in_flight != 0) --record->in_flight;
        if (record->in_flight == 0) record->lifecycle_cv.notify_all();
    } catch (...) {
    }
}

bool sao::ui::detail::retire_widget_lifecycle(void* handle) noexcept {
    if (handle == nullptr) return false;
    try {
        std::shared_ptr<WidgetHandleRecord> record;
        {
            auto& registry = widget_handle_registry();
            std::lock_guard registry_lock(registry.mutex);
            const auto found = registry.active.find(handle);
            if (found == registry.active.end()) return false;
            record = found->second;
            std::lock_guard lifecycle_lock(record->lifecycle_mutex);
            if (record->retired) return false;
            record->accepting = false;
            record->retired = true;
        }
        const size_t own_leases = static_cast<size_t>(std::count(
            active_widget_lifecycles.begin(), active_widget_lifecycles.end(), handle));
        std::unique_lock lifecycle_lock(record->lifecycle_mutex);
        record->lifecycle_cv.wait(lifecycle_lock, [record, own_leases] {
            return record->in_flight <= own_leases;
        });
        return true;
    } catch (...) {
        return false;
    }
}

std::shared_ptr<void> sao::ui::detail::acquire_widget_handle(
    void* handle, WidgetHandleFamily expected_family,
    int32_t expected_kind) noexcept {
    if (handle == nullptr) return {};
    try {
        auto& registry = widget_handle_registry();
        std::lock_guard lock(registry.mutex);
        const auto found = registry.active.find(handle);
        if (found == registry.active.end() ||
            found->second->metadata.family != expected_family ||
            found->second->metadata.kind != expected_kind) {
            return {};
        }
        std::lock_guard lifecycle_lock(found->second->lifecycle_mutex);
        if (!found->second->accepting || found->second->retired) return {};
        if (found->second->uses_shell) {
            const auto* shell = static_cast<const WidgetHandleShell*>(handle);
            if (shell->generation != found->second->metadata.generation) return {};
        }
        return found->second->state;
    } catch (...) {
        return {};
    }
}

std::shared_ptr<void> sao::ui::detail::retire_widget_handle(
    void* handle, WidgetHandleFamily expected_family,
    WidgetHandleMetadata* out_metadata) noexcept {
    if (handle == nullptr) return {};
    try {
        auto& registry = widget_handle_registry();
        {
            std::lock_guard lock(registry.mutex);
            const auto found = registry.active.find(handle);
            if (found == registry.active.end() ||
                found->second->metadata.family != expected_family) {
                return {};
            }
            std::lock_guard lifecycle_lock(found->second->lifecycle_mutex);
            if (found->second->retired || !found->second->accepting) return {};
            if (found->second->uses_shell) {
                const auto* shell = static_cast<const WidgetHandleShell*>(handle);
                if (shell->generation != found->second->metadata.generation) return {};
            }
            if (out_metadata != nullptr) *out_metadata = found->second->metadata;
        }
        if (!sao::ui::detail::retire_widget_lifecycle(handle)) return {};
        std::lock_guard lock(registry.mutex);
        const auto found = registry.active.find(handle);
        if (found == registry.active.end()) return {};
        std::lock_guard lifecycle_lock(found->second->lifecycle_mutex);
        std::shared_ptr<void> state = std::move(found->second->state);
        found->second->state.reset();
        return state;
    } catch (...) {
        return {};
    }
}

bool sao::ui::detail::inspect_widget_handle(
    void* handle, WidgetHandleMetadata* out_metadata) noexcept {
    if (handle == nullptr || out_metadata == nullptr) return false;
    try {
        auto& registry = widget_handle_registry();
        std::lock_guard lock(registry.mutex);
        const auto found = registry.active.find(handle);
        if (found == registry.active.end()) return false;
        std::lock_guard lifecycle_lock(found->second->lifecycle_mutex);
        if (!found->second->accepting || found->second->retired) return false;
        if (found->second->uses_shell) {
            const auto* shell = static_cast<const WidgetHandleShell*>(handle);
            if (shell->generation != found->second->metadata.generation) return false;
        }
        *out_metadata = found->second->metadata;
        return true;
    } catch (...) {
        return false;
    }
}

extern "C" uint32_t SAO_UI_CALL sao_ui_abi_version(void) {
    return SAO_UI_ABI_VERSION;
}

extern "C" uint32_t SAO_UI_CALL sao_ui_widget_kit_version(void) {
    return SAO_UI_WIDGET_KIT_VERSION;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_get_kind(
    sao_ui_widget_handle_t handle, int32_t* out_kind) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_kind == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    WidgetLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    const sao_status_t generic_status = sao_ui_widget_generic_backing_get_kind(handle, out_kind);
    if (generic_status == SAO_STATUS_OK)
        return SAO_STATUS_OK;
    if (generic_status == SAO_STATUS_ERR_SUBSCRIPTION_GONE)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    sao::ui::detail::WidgetHandleMetadata metadata{};
    if (sao::ui::detail::inspect_widget_handle(handle, &metadata)) {
        *out_kind = metadata.kind;
        return SAO_STATUS_OK;
    }
    if (sao_ui_widget_input_get_generation(handle, nullptr) == SAO_STATUS_OK ||
        sao_ui_widget_chart_get_generation(handle, nullptr) == SAO_STATUS_OK) {
        *out_kind = *reinterpret_cast<const int32_t*>(handle);
        return valid_widget_kind(*out_kind) ? SAO_STATUS_OK : SAO_STATUS_ERR_HANDLE_INVALID;
    }
    return SAO_STATUS_ERR_HANDLE_INVALID;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_add_event_handler(
    sao_ui_widget_handle_t handle, int32_t event_type,
    sao_ui_widget_event_cb_t callback, void* user_data,
    uint64_t* out_subscription_token) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_widget_event_type(event_type) || callback == nullptr ||
        out_subscription_token == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_subscription_token = 0;
    WidgetLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    int32_t kind = -1;
    if (sao_ui_widget_get_kind(handle, &kind) != SAO_STATUS_OK) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    const uint64_t token = allocate_token();
    try {
        auto& registry = extension_registry();
        std::lock_guard lock(registry.mutex);
        auto handler = std::make_shared<WidgetEventHandler>();
        handler->token = token;
        handler->generation = token;
        handler->event_type = event_type;
        handler->callback = callback;
        handler->user_data = user_data;
        registry.handlers[handle].push_back(std::move(handler));
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    *out_subscription_token = token;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_remove_event_handler(
    sao_ui_widget_handle_t handle, uint64_t subscription_token) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (subscription_token == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    WidgetLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::shared_ptr<WidgetEventHandler> removed;
        auto& registry = extension_registry();
        {
            std::lock_guard lock(registry.mutex);
            const auto owner = registry.handlers.find(
                static_cast<sao_ui_widget_handle_t>(handle));
            if (owner == registry.handlers.end())
                return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
            auto& handlers = owner->second;
            const auto found = std::find_if(
                handlers.begin(), handlers.end(),
                [subscription_token](const std::shared_ptr<WidgetEventHandler>& handler) {
                    return handler->token == subscription_token;
                });
            if (found == handlers.end())
                return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
            removed = *found;
            handlers.erase(found);
            if (handlers.empty())
                registry.handlers.erase(owner);
        }
        retire_event_handler(removed);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_dispatch_event(
    sao_ui_widget_handle_t handle, int32_t event_type,
    const uint8_t* event_payload_json_utf8, size_t payload_len) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_widget_event_type(event_type) ||
        (event_payload_json_utf8 == nullptr && payload_len != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    WidgetLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;

    std::vector<std::shared_ptr<WidgetEventHandler>> dispatch_order;
    try {
        auto& registry = extension_registry();
        std::lock_guard lock(registry.mutex);
        const auto owner = registry.handlers.find(handle);
        if (owner == registry.handlers.end()) return SAO_STATUS_OK;
        dispatch_order.reserve(owner->second.size());
        for (const auto& handler : owner->second) {
            if (handler->event_type == event_type) {
                dispatch_order.push_back(handler);
            }
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }

    for (const auto& handler : dispatch_order) {
        EventHandlerLease lease(handler);
        if (!lease || lease.callback() == nullptr)
            continue;
        try {
            lease.callback()(event_type, event_payload_json_utf8, payload_len,
                             lease.user_data());
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }
    return SAO_STATUS_OK;
}

sao_status_t sao::ui::detail::release_widget_event_handlers(
    void* handle, uint32_t* out_removed_count) noexcept {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::vector<std::shared_ptr<WidgetEventHandler>> removed;
        auto& registry = extension_registry();
        {
            std::lock_guard lock(registry.mutex);
            const auto owner = registry.handlers.find(
                reinterpret_cast<sao_ui_widget_handle_t>(handle));
            if (owner != registry.handlers.end()) {
                removed = std::move(owner->second);
                registry.handlers.erase(owner);
            }
        }
        if (out_removed_count != nullptr) {
            *out_removed_count = static_cast<uint32_t>(std::min<size_t>(
                removed.size(), static_cast<size_t>(UINT32_MAX)));
        }
        for (const auto& handler : removed)
            retire_event_handler(handler);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_release_event_handlers(
    sao_ui_widget_handle_t handle, uint32_t* out_removed_count) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    WidgetLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    return sao::ui::detail::release_widget_event_handlers(handle, out_removed_count);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_paint_at(
    sao_ui_widget_handle_t handle, sao_ui_paint_ctx_handle_t ctx,
    int32_t x, int32_t y, int32_t width, int32_t height,
    float opacity_0_to_1) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (ctx == nullptr || width <= 0 || height <= 0 ||
        !std::isfinite(opacity_0_to_1) || opacity_0_to_1 < 0.0F ||
        opacity_0_to_1 > 1.0F) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    WidgetLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    int32_t kind = -1;
    const sao_status_t status = sao_ui_widget_get_kind(handle, &kind);
    if (status != SAO_STATUS_OK) return status;
    sao_status_t paint_status = sao_ui_paint_ctx_push_clip(
        ctx, static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(width), static_cast<float>(height));
    if (paint_status != SAO_STATUS_OK) return paint_status;
    paint_status = sao_ui_paint_ctx_push_opacity(ctx, opacity_0_to_1);
    if (paint_status != SAO_STATUS_OK) {
        (void)sao_ui_paint_ctx_pop_clip(ctx);
        return paint_status;
    }

    if (kind <= SAO_UI_WIDGET_ICON) {
        paint_status = sao_ui_widget_paint(
            handle, ctx, static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(width), static_cast<float>(height));
    } else if (kind == SAO_UI_WIDGET_SCRIPTABLE_CANVAS) {
        paint_status = sao_ui_script_canvas_paint_widget(
            handle, ctx, static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(width), static_cast<float>(height));
    } else {
        WidgetRendererProvider provider{};
        {
            auto& registry = extension_registry();
            std::lock_guard lock(registry.mutex);
            const auto found = registry.renderers.find(kind);
            if (found != registry.renderers.end()) provider = found->second;
        }
        paint_status = provider.callback == nullptr
            ? paint_extended_default(kind, ctx, x, y, width, height)
            : provider.callback(handle, ctx, x, y, width, height,
                                provider.user_data);
    }

    const sao_status_t opacity_status = sao_ui_paint_ctx_pop_opacity(ctx);
    const sao_status_t clip_status = sao_ui_paint_ctx_pop_clip(ctx);
    if (paint_status != SAO_STATUS_OK) return paint_status;
    if (opacity_status != SAO_STATUS_OK) return opacity_status;
    return clip_status;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_widget_register_renderer_provider(
    int32_t widget_kind, sao_ui_widget_renderer_cb_t callback,
    void* user_data, uint64_t* out_provider_token) {
    if (!valid_widget_kind(widget_kind) || widget_kind <= SAO_UI_WIDGET_ICON ||
        callback == nullptr || out_provider_token == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_provider_token = 0;
    const uint64_t token = allocate_token();
    try {
        auto& registry = extension_registry();
        std::lock_guard lock(registry.mutex);
        if (registry.renderers.contains(widget_kind)) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        const auto [renderer_it, renderer_inserted] = registry.renderers.emplace(
            widget_kind,
            WidgetRendererProvider{token, widget_kind, callback, user_data});
        if (!renderer_inserted)
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        try {
            if (registry.fail_next_renderer_kind_insertion.exchange(false))
                throw std::bad_alloc{};
            const auto [token_it, token_inserted] = registry.renderer_kinds.emplace(
                token, widget_kind);
            if (!token_inserted) {
                (void)token_it;
                registry.renderers.erase(renderer_it);
                return SAO_STATUS_ERR_UNKNOWN;
            }
        } catch (...) {
            registry.renderers.erase(renderer_it);
            throw;
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    *out_provider_token = token;
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_test_fail_next_renderer_kind_insertion() {
    extension_registry().fail_next_renderer_kind_insertion.store(true);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_widget_unregister_renderer_provider(uint64_t provider_token) {
    if (provider_token == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto& registry = extension_registry();
    std::lock_guard lock(registry.mutex);
    const auto token = registry.renderer_kinds.find(provider_token);
    if (token == registry.renderer_kinds.end()) {
        return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
    }
    registry.renderers.erase(token->second);
    registry.renderer_kinds.erase(token);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_get_size_hint(
    sao_ui_widget_handle_t handle, int32_t available_width_px,
    int32_t available_height_px, SaoUiWidgetSizeHint* out_hint) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_hint == nullptr || available_width_px < 0 || available_height_px < 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    WidgetLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    int32_t kind = -1;
    const sao_status_t status = sao_ui_widget_get_kind(handle, &kind);
    if (status != SAO_STATUS_OK) return status;
    *out_hint = {};
    if (kind >= SAO_UI_WIDGET_TIME_SERIES_CHART && kind <= SAO_UI_WIDGET_SPARKLINE) {
        set_size_hint(*out_hint, 80, 48, 320, 180);
        out_hint->flex_grow = 1.0F;
    } else if (kind == SAO_UI_WIDGET_TABLE_EXT || kind == SAO_UI_WIDGET_TREE_VIEW) {
        set_size_hint(*out_hint, 120, 72, 320, 200);
        out_hint->flex_grow = 1.0F;
    } else if (kind >= SAO_UI_WIDGET_PROGRESS_BAR && kind <= SAO_UI_WIDGET_EMPTY_STATE) {
        set_size_hint(*out_hint, 48, 18, 160, 28);
    } else if (kind >= SAO_UI_WIDGET_BUTTON && kind <= SAO_UI_WIDGET_SLIDER_EXT) {
        set_size_hint(*out_hint, 48, 24, 96, 32);
    } else {
        set_size_hint(*out_hint, 16, 16, 120, 24);
    }
    if (available_width_px > 0) {
        out_hint->preferred_width_px =
            std::min(out_hint->preferred_width_px, available_width_px);
    }
    if (available_height_px > 0) {
        out_hint->preferred_height_px =
            std::min(out_hint->preferred_height_px, available_height_px);
    }
    return SAO_STATUS_OK;
}
