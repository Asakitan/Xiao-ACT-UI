// SAO Auto — SDK UI wire.
//
// Forwards `SaoSdkContext::ui->*` calls into the platform UI module:
//   * register_ui_panel      → sao_ui_panel_register (SaoPanelDescriptor)
//   * unregister_ui_panel    → sao_ui_panel_unregister
//   * panel_add_widget       → widget kit create + sao_ui_panel_update_body
//   * panel_update_widget    → widget-kit-specific update
//   * panel_remove_widget    → sao_ui_panel_update_body(REMOVE_NODE)
//   * request_redraw         → sao_ui_panel_bring_to_front + counter bump
//   * register_render_hook_clock — SDK-local dispatcher (fired via
//     sao_sdk_internal::fire_render_hook_test in tests; the real
//     compositor will call the same entry point in production).
//
// The legacy JSON-spec register_panel/set_panel_spec/set_overlay paths
// exist for scripting-language plugins that emit `ui_spec` blobs; they
// share the same PanelEntry storage.

#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include "sdk_callback_barrier.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <limits>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
// Include the specific widget headers directly.  The umbrella
// (sao/ui/widget_kit.h) re-declares an extended kind enum whose
// values collide with the legacy d2d_widgets.h enum — pulling it in
// alongside widget_input/text/data would double-define several
// enumerators.
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/widget_data.h"
#include "sao/ui/widget_input.h"
#include "sao/ui/widget_text.h"
#include "sao/ui/widget_table.h"
#include "sao/ui/widget_chart.h"
extern "C" void SAO_UI_CALL sao_ui_widget_text_family_destroy(sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_input_family_destroy(sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_data_family_destroy(sao_ui_widget_handle_t handle);

namespace sao_sdk_internal {
namespace {

std::atomic_bool g_fail_next_panel_state_insertion{false};
std::atomic<sao_sdk_status_t> g_fail_next_panel_unregister_status{SAO_SDK_OK};
std::atomic_bool g_fail_next_widget_state_insertion{false};
std::atomic<sao_sdk_status_t> g_fail_next_widget_remove_status{SAO_SDK_OK};

using Json = nlohmann::json;


struct CreatedWidget {
    sao_ui_widget_handle_t widget = nullptr;
    sao_ui_script_canvas_handle_t script_canvas = nullptr;
};

sao_sdk_status_t unregister_native_panel(sao_ui_panel_handle_t panel) noexcept {
    const auto injected = g_fail_next_panel_unregister_status.exchange(SAO_SDK_OK);
    if (injected != SAO_SDK_OK)
        return injected;
    return invoke_callback_barrier(
        [&] { return static_cast<sao_sdk_status_t>(sao_ui_panel_unregister(panel)); });
}

bool valid_widget_kind(int32_t kind) noexcept {
    switch (kind) {
    case SAO_SDK_UI_WIDGET_LABEL:
    case SAO_SDK_UI_WIDGET_BUTTON:
    case SAO_SDK_UI_WIDGET_PROGRESS_BAR:
    case SAO_SDK_UI_WIDGET_TABLE:
    case SAO_SDK_UI_WIDGET_ROUNDED_PANEL:
    case SAO_SDK_UI_WIDGET_STATUS_BADGE:
    case SAO_SDK_UI_WIDGET_TEXT_FIELD:
    case SAO_SDK_UI_WIDGET_CHECKBOX:
    case SAO_SDK_UI_WIDGET_DIVIDER:
    case SAO_SDK_UI_WIDGET_ICON:
    case SAO_SDK_UI_WIDGET_RADIO:
    case SAO_SDK_UI_WIDGET_SLIDER:
    case SAO_SDK_UI_WIDGET_DROPDOWN:
    case SAO_SDK_UI_WIDGET_SCROLLBAR:
    case SAO_SDK_UI_WIDGET_METRIC:
    case SAO_SDK_UI_WIDGET_EMPTY_STATE:
    case SAO_SDK_UI_WIDGET_TREE_VIEW:
    case SAO_SDK_UI_WIDGET_BAR_CHART:
    case SAO_SDK_UI_WIDGET_LINE_CHART:
    case SAO_SDK_UI_WIDGET_SPARKLINE:
    case SAO_SDK_UI_WIDGET_SCRIPTABLE_CANVAS:
        return true;
    default:
        return false;
    }
}

bool json_i32(const Json& value, int32_t* out) {
    if (out == nullptr || !value.is_number_integer()) return false;
    try {
        const auto parsed = value.get<int64_t>();
        if (parsed < std::numeric_limits<int32_t>::min() || parsed > std::numeric_limits<int32_t>::max()) return false;
        *out = static_cast<int32_t>(parsed);
        return true;
    } catch (...) { return false; }
}

bool json_float(const Json& value, float* out) {
    if (out == nullptr || !value.is_number()) return false;
    try {
        const double parsed = value.get<double>();
        if (!std::isfinite(parsed) || parsed < std::numeric_limits<float>::lowest() || parsed > std::numeric_limits<float>::max()) return false;
        *out = static_cast<float>(parsed);
        return true;
    } catch (...) { return false; }
}

bool json_bool(const Json& value, bool* out) {
    if (out == nullptr || !value.is_boolean()) return false;
    *out = value.get<bool>();
    return true;
}

bool props_only(const Json& props, std::initializer_list<const char*> allowed) {
    if (!props.is_object()) return false;
    for (const auto& item : props.items()) {
        bool found = false;
        for (const char* key : allowed) if (item.key() == key) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

sao_sdk_status_t parse_props(const SaoSdkWidgetSpec& spec, Json* out) {
    if (out == nullptr || (spec.props_json_utf8 == nullptr && spec.props_len != 0)) return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (spec.props_json_utf8 == nullptr) { *out = Json::object(); return SAO_SDK_OK; }
    if (spec.props_len == 0) return SAO_SDK_ERR_INVALID_ARGUMENT;
    try {
        *out = Json::parse(reinterpret_cast<const char*>(spec.props_json_utf8), reinterpret_cast<const char*>(spec.props_json_utf8) + spec.props_len);
        return out->is_object() ? SAO_SDK_OK : SAO_SDK_ERR_INVALID_ARGUMENT;
    } catch (...) { return SAO_SDK_ERR_INVALID_ARGUMENT; }
}

bool json_color(const Json& value) {
    if (!value.is_string())
        return false;
    const auto& text = value.get_ref<const std::string&>();
    if ((text.size() != 7U && text.size() != 9U) || text[0] != '#')
        return false;
    for (size_t index = 1; index < text.size(); ++index) {
        const char c = text[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

bool json_nonnegative_float(const Json& props, const char* key) {
    const auto item = props.find(key);
    if (item == props.end())
        return true;
    float value = 0.0F;
    return json_float(*item, &value) && value >= 0.0F;
}

bool validate_scrollbar_props(const Json& props) {
    if (!props_only(props, {"fill", "border", "fg", "accent", "canvas_bg", "track",
                            "thumb", "thumb_hover", "thumb_active", "arrow", "focus",
                            "radius", "border_width", "value", "ratio", "page_size",
                            "content_size", "nudge_step", "padding", "tooltip", "text",
                            "label", "title", "style", "active", "enabled", "show_arrows",
                            "keyboard_nudge"}))
        return false;
    for (const char* key : {"fill", "border", "fg", "accent", "canvas_bg", "track", "thumb",
                            "thumb_hover", "thumb_active", "arrow", "focus"}) {
        const auto item = props.find(key);
        if (item != props.end() && !json_color(*item))
            return false;
    }
    for (const char* key : {"value", "ratio"}) {
        const auto item = props.find(key);
        float value = 0.0F;
        if (item != props.end() &&
            (!json_float(*item, &value) || value < 0.0F || value > 1.0F))
            return false;
    }
    if (!json_nonnegative_float(props, "radius") ||
        !json_nonnegative_float(props, "border_width"))
        return false;
    const auto padding = props.find("padding");
    if (padding != props.end() &&
        (!padding->is_number_integer() || padding->get<int64_t>() < 0))
        return false;
    const auto page_size = props.find("page_size");
    float page = 0.0F;
    if (page_size != props.end() && (!json_float(*page_size, &page) || page < 0.0F))
        return false;
    const auto content_size = props.find("content_size");
    float content = 0.0F;
    if (content_size != props.end() &&
        (!json_float(*content_size, &content) || content <= 0.0F))
        return false;
    const auto nudge_step = props.find("nudge_step");
    float nudge = 0.0F;
    if (nudge_step != props.end() && (!json_float(*nudge_step, &nudge) || nudge <= 0.0F))
        return false;
    for (const char* key : {"tooltip", "text", "label", "title", "style"}) {
        const auto item = props.find(key);
        if (item != props.end() && !item->is_string())
            return false;
    }
    for (const char* key : {"active", "enabled", "show_arrows", "keyboard_nudge"}) {
        const auto item = props.find(key);
        bool value = false;
        if (item != props.end() && !json_bool(*item, &value))
            return false;
    }
    return true;
}

sao_sdk_status_t remove_widget_node(PanelEntry& panel, const WidgetEntry& widget) {
    const auto injected = g_fail_next_widget_remove_status.exchange(SAO_SDK_OK);
    if (injected != SAO_SDK_OK)
        return injected;
    SaoUiBodyMutation mutation{};
    mutation.kind = SAO_UI_BODY_REMOVE_NODE;
    mutation.target = widget.layout_node;
    mutation.widget = widget.ui_widget;
    return static_cast<sao_sdk_status_t>(sao_ui_panel_update_body(panel.ui_body, &mutation, 1));
}
Json generic_props(const SaoSdkWidgetSpec& spec, Json props) {
    if (!props.contains("active")) props["active"] = true;
    if (spec.text_utf8 != nullptr) props["text"] = spec.text_utf8;
    if (spec.kind == SAO_SDK_UI_WIDGET_PROGRESS_BAR) {
        const float maximum = spec.max_value > 0.0F ? spec.max_value : 1.0F;
        props["value"] = std::clamp(spec.value / maximum, 0.0F, 1.0F);
    }
    return props;
}

Json initial_typed_props(int32_t kind, const Json& props) {
    Json result = Json::object();
    if (kind == SAO_SDK_UI_WIDGET_RADIO && props.contains("selected")) result["selected"] = props.at("selected");
    if (kind == SAO_SDK_UI_WIDGET_SLIDER) {
        if (props.contains("value")) result["value"] = props.at("value");
        if (props.contains("keyboard_nudge")) result["keyboard_nudge"] = props.at("keyboard_nudge");
    }
    if (kind == SAO_SDK_UI_WIDGET_DROPDOWN && props.contains("selected_id")) result["selected_id"] = props.at("selected_id");
    if (kind == SAO_SDK_UI_WIDGET_METRIC && props.contains("value")) result["value"] = props.at("value");
    if (kind == SAO_SDK_UI_WIDGET_EMPTY_STATE && props.contains("detail")) result["detail"] = props.at("detail");
    if (kind != SAO_SDK_UI_WIDGET_RADIO && kind != SAO_SDK_UI_WIDGET_SLIDER && kind != SAO_SDK_UI_WIDGET_DROPDOWN && kind != SAO_SDK_UI_WIDGET_METRIC && kind != SAO_SDK_UI_WIDGET_EMPTY_STATE) result = props;
    return result;
}

sao_sdk_status_t create_widget_for_kind(const SaoSdkWidgetSpec& spec, const Json& props, CreatedWidget* out) {
    if (spec.kind == SAO_SDK_UI_WIDGET_SCROLLBAR && !validate_scrollbar_props(props))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (out == nullptr || !valid_widget_kind(spec.kind)) return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out = {};
    sao_status_t status = SAO_STATUS_OK;
    switch (spec.kind) {
    case SAO_SDK_UI_WIDGET_RADIO: {
        if (!props_only(props, {"group_id", "value_id", "selected", "disabled"})) return SAO_SDK_ERR_INVALID_ARGUMENT;
        SaoUiRadioSpec native{}; native.label_utf8 = spec.text_utf8; native.font_size_px = 12; native.ring_size_px = 14;
        const auto group = props.find("group_id"); const auto value = props.find("value_id");
        if ((group != props.end() && !json_i32(*group, &native.group_id)) || (value != props.end() && !json_i32(*value, &native.value_id))) return SAO_SDK_ERR_INVALID_ARGUMENT;
        const auto selected = props.find("selected"); const auto disabled = props.find("disabled");
        if ((selected != props.end() && !json_bool(*selected, &native.selected)) || (disabled != props.end() && !json_bool(*disabled, &native.disabled))) return SAO_SDK_ERR_INVALID_ARGUMENT;
        status = sao_ui_radio_create(nullptr, &native, &out->widget); break;
    }
    case SAO_SDK_UI_WIDGET_SLIDER: {
        if (!props_only(props, {"value", "min_value", "max_value", "step", "vertical", "disabled", "show_value_label", "keyboard_nudge"})) return SAO_SDK_ERR_INVALID_ARGUMENT;
        SaoUiSliderSpec native{}; native.value = std::isfinite(spec.value) ? spec.value : 0.0F; native.min_value = 0.0F; native.max_value = spec.max_value > 0.0F ? spec.max_value : 1.0F; native.track_thickness_px = 4; native.thumb_size_px = 12;
        const auto set_float = [&](const char* key, float* target) { const auto item = props.find(key); return item == props.end() || json_float(*item, target); };
        const auto vertical = props.find("vertical"); const auto disabled = props.find("disabled"); const auto label = props.find("show_value_label");
        if (!set_float("value", &native.value) || !set_float("min_value", &native.min_value) || !set_float("max_value", &native.max_value) || !set_float("step", &native.step) || native.min_value >= native.max_value || native.step < 0.0F || (vertical != props.end() && !json_bool(*vertical, &native.vertical)) || (disabled != props.end() && !json_bool(*disabled, &native.disabled)) || (label != props.end() && !json_bool(*label, &native.show_value_label))) return SAO_SDK_ERR_INVALID_ARGUMENT;
        status = sao_ui_slider_create(nullptr, &native, &out->widget); break;
    }
    case SAO_SDK_UI_WIDGET_DROPDOWN: {
        if (!props_only(props, {"text", "entries", "selected_id"})) return SAO_SDK_ERR_INVALID_ARGUMENT;
        std::vector<std::string> labels; std::vector<SaoUiDropdownEntry> entries; const auto items = props.find("entries");
        if (items != props.end()) {
            if (!items->is_array() || items->size() > 4096) return SAO_SDK_ERR_INVALID_ARGUMENT;
            labels.resize(items->size()); entries.resize(items->size()); std::unordered_set<int32_t> ids;
            for (size_t i = 0; i < items->size(); ++i) { const auto& item = (*items)[i]; if (!props_only(item, {"label", "item_id", "enabled", "checked"})) return SAO_SDK_ERR_INVALID_ARGUMENT; int32_t id = 0; const auto id_json = item.find("item_id"); if (id_json == item.end() || !json_i32(*id_json, &id)) return SAO_SDK_ERR_INVALID_ARGUMENT; const auto label = item.find("label"); if (id != SAO_UI_DROPDOWN_SEPARATOR && (label == item.end() || !label->is_string() || !ids.insert(id).second)) return SAO_SDK_ERR_INVALID_ARGUMENT; labels[i] = label == item.end() ? std::string() : label->get<std::string>(); entries[i].label_utf8 = labels[i].c_str(); entries[i].item_id = id; entries[i].enabled = true; entries[i].checked = false; const auto enabled = item.find("enabled"); const auto checked = item.find("checked"); if ((enabled != item.end() && !json_bool(*enabled, &entries[i].enabled)) || (checked != item.end() && !json_bool(*checked, &entries[i].checked))) return SAO_SDK_ERR_INVALID_ARGUMENT; }
        }
        SaoUiDropdownButtonSpec native{}; native.text_utf8 = spec.text_utf8; native.kind = SAO_UI_BTN_NORMAL; native.entries = entries.empty() ? nullptr : entries.data(); native.entry_count = entries.size();
        const auto text = props.find("text"); if (text != props.end()) { if (!text->is_string()) return SAO_SDK_ERR_INVALID_ARGUMENT; native.text_utf8 = text->get_ref<const std::string&>().c_str(); }
        status = sao_ui_dropdown_button_create(nullptr, &native, &out->widget); break;
    }
    case SAO_SDK_UI_WIDGET_METRIC: {
        if (!props_only(props, {"label", "value", "unit", "emphasize"})) return SAO_SDK_ERR_INVALID_ARGUMENT;
        SaoUiMetricSpec native{}; native.label_utf8 = ""; native.value_utf8 = spec.text_utf8 == nullptr ? "" : spec.text_utf8; native.value_font_size_px = 14;
        const auto label = props.find("label"); const auto value = props.find("value"); const auto unit = props.find("unit"); const auto emphasize = props.find("emphasize");
        if (label != props.end() && !label->is_string()) return SAO_SDK_ERR_INVALID_ARGUMENT; if (value != props.end() && !value->is_string()) return SAO_SDK_ERR_INVALID_ARGUMENT; if (unit != props.end() && !unit->is_string()) return SAO_SDK_ERR_INVALID_ARGUMENT; if (emphasize != props.end() && !json_bool(*emphasize, &native.emphasize)) return SAO_SDK_ERR_INVALID_ARGUMENT;
        if (label != props.end()) native.label_utf8 = label->get_ref<const std::string&>().c_str(); if (value != props.end()) native.value_utf8 = value->get_ref<const std::string&>().c_str(); if (unit != props.end()) native.unit_utf8 = unit->get_ref<const std::string&>().c_str();
        status = sao_ui_metric_create(nullptr, &native, &out->widget); break;
    }
    case SAO_SDK_UI_WIDGET_EMPTY_STATE: {
        if (!props_only(props, {"title", "detail", "action", "action_enabled", "icon_slot"})) return SAO_SDK_ERR_INVALID_ARGUMENT;
        SaoUiEmptyStateSpec native{}; native.detail_utf8 = spec.text_utf8 == nullptr ? "" : spec.text_utf8; native.action_enabled = false; native.icon_slot = -1;
        const auto title = props.find("title"); const auto detail = props.find("detail"); const auto action = props.find("action"); const auto enabled = props.find("action_enabled"); const auto icon = props.find("icon_slot");
        if (title != props.end() && !title->is_string()) return SAO_SDK_ERR_INVALID_ARGUMENT; if (detail != props.end() && !detail->is_string()) return SAO_SDK_ERR_INVALID_ARGUMENT; if (action != props.end() && !action->is_string()) return SAO_SDK_ERR_INVALID_ARGUMENT; if (enabled != props.end() && !json_bool(*enabled, &native.action_enabled)) return SAO_SDK_ERR_INVALID_ARGUMENT; if (icon != props.end() && !json_i32(*icon, &native.icon_slot)) return SAO_SDK_ERR_INVALID_ARGUMENT;
        if (title != props.end()) native.title_utf8 = title->get_ref<const std::string&>().c_str(); if (detail != props.end()) native.detail_utf8 = detail->get_ref<const std::string&>().c_str(); if (action != props.end()) native.action_utf8 = action->get_ref<const std::string&>().c_str();
        status = sao_ui_empty_state_create(nullptr, &native, &out->widget); break;
    }
    case SAO_SDK_UI_WIDGET_TREE_VIEW: { SaoUiTreeViewSpec native{}; native.row_height_px = 22; native.indent_px = 16; native.caret_width_px = 12; status = sao_ui_tree_view_create(nullptr, &native, &out->widget); break; }
    case SAO_SDK_UI_WIDGET_BAR_CHART: { SaoUiBarChartSpec native{}; status = sao_ui_bar_chart_create(nullptr, &native, &out->widget); break; }
    case SAO_SDK_UI_WIDGET_LINE_CHART: { SaoUiLineChartSpec native{}; status = sao_ui_line_chart_create(nullptr, &native, &out->widget); break; }
    case SAO_SDK_UI_WIDGET_SPARKLINE: { SaoUiSparklineSpec native{}; native.max_points = 64; native.line_width_px = 1.0F; status = sao_ui_sparkline_create(nullptr, &native, &out->widget); break; }
    case SAO_SDK_UI_WIDGET_SCRIPTABLE_CANVAS:
        return SAO_SDK_ERR_UNSUPPORTED;
    default: {
        int32_t native_kind = SAO_UI_WIDGET_ROUNDED_PANEL;
        switch (spec.kind) { case SAO_SDK_UI_WIDGET_LABEL: native_kind = SAO_UI_WIDGET_TEXT; break; case SAO_SDK_UI_WIDGET_BUTTON: native_kind = SAO_UI_WIDGET_ACTION_BUTTON; break; case SAO_SDK_UI_WIDGET_PROGRESS_BAR: native_kind = SAO_UI_WIDGET_BAR; break; case SAO_SDK_UI_WIDGET_TABLE: native_kind = SAO_UI_WIDGET_TABLE; break; case SAO_SDK_UI_WIDGET_STATUS_BADGE: native_kind = SAO_UI_WIDGET_STATUS_BADGE; break; case SAO_SDK_UI_WIDGET_TEXT_FIELD: native_kind = SAO_UI_WIDGET_INPUT; break; case SAO_SDK_UI_WIDGET_CHECKBOX: native_kind = SAO_UI_WIDGET_CHECKBOX; break; case SAO_SDK_UI_WIDGET_DIVIDER: native_kind = SAO_UI_WIDGET_DIVIDER; break; case SAO_SDK_UI_WIDGET_ICON: native_kind = SAO_UI_WIDGET_ICON; break; case SAO_SDK_UI_WIDGET_SCROLLBAR: native_kind = SAO_UI_WIDGET_SCROLLBAR; break; case SAO_SDK_UI_WIDGET_ROUNDED_PANEL: break; default: return SAO_SDK_ERR_INVALID_ARGUMENT; }
        status = sao_ui_widget_create(native_kind, nullptr, &out->widget); break;
    }
    }
    if (status != SAO_STATUS_OK || out->widget == nullptr) return status == SAO_STATUS_OK ? SAO_SDK_ERR_UNSUPPORTED : static_cast<sao_sdk_status_t>(status);
    return SAO_SDK_OK;
}

void clear_legacy_canvases(PanelEntry& panel) {
    for (const auto canvas : panel.canvases) {
        sao_ui_script_canvas_destroy(canvas);
    }
    panel.canvases.clear();
    for (const auto placeholder : panel.canvas_placeholders) {
        sao_ui_widget_destroy(placeholder);
    }
    panel.canvas_placeholders.clear();
}

void destroy_panel_resources(PanelEntry& panel) {
    clear_legacy_canvases(panel);
    for (const auto& widget : panel.widgets)
        destroy_widget_for_kind(widget.kind, widget.ui_widget, widget.script_canvas);
    panel.widgets.clear();
}

sao_sdk_status_t materialize_legacy_canvas(PanelEntry& panel, const uint8_t* spec_json_utf8,
                                           size_t spec_len) {
    if (spec_json_utf8 == nullptr || spec_len == 0)
        return SAO_SDK_OK;
    const std::string spec(reinterpret_cast<const char*>(spec_json_utf8), spec_len);
    if (spec.find("\"canvas\"") == std::string::npos)
        return SAO_SDK_OK;

    SaoUiScriptCanvasSpec canvas_spec{};
    canvas_spec.width_px = 640;
    canvas_spec.height_px = 360;
    canvas_spec.antialias = true;
    canvas_spec.retain_ops_between_frames = true;
    canvas_spec.max_ops_per_frame = 4000;
    sao_ui_widget_handle_t widget = nullptr;
    sao_ui_script_canvas_handle_t canvas = nullptr;
    const sao_status_t canvas_rc =
        sao_ui_script_canvas_create(nullptr, &canvas_spec, &widget, &canvas);
    if (canvas_rc != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(canvas_rc);

    sao_ui_widget_handle_t placeholder = nullptr;
    if (sao_ui_widget_create(SAO_UI_WIDGET_ROUNDED_PANEL, nullptr, &placeholder) != SAO_STATUS_OK) {
        sao_ui_script_canvas_destroy(canvas);
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
    static constexpr char kCanvasProps[] = R"({"active":true})";
    if (sao_ui_widget_apply_props(placeholder, reinterpret_cast<const uint8_t*>(kCanvasProps),
                                  sizeof(kCanvasProps) - 1) != SAO_STATUS_OK) {
        sao_ui_widget_destroy(placeholder);
        sao_ui_script_canvas_destroy(canvas);
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }

    SaoUiLayoutSpec layout{};
    layout.fixed_width_px = canvas_spec.width_px;
    layout.fixed_height_px = canvas_spec.height_px;
    layout.hit_testable = true;
    SaoUiBodyMutation mutation{};
    mutation.kind = SAO_UI_BODY_ADD_WIDGET;
    mutation.widget = placeholder;
    mutation.spec = &layout;
    const sao_status_t panel_rc = sao_ui_panel_update_body(panel.ui_body, &mutation, 1);
    if (panel_rc != SAO_STATUS_OK) {
        sao_ui_widget_destroy(placeholder);
        sao_ui_script_canvas_destroy(canvas);
        return static_cast<sao_sdk_status_t>(panel_rc);
    }
    panel.canvases.push_back(canvas);
    panel.canvas_placeholders.push_back(placeholder);
    return SAO_SDK_OK;
}

// ─── UI table vtable functions ───────────────────────────────────────

sao_sdk_status_t SAO_SDK_CALL ui_register_ui_panel(void* ctx_impl,
                                                   const SaoSdkPanelDescriptor* descriptor,
                                                   sao_sdk_ui_panel_t* out_panel);

sao_sdk_status_t SAO_SDK_CALL ui_unregister_ui_panel(void* ctx_impl, sao_sdk_ui_panel_t panel);

sao_sdk_status_t SAO_SDK_CALL ui_register_panel(void* ctx_impl, const char* panel_id_utf8,
                                                const char* title_utf8,
                                                const uint8_t* initial_spec_json_utf8,
                                                size_t spec_len,
                                                sao_sdk_panel_action_callback_t action_cb,
                                                void* action_user_data,
                                                sao_sdk_ui_panel_t* out_panel) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_panel == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_panel = nullptr;
    if (initial_spec_json_utf8 == nullptr && spec_len != 0) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    SaoSdkPanelDescriptor descriptor{};
    descriptor.panel_id_utf8 = panel_id_utf8;
    descriptor.title_utf8 = title_utf8;
    descriptor.default_width_px = 480;
    descriptor.default_height_px = 320;
    descriptor.min_width_px = 160;
    descriptor.min_height_px = 100;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = true;
    descriptor.remember_geometry = true;
    descriptor.initial_opacity = 1.0f;

    const sao_sdk_status_t register_rc = ui_register_ui_panel(ctx_impl, &descriptor, out_panel);
    if (register_rc != SAO_SDK_OK)
        return register_rc;

    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    sao_ui_panel_body_handle_t body = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->panels.find(*out_panel);
        if (it == state->panels.end())
            return SAO_SDK_ERR_HANDLE_INVALID;
        it->second.legacy_action_cb = action_cb;
        it->second.legacy_action_user_data = action_user_data;
        body = it->second.ui_body;
    }
    if (initial_spec_json_utf8 != nullptr) {
        const sao_status_t spec_rc =
            sao_ui_panel_body_set_spec(body, initial_spec_json_utf8, spec_len);
        if (spec_rc != SAO_STATUS_OK) {
            (void)ui_unregister_ui_panel(ctx_impl, *out_panel);
            *out_panel = nullptr;
            return static_cast<sao_sdk_status_t>(spec_rc);
        }
        sao_sdk_status_t canvas_rc = SAO_SDK_ERR_HANDLE_INVALID;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            const auto it = state->panels.find(*out_panel);
            if (it != state->panels.end()) {
                canvas_rc = materialize_legacy_canvas(it->second, initial_spec_json_utf8, spec_len);
            }
        }
        if (canvas_rc != SAO_SDK_OK) {
            (void)ui_unregister_ui_panel(ctx_impl, *out_panel);
            *out_panel = nullptr;
            return canvas_rc;
        }
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_set_panel_spec(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                const uint8_t* spec_json_utf8, size_t spec_len) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr || (spec_json_utf8 == nullptr && spec_len != 0)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    sao_ui_panel_body_handle_t body = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return SAO_SDK_ERR_NOT_FOUND;
        if (it->second.unregistering)
            return SAO_SDK_ERR_BUSY;
        body = it->second.ui_body;
    }
    const sao_status_t spec_rc = sao_ui_panel_body_set_spec(body, spec_json_utf8, spec_len);
    if (spec_rc != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(spec_rc);
    std::lock_guard<std::mutex> lk(state->mu);
    const auto it = state->panels.find(panel);
    if (it == state->panels.end())
        return SAO_SDK_ERR_NOT_FOUND;
    if (it->second.unregistering)
        return SAO_SDK_ERR_BUSY;
    clear_legacy_canvases(it->second);
    return materialize_legacy_canvas(it->second, spec_json_utf8, spec_len);
}

sao_sdk_status_t SAO_SDK_CALL ui_set_overlay(void* ctx_impl, const char* surface_id_utf8,
                                             const uint8_t* spec_json_utf8, size_t spec_len) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (surface_id_utf8 == nullptr || surface_id_utf8[0] == '\0' ||
        (spec_json_utf8 == nullptr && spec_len != 0)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    sao_sdk_overlay_token_t previous = 0;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->overlays.find(surface_id_utf8);
        if (it != state->overlays.end()) {
            previous = reinterpret_cast<uint64_t>(it->second);
        }
    }
    if (previous != 0) {
        const auto clear_status = provider_overlay_clear(state, previous);
        if (clear_status != SAO_SDK_OK)
            return clear_status;
    }
    SaoSdkOverlaySpec spec{};
    spec.surface_id_utf8 = surface_id_utf8;
    spec.spec_json_utf8 = spec_json_utf8;
    spec.spec_len = spec_len;
    sao_sdk_overlay_token_t overlay = 0;
    return provider_overlay_set(state, &spec, &overlay);
}

sao_sdk_status_t SAO_SDK_CALL ui_register_render_hook_legacy(void* ctx_impl,
                                                             const char* surface_id_utf8,
                                                             float priority, void* hook_fn,
                                                             void* hook_user_data,
                                                             sao_sdk_hook_token_t* out_token) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_token != nullptr)
        *out_token = 0;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (surface_id_utf8 == nullptr || surface_id_utf8[0] == '\0' || hook_fn == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    (void)priority;
    (void)hook_user_data;
    return SAO_SDK_ERR_UNSUPPORTED;
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_render_hook_legacy(void* ctx_impl,
                                                               sao_sdk_hook_token_t token) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    return provider_render_unregister(cast_ctx(ctx_impl), token);
}

sao_sdk_status_t SAO_SDK_CALL ui_request_redraw(void* ctx_impl, const char* surface_id_utf8) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    const char* surface = surface_id_utf8 == nullptr ? SAO_ENGINE_ALL_SURFACES : surface_id_utf8;
    const auto status = provider_request_redraw(state, surface);
    if (status != SAO_SDK_OK)
        return status;
    std::lock_guard<std::mutex> lk(state->mu);
    for (auto& [handle, panel] : state->panels) {
        (void)handle;
        if (surface_id_utf8 == nullptr || panel.panel_id == surface_id_utf8) {
            ++panel.redraw_count;
        }
    }
    return SAO_SDK_OK;
}

// ─── Append-only typed panel and widget fields ─────────────────────

sao_sdk_status_t SAO_SDK_CALL ui_register_ui_panel(void* ctx_impl,
                                                   const SaoSdkPanelDescriptor* descriptor,
                                                   sao_sdk_ui_panel_t* out_panel) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_panel == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_panel = nullptr;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (descriptor == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (descriptor->panel_id_utf8 == nullptr || descriptor->panel_id_utf8[0] == '\0') {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    // Build a full SaoPanelDescriptor from the SDK subset.
    SaoPanelDescriptor full{};
    full.struct_size = sizeof(SaoPanelDescriptor);
    full.panel_id_utf8 = descriptor->panel_id_utf8;
    full.title_utf8 = descriptor->title_utf8;
    full.anchor = SAO_UI_PANEL_ANCHOR_ABSOLUTE;
    full.default_x_px = descriptor->default_x_px;
    full.default_y_px = descriptor->default_y_px;
    full.default_width_px = descriptor->default_width_px;
    full.default_height_px = descriptor->default_height_px;
    full.min_width_px = descriptor->min_width_px;
    full.min_height_px = descriptor->min_height_px;
    full.max_width_px = 0;
    full.max_height_px = 0;
    full.movable = descriptor->movable;
    full.resizable = descriptor->resizable;
    full.show_titlebar = descriptor->show_titlebar;
    full.show_close_button = descriptor->show_close_button;
    full.visible = descriptor->visible;
    full.remember_geometry = descriptor->remember_geometry;
    full.modal = descriptor->modal;
    full.overlay_style = descriptor->overlay_style;
    full.z_class = descriptor->z_class;
    full.z_within_class = descriptor->z_within_class;
    full.initial_opacity = descriptor->initial_opacity;

    std::list<PanelEntry>::iterator pending;
    try {
        PanelEntry entry;
        entry.panel_id = descriptor->panel_id_utf8;
        std::lock_guard<std::mutex> lock(state->mu);
        state->panel_cleanup_pending.push_back(std::move(entry));
        pending = std::prev(state->panel_cleanup_pending.end());
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }

    auto& rt = SharedRuntime::instance();
    sao_ui_panel_handle_t ui_panel = nullptr;
    sao_ui_panel_body_handle_t ui_body = nullptr;
    const auto register_status = invoke_callback_barrier([&]() -> sao_sdk_status_t {
        return static_cast<sao_sdk_status_t>(
            sao_ui_panel_register(rt.compositor, &full, &ui_panel, &ui_body));
    });
    if (register_status != SAO_SDK_OK) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->panel_cleanup_pending.erase(pending);
        if (register_status == SAO_STATUS_ERR_ALREADY_EXISTS)
            return SAO_SDK_ERR_ALREADY_EXISTS;
        if (register_status == SAO_STATUS_ERR_INVALID_ARGUMENT)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return register_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        pending->sdk_handle = reinterpret_cast<sao_sdk_ui_panel_t>(ui_panel);
        pending->ui_panel = ui_panel;
        pending->ui_body = ui_body;
    }
    if (ui_panel == nullptr || ui_body == nullptr) {
        const auto rollback_status =
            ui_panel == nullptr ? SAO_SDK_OK : unregister_native_panel(ui_panel);
        if (rollback_status == SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            state->panel_cleanup_pending.erase(pending);
        }
        return SAO_SDK_ERR_HANDLE_INVALID;
    }

    pause_context_api_test_point(ContextApiTestPoint::panel_registered);
    sao_sdk_status_t insertion_status = SAO_SDK_OK;
    try {
        if (g_fail_next_panel_state_insertion.exchange(false))
            throw std::bad_alloc{};
        std::lock_guard<std::mutex> lk(state->mu);
        const auto handle = pending->sdk_handle;
        if (state->panels.find(handle) != state->panels.end()) {
            insertion_status = SAO_SDK_ERR_ALREADY_EXISTS;
        } else {
            state->panels.emplace(handle, *pending);
            state->panel_cleanup_pending.erase(pending);
        }
    } catch (...) {
        insertion_status = SAO_SDK_ERR_INTERNAL;
    }
    if (insertion_status != SAO_SDK_OK) {
        const auto rollback_status = unregister_native_panel(ui_panel);
        if (rollback_status == SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            state->panel_cleanup_pending.erase(pending);
        }
        return rollback_status == SAO_SDK_OK ? insertion_status : rollback_status;
    }
    *out_panel = reinterpret_cast<sao_sdk_ui_panel_t>(ui_panel);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_ui_panel(void* ctx_impl, sao_sdk_ui_panel_t panel) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    sao_ui_panel_handle_t native_panel = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return SAO_SDK_ERR_NOT_FOUND;
        if (it->second.unregistering)
            return SAO_SDK_ERR_BUSY;
        it->second.unregistering = true;
        native_panel = it->second.ui_panel;
    }

    const auto unregister_status = unregister_native_panel(native_panel);
    if (unregister_status != SAO_SDK_OK) {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto found = state->panels.find(panel);
        if (found != state->panels.end())
            found->second.unregistering = false;
        return unregister_status;
    }

    PanelEntry retired;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return SAO_SDK_ERR_NOT_FOUND;
        retired = std::move(it->second);
        for (auto overlay = state->overlays.begin(); overlay != state->overlays.end();) {
            if (overlay->second == panel) {
                overlay = state->overlays.erase(overlay);
            } else {
                ++overlay;
            }
        }
        state->panels.erase(it);
    }
    destroy_panel_resources(retired);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_add_widget(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                  const SaoSdkWidgetSpec* widget_spec,
                                                  sao_sdk_ui_widget_t* out_widget) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_widget != nullptr)
        *out_widget = nullptr;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr || panel == nullptr || widget_spec == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    Json parsed;
    const auto parse_status = parse_props(*widget_spec, &parsed);
    if (parse_status != SAO_SDK_OK || !valid_widget_kind(widget_spec->kind))
        return parse_status != SAO_SDK_OK ? parse_status : SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget_spec->kind == SAO_SDK_UI_WIDGET_SCRIPTABLE_CANVAS)
        return SAO_SDK_ERR_UNSUPPORTED;

    CreatedWidget created;
    const auto create_status = create_widget_for_kind(*widget_spec, parsed, &created);
    if (create_status != SAO_SDK_OK)
        return create_status;

    Json wire_props = widget_spec->kind <= SAO_SDK_UI_WIDGET_ICON ||
                              widget_spec->kind == SAO_SDK_UI_WIDGET_SCROLLBAR
                          ? generic_props(*widget_spec, parsed)
                          : initial_typed_props(widget_spec->kind, parsed);
    const std::string wire_json = wire_props.dump();
    pause_context_api_test_point(ContextApiTestPoint::panel_operation_unlocked);
    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end()) {
        destroy_widget_for_kind(widget_spec->kind, created.widget, created.script_canvas);
        return SAO_SDK_ERR_NOT_FOUND;
    }
    PanelEntry& pe = it->second;
    if (pe.unregistering) {
        destroy_widget_for_kind(widget_spec->kind, created.widget, created.script_canvas);
        return SAO_SDK_ERR_BUSY;
    }
    if (widget_spec->widget_id_utf8 != nullptr && widget_spec->widget_id_utf8[0] != '\0') {
        for (const auto& w : pe.widgets) {
            if (w.widget_id == widget_spec->widget_id_utf8) {
                destroy_widget_for_kind(widget_spec->kind, created.widget, created.script_canvas);
                return SAO_SDK_ERR_ALREADY_EXISTS;
            }
        }
    }

    SaoUiLayoutSpec layout{};
    layout.fixed_width_px = widget_spec->width_px;
    layout.fixed_height_px = widget_spec->height_px;
    layout.absolute_x_px = widget_spec->x_px;
    layout.absolute_y_px = widget_spec->y_px;
    layout.absolute_z = widget_spec->z_order;
    layout.hit_testable = true;
    WidgetEntry entry;
    entry.sdk_handle = reinterpret_cast<sao_sdk_ui_widget_t>(reinterpret_cast<uintptr_t>(&pe) + pe.next_widget_id);
    entry.ui_widget = created.widget;
    entry.script_canvas = created.script_canvas;
    entry.widget_id = widget_spec->widget_id_utf8 == nullptr ? std::string() : widget_spec->widget_id_utf8;
    entry.props_json = wire_json;
    entry.kind = widget_spec->kind;
    SaoUiBodyMutation mutations[2]{};
    mutations[0].kind = SAO_UI_BODY_ADD_WIDGET;
    mutations[0].widget = created.widget;
    mutations[0].spec = &layout;
    mutations[0].out_new_node = &entry.layout_node;
    mutations[1].kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    mutations[1].widget = created.widget;
    mutations[1].props_json_utf8 = reinterpret_cast<const uint8_t*>(wire_json.data());
    mutations[1].props_len = wire_json.size();
    const auto batch_status = sao_ui_panel_update_body(pe.ui_body, mutations, 2);
    if (batch_status != SAO_STATUS_OK) {
        destroy_widget_for_kind(widget_spec->kind, created.widget, created.script_canvas);
        return static_cast<sao_sdk_status_t>(batch_status);
    }

    try {
        pe.next_widget_id += 8;
        pe.widgets.push_back(std::move(entry));
    } catch (...) {
        SaoUiBodyMutation rollback{};
        rollback.kind = SAO_UI_BODY_REMOVE_NODE;
        rollback.target = entry.layout_node;
        (void)sao_ui_panel_update_body(pe.ui_body, &rollback, 1);
        destroy_widget_for_kind(widget_spec->kind, created.widget, created.script_canvas);
        return SAO_SDK_ERR_INTERNAL;
    }
    if (g_fail_next_widget_state_insertion.exchange(false)) {
        WidgetEntry& residual = pe.widgets.back();
        const auto remove_status = remove_widget_node(pe, residual);
        if (remove_status == SAO_SDK_OK) {
            destroy_widget_for_kind(residual.kind, residual.ui_widget, residual.script_canvas);
            pe.widgets.pop_back();
            return SAO_SDK_ERR_INTERNAL;
        }
        residual.cleanup_pending = true;
        return SAO_SDK_ERR_INTERNAL;
    }
    if (out_widget != nullptr)
        *out_widget = pe.widgets.back().sdk_handle;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_update_widget(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                     sao_sdk_ui_widget_t widget,
                                                     const SaoSdkWidgetSpec* widget_spec) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr || panel == nullptr || widget == nullptr || widget_spec == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    Json parsed;
    const auto parse_status = parse_props(*widget_spec, &parsed);
    if (parse_status != SAO_SDK_OK)
        return parse_status;
    if (!valid_widget_kind(widget_spec->kind))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget_spec->kind == SAO_SDK_UI_WIDGET_SCROLLBAR && !validate_scrollbar_props(parsed))
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end())
        return SAO_SDK_ERR_NOT_FOUND;
    PanelEntry& pe = it->second;
    if (pe.unregistering)
        return SAO_SDK_ERR_BUSY;
    WidgetEntry* target = nullptr;
    for (auto& candidate : pe.widgets) {
        if (candidate.sdk_handle == widget) {
            target = &candidate;
            break;
        }
    }
    if (target == nullptr)
        return SAO_SDK_ERR_NOT_FOUND;
    if (target->kind != widget_spec->kind || !valid_widget_kind(widget_spec->kind))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (target->kind == SAO_SDK_UI_WIDGET_SCRIPTABLE_CANVAS)
        return SAO_SDK_ERR_UNSUPPORTED;

    const Json wire_props = widget_spec->kind <= SAO_SDK_UI_WIDGET_ICON ||
                                    widget_spec->kind == SAO_SDK_UI_WIDGET_SCROLLBAR
                                ? generic_props(*widget_spec, parsed)
                                : parsed;
    const std::string wire_json = wire_props.dump();
    SaoUiBodyMutation mutation{};
    mutation.kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    mutation.target = target->layout_node;
    mutation.widget = target->ui_widget;
    mutation.props_json_utf8 = reinterpret_cast<const uint8_t*>(wire_json.data());
    mutation.props_len = wire_json.size();
    const auto update_status = sao_ui_panel_update_body(pe.ui_body, &mutation, 1);
    if (update_status != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(update_status);
    target->props_json = wire_json;
    target->cleanup_pending = false;
    return SAO_SDK_OK;
}
sao_sdk_status_t SAO_SDK_CALL ui_panel_remove_widget(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                     sao_sdk_ui_widget_t widget) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end())
        return SAO_SDK_ERR_NOT_FOUND;
    PanelEntry& pe = it->second;
    if (pe.unregistering)
        return SAO_SDK_ERR_BUSY;

    auto pred = [widget](const WidgetEntry& w) { return w.sdk_handle == widget; };
    auto rem = std::find_if(pe.widgets.begin(), pe.widgets.end(), pred);
    if (rem == pe.widgets.end())
        return SAO_SDK_ERR_NOT_FOUND;

    const auto update_rc = remove_widget_node(pe, *rem);
    if (update_rc != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(update_rc);
    destroy_widget_for_kind(rem->kind, rem->ui_widget, rem->script_canvas);
    pe.widgets.erase(rem);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_register_render_hook_clock(void* ctx_impl, int32_t hook_point,
                                                            sao_sdk_render_hook_callback_t callback,
                                                            void* user_data,
                                                            sao_sdk_hook_token_t* out_token) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_token != nullptr)
        *out_token = 0;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (callback == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    return provider_render_register(state, hook_point, callback, user_data, out_token);
}

