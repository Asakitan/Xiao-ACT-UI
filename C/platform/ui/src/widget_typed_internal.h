#pragma once

#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

namespace sao::ui::detail {

using WidgetPropsJson = nlohmann::json;
using WidgetPropsSnapshot = std::shared_ptr<void>;

inline constexpr int32_t kWidgetInteractionHovered = 0;
inline constexpr int32_t kWidgetInteractionPressed = 1;
inline constexpr int32_t kWidgetInteractionFocused = 2;

inline bool widget_props_has_only(const WidgetPropsJson& props,
                                  std::initializer_list<std::string_view> allowed) {
    if (!props.is_object())
        return false;
    for (auto property = props.begin(); property != props.end(); ++property) {
        const std::string_view key(property.key());
        bool found = false;
        for (const std::string_view candidate : allowed) {
            if (candidate == key) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

inline bool widget_props_bool(const WidgetPropsJson& value, bool* out) {
    if (out == nullptr || !value.is_boolean())
        return false;
    *out = value.get<bool>();
    return true;
}

inline bool widget_props_i64(const WidgetPropsJson& value, int64_t* out) {
    if (out == nullptr || !value.is_number_integer())
        return false;
    try {
        *out = value.get<int64_t>();
        return true;
    } catch (...) {
        return false;
    }
}

inline bool widget_props_i32(const WidgetPropsJson& value, int32_t* out) {
    int64_t parsed = 0;
    if (out == nullptr || !widget_props_i64(value, &parsed) ||
        parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *out = static_cast<int32_t>(parsed);
    return true;
}

inline bool widget_props_size(const WidgetPropsJson& value, size_t* out) {
    if (out == nullptr || !value.is_number_unsigned())
        return false;
    try {
        const uint64_t parsed = value.get<uint64_t>();
        if (parsed > std::numeric_limits<size_t>::max())
            return false;
        *out = static_cast<size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool widget_props_double(const WidgetPropsJson& value, double* out) {
    if (out == nullptr || !value.is_number())
        return false;
    try {
        const double parsed = value.get<double>();
        if (!std::isfinite(parsed))
            return false;
        *out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

inline bool widget_props_float(const WidgetPropsJson& value, float* out) {
    double parsed = 0.0;
    if (out == nullptr || !widget_props_double(value, &parsed) ||
        parsed < static_cast<double>(std::numeric_limits<float>::lowest()) ||
        parsed > static_cast<double>(std::numeric_limits<float>::max())) {
        return false;
    }
    *out = static_cast<float>(parsed);
    return true;
}

inline bool widget_props_argb(const WidgetPropsJson& value, uint32_t* out) {
    if (out == nullptr)
        return false;
    if (value.is_number_unsigned()) {
        try {
            const uint64_t parsed = value.get<uint64_t>();
            if (parsed > std::numeric_limits<uint32_t>::max())
                return false;
            *out = static_cast<uint32_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    }
    if (!value.is_string())
        return false;
    const std::string& text = value.get_ref<const std::string&>();
    if ((text.size() != 7U && text.size() != 9U) || text[0] != '#')
        return false;
    uint32_t parsed = 0;
    for (size_t index = 1; index < text.size(); ++index) {
        const char character = text[index];
        uint32_t digit = 0;
        if (character >= '0' && character <= '9')
            digit = static_cast<uint32_t>(character - '0');
        else if (character >= 'a' && character <= 'f')
            digit = static_cast<uint32_t>(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F')
            digit = static_cast<uint32_t>(character - 'A' + 10);
        else
            return false;
        parsed = (parsed << 4U) | digit;
    }
    *out = text.size() == 7U ? 0xff000000U | parsed : ((parsed & 0xffU) << 24U) | (parsed >> 8U);
    return true;
}

bool is_typed_widget_handle(sao_ui_widget_handle_t handle) noexcept;

sao_status_t apply_typed_widget_props(sao_ui_widget_handle_t handle, const WidgetPropsJson& props,
                                      WidgetPropsSnapshot* out_snapshot) noexcept;

sao_status_t restore_typed_widget_props(sao_ui_widget_handle_t handle,
                                        const WidgetPropsSnapshot& snapshot) noexcept;

sao_status_t paint_typed_widget(sao_ui_widget_handle_t handle, sao_ui_paint_ctx_handle_t context,
                                int32_t x, int32_t y, int32_t width, int32_t height) noexcept;

sao_status_t widget_text_apply_props(sao_ui_widget_handle_t handle, int32_t kind,
                                     const WidgetPropsJson& props,
                                     WidgetPropsSnapshot* out_snapshot) noexcept;
sao_status_t widget_text_restore_props(sao_ui_widget_handle_t handle, int32_t kind,
                                       const WidgetPropsSnapshot& snapshot) noexcept;
sao_status_t widget_text_paint(sao_ui_widget_handle_t handle, int32_t kind,
                               sao_ui_paint_ctx_handle_t context, int32_t x, int32_t y,
                               int32_t width, int32_t height) noexcept;

sao_status_t widget_input_apply_props(sao_ui_widget_handle_t handle, int32_t kind,
                                      const WidgetPropsJson& props,
                                      WidgetPropsSnapshot* out_snapshot) noexcept;
sao_status_t widget_input_restore_props(sao_ui_widget_handle_t handle, int32_t kind,
                                        const WidgetPropsSnapshot& snapshot) noexcept;
sao_status_t widget_input_paint(sao_ui_widget_handle_t handle, int32_t kind,
                                sao_ui_paint_ctx_handle_t context, int32_t x, int32_t y,
                                int32_t width, int32_t height) noexcept;

sao_status_t widget_input_set_interaction_state(sao_ui_widget_handle_t handle, int32_t state,
                                                bool value) noexcept;
sao_status_t widget_input_nudge_value(sao_ui_widget_handle_t handle, int32_t direction) noexcept;

sao_status_t widget_data_apply_props(sao_ui_widget_handle_t handle, int32_t kind,
                                     const WidgetPropsJson& props,
                                     WidgetPropsSnapshot* out_snapshot) noexcept;
sao_status_t widget_data_restore_props(sao_ui_widget_handle_t handle, int32_t kind,
                                       const WidgetPropsSnapshot& snapshot) noexcept;
sao_status_t widget_data_paint(sao_ui_widget_handle_t handle, int32_t kind,
                               sao_ui_paint_ctx_handle_t context, int32_t x, int32_t y,
                               int32_t width, int32_t height) noexcept;

sao_status_t widget_chart_apply_props(sao_ui_widget_handle_t handle, int32_t kind,
                                      const WidgetPropsJson& props,
                                      WidgetPropsSnapshot* out_snapshot) noexcept;
sao_status_t widget_chart_restore_props(sao_ui_widget_handle_t handle, int32_t kind,
                                        const WidgetPropsSnapshot& snapshot) noexcept;
sao_status_t widget_chart_paint(sao_ui_widget_handle_t handle, int32_t kind,
                                sao_ui_paint_ctx_handle_t context, int32_t x, int32_t y,
                                int32_t width, int32_t height) noexcept;

sao_status_t widget_table_apply_props(sao_ui_widget_handle_t handle, int32_t kind,
                                      const WidgetPropsJson& props,
                                      WidgetPropsSnapshot* out_snapshot) noexcept;
sao_status_t widget_table_restore_props(sao_ui_widget_handle_t handle, int32_t kind,
                                        const WidgetPropsSnapshot& snapshot) noexcept;
sao_status_t widget_table_paint(sao_ui_widget_handle_t handle, int32_t kind,
                                sao_ui_paint_ctx_handle_t context, int32_t x, int32_t y,
                                int32_t width, int32_t height) noexcept;

} // namespace sao::ui::detail
