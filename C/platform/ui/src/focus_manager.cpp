// SAO Auto — keyboard focus manager implementation.
//
// Owns the ordered list of focusable widget handles for the active
// panel and drives sao_ui_widget_set_focused on enter/leave so the
// existing focus ring (d2d_widgets.cpp paint_focus_ring, accent token)
// renders without any widget-side change.  Tab/Shift-Tab traversal
// wraps; set/clear/get are O(1).

#include "sao/ui/focus_manager.h"
#include "sao/ui/d2d_widgets.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

struct sao_ui_focus_s {
    std::mutex mu;
    std::vector<sao_ui_widget_handle_t> order;
    sao_ui_widget_handle_t focused{nullptr};
};

namespace {

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
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        clear_widget_focus(handle->focused);
    }
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_register(
    sao_ui_focus_handle_t handle, sao_ui_widget_handle_t widget) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (widget == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mu);
    if (std::find(handle->order.begin(), handle->order.end(), widget) != handle->order.end())
        return SAO_STATUS_OK;  // duplicate ignored
    handle->order.push_back(widget);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_unregister(
    sao_ui_focus_handle_t handle, sao_ui_widget_handle_t widget) {
    if (handle == nullptr || widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    const auto it = std::find(handle->order.begin(), handle->order.end(), widget);
    if (it == handle->order.end())
        return SAO_STATUS_ERR_NOT_FOUND;
    handle->order.erase(it);
    if (handle->focused == widget) {
        clear_widget_focus(widget);
        handle->focused = nullptr;
    }
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
    new_order.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (widgets[i] == nullptr)
            continue;
        if (std::find(new_order.begin(), new_order.end(), widgets[i]) != new_order.end())
            continue;  // de-dup, first occurrence wins
        new_order.push_back(widgets[i]);
    }
    std::lock_guard<std::mutex> lock(handle->mu);
    // If the focused widget is no longer in the order, clear it.
    if (handle->focused != nullptr &&
        std::find(new_order.begin(), new_order.end(), handle->focused) == new_order.end()) {
        clear_widget_focus(handle->focused);
        handle->focused = nullptr;
    }
    handle->order = std::move(new_order);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_clear(sao_ui_focus_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    clear_widget_focus(handle->focused);
    handle->focused = nullptr;
    handle->order.clear();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_set(
    sao_ui_focus_handle_t handle, sao_ui_widget_handle_t widget) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    if (widget != nullptr &&
        std::find(handle->order.begin(), handle->order.end(), widget) == handle->order.end())
        return SAO_STATUS_ERR_NOT_FOUND;
    if (handle->focused == widget)
        return SAO_STATUS_OK;
    clear_widget_focus(handle->focused);
    handle->focused = widget;
    set_widget_focus(widget);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_get(
    sao_ui_focus_handle_t handle, sao_ui_widget_handle_t* out_widget) {
    if (handle == nullptr || out_widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    *out_widget = handle->focused;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_tab_next(
    sao_ui_focus_handle_t handle, bool reverse) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
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
    sao_ui_widget_handle_t next = handle->order[index];
    if (handle->focused == next)
        return SAO_STATUS_OK;
    clear_widget_focus(handle->focused);
    handle->focused = next;
    set_widget_focus(next);
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
        std::lock_guard<std::mutex> lock(handle->mu);
        if (handle->focused != nullptr &&
            std::find(new_order.begin(), new_order.end(), handle->focused) == new_order.end()) {
            clear_widget_focus(handle->focused);
            handle->focused = nullptr;
        }
        handle->order = std::move(new_order);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_order_count(
    sao_ui_focus_handle_t handle, size_t* out_count) {
    if (handle == nullptr || out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    *out_count = handle->order.size();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_focus_order_at(
    sao_ui_focus_handle_t handle, size_t index, sao_ui_widget_handle_t* out_widget) {
    if (handle == nullptr || out_widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    if (index >= handle->order.size())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_widget = handle->order[index];
    return SAO_STATUS_OK;
}