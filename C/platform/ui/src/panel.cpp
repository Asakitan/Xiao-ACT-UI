// SAO Auto — generic spec-driven panel backed by a compositor layer.

#include "sao/ui/panel.h"

#include "sao/engine/ui_spec.h"
#include "sao/ui/sound.h"
#include "sao/ui/theme.h"
#include "sao/ui/widget_input.h"
#include "sao/ui/widget_kit.h"

#include "panel_theme_internal.h"
#include "panel_viewport_internal.h"
#include "native_text_edit.h"
#include "widget_paint_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_apply_theme_override_(
    sao_ui_panel_handle_t panel, const uint8_t* override_json_utf8, size_t override_len);
extern "C" bool SAO_UI_CALL
sao_ui_panel_runtime_is_registered_(sao_ui_panel_handle_t panel);
extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_runtime_find_(
    sao_ui_compositor_handle_t compositor, const char* panel_id_utf8,
    sao_ui_panel_handle_t* out_panel);
extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_runtime_publish_(
    sao_ui_panel_handle_t panel, sao_ui_compositor_handle_t compositor,
    const char* panel_id_utf8, bool single_instance, sao_ui_panel_handle_t* out_panel);
extern "C" bool SAO_UI_CALL sao_ui_panel_runtime_retire_(sao_ui_panel_handle_t panel);
extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_runtime_enumerate_(
    sao_ui_compositor_handle_t compositor, sao_ui_panel_handle_t* out_panels,
    size_t capacity, size_t* out_written);
extern "C" void SAO_UI_CALL sao_ui_panel_runtime_destroy_(sao_ui_panel_handle_t panel);
extern "C" void SAO_UI_CALL sao_ui_panel_destroy_through_sdk_(
    sao_ui_panel_handle_t panel);