sao_sdk_status_t SAO_SDK_CALL ui_register_panel_boundary(
    void* ctx_impl, const char* panel_id_utf8, const char* title_utf8,
    const uint8_t* initial_spec_json_utf8, size_t spec_len,
    sao_sdk_panel_action_callback_t action_cb, void* action_user_data,
    sao_sdk_ui_panel_t* out_panel) noexcept {
    return invoke_callback_barrier([&] {
        return ui_register_panel(ctx_impl, panel_id_utf8, title_utf8, initial_spec_json_utf8,
                                 spec_len, action_cb, action_user_data, out_panel);
    });
}

sao_sdk_status_t SAO_SDK_CALL ui_set_panel_spec_boundary(
    void* ctx_impl, sao_sdk_ui_panel_t panel, const uint8_t* spec_json_utf8,
    size_t spec_len) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_set_panel_spec(ctx_impl, panel, spec_json_utf8, spec_len); });
}

sao_sdk_status_t SAO_SDK_CALL ui_set_overlay_boundary(
    void* ctx_impl, const char* surface_id_utf8, const uint8_t* spec_json_utf8,
    size_t spec_len) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_set_overlay(ctx_impl, surface_id_utf8, spec_json_utf8, spec_len); });
}

sao_sdk_status_t SAO_SDK_CALL ui_register_render_hook_legacy_boundary(
    void* ctx_impl, const char* surface_id_utf8, float priority, void* hook_fn,
    void* hook_user_data, sao_sdk_hook_token_t* out_token) noexcept {
    return invoke_callback_barrier([&] {
        return ui_register_render_hook_legacy(ctx_impl, surface_id_utf8, priority, hook_fn,
                                              hook_user_data, out_token);
    });
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_render_hook_legacy_boundary(
    void* ctx_impl, sao_sdk_hook_token_t token) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_unregister_render_hook_legacy(ctx_impl, token); });
}

