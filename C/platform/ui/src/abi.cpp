#include "sao/ui/abi.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct WidgetEventHandler {
    uint64_t token{};
    int32_t event_type{};
    sao_ui_widget_event_cb_t callback{};
    void* user_data{};
};

struct WidgetRendererProvider {
    uint64_t token{};
    int32_t widget_kind{};
    sao_ui_widget_renderer_cb_t callback{};
    void* user_data{};
};

struct WidgetExtensionRegistry {
    std::mutex mutex;
    std::unordered_map<sao_ui_widget_handle_t, std::vector<WidgetEventHandler>> handlers;
    std::unordered_map<int32_t, WidgetRendererProvider> renderers;
    std::unordered_map<uint64_t, int32_t> renderer_kinds;
    std::atomic<uint64_t> next_token{1};
};

WidgetExtensionRegistry& extension_registry() {
    static WidgetExtensionRegistry registry;
    return registry;
}

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
    const int32_t kind = *reinterpret_cast<const int32_t*>(handle);
    if (!valid_widget_kind(kind)) return SAO_STATUS_ERR_HANDLE_INVALID;
    *out_kind = kind;
    return SAO_STATUS_OK;
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
    int32_t kind = -1;
    if (sao_ui_widget_get_kind(handle, &kind) != SAO_STATUS_OK) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    const uint64_t token = allocate_token();
    try {
        auto& registry = extension_registry();
        std::lock_guard lock(registry.mutex);
        registry.handlers[handle].push_back(
            {token, event_type, callback, user_data});
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
    auto& registry = extension_registry();
    std::lock_guard lock(registry.mutex);
    const auto owner = registry.handlers.find(handle);
    if (owner == registry.handlers.end()) {
        return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
    }
    auto& handlers = owner->second;
    const auto found = std::find_if(
        handlers.begin(), handlers.end(),
        [subscription_token](const WidgetEventHandler& handler) {
            return handler.token == subscription_token;
        });
    if (found == handlers.end()) return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
    handlers.erase(found);
    if (handlers.empty()) registry.handlers.erase(owner);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_dispatch_event(
    sao_ui_widget_handle_t handle, int32_t event_type,
    const uint8_t* event_payload_json_utf8, size_t payload_len) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_widget_event_type(event_type) ||
        (event_payload_json_utf8 == nullptr && payload_len != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    std::vector<uint64_t> dispatch_order;
    try {
        auto& registry = extension_registry();
        std::lock_guard lock(registry.mutex);
        const auto owner = registry.handlers.find(handle);
        if (owner == registry.handlers.end()) return SAO_STATUS_OK;
        dispatch_order.reserve(owner->second.size());
        for (const auto& handler : owner->second) {
            if (handler.event_type == event_type) {
                dispatch_order.push_back(handler.token);
            }
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }

    for (const uint64_t token : dispatch_order) {
        sao_ui_widget_event_cb_t callback = nullptr;
        void* user_data = nullptr;
        {
            auto& registry = extension_registry();
            std::lock_guard lock(registry.mutex);
            const auto owner = registry.handlers.find(handle);
            if (owner != registry.handlers.end()) {
                const auto found = std::find_if(
                    owner->second.begin(), owner->second.end(),
                    [token, event_type](const WidgetEventHandler& handler) {
                        return handler.token == token &&
                               handler.event_type == event_type;
                    });
                if (found != owner->second.end()) {
                    callback = found->callback;
                    user_data = found->user_data;
                }
            }
        }
        if (callback != nullptr) {
            callback(event_type, event_payload_json_utf8, payload_len, user_data);
        }
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_release_event_handlers(
    sao_ui_widget_handle_t handle, uint32_t* out_removed_count) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    auto& registry = extension_registry();
    std::lock_guard lock(registry.mutex);
    const auto owner = registry.handlers.find(handle);
    const size_t count = owner == registry.handlers.end()
        ? 0U
        : owner->second.size();
    if (owner != registry.handlers.end()) registry.handlers.erase(owner);
    if (out_removed_count != nullptr) {
        *out_removed_count = static_cast<uint32_t>(std::min<size_t>(
            count, static_cast<size_t>(UINT32_MAX)));
    }
    return SAO_STATUS_OK;
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
        registry.renderers.emplace(
            widget_kind,
            WidgetRendererProvider{token, widget_kind, callback, user_data});
        registry.renderer_kinds.emplace(token, widget_kind);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    *out_provider_token = token;
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