namespace {

using json = nlohmann::json;
std::atomic<int32_t> g_body_replace_failure_point{0};
std::atomic<int32_t> g_theme_upload_failure_count{0};
std::atomic<int32_t> g_create_theme_switch{-1};
constexpr size_t kMaximumPanelSpecBytes = 1U << 20U;
constexpr int32_t kMaximumPanelSpecDepth = 8;
constexpr size_t kMaximumPanelSpecNodes = 400U;

int64_t saturating_add_i64(int64_t left, int64_t right) noexcept {
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        return std::numeric_limits<int64_t>::max();
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
        return std::numeric_limits<int64_t>::min();
    return left + right;
}

int64_t saturating_sub_i64(int64_t left, int64_t right) noexcept {
    if (right > 0 && left < std::numeric_limits<int64_t>::min() + right)
        return std::numeric_limits<int64_t>::min();
    if (right < 0 && left > std::numeric_limits<int64_t>::max() + right)
        return std::numeric_limits<int64_t>::max();
    return left - right;
}

int32_t clamp_i64_to_i32(int64_t value) noexcept {
    return static_cast<int32_t>(std::clamp(
        value, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
        static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
}

int64_t rounded_finite_long_double_to_i64(long double value) noexcept {
    const long double rounded = std::round(value);
    if (rounded <= static_cast<long double>(std::numeric_limits<int64_t>::min()))
        return std::numeric_limits<int64_t>::min();
    if (rounded >= static_cast<long double>(std::numeric_limits<int64_t>::max()))
        return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(rounded);
}

int32_t saturating_float_to_i32(float value) noexcept {
    if (std::isnan(value))
        return 0;
    if (value <= static_cast<float>(std::numeric_limits<int32_t>::min()))
        return std::numeric_limits<int32_t>::min();
    if (value >= static_cast<float>(std::numeric_limits<int32_t>::max()))
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(value);
}

int32_t titlebar_height(const sao::ui::detail::PanelResolvedTheme& theme) noexcept {
    const int64_t padding = theme.metrics[SAO_UI_METRIC_PADDING_L];
    return clamp_i64_to_i32(saturating_add_i64(padding, padding));
}
bool parse_argb(const json& value, uint32_t* out_argb) {
    if (out_argb == nullptr)
        return false;
    if (value.is_number_unsigned()) {
        const uint64_t raw = value.get<uint64_t>();
        if (raw > UINT32_MAX)
            return false;
        *out_argb = static_cast<uint32_t>(raw);
        return true;
    }
    if (!value.is_string())
        return false;
    const std::string& text = value.get_ref<const std::string&>();
    if ((text.size() != 7U && text.size() != 9U) || text.front() != '#')
        return false;
    uint32_t parsed = 0;
    for (size_t index = 1; index < text.size(); ++index) {
        const char ch = text[index];
        uint32_t nibble = 0;
        if (ch >= '0' && ch <= '9')
            nibble = static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            nibble = static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            nibble = static_cast<uint32_t>(ch - 'A' + 10);
        else
            return false;
        parsed = (parsed << 4U) | nibble;
    }
    *out_argb = text.size() == 7U ? 0xff000000U | parsed
                                  : ((parsed & 0xffU) << 24U) | (parsed >> 8U);
    return true;
}

std::optional<SaoUiColorToken> color_token_for_name(std::string_view name) noexcept {
    std::array<char, 64> token_name{};
    for (int32_t index = 0; index < SAO_UI_COLOR_TOKEN_COUNT; ++index) {
        const auto token = static_cast<SaoUiColorToken>(index);
        if (sao_ui_theme_get_token_name(token, token_name.data(), token_name.size()) ==
                SAO_STATUS_OK &&
            name == token_name.data()) {
            return token;
        }
    }
    return std::nullopt;
}

struct PanelThemeOverrides {
    std::array<uint32_t, SAO_UI_COLOR_TOKEN_COUNT> colors{};
    std::array<bool, SAO_UI_COLOR_TOKEN_COUNT> has_color{};
};

sao_status_t parse_theme_overrides(const uint8_t* bytes, size_t length,
                                   PanelThemeOverrides* out_overrides) {
    if (out_overrides == nullptr || (bytes == nullptr && length != 0U))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelThemeOverrides parsed{};
    if (bytes == nullptr || length == 0U) {
        *out_overrides = parsed;
        return SAO_STATUS_OK;
    }
    try {
        const json document = json::parse(bytes, bytes + length, nullptr, false, false);
        if (document.is_discarded() || !document.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const json* colors = &document;
        if (const auto nested = document.find("colors"); nested != document.end()) {
            if (!nested->is_object())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            colors = &*nested;
        }
        for (auto property = colors->begin(); property != colors->end(); ++property) {
            const auto token = color_token_for_name(property.key());
            if (!token.has_value())
                continue;
            uint32_t argb = 0;
            if (!parse_argb(property.value(), &argb))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            const size_t index = static_cast<size_t>(*token);
            parsed.colors[index] = argb;
            parsed.has_color[index] = true;
        }
        *out_overrides = parsed;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

std::optional<SaoUiColorToken> semantic_accent_token(std::string_view value) noexcept {
    if (value == "cyan" || value == "accent" || value == "info")
        return SAO_UI_TOKEN_APP_ACCENT;
    if (value == "gold" || value == "warn")
        return SAO_UI_TOKEN_APP_GOLD;
    if (value == "ok" || value == "good" || value == "heal")
        return SAO_UI_TOKEN_APP_GREEN;
    if (value == "bad" || value == "danger" || value == "error")
        return SAO_UI_TOKEN_APP_RED;
    if (value == "muted" || value == "dim")
        return SAO_UI_TOKEN_APP_TEXT_DIM;
    return std::nullopt;
}

bool parse_container_accent(const json& value, const sao::ui::detail::PanelResolvedTheme& theme,
                            uint32_t* out_color, SaoUiColorToken* out_token,
                            bool* out_explicit) {
    if (out_color == nullptr || out_token == nullptr || out_explicit == nullptr ||
        !value.is_string())
        return false;
    std::string text = value.get<std::string>();
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (const auto token = semantic_accent_token(text); token.has_value()) {
        *out_token = *token;
        *out_color = theme.colors[static_cast<size_t>(*token)];
        *out_explicit = false;
        return true;
    }
    if (!parse_argb(value, out_color))
        return false;
    *out_token = SAO_UI_TOKEN_APP_ACCENT;
    *out_explicit = true;
    return true;
}

bool parse_nonnegative_int(const json& node, const char* key, int32_t* out_value) {
    const auto property = node.find(key);
    if (property == node.end())
        return true;
    if (!property->is_number_integer())
        return false;
    const int64_t value = property->get<int64_t>();
    if (value < 0 || value > std::numeric_limits<int32_t>::max())
        return false;
    *out_value = static_cast<int32_t>(value);
    return true;
}

bool apply_container_layout_metadata(const json& node, SaoUiLayoutSpec* spec) {
    if (!node.is_object() || spec == nullptr)
        return false;
    const auto set_size = [&node](const char* primary, const char* alias, int32_t* target) {
        const auto property = node.find(primary);
        const auto fallback = property == node.end() ? node.find(alias) : property;
        if (fallback == node.end())
            return true;
        if (!fallback->is_number_integer())
            return false;
        const int64_t value = fallback->get<int64_t>();
        if (value < 0 || value > std::numeric_limits<int32_t>::max())
            return false;
        *target = static_cast<int32_t>(value);
        return true;
    };
    if (!set_size("width", "fixed_width", &spec->fixed_width_px) ||
        !set_size("height", "fixed_height", &spec->fixed_height_px) ||
        !parse_nonnegative_int(node, "min_width", &spec->min_width_px) ||
        !parse_nonnegative_int(node, "min_height", &spec->min_height_px) ||
        !parse_nonnegative_int(node, "max_width", &spec->max_width_px) ||
        !parse_nonnegative_int(node, "max_height", &spec->max_height_px) ||
        !parse_nonnegative_int(node, "gap", &spec->gap_px))
        return false;
    const auto weight = node.find("weight");
    if (weight != node.end()) {
        if (!weight->is_number())
            return false;
        const double value = weight->get<double>();
        if (!std::isfinite(value) || value < 0.0 ||
            value > static_cast<double>(std::numeric_limits<float>::max()))
            return false;
        spec->weight = static_cast<float>(value);
    }
    const auto padding = node.find("padding");
    if (padding != node.end()) {
        if (padding->is_number_integer()) {
            const int64_t value = padding->get<int64_t>();
            if (value < 0 || value > std::numeric_limits<int32_t>::max())
                return false;
            spec->pad_top_px = spec->pad_right_px = spec->pad_bottom_px =
                spec->pad_left_px = static_cast<int32_t>(value);
        } else if (padding->is_object()) {
            if (!parse_nonnegative_int(*padding, "top", &spec->pad_top_px) ||
                !parse_nonnegative_int(*padding, "right", &spec->pad_right_px) ||
                !parse_nonnegative_int(*padding, "bottom", &spec->pad_bottom_px) ||
                !parse_nonnegative_int(*padding, "left", &spec->pad_left_px))
                return false;
        } else {
            return false;
        }
    }
    if (const auto clip = node.find("clip"); clip != node.end()) {
        if (!clip->is_boolean())
            return false;
        spec->clip_children = clip->get<bool>();
    }
    if (const auto dock = node.find("dock"); dock != node.end()) {
        if (!dock->is_string())
            return false;
        std::string side = dock->get<std::string>();
        std::transform(side.begin(), side.end(), side.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (side == "top")
            spec->dock_side = SAO_UI_DOCK_TOP;
        else if (side == "right")
            spec->dock_side = SAO_UI_DOCK_RIGHT;
        else if (side == "bottom")
            spec->dock_side = SAO_UI_DOCK_BOTTOM;
        else if (side == "left")
            spec->dock_side = SAO_UI_DOCK_LEFT;
        else if (side == "fill" || side == "center")
            spec->dock_side = SAO_UI_DOCK_FILL;
        else
            return false;
    }
    return !((spec->max_width_px > 0 && spec->max_width_px < spec->min_width_px) ||
             (spec->max_height_px > 0 && spec->max_height_px < spec->min_height_px) ||
             (spec->fixed_width_px > 0 && spec->fixed_width_px < spec->min_width_px) ||
             (spec->fixed_height_px > 0 && spec->fixed_height_px < spec->min_height_px));
}

struct ContainerLayoutMetadata {
    SaoUiLayoutSpec explicit_spec{};
    bool fixed_width{};
    bool fixed_height{};
    bool min_width{};
    bool min_height{};
    bool max_width{};
    bool max_height{};
    bool weight{};
    bool gap{};
    bool pad_top{};
    bool pad_right{};
    bool pad_bottom{};
    bool pad_left{};
};

ContainerLayoutMetadata capture_container_layout_metadata(
    const json& node, const SaoUiLayoutSpec& spec) {
    ContainerLayoutMetadata metadata{};
    metadata.explicit_spec = spec;
    metadata.fixed_width = node.contains("width") ||
                           (!node.contains("width") && node.contains("fixed_width"));
    metadata.fixed_height = node.contains("height") ||
                            (!node.contains("height") && node.contains("fixed_height"));
    metadata.min_width = node.contains("min_width");
    metadata.min_height = node.contains("min_height");
    metadata.max_width = node.contains("max_width");
    metadata.max_height = node.contains("max_height");
    metadata.weight = node.contains("weight");
    metadata.gap = node.contains("gap");
    const auto padding = node.find("padding");
    if (padding != node.end()) {
        if (padding->is_number_integer()) {
            metadata.pad_top = metadata.pad_right = metadata.pad_bottom = metadata.pad_left = true;
        } else if (padding->is_object()) {
            metadata.pad_top = padding->contains("top");
            metadata.pad_right = padding->contains("right");
            metadata.pad_bottom = padding->contains("bottom");
            metadata.pad_left = padding->contains("left");
        }
    }
    return metadata;
}

void apply_explicit_container_layout_metadata(const ContainerLayoutMetadata& metadata,
                                              SaoUiLayoutSpec* spec) noexcept {
    if (metadata.fixed_width) spec->fixed_width_px = metadata.explicit_spec.fixed_width_px;
    if (metadata.fixed_height) spec->fixed_height_px = metadata.explicit_spec.fixed_height_px;
    if (metadata.min_width) spec->min_width_px = metadata.explicit_spec.min_width_px;
    if (metadata.min_height) spec->min_height_px = metadata.explicit_spec.min_height_px;
    if (metadata.max_width) spec->max_width_px = metadata.explicit_spec.max_width_px;
    if (metadata.max_height) spec->max_height_px = metadata.explicit_spec.max_height_px;
    if (metadata.weight) spec->weight = metadata.explicit_spec.weight;
    if (metadata.gap) spec->gap_px = metadata.explicit_spec.gap_px;
    if (metadata.pad_top) spec->pad_top_px = metadata.explicit_spec.pad_top_px;
    if (metadata.pad_right) spec->pad_right_px = metadata.explicit_spec.pad_right_px;
    if (metadata.pad_bottom) spec->pad_bottom_px = metadata.explicit_spec.pad_bottom_px;
    if (metadata.pad_left) spec->pad_left_px = metadata.explicit_spec.pad_left_px;
}

struct ContainerResponsiveLayout {
    int32_t primary_mode{SAO_UI_LAYOUT_VERTICAL};
    int32_t fallback_mode{SAO_UI_LAYOUT_VERTICAL};
    int32_t threshold_width{};
    int32_t min_width{};
    bool has_fallback{};
};

int32_t parse_container_layout_mode(std::string_view value, int32_t fallback) noexcept {
    if (value == "horizontal")
        return SAO_UI_LAYOUT_HORIZONTAL;
    if (value == "vertical")
        return SAO_UI_LAYOUT_VERTICAL;
    if (value == "grid")
        return SAO_UI_LAYOUT_GRID;
    if (value == "absolute")
        return SAO_UI_LAYOUT_ABSOLUTE;
    if (value == "flex")
        return SAO_UI_LAYOUT_FLEX;
    if (value == "dock")
        return SAO_UI_LAYOUT_DOCK;
    return fallback;
}

bool parse_scroll_metadata(const json& node, int* out_axis, bool* out_wheel, bool* out_bar) {
    *out_axis = 0;
    *out_wheel = true;
    *out_bar = true;
    const auto scroll = node.find("scroll");
    if (scroll == node.end())
        return true;
    if (!scroll->is_object())
        return false;
    const auto axis_property = scroll->find("axis");
    if (axis_property == scroll->end() || !axis_property->is_string())
        return false;
    std::string axis = axis_property->get<std::string>();
    std::transform(axis.begin(), axis.end(), axis.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (axis == "horizontal" || axis == "x")
        *out_axis = 1;
    else if (axis == "vertical" || axis == "y")
        *out_axis = 2;
    else if (axis == "both" || axis == "xy")
        *out_axis = 3;
    else if (axis != "none")
        return false;
    if (const auto wheel = scroll->find("wheel"); wheel != scroll->end()) {
        if (!wheel->is_boolean())
            return false;
        *out_wheel = wheel->get<bool>();
    }
    if (const auto bar = scroll->find("bar"); bar != scroll->end()) {
        if (bar->is_boolean())
            *out_bar = bar->get<bool>();
        else if (bar->is_string()) {
            std::string value = bar->get<std::string>();
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (value == "auto" || value == "always")
                *out_bar = true;
            else if (value == "none" || value == "hidden")
                *out_bar = false;
            else
                return false;
        } else
            return false;
    }
    return true;
}

bool parse_container_responsive_layout(const json& node, int32_t primary_mode,
                                       ContainerResponsiveLayout* out_layout) {
    if (out_layout == nullptr)
        return false;
    ContainerResponsiveLayout parsed{};
    parsed.primary_mode = primary_mode;
    const auto fallback = node.find("fallback");
    if (fallback == node.end()) {
        *out_layout = parsed;
        return true;
    }
    if (!fallback->is_object())
        return false;
    parsed.has_fallback = true;
    const auto layout = fallback->find("layout");
    if (layout != fallback->end()) {
        if (!layout->is_string())
            return false;
        std::string normalized = layout->get<std::string>();
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        parsed.fallback_mode = parse_container_layout_mode(normalized, -1);
        if (parsed.fallback_mode < 0)
            return false;
    }
    if (!parse_nonnegative_int(*fallback, "threshold_width", &parsed.threshold_width) ||
        !parse_nonnegative_int(*fallback, "min_width", &parsed.min_width))
        return false;
    *out_layout = parsed;
    return true;
}

int32_t resolve_container_layout_mode(const ContainerResponsiveLayout& layout,
                                      int32_t viewport_width) noexcept {
    if (!layout.has_fallback)
        return layout.primary_mode;
    const int32_t threshold = layout.threshold_width > 0 ? layout.threshold_width
                                                          : layout.min_width;
    if (threshold <= 0)
        return layout.primary_mode;
    return viewport_width >= threshold ? layout.primary_mode : layout.fallback_mode;
}

struct OwnedWidget {
    struct DropdownItem {
        int32_t id{};
        std::string label;
        std::string value;
        bool enabled{true};
        bool checked{};
    };
    sao_ui_widget_handle_t handle{};
    sao_ui_layout_node_handle_t node{};
    int32_t kind{};
    std::string type;
    std::string id;
    std::string path;
    std::string action;
    std::string action_args{"{}"};
    json props{json::object()};
    std::vector<DropdownItem> dropdown_items;
    int32_t dropdown_selected_id{};
    float slider_value{};
    bool enabled{true};
    bool owns_handle{true};

    ~OwnedWidget() {
        if (owns_handle)
            sao_ui_widget_destroy(handle);
    }
};

void SAO_UI_CALL panel_slider_changed(float value, void* user_data) {
    auto* widget = static_cast<OwnedWidget*>(user_data);
    if (widget == nullptr)
        return;
    widget->slider_value = value;
    widget->props["value"] = value;
    widget->props["text"] = value;
}

void SAO_UI_CALL panel_dropdown_picked(int32_t item_id, void* user_data) {
    auto* widget = static_cast<OwnedWidget*>(user_data);
    if (widget == nullptr)
        return;
    widget->dropdown_selected_id = item_id;
    widget->props["selected_id"] = item_id;
    const auto found = std::find_if(widget->dropdown_items.begin(), widget->dropdown_items.end(),
                                    [item_id](const auto& item) { return item.id == item_id; });
    if (found != widget->dropdown_items.end()) {
        widget->props["value"] = found->value;
        widget->props["text"] = found->label;
    }
}

sao_status_t apply_owned_widget_props(OwnedWidget* widget) {
    if (widget == nullptr || widget->handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (widget->type == "slider") {
        const float value = widget->props.value("value", widget->slider_value);
        return sao_ui_slider_set_value(widget->handle, value);
    }
    if (widget->type == "dropdown") {
        const int32_t selected = widget->props.value("selected_id", widget->dropdown_selected_id);
        return sao_ui_dropdown_button_select(widget->handle, selected);
    }
    const std::string props = widget->props.dump();
    return sao_ui_widget_apply_props(widget->handle, reinterpret_cast<const uint8_t*>(props.data()),
                                     props.size());
}

struct PanelContent {
    struct ThemeLayoutNode {
        sao_ui_layout_node_handle_t node{};
        SaoUiLayoutSpec spec{};
        ContainerLayoutMetadata explicit_metadata{};
        ContainerResponsiveLayout responsive{};
        int32_t active_mode{SAO_UI_LAYOUT_VERTICAL};
        std::string semantic_type;
        bool has_title{};
    };

    struct ContainerVisual {
        sao_ui_layout_node_handle_t node{};
        int32_t kind{};
        std::string semantic_type;
        std::string parent_type;
        std::string title;
        uint32_t accent{};
        SaoUiColorToken accent_token{SAO_UI_TOKEN_APP_ACCENT};
        bool accent_is_explicit{};
        ContainerResponsiveLayout responsive{};
        int32_t active_mode{SAO_UI_LAYOUT_VERTICAL};
        bool has_title{};
    };

    sao_ui_layout_tree_handle_t tree{};
    sao_ui_layout_node_handle_t root{};
    SaoUiLayoutSpec root_spec{};
    std::vector<std::unique_ptr<OwnedWidget>> widgets;
    std::unordered_map<std::string, OwnedWidget*> by_id;
    std::unordered_map<sao_ui_widget_handle_t, OwnedWidget*> by_handle;
    std::vector<ThemeLayoutNode> theme_layout_nodes;
    std::vector<ContainerVisual> containers;
    sao::ui::detail::PanelResolvedTheme layout_theme;
    bool uses_theme_layout_metrics{};
    bool explicit_viewports{};
    int32_t scroll_offset_px{};
    int32_t content_extent_px{};
    int32_t panel_width_px{};
    int32_t viewport_top_px{};
    int32_t viewport_height_px{};
    std::mutex mutex;

    ~PanelContent() {
        sao_ui_layout_tree_destroy(tree);
    }
};

struct PanelFrame {
    std::vector<uint8_t> pixels;
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
};

constexpr size_t kPanelCallbackKindCount = 4U;

struct ActiveTextEditAnchor {
    sao_ui_panel_s* panel{};
    sao_ui_widget_handle_t widget{};
    void* hwnd{};
    std::string widget_id;
};

std::mutex& text_edit_anchor_mutex() {
    static std::mutex mutex;
    return mutex;
}

ActiveTextEditAnchor& text_edit_anchor() {
    static ActiveTextEditAnchor anchor;
    return anchor;
}

} // namespace

struct sao_ui_panel_s {
    sao_ui_compositor_handle_t compositor{};
    sao_ui_layer_handle_t layer{};
    std::string id;
    std::string title;
    std::string theme_page;
    SaoPanelState state{};
    bool show_titlebar{true};
    bool show_close_button{};
    bool movable{};
    bool resizable{};
    int32_t interaction_mode{};
    int32_t resize_edges{};
    int32_t pointer_down_x{};
    int32_t pointer_down_y{};
    int32_t pointer_x{};
    int32_t pointer_y{};
    SaoPanelState interaction_start_state{};
    sao_ui_widget_handle_t hovered_widget{};
    sao_ui_widget_handle_t pressed_widget{};
    sao_ui_widget_handle_t focused_widget{};
    sao_ui_widget_handle_t slider_drag_widget{};
    std::string responsive_focus_id;
    std::string responsive_focus_path;
    bool close_armed{};
    bool scrollbar_hovered{};
    bool scrollbar_pressed{};
    int32_t scrollbar_drag_start_y{};
    int32_t scrollbar_drag_start_offset{};
    int32_t min_width{};
    int32_t min_height{};
    int32_t max_width{};
    int32_t max_height{};
    std::shared_ptr<PanelContent> content;
    sao_ui_panel_action_callback_t action_callback{};
    void* action_user_data{};
    sao_ui_panel_event_callback_t event_callback{};
    void* event_user_data{};
    sao_ui_panel_render_fn_t render_callback{};
    void* render_user_data{};
    std::string spec_json;
    std::mutex mutex;
    std::recursive_mutex render_mutex;
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    size_t operations_in_flight{};
    size_t callbacks_in_flight{};
    std::array<uint64_t, kPanelCallbackKindCount> callback_generations{1, 1, 1, 1};
    std::array<std::unordered_map<uint64_t, size_t>, kPanelCallbackKindCount>
        callback_in_flight_by_generation;
    bool accepting_operations{true};
    bool destroy_requested{};
    bool finalizing{};
    bool finalized{};
    bool stopping{};
    PanelThemeOverrides theme_overrides;
    uint64_t requested_theme_generation{};
    uint64_t uploaded_theme_generation{};
    bool theme_dirty{true};
    sao_ui_theme_callback_handle_t theme_callback_handle{
        SAO_UI_THEME_CALLBACK_HANDLE_INVALID};

    ~sao_ui_panel_s() {
        void* edit_hwnd = nullptr;
        sao_ui_widget_handle_t edit_widget = nullptr;
        {
            std::lock_guard lock(text_edit_anchor_mutex());
            if (text_edit_anchor().panel == this) {
                edit_hwnd = text_edit_anchor().hwnd;
                edit_widget = text_edit_anchor().widget;
                text_edit_anchor() = {};
            }
        }
        if (edit_hwnd != nullptr && edit_widget != nullptr)
            sao::ui::detail::end_native_text_edit_for_widget(edit_hwnd, edit_widget, true);
        if (theme_callback_handle != SAO_UI_THEME_CALLBACK_HANDLE_INVALID) {
            (void)sao_ui_theme_unregister_change_callback(theme_callback_handle);
            theme_callback_handle = SAO_UI_THEME_CALLBACK_HANDLE_INVALID;
        }
        if (layer != nullptr) {
            (void)sao_ui_layer_set_input_callbacks(layer, nullptr, nullptr, nullptr, nullptr,
                                                   nullptr);
            sao_ui_layer_destroy(layer);
        }
    }
};

namespace {

bool panel_has_active_text_edit(sao_ui_panel_s* panel) {
    std::lock_guard lock(text_edit_anchor_mutex());
    return text_edit_anchor().panel == panel;
}

void end_panel_text_edit(sao_ui_panel_s* panel, bool commit) {
    void* hwnd = nullptr;
    sao_ui_widget_handle_t widget = nullptr;
    {
        std::lock_guard lock(text_edit_anchor_mutex());
        if (text_edit_anchor().panel != panel)
            return;
        hwnd = text_edit_anchor().hwnd;
        widget = text_edit_anchor().widget;
    }
    if (hwnd != nullptr && widget != nullptr)
        sao::ui::detail::end_native_text_edit_for_widget(hwnd, widget, commit);
    {
        std::lock_guard lock(text_edit_anchor_mutex());
        if (text_edit_anchor().panel == panel && text_edit_anchor().widget == widget)
            text_edit_anchor() = {};
    }
}

std::mutex& panel_storage_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::unique_ptr<sao_ui_panel_s>>& panel_storage() {
    static std::vector<std::unique_ptr<sao_ui_panel_s>> values;
    return values;
}

void erase_panel_storage(sao_ui_panel_s* panel) {
    std::unique_ptr<sao_ui_panel_s> removed;
    {
        std::lock_guard lock(panel_storage_mutex());
        const auto found = std::ranges::find_if(
            panel_storage(), [panel](const std::unique_ptr<sao_ui_panel_s>& candidate) {
                return candidate.get() == panel;
            });
        if (found != panel_storage().end()) {
            removed = std::move(*found);
            panel_storage().erase(found);
        }
    }
}

enum class CallbackKind : size_t { Action = 0, Event = 1, Render = 2, Theme = 3, Count = 4 };

static_assert(static_cast<size_t>(CallbackKind::Count) == kPanelCallbackKindCount);

struct ActiveCallback {
    sao_ui_panel_s* panel{};
    CallbackKind kind{};
    uint64_t generation{};
    ActiveCallback* previous{};
};

thread_local ActiveCallback* active_callback = nullptr;

struct ActiveRender {
    sao_ui_panel_s* panel{};
    ActiveRender* previous{};
};

thread_local ActiveRender* active_render = nullptr;

bool callback_is_active(sao_ui_panel_s* panel, CallbackKind kind, uint64_t generation) {
    for (const ActiveCallback* active = active_callback; active != nullptr;
         active = active->previous) {
        if (active->panel == panel && active->kind == kind && active->generation == generation)
            return true;
    }
    return false;
}

bool claim_finalization(sao_ui_panel_s* panel) {
    std::lock_guard lock(panel->lifecycle_mutex);
    if (!panel->destroy_requested || panel->finalizing || panel->finalized ||
        panel->operations_in_flight != 0 || panel->callbacks_in_flight != 0) {
        return false;
    }
    panel->finalizing = true;
    return true;
}

void finalize_panel(sao_ui_panel_s* panel) noexcept {
    try {
        const auto theme_callback = std::exchange(
            panel->theme_callback_handle, SAO_UI_THEME_CALLBACK_HANDLE_INVALID);
        if (theme_callback != SAO_UI_THEME_CALLBACK_HANDLE_INVALID) {
            (void)sao_ui_theme_unregister_change_callback(theme_callback);
        }
        sao_ui_layer_handle_t layer = nullptr;
        {
            std::lock_guard lock(panel->mutex);
            layer = std::exchange(panel->layer, nullptr);
            panel->content.reset();
            panel->action_callback = nullptr;
            panel->action_user_data = nullptr;
            panel->event_callback = nullptr;
            panel->event_user_data = nullptr;
            panel->render_callback = nullptr;
            panel->render_user_data = nullptr;
        }
        if (layer != nullptr) {
            (void)sao_ui_layer_set_input_callbacks(layer, nullptr, nullptr, nullptr, nullptr,
                                                   nullptr);
            (void)sao_ui_layer_set_input_enabled(layer, false);
            sao_ui_layer_destroy(layer);
        }
    } catch (...) {
    }
    {
        std::lock_guard lock(panel->lifecycle_mutex);
        panel->finalized = true;
        panel->finalizing = false;
    }
    panel->lifecycle_cv.notify_all();
}

void finalize_if_ready(sao_ui_panel_s* panel) noexcept {
    if (callback_is_active(panel, CallbackKind::Theme, 0))
        return;
    if (claim_finalization(panel))
        finalize_panel(panel);
}

void retry_dirty_theme(sao_ui_panel_s* panel) noexcept;

class PanelOperation {
  public:
    explicit PanelOperation(sao_ui_panel_s* panel) : panel_(panel) {
        if (panel_ == nullptr)
            return;
        try {
            if (!sao_ui_panel_runtime_is_registered_(panel_))
                return;
            std::lock_guard lifecycle_lock(panel_->lifecycle_mutex);
            if (!panel_->accepting_operations)
                return;
            ++panel_->operations_in_flight;
            acquired_ = true;
        } catch (...) {
        }
    }

    PanelOperation(const PanelOperation&) = delete;
    PanelOperation& operator=(const PanelOperation&) = delete;

    ~PanelOperation() {
        if (!acquired_)
            return;
        retry_dirty_theme(panel_);
        {
            std::lock_guard lock(panel_->lifecycle_mutex);
            --panel_->operations_in_flight;
        }
        panel_->lifecycle_cv.notify_all();
        finalize_if_ready(panel_);
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

  private:
    sao_ui_panel_s* panel_{};
    bool acquired_{};
};

class PanelCallbackLease {
  public:
    PanelCallbackLease(sao_ui_panel_s* panel, CallbackKind kind) : panel_(panel), kind_(kind) {
        try {
            std::lock_guard lock(panel_->lifecycle_mutex);
            if (!panel_->accepting_operations)
                return;
            const size_t index = static_cast<size_t>(kind_);
            generation_ = panel_->callback_generations[index];
            {
                std::lock_guard state_lock(panel_->mutex);
                if (kind_ == CallbackKind::Action) {
                    action_ = panel_->action_callback;
                    user_data_ = panel_->action_user_data;
                } else if (kind_ == CallbackKind::Event) {
                    event_ = panel_->event_callback;
                    user_data_ = panel_->event_user_data;
                } else {
                    render_ = panel_->render_callback;
                    user_data_ = panel_->render_user_data;
                }
            }
            ++panel_->callback_in_flight_by_generation[index][generation_];
            ++panel_->callbacks_in_flight;
            active_marker_ = {panel_, kind_, generation_, active_callback};
            active_callback = &active_marker_;
            acquired_ = true;
        } catch (...) {
        }
    }

    PanelCallbackLease(const PanelCallbackLease&) = delete;
    PanelCallbackLease& operator=(const PanelCallbackLease&) = delete;

    ~PanelCallbackLease() {
        if (!acquired_)
            return;
        active_callback = active_marker_.previous;
        {
            std::lock_guard lock(panel_->lifecycle_mutex);
            const size_t index = static_cast<size_t>(kind_);
            auto& by_generation = panel_->callback_in_flight_by_generation[index];
            auto found = by_generation.find(generation_);
            if (found != by_generation.end() && --found->second == 0)
                by_generation.erase(found);
            --panel_->callbacks_in_flight;
        }
        panel_->lifecycle_cv.notify_all();
        finalize_if_ready(panel_);
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

    sao_ui_panel_action_callback_t action() const noexcept {
        return action_;
    }

    sao_ui_panel_event_callback_t event() const noexcept {
        return event_;
    }

    sao_ui_panel_render_fn_t render() const noexcept {
        return render_;
    }

    void* user_data() const noexcept {
        return user_data_;
    }

  private:
    sao_ui_panel_s* panel_{};
    CallbackKind kind_{};
    uint64_t generation_{};
    sao_ui_panel_action_callback_t action_{};
    sao_ui_panel_event_callback_t event_{};
    sao_ui_panel_render_fn_t render_{};
    void* user_data_{};
    ActiveCallback active_marker_{};
    bool acquired_{};
};

void wait_for_callback_generation(sao_ui_panel_s* panel, CallbackKind kind, uint64_t generation) {
    if (callback_is_active(panel, kind, generation))
        return;
    const size_t index = static_cast<size_t>(kind);
    std::unique_lock lock(panel->lifecycle_mutex);
    panel->lifecycle_cv.wait(
        lock, [&] { return !panel->callback_in_flight_by_generation[index].contains(generation); });
}

class PanelRenderGuard {
  public:
    explicit PanelRenderGuard(sao_ui_panel_s* panel) : panel_(panel) {
        for (const ActiveRender* active = active_render; active != nullptr;
             active = active->previous) {
            if (active->panel == panel_)
                return;
        }
        render_lock_ = std::unique_lock<std::recursive_mutex>(panel_->render_mutex);
        marker_ = {panel_, active_render};
        active_render = &marker_;
        acquired_ = true;
    }

    PanelRenderGuard(const PanelRenderGuard&) = delete;
    PanelRenderGuard& operator=(const PanelRenderGuard&) = delete;

    ~PanelRenderGuard() {
        if (acquired_)
            active_render = marker_.previous;
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

  private:
    sao_ui_panel_s* panel_{};
    std::unique_lock<std::recursive_mutex> render_lock_;
    ActiveRender marker_{};
    bool acquired_{};
};

int32_t content_top(const sao_ui_panel_s& panel,
                    const sao::ui::detail::PanelResolvedTheme& theme) {
    return panel.show_titlebar ? titlebar_height(theme) : 0;
}

int32_t clamp_dimension(int32_t value, int32_t minimum, int32_t maximum) {
    value = std::max(value, std::max(1, minimum));
    return maximum > 0 ? std::min(value, maximum) : value;
}

std::string normalized_type(const json& node) {
    if (!node.is_object())
        return {};
    const char* key = node.contains("type") && node["type"].is_string()
                          ? "type"
                          : (node.contains("kind") && node["kind"].is_string()
                                 ? "kind"
                                 : nullptr);
    if (key == nullptr)
        return {};
    std::string type = node[key].get<std::string>();
    size_t first = 0;
    size_t last = type.size();
    while (first < last && std::isspace(static_cast<unsigned char>(type[first])) != 0)
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(type[last - 1])) != 0)
        --last;
    type = type.substr(first, last - first);
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return type;
}

bool is_container_type(std::string_view type) noexcept {
    return type == "panel" || type == "section" || type == "card" || type == "row" ||
           type == "group";
}

bool is_leaf_type(std::string_view type) noexcept {
    return type == "text" || type == "kv" || type == "bar" || type == "badge" ||
           type == "divider" || type == "spacer" || type == "button" || type == "input" ||
           type == "checkbox" || type == "radio" || type == "slider" || type == "dropdown" ||
           type == "table" || type == "canvas" || type == "rgba_frame";
}

bool validate_spec_node(const json& node, int32_t depth, size_t* node_count,
                        bool require_normalized_children) {
    if (node_count == nullptr || !node.is_object() || depth > kMaximumPanelSpecDepth ||
        *node_count >= kMaximumPanelSpecNodes)
        return false;
    ++*node_count;
    const std::string type = normalized_type(node);
    if (!is_container_type(type) && !is_leaf_type(type))
        return false;
    const auto children = node.find("children");
    const bool has_children = children != node.end();
    if (has_children && !children->is_array())
        return false;
    if (is_container_type(type)) {
        if (require_normalized_children && !has_children)
            return false;
        if (has_children) {
            for (const auto& child : *children) {
                if (!validate_spec_node(child, depth + 1, node_count,
                                        require_normalized_children))
                    return false;
            }
        }
    } else if (has_children) {
        return false;
    }
    return true;
}

bool validate_spec_document(const json& input) {
    size_t node_count = 0;
    const bool versioned =
        input.is_object() && input.contains("version") && input["version"].is_number_integer() &&
        input["version"].get<int64_t>() == SAO_UI_SPEC_VERSION;
    if (versioned) {
        const auto nodes = input.find("nodes");
        if (nodes == input.end() || !nodes->is_array())
            return false;
        for (const auto& node : *nodes) {
            if (!validate_spec_node(node, 0, &node_count, true))
                return false;
        }
        return true;
    }
    if (input.is_array()) {
        for (const auto& node : input) {
            if (!validate_spec_node(node, 0, &node_count, false))
                return false;
        }
        return true;
    }
    if (!input.is_object())
        return false;
    if (const auto nodes = input.find("nodes"); nodes != input.end()) {
        if (!nodes->is_array())
            return false;
        for (const auto& node : *nodes) {
            if (!validate_spec_node(node, 0, &node_count, false))
                return false;
        }
        return true;
    }
    if (const auto children = input.find("children"); children != input.end()) {
        if (!children->is_array())
            return false;
        for (const auto& node : *children) {
            if (!validate_spec_node(node, 0, &node_count, false))
                return false;
        }
        return true;
    }
    if (input.contains("type"))
        return validate_spec_node(input, 0, &node_count, false);
    return true;
}

void restore_node_ids(const json& source_nodes, json& normalized_nodes) {
    if (!source_nodes.is_array() || !normalized_nodes.is_array())
        return;
    std::vector<bool> used(source_nodes.size(), false);
    const auto source_id = [](const json& node) -> std::string {
        return node.is_object() && node.contains("id") && node["id"].is_string()
                   ? node["id"].get<std::string>()
                   : std::string();
    };
    for (size_t normalized_index = 0; normalized_index < normalized_nodes.size();
         ++normalized_index) {
        auto& normalized = normalized_nodes[normalized_index];
        if (!normalized.is_object())
            continue;
        const std::string target_type = normalized_type(normalized);
        const std::string target_id = source_id(normalized);
        const json* source = nullptr;
        if (!target_id.empty()) {
            for (size_t source_index = 0; source_index < source_nodes.size(); ++source_index) {
                if (!used[source_index] && normalized_type(source_nodes[source_index]) == target_type &&
                    source_id(source_nodes[source_index]) == target_id) {
                    source = &source_nodes[source_index];
                    used[source_index] = true;
                    break;
                }
            }
        }
        if (source == nullptr && normalized_index < source_nodes.size() &&
            !used[normalized_index] && normalized_type(source_nodes[normalized_index]) == target_type) {
            source = &source_nodes[normalized_index];
            used[normalized_index] = true;
        }
        if (source == nullptr) {
            for (size_t source_index = 0; source_index < source_nodes.size(); ++source_index) {
                if (!used[source_index] && normalized_type(source_nodes[source_index]) == target_type) {
                    source = &source_nodes[source_index];
                    used[source_index] = true;
                    break;
                }
            }
        }
        if (source == nullptr || !source->is_object())
            continue;
        if (source->contains("id") && (*source)["id"].is_string()) {
            std::string id = (*source)["id"].get<std::string>();
            size_t byte = 0;
            size_t codepoints = 0;
            while (byte < id.size() && codepoints < 80u) {
                const unsigned char lead = static_cast<unsigned char>(id[byte]);
                size_t width = 1;
                if ((lead & 0xe0U) == 0xc0U)
                    width = 2;
                else if ((lead & 0xf0U) == 0xe0U)
                    width = 3;
                else if ((lead & 0xf8U) == 0xf0U)
                    width = 4;
                if (byte + width > id.size())
                    break;
                byte += width;
                ++codepoints;
            }
            if (byte < id.size())
                id.resize(byte);
            normalized["id"] = std::move(id);
        }
        if (source->contains("children") && normalized.contains("children"))
            restore_node_ids((*source)["children"], normalized["children"]);
    }
}

bool merge_widget_spec_by_id(json& nodes, std::string_view widget_id, const json& patch) {
    if (nodes.is_array()) {
        for (auto& node : nodes) {
            if (merge_widget_spec_by_id(node, widget_id, patch))
                return true;
        }
        return false;
    }
    if (!nodes.is_object())
        return false;
    if (nodes.value("id", std::string()) == widget_id) {
        for (auto property = patch.begin(); property != patch.end(); ++property)
            nodes[property.key()] = property.value();
        if (patch.contains("label") && patch["label"].is_string())
            nodes["text"] = patch["label"];
        if (patch.contains("pct") && patch["pct"].is_number())
            nodes["value"] = patch["pct"];
        return true;
    }
    if (const auto document_nodes = nodes.find("nodes");
        document_nodes != nodes.end() && merge_widget_spec_by_id(*document_nodes, widget_id, patch))
        return true;
    const auto children = nodes.find("children");
    return children != nodes.end() && merge_widget_spec_by_id(*children, widget_id, patch);
}

json source_nodes_for(const json& input) {
    if (input.is_array())
        return input;
    if (!input.is_object())
        return json::array({input});
    if (input.contains("nodes") && input["nodes"].is_array()) {
        return input["nodes"];
    }
    if (input.contains("children") && input["children"].is_array()) {
        return input["children"];
    }
    return json::array({input});
}

sao_status_t normalize_spec(const uint8_t* bytes, size_t length, json* out_spec,
                            std::string* out_serialized) {
    if (out_spec == nullptr || out_serialized == nullptr || length > kMaximumPanelSpecBytes)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    json input;
    try {
        input = length == 0 ? json::object() : json::parse(bytes, bytes + length);
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!validate_spec_document(input))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (input.is_object() && input.contains("version") && input["version"].is_number_integer() &&
        input["version"].get<int64_t>() == SAO_UI_SPEC_VERSION &&
        input.contains("nodes") && input["nodes"].is_array()) {
        *out_spec = std::move(input);
        *out_serialized = out_spec->dump();
        return SAO_STATUS_OK;
    }

    const std::string envelope = json{{"spec", input}, {"title", ""}}.dump();
    size_t required = 0;
    sao_status_t status = sao_engine_ui_spec_normalize(
        reinterpret_cast<const uint8_t*>(envelope.data()), envelope.size(), nullptr, 0, &required);
    if (status != SAO_STATUS_OK)
        return status;
    std::vector<uint8_t> normalized(required);
    status = sao_engine_ui_spec_normalize(reinterpret_cast<const uint8_t*>(envelope.data()),
                                          envelope.size(), normalized.data(), normalized.size(),
                                          &required);
    if (status != SAO_STATUS_OK)
        return status;
    try {
        out_serialized->assign(reinterpret_cast<const char*>(normalized.data()), required);
        *out_spec = json::parse(*out_serialized);
        if (out_spec->contains("nodes")) {
            const json source_nodes = source_nodes_for(input);
            restore_node_ids(source_nodes, (*out_spec)["nodes"]);
            *out_serialized = out_spec->dump();
        }
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

int32_t widget_kind(const std::string& type) {
    if (type == "button")
        return SAO_UI_WIDGET_ACTION_BUTTON;
    if (type == "badge")
        return SAO_UI_WIDGET_STATUS_BADGE;
    if (type == "bar")
        return SAO_UI_WIDGET_BAR;
    if (type == "divider")
        return SAO_UI_WIDGET_DIVIDER;
    if (type == "input")
        return SAO_UI_WIDGET_TEXT_FIELD;
    if (type == "checkbox")
        return SAO_UI_WIDGET_CHECKBOX;
    if (type == "radio")
        return SAO_UI_WIDGET_RADIO;
    if (type == "dropdown")
        return SAO_UI_WIDGET_DROPDOWN_BUTTON;
    if (type == "slider")
        return SAO_UI_WIDGET_SLIDER;
    if (type == "table")
        return SAO_UI_WIDGET_TABLE;
    return SAO_UI_WIDGET_TEXT;
}

json make_widget_props(const json& node, const std::string& type) {
    json props = node;
    props["enabled"] = !node.value("disabled", false);
    if (type == "bar" && node.contains("pct"))
        props["value"] = node["pct"];
    if (type == "kv") {
        props["text"] =
            node.value("label", std::string()) + ": " + node.value("value", std::string());
    } else if (type == "button" || type == "checkbox" || type == "radio") {
        props["text"] = node.value("label", std::string());
    } else if (type == "input") {
        props["text"] = node.value("value", std::string());
    } else if (type == "table") {
        props["text"] = node.value("title", std::string());
    } else if (type == "bar" && !node.value("caption", std::string()).empty()) {
        props["text"] = node.value("caption", std::string());
    }
    return props;
}

SaoUiLayoutSpec container_spec_for_theme(
    std::string_view semantic_type, bool has_title,
    const sao::ui::detail::PanelResolvedTheme& theme) {
    SaoUiLayoutSpec spec{};
    sao_ui_layout_spec_defaults(&spec);
    const int32_t pad_s = theme.metrics[SAO_UI_METRIC_PADDING_S];
    const int32_t pad_m = theme.metrics[SAO_UI_METRIC_PADDING_M];
    if (semantic_type == "row") {
        spec.gap_px = theme.metrics[SAO_UI_METRIC_GAP_S];
    } else if (semantic_type == "panel") {
        spec.pad_top_px = pad_m;
        spec.pad_right_px = pad_m;
        spec.pad_bottom_px = pad_m;
        spec.pad_left_px = pad_m;
        spec.gap_px = theme.metrics[SAO_UI_METRIC_GAP_M];
    } else {
        spec.pad_top_px = pad_s;
        spec.pad_right_px = pad_s;
        spec.pad_bottom_px = pad_s;
        spec.pad_left_px = pad_s;
        spec.gap_px = theme.metrics[SAO_UI_METRIC_GAP_S];
    }
    if (has_title && semantic_type != "row")
        spec.pad_top_px += pad_m + 14;
    return spec;
}

size_t utf8_codepoint_count(std::string_view text) noexcept;

std::string ellipsize_utf8(std::string_view text, size_t max_codepoints) {
    if (max_codepoints == 0U)
        return {};
    if (utf8_codepoint_count(text) <= max_codepoints)
        return std::string(text);
    if (max_codepoints == 1U)
        return "\xE2\x80\xA6";
    std::string result;
    size_t codepoints = 0;
    for (size_t index = 0; index < text.size() && codepoints < max_codepoints - 1U;) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        size_t width = 1;
        if ((lead & 0xe0U) == 0xc0U) width = 2;
        else if ((lead & 0xf0U) == 0xe0U) width = 3;
        else if ((lead & 0xf8U) == 0xf0U) width = 4;
        if (index + width > text.size()) width = 1;
        result.append(text.substr(index, width));
        index += width;
        ++codepoints;
    }
    result += "\xE2\x80\xA6";
    return result;
}

size_t utf8_codepoint_count(std::string_view text) noexcept {
    size_t count = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        if ((static_cast<unsigned char>(text[index]) & 0xc0U) != 0x80U)
            ++count;
    }
    return count;
}

sao_status_t append_nodes(PanelContent & content, sao_ui_layout_node_handle_t parent,
                          const json & nodes,
                          const sao::ui::detail::PanelResolvedTheme & theme,
                          int32_t viewport_width, std::string_view parent_type = {},
                          int32_t depth = 0, std::string_view parent_path = {}) {
    const sao::ui::detail::ScopedPanelPaintTheme theme_scope(theme);
    if (not nodes.is_array())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    for (size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        const std::string node_path = parent_path.empty()
                                               ? std::to_string(index)
                                               : std::string(parent_path) + "/" + std::to_string(index);
        if (not node.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const std::string type = normalized_type(node);
        if ((!is_container_type(type) && !is_leaf_type(type)) ||
            (node.contains("children") && !node["children"].is_array()) ||
            (node.contains("children") && is_leaf_type(type)))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (node.contains("children") and node["children"].is_array()) {
            std::string container_title = node.value("title", std::string());
            if (container_title.empty())
                container_title = node.value("label", std::string());
            const bool has_title = !container_title.empty();
            SaoUiLayoutSpec spec = container_spec_for_theme(type, has_title, theme);
            if (!apply_container_layout_metadata(node, &spec))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            sao_ui_layout_node_handle_t child = nullptr;
            std::string layout;
            if (const auto layout_property = node.find("layout");
                layout_property != node.end()) {
                if (!layout_property->is_string())
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                layout = layout_property->get<std::string>();
            }
            std::transform(layout.begin(), layout.end(), layout.begin(),
                           [](unsigned char character) {
                               return static_cast<char>(std::tolower(character));
                           });
            int32_t primary_mode = type == "row" ? SAO_UI_LAYOUT_HORIZONTAL
                                                   : SAO_UI_LAYOUT_VERTICAL;
            if (!layout.empty()) {
                primary_mode = parse_container_layout_mode(layout, -1);
                if (primary_mode < 0)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            ContainerResponsiveLayout responsive{};
            if (!parse_container_responsive_layout(node, primary_mode, &responsive))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            const int32_t mode = resolve_container_layout_mode(responsive, viewport_width);
            sao_status_t status = sao_ui_layout_node_add_container(parent, mode, & spec, & child);
            if (status != SAO_STATUS_OK)
                return status;
            int scroll_axis = 0;
            bool scroll_wheel = true;
            bool scroll_bar = true;
            if (!parse_scroll_metadata(node, &scroll_axis, &scroll_wheel, &scroll_bar))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            if (node.contains("scroll")) {
                const std::string viewport_id = node.value("id", node_path);
                sao::ui::detail::layout_set_viewport(child, viewport_id.c_str(), scroll_axis,
                                                     scroll_wheel, scroll_bar);
                content.explicit_viewports = true;
            }
            PanelContent::ThemeLayoutNode layout_node{};
            layout_node.node = child;
            layout_node.spec = spec;
            layout_node.explicit_metadata = capture_container_layout_metadata(node, spec);
            layout_node.responsive = responsive;
            layout_node.active_mode = mode;
            layout_node.semantic_type = type;
            layout_node.has_title = has_title;
            content.theme_layout_nodes.push_back(std::move(layout_node));
            uint32_t accent = theme.colors[SAO_UI_TOKEN_APP_ACCENT];
            SaoUiColorToken accent_token = SAO_UI_TOKEN_APP_ACCENT;
            bool accent_is_explicit = false;
            const auto accent_property = node.find("accent");
            if (accent_property != node.end() &&
                !parse_container_accent(*accent_property, theme, &accent, &accent_token,
                                         &accent_is_explicit))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            PanelContent::ContainerVisual visual{};
            visual.node = child;
            visual.kind = node.value("kind", 0);
            visual.semantic_type = type;
            visual.parent_type = std::string(parent_type);
            visual.title = std::move(container_title);
            visual.accent = accent;
            visual.accent_token = accent_token;
            visual.accent_is_explicit = accent_is_explicit;
            visual.responsive = responsive;
            visual.active_mode = mode;
            visual.has_title = has_title;
            content.containers.push_back(std::move(visual));
            const std::string_view child_parent_type =
                type == "row" && mode != SAO_UI_LAYOUT_HORIZONTAL ? std::string_view{} :
                                                                        std::string_view(type);
            status = append_nodes(content, child, node["children"], theme, viewport_width,
                                  child_parent_type, depth + 1, node_path);
            if (status != SAO_STATUS_OK)
                return status;
            continue;
        }
        if (!is_leaf_type(type))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        auto owned = std::make_unique<OwnedWidget>();
        owned->kind = widget_kind(type);
        owned->type = type;
        owned->path = node_path;
        owned->id = node.value("id", std::string());
        owned->action = node.value("action", std::string());
        const auto disabled_property = node.find("disabled");
        if (disabled_property != node.end() && !disabled_property->is_boolean())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        owned->enabled = disabled_property == node.end() || !disabled_property->get<bool>();
        if (node.contains("payload"))
            owned->action_args = node["payload"].dump();
        owned->props = make_widget_props(node, type);
        if (not owned->id.empty() and content.by_id.contains(owned->id))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        sao_status_t status = SAO_STATUS_OK;
        if (type == "slider") {
            if (!owned->props.contains("value") || !owned->props["value"].is_number())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            const double raw_value = owned->props["value"].get<double>();
            if (!std::isfinite(raw_value) || raw_value < 0.0 || raw_value > 1.0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            SaoUiSliderSpec slider{};
            slider.value = static_cast<float>(raw_value);
            slider.min_value = 0.0F;
            slider.max_value = 1.0F;
            slider.step = owned->props.value("step", 0.0F);
            slider.vertical = owned->props.value("vertical", false);
            slider.disabled = !owned->enabled;
            slider.show_value_label = owned->props.value("show_value_label", true);
            slider.track_thickness_px = owned->props.value("track_thickness", 4);
            slider.thumb_size_px = owned->props.value("thumb_size", 14);
            owned->slider_value = slider.value;
            status = sao_ui_slider_create(nullptr, &slider, &owned->handle);
            if (status == SAO_STATUS_OK)
                status = sao_ui_slider_set_change_handler(owned->handle, &panel_slider_changed,
                                                          owned.get());
        } else if (type == "dropdown") {
            const auto items = owned->props.find("items");
            if (items == owned->props.end() || !items->is_array() || items->empty())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            owned->dropdown_items.reserve(items->size());
            for (const auto& item : *items) {
                if (!item.is_object() || !item.contains("id") || !item["id"].is_number_integer() ||
                    !item.contains("label") || !item["label"].is_string() ||
                    !item.contains("value") || !item["value"].is_string())
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const int64_t id = item["id"].get<int64_t>();
                if (id < 0 || id > std::numeric_limits<int32_t>::max())
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                if (std::ranges::any_of(owned->dropdown_items, [id](const auto& existing) {
                        return existing.id == static_cast<int32_t>(id);
                    }))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                owned->dropdown_items.push_back(
                    {static_cast<int32_t>(id), item["label"].get<std::string>(),
                     item["value"].get<std::string>(), item.value("enabled", true),
                     item.value("checked", false)});
            }
            owned->dropdown_selected_id =
                owned->props.value("selected_id", owned->dropdown_items.front().id);
            std::vector<SaoUiDropdownEntry> entries;
            entries.reserve(owned->dropdown_items.size());
            std::string selected_label = owned->dropdown_items.front().label;
            for (const auto& item : owned->dropdown_items) {
                entries.push_back(
                    {item.label.c_str(), item.id, item.enabled, item.checked, {0, 0}});
                if (item.id == owned->dropdown_selected_id)
                    selected_label = item.label;
            }
            SaoUiDropdownButtonSpec dropdown{};
            dropdown.text_utf8 = selected_label.c_str();
            dropdown.kind = SAO_UI_BTN_NORMAL;
            dropdown.entries = entries.data();
            dropdown.entry_count = entries.size();
            status = sao_ui_dropdown_button_create(nullptr, &dropdown, &owned->handle);
            if (status == SAO_STATUS_OK)
                status = sao_ui_dropdown_button_set_pick_handler(
                    owned->handle, &panel_dropdown_picked, owned.get());
        } else {
            status = sao_ui_widget_create(owned->kind, nullptr, &owned->handle);
        }
        if (status != SAO_STATUS_OK)
            return status;
        status = apply_owned_widget_props(owned.get());
        if (status != SAO_STATUS_OK)
            return status;
        status = sao_ui_widget_set_enabled(owned->handle, owned->enabled);
        if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_NOT_IMPLEMENTED)
            return status;
        SaoUiLayoutSpec spec{};
        sao_ui_layout_spec_defaults(& spec);
        if (!apply_container_layout_metadata(node, &spec))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (type == "spacer")
            spec.fixed_height_px = std::max(0, node.value("size", 8));
        else if (node.contains("height") and node["height"].is_number_integer())
            spec.fixed_height_px = std::max(1, node["height"].get<int32_t>());
        if (node.contains("width") and node["width"].is_number_integer())
            spec.fixed_width_px = std::max(1, node["width"].get<int32_t>());
        if (parent_type == "row") {
            if (type == "button")
                spec.min_width_px = std::max(spec.min_width_px, 72);
            else if (type != "spacer") {
                spec.weight = 1.0F;
                spec.min_width_px = std::max(spec.min_width_px, 40);
            }
        }
        std::string text_value;
        if (node.contains("text") and node["text"].is_string())
            text_value = node["text"].get<std::string>();
        else if (node.contains("label") and node["label"].is_string())
            text_value = node["label"].get<std::string>();
        else if (node.contains("value") and node["value"].is_string())
            text_value = node["value"].get<std::string>();
        if (not text_value.empty() and (type == "text" or type == "kv" or type == "input")) {
            SaoUiWidgetSizeHint hint{};
            if (sao_ui_widget_get_size_hint(owned->handle, std::max(0, viewport_width),
                                             1 << 30, &hint) == SAO_STATUS_OK)
                spec.min_height_px = std::max(spec.min_height_px, hint.min_height_px);
            const size_t glyphs = utf8_codepoint_count(text_value);
            const size_t glyphs_per_line = 48U;
            const size_t lines = (glyphs + glyphs_per_line - 1U) / glyphs_per_line;
            spec.min_height_px = std::max(spec.min_height_px,
                                          18 + static_cast<int32_t>(std::max<size_t>(1U, lines) - 1U) * 16);
        }
        status = sao_ui_layout_node_add_widget(parent, owned->handle, & spec, & owned->node);
        if (status != SAO_STATUS_OK)
            return status;
        content.by_handle.emplace(owned->handle, owned.get());
        if (not owned->id.empty())
            content.by_id.emplace(owned->id, owned.get());
        content.widgets.push_back(std::move(owned));
    }
    return SAO_STATUS_OK;
}

sao_status_t arrange_content(PanelContent & content, int32_t width, int32_t height, int32_t top) {
    const int32_t viewport_height = std::max(1, height - top);
    content.panel_width_px = width;
    content.viewport_top_px = top;
    content.viewport_height_px = viewport_height;
    SaoUiLayoutSpec natural_spec = content.root_spec;
    natural_spec.fixed_width_px = 0;
    natural_spec.fixed_height_px = 0;
    int32_t layout_width = std::max(1, width);
    if (content.explicit_viewports) {
        content.scroll_offset_px = 0;
        content.content_extent_px = viewport_height;
        content.root_spec.fixed_width_px = layout_width;
        content.root_spec.fixed_height_px = viewport_height;
        sao_status_t status = sao_ui_layout_node_set_spec(content.root, &content.root_spec);
        if (status != SAO_STATUS_OK)
            return status;
        SaoUiSize preferred{};
        status = sao_ui_layout_measure(content.root, {layout_width, viewport_height}, &preferred);
        if (status != SAO_STATUS_OK)
            return status;
        return sao_ui_layout_arrange(content.root, {0, top, layout_width, viewport_height});
    }
    sao_status_t status = sao_ui_layout_node_set_spec(content.root, & natural_spec);
    if (status != SAO_STATUS_OK)
        return status;
    SaoUiSize preferred{};
    status = sao_ui_layout_measure(content.root, {width, 1 << 30}, & preferred);
    if (status != SAO_STATUS_OK)
        return status;
    content.content_extent_px = std::max(viewport_height, preferred.height_px);
    const int32_t scrollbar_width = std::max(0, content.layout_theme.metrics[SAO_UI_METRIC_SCROLLBAR_WIDTH]);
    if (preferred.height_px > viewport_height && scrollbar_width > 0) {
        layout_width = std::max(1, width - scrollbar_width);
        status = sao_ui_layout_measure(content.root, {layout_width, 1 << 30}, &preferred);
        if (status != SAO_STATUS_OK)
            return status;
        content.content_extent_px = std::max(viewport_height, preferred.height_px);
    }
    content.scroll_offset_px = std::clamp(content.scroll_offset_px, 0,
                                           std::max(0, content.content_extent_px - viewport_height));
    content.root_spec.fixed_width_px = layout_width;
    content.root_spec.fixed_height_px = viewport_height;
    status = sao_ui_layout_node_set_spec(content.root, & content.root_spec);
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_layout_measure(content.root, {layout_width, viewport_height}, & preferred);
    if (status != SAO_STATUS_OK)
        return status;
    return sao_ui_layout_arrange(content.root, {0, top, layout_width, content.content_extent_px});
}

sao_status_t build_content(const json& normalized, int32_t width, int32_t height, int32_t top,
                           const sao::ui::detail::PanelResolvedTheme& theme,
                           std::shared_ptr<PanelContent>* out_content) {
    if (!normalized.is_object() || !normalized.contains("nodes") ||
        !normalized["nodes"].is_array() || out_content == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto content = std::make_shared<PanelContent>();
    content->layout_theme = theme;
    content->uses_theme_layout_metrics = true;
    sao_ui_layout_spec_defaults(&content->root_spec);
    const int32_t body_padding = theme.metrics[SAO_UI_METRIC_PADDING_M];
    content->root_spec.pad_top_px = body_padding;
    content->root_spec.pad_right_px = body_padding;
    content->root_spec.pad_bottom_px = body_padding;
    content->root_spec.pad_left_px = body_padding;
    content->root_spec.gap_px = theme.metrics[SAO_UI_METRIC_GAP_S];
    sao_status_t status = sao_ui_layout_tree_create(&content->tree);
    if (status != SAO_STATUS_OK)
        return status;
    int32_t root_layout = SAO_UI_LAYOUT_VERTICAL;
    if (const auto layout = normalized.find("layout"); layout != normalized.end()) {
        if (!layout->is_string())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::string mode = layout->get<std::string>();
        std::transform(mode.begin(), mode.end(), mode.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        root_layout = parse_container_layout_mode(mode, -1);
        if (root_layout < 0)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    status = sao_ui_layout_tree_set_root(content->tree, root_layout, &content->root_spec,
                                         &content->root);
    if (status != SAO_STATUS_OK)
        return status;
    status = append_nodes(*content, content->root, normalized["nodes"], theme, width);
    if (status != SAO_STATUS_OK)
        return status;
    status = arrange_content(*content, width, height, top);
    if (status != SAO_STATUS_OK)
        return status;
    *out_content = std::move(content);
    return SAO_STATUS_OK;
}

sao_status_t migrate_responsive_content_state(
    sao_ui_panel_s* panel, const std::shared_ptr<PanelContent>& current,
    const std::shared_ptr<PanelContent>& replacement) {
    if (panel == nullptr || current == nullptr || replacement == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_ui_widget_handle_t hovered_widget = nullptr;
    sao_ui_widget_handle_t pressed_widget = nullptr;
    sao_ui_widget_handle_t focused_widget = nullptr;
    std::string focused_id;
    std::string focused_path;
    {
        std::lock_guard lock(panel->mutex);
        hovered_widget = panel->hovered_widget;
        pressed_widget = panel->pressed_widget;
        focused_widget = panel->focused_widget;
        focused_id = panel->responsive_focus_id;
        focused_path = panel->responsive_focus_path;
    }
    std::scoped_lock lock(current->mutex, replacement->mutex);
    for (const auto& previous : current->widgets) {
        if (previous->handle == focused_widget || previous->props.value("focused", false)) {
            focused_id = previous->id;
            focused_path = previous->path;
            break;
        }
    }
    replacement->scroll_offset_px = std::clamp(
        current->scroll_offset_px, 0,
        std::max(0, replacement->content_extent_px - replacement->viewport_height_px));
    for (auto& replacement_entry : replacement->widgets) {
        OwnedWidget* previous = nullptr;
        if (!replacement_entry->id.empty()) {
            const auto found = current->by_id.find(replacement_entry->id);
            if (found != current->by_id.end())
                previous = found->second;
        }
        if (previous == nullptr) {
            for (const auto& candidate : current->widgets) {
                if (candidate->path == replacement_entry->path &&
                    candidate->type == replacement_entry->type) {
                    previous = candidate.get();
                    break;
                }
            }
        }
        if (previous == nullptr)
            continue;
        replacement_entry->props = previous->props;
        replacement_entry->action = previous->action;
        replacement_entry->action_args = previous->action_args;
        replacement_entry->enabled = previous->enabled;
        sao_status_t status = apply_owned_widget_props(replacement_entry.get());
        if (status != SAO_STATUS_OK)
            return status;
        status = sao_ui_widget_set_enabled(replacement_entry->handle, replacement_entry->enabled);
        if (status != SAO_STATUS_OK)
            return status;
        if (previous->handle == hovered_widget) {
            status = sao_ui_widget_set_hovered(replacement_entry->handle, true);
            if (status != SAO_STATUS_OK)
                return status;
        }
        if (previous->handle == pressed_widget) {
            status = sao_ui_widget_set_pressed(replacement_entry->handle, true);
            if (status != SAO_STATUS_OK)
                return status;
        }
        const bool restore_focus =
            (!focused_id.empty() && replacement_entry->id == focused_id) ||
            (focused_id.empty() && !focused_path.empty() && replacement_entry->path == focused_path);
        if (restore_focus) {
            status = sao_ui_widget_set_focused(replacement_entry->handle, true);
            if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_NOT_IMPLEMENTED)
                return status;
            std::lock_guard panel_lock(panel->mutex);
            panel->focused_widget = replacement_entry->handle;
            panel->responsive_focus_id = replacement_entry->id;
            panel->responsive_focus_path = replacement_entry->path;
        }
    }
    return SAO_STATUS_OK;
}

bool responsive_layout_needs_rebuild(const PanelContent& content, int32_t viewport_width) {
    for (const auto& layout_node : content.theme_layout_nodes) {
        if (layout_node.active_mode !=
            resolve_container_layout_mode(layout_node.responsive, viewport_width))
            return true;
    }
    return false;
}

sao_status_t prepare_responsive_content(
    sao_ui_panel_s* panel, const sao::ui::detail::PanelResolvedTheme& theme, int32_t width,
    int32_t height, int32_t top, std::shared_ptr<PanelContent>* out_content,
    bool* out_rebuilt) {
    if (panel == nullptr || out_content == nullptr || out_rebuilt == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_content = nullptr;
    *out_rebuilt = false;
    std::shared_ptr<PanelContent> current;
    std::string spec_json;
    {
        std::lock_guard lock(panel->mutex);
        current = panel->content;
        spec_json = panel->spec_json;
    }
    if (current == nullptr) {
        *out_content = current;
        return SAO_STATUS_OK;
    }
    if (panel_has_active_text_edit(panel)) {
        *out_content = current;
        return SAO_STATUS_OK;
    }
    {
        std::lock_guard lock(current->mutex);
        if (!responsive_layout_needs_rebuild(*current, width) || spec_json.empty()) {
            *out_content = current;
            return SAO_STATUS_OK;
        }
    }
    try {
        const json normalized = json::parse(spec_json);
        std::shared_ptr<PanelContent> replacement;
        const sao_status_t status =
            build_content(normalized, width, height, top, theme, &replacement);
        if (status != SAO_STATUS_OK)
            return status;
        sao::ui::detail::layout_restore_viewports(current->root, replacement->root);
        const sao_status_t migration_status =
            migrate_responsive_content_state(panel, current, replacement);
        if (migration_status != SAO_STATUS_OK)
            return migration_status;
        *out_content = std::move(replacement);
        *out_rebuilt = true;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

sao_status_t build_body_content(const SaoUiPanelBodyModelNode* nodes, size_t node_count,
                                int32_t width, int32_t height, int32_t top,
                                const sao::ui::detail::PanelResolvedTheme& theme,
                                std::shared_ptr<PanelContent>* out_content,
                                std::vector<sao_ui_layout_node_handle_t>* out_nodes) {
    if (nodes == nullptr || node_count == 0 || out_content == nullptr || out_nodes == nullptr ||
        nodes[0].model_id == 0 || nodes[0].parent_model_id != 0 || nodes[0].widget != nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto content = std::make_shared<PanelContent>();
    content->layout_theme = theme;
    content->root_spec = nodes[0].spec;
    const int32_t body_padding = theme.metrics[SAO_UI_METRIC_PADDING_M];
    content->uses_theme_layout_metrics =
        content->root_spec.pad_top_px == body_padding &&
        content->root_spec.pad_right_px == body_padding &&
        content->root_spec.pad_bottom_px == body_padding &&
        content->root_spec.pad_left_px == body_padding &&
        content->root_spec.gap_px == theme.metrics[SAO_UI_METRIC_GAP_S];
    sao_status_t status = sao_ui_layout_tree_create(&content->tree);
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_layout_tree_set_root(content->tree, nodes[0].layout_mode, &content->root_spec,
                                         &content->root);
    if (status != SAO_STATUS_OK)
        return status;

    std::unordered_map<uint64_t, sao_ui_layout_node_handle_t> by_model_id;
    by_model_id.emplace(nodes[0].model_id, content->root);
    out_nodes->assign(node_count, nullptr);
    (*out_nodes)[0] = content->root;
    for (size_t index = 1; index < node_count; ++index) {
        const SaoUiPanelBodyModelNode& model = nodes[index];
        if (model.model_id == 0 || by_model_id.contains(model.model_id))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const auto parent = by_model_id.find(model.parent_model_id);
        if (parent == by_model_id.end())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        sao_ui_layout_node_handle_t actual = nullptr;
        if (model.widget != nullptr) {
            auto widget = std::make_unique<OwnedWidget>();
            widget->handle = model.widget;
            widget->owns_handle = false;
            status =
                sao_ui_layout_node_add_widget(parent->second, model.widget, &model.spec, &actual);
            if (status != SAO_STATUS_OK)
                return status;
            widget->node = actual;
            content->by_handle.emplace(model.widget, widget.get());
            content->widgets.push_back(std::move(widget));
        } else {
            status = sao_ui_layout_node_add_container(parent->second, model.layout_mode,
                                                      &model.spec, &actual);
            if (status != SAO_STATUS_OK)
                return status;
        }
        by_model_id.emplace(model.model_id, actual);
        (*out_nodes)[index] = actual;
    }
    status = arrange_content(*content, width, height, top);
    if (status != SAO_STATUS_OK)
        return status;
    *out_content = std::move(content);
    return SAO_STATUS_OK;
}

struct PanelScrollbarGeometry {
    bool visible{};
    int32_t hit_x{};
    int32_t hit_y{};
    int32_t hit_width{};
    int32_t hit_height{};
    float track_x{};
    float track_y{};
    float track_width{};
    float track_height{};
    float thumb_x{};
    float thumb_y{};
    float thumb_width{};
    float thumb_height{};
    float thumb_travel{};
    int32_t range{};
};

struct PanelScrollbarGeometryTestSnapshot {
    bool visible{};
    uint8_t _pad[3]{};
    int32_t hit_x{};
    int32_t hit_y{};
    int32_t hit_width{};
    int32_t hit_height{};
    float track_x{};
    float track_y{};
    float track_width{};
    float track_height{};
    float thumb_x{};
    float thumb_y{};
    float thumb_width{};
    float thumb_height{};
};

PanelScrollbarGeometry panel_scrollbar_geometry(
    int32_t panel_width, int32_t panel_height, int32_t top, int32_t viewport_height,
    int32_t content_extent, int32_t scroll_offset,
    const sao::ui::detail::PanelResolvedTheme& theme) {
    PanelScrollbarGeometry geometry{};
    const int32_t bounded_width = std::max(0, panel_width);
    const int32_t bounded_height = std::max(0, panel_height);
    const int32_t bounded_top = std::clamp(top, 0, bounded_height);
    const int32_t available_height = std::max(0, bounded_height - bounded_top);
    const int32_t bounded_viewport = std::min(std::max(0, viewport_height), available_height);
    const int32_t scrollbar_width =
        std::max(0, theme.metrics[SAO_UI_METRIC_SCROLLBAR_WIDTH]);
    const int32_t hit_area = std::max(
        scrollbar_width, theme.metrics[SAO_UI_METRIC_SCROLLBAR_HIT_AREA]);
    const int32_t minimum_thumb =
        std::max(0, theme.metrics[SAO_UI_METRIC_SCROLLBAR_MIN_THUMB]);
    geometry.hit_width = std::min(hit_area, bounded_width);
    geometry.hit_height = bounded_viewport;
    geometry.hit_x = bounded_width - geometry.hit_width;
    geometry.hit_y = bounded_top;
    geometry.track_width = static_cast<float>(std::min(scrollbar_width, bounded_width));
    geometry.track_x = static_cast<float>(bounded_width) - geometry.track_width;
    const int32_t vertical_inset =
        std::min(std::max(0, scrollbar_width / 4), bounded_viewport / 2);
    geometry.track_y = static_cast<float>(bounded_top + vertical_inset);
    geometry.track_height = static_cast<float>(
        std::max(0, bounded_viewport - vertical_inset * 2));
    geometry.range = clamp_i64_to_i32(std::max(
        int64_t{0}, static_cast<int64_t>(content_extent) - static_cast<int64_t>(viewport_height)));
    geometry.visible = content_extent > viewport_height && geometry.hit_width > 0 &&
                       geometry.hit_height > 0 && geometry.track_width > 0.0F &&
                       geometry.track_height > 0.0F;
    if (!geometry.visible)
        return geometry;
    const float thumb_height = std::min(
        geometry.track_height,
        std::max(static_cast<float>(minimum_thumb),
                 geometry.track_height * static_cast<float>(viewport_height) /
                     static_cast<float>(std::max(1, content_extent))));
    geometry.thumb_travel = std::max(0.0F, geometry.track_height - thumb_height);
    const float fraction = geometry.range > 0
                               ? static_cast<float>(std::clamp(scroll_offset, 0, geometry.range)) /
                                     static_cast<float>(geometry.range)
                               : 0.0F;
    geometry.thumb_x = geometry.track_x;
    geometry.thumb_y = geometry.track_y + geometry.thumb_travel * fraction;
    geometry.thumb_width = geometry.track_width;
    geometry.thumb_height = thumb_height;
    return geometry;
}

uint32_t theme_color_with_alpha(uint32_t argb, uint8_t alpha) noexcept {
    const uint32_t source_alpha = (argb >> 24U) & 0xffU;
    const uint32_t effective_alpha = std::min(source_alpha, static_cast<uint32_t>(alpha));
    return (argb & 0x00ffffffU) | (effective_alpha << 24U);
}

struct ScopedPaintClip {
    sao_ui_paint_ctx_handle_t context{};
    bool active{};
    ~ScopedPaintClip() {
        if (active)
            (void)sao_ui_paint_ctx_pop_clip(context);
    }
};

sao_status_t paint_panel(const std::shared_ptr<PanelContent> & content, const SaoPanelState & state,
                         bool show_titlebar, const std::string & title, bool show_close_button,
                         const sao::ui::detail::PanelResolvedTheme & theme,
                         sao_ui_panel_s* panel, sao_ui_offscreen_raster_handle_t raster) {
    const sao::ui::detail::ScopedPanelPaintTheme theme_scope(theme);
    sao_ui_paint_ctx_handle_t context = nullptr;
    sao_status_t status = sao_ui_paint_ctx_create_offscreen(raster, & context);
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_paint_ctx_begin_frame(context);
    const int32_t top = show_titlebar ? titlebar_height(theme) : 0;
    bool scrollbar_hovered = false;
    bool scrollbar_pressed = false;
    bool close_armed = false;
    if (panel != nullptr) {
        std::lock_guard lock(panel->mutex);
        scrollbar_hovered = panel->scrollbar_hovered;
        scrollbar_pressed = panel->scrollbar_pressed;
        close_armed = panel->close_armed;
    }
    if (status == SAO_STATUS_OK)
        status = sao_ui_paint_ctx_fill_rect(context, 0.0F, 0.0F, static_cast<float>(state.width),
                                            static_cast<float>(state.height),
                                            theme.colors[SAO_UI_TOKEN_APP_BG]);
    if (status == SAO_STATUS_OK and show_titlebar) {
        status = sao_ui_paint_ctx_fill_rect(
            context, 0.0F, 0.0F, static_cast<float>(state.width),
            static_cast<float>(std::max(1, top)), theme.colors[SAO_UI_TOKEN_APP_CARD]);
        if (status == SAO_STATUS_OK && !theme.high_contrast && top > 4) {
            status = sao_ui_paint_ctx_fill_rect(context, 0.0F, 0.0F,
                                                static_cast<float>(std::min(state.width, 72)), 2.0F,
                                                theme.colors[SAO_UI_TOKEN_APP_ACCENT]);
        }
        if (status == SAO_STATUS_OK and not title.empty()) {
            const int32_t title_padding = theme.metrics[SAO_UI_METRIC_PADDING_M];
            const int32_t close_space = show_close_button ? top : 24;
            const int32_t title_width = std::max(0, state.width - title_padding * 2 - close_space);
            const std::string visible_title = ellipsize_utf8(title,
                static_cast<size_t>(title_width / 7));
            status = sao_ui_paint_ctx_draw_utf8(
                context, static_cast<float>(title_padding),
                std::max(0.0F, (static_cast<float>(top) - 14.0F) * 0.5F), visible_title.c_str(),
                14.0F, theme.colors[SAO_UI_TOKEN_APP_TEXT]);
        }
        if (status == SAO_STATUS_OK and show_close_button) {
            const float close_x = static_cast<float>(state.width - top + theme.metrics[SAO_UI_METRIC_PADDING_S]);
            const float close_y = static_cast<float>(top / 2);
            const float arm = 5.0F;
            if (close_armed && !theme.high_contrast)
                status = sao::ui::detail::paint_rounded_rect(
                    context, close_x - 11.0F, close_y - 11.0F, 22.0F, 22.0F, 5.0F,
                    theme_color_with_alpha(theme.colors[SAO_UI_TOKEN_CLOSE_RED], 32));
            if (status == SAO_STATUS_OK)
                status = sao_ui_paint_ctx_stroke_line(
                    context, close_x - arm, close_y - arm, close_x + arm, close_y + arm, 1.5F,
                    theme.colors[close_armed ? SAO_UI_TOKEN_CLOSE_RED : SAO_UI_TOKEN_APP_TEXT_DIM]);
            if (status == SAO_STATUS_OK)
                status = sao_ui_paint_ctx_stroke_line(context, close_x + arm, close_y - arm,
                                                       close_x - arm, close_y + arm, 1.5F,
                                                       theme.colors[close_armed
                                                                        ? SAO_UI_TOKEN_CLOSE_RED
                                                                        : SAO_UI_TOKEN_APP_TEXT_DIM]);
        }
        if (status == SAO_STATUS_OK && top > 0)
            status = sao_ui_paint_ctx_stroke_line(
                context, 0.0F, static_cast<float>(top - 1), static_cast<float>(state.width),
                static_cast<float>(top - 1), 1.0F,
                theme.colors[SAO_UI_TOKEN_APP_BORDER]);
    }
    if (status == SAO_STATUS_OK and content != nullptr) {
        std::scoped_lock lock(content->mutex);
        const int32_t viewport_height = std::max(1, content->viewport_height_px);
        status = sao_ui_paint_ctx_push_clip(context, 0.0F, static_cast<float>(top),
                                            static_cast<float>(state.width),
                                            static_cast<float>(viewport_height));
        if (status == SAO_STATUS_OK) {
            for (size_t index = 0; index < content->containers.size(); ++index) {
                const auto visual = content->containers[index];
                if (visual.semantic_type == "row")
                    continue;
                SaoUiRect rect{};
                SaoUiRect node_clip{};
                if (!sao::ui::detail::layout_visual_geometry(visual.node, rect, node_clip))
                    continue;
                rect.y_px -= content->scroll_offset_px;
                node_clip.y_px -= content->scroll_offset_px;
                const float x = static_cast<float>(rect.x_px);
                const float y = static_cast<float>(rect.y_px);
                const float width = static_cast<float>(rect.width_px);
                const float height = static_cast<float>(rect.height_px);
                if (!(width > 0.0F && height > 0.0F))
                    continue;
                if (y + height <= static_cast<float>(top) ||
                    y >= static_cast<float>(top + viewport_height))
                    continue;
                status = sao_ui_paint_ctx_push_clip(context, static_cast<float>(node_clip.x_px),
                                                    static_cast<float>(node_clip.y_px),
                                                    static_cast<float>(node_clip.width_px),
                                                    static_cast<float>(node_clip.height_px));
                if (status != SAO_STATUS_OK)
                    break;
                ScopedPaintClip node_clip_guard{context, true};
                const bool panel_role = visual.semantic_type == "panel";
                const bool section_role = visual.semantic_type == "section";
                const bool card_role = visual.semantic_type == "card";
                const bool group_role = visual.semantic_type == "group";
                const uint32_t fill = panel_role
                                          ? theme.colors[SAO_UI_TOKEN_APP_BG]
                                          : theme.colors[SAO_UI_TOKEN_APP_CARD];
                const uint32_t border = theme_color_with_alpha(
                    panel_role || section_role || group_role || card_role
                        ? theme.colors[SAO_UI_TOKEN_APP_BORDER]
                        : theme.colors[SAO_UI_TOKEN_APP_TEXT_DIM],
                    panel_role ? 0xd0U : 0xa8U);
                const float radius = static_cast<float>(
                    panel_role || card_role ? theme.metrics[SAO_UI_METRIC_BORDER_RADIUS_LARGE]
                                            : theme.metrics[SAO_UI_METRIC_BORDER_RADIUS_SMALL]);
                const float paint_inset = static_cast<float>(std::max(
                    1, theme.metrics[SAO_UI_METRIC_PADDING_XS]));
                constexpr float stroke_width = 1.0F;
                if (!section_role)
                    status = sao_ui_paint_ctx_fill_rounded_rect(context, x, y, width, height,
                                                                radius, fill);
                if (status != SAO_STATUS_OK)
                    break;
                const float rail_width = section_role ? 1.0F : 0.0F;
                if (rail_width > 0.0F && width > paint_inset * 2.0F &&
                    height > paint_inset * 2.0F) {
                    const uint32_t rail_color = theme.high_contrast
                                                    ? theme.colors[SAO_UI_TOKEN_FOCUS_RING]
                                                    : visual.accent;
                    status = sao_ui_paint_ctx_stroke_line(
                        context, x, y + paint_inset, x, y + height - paint_inset,
                        rail_width, rail_color);
                    if (status != SAO_STATUS_OK)
                        break;
                }
                if (section_role)
                    status = sao_ui_paint_ctx_stroke_line(
                        context, x, y + height - 1.0F, x + width, y + height - 1.0F,
                        1.0F, border);
                else
                    status = sao::ui::detail::paint_rounded_rect_stroke(
                        context, x, y, width, height, radius, stroke_width, border);
                if (status != SAO_STATUS_OK)
                    break;
                if (card_role && !theme.high_contrast && width > 24.0F && height > 12.0F) {
                    status = sao_ui_paint_ctx_stroke_line(context, x + radius, y + 1.0F,
                                                          x + std::min(width - radius, 48.0F),
                                                          y + 1.0F, 1.0F, visual.accent);
                    if (status != SAO_STATUS_OK)
                        break;
                }
                if (!visual.title.empty()) {
                    const uint32_t title_color = section_role
                                                      ? theme.colors[SAO_UI_TOKEN_APP_TEXT_2]
                                                      : theme.colors[SAO_UI_TOKEN_APP_TEXT];
                    const float title_leading = section_role ? rail_width
                                                             : group_role ? paint_inset : 0.0F;
                    const float title_x = x + static_cast<float>(theme.metrics[SAO_UI_METRIC_PADDING_S]) +
                                          title_leading;
                    const float title_width = std::max(0.0F,
                        width - static_cast<float>(theme.metrics[SAO_UI_METRIC_PADDING_S]) * 2.0F -
                        title_leading);
                    const std::string visible_title = ellipsize_utf8(visual.title,
                        static_cast<size_t>(title_width / 6.5F));
                    status = sao_ui_paint_ctx_draw_utf8(
                        context, title_x, y + static_cast<float>(theme.metrics[SAO_UI_METRIC_PADDING_S]),
                        visible_title.c_str(), 11.0F, title_color);
                    if (status != SAO_STATUS_OK)
                        break;
                }
            }
        }
        if (status == SAO_STATUS_OK) {
            for (size_t index = 0; index < content->widgets.size(); ++index) {
                auto widget = content->widgets[index].get();
                SaoUiRect rect{};
                SaoUiRect node_clip{};
                if (!sao::ui::detail::layout_visual_geometry(widget->node, rect, node_clip))
                    continue;
                rect.y_px -= content->scroll_offset_px;
                node_clip.y_px -= content->scroll_offset_px;
                status = sao_ui_paint_ctx_push_clip(context, static_cast<float>(node_clip.x_px),
                                                    static_cast<float>(node_clip.y_px),
                                                    static_cast<float>(node_clip.width_px),
                                                    static_cast<float>(node_clip.height_px));
                if (status != SAO_STATUS_OK)
                    break;
                status = sao_ui_widget_paint_at(widget->handle, context, rect.x_px, rect.y_px,
                                                std::max(1, rect.width_px),
                                                std::max(1, rect.height_px), 1.0F);
                const sao_status_t widget_pop = sao_ui_paint_ctx_pop_clip(context);
                if (status == SAO_STATUS_OK)
                    status = widget_pop;
                if (status != SAO_STATUS_OK)
                    break;
            }
        }
        if (status == SAO_STATUS_OK && content->explicit_viewports) {
            const auto bars = sao::ui::detail::layout_scrollbars(content->root);
            for (const auto& bar : bars) {
                status = sao_ui_paint_ctx_push_clip(
                    context, static_cast<float>(bar.clip.x_px), static_cast<float>(bar.clip.y_px),
                    static_cast<float>(bar.clip.width_px), static_cast<float>(bar.clip.height_px));
                if (status != SAO_STATUS_OK)
                    break;
                status = sao_ui_paint_ctx_fill_rounded_rect(
                    context, static_cast<float>(bar.track.x_px), static_cast<float>(bar.track.y_px),
                    static_cast<float>(bar.track.width_px), static_cast<float>(bar.track.height_px),
                    2.0F, theme.colors[SAO_UI_TOKEN_SCROLLBAR_TRACK]);
                if (status == SAO_STATUS_OK)
                    status = sao_ui_paint_ctx_fill_rounded_rect(
                        context, static_cast<float>(bar.thumb.x_px),
                        static_cast<float>(bar.thumb.y_px), static_cast<float>(bar.thumb.width_px),
                        static_cast<float>(bar.thumb.height_px), 2.0F,
                        theme.colors[bar.active ? SAO_UI_TOKEN_SCROLLBAR_PRESSED
                                                : SAO_UI_TOKEN_SCROLLBAR_THUMB]);
                const sao_status_t bar_pop = sao_ui_paint_ctx_pop_clip(context);
                if (status == SAO_STATUS_OK)
                    status = bar_pop;
                if (status != SAO_STATUS_OK)
                    break;
            }
        }
        if (status == SAO_STATUS_OK) {
            const PanelScrollbarGeometry geometry = panel_scrollbar_geometry(
                state.width, state.height, top, viewport_height, content->content_extent_px,
                content->scroll_offset_px, theme);
            if (!content->explicit_viewports && geometry.visible) {
                const uint32_t track = theme.colors[SAO_UI_TOKEN_SCROLLBAR_TRACK];
                const uint32_t thumb = scrollbar_pressed
                                           ? theme.colors[SAO_UI_TOKEN_SCROLLBAR_PRESSED]
                                           : (scrollbar_hovered
                                                  ? theme.colors[SAO_UI_TOKEN_SCROLLBAR_HOVER]
                                                  : theme.colors[SAO_UI_TOKEN_SCROLLBAR_THUMB]);
                status = sao_ui_paint_ctx_fill_rounded_rect(
                    context, geometry.track_x, geometry.track_y, geometry.track_width,
                    geometry.track_height, 2.0F, track);
                if (status == SAO_STATUS_OK)
                    status = sao_ui_paint_ctx_fill_rounded_rect(
                        context, geometry.thumb_x, geometry.thumb_y, geometry.thumb_width,
                        geometry.thumb_height, 2.0F, thumb);
            }
        }
        if (status == SAO_STATUS_OK) {
            for (const auto& owned : content->widgets) {
                if (owned->type != "dropdown")
                    continue;
                bool open = false;
                if (sao_ui_dropdown_button_popup_is_open(owned->handle, &open) != SAO_STATUS_OK ||
                    !open)
                    continue;
                SaoUiRect rect{};
                SaoUiRect popup_clip{};
                if (!sao::ui::detail::layout_visual_geometry(owned->node, rect, popup_clip))
                    continue;
                rect.y_px -= content->scroll_offset_px;
                popup_clip.y_px -= content->scroll_offset_px;
                status = sao_ui_paint_ctx_push_clip(context, static_cast<float>(popup_clip.x_px),
                                                    static_cast<float>(popup_clip.y_px),
                                                    static_cast<float>(popup_clip.width_px),
                                                    static_cast<float>(popup_clip.height_px));
                if (status != SAO_STATUS_OK)
                    break;
                status = sao_ui_widget_paint_at(owned->handle, context, rect.x_px, rect.y_px,
                                                std::max(1, rect.width_px),
                                                std::max(1, rect.height_px), 1.0F);
                const sao_status_t popup_pop = sao_ui_paint_ctx_pop_clip(context);
                if (status == SAO_STATUS_OK)
                    status = popup_pop;
                break;
            }
        }
        const sao_status_t pop_status = sao_ui_paint_ctx_pop_clip(context);
        if (status == SAO_STATUS_OK)
            status = pop_status;
    }
    PanelCallbackLease render_lease(panel, CallbackKind::Render);
    if (status == SAO_STATUS_OK and render_lease and render_lease.render() != nullptr) {
        try {
            render_lease.render()(context, 0.0F, 0.0F, static_cast<float>(state.width),
                                  static_cast<float>(state.height), render_lease.user_data());
        } catch (...) {
            status = SAO_STATUS_ERR_UNKNOWN;
        }
    }
    if (status == SAO_STATUS_OK) {
        status = sao::ui::detail::paint_rounded_rect_stroke(
            context, 0.5F, 0.5F, static_cast<float>(std::max(1, state.width - 1)),
            static_cast<float>(std::max(1, state.height - 1)),
            static_cast<float>(theme.metrics[SAO_UI_METRIC_BORDER_RADIUS_MEDIUM]), 1.0F,
            theme_color_with_alpha(theme.colors[SAO_UI_TOKEN_APP_BORDER], 0xd0U));
    }
    const sao_status_t end_status = sao_ui_paint_ctx_end_frame(context);
    sao_ui_paint_ctx_destroy(context);
    return status == SAO_STATUS_OK ? end_status : status;
}

sao_status_t render_frame(const std::shared_ptr<PanelContent>& content, const SaoPanelState& state,
                          bool show_titlebar, const std::string& title, bool show_close_button,
                          const sao::ui::detail::PanelResolvedTheme& theme,
                          sao_ui_panel_s* panel, PanelFrame* out_frame) {
    SaoUiOffscreenRasterDesc desc{};
    desc.width_px = static_cast<uint32_t>(state.width);
    desc.height_px = static_cast<uint32_t>(state.height);
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_status_t status = sao_ui_offscreen_raster_create(&desc, &raster);
    if (status != SAO_STATUS_OK)
        return status;
    status = paint_panel(content, state, show_titlebar, title, show_close_button, theme, panel, raster);
    if (status == SAO_STATUS_OK) {
        size_t required = 0;
        status = sao_ui_offscreen_raster_snapshot(raster, nullptr, 0, &required, &out_frame->width,
                                                  &out_frame->height, &out_frame->stride);
        if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
            out_frame->pixels.resize(required);
            status = sao_ui_offscreen_raster_snapshot(
                raster, out_frame->pixels.data(), out_frame->pixels.size(), &required,
                &out_frame->width, &out_frame->height, &out_frame->stride);
        }
    }
    sao_ui_offscreen_raster_destroy(raster);
    return status;
}

sao::ui::detail::PanelResolvedTheme resolve_panel_theme(sao_ui_panel_s* panel) {
    auto theme = sao::ui::detail::resolve_process_theme();
    PanelThemeOverrides overrides;
    std::string theme_page;
    {
        std::lock_guard lock(panel->mutex);
        overrides = panel->theme_overrides;
        theme_page = panel->theme_page;
    }
    std::transform(theme_page.begin(), theme_page.end(), theme_page.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const auto page_has_suffix = [&theme_page](std::string_view suffix) {
        return theme_page.size() >= suffix.size() &&
               theme_page.compare(theme_page.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    SaoUiThemeId page_theme = theme.theme_id;
    if (theme_page == "dark" || page_has_suffix("_dark"))
        page_theme = SAO_UI_THEME_DARK;
    else if (theme_page == "light" || page_has_suffix("_light"))
        page_theme = SAO_UI_THEME_LIGHT;
    else if (theme_page == "glass" || page_has_suffix("_glass"))
        page_theme = SAO_UI_THEME_GLASS;
    if (page_theme != theme.theme_id)
        theme = sao::ui::detail::resolve_theme(page_theme, theme.generation);
    if (!theme.high_contrast) {
        for (size_t index = 0; index < overrides.has_color.size(); ++index) {
            if (overrides.has_color[index])
                theme.colors[index] = overrides.colors[index];
        }
    }
    return theme;
}

bool consume_theme_upload_failure() noexcept {
    int32_t remaining = g_theme_upload_failure_count.load(std::memory_order_acquire);
    while (remaining > 0) {
        if (g_theme_upload_failure_count.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}

sao_status_t prepare_panel_theme_layout(
    sao_ui_panel_s* panel, const sao::ui::detail::PanelResolvedTheme& theme,
    std::shared_ptr<PanelContent>* out_content, bool* out_rebuilt) {
    if (panel == nullptr || out_content == nullptr || out_rebuilt == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_content = nullptr;
    *out_rebuilt = false;
    SaoPanelState state{};
    bool show_titlebar = false;
    std::shared_ptr<PanelContent> content;
    {
        std::lock_guard lock(panel->mutex);
        state = panel->state;
        show_titlebar = panel->show_titlebar;
        content = panel->content;
    }
    if (content == nullptr) {
        *out_content = content;
        return SAO_STATUS_OK;
    }
    std::shared_ptr<PanelContent> responsive_content;
    bool responsive_rebuilt = false;
    const sao_status_t responsive_status = prepare_responsive_content(
        panel, theme, state.width, state.height,
        show_titlebar ? titlebar_height(theme) : 0, &responsive_content, &responsive_rebuilt);
    if (responsive_status != SAO_STATUS_OK)
        return responsive_status;
    content = responsive_content;
    *out_content = content;
    *out_rebuilt = responsive_rebuilt;
    {
        std::lock_guard lock(content->mutex);
        const SaoUiLayoutSpec previous_root_spec = content->root_spec;
        const auto previous_layout_theme = content->layout_theme;
        const int32_t previous_scroll_offset = content->scroll_offset_px;
        const int32_t previous_content_extent = content->content_extent_px;
        const int32_t previous_panel_width = content->panel_width_px;
        const int32_t previous_viewport_top = content->viewport_top_px;
        const int32_t previous_viewport_height = content->viewport_height_px;
        std::vector<SaoUiLayoutSpec> previous_layout_specs;
        previous_layout_specs.reserve(content->theme_layout_nodes.size());
        std::vector<uint32_t> previous_accents;
        previous_accents.reserve(content->containers.size());
        for (const auto& layout_node : content->theme_layout_nodes)
            previous_layout_specs.push_back(layout_node.spec);
        for (const auto& visual : content->containers)
            previous_accents.push_back(visual.accent);
        for (auto& visual : content->containers) {
            if (!visual.accent_is_explicit)
                visual.accent = theme.colors[static_cast<size_t>(visual.accent_token)];
        }
        const auto rollback = [&]() noexcept {
            bool restored = true;
            content->root_spec = previous_root_spec;
            if (sao_ui_layout_node_set_spec(content->root, &content->root_spec) != SAO_STATUS_OK)
                restored = false;
            for (size_t index = 0; index < content->theme_layout_nodes.size(); ++index) {
                if (index >= previous_layout_specs.size()) {
                    restored = false;
                    continue;
                }
                auto& layout_node = content->theme_layout_nodes[index];
                layout_node.spec = previous_layout_specs[index];
                if (sao_ui_layout_node_set_spec(layout_node.node, &layout_node.spec) != SAO_STATUS_OK)
                    restored = false;
            }
            for (size_t index = 0; index < content->containers.size(); ++index) {
                if (index < previous_accents.size())
                    content->containers[index].accent = previous_accents[index];
            }
            if (arrange_content(*content, previous_panel_width,
                                previous_viewport_top + previous_viewport_height,
                                previous_viewport_top) != SAO_STATUS_OK)
                restored = false;
            content->scroll_offset_px = previous_scroll_offset;
            content->content_extent_px = previous_content_extent;
            content->panel_width_px = previous_panel_width;
            content->viewport_top_px = previous_viewport_top;
            content->viewport_height_px = previous_viewport_height;
            content->layout_theme = previous_layout_theme;
            return restored;
        };
        const int32_t next_viewport_top = show_titlebar ? titlebar_height(theme) : 0;
        const int32_t next_viewport_height = std::max(1, state.height - next_viewport_top);
        const bool geometry_changed = previous_panel_width != state.width ||
                                      previous_viewport_top != next_viewport_top ||
                                      previous_viewport_height != next_viewport_height;
        if (content->layout_theme.metrics == theme.metrics) {
            if (geometry_changed) {
                const sao_status_t status =
                    arrange_content(*content, state.width, state.height, next_viewport_top);
                if (status != SAO_STATUS_OK)
                    return rollback() ? status : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
            }
            content->layout_theme = theme;
            return SAO_STATUS_OK;
        }
        if (content->uses_theme_layout_metrics) {
            const int32_t body_padding = theme.metrics[SAO_UI_METRIC_PADDING_M];
            content->root_spec.pad_top_px = body_padding;
            content->root_spec.pad_right_px = body_padding;
            content->root_spec.pad_bottom_px = body_padding;
            content->root_spec.pad_left_px = body_padding;
            content->root_spec.gap_px = theme.metrics[SAO_UI_METRIC_GAP_S];
        }
        for (auto& layout_node : content->theme_layout_nodes) {
            SaoUiLayoutSpec next_spec = container_spec_for_theme(
                layout_node.semantic_type, layout_node.has_title, theme);
            apply_explicit_container_layout_metadata(layout_node.explicit_metadata, &next_spec);
            const sao_status_t status = sao_ui_layout_node_set_spec(layout_node.node, &next_spec);
            if (status != SAO_STATUS_OK)
                return rollback() ? status : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
            layout_node.spec = next_spec;
        }
        const sao_status_t status = arrange_content(
            *content, state.width, state.height,
            show_titlebar ? titlebar_height(theme) : 0);
        if (status != SAO_STATUS_OK)
            return rollback() ? status : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
        content->layout_theme = theme;
        return SAO_STATUS_OK;
    }
}

sao_status_t snapshot_panel(sao_ui_panel_s* panel,
                            const sao::ui::detail::PanelResolvedTheme& theme,
                            PanelFrame* out_frame) {
    SaoPanelState state{};
    bool titlebar = false;
    bool close_button = false;
    std::string title;
    std::shared_ptr<PanelContent> content;
    {
        std::scoped_lock lock(panel->mutex);
        state = panel->state;
        titlebar = panel->show_titlebar;
        close_button = panel->show_close_button;
        title = panel->title;
        content = panel->content;
    }
    return render_frame(content, state, titlebar, title, close_button, theme, panel, out_frame);
}

sao_status_t upload_panel(sao_ui_panel_s* panel) {
    PanelRenderGuard render_guard(panel);
    if (!render_guard)
        return SAO_UI_PANEL_STATUS_ERR_BUSY;
    constexpr int32_t kMaximumThemePasses = 8;
    for (int32_t pass = 0; pass < kMaximumThemePasses; ++pass) {
        const auto theme = resolve_panel_theme(panel);
        {
            std::lock_guard lock(panel->mutex);
            panel->requested_theme_generation =
                std::max(panel->requested_theme_generation, theme.generation);
        }
        std::shared_ptr<PanelContent> candidate_content;
        std::shared_ptr<PanelContent> previous_content;
        bool responsive_rebuilt = false;
        {
            std::lock_guard lock(panel->mutex);
            previous_content = panel->content;
        }
        sao_status_t status = prepare_panel_theme_layout(
            panel, theme, &candidate_content, &responsive_rebuilt);
        if (status != SAO_STATUS_OK) {
            std::lock_guard lock(panel->mutex);
            panel->theme_dirty = true;
            return status;
        }
        sao_ui_layer_handle_t layer = nullptr;
        SaoPanelState state{};
        bool show_titlebar = false;
        bool close_button = false;
        std::string title;
        {
            std::lock_guard lock(panel->mutex);
            state = panel->state;
            show_titlebar = panel->show_titlebar;
            close_button = panel->show_close_button;
            title = panel->title;
            layer = panel->layer;
        }
        PanelFrame frame;
        status = render_frame(candidate_content, state, show_titlebar, title, close_button,
                              theme, panel, &frame);
        if (status == SAO_STATUS_OK && consume_theme_upload_failure())
            status = SAO_STATUS_ERR_UNKNOWN;
        if (status == SAO_STATUS_OK && layer != nullptr) {
            status = sao_ui_layer_update_bgra(layer, frame.pixels.data(), frame.width,
                                              frame.height, frame.stride);
        }
        if (status != SAO_STATUS_OK) {
            std::lock_guard lock(panel->mutex);
            panel->theme_dirty = true;
            return status;
        }
        const uint64_t latest_generation = sao::ui::detail::process_theme_generation();
        bool complete = false;
        {
            std::lock_guard lock(panel->mutex);
            panel->requested_theme_generation =
                std::max(panel->requested_theme_generation, latest_generation);
            panel->uploaded_theme_generation = theme.generation;
            complete = theme.generation >= panel->requested_theme_generation;
            panel->theme_dirty = !complete;
        }
        if (complete) {
            if (responsive_rebuilt) {
                std::lock_guard lock(panel->mutex);
                if (panel->content != previous_content)
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                panel->content = std::move(candidate_content);
            }
            return SAO_STATUS_OK;
        }
    }
    return SAO_UI_PANEL_STATUS_ERR_BUSY;
}
void retry_dirty_theme(sao_ui_panel_s* panel) noexcept {
    bool dirty = false;
    {
        std::lock_guard lock(panel->mutex);
        dirty = panel->theme_dirty;
    }
    if (!dirty)
        return;
    try {
        (void)upload_panel(panel);
    } catch (...) {
    }
}

bool point_in_rect(int32_t x, int32_t y, int32_t left, int32_t top, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0)
        return false;
    const int64_t right = static_cast<int64_t>(left) + width;
    const int64_t bottom = static_cast<int64_t>(top) + height;
    return static_cast<int64_t>(x) >= left && static_cast<int64_t>(y) >= top &&
           static_cast<int64_t>(x) < right && static_cast<int64_t>(y) < bottom;
}

int32_t nearest_resize_axis_edge(int32_t coordinate, int32_t extent, int32_t leading_edge,
                                 int32_t trailing_edge) {
    const bool near_leading = coordinate <= 6;
    const bool near_trailing = static_cast<int64_t>(coordinate) >=
                               static_cast<int64_t>(extent) - 7;
    if (!near_leading)
        return near_trailing ? trailing_edge : 0;
    if (!near_trailing)
        return leading_edge;
    const int64_t leading_delta = coordinate;
    const int64_t trailing_delta =
        static_cast<int64_t>(coordinate) - (static_cast<int64_t>(extent) - 1);
    const int64_t leading_distance = leading_delta < 0 ? -leading_delta : leading_delta;
    const int64_t trailing_distance = trailing_delta < 0 ? -trailing_delta : trailing_delta;
    return leading_distance <= trailing_distance ? leading_edge : trailing_edge;
}

sao_status_t widget_at(sao_ui_panel_s* panel, int32_t x, int32_t y,
                       sao_ui_widget_handle_t* out_widget) {
    if (panel == nullptr || out_widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_widget = nullptr;
    std::shared_ptr<PanelContent> content;
    bool titlebar = false;
    SaoPanelState state{};
    {
        std::scoped_lock lock(panel->mutex);
        content = panel->content;
        titlebar = panel->show_titlebar;
        state = panel->state;
    }
    if (content == nullptr) {
        return SAO_STATUS_OK;
    }
    // A globally open dropdown popup floats above sibling widgets: probe
    // it first so hover/click over the list resolves to the dropdown
    // instead of the widgets painted underneath.
    {
        sao_ui_widget_handle_t popup_owner = nullptr;
        int32_t popup_entry = -1;
        bool popup_consumed = false;
        const sao_status_t popup_status = sao_ui_widget_dropdown_popup_hit_global(
            x, y, &popup_owner, &popup_entry, &popup_consumed);
        (void)popup_entry;
        if (popup_status == SAO_STATUS_OK && popup_consumed &&
            popup_owner != nullptr) {
            bool belongs = false;
            {
                std::lock_guard content_lock(content->mutex);
                belongs = content->by_handle.contains(popup_owner);
            }
            if (belongs) {
                *out_widget = popup_owner;
                return SAO_STATUS_OK;
            }
        }
    }
    const auto theme = resolve_panel_theme(panel);
    const int32_t top = titlebar ? titlebar_height(theme) : 0;
    std::scoped_lock lock(content->mutex);
    const int64_t bottom = static_cast<int64_t>(top) + content->viewport_height_px;
    if (static_cast<int64_t>(y) < top || static_cast<int64_t>(y) >= bottom)
        return SAO_STATUS_OK;
    const PanelScrollbarGeometry geometry = panel_scrollbar_geometry(
        state.width, state.height, top, content->viewport_height_px, content->content_extent_px,
        content->scroll_offset_px, theme);
    if (geometry.visible && point_in_rect(x, y, geometry.hit_x, geometry.hit_y,
                          geometry.hit_width, geometry.hit_height))
        return SAO_STATUS_OK;
    for (const auto& bar : sao::ui::detail::layout_scrollbars(content->root)) {
        if (point_in_rect(x, y, bar.clip.x_px, bar.clip.y_px, bar.clip.width_px,
                          bar.clip.height_px) &&
            point_in_rect(x, y, bar.track.x_px, bar.track.y_px, bar.track.width_px,
                          bar.track.height_px))
            return SAO_STATUS_OK;
    }
    const int64_t content_y = static_cast<int64_t>(y) + content->scroll_offset_px;
    if (content_y < std::numeric_limits<int32_t>::min() ||
        content_y > std::numeric_limits<int32_t>::max())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    SaoUiHitResult hit{};
    const sao_status_t status = sao_ui_layout_hit_test(
        content->root, x, static_cast<int32_t>(content_y), &hit);
    if (status != SAO_STATUS_OK)
        return status;
    if (hit.widget == nullptr)
        return SAO_STATUS_OK;
    const auto found = content->by_handle.find(hit.widget);
    if (found != content->by_handle.end() && !found->second->enabled)
        return SAO_STATUS_OK;
    *out_widget = hit.widget;
    return SAO_STATUS_OK;
}

sao_status_t resolve_action_at(sao_ui_panel_s* panel, int32_t x, int32_t y, std::string* out_action,
                               std::string* out_args) {
    std::shared_ptr<PanelContent> content;
    bool titlebar = false;
    int32_t panel_width = 0;
    int32_t panel_height = 0;
    {
        std::scoped_lock lock(panel->mutex);
        content = panel->content;
        titlebar = panel->show_titlebar;
        panel_width = panel->state.width;
        panel_height = panel->state.height;
    }
    if (content == nullptr) {
        return SAO_STATUS_OK;
    }
    const auto theme = resolve_panel_theme(panel);
    const int32_t top = titlebar ? titlebar_height(theme) : 0;
    std::scoped_lock lock(content->mutex);
    const int64_t content_bottom = static_cast<int64_t>(top) + content->viewport_height_px;
    if (static_cast<int64_t>(y) < top || static_cast<int64_t>(y) >= content_bottom)
        return SAO_STATUS_ERR_NOT_FOUND;
    const int64_t content_y = static_cast<int64_t>(y) + content->scroll_offset_px;
    if (content_y < std::numeric_limits<int32_t>::min() ||
        content_y > std::numeric_limits<int32_t>::max())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const PanelScrollbarGeometry geometry = panel_scrollbar_geometry(
        panel_width, panel_height, top, content->viewport_height_px, content->content_extent_px,
        content->scroll_offset_px, theme);
    if (geometry.visible &&
        point_in_rect(x, y, geometry.hit_x, geometry.hit_y, geometry.hit_width,
                      geometry.hit_height))
        return SAO_STATUS_ERR_NOT_FOUND;
    for (const auto& bar : sao::ui::detail::layout_scrollbars(content->root)) {
        if (point_in_rect(x, y, bar.clip.x_px, bar.clip.y_px, bar.clip.width_px,
                          bar.clip.height_px) &&
            point_in_rect(x, y, bar.track.x_px, bar.track.y_px, bar.track.width_px,
                          bar.track.height_px))
            return SAO_STATUS_ERR_NOT_FOUND;
    }
    SaoUiHitResult hit{};
    const sao_status_t status = sao_ui_layout_hit_test(
        content->root, x, static_cast<int32_t>(content_y), &hit);
    if (status != SAO_STATUS_OK or hit.widget == nullptr)
        return status == SAO_STATUS_OK ? SAO_STATUS_ERR_NOT_FOUND : status;
    const auto found = content->by_handle.find(hit.widget);
    if (found == content->by_handle.end() or not found->second->enabled or
        found->second->action.empty())
        return SAO_STATUS_ERR_NOT_FOUND;
    *out_action = found->second->action;
    *out_args = found->second->action_args;
    return SAO_STATUS_OK;
}

sao_status_t dispatch_action_at(sao_ui_panel_s* panel, int32_t x, int32_t y) {
    std::string action;
    std::string args;
    const sao_status_t status = resolve_action_at(panel, x, y, & action, & args);
    if (status != SAO_STATUS_OK)
        return status;
    PanelCallbackLease callback(panel, CallbackKind::Action);
    if (not callback or callback.action() == nullptr)
        return SAO_STATUS_OK;
    try {
        callback.action()(action.c_str(), reinterpret_cast<const uint8_t*>(args.data()),
                          args.size(), callback.user_data());
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return SAO_STATUS_OK;
}

sao_status_t update_slider_from_pointer(sao_ui_panel_s* panel, sao_ui_widget_handle_t handle,
                                        int32_t x, int32_t y) {
    std::shared_ptr<PanelContent> content;
    {
        std::lock_guard lock(panel->mutex);
        content = panel->content;
    }
    if (content == nullptr)
        return SAO_STATUS_ERR_NOT_FOUND;
    SaoUiRect rect{};
    SaoUiRect clip{};
    bool vertical = false;
    {
        std::lock_guard lock(content->mutex);
        const auto found = content->by_handle.find(handle);
        if (found == content->by_handle.end() || found->second->type != "slider")
            return SAO_STATUS_ERR_NOT_FOUND;
        vertical = found->second->props.value("vertical", false);
        if (!sao::ui::detail::layout_visual_geometry(found->second->node, rect, clip))
            return SAO_STATUS_ERR_NOT_FOUND;
        rect.y_px -= content->scroll_offset_px;
    }
    const float value = vertical
                            ? 1.0F - std::clamp(static_cast<float>(y - rect.y_px) /
                                                    static_cast<float>(std::max(1, rect.height_px)),
                                                0.0F, 1.0F)
                            : std::clamp(static_cast<float>(x - rect.x_px) /
                                             static_cast<float>(std::max(1, rect.width_px)),
                                         0.0F, 1.0F);
    return sao_ui_slider_set_value(handle, value);
}

sao_status_t dispatch_owned_action(sao_ui_panel_s* panel, sao_ui_widget_handle_t handle,
                                   bool include_control_value) {
    std::shared_ptr<PanelContent> content;
    {
        std::lock_guard lock(panel->mutex);
        content = panel->content;
    }
    if (content == nullptr)
        return SAO_STATUS_ERR_NOT_FOUND;
    std::string action;
    std::string args;
    std::string widget_id;
    json spec_patch = json::object();
    {
        std::lock_guard lock(content->mutex);
        const auto found = content->by_handle.find(handle);
        if (found == content->by_handle.end() || !found->second->enabled)
            return SAO_STATUS_ERR_NOT_FOUND;
        action = found->second->action;
        widget_id = found->second->id;
        json payload = json::object();
        try {
            payload = json::parse(found->second->action_args);
            if (!payload.is_object())
                payload = json::object();
        } catch (...) {
            payload = json::object();
        }
        if (include_control_value) {
            if (found->second->type == "slider") {
                payload["value"] = found->second->slider_value;
                payload["text"] = found->second->slider_value;
                spec_patch["value"] = found->second->slider_value;
            } else if (found->second->type == "dropdown") {
                payload["selected_id"] = found->second->dropdown_selected_id;
                payload["value"] = found->second->props.value("value", std::string());
                payload["text"] = found->second->props.value("text", std::string());
                spec_patch["selected_id"] = found->second->dropdown_selected_id;
            }
        }
        args = payload.dump();
    }
    if (!widget_id.empty() && !spec_patch.empty()) {
        std::lock_guard lock(panel->mutex);
        if (panel->content == content && !panel->spec_json.empty()) {
            try {
                json document = json::parse(panel->spec_json);
                json* nodes = document.contains("nodes") ? &document["nodes"] : &document;
                if (merge_widget_spec_by_id(*nodes, widget_id, spec_patch))
                    panel->spec_json = document.dump();
            } catch (...) {
            }
        }
    }
    if (action.empty())
        return SAO_STATUS_OK;
    PanelCallbackLease callback(panel, CallbackKind::Action);
    if (!callback || callback.action() == nullptr)
        return SAO_STATUS_OK;
    try {
        callback.action()(action.c_str(), reinterpret_cast<const uint8_t*>(args.data()),
                          args.size(), callback.user_data());
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t set_hovered_widget(sao_ui_panel_s* panel, sao_ui_widget_handle_t next,
                                bool* visual_changed) {
    sao_ui_widget_handle_t previous = nullptr;
    {
        std::lock_guard lock(panel->mutex);
        previous = panel->hovered_widget;
        if (previous == next)
            return SAO_STATUS_OK;
        panel->hovered_widget = next;
        *visual_changed = true;
    }
    sao_status_t status = SAO_STATUS_OK;
    if (previous != nullptr)
        status = sao_ui_widget_set_hovered(previous, false);
    if (next != nullptr) {
        const sao_status_t next_status = sao_ui_widget_set_hovered(next, true);
        if (status == SAO_STATUS_OK && next_status != SAO_STATUS_OK)
            status = next_status;
    }
    return status;
}

sao_status_t update_scrollbar_hover(sao_ui_panel_s* panel, int32_t x, int32_t y,
                                    bool* visual_changed) {
    const auto theme = resolve_panel_theme(panel);
    SaoPanelState state{};
    bool titlebar = false;
    std::shared_ptr<PanelContent> content;
    {
        std::scoped_lock lock(panel->mutex);
        state = panel->state;
        titlebar = panel->show_titlebar;
        content = panel->content;
    }
    if (content == nullptr) {
        return SAO_STATUS_OK;
    }
    const int32_t top = titlebar ? titlebar_height(theme) : 0;
    bool hovered = false;
    {
        std::scoped_lock lock(content->mutex);
        const PanelScrollbarGeometry geometry = panel_scrollbar_geometry(
            state.width, state.height, top, content->viewport_height_px,
            content->content_extent_px,
            content->scroll_offset_px, theme);
        hovered = geometry.visible &&
                  point_in_rect(x, y, geometry.hit_x, geometry.hit_y, geometry.hit_width,
                                geometry.hit_height);
    }
    {
        std::lock_guard lock(panel->mutex);
        if (panel->scrollbar_hovered != hovered) {
            panel->scrollbar_hovered = hovered;
            *visual_changed = true;
        }
    }
    return SAO_STATUS_OK;
}

sao_status_t panel_cursor_pos(sao_ui_panel_s* panel, int32_t ix, int32_t iy) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (not operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    int32_t mode = 0;
    int32_t edges = 0;
    int32_t start_x = 0;
    int32_t start_y = 0;
    bool movable = false;
    bool resizable = false;
    SaoPanelState start_state{};
    {
        std::scoped_lock lock(panel->mutex);
        mode = panel->interaction_mode;
        movable = panel->movable;
        resizable = panel->resizable;
        edges = panel->resize_edges;
        start_x = panel->pointer_down_x;
        start_y = panel->pointer_down_y;
        start_state = panel->interaction_start_state;
        panel->pointer_x = ix;
        panel->pointer_y = iy;
    }
    const int64_t delta_x = static_cast<int64_t>(ix) - start_x;
    const int64_t delta_y = static_cast<int64_t>(iy) - start_y;
    if (mode == 1 && movable) {
        return sao_ui_panel_set_position(
            panel, clamp_i64_to_i32(saturating_add_i64(start_state.x, delta_x)),
            clamp_i64_to_i32(saturating_add_i64(start_state.y, delta_y)));
    }
    if (mode == 2 && resizable) {
        int64_t next_x = start_state.x;
        int64_t next_y = start_state.y;
        int64_t next_w = start_state.width;
        int64_t next_h = start_state.height;
        if ((edges & 1) != 0) {
            next_x = saturating_add_i64(next_x, delta_x);
            next_w = saturating_sub_i64(next_w, delta_x);
        }
        if ((edges & 2) != 0)
            next_w = saturating_add_i64(next_w, delta_x);
        if ((edges & 4) != 0) {
            next_y = saturating_add_i64(next_y, delta_y);
            next_h = saturating_sub_i64(next_h, delta_y);
        }
        if ((edges & 8) != 0)
            next_h = saturating_add_i64(next_h, delta_y);
        return sao_ui_panel_set_geometry(
            panel, clamp_i64_to_i32(next_x), clamp_i64_to_i32(next_y),
            clamp_i64_to_i32(next_w), clamp_i64_to_i32(next_h));
    }
    if (mode == 5) {
        sao_ui_widget_handle_t slider = nullptr;
        {
            std::lock_guard lock(panel->mutex);
            slider = panel->slider_drag_widget;
        }
        const sao_status_t slider_status = update_slider_from_pointer(panel, slider, ix, iy);
        if (slider_status != SAO_STATUS_OK)
            return slider_status;
        return upload_panel(panel);
    }
    if (mode == 0 || mode == 4) {
        std::shared_ptr<PanelContent> content;
        {
            std::lock_guard lock(panel->mutex);
            content = panel->content;
        }
        if (content != nullptr) {
            bool changed = false;
            {
                std::lock_guard lock(content->mutex);
                changed = sao::ui::detail::layout_scrollbar_pointer(
                    content->root, ix, iy + content->scroll_offset_px, 2);
            }
            if (changed)
                return upload_panel(panel);
        }
    }
    if (mode == 3) {
        std::shared_ptr<PanelContent> content;
        SaoPanelState state{};
        int32_t drag_start_offset = 0;
        bool show_titlebar = false;
        const auto theme = resolve_panel_theme(panel);
        {
            std::lock_guard lock(panel->mutex);
            state = panel->state;
            show_titlebar = panel->show_titlebar;
            drag_start_offset = panel->scrollbar_drag_start_offset;
            content = panel->content;
        }
        const int32_t top = show_titlebar ? titlebar_height(theme) : 0;
        if (content != nullptr) {
            std::lock_guard lock(content->mutex);
            const PanelScrollbarGeometry geometry = panel_scrollbar_geometry(
                state.width, state.height, top, content->viewport_height_px,
                content->content_extent_px,
                drag_start_offset, theme);
            const long double mapped = geometry.thumb_travel > 0.0F
                                           ? static_cast<long double>(drag_start_offset) +
                                                 static_cast<long double>(delta_y) *
                                                     static_cast<long double>(geometry.range) /
                                                     static_cast<long double>(geometry.thumb_travel)
                                           : static_cast<long double>(drag_start_offset);
            const int64_t mapped_offset = rounded_finite_long_double_to_i64(mapped);
            content->scroll_offset_px = clamp_i64_to_i32(std::clamp(
                mapped_offset, int64_t{0}, static_cast<int64_t>(geometry.range)));
        }
        return upload_panel(panel);
    }
    sao_ui_widget_handle_t hit = nullptr;
    sao_status_t status = widget_at(panel, ix, iy, &hit);
    if (status != SAO_STATUS_OK)
        return status;
    bool visual_changed = false;
    sao_ui_widget_handle_t popup_owner = nullptr;
    int32_t popup_entry = -1;
    bool popup_consumed = false;
    (void)sao_ui_widget_dropdown_popup_hit_global(ix, iy, &popup_owner, &popup_entry,
                                                  &popup_consumed);
    (void)popup_entry;
    if (popup_consumed && popup_owner == hit)
        visual_changed = true;
    status = set_hovered_widget(panel, hit, &visual_changed);
    const sao_status_t scrollbar_status = update_scrollbar_hover(panel, ix, iy, &visual_changed);
    if (status == SAO_STATUS_OK)
        status = scrollbar_status;
    if (visual_changed) {
        const sao_status_t upload_status = upload_panel(panel);
        if (status == SAO_STATUS_OK)
            status = upload_status;
    }
    return status;
}

sao_status_t panel_cursor_leave(sao_ui_panel_s* panel) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (not operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    bool changed = false;
    sao_status_t status = set_hovered_widget(panel, nullptr, &changed);
    {
        std::lock_guard lock(panel->mutex);
        if (panel->scrollbar_hovered) {
            panel->scrollbar_hovered = false;
            changed = true;
        }
    }
    if (changed) {
        const sao_status_t upload_status = upload_panel(panel);
        if (status == SAO_STATUS_OK)
            status = upload_status;
    }
    return status;
}

sao_status_t panel_scroll(sao_ui_panel_s* panel, float dx, float dy) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!std::isfinite(dx) || !std::isfinite(dy))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (not operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::shared_ptr<PanelContent> content;
    int32_t pointer_x = 0;
    int32_t pointer_y = 0;
    SaoPanelState panel_state{};
    sao_ui_compositor_handle_t compositor = nullptr;
    {
        std::lock_guard lock(panel->mutex);
        content = panel->content;
        pointer_x = panel->pointer_x;
        pointer_y = panel->pointer_y;
        panel_state = panel->state;
        compositor = panel->compositor;
    }
    if (content == nullptr) {
        return SAO_STATUS_OK;
    }
#if defined(_WIN32)
    // Wheel callbacks carry deltas but no coordinates.  Resolve the cursor at
    // dispatch time so a wheel that follows a programmatic pointer move (or a
    // forwarded native EDIT wheel) selects the viewport beneath the cursor
    // rather than whichever viewport received the last WM_MOUSEMOVE callback.
    const HWND host = compositor != nullptr
                          ? reinterpret_cast<HWND>(sao_ui_compositor_host_hwnd(compositor))
                          : nullptr;
    POINT live_pointer{};
    if (host != nullptr && ::GetCursorPos(&live_pointer) != FALSE &&
        ::ScreenToClient(host, &live_pointer) != FALSE) {
        pointer_x = clamp_i64_to_i32(static_cast<int64_t>(live_pointer.x) - panel_state.x);
        pointer_y = clamp_i64_to_i32(static_cast<int64_t>(live_pointer.y) - panel_state.y);
        std::lock_guard lock(panel->mutex);
        panel->pointer_x = pointer_x;
        panel->pointer_y = pointer_y;
    }
#endif
    bool viewport_changed = false;
    {
        std::lock_guard lock(content->mutex);
        const int32_t delta_x = clamp_i64_to_i32(
            rounded_finite_long_double_to_i64(-static_cast<long double>(dx) * 32.0L));
        const int32_t delta_y = clamp_i64_to_i32(
            rounded_finite_long_double_to_i64(-static_cast<long double>(dy) * 32.0L));
        viewport_changed = sao::ui::detail::layout_scroll_at(
            content->root, pointer_x, pointer_y + content->scroll_offset_px, delta_x, delta_y);
        if (!viewport_changed) {
            const int64_t range =
                std::max(int64_t{0}, static_cast<int64_t>(content->content_extent_px) -
                                         static_cast<int64_t>(content->viewport_height_px));
            const long double scaled = -static_cast<long double>(dy) * 32.0L;
            const int64_t delta = rounded_finite_long_double_to_i64(scaled);
            const int64_t next_offset = saturating_add_i64(content->scroll_offset_px, delta);
            content->scroll_offset_px =
                clamp_i64_to_i32(std::clamp(next_offset, int64_t{0}, range));
        }
    }
    return upload_panel(panel);
}

void SAO_UI_CALL layer_cursor_pos_callback(float x, float y, void* user_data) {
    if (user_data != nullptr)
        (void)panel_cursor_pos(static_cast<sao_ui_panel_s*>(user_data),
                               saturating_float_to_i32(x), saturating_float_to_i32(y));
}

void SAO_UI_CALL layer_cursor_leave_callback(void* user_data) {
    if (user_data != nullptr)
        (void)panel_cursor_leave(static_cast<sao_ui_panel_s*>(user_data));
}

void SAO_UI_CALL layer_scroll_callback(float dx, float dy, void* user_data) {
    if (user_data != nullptr)
        (void)panel_scroll(static_cast<sao_ui_panel_s*>(user_data), dx, dy);
}

sao_status_t notify(sao_ui_panel_s* panel, int32_t event);

sao_status_t begin_text_edit(sao_ui_panel_s* panel, sao_ui_widget_handle_t widget) {
    std::shared_ptr<PanelContent> content;
    SaoPanelState state{};
    {
        std::lock_guard lock(panel->mutex);
        content = panel->content;
        state = panel->state;
    }
    if (content == nullptr)
        return SAO_STATUS_ERR_NOT_FOUND;
    SaoUiRect rect{};
    SaoUiRect clip{};
    std::string widget_id;
    {
        std::lock_guard lock(content->mutex);
        const auto found = content->by_handle.find(widget);
        if (found == content->by_handle.end() || found->second->type != "input")
            return SAO_STATUS_ERR_NOT_FOUND;
        widget_id = found->second->id;
        if (!sao::ui::detail::layout_visual_geometry(found->second->node, rect, clip))
            return SAO_STATUS_ERR_NOT_FOUND;
        rect.y_px -= content->scroll_offset_px;
    }
    void* hwnd = sao_ui_compositor_host_hwnd(panel->compositor);
    if (hwnd == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    void* previous_hwnd = nullptr;
    sao_ui_widget_handle_t previous_widget = nullptr;
    {
        std::lock_guard lock(text_edit_anchor_mutex());
        const auto& anchor = text_edit_anchor();
        if (anchor.hwnd != nullptr && anchor.widget != nullptr &&
            (anchor.panel != panel || anchor.widget != widget)) {
            previous_hwnd = anchor.hwnd;
            previous_widget = anchor.widget;
        }
    }
    if (previous_hwnd != nullptr && previous_widget != nullptr)
        sao::ui::detail::end_native_text_edit_for_widget(previous_hwnd, previous_widget, true);
    {
        std::lock_guard lock(text_edit_anchor_mutex());
        text_edit_anchor() = {panel, widget, hwnd, widget_id};
    }
    const bool started = sao::ui::detail::begin_native_text_edit(
        hwnd, widget, state.x + rect.x_px, state.y + rect.y_px, std::max(1, rect.width_px),
        std::max(1, rect.height_px),
        [panel, widget_id](const std::string& value, sao::ui::detail::TextEditPhase phase) {
            {
                std::lock_guard anchor_lock(text_edit_anchor_mutex());
                const auto& anchor = text_edit_anchor();
                if (anchor.panel != panel || anchor.widget_id != widget_id)
                    return;
            }
            PanelOperation operation(panel);
            if (!operation)
                return;
            std::shared_ptr<PanelContent> current;
            std::string action;
            std::string action_args;
            {
                std::lock_guard panel_lock(panel->mutex);
                current = panel->content;
            }
            if (current == nullptr)
                return;
            sao_ui_widget_handle_t current_handle = nullptr;
            bool text_changed = false;
            {
                std::lock_guard content_lock(current->mutex);
                const auto found = current->by_id.find(widget_id);
                if (found == current->by_id.end() || found->second->type != "input")
                    return;
                current_handle = found->second->handle;
                const auto previous = found->second->props.find("value");
                text_changed = previous == found->second->props.end() || !previous->is_string() ||
                               previous->get<std::string>() != value;
                found->second->props["value"] = value;
                found->second->props["text"] = value;
                action = found->second->action;
                action_args = found->second->action_args;
            }
            sao::ui::detail::TextEditSnapshot current_text;
            if (sao::ui::detail::text_edit_snapshot(current_handle, current_text) &&
                current_text.text != value)
                (void)sao_ui_text_field_set_text(current_handle, value.c_str());
            if (text_changed) {
                std::lock_guard panel_lock(panel->mutex);
                try {
                    if (!panel->spec_json.empty()) {
                        json document = json::parse(panel->spec_json);
                        json* nodes = document.contains("nodes") ? &document["nodes"] : &document;
                        (void)merge_widget_spec_by_id(*nodes, widget_id,
                                                      json{{"value", value}, {"text", value}});
                        panel->spec_json = document.dump();
                    }
                } catch (...) {
                }
            }
            (void)upload_panel(panel);
            if (!action.empty() &&
                (text_changed || phase != sao::ui::detail::TextEditPhase::Change)) {
                const char* phase_name = phase == sao::ui::detail::TextEditPhase::Change ? "change"
                                         : phase == sao::ui::detail::TextEditPhase::Cancel
                                             ? "cancel"
                                             : "commit";
                json payload = json::object();
                try {
                    payload = json::parse(action_args);
                    if (!payload.is_object())
                        payload = json::object();
                } catch (...) {
                    payload = json::object();
                }
                payload["text"] = value;
                payload["value"] = value;
                payload["phase"] = phase_name;
                const std::string args = payload.dump();
                PanelCallbackLease callback(panel, CallbackKind::Action);
                if (callback && callback.action() != nullptr) {
                    try {
                        callback.action()(action.c_str(),
                                          reinterpret_cast<const uint8_t*>(args.data()),
                                          args.size(), callback.user_data());
                    } catch (...) {
                    }
                }
            }
            if (phase != sao::ui::detail::TextEditPhase::Change) {
                std::lock_guard anchor_lock(text_edit_anchor_mutex());
                if (text_edit_anchor().panel == panel && text_edit_anchor().widget_id == widget_id)
                    text_edit_anchor() = {};
            }
        });
    if (!started) {
        std::lock_guard lock(text_edit_anchor_mutex());
        if (text_edit_anchor().panel == panel && text_edit_anchor().widget == widget)
            text_edit_anchor() = {};
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return SAO_STATUS_OK;
}

sao_status_t panel_button(sao_ui_panel_s* panel, int32_t button, int32_t action, int32_t ix,
                          int32_t iy) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (not operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (button != 0)
        return SAO_STATUS_OK;
    const auto theme = resolve_panel_theme(panel);
    SaoPanelState state{};
    bool titlebar = false;
    bool close_button = false;
    bool movable = false;
    bool resizable = false;
    {
        std::scoped_lock lock(panel->mutex);
        state = panel->state;
        titlebar = panel->show_titlebar;
        close_button = panel->show_close_button;
        movable = panel->movable;
        resizable = panel->resizable;
    }
    const int32_t top = titlebar ? titlebar_height(theme) : 0;
    sao_status_t status = SAO_STATUS_OK;
    if (action == 0) {
        int32_t mode = 0;
        int32_t edges = 0;
        const bool close_armed =
            close_button && point_in_rect(ix, iy, state.width - top, 0, top, top);
        std::shared_ptr<PanelContent> content;
        sao_ui_widget_handle_t previous_pressed = nullptr;
        {
            std::lock_guard lock(panel->mutex);
            content = panel->content;
            previous_pressed = panel->pressed_widget;
            panel->pressed_widget = nullptr;
            panel->pointer_down_x = ix;
            panel->pointer_down_y = iy;
            panel->interaction_start_state = state;
            panel->close_armed = close_armed;
            panel->interaction_mode = 0;
            panel->resize_edges = 0;
            panel->scrollbar_pressed = false;
        }
        if (previous_pressed != nullptr)
            status = sao_ui_widget_set_pressed(previous_pressed, false);
        bool handled_scrollbar = false;
        bool page_changed = false;
        bool scrollbar_pressed = false;
        int32_t drag_start_y = 0;
        int32_t drag_start_offset = 0;
        if (resizable && !close_armed) {
            edges |= nearest_resize_axis_edge(ix, state.width, 1, 2);
            edges |= nearest_resize_axis_edge(iy, state.height, 4, 8);
            if (edges != 0)
                mode = 2;
        }
        if (mode == 0 && !close_armed && content != nullptr) {
            std::lock_guard lock(content->mutex);
            if (sao::ui::detail::layout_scrollbar_pointer(content->root, ix,
                                                          iy + content->scroll_offset_px, 0)) {
                handled_scrollbar = true;
                scrollbar_pressed = true;
                mode = 4;
            }
            const PanelScrollbarGeometry geometry = panel_scrollbar_geometry(
                state.width, state.height, top, content->viewport_height_px,
                content->content_extent_px,
                content->scroll_offset_px, theme);
            if (!handled_scrollbar && geometry.visible &&
                point_in_rect(ix, iy, geometry.hit_x, geometry.hit_y, geometry.hit_width,
                              geometry.hit_height)) {
                handled_scrollbar = true;
                const bool in_thumb = static_cast<float>(ix) >= geometry.thumb_x &&
                                      static_cast<float>(ix) <
                                          geometry.thumb_x + geometry.thumb_width &&
                                      static_cast<float>(iy) >= geometry.thumb_y &&
                                      static_cast<float>(iy) <
                                          geometry.thumb_y + geometry.thumb_height;
                if (in_thumb) {
                    mode = 3;
                    scrollbar_pressed = true;
                    drag_start_y = iy;
                    drag_start_offset = content->scroll_offset_px;
                } else {
                    const int32_t previous_offset = content->scroll_offset_px;
                    const float thumb_center_y = geometry.thumb_y + geometry.thumb_height * 0.5F;
                    if (static_cast<float>(iy) < thumb_center_y) {
                        content->scroll_offset_px = std::max(
                            0, previous_offset - content->viewport_height_px);
                    } else {
                        content->scroll_offset_px = std::min(
                            geometry.range, previous_offset + content->viewport_height_px);
                    }
                    page_changed = content->scroll_offset_px != previous_offset;
                }
            }
        }
        if (mode == 0 && movable && iy < top && !close_armed && !handled_scrollbar)
            mode = 1;
        {
            std::lock_guard lock(panel->mutex);
            panel->resize_edges = edges;
            panel->interaction_mode = mode;
            panel->scrollbar_pressed = scrollbar_pressed;
            if (scrollbar_pressed) {
                panel->scrollbar_drag_start_y = drag_start_y;
                panel->scrollbar_drag_start_offset = drag_start_offset;
            }
        }
        if (mode == 0 && !close_armed && !handled_scrollbar) {
            sao_ui_widget_handle_t pressed = nullptr;
            const sao_status_t hit_status = widget_at(panel, ix, iy, &pressed);
            if (status == SAO_STATUS_OK && hit_status != SAO_STATUS_OK)
                status = hit_status;
            bool any_popup_open = false;
            if (sao_ui_widget_dropdown_any_open(&any_popup_open) == SAO_STATUS_OK &&
                any_popup_open) {
                bool pressed_owns_popup = false;
                if (pressed != nullptr)
                    (void)sao_ui_dropdown_button_popup_is_open(pressed, &pressed_owns_popup);
                if (!pressed_owns_popup) {
                    const sao_status_t close_status = sao_ui_widget_dropdown_close_popup_global();
                    if (status == SAO_STATUS_OK)
                        status = close_status;
                    page_changed = close_status == SAO_STATUS_OK;
                }
            }
            {
                std::lock_guard lock(panel->mutex);
                panel->pressed_widget = pressed;
            }
            if (pressed != nullptr) {
                bool focusable = false;
                if (sao_ui_widget_is_focusable(pressed, &focusable) == SAO_STATUS_OK && focusable) {
                    std::shared_ptr<PanelContent> focus_content;
                    sao_ui_widget_handle_t previous_focus = nullptr;
                    {
                        std::lock_guard focus_lock(panel->mutex);
                        focus_content = panel->content;
                        previous_focus = panel->focused_widget;
                    }
                    bool previous_focus_is_current = false;
                    std::string focus_id;
                    std::string focus_path;
                    if (focus_content != nullptr) {
                        std::lock_guard content_lock(focus_content->mutex);
                        previous_focus_is_current = previous_focus != nullptr &&
                            focus_content->by_handle.contains(previous_focus);
                        if (const auto found = focus_content->by_handle.find(pressed);
                            found != focus_content->by_handle.end() && found->second != nullptr) {
                            focus_id = found->second->id;
                            focus_path = found->second->path;
                        }
                    }
                    sao_status_t focus_status = SAO_STATUS_OK;
                    if (previous_focus_is_current && previous_focus != pressed)
                        focus_status = sao_ui_widget_set_focused(previous_focus, false);
                    if (focus_status == SAO_STATUS_OK)
                        focus_status = sao_ui_widget_set_focused(pressed, true);
                    if (focus_status != SAO_STATUS_OK && previous_focus_is_current &&
                        previous_focus != pressed) {
                        (void)sao_ui_widget_set_focused(previous_focus, true);
                    }
                    if (focus_status == SAO_STATUS_OK) {
                        std::lock_guard focus_lock(panel->mutex);
                        if (panel->content == focus_content) {
                            panel->focused_widget = pressed;
                            panel->responsive_focus_id = std::move(focus_id);
                            panel->responsive_focus_path = std::move(focus_path);
                        }
                    } else if (status == SAO_STATUS_OK) {
                        status = focus_status;
                    }
                }
                const sao_status_t pressed_status = sao_ui_widget_set_pressed(pressed, true);
                if (status == SAO_STATUS_OK && pressed_status != SAO_STATUS_OK)
                    status = pressed_status;
                bool is_text_input = false;
                bool is_slider = false;
                if (content != nullptr) {
                    std::lock_guard content_lock(content->mutex);
                    const auto found = content->by_handle.find(pressed);
                    is_text_input =
                        found != content->by_handle.end() && found->second->type == "input";
                    is_slider =
                        found != content->by_handle.end() && found->second->type == "slider";
                }
                if (is_slider) {
                    {
                        std::lock_guard panel_lock(panel->mutex);
                        panel->interaction_mode = 5;
                        panel->slider_drag_widget = pressed;
                    }
                    const sao_status_t slider_status =
                        update_slider_from_pointer(panel, pressed, ix, iy);
                    if (status == SAO_STATUS_OK)
                        status = slider_status;
                    page_changed = slider_status == SAO_STATUS_OK;
                } else if (is_text_input) {
                    const sao_status_t edit_status = begin_text_edit(panel, pressed);
                    if (status == SAO_STATUS_OK)
                        status = edit_status;
                } else {
                    end_panel_text_edit(panel, true);
                }
            } else {
                end_panel_text_edit(panel, true);
            }
        } else {
            bool any_popup_open = false;
            if (sao_ui_widget_dropdown_any_open(&any_popup_open) == SAO_STATUS_OK &&
                any_popup_open) {
                const sao_status_t close_status = sao_ui_widget_dropdown_close_popup_global();
                if (status == SAO_STATUS_OK)
                    status = close_status;
                page_changed = close_status == SAO_STATUS_OK;
            }
        }
        if (page_changed || scrollbar_pressed) {
            const sao_status_t upload_status = upload_panel(panel);
            if (status == SAO_STATUS_OK)
                status = upload_status;
        }
        return status;
    }
    if (action != 1)
        return SAO_STATUS_OK;
    sao_ui_widget_handle_t pressed = nullptr;
    bool close = false;
    int32_t mode = 0;
    {
        std::lock_guard lock(panel->mutex);
        pressed = panel->pressed_widget;
        panel->pressed_widget = nullptr;
        close = panel->close_armed && point_in_rect(ix, iy, state.width - top, 0, top, top);
        panel->close_armed = false;
        mode = panel->interaction_mode;
        panel->interaction_mode = 0;
        panel->slider_drag_widget = nullptr;
        panel->resize_edges = 0;
        panel->scrollbar_pressed = false;
    }
    if (mode == 4) {
        std::shared_ptr<PanelContent> content;
        {
            std::lock_guard lock(panel->mutex);
            content = panel->content;
        }
        if (content != nullptr) {
            std::lock_guard lock(content->mutex);
            (void)sao::ui::detail::layout_scrollbar_pointer(content->root, ix,
                                                            iy + content->scroll_offset_px, 1);
        }
    }
    if (pressed != nullptr) {
        const sao_status_t pressed_status = sao_ui_widget_set_pressed(pressed, false);
        if (status == SAO_STATUS_OK && pressed_status != SAO_STATUS_OK)
            status = pressed_status;
        sao_ui_widget_handle_t hit = nullptr;
        const sao_status_t hit_status = widget_at(panel, ix, iy, &hit);
        if (status == SAO_STATUS_OK && hit_status != SAO_STATUS_OK)
            status = hit_status;
        bool pressed_is_text_input = false;
        bool pressed_is_slider = false;
        bool pressed_is_dropdown = false;
        std::shared_ptr<PanelContent> current_content;
        {
            std::lock_guard lock(panel->mutex);
            current_content = panel->content;
        }
        if (current_content != nullptr) {
            std::lock_guard lock(current_content->mutex);
            const auto found = current_content->by_handle.find(pressed);
            pressed_is_text_input =
                found != current_content->by_handle.end() && found->second->type == "input";
            pressed_is_slider =
                found != current_content->by_handle.end() && found->second->type == "slider";
            pressed_is_dropdown =
                found != current_content->by_handle.end() && found->second->type == "dropdown";
        }
        if (pressed_is_slider && mode == 5) {
            (void)sao_ui_sound_play(SAO_UI_SOUND_CLICK, 40);
            const sao_status_t action_status = dispatch_owned_action(panel, pressed, true);
            if (status == SAO_STATUS_OK && action_status != SAO_STATUS_OK)
                status = action_status;
        } else if (pressed_is_dropdown && hit_status == SAO_STATUS_OK && pressed == hit) {
            sao_ui_widget_handle_t popup_owner = nullptr;
            int32_t popup_entry = -1;
            bool popup_consumed = false;
            (void)sao_ui_widget_dropdown_popup_hit_global(ix, iy, &popup_owner, &popup_entry,
                                                          &popup_consumed);
            bool popup_open = false;
            (void)sao_ui_dropdown_button_popup_is_open(pressed, &popup_open);
            bool valid_pick = false;
            if (current_content != nullptr) {
                std::lock_guard lock(current_content->mutex);
                const auto found = current_content->by_handle.find(pressed);
                if (found != current_content->by_handle.end()) {
                    valid_pick =
                        popup_owner == pressed && popup_entry >= 0 &&
                        static_cast<size_t>(popup_entry) < found->second->dropdown_items.size() &&
                        found->second->dropdown_items[static_cast<size_t>(popup_entry)].enabled;
                }
            }
            sao_status_t dropdown_status = SAO_STATUS_OK;
            if (popup_open)
                dropdown_status = sao_ui_dropdown_button_popup_click(pressed);
            else
                dropdown_status = sao_ui_dropdown_button_toggle_popup(pressed);
            if (status == SAO_STATUS_OK)
                status = dropdown_status;
            if (dropdown_status == SAO_STATUS_OK && popup_consumed && valid_pick) {
                (void)sao_ui_sound_play(SAO_UI_SOUND_CLICK, 50);
                const sao_status_t action_status = dispatch_owned_action(panel, pressed, true);
                if (status == SAO_STATUS_OK && action_status != SAO_STATUS_OK)
                    status = action_status;
            }
        } else if (hit_status == SAO_STATUS_OK && pressed == hit && !pressed_is_text_input) {
            (void)sao_ui_sound_play(SAO_UI_SOUND_CLICK, 50);
            const sao_status_t action_status = dispatch_action_at(panel, ix, iy);
            if (status == SAO_STATUS_OK && action_status != SAO_STATUS_OK)
                status = action_status;
        }
    }
    if (close && mode == 0) {
        (void)sao_ui_sound_play(SAO_UI_SOUND_ALERT_CLOSE, 70);
        const sao_status_t event_status = notify(panel, SAO_UI_PANEL_EVENT_CLOSE);
        if (status == SAO_STATUS_OK && event_status != SAO_STATUS_OK)
            status = event_status;
    }
    const sao_status_t upload_status = upload_panel(panel);
    return status == SAO_STATUS_OK ? upload_status : status;
}

void SAO_UI_CALL layer_button_callback(int32_t button, int32_t action, int32_t, float x, float y,
                                       void* user_data) {
    if (user_data == nullptr)
        return;
    sao_ui_sound_event_scope_t scope = 0;
    const sao_status_t begin_status = sao_ui_sound_event_begin(0, &scope);
    const sao_status_t status =
        panel_button(static_cast<sao_ui_panel_s*>(user_data), button, action,
                     saturating_float_to_i32(x), saturating_float_to_i32(y));
    if (begin_status == SAO_STATUS_OK) {
        if (status == SAO_STATUS_OK)
            (void)sao_ui_sound_event_commit(scope);
        else
            (void)sao_ui_sound_event_cancel(scope);
    }
}

sao_status_t notify(sao_ui_panel_s* panel, int32_t event) {
    PanelCallbackLease callback(panel, CallbackKind::Event);
    if (callback && callback.event() != nullptr) {
        try {
            callback.event()(event, callback.user_data());
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }
    return SAO_STATUS_OK;
}

void SAO_UI_CALL active_theme_changed(SaoUiThemeId, void* user_data) {
    auto* panel = static_cast<sao_ui_panel_s*>(user_data);
    if (panel == nullptr)
        return;
    ActiveCallback theme_marker{panel, CallbackKind::Theme, 0, active_callback};
    active_callback = &theme_marker;
    try {
        {
            PanelOperation operation(panel);
            if (!operation) {
                active_callback = theme_marker.previous;
                return;
            }
            {
                std::lock_guard lock(panel->mutex);
                panel->requested_theme_generation = std::max(
                    panel->requested_theme_generation,
                    sao::ui::detail::process_theme_generation());
                panel->theme_dirty = true;
            }
            try {
                (void)upload_panel(panel);
            } catch (...) {
            }
        }
    } catch (...) {
    }
    active_callback = theme_marker.previous;
    finalize_if_ready(panel);
}

} // namespace

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_input_hit_test_(sao_ui_panel_handle_t panel, int32_t x, int32_t y,
                             sao_ui_widget_handle_t* out_widget) {
    if (panel == nullptr || out_widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_widget = nullptr;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        return widget_at(panel, x, y, out_widget);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_input_activate_widget_(sao_ui_widget_handle_t widget) {
    if (widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::unique_lock storage_lock(panel_storage_mutex());
        for (const auto& stored_panel : panel_storage()) {
            sao_ui_panel_s* const panel = stored_panel.get();
            std::shared_ptr<PanelContent> content;
            {
                std::lock_guard panel_lock(panel->mutex);
                content = panel->content;
            }
            if (content == nullptr)
                continue;
            {
                std::lock_guard content_lock(content->mutex);
                if (!content->by_handle.contains(widget))
                    continue;
            }

            PanelOperation operation(panel);
            if (!operation) {
                storage_lock.unlock();
                return SAO_STATUS_ERR_HANDLE_INVALID;
            }
            storage_lock.unlock();

            std::string action;
            std::string args;
            {
                std::lock_guard panel_lock(panel->mutex);
                content = panel->content;
            }
            if (content == nullptr)
                return SAO_STATUS_ERR_NOT_FOUND;
            {
                std::lock_guard content_lock(content->mutex);
                const auto found = content->by_handle.find(widget);
                if (found == content->by_handle.end() || !found->second->enabled ||
                    found->second->action.empty()) {
                    return SAO_STATUS_ERR_NOT_FOUND;
                }
                if (found->second->type == "dropdown")
                    return SAO_STATUS_ERR_NOT_FOUND;
                action = found->second->action;
                args = found->second->action_args;
                if (found->second->type == "slider" || found->second->type == "dropdown") {
                    json payload = json::object();
                    try {
                        payload = json::parse(args);
                        if (!payload.is_object())
                            payload = json::object();
                    } catch (...) {
                        payload = json::object();
                    }
                    if (found->second->type == "slider") {
                        float value = found->second->slider_value;
                        (void)sao_ui_slider_get_value(found->second->handle, &value);
                        found->second->slider_value = value;
                        payload["value"] = value;
                        payload["text"] = value;
                    } else {
                        payload["selected_id"] = found->second->dropdown_selected_id;
                        payload["value"] = found->second->props.value("value", std::string());
                        payload["text"] = found->second->props.value("text", std::string());
                    }
                    args = payload.dump();
                }
            }

            PanelCallbackLease callback(panel, CallbackKind::Action);
            if (!callback || callback.action() == nullptr)
                return SAO_STATUS_OK;
            try {
                callback.action()(action.c_str(), reinterpret_cast<const uint8_t*>(args.data()),
                                  args.size(), callback.user_data());
            } catch (...) {
                return SAO_STATUS_ERR_UNKNOWN;
            }
            return SAO_STATUS_OK;
        }
        return SAO_STATUS_ERR_NOT_FOUND;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_input_dispatch_control_action_(sao_ui_widget_handle_t widget) {
    if (widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::unique_lock storage_lock(panel_storage_mutex());
    for (const auto& stored : panel_storage()) {
        std::shared_ptr<PanelContent> content;
        {
            std::lock_guard panel_lock(stored->mutex);
            content = stored->content;
        }
        if (content == nullptr)
            continue;
        {
            std::lock_guard content_lock(content->mutex);
            if (!content->by_handle.contains(widget))
                continue;
        }
        sao_ui_panel_s* panel = stored.get();
        PanelOperation operation(panel);
        if (!operation)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        storage_lock.unlock();
        return dispatch_owned_action(panel, widget, true);
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_input_current_focus_(
    sao_ui_compositor_handle_t compositor, sao_ui_widget_handle_t* out_widget) {
    if (out_widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_widget = nullptr;
    std::lock_guard storage_lock(panel_storage_mutex());
    for (const auto& stored : panel_storage()) {
        if (stored->compositor != compositor)
            continue;
        std::lock_guard panel_lock(stored->mutex);
        if (stored->state.visible && stored->focused_widget != nullptr) {
            *out_widget = stored->focused_widget;
            return SAO_STATUS_OK;
        }
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_input_focus_adjacent_(
    sao_ui_widget_handle_t current, bool reverse, sao_ui_widget_handle_t* out_widget) {
    if (current == nullptr || out_widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_widget = nullptr;
    std::lock_guard storage_lock(panel_storage_mutex());
    for (const auto& stored : panel_storage()) {
        std::shared_ptr<PanelContent> content;
        {
            std::lock_guard panel_lock(stored->mutex);
            content = stored->content;
        }
        if (content == nullptr)
            continue;
        std::lock_guard content_lock(content->mutex);
        const auto found = content->by_handle.find(current);
        if (found == content->by_handle.end())
            continue;
        const auto position =
            std::find_if(content->widgets.begin(), content->widgets.end(),
                         [current](const auto& candidate) { return candidate->handle == current; });
        if (position == content->widgets.end())
            return SAO_STATUS_ERR_NOT_FOUND;
        const size_t start = static_cast<size_t>(std::distance(content->widgets.begin(), position));
        for (size_t step = 1; step <= content->widgets.size(); ++step) {
            const size_t index =
                reverse ? (start + content->widgets.size() - step) % content->widgets.size()
                        : (start + step) % content->widgets.size();
            auto* candidate = content->widgets[index].get();
            bool focusable = false;
            if (candidate->enabled &&
                sao_ui_widget_is_focusable(candidate->handle, &focusable) == SAO_STATUS_OK &&
                focusable) {
                *out_widget = candidate->handle;
                return SAO_STATUS_OK;
            }
        }
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_input_accept_keyboard_focus_(sao_ui_widget_handle_t widget) {
    if (widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::unique_lock storage_lock(panel_storage_mutex());
    for (const auto& stored : panel_storage()) {
        std::shared_ptr<PanelContent> content;
        {
            std::lock_guard panel_lock(stored->mutex);
            content = stored->content;
        }
        if (content == nullptr)
            continue;
        std::string focus_id;
        std::string focus_path;
        bool is_input = false;
        {
            std::lock_guard content_lock(content->mutex);
            const auto found = content->by_handle.find(widget);
            if (found == content->by_handle.end())
                continue;
            focus_id = found->second->id;
            focus_path = found->second->path;
            is_input = found->second->type == "input";
        }
        sao_ui_panel_s* panel = stored.get();
        {
            std::lock_guard panel_lock(panel->mutex);
            panel->focused_widget = widget;
            panel->responsive_focus_id = std::move(focus_id);
            panel->responsive_focus_path = std::move(focus_path);
        }
        storage_lock.unlock();
        if (is_input)
            return begin_text_edit(panel, widget);
        end_panel_text_edit(panel, true);
        return SAO_STATUS_OK;
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_input_refresh_widget_(sao_ui_widget_handle_t widget) {
    if (widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::unique_lock storage_lock(panel_storage_mutex());
    for (const auto& stored : panel_storage()) {
        std::shared_ptr<PanelContent> content;
        {
            std::lock_guard panel_lock(stored->mutex);
            content = stored->content;
        }
        if (content == nullptr)
            continue;
        {
            std::lock_guard content_lock(content->mutex);
            if (!content->by_handle.contains(widget))
                continue;
        }
        sao_ui_panel_s* panel = stored.get();
        PanelOperation operation(panel);
        if (!operation)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        storage_lock.unlock();
        return upload_panel(panel);
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" bool SAO_UI_CALL
sao_ui_panel_input_widget_owner_(sao_ui_widget_handle_t widget,
                                 sao_ui_panel_handle_t* out_panel) {
    if (widget == nullptr || out_panel == nullptr)
        return false;
    *out_panel = nullptr;
    try {
        std::unique_lock storage_lock(panel_storage_mutex());
        for (const auto& stored_panel : panel_storage()) {
            sao_ui_panel_s* const panel = stored_panel.get();
            std::shared_ptr<PanelContent> content;
            {
                std::lock_guard panel_lock(panel->mutex);
                content = panel->content;
            }
            if (content == nullptr)
                continue;
            {
                std::lock_guard content_lock(content->mutex);
                if (!content->by_handle.contains(widget))
                    continue;
            }

            PanelOperation operation(panel);
            if (!operation) {
                storage_lock.unlock();
                return false;
            }
            storage_lock.unlock();
            {
                std::lock_guard panel_lock(panel->mutex);
                content = panel->content;
            }
            if (content == nullptr)
                return false;
            std::lock_guard content_lock(content->mutex);
            if (!content->by_handle.contains(widget))
                return false;
            *out_panel = panel;
            return true;
        }
    } catch (...) {
        return false;
    }
    sao::ui::detail::WidgetHandleMetadata metadata{};
    return sao::ui::detail::inspect_widget_handle(widget, &metadata);
}

extern "C" bool SAO_UI_CALL
sao_ui_panel_input_widget_is_visible_(sao_ui_widget_handle_t widget) {
    if (widget == nullptr)
        return false;
    try {
        std::unique_lock storage_lock(panel_storage_mutex());
        for (const auto& stored_panel : panel_storage()) {
            sao_ui_panel_s* const panel = stored_panel.get();
            std::shared_ptr<PanelContent> content;
            {
                std::lock_guard panel_lock(panel->mutex);
                content = panel->content;
            }
            if (content == nullptr)
                continue;
            {
                std::lock_guard content_lock(content->mutex);
                if (!content->by_handle.contains(widget))
                    continue;
            }

            PanelOperation operation(panel);
            if (!operation) {
                storage_lock.unlock();
                return false;
            }
            storage_lock.unlock();

            bool panel_visible = false;
            {
                std::lock_guard panel_lock(panel->mutex);
                panel_visible = panel->state.visible;
                content = panel->content;
            }
            if (!panel_visible || content == nullptr)
                return false;
            std::lock_guard content_lock(content->mutex);
            const auto found = content->by_handle.find(widget);
            return found != content->by_handle.end();
        }
    } catch (...) {
        return false;
    }
    sao::ui::detail::WidgetHandleMetadata metadata{};
    return sao::ui::detail::inspect_widget_handle(widget, &metadata);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_create(sao_ui_compositor_handle_t compositor,
                                                        const SaoPanelConfig* config,
                                                        sao_ui_panel_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (config == nullptr || config->panel_id_utf8 == nullptr || config->panel_id_utf8[0] == '\0' ||
        config->default_width <= 0 || config->default_height <= 0 || config->min_width < 0 ||
        config->min_height < 0 || config->max_width < 0 || config->max_height < 0 ||
        (config->max_width > 0 && config->max_width < config->min_width) ||
        (config->max_height > 0 && config->max_height < config->min_height))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        sao_ui_panel_handle_t existing = nullptr;
        const sao_status_t find_status =
            sao_ui_panel_runtime_find_(compositor, config->panel_id_utf8, &existing);
        if (find_status == SAO_STATUS_OK) {
            if (config->single_instance) {
                *out_handle = existing;
                return SAO_STATUS_OK;
            }
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        if (find_status != SAO_STATUS_ERR_NOT_FOUND)
            return find_status;

        auto panel = std::make_unique<sao_ui_panel_s>();
        panel->compositor = compositor;
        panel->id = config->panel_id_utf8;
        panel->title = config->title_utf8 == nullptr ? "" : config->title_utf8;
        panel->theme_page = config->theme_page_utf8 == nullptr ? "" : config->theme_page_utf8;
        panel->show_titlebar = config->show_titlebar;
        panel->show_close_button = config->show_close_button;
        panel->movable = config->movable;
        panel->resizable = config->resizable;
        panel->min_width = config->min_width;
        panel->min_height = config->min_height;
        panel->max_width = config->max_width;
        panel->max_height = config->max_height;
        panel->state.x = config->default_x;
        panel->state.y = config->default_y;
        panel->state.width =
            clamp_dimension(config->default_width, config->min_width, config->max_width);
        panel->state.height =
            clamp_dimension(config->default_height, config->min_height, config->max_height);
        const auto initial_theme = resolve_panel_theme(panel.get());
        panel->requested_theme_generation = initial_theme.generation;
        const json empty_spec{
            {"version", SAO_UI_SPEC_VERSION}, {"title", ""}, {"nodes", json::array()}};
        sao_status_t status = build_content(empty_spec, panel->state.width, panel->state.height,
                                            content_top(*panel, initial_theme), initial_theme,
                                            &panel->content);
        if (status != SAO_STATUS_OK)
            return status;

        if (compositor != nullptr) {
            const std::string layer_name = "panel." + panel->id;
            SaoLayerConfig layer_config{};
            layer_config.struct_size = sizeof(SaoLayerConfig);
            layer_config.name_utf8 = layer_name.c_str();
            layer_config.x = panel->state.x;
            layer_config.y = panel->state.y;
            layer_config.width = panel->state.width;
            layer_config.height = panel->state.height;
            layer_config.click_through = false;
            layer_config.rect_hit = true;
            layer_config.bgra_swizzle = true;
            status = sao_ui_layer_create(compositor, &layer_config, &panel->layer);
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_set_input_callbacks(
                    panel->layer, &layer_cursor_pos_callback, &layer_cursor_leave_callback,
                    &layer_button_callback, &layer_scroll_callback, panel.get());
            }
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_set_visible(panel->layer, false);
            }
            if (status != SAO_STATUS_OK)
                return status;
        }
        status = sao_ui_theme_register_change_callback(
            &active_theme_changed, panel.get(), &panel->theme_callback_handle);
        if (status != SAO_STATUS_OK)
            return status;
        sao_ui_panel_s* const raw = panel.get();
        {
            std::lock_guard storage_lock(panel_storage_mutex());
            panel_storage().reserve(panel_storage().size() + 1U);
            panel_storage().push_back(std::move(panel));
        }
        sao_ui_panel_handle_t published = nullptr;
        status = sao_ui_panel_runtime_publish_(raw, compositor, config->panel_id_utf8,
                                               config->single_instance, &published);
        if (status != SAO_STATUS_OK || published != raw) {
            erase_panel_storage(raw);
            if (status == SAO_STATUS_OK)
                *out_handle = published;
            return status;
        }

        const int32_t create_theme_switch =
            g_create_theme_switch.exchange(-1, std::memory_order_acq_rel);
        if (create_theme_switch >= 0 && create_theme_switch < SAO_UI_THEME_COUNT) {
            status = sao_ui_theme_set_active_id(
                static_cast<SaoUiThemeId>(create_theme_switch));
        }
        if (status == SAO_STATUS_OK)
            status = upload_panel(raw);
        if (status != SAO_STATUS_OK) {
            sao_ui_panel_runtime_destroy_(raw);
            erase_panel_storage(raw);
            return status;
        }
        *out_handle = raw;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_panel_destroy(sao_ui_panel_handle_t panel) {
    sao_ui_panel_destroy_through_sdk_(panel);
}

extern "C" void SAO_UI_CALL sao_ui_panel_runtime_destroy_(sao_ui_panel_handle_t panel) {
    if (panel == nullptr)
        return;
    try {
        end_panel_text_edit(panel, true);
        {
            if (!sao_ui_panel_runtime_retire_(panel))
                return;
            std::lock_guard lifecycle_lock(panel->lifecycle_mutex);
            panel->accepting_operations = false;
            panel->destroy_requested = true;
        }
        {
            std::lock_guard lock(panel->mutex);
            panel->stopping = true;
        }
        finalize_if_ready(panel);
        bool self_callback = false;
        for (const ActiveCallback* active = active_callback; active != nullptr;
             active = active->previous) {
            if (active->panel == panel) {
                self_callback = true;
                break;
            }
        }
        if (self_callback)
            return;
        std::unique_lock lock(panel->lifecycle_mutex);
        panel->lifecycle_cv.wait(lock, [panel] { return panel->finalized; });
    } catch (...) {
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_spec(sao_ui_panel_handle_t panel,
                                                          const uint8_t* spec_json_utf8,
                                                          size_t spec_len) {
    if (panel == nullptr || (spec_json_utf8 == nullptr && spec_len != 0U) ||
        spec_len > kMaximumPanelSpecBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard render_lock(panel->render_mutex);
        json normalized;
        std::string serialized;
        sao_status_t status = normalize_spec(spec_json_utf8, spec_len, &normalized, &serialized);
        if (status != SAO_STATUS_OK)
            return status;
        const auto theme = resolve_panel_theme(panel);
        SaoPanelState state{};
        int32_t top = 0;
        {
            std::scoped_lock lock(panel->mutex);
            state = panel->state;
            top = content_top(*panel, theme);
        }
        std::shared_ptr<PanelContent> replacement;
        status = build_content(normalized, state.width, state.height, top, theme, &replacement);
        if (status != SAO_STATUS_OK)
            return status;
        std::shared_ptr<PanelContent> previous;
        std::string previous_spec;
        std::array<sao_ui_widget_handle_t, 3> previous_interaction{};
        std::array<sao_ui_widget_handle_t, 3> next_interaction{};
        std::string previous_focus_id;
        std::string previous_focus_path;
        std::string next_focus_id;
        std::string next_focus_path;
        std::string active_edit_id;
        sao_ui_widget_handle_t active_edit_handle = nullptr;
        OwnedWidget* reused_old_widget = nullptr;
        OwnedWidget* reused_next_widget = nullptr;
        {
            std::lock_guard anchor_lock(text_edit_anchor_mutex());
            if (text_edit_anchor().panel == panel) {
                active_edit_id = text_edit_anchor().widget_id;
                active_edit_handle = text_edit_anchor().widget;
            }
        }
        {
            std::scoped_lock lock(panel->mutex);
            previous = panel->content;
            previous_spec = panel->spec_json;
            previous_interaction = {panel->hovered_widget, panel->pressed_widget,
                                    panel->focused_widget};
            previous_focus_id = panel->responsive_focus_id;
            previous_focus_path = panel->responsive_focus_path;
        }
        if (previous != nullptr) {
            std::scoped_lock lock(previous->mutex, replacement->mutex);
            sao::ui::detail::layout_restore_viewports(previous->root, replacement->root);
            if (!active_edit_id.empty()) {
                const auto old_edit = previous->by_id.find(active_edit_id);
                const auto next_edit = replacement->by_id.find(active_edit_id);
                if (old_edit != previous->by_id.end() && next_edit != replacement->by_id.end() &&
                    old_edit->second->handle == active_edit_handle &&
                    old_edit->second->type == "input" && next_edit->second->type == "input") {
                    reused_old_widget = old_edit->second;
                    reused_next_widget = next_edit->second;
                    replacement->by_handle.erase(reused_next_widget->handle);
                    sao_ui_widget_destroy(reused_next_widget->handle);
                    reused_old_widget->owns_handle = false;
                    reused_next_widget->handle = reused_old_widget->handle;
                    reused_next_widget->owns_handle = true;
                    reused_next_widget->props["value"] =
                        reused_old_widget->props.value("value", std::string());
                    reused_next_widget->props["text"] =
                        reused_old_widget->props.value("text", std::string());
                    if (!sao::ui::detail::layout_replace_widget(reused_next_widget->node,
                                                                reused_next_widget->handle)) {
                        reused_old_widget->owns_handle = true;
                        reused_next_widget->owns_handle = false;
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                    const std::string current_props = reused_next_widget->props.dump();
                    const sao_status_t props_status = sao_ui_widget_apply_props(
                        reused_next_widget->handle,
                        reinterpret_cast<const uint8_t*>(current_props.data()),
                        current_props.size());
                    if (props_status != SAO_STATUS_OK) {
                        reused_old_widget->owns_handle = true;
                        reused_next_widget->owns_handle = false;
                        return props_status;
                    }
                    replacement->by_handle.emplace(reused_next_widget->handle, reused_next_widget);
                }
            }
            replacement->scroll_offset_px = std::clamp(
                previous->scroll_offset_px, 0,
                std::max(0, replacement->content_extent_px - replacement->viewport_height_px));
            for (size_t index = 0; index < previous_interaction.size(); ++index) {
                const auto old = previous->by_handle.find(previous_interaction[index]);
                if (old == previous->by_handle.end() || old->second->id.empty())
                    continue;
                const auto next = replacement->by_id.find(old->second->id);
                if (next == replacement->by_id.end() || !next->second->enabled ||
                    next->second->type != old->second->type ||
                    next->second->action != old->second->action)
                    continue;
                if (index == 1U && next->second->action_args != old->second->action_args)
                    continue;
                const auto handle = next->second->handle;
                status = index == 0U   ? sao_ui_widget_set_hovered(handle, true)
                         : index == 1U ? sao_ui_widget_set_pressed(handle, true)
                                       : sao_ui_widget_set_focused(handle, true);
                if (status == SAO_STATUS_ERR_NOT_IMPLEMENTED && index == 2U)
                    continue;
                if (status != SAO_STATUS_OK)
                    return status;
                next_interaction[index] = handle;
                if (index == 2U) {
                    next_focus_id = next->second->id;
                    next_focus_path = next->second->path;
                }
            }
        }
        {
            std::scoped_lock lock(panel->mutex);
            panel->content = replacement;
            panel->spec_json = std::move(serialized);
            panel->hovered_widget = next_interaction[0];
            panel->pressed_widget = next_interaction[1];
            panel->focused_widget = next_interaction[2];
            panel->responsive_focus_id = std::move(next_focus_id);
            panel->responsive_focus_path = std::move(next_focus_path);
        }
        status = upload_panel(panel);
        if (status != SAO_STATUS_OK) {
            std::scoped_lock lock(panel->mutex);
            if (reused_old_widget != nullptr && reused_next_widget != nullptr) {
                reused_old_widget->owns_handle = true;
                reused_next_widget->owns_handle = false;
            }
            panel->content = std::move(previous);
            panel->spec_json = std::move(previous_spec);
            panel->hovered_widget = previous_interaction[0];
            panel->pressed_widget = previous_interaction[1];
            panel->focused_widget = previous_interaction[2];
            panel->responsive_focus_id = std::move(previous_focus_id);
            panel->responsive_focus_path = std::move(previous_focus_path);
        } else if (reused_next_widget != nullptr) {
            status = begin_text_edit(panel, reused_next_widget->handle);
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_update_widget(sao_ui_panel_handle_t panel,
                                                               const char* widget_id_utf8,
                                                               const uint8_t* props_json_utf8,
                                                               size_t props_len) {
    if (panel == nullptr || widget_id_utf8 == nullptr || widget_id_utf8[0] == '\0' ||
        (props_json_utf8 == nullptr && props_len != 0U)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard render_lock(panel->render_mutex);
        const auto theme = resolve_panel_theme(panel);
        const sao::ui::detail::ScopedPanelPaintTheme theme_scope(theme);
        const json patch = props_len == 0
                               ? json::object()
                               : json::parse(props_json_utf8, props_json_utf8 + props_len);
        if (!patch.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::shared_ptr<PanelContent> content;
        std::string previous_spec;
        std::string candidate_spec;
        {
            std::scoped_lock lock(panel->mutex);
            content = panel->content;
            previous_spec = panel->spec_json;
        }
        if (content == nullptr)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        json candidate_document;
        bool spec_updated = false;
        if (!previous_spec.empty()) {
            try {
                candidate_document = json::parse(previous_spec);
                spec_updated = merge_widget_spec_by_id(candidate_document, widget_id_utf8, patch);
                if (spec_updated)
                    candidate_spec = candidate_document.dump();
            } catch (...) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
        }
        OwnedWidget* widget = nullptr;
        json previous_props;
        std::string previous_action;
        std::string previous_args;
        bool previous_enabled = true;
        {
            std::scoped_lock lock(content->mutex);
            const auto found = content->by_id.find(widget_id_utf8);
            if (found == content->by_id.end())
                return SAO_STATUS_ERR_NOT_FOUND;
            widget = found->second;
            previous_props = widget->props;
            previous_action = widget->action;
            previous_args = widget->action_args;
            previous_enabled = widget->enabled;
            for (auto it = patch.begin(); it != patch.end(); ++it) {
                widget->props[it.key()] = it.value();
            }
            if (widget->kind == SAO_UI_WIDGET_ACTION_BUTTON && patch.contains("label") &&
                patch["label"].is_string()) {
                widget->props["text"] = patch["label"];
            }
            if (widget->kind == SAO_UI_WIDGET_BAR && patch.contains("pct") &&
                patch["pct"].is_number()) {
                widget->props["value"] = patch["pct"];
            }
            if (patch.contains("action") && patch["action"].is_string()) {
                widget->action = patch["action"].get<std::string>();
            }
            if (patch.contains("payload"))
                widget->action_args = patch["payload"].dump();
            if (patch.contains("disabled")) {
                widget->enabled = !patch.value("disabled", false);
                widget->props["enabled"] = widget->enabled;
            }
            sao_status_t status = apply_owned_widget_props(widget);
            if (status == SAO_STATUS_OK)
                status = sao_ui_widget_set_enabled(widget->handle, widget->enabled);
            if (status != SAO_STATUS_OK) {
                widget->props = std::move(previous_props);
                widget->action = std::move(previous_action);
                widget->action_args = std::move(previous_args);
                widget->enabled = previous_enabled;
                return status;
            }
            (void)sao_ui_layout_node_invalidate(widget->node);
        }
        const sao_status_t status = upload_panel(panel);
        if (status == SAO_STATUS_OK && spec_updated) {
            std::scoped_lock lock(panel->mutex);
            panel->spec_json = candidate_spec;
        }
        if (status != SAO_STATUS_OK) {
            std::scoped_lock lock(content->mutex);
            widget->props = std::move(previous_props);
            widget->action = std::move(previous_action);
            widget->action_args = std::move(previous_args);
            widget->enabled = previous_enabled;
            (void)apply_owned_widget_props(widget);
            (void)sao_ui_widget_set_enabled(widget->handle, widget->enabled);
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_replace_body_model(
    sao_ui_panel_handle_t panel, const SaoUiPanelBodyModelNode* nodes, size_t node_count,
    sao_ui_layout_node_handle_t* out_layout_nodes, size_t out_layout_node_capacity,
    sao_ui_layout_tree_handle_t* out_tree) {
    if (out_tree != nullptr)
        *out_tree = nullptr;
    if (panel == nullptr || nodes == nullptr || node_count == 0 ||
        (out_layout_nodes != nullptr && out_layout_node_capacity < node_count)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        PanelRenderGuard render_guard(panel);
        if (!render_guard)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;

        SaoPanelState state{};
        bool show_titlebar = false;
        std::string title;
        sao_ui_layer_handle_t layer = nullptr;
        {
            std::lock_guard lock(panel->mutex);
            state = panel->state;
            show_titlebar = panel->show_titlebar;
            title = panel->title;
            layer = panel->layer;
        }

        auto theme = resolve_panel_theme(panel);
        const sao::ui::detail::ScopedPanelPaintTheme theme_scope(theme);
        std::shared_ptr<PanelContent> replacement;
        std::vector<sao_ui_layout_node_handle_t> actual_nodes;
        sao_status_t status =
            build_body_content(nodes, node_count, state.width, state.height,
                               show_titlebar ? titlebar_height(theme) : 0, theme, &replacement,
                               &actual_nodes);
        if (status != SAO_STATUS_OK)
            return status;
        const bool body_uses_theme_layout_metrics = replacement->uses_theme_layout_metrics;

        std::vector<size_t> applied_props;
        applied_props.reserve(node_count);
        struct PropsRollback {
            const SaoUiPanelBodyModelNode* nodes{};
            const std::vector<size_t>* applied{};
            bool armed{true};

            sao_status_t run() {
                if (!armed)
                    return SAO_STATUS_OK;
                armed = false;
                sao_status_t first_failure = SAO_STATUS_OK;
                for (auto it = applied->rbegin(); it != applied->rend(); ++it) {
                    const SaoUiPanelBodyModelNode& node = nodes[*it];
                    sao_status_t rollback_status = SAO_STATUS_OK;
                    if (g_body_replace_failure_point.load(std::memory_order_acquire) == 2) {
                        rollback_status = SAO_STATUS_ERR_UNKNOWN;
                    } else {
                        rollback_status = sao_ui_widget_apply_props(
                            node.widget, node.rollback_props_json_utf8,
                            node.rollback_props_len);
                    }
                    if (first_failure == SAO_STATUS_OK && rollback_status != SAO_STATUS_OK)
                        first_failure = rollback_status;
                }
                return first_failure;
            }

            void dismiss() noexcept {
                armed = false;
            }
        } rollback{nodes, &applied_props};
        const auto fail_with_rollback = [&](sao_status_t failure) {
            return rollback.run() == SAO_STATUS_OK ? failure
                                                   : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
        };
        for (size_t index = 0; index < node_count; ++index) {
            const SaoUiPanelBodyModelNode& node = nodes[index];
            if (node.widget == nullptr || node.props_json_utf8 == nullptr)
                continue;
            status = sao_ui_widget_apply_props(node.widget, node.props_json_utf8, node.props_len);
            if (status != SAO_STATUS_OK)
                return fail_with_rollback(status);
            applied_props.push_back(index);
        }

        if (g_body_replace_failure_point.load(std::memory_order_acquire) != 0)
            return fail_with_rollback(SAO_STATUS_ERR_UNKNOWN);

        PanelFrame frame;
        constexpr int32_t kMaximumThemePasses = 8;
        bool rendered_latest = false;
        for (int32_t pass = 0; pass < kMaximumThemePasses; ++pass) {
            frame = {};
            status = render_frame(replacement, state, show_titlebar, title, panel->show_close_button,
                                  theme, panel, &frame);
            if (status != SAO_STATUS_OK)
                return fail_with_rollback(status);
            const uint64_t latest_generation = sao::ui::detail::process_theme_generation();
            if (latest_generation <= theme.generation) {
                rendered_latest = true;
                break;
            }
            theme = resolve_panel_theme(panel);
            status = build_body_content(nodes, node_count, state.width, state.height,
                                        show_titlebar ? titlebar_height(theme) : 0, theme,
                                        &replacement, &actual_nodes);
            if (status != SAO_STATUS_OK)
                return fail_with_rollback(status);
            if (body_uses_theme_layout_metrics && !replacement->uses_theme_layout_metrics) {
                const int32_t body_padding = theme.metrics[SAO_UI_METRIC_PADDING_M];
                replacement->uses_theme_layout_metrics = true;
                replacement->root_spec.pad_top_px = body_padding;
                replacement->root_spec.pad_right_px = body_padding;
                replacement->root_spec.pad_bottom_px = body_padding;
                replacement->root_spec.pad_left_px = body_padding;
                replacement->root_spec.gap_px = theme.metrics[SAO_UI_METRIC_GAP_S];
                status = arrange_content(*replacement, state.width, state.height,
                                         show_titlebar ? titlebar_height(theme) : 0);
                if (status != SAO_STATUS_OK)
                    return fail_with_rollback(status);
            }
        }
        if (!rendered_latest)
            return fail_with_rollback(SAO_UI_PANEL_STATUS_ERR_BUSY);
        if (layer != nullptr) {
            status = sao_ui_layer_update_bgra(layer, frame.pixels.data(), frame.width, frame.height,
                                              frame.stride);
            if (status != SAO_STATUS_OK)
                return fail_with_rollback(status);
        }
        const sao_ui_layout_tree_handle_t committed_tree = replacement->tree;
        {
            std::lock_guard lock(panel->mutex);
            panel->content = std::move(replacement);
            panel->spec_json.clear();
            panel->requested_theme_generation = std::max(
                panel->requested_theme_generation,
                sao::ui::detail::process_theme_generation());
            panel->uploaded_theme_generation = theme.generation;
            panel->theme_dirty =
                panel->uploaded_theme_generation < panel->requested_theme_generation;
        }
        rollback.dismiss();
        if (out_tree != nullptr)
            *out_tree = committed_tree;
        if (out_layout_nodes != nullptr) {
            std::copy(actual_nodes.begin(), actual_nodes.end(), out_layout_nodes);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_panel_test_set_body_replace_failure_point(int32_t point) {
    g_body_replace_failure_point.store(point, std::memory_order_release);
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_panel_test_set_theme_upload_failure_count(int32_t count) {
    g_theme_upload_failure_count.store(std::max(0, count), std::memory_order_release);
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_panel_test_set_create_theme_switch(int32_t theme_id) {
    g_create_theme_switch.store(theme_id, std::memory_order_release);
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_panel_test_set_metric_override(int32_t metric, int32_t value) {
    if (metric < 0 || metric >= SAO_UI_METRIC_TOKEN_COUNT)
        return;
    sao::ui::detail::g_panel_metric_test_overrides[static_cast<size_t>(metric)].store(
        std::max(0, value), std::memory_order_release);
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_test_get_theme_state(
    sao_ui_panel_handle_t panel, uint64_t* requested_generation,
    uint64_t* uploaded_generation, bool* dirty) {
    if (panel == nullptr || requested_generation == nullptr || uploaded_generation == nullptr ||
        dirty == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard lock(panel->mutex);
    *requested_generation = panel->requested_theme_generation;
    *uploaded_generation = panel->uploaded_theme_generation;
    *dirty = panel->theme_dirty;
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_test_apply_theme_override(
    sao_ui_panel_handle_t panel, const uint8_t* override_json_utf8, size_t override_len) {
    return sao_ui_panel_apply_theme_override_(panel, override_json_utf8, override_len);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_visible(sao_ui_panel_handle_t panel,
                                                             bool visible) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        if (!visible)
            end_panel_text_edit(panel, true);
        bool changed = false;
        {
            std::scoped_lock lock(panel->mutex);
            changed = panel->state.visible != visible;
        }
        if (panel->layer != nullptr) {
            sao_status_t status = sao_ui_layer_set_visible(panel->layer, visible);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_input_enabled(panel->layer, visible);
            if (status != SAO_STATUS_OK) {
                (void)sao_ui_layer_set_visible(panel->layer, !visible);
                return status;
            }
        }
        {
            std::scoped_lock lock(panel->mutex);
            panel->state.visible = visible;
        }
        if (changed && visible)
            (void)sao_ui_sound_play(SAO_UI_SOUND_PANEL, 70);
        if (changed)
            notify(panel, visible ? SAO_UI_PANEL_EVENT_SHOW : SAO_UI_PANEL_EVENT_HIDE);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_position(sao_ui_panel_handle_t panel,
                                                              int32_t x, int32_t y) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        if (panel->layer != nullptr) {
            const sao_status_t status = sao_ui_layer_set_position(panel->layer, x, y);
            if (status != SAO_STATUS_OK)
                return status;
        }
        {
            std::scoped_lock lock(panel->mutex);
            panel->state.x = x;
            panel->state.y = y;
        }
        return notify(panel, SAO_UI_PANEL_EVENT_MOVE);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_geometry(sao_ui_panel_handle_t panel,
                                                              int32_t x, int32_t y, int32_t width,
                                                              int32_t height) {
    if (panel == nullptr || width <= 0 || height <= 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard render_lock(panel->render_mutex);
        width = clamp_dimension(width, panel->min_width, panel->max_width);
        height = clamp_dimension(height, panel->min_height, panel->max_height);
        const auto theme = resolve_panel_theme(panel);
        SaoPanelState previous{};
        std::shared_ptr<PanelContent> previous_content;
        {
            std::scoped_lock lock(panel->mutex);
            previous = panel->state;
            panel->state.x = x;
            panel->state.y = y;
            panel->state.width = width;
            panel->state.height = height;
            previous_content = panel->content;
        }
        sao_status_t status = upload_panel(panel);
        if (status == SAO_STATUS_OK && panel->layer != nullptr)
            status = sao_ui_layer_set_position(panel->layer, x, y);
        if (status != SAO_STATUS_OK) {
            {
                std::scoped_lock lock(panel->mutex);
                panel->state = previous;
            }
            if (previous_content != nullptr) {
                std::scoped_lock lock(previous_content->mutex);
                (void)arrange_content(*previous_content, previous.width, previous.height,
                                      content_top(*panel, theme));
            }
            (void)upload_panel(panel);
            return status;
        }
        return notify(panel, SAO_UI_PANEL_EVENT_RESIZE);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_ui_layer_handle_t SAO_UI_CALL sao_ui_panel_layer(sao_ui_panel_handle_t panel) {
    PanelOperation operation(panel);
    if (!operation)
        return nullptr;
    try {
        std::lock_guard lock(panel->mutex);
        return panel->layer;
    } catch (...) {
        return nullptr;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_get_layout_tree(sao_ui_panel_handle_t panel, sao_ui_layout_tree_handle_t* out_tree,
                             sao_ui_layout_node_handle_t* out_root) {
    if (panel == nullptr || out_tree == nullptr || out_root == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::scoped_lock lock(panel->mutex);
        if (panel->content == nullptr)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        *out_tree = panel->content->tree;
        *out_root = panel->content->root;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_rasterize(sao_ui_panel_handle_t panel, sao_ui_offscreen_raster_handle_t raster) {
    if (panel == nullptr || raster == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        PanelRenderGuard render_guard(panel);
        if (!render_guard)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        SaoPanelState state{};
        bool show_titlebar = false;
        std::string title;
        std::shared_ptr<PanelContent> content;
        std::shared_ptr<PanelContent> previous_content;
        bool responsive_rebuilt = false;
        auto theme = resolve_panel_theme(panel);
        {
            std::lock_guard lock(panel->mutex);
            previous_content = panel->content;
        }
        const sao_status_t layout_status = prepare_panel_theme_layout(
            panel, theme, &content, &responsive_rebuilt);
        if (layout_status != SAO_STATUS_OK)
            return layout_status;
        {
            std::scoped_lock lock(panel->mutex);
            state = panel->state;
            show_titlebar = panel->show_titlebar;
            title = panel->title;
        }
        const sao_status_t status = paint_panel(
            content, state, show_titlebar, title, panel->show_close_button, theme, panel, raster);
        if (status != SAO_STATUS_OK || !responsive_rebuilt)
            return status;
        std::lock_guard lock(panel->mutex);
        if (panel->content != previous_content)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        panel->content = std::move(content);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_apply_theme_override_(
    sao_ui_panel_handle_t panel, const uint8_t* override_json_utf8, size_t override_len) {
    if (panel == nullptr || (override_json_utf8 == nullptr && override_len != 0U))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    PanelThemeOverrides replacement{};
    const sao_status_t parse_status =
        parse_theme_overrides(override_json_utf8, override_len, &replacement);
    if (parse_status != SAO_STATUS_OK)
        return parse_status;
    try {
        std::lock_guard render_lock(panel->render_mutex);
        PanelThemeOverrides previous{};
        {
            std::lock_guard lock(panel->mutex);
            previous = panel->theme_overrides;
            panel->theme_overrides = replacement;
        }
        const sao_status_t status = upload_panel(panel);
        if (status == SAO_STATUS_OK)
            return SAO_STATUS_OK;
        {
            std::lock_guard lock(panel->mutex);
            panel->theme_overrides = previous;
        }
        const sao_status_t rollback_status = upload_panel(panel);
        if (rollback_status != SAO_STATUS_OK) {
            std::lock_guard lock(panel->mutex);
            panel->theme_dirty = true;
            return SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_action_handler(
    sao_ui_panel_handle_t panel, sao_ui_panel_action_callback_t callback, void* user_data) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        uint64_t previous_generation = 0;
        {
            std::lock_guard lifecycle_lock(panel->lifecycle_mutex);
            previous_generation =
                panel->callback_generations[static_cast<size_t>(CallbackKind::Action)]++;
            std::lock_guard state_lock(panel->mutex);
            panel->action_callback = callback;
            panel->action_user_data = user_data;
        }
        wait_for_callback_generation(panel, CallbackKind::Action, previous_generation);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_event_handler(
    sao_ui_panel_handle_t panel, sao_ui_panel_event_callback_t callback, void* user_data) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        uint64_t previous_generation = 0;
        {
            std::lock_guard lifecycle_lock(panel->lifecycle_mutex);
            previous_generation =
                panel->callback_generations[static_cast<size_t>(CallbackKind::Event)]++;
            std::lock_guard state_lock(panel->mutex);
            panel->event_callback = callback;
            panel->event_user_data = user_data;
        }
        wait_for_callback_generation(panel, CallbackKind::Event, previous_generation);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_render_fn(sao_ui_panel_handle_t panel,
                                                               sao_ui_panel_render_fn_t callback,
                                                               void* user_data) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard render_lock(panel->render_mutex);
        sao_ui_panel_render_fn_t previous_callback = nullptr;
        void* previous_user_data = nullptr;
        uint64_t previous_generation = 0;
        {
            std::lock_guard lifecycle_lock(panel->lifecycle_mutex);
            previous_generation =
                panel->callback_generations[static_cast<size_t>(CallbackKind::Render)]++;
            std::lock_guard lock(panel->mutex);
            previous_callback = panel->render_callback;
            previous_user_data = panel->render_user_data;
            panel->render_callback = callback;
            panel->render_user_data = user_data;
        }
        wait_for_callback_generation(panel, CallbackKind::Render, previous_generation);
        const sao_status_t status = upload_panel(panel);
        if (status != SAO_STATUS_OK) {
            std::scoped_lock lock(panel->mutex);
            panel->render_callback = previous_callback;
            panel->render_user_data = previous_user_data;
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_get_state(sao_ui_panel_handle_t panel,
                                                           SaoPanelState* out_state) {
    if (panel == nullptr || out_state == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::scoped_lock lock(panel->mutex);
        *out_state = panel->state;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_find_by_id(sao_ui_compositor_handle_t compositor,
                                                            const char* panel_id_utf8,
                                                            sao_ui_panel_handle_t* out_handle) {
    return sao_ui_panel_runtime_find_(compositor, panel_id_utf8, out_handle);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_enumerate(sao_ui_compositor_handle_t compositor,
                                                           sao_ui_panel_handle_t* out_handles,
                                                           size_t capacity, size_t* out_written) {
    return sao_ui_panel_runtime_enumerate_(compositor, out_handles, capacity, out_written);
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_test_pointer_move(sao_ui_panel_handle_t panel, int32_t x, int32_t y) {
    return panel_cursor_pos(panel, x, y);
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_test_pointer_leave(sao_ui_panel_handle_t panel) {
    return panel_cursor_leave(panel);
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_test_pointer_button_state(sao_ui_panel_handle_t panel, int32_t button, int32_t action,
                                       int32_t x, int32_t y) {
    return panel_button(panel, button, action, x, y);
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_test_scroll(sao_ui_panel_handle_t panel, float dy) {
    return panel_scroll(panel, 0.0F, dy);
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_test_get_scroll_state(sao_ui_panel_handle_t panel, int32_t* out_offset,
                                    int32_t* out_extent, int32_t* out_viewport) {
    if (panel == nullptr || out_offset == nullptr || out_extent == nullptr || out_viewport == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (not operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::shared_ptr<PanelContent> content;
    {
        std::lock_guard lock(panel->mutex);
        content = panel->content;
    }
    if (content == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    std::lock_guard lock(content->mutex);
    *out_offset = content->scroll_offset_px;
    *out_extent = content->content_extent_px;
    *out_viewport = content->viewport_height_px;
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_test_get_scrollbar_geometry(
    sao_ui_panel_handle_t panel, PanelScrollbarGeometryTestSnapshot* out_geometry) {
    if (panel == nullptr || out_geometry == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (not operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    SaoPanelState state{};
    bool show_titlebar = false;
    std::shared_ptr<PanelContent> content;
    {
        std::lock_guard lock(panel->mutex);
        state = panel->state;
        show_titlebar = panel->show_titlebar;
        content = panel->content;
    }
    if (content == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    const auto theme = resolve_panel_theme(panel);
    const int32_t top = show_titlebar ? titlebar_height(theme) : 0;
    std::lock_guard lock(content->mutex);
    const PanelScrollbarGeometry geometry = panel_scrollbar_geometry(
        state.width, state.height, top, content->viewport_height_px,
        content->content_extent_px, content->scroll_offset_px, theme);
    *out_geometry = {
        geometry.visible,
        {},
        geometry.hit_x,
        geometry.hit_y,
        geometry.hit_width,
        geometry.hit_height,
        geometry.track_x,
        geometry.track_y,
        geometry.track_width,
        geometry.track_height,
        geometry.thumb_x,
        geometry.thumb_y,
        geometry.thumb_width,
        geometry.thumb_height,
    };
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_test_pointer_button(sao_ui_panel_handle_t panel, int32_t x, int32_t y) {
    const sao_status_t press_status = panel_button(panel, 0, 0, x, y);
    if (press_status != SAO_STATUS_OK)
        return press_status;
    return panel_button(panel, 0, 1, x, y);
}