sao_sdk_status_t SAO_SDK_CALL ui_register_render_hook_clock_boundary(
    void* ctx_impl, int32_t hook_point, sao_sdk_render_hook_callback_t callback, void* user_data,
    sao_sdk_hook_token_t* out_token) noexcept {
    return invoke_callback_barrier([&] {
        return ui_register_render_hook_clock(ctx_impl, hook_point, callback, user_data, out_token);
    });
}

sao_sdk_status_t SAO_SDK_CALL ui_request_redraw_boundary(
    void* ctx_impl, const char* surface_id_utf8) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_request_redraw(ctx_impl, surface_id_utf8); });
}

sao_sdk_status_t SAO_SDK_CALL ui_register_ui_panel_boundary(
    void* ctx_impl, const SaoSdkPanelDescriptor* descriptor,
    sao_sdk_ui_panel_t* out_panel) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_register_ui_panel(ctx_impl, descriptor, out_panel); });
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_ui_panel_boundary(
    void* ctx_impl, sao_sdk_ui_panel_t panel) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_unregister_ui_panel(ctx_impl, panel); });
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_add_widget_boundary(
    void* ctx_impl, sao_sdk_ui_panel_t panel, const SaoSdkWidgetSpec* widget_spec,
    sao_sdk_ui_widget_t* out_widget) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_panel_add_widget(ctx_impl, panel, widget_spec, out_widget); });
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_update_widget_boundary(
    void* ctx_impl, sao_sdk_ui_panel_t panel, sao_sdk_ui_widget_t widget,
    const SaoSdkWidgetSpec* widget_spec) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_panel_update_widget(ctx_impl, panel, widget, widget_spec); });
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_remove_widget_boundary(
    void* ctx_impl, sao_sdk_ui_panel_t panel, sao_sdk_ui_widget_t widget) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_panel_remove_widget(ctx_impl, panel, widget); });
}

} // namespace

