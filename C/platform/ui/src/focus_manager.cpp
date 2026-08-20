// SAO Auto — keyboard focus manager implementation.
//
// Owns the ordered list of focusable widget handles for the active
// panel and drives sao_ui_widget_set_focused on enter/leave so the
// existing focus ring (d2d_widgets.cpp paint_focus_ring, accent token)
// renders without any widget-side change.  Tab/Shift-Tab traversal
// wraps; set/clear/get are O(1).

#include "sao/ui/focus_manager.h"
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" bool SAO_UI_CALL sao_ui_panel_input_widget_is_visible_(sao_ui_widget_handle_t widget);
extern "C" void SAO_UI_CALL sao_ui_input_router_clear_focus_widget_(sao_ui_widget_handle_t widget);

#include <nlohmann/json.hpp>

struct sao_ui_focus_s {
    std::mutex mu;
    std::vector<sao_ui_widget_handle_t> order;
    std::unordered_map<sao_ui_widget_handle_t, uint64_t> generations;
    sao_ui_widget_handle_t focused{nullptr};
    uint64_t focused_generation{0};
};

namespace {

bool widget_is_live_focusable(sao_ui_widget_handle_t widget, uint64_t expected_generation = 0) {
    if (widget == nullptr)
        return false;
    sao::ui::detail::WidgetHandleMetadata metadata{};
    if (!sao::ui::detail::inspect_widget_handle(widget, &metadata) ||
        (expected_generation != 0 && metadata.generation != expected_generation))
        return false;
    bool focusable = false;
    return sao_ui_widget_is_focusable(widget, &focusable) == SAO_STATUS_OK && focusable &&
           sao_ui_panel_input_widget_is_visible_(widget);
}

void dispatch_focus_event(sao_ui_widget_handle_t widget, bool gained) {
    if (widget == nullptr)
        return;
    (void)sao_ui_widget_set_focused(widget, gained);
    (void)sao_ui_widget_dispatch_event(
        widget, gained ? SAO_UI_EVT_FOCUS_GAINED : SAO_UI_EVT_FOCUS_LOST, nullptr, 0);
}

void prune_focus_state_locked(sao_ui_focus_s* handle) {
    handle->order.erase(
        std::remove_if(handle->order.begin(), handle->order.end(), [&](auto widget) {
            const auto found = handle->generations.find(widget);
            if (found != handle->generations.end() &&
                widget_is_live_focusable(widget, found->second))
                return false;
            handle->generations.erase(widget);
            if (handle->focused == widget) {
                handle->focused = nullptr;
                handle->focused_generation = 0;
            }
            sao_ui_input_router_clear_focus_widget_(widget);
            return true;
        }),
        handle->order.end());
    if (handle->focused != nullptr &&
        !widget_is_live_focusable(handle->focused, handle->focused_generation)) {
        sao_ui_input_router_clear_focus_widget_(handle->focused);
        handle->focused = nullptr;
        handle->focused_generation = 0;
    }
}

void clear_widget_focus(sao_ui_widget_handle_t widget) {
    if (widget != nullptr)
        sao_ui_widget_set_focused(widget, false);
}

void set_widget_focus(sao_ui_widget_handle_t widget) {
    if (widget != nullptr)
        sao_ui_widget_set_focused(widget, true);
}

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_create(
    sao_ui_focus_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto* handle = new sao_ui_focus_s();
        *out_handle = handle;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_focus_destroy(sao_ui_focus_handle_t handle) {
    if (handle == nullptr)
        return;
    sao_ui_widget_handle_t focused = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        focused = handle->focused;
        handle->focused = nullptr;
        handle->focused_generation = 0;
        handle->order.clear();
        handle->generations.clear();
    }
    sao_ui_input_router_clear_focus_widget_(focused);
    dispatch_focus_event(focused, false);
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_register(
    sao_ui_focus_handle_t handle, sao_ui_widget_handle_t widget) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (widget == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    sao::ui::detail::WidgetHandleMetadata metadata{};
    if (!sao::ui::detail::inspect_widget_handle(widget, &metadata) ||
        !widget_is_live_focusable(widget))
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mu);
    prune_focus_state_locked(handle);
    if (std::find(handle->order.begin(), handle->order.end(), widget) != handle->order.end())
        return SAO_STATUS_OK;
    handle->order.push_back(widget);
    handle->generations[widget] = metadata.generation;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_unregister(
    sao_ui_focus_handle_t handle, sao_ui_widget_handle_t widget) {
    if (handle == nullptr || widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_ui_widget_handle_t previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        prune_focus_state_locked(handle);
        const auto it = std::find(handle->order.begin(), handle->order.end(), widget);
        if (it == handle->order.end())
            return SAO_STATUS_ERR_NOT_FOUND;
        handle->order.erase(it);
        handle->generations.erase(widget);
        if (handle->focused == widget) {
            previous = handle->focused;
            handle->focused = nullptr;
            handle->focused_generation = 0;
        }
    }
    sao_ui_input_router_clear_focus_widget_(widget);
    dispatch_focus_event(previous, false);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_set_order(
    sao_ui_focus_handle_t handle,
    const sao_ui_widget_handle_t* widgets, size_t count) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (widgets == nullptr && count != 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::vector<sao_ui_widget_handle_t> new_order;
    std::unordered_map<sao_ui_widget_handle_t, uint64_t> new_generations;
    new_order.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (widgets[i] == nullptr)
            continue;
        sao::ui::detail::WidgetHandleMetadata metadata{};
        if (!sao::ui::detail::inspect_widget_handle(widgets[i], &metadata) ||
            !widget_is_live_focusable(widgets[i]))
            continue;
        if (std::find(new_order.begin(), new_order.end(), widgets[i]) != new_order.end())
            continue;
        new_order.push_back(widgets[i]);
        new_generations[widgets[i]] = metadata.generation;
    }
    sao_ui_widget_handle_t previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        prune_focus_state_locked(handle);
        if (handle->focused != nullptr &&
            std::find(new_order.begin(), new_order.end(), handle->focused) == new_order.end()) {
            previous = handle->focused;
            handle->focused = nullptr;
            handle->focused_generation = 0;
        }
        handle->order = std::move(new_order);
        handle->generations = std::move(new_generations);
    }
    dispatch_focus_event(previous, false);
    sao_ui_input_router_clear_focus_widget_(previous);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_clear(sao_ui_focus_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_ui_widget_handle_t previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        previous = handle->focused;
        handle->focused = nullptr;
        handle->focused_generation = 0;
        handle->order.clear();
        handle->generations.clear();
    }
    sao_ui_input_router_clear_focus_widget_(previous);
    dispatch_focus_event(previous, false);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_set(
    sao_ui_focus_handle_t handle, sao_ui_widget_handle_t widget) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    uint64_t generation = 0;
    if (widget != nullptr) {
        sao::ui::detail::WidgetHandleMetadata metadata{};
        if (!sao::ui::detail::inspect_widget_handle(widget, &metadata) ||
            !widget_is_live_focusable(widget))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        generation = metadata.generation;
    }
    sao_ui_widget_handle_t previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        prune_focus_state_locked(handle);
        if (widget != nullptr &&
            std::find(handle->order.begin(), handle->order.end(), widget) == handle->order.end())
            return SAO_STATUS_ERR_NOT_FOUND;
        if (handle->focused == widget &&
            (widget == nullptr || handle->focused_generation == generation))
            return SAO_STATUS_OK;
        previous = handle->focused;
        handle->focused = widget;
        handle->focused_generation = generation;
    }
    dispatch_focus_event(previous, false);
    sao_ui_input_router_clear_focus_widget_(previous);
    dispatch_focus_event(widget, true);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_get(
    sao_ui_focus_handle_t handle, sao_ui_widget_handle_t* out_widget) {
    if (handle == nullptr || out_widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    prune_focus_state_locked(handle);
    *out_widget = handle->focused;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_tab_next(
    sao_ui_focus_handle_t handle, bool reverse) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_ui_widget_handle_t previous = nullptr;
    sao_ui_widget_handle_t next = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        prune_focus_state_locked(handle);
        if (handle->order.empty())
            return SAO_STATUS_ERR_NOT_FOUND;
        size_t index = 0;
        if (handle->focused != nullptr) {
            const auto it = std::find(handle->order.begin(), handle->order.end(), handle->focused);
            if (it != handle->order.end()) {
                const size_t current = static_cast<size_t>(it - handle->order.begin());
                if (reverse)
                    index = (current == 0) ? handle->order.size() - 1 : current - 1;
                else
                    index = (current + 1) % handle->order.size();
            } else {
                index = reverse ? handle->order.size() - 1 : 0;
            }
        } else {
            index = reverse ? handle->order.size() - 1 : 0;
        }
        next = handle->order[index];
        if (handle->focused == next)
            return SAO_STATUS_OK;
        previous = handle->focused;
        handle->focused = next;
        handle->focused_generation = handle->generations[next];
    }
    dispatch_focus_event(previous, false);
    dispatch_focus_event(next, true);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_apply_props(
    sao_ui_focus_handle_t handle,
    const uint8_t* props_json_utf8, size_t props_len) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (props_json_utf8 == nullptr && props_len != 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        nlohmann::json props = nlohmann::json::parse(
            props_json_utf8, props_json_utf8 + props_len, nullptr, false);
        if (!props.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const auto order_it = props.find("order");
        if (order_it == props.end() || !order_it->is_array())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::vector<sao_ui_widget_handle_t> new_order;
        new_order.reserve(order_it->size());
        for (const auto& item : *order_it) {
            if (!item.is_number_integer())
                continue;
            const int64_t value = item.get<int64_t>();
            if (value == 0)
                continue;
            const auto widget = reinterpret_cast<sao_ui_widget_handle_t>(static_cast<uintptr_t>(value));
            if (std::find(new_order.begin(), new_order.end(), widget) != new_order.end())
                continue;
            new_order.push_back(widget);
        }
        return sao_ui_focus_set_order(handle, new_order.data(), new_order.size());
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_order_count(
    sao_ui_focus_handle_t handle, size_t* out_count) {
    if (handle == nullptr || out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    prune_focus_state_locked(handle);
    *out_count = handle->order.size();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_order_at(
    sao_ui_focus_handle_t handle, size_t index, sao_ui_widget_handle_t* out_widget) {
    if (handle == nullptr || out_widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    prune_focus_state_locked(handle);
    if (index >= handle->order.size())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_widget = handle->order[index];
    return SAO_STATUS_OK;
}