void destroy_widget_for_kind(int32_t kind, sao_ui_widget_handle_t widget,
                             sao_ui_script_canvas_handle_t script_canvas) {
    if (kind == SAO_SDK_UI_WIDGET_SCRIPTABLE_CANVAS && script_canvas != nullptr) {
        sao_ui_script_canvas_destroy(script_canvas);
        return;
    }
    if (widget != nullptr)
        sao_ui_widget_destroy(widget);
}

sao_sdk_status_t cleanup_ui_panels(ContextState* state) {
    std::list<PanelEntry> pending;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        for (auto& [handle, panel] : state->panels) {
            (void)handle;
            pending.push_back(std::move(panel));
        }
        state->panels.clear();
        state->overlays.clear();
        pending.splice(pending.end(), state->panel_cleanup_pending);
    }

    sao_sdk_status_t cleanup_status = SAO_SDK_OK;
    std::list<PanelEntry> failed;
    for (auto current = pending.begin(); current != pending.end();) {
        auto candidate = current++;
        const auto status = candidate->ui_panel == nullptr
                                ? SAO_SDK_OK
                                : unregister_native_panel(candidate->ui_panel);
        if (status != SAO_SDK_OK) {
            if (cleanup_status == SAO_SDK_OK)
                cleanup_status = status;
            failed.splice(failed.end(), pending, candidate);
            continue;
        }
        destroy_panel_resources(*candidate);
        pending.erase(candidate);
    }
    if (!failed.empty()) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->panel_cleanup_pending.splice(state->panel_cleanup_pending.end(), failed);
    }
    return cleanup_status;
}

const SaoSdkUiTable* make_ui_table() {
    static const SaoSdkUiTable table = {
        // Legacy JSON path retained for scripting-language ui_spec consumers.
        ui_register_panel_boundary,
        ui_set_panel_spec_boundary,
        ui_set_overlay_boundary,
        ui_register_render_hook_legacy_boundary,
        ui_unregister_render_hook_legacy_boundary,
        // Typed render-clock hook.
        ui_register_render_hook_clock_boundary,
        ui_request_redraw_boundary,
        // Descriptor-based panel and widget CRUD.
        ui_register_ui_panel_boundary,
        ui_unregister_ui_panel_boundary,
        ui_panel_add_widget_boundary,
        ui_panel_update_widget_boundary,
        ui_panel_remove_widget_boundary,
    };
    return &table;
}

void test_fail_next_panel_state_insertion() noexcept {
    g_fail_next_panel_state_insertion.store(true);
}

void test_fail_next_panel_unregister(sao_sdk_status_t status) noexcept {
    g_fail_next_panel_unregister_status.store(status);
}

void test_fail_next_widget_state_insertion() noexcept {
    g_fail_next_widget_state_insertion.store(true);
}

void test_fail_next_widget_remove(sao_sdk_status_t status) noexcept {
    g_fail_next_widget_remove_status.store(status);
}

// ─── Public free-function wrappers ──────────────────────────────────

} // namespace sao_sdk_internal

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_ui_panel(
    const struct SaoSdkContext* ctx, const struct SaoSdkPanelDescriptor* descriptor,
    sao_sdk_ui_panel_t* out_panel) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->register_ui_panel == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->register_ui_panel(lease.state(), descriptor, out_panel);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_unregister_ui_panel(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->unregister_ui_panel == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->unregister_ui_panel(lease.state(), panel);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_add_widget(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel,
    const struct SaoSdkWidgetSpec* widget_spec, sao_sdk_ui_widget_t* out_widget) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->panel_add_widget == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->panel_add_widget(lease.state(), panel, widget_spec, out_widget);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_update_widget(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel, sao_sdk_ui_widget_t widget,
    const struct SaoSdkWidgetSpec* widget_spec) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->panel_update_widget == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->panel_update_widget(lease.state(), panel, widget, widget_spec);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_remove_widget(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel, sao_sdk_ui_widget_t widget) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->panel_remove_widget == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->panel_remove_widget(lease.state(), panel, widget);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_render_hook(
    const struct SaoSdkContext* ctx, int32_t hook_point, sao_sdk_render_hook_callback_t callback,
    void* user_data, sao_sdk_hook_token_t* out_hook_handle) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr ||
            public_context->ui->register_render_hook_clock == nullptr) {
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        }
        return public_context->ui->register_render_hook_clock(
            lease.state(), hook_point, callback, user_data, out_hook_handle);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_render_hook_ex(
    const struct SaoSdkContext* ctx, const struct SaoSdkRenderHookSpec* spec,
    sao_sdk_render_hook_callback_t callback, void* user_data,
    sao_sdk_hook_token_t* out_hook_handle) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        return sao_sdk_internal::provider_render_register_ex(
            lease.state(), spec, callback, user_data, out_hook_handle);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_unregister_render_hook(const struct SaoSdkContext* ctx, sao_sdk_hook_token_t hook_handle) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        return sao_sdk_internal::make_ui_table()->unregister_render_hook(lease.state(),
                                                                         hook_handle);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_request_redraw(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->request_redraw == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        auto* state = lease.state();
        std::string surface;
        if (panel != nullptr) {
            std::lock_guard<std::mutex> lk(state->mu);
            const auto found = state->panels.find(panel);
            if (found == state->panels.end())
                return SAO_SDK_ERR_NOT_FOUND;
            if (found->second.unregistering)
                return SAO_SDK_ERR_BUSY;
            surface = found->second.panel_id;
        }
        return public_context->ui->request_redraw(lease.state(),
                                                  surface.empty() ? nullptr : surface.c_str());
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_request_redraw_surface(const struct SaoSdkContext* ctx, const char* surface_id_utf8) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->request_redraw == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->request_redraw(lease.state(), surface_id_utf8);
    });
}

// ─── Test-only observability hooks ──────────────────────────────────
//
// Exported so the demo test file can inspect the SDK state without
// grubbing through the ctx_impl.  Only linked in tests — production
// builds strip these via the linker's dead-code elimination.

#if defined(SAO_SDK_TESTING)
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_panel_state_insertion(void) {
    sao_sdk_internal::test_fail_next_panel_state_insertion();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fail_next_panel_unregister(sao_sdk_status_t status) {
    sao_sdk_internal::test_fail_next_panel_unregister(status);
}

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fail_next_widget_state_insertion(void) {
    sao_sdk_internal::test_fail_next_widget_state_insertion();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fail_next_widget_remove(sao_sdk_status_t status) {
    sao_sdk_internal::test_fail_next_widget_remove(status);
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_widget_count(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    try {
        if (panel == nullptr)
            return 0;
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return 0;
        auto* state = lease.state();
        std::lock_guard<std::mutex> lk(state->mu);
        auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return 0;
        return it->second.widgets.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_canvas_count(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    try {
        if (panel == nullptr)
            return 0;
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return 0;
        auto* state = lease.state();
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return 0;
        return it->second.canvases.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_SDK_API uint64_t SAO_SDK_CALL
sao_sdk_test_panel_redraw_count(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    try {
        if (panel == nullptr)
            return 0;
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return 0;
        auto* state = lease.state();
        std::lock_guard<std::mutex> lk(state->mu);
        auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return 0;
        return it->second.redraw_count;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_test_panel_invoke_action(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel, const char* action_key_utf8) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    if (panel == nullptr || action_key_utf8 == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (panel == nullptr || action_key_utf8 == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        auto* state = lease.state();
        sao_sdk_panel_action_callback_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            const auto it = state->panels.find(panel);
            if (it == state->panels.end())
                return SAO_SDK_ERR_NOT_FOUND;
            callback = it->second.legacy_action_cb;
            user_data = it->second.legacy_action_user_data;
        }
        if (callback == nullptr)
            return SAO_SDK_ERR_UNSUPPORTED;
        sao_sdk_internal::PluginCallbackLease callback_lease(state);
        if (!callback_lease)
            return SAO_SDK_ERR_BUSY;
        return sao_sdk_internal::invoke_void_callback_barrier(
            [&] { callback(action_key_utf8, nullptr, 0, user_data); });
    });
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_render_hook_count(const struct SaoSdkContext* ctx) {
    try {
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return 0;
        auto* state = lease.state();
        std::lock_guard<std::mutex> lk(state->mu);
        return state->render_hooks.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_cleanup_pending_count(const struct SaoSdkContext* ctx) {
    try {
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return 0;
        std::lock_guard<std::mutex> lock(lease.state()->mu);
        return lease.state()->panel_cleanup_pending.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fire_render_hook(int32_t hook_point, const struct SaoSdkRenderHookPayload* payload) {
    try {
        SaoSdkRenderHookPayload local{};
        if (payload != nullptr)
            local = *payload;
        sao_sdk_internal::fire_render_hook_test(hook_point, local);
    } catch (...) {
    }
}
extern "C" SAO_SDK_API sao_ui_widget_handle_t SAO_SDK_CALL sao_sdk_test_widget_native_handle(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel, sao_sdk_ui_widget_t widget) {
    try {
        if (panel == nullptr || widget == nullptr) return nullptr;
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease) return nullptr;
        auto* state = lease.state();
        std::lock_guard<std::mutex> lock(state->mu);
        const auto panel_it = state->panels.find(panel);
        if (panel_it == state->panels.end()) return nullptr;
        for (const auto& entry : panel_it->second.widgets)
            if (entry.sdk_handle == widget) return entry.ui_widget;
        return nullptr;
    } catch (...) { return nullptr; }
}

#endif
