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
#include <string_view>
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
struct LegacyActionBridge {
    std::shared_ptr<ContextCallbackGate> callback_gate;
    sao_sdk_panel_action_callback_t callback = nullptr;
    void* user_data = nullptr;
};

void SAO_UI_CALL legacy_action_bridge_callback(const char* action_key_utf8, const uint8_t* action_arg_json_utf8, size_t action_arg_len, void* user_data) noexcept {
    auto* bridge = static_cast<LegacyActionBridge*>(user_data);
    if (bridge == nullptr || bridge->callback == nullptr) return;
    PluginCallbackLease lease(bridge->callback_gate);
    if (!lease) return;
    (void)invoke_void_callback_barrier([&] { bridge->callback(action_key_utf8, action_arg_json_utf8, action_arg_len, bridge->user_data); });
}

namespace {

sao_sdk_status_t SAO_SDK_CALL ui_unregister_ui_panel(
    void* ctx_impl, sao_sdk_ui_panel_t panel);

std::atomic_bool g_fail_next_panel_state_insertion{false};
std::atomic<sao_sdk_status_t> g_fail_next_panel_unregister_status{SAO_SDK_OK};
std::atomic_bool g_fail_next_widget_state_insertion{false};
std::atomic<sao_sdk_status_t> g_fail_next_widget_remove_status{SAO_SDK_OK};
std::mutex g_widget_token_mutex;
std::vector<std::unique_ptr<WidgetToken>> g_widget_token_quarantine;
uint64_t g_next_widget_token_generation = 1;

using Json = nlohmann::json;

constexpr size_t kMaximumTypedPropsBytes = 8U * 1024U * 1024U;
constexpr size_t kMaximumTypedPropsDepth = 64U;
constexpr size_t kMaximumTypedPropsNodes = 16384U;
constexpr size_t kMaximumTypedPropsStringBytes = 1024U * 1024U;
constexpr size_t kMaximumTypedPropsTotalStringBytes = 4U * 1024U * 1024U;

bool valid_utf8(std::string_view value) noexcept {
    size_t index = 0;
    while (index < value.size()) {
        const auto byte = static_cast<unsigned char>(value[index]);
        size_t width = 0;
        uint32_t code_point = 0;
        if (byte <= 0x7fU) {
            width = 1;
            code_point = byte;
        } else if (byte >= 0xc2U && byte <= 0xdfU) {
            width = 2;
            code_point = byte & 0x1fU;
        } else if (byte >= 0xe0U && byte <= 0xefU) {
            width = 3;
            code_point = byte & 0x0fU;
        } else if (byte >= 0xf0U && byte <= 0xf4U) {
            width = 4;
            code_point = byte & 0x07U;
        } else {
            return false;
        }
        if (index + width > value.size()) return false;
        for (size_t offset = 1; offset < width; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if ((width == 2U && code_point < 0x80U) ||
            (width == 3U && code_point < 0x800U) ||
            (width == 4U && code_point < 0x10000U) ||
            code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += width;
    }
    return true;
}

bool valid_optional_utf8_c_string(const char* value) noexcept {
    if (value == nullptr) return true;
    size_t length = 0;
    while (length <= kMaximumTypedPropsStringBytes && value[length] != '\0') ++length;
    return length <= kMaximumTypedPropsStringBytes &&
           valid_utf8(std::string_view(value, length));
}

class bounded_typed_props_sax final : public Json::json_sax_t {
public:
    bool null() override { return consume_node(); }
    bool boolean(bool) override { return consume_node(); }
    bool number_integer(number_integer_t) override { return consume_node(); }
    bool number_unsigned(number_unsigned_t) override { return consume_node(); }
    bool number_float(number_float_t value, const string_t&) override { return std::isfinite(value) && consume_node(); }
    bool string(string_t& value) override { return consume_node() && consume_string(value); }
    bool binary(binary_t& value) override { return consume_node() && value.size() <= kMaximumTypedPropsBytes; }
    bool start_object(std::size_t) override { return start_container(); }
    bool key(string_t& value) override { return consume_string(value); }
    bool end_object() override { return end_container(); }
    bool start_array(std::size_t) override { return start_container(); }
    bool end_array() override { return end_container(); }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

private:
    size_t depth_ = 0;
    size_t nodes_ = 0;
    size_t string_bytes_ = 0;

    bool consume_node() noexcept {
        if (nodes_ >= kMaximumTypedPropsNodes) return false;
        ++nodes_;
        return true;
    }

    bool consume_string(std::string_view value) noexcept {
        if (value.find('\0') != std::string_view::npos ||
            !valid_utf8(value) || value.size() > kMaximumTypedPropsStringBytes ||
            string_bytes_ > kMaximumTypedPropsTotalStringBytes - value.size()) {
            return false;
        }
        string_bytes_ += value.size();
        return true;
    }

    bool start_container() noexcept {
        if (depth_ >= kMaximumTypedPropsDepth || !consume_node()) return false;
        ++depth_;
        return true;
    }

    bool end_container() noexcept {
        if (depth_ == 0) return false;
        --depth_;
        return true;
    }
};

bool validate_typed_props_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaximumTypedPropsBytes || !valid_utf8(text)) return false;
    try { bounded_typed_props_sax sax; return Json::sax_parse(text.begin(), text.end(), &sax); }
    catch (...) { return false; }
}
bool serialize_validated_typed_props(const Json& value, std::string& output) noexcept {
    try { output = value.dump(); return validate_typed_props_text(output); }
    catch (...) { output.clear(); return false; }
}

struct CreatedWidget {
    sao_ui_widget_handle_t widget = nullptr;
    sao_ui_script_canvas_handle_t script_canvas = nullptr;
};

struct LegacyCanvasResources {
    std::vector<sao_ui_script_canvas_handle_t> canvases;
    std::vector<sao_ui_widget_handle_t> placeholders;
};

bool panel_native_handles_current(const PanelEntry& entry, sao_ui_panel_handle_t native_panel,
                                  sao_ui_panel_body_handle_t body) noexcept {
    SaoPanelState panel_state{};
    if (entry.ui_panel != native_panel || entry.ui_body != body || entry.unregistering ||
        native_panel == nullptr || body == nullptr ||
        sao_ui_panel_get_state(native_panel, &panel_state) != SAO_STATUS_OK)
        return false;
    sao_ui_layout_node_handle_t root = nullptr;
    return sao_ui_panel_body_get_root(body, &root) == SAO_STATUS_OK && root != nullptr;
}

bool widget_entry_current(const PanelEntry& entry, const WidgetEntry& expected) noexcept {
    const auto found = std::find_if(entry.widgets.begin(), entry.widgets.end(),
                                    [&](const WidgetEntry& candidate) {
                                        return candidate.sdk_handle == expected.sdk_handle;
                                    });
    return found != entry.widgets.end() && found->ui_widget == expected.ui_widget &&
           found->script_canvas == expected.script_canvas &&
           found->layout_node == expected.layout_node && found->kind == expected.kind;
}

bool sdk_struct_size_valid(uint32_t declared_size, size_t required_size, size_t full_size) {
    if (required_size > full_size) return false;
    const size_t size = declared_size == 0u ? full_size : declared_size;
    return size >= required_size;
}

sao_sdk_ui_widget_t allocate_widget_token() {
    auto token = std::make_unique<WidgetToken>();
    std::lock_guard<std::mutex> lock(g_widget_token_mutex);
    token->generation = g_next_widget_token_generation++;
    if (token->generation == 0)
        token->generation = g_next_widget_token_generation++;
    token->serial = token->generation;
    const auto handle = reinterpret_cast<sao_sdk_ui_widget_t>(token.get());
    g_widget_token_quarantine.push_back(std::move(token));
    return handle;
}

bool uses_generic_sdk_props(int32_t kind) noexcept {
    switch (kind) {
    case SAO_SDK_UI_WIDGET_LABEL:
    case SAO_SDK_UI_WIDGET_BUTTON:
    case SAO_SDK_UI_WIDGET_ROUNDED_PANEL:
    case SAO_SDK_UI_WIDGET_TEXT_FIELD:
    case SAO_SDK_UI_WIDGET_CHECKBOX:
    case SAO_SDK_UI_WIDGET_DIVIDER:
    case SAO_SDK_UI_WIDGET_ICON:
    case SAO_SDK_UI_WIDGET_SCROLLBAR:
        return true;
    default:
        return false;
    }
}

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

bool parse_table_columns(const Json& props, std::vector<std::string>* keys,
                         std::vector<std::string>* titles,
                         std::vector<SaoUiTableColumn>* columns) {
    if (keys == nullptr || titles == nullptr || columns == nullptr)
        return false;
    const auto property = props.find("columns");
    if (property == props.end())
        return true;
    if (!property->is_array() || property->size() > 16U)
        return false;
    keys->resize(property->size());
    titles->resize(property->size());
    columns->resize(property->size());
    for (size_t index = 0; index < property->size(); ++index) {
        const auto& item = (*property)[index];
        if (!props_only(item, {"key", "title", "type", "align", "min_width_px",
                               "max_width_px", "flex_weight", "sortable", "filterable",
                               "resizable", "hidden", "header_bg_argb", "header_fg_argb",
                               "cell_fg_argb", "cell_bg_alt_argb"}))
            return false;
        const auto key = item.find("key");
        if (key == item.end() || !key->is_string() || key->get<std::string>().empty())
            return false;
        (*keys)[index] = key->get<std::string>();
        const auto title = item.find("title");
        if (title != item.end() && !title->is_string())
            return false;
        (*titles)[index] = title == item.end() ? (*keys)[index] : title->get<std::string>();
        auto& column = (*columns)[index];
        column.key_utf8 = (*keys)[index].c_str();
        column.title_utf8 = (*titles)[index].c_str();
        column.filterable = true;
        const auto type = item.find("type");
        const auto align = item.find("align");
        if (type != item.end() && !json_i32(*type, &column.type))
            return false;
        if (align != item.end() && !json_i32(*align, &column.align))
            return false;
        const auto parse_i32 = [&](const char* name, int32_t* output) {
            const auto value = item.find(name);
            return value == item.end() || json_i32(*value, output);
        };
        const auto parse_float = [&](const char* name, float* output) {
            const auto value = item.find(name);
            return value == item.end() || json_float(*value, output);
        };
        const auto parse_bool = [&](const char* name, bool* output) {
            const auto value = item.find(name);
            return value == item.end() || json_bool(*value, output);
        };
        if (!parse_i32("min_width_px", &column.min_width_px) ||
            !parse_i32("max_width_px", &column.max_width_px) ||
            !parse_float("flex_weight", &column.flex_weight) ||
            !parse_bool("sortable", &column.sortable) ||
            !parse_bool("filterable", &column.filterable) ||
            !parse_bool("resizable", &column.resizable) ||
            !parse_bool("hidden", &column.hidden) ||
            column.min_width_px < 0 || column.max_width_px < 0 ||
            column.flex_weight < 0.0F)
            return false;
        for (const char* name : {"header_bg_argb", "header_fg_argb", "cell_fg_argb",
                                 "cell_bg_alt_argb"}) {
            const auto value = item.find(name);
            if (value != item.end() &&
                (!value->is_number_unsigned() || value->get<uint64_t>() > UINT32_MAX))
                return false;
        }
        if (const auto value = item.find("header_bg_argb"); value != item.end())
            column.header_bg_argb = value->get<uint32_t>();
        if (const auto value = item.find("header_fg_argb"); value != item.end())
            column.header_fg_argb = value->get<uint32_t>();
        if (const auto value = item.find("cell_fg_argb"); value != item.end())
            column.cell_fg_argb = value->get<uint32_t>();
        if (const auto value = item.find("cell_bg_alt_argb"); value != item.end())
            column.cell_bg_alt_argb = value->get<uint32_t>();
    }
    return true;
}

sao_sdk_status_t parse_props(const SaoSdkWidgetSpec& spec, Json* out) {
    if (out == nullptr || (spec.props_json_utf8 == nullptr && spec.props_len != 0))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (spec.props_json_utf8 == nullptr) {
        *out = Json::object();
        return SAO_SDK_OK;
    }
    if (spec.props_len == 0 || spec.props_len > kMaximumTypedPropsBytes ||
        !valid_utf8(std::string_view(reinterpret_cast<const char*>(spec.props_json_utf8),
                                     spec.props_len))) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    try {
        const auto* begin = reinterpret_cast<const char*>(spec.props_json_utf8);
        const auto* end = begin + spec.props_len;
        bounded_typed_props_sax sax;
        if (!Json::sax_parse(begin, end, &sax)) return SAO_SDK_ERR_INVALID_ARGUMENT;
        *out = Json::parse(begin, end);
        return out->is_object() ? SAO_SDK_OK : SAO_SDK_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
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

bool native_widget_update_state_uncertain(sao_status_t status) noexcept {
    return status == SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED ||
           status == SAO_STATUS_ERR_UNKNOWN;
}

sao_sdk_status_t remove_candidate_nodes_reverse(
    sao_ui_panel_body_handle_t body,
    const std::vector<sao_ui_layout_node_handle_t>& candidate_nodes) {
    sao_sdk_status_t first_failure = SAO_SDK_OK;
    for (auto it = candidate_nodes.rbegin(); it != candidate_nodes.rend(); ++it) {
        if (*it == nullptr)
            continue;
        SaoUiBodyMutation mutation{};
        mutation.kind = SAO_UI_BODY_REMOVE_NODE;
        mutation.target = *it;
        const auto status = static_cast<sao_sdk_status_t>(
            sao_ui_panel_update_body(body, &mutation, 1));
        if (first_failure == SAO_SDK_OK && status != SAO_SDK_OK)
            first_failure = status;
    }
    return first_failure;
}

sao_sdk_status_t restore_widget_node(sao_ui_panel_body_handle_t body, const WidgetEntry& widget,
                                     sao_ui_layout_node_handle_t* out_node) {
    if (body == nullptr || widget.ui_widget == nullptr || out_node == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_node = nullptr;
    SaoUiLayoutSpec layout = widget.layout_spec;
    SaoUiBodyMutation mutations[2]{};
    mutations[0].kind = SAO_UI_BODY_ADD_WIDGET;
    mutations[0].widget = widget.ui_widget;
    mutations[0].spec = &layout;
    mutations[0].out_new_node = out_node;
    size_t mutation_count = 1;
    if (!widget.props_json.empty()) {
        mutations[1].kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
        mutations[1].widget = widget.ui_widget;
        mutations[1].props_json_utf8 =
            reinterpret_cast<const uint8_t*>(widget.props_json.data());
        mutations[1].props_len = widget.props_json.size();
        mutation_count = 2;
    }
    return static_cast<sao_sdk_status_t>(
        sao_ui_panel_update_body(body, mutations, mutation_count));
}

sao_sdk_status_t restore_legacy_body(
    sao_ui_panel_body_handle_t body, const std::vector<WidgetEntry>& old_widgets,
    const std::vector<sao_ui_widget_handle_t>& old_placeholders,
    std::vector<sao_ui_layout_node_handle_t>* out_widget_nodes,
    std::vector<sao_ui_layout_node_handle_t>* out_placeholder_nodes) {
    if (body == nullptr || out_widget_nodes == nullptr || out_placeholder_nodes == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    out_widget_nodes->assign(old_widgets.size(), nullptr);
    out_placeholder_nodes->assign(old_placeholders.size(), nullptr);
    if (old_widgets.empty() && old_placeholders.empty())
        return SAO_SDK_OK;

    static constexpr char kCanvasProps[] = R"({"active":true})";
    std::vector<SaoUiLayoutSpec> placeholder_layouts(old_placeholders.size());
    for (auto& layout : placeholder_layouts) {
        layout.fixed_width_px = 640;
        layout.fixed_height_px = 360;
        layout.hit_testable = true;
    }
    std::vector<SaoUiBodyMutation> mutations;
    mutations.reserve(old_widgets.size() * 2U + old_placeholders.size() * 2U);
    for (size_t index = 0; index < old_widgets.size(); ++index) {
        const auto& widget = old_widgets[index];
        SaoUiBodyMutation add{};
        add.kind = SAO_UI_BODY_ADD_WIDGET;
        add.widget = widget.ui_widget;
        add.spec = &widget.layout_spec;
        add.out_new_node = &(*out_widget_nodes)[index];
        mutations.push_back(add);
        if (!widget.props_json.empty()) {
            SaoUiBodyMutation props{};
            props.kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
            props.widget = widget.ui_widget;
            props.props_json_utf8 =
                reinterpret_cast<const uint8_t*>(widget.props_json.data());
            props.props_len = widget.props_json.size();
            mutations.push_back(props);
        }
    }
    for (size_t index = 0; index < old_placeholders.size(); ++index) {
        SaoUiBodyMutation add{};
        add.kind = SAO_UI_BODY_ADD_WIDGET;
        add.widget = old_placeholders[index];
        add.spec = &placeholder_layouts[index];
        add.out_new_node = &(*out_placeholder_nodes)[index];
        mutations.push_back(add);
        SaoUiBodyMutation props{};
        props.kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
        props.widget = old_placeholders[index];
        props.props_json_utf8 = reinterpret_cast<const uint8_t*>(kCanvasProps);
        props.props_len = sizeof(kCanvasProps) - 1U;
        mutations.push_back(props);
    }
    return static_cast<sao_sdk_status_t>(sao_ui_panel_update_body(
        body, mutations.data(), mutations.size()));
}

void quarantine_panel_cleanup(ContextState* state, sao_sdk_ui_panel_t panel,
                              sao_ui_panel_handle_t native_panel,
                              sao_ui_panel_body_handle_t body, PanelEntry residual) {
    if (state == nullptr)
        return;
    residual.cleanup_pending = true;
    residual.unregistering = true;
    std::lock_guard<std::mutex> lock(state->mu);
    const auto found = state->panels.find(panel);
    if (found != state->panels.end() && found->second.ui_body == body) {
        found->second.cleanup_pending = true;
        found->second.unregistering = true;
        for (auto& widget : residual.widgets)
            found->second.widgets.push_back(std::move(widget));
        for (auto canvas : residual.canvases)
            found->second.canvases.push_back(canvas);
        for (auto placeholder : residual.canvas_placeholders)
            found->second.canvas_placeholders.push_back(placeholder);
        for (auto node : residual.canvas_placeholder_nodes)
            found->second.canvas_placeholder_nodes.push_back(node);
        return;
    }
    residual.sdk_handle = panel;
    residual.ui_panel = native_panel == nullptr ? reinterpret_cast<sao_ui_panel_handle_t>(panel)
                                                : native_panel;
    residual.ui_body = body;
    state->panel_cleanup_pending.push_back(std::move(residual));
}

void quarantine_widget_cleanup(ContextState* state, sao_sdk_ui_panel_t panel,
                               sao_ui_panel_body_handle_t body, WidgetEntry entry) {
    if (state == nullptr)
        return;
    entry.cleanup_pending = true;
    std::lock_guard<std::mutex> lock(state->mu);
    const auto found = state->panels.find(panel);
    if (found != state->panels.end() && found->second.ui_body == body) {
        found->second.cleanup_pending = true;
        found->second.unregistering = true;
        const auto existing = std::find_if(
            found->second.widgets.begin(), found->second.widgets.end(),
            [&](const WidgetEntry& candidate) { return candidate.sdk_handle == entry.sdk_handle; });
        if (existing == found->second.widgets.end()) {
            found->second.widgets.push_back(std::move(entry));
        } else {
            existing->cleanup_pending = true;
            if (!entry.degraded_new_props_json.empty())
                existing->degraded_new_props_json = std::move(entry.degraded_new_props_json);
        }
        return;
    }
    PanelEntry pending;
    pending.sdk_handle = panel;
    pending.ui_panel = reinterpret_cast<sao_ui_panel_handle_t>(panel);
    pending.ui_body = body;
    pending.cleanup_pending = true;
    pending.unregistering = true;
    pending.widgets.push_back(std::move(entry));
    state->panel_cleanup_pending.push_back(std::move(pending));
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
    Json result = props;
    if (kind == SAO_SDK_UI_WIDGET_RADIO) {
        result = Json::object();
        if (props.contains("selected")) result["selected"] = props.at("selected");
    } else if (kind == SAO_SDK_UI_WIDGET_SLIDER) {
        result = Json::object();
        if (props.contains("value")) result["value"] = props.at("value");
        if (props.contains("keyboard_nudge")) result["keyboard_nudge"] = props.at("keyboard_nudge");
    } else if (kind == SAO_SDK_UI_WIDGET_DROPDOWN) {
        result = Json::object();
        if (props.contains("selected_id")) result["selected_id"] = props.at("selected_id");
    } else if (kind == SAO_SDK_UI_WIDGET_METRIC) {
        result = Json::object();
        if (props.contains("value")) result["value"] = props.at("value");
    } else if (kind == SAO_SDK_UI_WIDGET_EMPTY_STATE) {
        result = Json::object();
        if (props.contains("detail")) result["detail"] = props.at("detail");
    } else if (kind == SAO_SDK_UI_WIDGET_STATUS_BADGE) {
        result = Json::object();
        if (props.contains("text")) result["text"] = props.at("text");
    } else if (kind == SAO_SDK_UI_WIDGET_PROGRESS_BAR) {
        result = Json::object();
        for (const char* key : {"value", "max_value", "style", "trail_argb", "trail_lag_ms", "gap_px", "segment_pulse_ms"})
            if (props.contains(key)) result[key] = props.at(key);
    } else if (kind == SAO_SDK_UI_WIDGET_TABLE) {
        result.erase("columns");
    }
    return result;
}

sao_sdk_status_t create_widget_for_kind(const SaoSdkWidgetSpec& spec, const Json& props, CreatedWidget* out) {
    if (spec.kind == SAO_SDK_UI_WIDGET_SCROLLBAR && !validate_scrollbar_props(props))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (out == nullptr || !valid_widget_kind(spec.kind)) return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out = {};
    sao_status_t status = SAO_STATUS_OK;
    switch (spec.kind) {
    case SAO_SDK_UI_WIDGET_PROGRESS_BAR: {
        SaoUiProgressBarSpec native{};
        native.value = std::isfinite(spec.value) ? spec.value : 0.0F;
        native.max_value = spec.max_value > 0.0F ? spec.max_value : 1.0F;
        native.style = SAO_UI_PROGRESS_FLAT;
        status = sao_ui_progress_bar_create(nullptr, &native, &out->widget);
        break;
    }
    case SAO_SDK_UI_WIDGET_STATUS_BADGE: {
        SaoUiStatusBadgeSpec native{};
        native.text_utf8 = spec.text_utf8;
        native.font_size_px = 12;
        status = sao_ui_status_badge_create(nullptr, &native, &out->widget);
        break;
    }
    case SAO_SDK_UI_WIDGET_TABLE: {
        SaoUiTableSpec native{};
        native.row_height_px = 22;
        native.header_height_px = 24;
        native.show_header = true;
        std::vector<std::string> keys;
        std::vector<std::string> titles;
        std::vector<SaoUiTableColumn> columns;
        if (!parse_table_columns(props, &keys, &titles, &columns))
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        native.columns = columns.empty() ? nullptr : columns.data();
        native.column_count = columns.size();
        status = sao_ui_table_create(nullptr, &native, &out->widget);
        break;
    }
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
        switch (spec.kind) { case SAO_SDK_UI_WIDGET_LABEL: native_kind = SAO_UI_WIDGET_TEXT; break; case SAO_SDK_UI_WIDGET_BUTTON: native_kind = SAO_UI_WIDGET_ACTION_BUTTON; break; case SAO_SDK_UI_WIDGET_TEXT_FIELD: native_kind = SAO_UI_WIDGET_INPUT; break; case SAO_SDK_UI_WIDGET_CHECKBOX: native_kind = SAO_UI_WIDGET_CHECKBOX; break; case SAO_SDK_UI_WIDGET_DIVIDER: native_kind = SAO_UI_WIDGET_DIVIDER; break; case SAO_SDK_UI_WIDGET_ICON: native_kind = SAO_UI_WIDGET_ICON; break; case SAO_SDK_UI_WIDGET_SCROLLBAR: native_kind = SAO_UI_WIDGET_SCROLLBAR; break; case SAO_SDK_UI_WIDGET_ROUNDED_PANEL: break; default: return SAO_SDK_ERR_INVALID_ARGUMENT; }
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
    panel.canvas_placeholder_nodes.clear();
}

void destroy_panel_resources(PanelEntry& panel) {
    clear_legacy_canvases(panel);
    for (const auto& widget : panel.widgets)
        destroy_widget_for_kind(widget.kind, widget.ui_widget, widget.script_canvas);
    panel.widgets.clear();
}

sao_sdk_status_t materialize_legacy_canvas(const uint8_t* spec_json_utf8, size_t spec_len,
                                           LegacyCanvasResources* out_resources) {
    if (out_resources == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_resources = {};
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

    out_resources->canvases.push_back(canvas);
    out_resources->placeholders.push_back(placeholder);
    return SAO_SDK_OK;
}

// ─── UI table vtable functions ───────────────────────────────────────

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
    if (state == nullptr || descriptor == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (!sdk_struct_size_valid(descriptor->struct_size, SAO_SDK_PANEL_DESCRIPTOR_REQUIRED_SIZE,
                               sizeof(SaoSdkPanelDescriptor)))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (descriptor->panel_id_utf8 == nullptr || descriptor->panel_id_utf8[0] == '\0')
        return SAO_SDK_ERR_INVALID_ARGUMENT;

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
    full.initial_opacity = descriptor->initial_opacity == 0.0F ? 1.0F : descriptor->initial_opacity;

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

    auto& runtime = SharedRuntime::instance();
    sao_ui_panel_handle_t native_panel = nullptr;
    sao_ui_panel_body_handle_t native_body = nullptr;
    const auto register_status = invoke_callback_barrier([&]() -> sao_sdk_status_t {
        return static_cast<sao_sdk_status_t>(sao_ui_panel_register(
            runtime.compositor, &full, &native_panel, &native_body));
    });
    if (register_status != SAO_SDK_OK) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->panel_cleanup_pending.erase(pending);
        if (register_status == SAO_STATUS_ERR_ALREADY_EXISTS)
            return SAO_SDK_ERR_ALREADY_EXISTS;
        return register_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        pending->sdk_handle = reinterpret_cast<sao_sdk_ui_panel_t>(native_panel);
        pending->ui_panel = native_panel;
        pending->ui_body = native_body;
    }
    if (native_panel == nullptr || native_body == nullptr) {
        const auto rollback = native_panel == nullptr ? SAO_SDK_OK : unregister_native_panel(native_panel);
        if (rollback == SAO_SDK_OK) {
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
        std::lock_guard<std::mutex> lock(state->mu);
        const auto handle = pending->sdk_handle;
        if (state->panels.find(handle) != state->panels.end())
            insertion_status = SAO_SDK_ERR_ALREADY_EXISTS;
        else {
            state->panels.emplace(handle, *pending);
            state->panel_cleanup_pending.erase(pending);
        }
    } catch (...) {
        insertion_status = SAO_SDK_ERR_INTERNAL;
    }
    if (insertion_status != SAO_SDK_OK) {
        const auto rollback = unregister_native_panel(native_panel);
        if (rollback == SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            state->panel_cleanup_pending.erase(pending);
        }
        return rollback == SAO_SDK_OK ? insertion_status : rollback;
    }
    *out_panel = reinterpret_cast<sao_sdk_ui_panel_t>(native_panel);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_set_panel_spec(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                const uint8_t* spec_json_utf8, size_t spec_len);

sao_sdk_status_t SAO_SDK_CALL ui_register_panel(void* ctx_impl, const char* panel_id_utf8,
                                                const char* title_utf8,
                                                const uint8_t* initial_spec_json_utf8,
                                                size_t spec_len,
                                                sao_sdk_panel_action_callback_t action_cb,
                                                void* action_user_data,
                                                sao_sdk_ui_panel_t* out_panel) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease) return lease.status();
    if (out_panel == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (initial_spec_json_utf8 == nullptr && spec_len != 0) return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_panel = nullptr;
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
    descriptor.initial_opacity = 1.0F;
    descriptor.struct_size = sizeof(descriptor);
    const auto status = ui_register_ui_panel(ctx_impl, &descriptor, out_panel);
    if (status != SAO_SDK_OK) return status;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    sao_ui_panel_handle_t native_panel = nullptr;
    std::shared_ptr<std::mutex> native_mutex;
    std::shared_ptr<LegacyActionBridge> bridge;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(*out_panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_HANDLE_INVALID;
        native_panel = it->second.ui_panel;
        native_mutex = it->second.native_mutation_mutex;
        if (action_cb != nullptr) {
            bridge = std::make_shared<LegacyActionBridge>();
            bridge->callback_gate = state->callback_gate;
            bridge->callback = action_cb;
            bridge->user_data = action_user_data;
        }
        it->second.legacy_action_cb = action_cb;
        it->second.legacy_action_user_data = action_user_data;
        it->second.legacy_action_bridge = bridge;
        it->second.ui_mode = PanelEntry::UiMode::legacy_spec;
    }
    if (bridge != nullptr) {
        sao_sdk_status_t action_status = SAO_SDK_OK;
        {
            std::lock_guard<std::mutex> native_lock(*native_mutex);
            action_status = invoke_callback_barrier([&] {
                return static_cast<sao_sdk_status_t>(sao_ui_panel_set_action_handler(
                    native_panel, &legacy_action_bridge_callback, bridge.get()));
            });
        }
        if (action_status != SAO_SDK_OK) {
            (void)ui_unregister_ui_panel(ctx_impl, *out_panel);
            *out_panel = nullptr;
            return action_status;
        }
    }
    if (initial_spec_json_utf8 != nullptr || spec_len != 0) {
        const auto spec_status = ui_set_panel_spec(ctx_impl, *out_panel,
                                                   initial_spec_json_utf8, spec_len);
        if (spec_status != SAO_SDK_OK) {
            (void)ui_unregister_ui_panel(ctx_impl, *out_panel);
            *out_panel = nullptr;
            return spec_status;
        }
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_set_panel_spec(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                const uint8_t* spec_json_utf8, size_t spec_len) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease) return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (spec_json_utf8 == nullptr && spec_len != 0) return SAO_SDK_ERR_INVALID_ARGUMENT;
    sao_ui_panel_handle_t native_panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    std::shared_ptr<std::mutex> native_mutex;
    std::vector<WidgetEntry> old_widgets;
    std::vector<sao_ui_widget_handle_t> old_placeholders;
    std::vector<sao_ui_layout_node_handle_t> old_placeholder_nodes;
    std::string old_spec;
    PanelEntry::UiMode old_mode = PanelEntry::UiMode::unspecified;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        if (it->second.unregistering) return SAO_SDK_ERR_BUSY;
        native_panel = it->second.ui_panel;
        body = it->second.ui_body;
        native_mutex = it->second.native_mutation_mutex;
        old_widgets = it->second.widgets;
        old_placeholders = it->second.canvas_placeholders;
        old_placeholder_nodes = it->second.canvas_placeholder_nodes;
        old_spec = it->second.legacy_spec_json;
        old_mode = it->second.ui_mode;
    }
    std::lock_guard<std::mutex> native_lock(*native_mutex);
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        if (!panel_native_handles_current(it->second, native_panel, body) ||
            it->second.ui_mode != old_mode ||
            it->second.canvas_placeholders != old_placeholders ||
            it->second.canvas_placeholder_nodes != old_placeholder_nodes)
            return it->second.unregistering ? SAO_SDK_ERR_BUSY : SAO_SDK_ERR_HANDLE_INVALID;
        for (const auto& old_widget : old_widgets)
            if (!widget_entry_current(it->second, old_widget))
                return SAO_SDK_ERR_HANDLE_INVALID;
    }
    if (old_placeholders.size() != old_placeholder_nodes.size()) return SAO_SDK_ERR_INTERNAL;
    LegacyCanvasResources replacement;
    const auto stage_status = materialize_legacy_canvas(spec_json_utf8, spec_len, &replacement);
    if (stage_status != SAO_SDK_OK) return stage_status;
    std::string candidate_spec;
    if (spec_json_utf8 != nullptr && spec_len != 0)
        candidate_spec.assign(reinterpret_cast<const char*>(spec_json_utf8), spec_len);
    std::vector<SaoUiLayoutSpec> layouts(replacement.placeholders.size());
    for (auto& layout : layouts) {
        layout.fixed_width_px = 640;
        layout.fixed_height_px = 360;
        layout.hit_testable = true;
    }
    std::vector<sao_ui_layout_node_handle_t> replacement_nodes(replacement.placeholders.size());
    std::vector<SaoUiBodyMutation> mutations;
    mutations.reserve(old_widgets.size() + old_placeholders.size() + replacement.placeholders.size());
    for (const auto& old_widget : old_widgets) {
        SaoUiBodyMutation mutation{};
        mutation.kind = SAO_UI_BODY_REMOVE_NODE;
        mutation.target = old_widget.layout_node;
        mutation.widget = old_widget.ui_widget;
        mutations.push_back(mutation);
    }
    for (size_t index = 0; index < old_placeholders.size(); ++index) {
        SaoUiBodyMutation mutation{};
        mutation.kind = SAO_UI_BODY_REMOVE_NODE;
        mutation.target = old_placeholder_nodes[index];
        mutation.widget = old_placeholders[index];
        mutations.push_back(mutation);
    }
    for (size_t index = 0; index < replacement.placeholders.size(); ++index) {
        SaoUiBodyMutation mutation{};
        mutation.kind = SAO_UI_BODY_ADD_WIDGET;
        mutation.widget = replacement.placeholders[index];
        mutation.spec = &layouts[index];
        mutation.out_new_node = &replacement_nodes[index];
        mutations.push_back(mutation);
    }
    const auto native_status = invoke_callback_barrier([&] {
        return static_cast<sao_sdk_status_t>(sao_ui_panel_set_spec(
            native_panel, spec_json_utf8, spec_len));
    });
    if (native_status != SAO_SDK_OK) {
        for (const auto canvas : replacement.canvases) sao_ui_script_canvas_destroy(canvas);
        for (const auto placeholder : replacement.placeholders) sao_ui_widget_destroy(placeholder);
        return native_status;
    }
    const bool needs_body_sync = !old_widgets.empty() || !old_placeholders.empty() || !replacement.placeholders.empty();
    const auto body_status = needs_body_sync
                                 ? sao_ui_panel_update_body(
                                       body, mutations.empty() ? nullptr : mutations.data(),
                                       mutations.size())
                                 : SAO_STATUS_OK;
    if (body_status != SAO_STATUS_OK) {
        const auto candidate_remove = remove_candidate_nodes_reverse(body, replacement_nodes);
        const auto rollback_spec = invoke_callback_barrier([&] {
            return static_cast<sao_sdk_status_t>(sao_ui_panel_set_spec(
                native_panel, old_spec.empty() ? nullptr
                                                : reinterpret_cast<const uint8_t*>(old_spec.data()),
                old_spec.size()));
        });
        std::vector<sao_ui_layout_node_handle_t> restored_widget_nodes;
        std::vector<sao_ui_layout_node_handle_t> restored_placeholder_nodes;
        const auto restore_body_status = restore_legacy_body(
            body, old_widgets, old_placeholders, &restored_widget_nodes,
            &restored_placeholder_nodes);
        const auto rollback_body = rollback_spec != SAO_SDK_OK
                                       ? rollback_spec
                                       : restore_body_status;
        if (candidate_remove == SAO_SDK_OK && rollback_body == SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            const auto it = state->panels.find(panel);
            if (it != state->panels.end() && it->second.ui_mode == old_mode) {
                for (size_t index = 0; index < old_widgets.size(); ++index) {
                    const auto found = std::find_if(
                        it->second.widgets.begin(), it->second.widgets.end(),
                        [&](const WidgetEntry& item) {
                            return item.sdk_handle == old_widgets[index].sdk_handle;
                        });
                    if (found != it->second.widgets.end())
                        found->layout_node = restored_widget_nodes[index];
                }
                it->second.canvas_placeholder_nodes = restored_placeholder_nodes;
            }
            for (const auto canvas : replacement.canvases)
                sao_ui_script_canvas_destroy(canvas);
            for (const auto placeholder : replacement.placeholders)
                sao_ui_widget_destroy(placeholder);
            return static_cast<sao_sdk_status_t>(body_status);
        }
        PanelEntry residual;
        residual.canvases = std::move(replacement.canvases);
        residual.canvas_placeholders = std::move(replacement.placeholders);
        residual.canvas_placeholder_nodes = std::move(replacement_nodes);
        quarantine_panel_cleanup(state, panel, native_panel, body, std::move(residual));
        return candidate_remove != SAO_SDK_OK ? candidate_remove : rollback_body;
    }
    sao_sdk_status_t final_status = SAO_SDK_OK;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end())
            final_status = SAO_SDK_ERR_NOT_FOUND;
        else if (!panel_native_handles_current(it->second, native_panel, body) ||
                 it->second.ui_mode != old_mode)
            final_status = it->second.unregistering ? SAO_SDK_ERR_BUSY : SAO_SDK_ERR_HANDLE_INVALID;
    }
    if (final_status != SAO_SDK_OK) {
        const auto candidate_remove = remove_candidate_nodes_reverse(body, replacement_nodes);
        const auto rollback_spec = invoke_callback_barrier([&] {
            return static_cast<sao_sdk_status_t>(sao_ui_panel_set_spec(
                native_panel, old_spec.empty() ? nullptr
                                                : reinterpret_cast<const uint8_t*>(old_spec.data()),
                old_spec.size()));
        });
        std::vector<sao_ui_layout_node_handle_t> restored_widget_nodes;
        std::vector<sao_ui_layout_node_handle_t> restored_placeholder_nodes;
        const auto restore_body_status = restore_legacy_body(
            body, old_widgets, old_placeholders, &restored_widget_nodes,
            &restored_placeholder_nodes);
        const auto rollback_body = rollback_spec != SAO_SDK_OK
                                       ? rollback_spec
                                       : restore_body_status;
        if (candidate_remove == SAO_SDK_OK && rollback_body == SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            const auto it = state->panels.find(panel);
            if (it != state->panels.end() && it->second.ui_mode == old_mode) {
                for (size_t index = 0; index < old_widgets.size(); ++index) {
                    const auto found = std::find_if(
                        it->second.widgets.begin(), it->second.widgets.end(),
                        [&](const WidgetEntry& item) {
                            return item.sdk_handle == old_widgets[index].sdk_handle;
                        });
                    if (found != it->second.widgets.end())
                        found->layout_node = restored_widget_nodes[index];
                }
                it->second.canvas_placeholder_nodes = restored_placeholder_nodes;
            }
            for (const auto canvas : replacement.canvases)
                sao_ui_script_canvas_destroy(canvas);
            for (const auto placeholder : replacement.placeholders)
                sao_ui_widget_destroy(placeholder);
            return final_status;
        }
        PanelEntry residual;
        residual.canvases = std::move(replacement.canvases);
        residual.canvas_placeholders = std::move(replacement.placeholders);
        residual.canvas_placeholder_nodes = std::move(replacement_nodes);
        quarantine_panel_cleanup(state, panel, native_panel, body, std::move(residual));
        return candidate_remove != SAO_SDK_OK ? candidate_remove : rollback_body;
    }

    PanelEntry retired;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end() ||
            !panel_native_handles_current(it->second, native_panel, body) ||
            it->second.ui_mode != old_mode)
            return SAO_SDK_ERR_BUSY;
        retired.widgets = std::move(it->second.widgets);
        retired.canvases = std::move(it->second.canvases);
        retired.canvas_placeholders = std::move(it->second.canvas_placeholders);
        retired.canvas_placeholder_nodes = std::move(it->second.canvas_placeholder_nodes);
        it->second.widgets.clear();
        it->second.canvases = std::move(replacement.canvases);
        it->second.canvas_placeholders = std::move(replacement.placeholders);
        it->second.canvas_placeholder_nodes = std::move(replacement_nodes);
        it->second.pending_widget_ids.clear();
        it->second.legacy_spec_json = std::move(candidate_spec);
        it->second.ui_mode = PanelEntry::UiMode::legacy_spec;
    }
    destroy_panel_resources(retired);
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
    std::shared_ptr<std::mutex> native_mutex;
    std::shared_ptr<LegacyActionBridge> bridge;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return SAO_SDK_ERR_NOT_FOUND;
        if (it->second.unregistering)
            return SAO_SDK_ERR_BUSY;
        native_panel = it->second.ui_panel;
        native_mutex = it->second.native_mutation_mutex;
        bridge = it->second.legacy_action_bridge;
    }

    std::lock_guard<std::mutex> native_lock(*native_mutex);
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        if (it->second.unregistering || it->second.ui_panel != native_panel ||
            it->second.native_mutation_mutex != native_mutex)
            return SAO_SDK_ERR_BUSY;
        it->second.unregistering = true;
    }
    const auto clear_action_status = invoke_callback_barrier([&] {
        return static_cast<sao_sdk_status_t>(
            sao_ui_panel_set_action_handler(native_panel, nullptr, nullptr));
    });
    if (clear_action_status != SAO_SDK_OK) {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto found = state->panels.find(panel);
        if (found != state->panels.end())
            found->second.unregistering = false;
        return clear_action_status;
    }
    const auto unregister_status = unregister_native_panel(native_panel);
    if (unregister_status != SAO_SDK_OK) {
        if (bridge != nullptr) {
            (void)invoke_callback_barrier([&] {
                return static_cast<sao_sdk_status_t>(sao_ui_panel_set_action_handler(
                    native_panel, &legacy_action_bridge_callback, bridge.get()));
            });
        }
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
            if (overlay->second == panel)
                overlay = state->overlays.erase(overlay);
            else
                ++overlay;
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
    if (!sdk_struct_size_valid(widget_spec->struct_size, SAO_SDK_WIDGET_SPEC_REQUIRED_SIZE,
                               sizeof(SaoSdkWidgetSpec)) ||
        !valid_optional_utf8_c_string(widget_spec->widget_id_utf8) ||
        !valid_optional_utf8_c_string(widget_spec->text_utf8))
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
    const Json wire_props = uses_generic_sdk_props(widget_spec->kind)
                                ? generic_props(*widget_spec, parsed)
                                : initial_typed_props(widget_spec->kind, parsed);
    std::string wire_json;
    if (!serialize_validated_typed_props(wire_props, wire_json)) {
        destroy_widget_for_kind(widget_spec->kind, created.widget, created.script_canvas);
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    sao_ui_panel_body_handle_t body = nullptr;
    std::shared_ptr<std::mutex> native_mutex;
    sao_sdk_status_t add_preflight_status = SAO_SDK_OK;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end())
            add_preflight_status = SAO_SDK_ERR_NOT_FOUND;
        else if (it->second.unregistering || it->second.ui_mode == PanelEntry::UiMode::legacy_spec)
            add_preflight_status = SAO_SDK_ERR_BUSY;
        else if (widget_spec->widget_id_utf8 != nullptr && widget_spec->widget_id_utf8[0] != '\0') {
            for (const auto& existing : it->second.widgets)
                if (existing.widget_id == widget_spec->widget_id_utf8)
                    add_preflight_status = SAO_SDK_ERR_ALREADY_EXISTS;
        }
        if (add_preflight_status == SAO_SDK_OK) {
            body = it->second.ui_body;
            native_mutex = it->second.native_mutation_mutex;
        }
    }
    if (add_preflight_status != SAO_SDK_OK) {
        destroy_widget_for_kind(widget_spec->kind, created.widget, created.script_canvas);
        return add_preflight_status;
    }
    pause_context_api_test_point(ContextApiTestPoint::panel_operation_unlocked);
    std::lock_guard<std::mutex> native_lock(*native_mutex);
    sao_sdk_status_t add_locked_status = SAO_SDK_OK;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end())
            add_locked_status = SAO_SDK_ERR_NOT_FOUND;
        else if (it->second.unregistering || it->second.ui_mode == PanelEntry::UiMode::legacy_spec ||
                 !panel_native_handles_current(it->second, reinterpret_cast<sao_ui_panel_handle_t>(panel), body))
            add_locked_status = it->second.unregistering ? SAO_SDK_ERR_BUSY : SAO_SDK_ERR_HANDLE_INVALID;
        else
            body = it->second.ui_body;
    }
    if (add_locked_status != SAO_SDK_OK) {
        destroy_widget_for_kind(widget_spec->kind, created.widget, created.script_canvas);
        return add_locked_status;
    }

    SaoUiLayoutSpec layout{};
    layout.fixed_width_px = widget_spec->width_px;
    layout.fixed_height_px = widget_spec->height_px;
    layout.absolute_x_px = widget_spec->x_px;
    layout.absolute_y_px = widget_spec->y_px;
    layout.absolute_z = widget_spec->z_order;
    layout.hit_testable = true;
    WidgetEntry entry;
    entry.sdk_handle = allocate_widget_token();
    entry.ui_widget = created.widget;
    entry.script_canvas = created.script_canvas;
    entry.widget_id = widget_spec->widget_id_utf8 == nullptr ? std::string() : widget_spec->widget_id_utf8;
    entry.props_json = wire_json;
    entry.kind = widget_spec->kind;
    entry.layout_spec = layout;
    SaoUiBodyMutation mutations[2]{};
    mutations[0].kind = SAO_UI_BODY_ADD_WIDGET;
    mutations[0].widget = created.widget;
    mutations[0].spec = &layout;
    mutations[0].out_new_node = &entry.layout_node;
    mutations[1].kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    mutations[1].widget = created.widget;
    mutations[1].props_json_utf8 = reinterpret_cast<const uint8_t*>(wire_json.data());
    mutations[1].props_len = wire_json.size();
    const auto batch_status = sao_ui_panel_update_body(body, mutations, 2);
    if (batch_status != SAO_STATUS_OK) {
        destroy_widget_for_kind(widget_spec->kind, created.widget, created.script_canvas);
        return static_cast<sao_sdk_status_t>(batch_status);
    }

    sao_sdk_status_t commit_status = SAO_SDK_OK;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end() || it->second.unregistering ||
            it->second.ui_mode == PanelEntry::UiMode::legacy_spec) {
            commit_status = it == state->panels.end() ? SAO_SDK_ERR_NOT_FOUND : SAO_SDK_ERR_BUSY;
        } else {
            try {
                it->second.ui_mode = PanelEntry::UiMode::typed;
                it->second.widgets.push_back(entry);
            } catch (...) {
                commit_status = SAO_SDK_ERR_INTERNAL;
            }
        }
    }
    if (commit_status != SAO_SDK_OK) {
        SaoUiBodyMutation rollback{};
        rollback.kind = SAO_UI_BODY_REMOVE_NODE;
        rollback.target = entry.layout_node;
        const auto rollback_status =
            static_cast<sao_sdk_status_t>(sao_ui_panel_update_body(body, &rollback, 1));
        if (rollback_status == SAO_SDK_OK) {
            destroy_widget_for_kind(widget_spec->kind, created.widget, created.script_canvas);
        } else {
            quarantine_widget_cleanup(state, panel, body, entry);
        }
        return commit_status;
    }
    if (g_fail_next_widget_state_insertion.exchange(false)) {
        WidgetEntry residual;
        {
            std::lock_guard<std::mutex> lock(state->mu);
            const auto it = state->panels.find(panel);
            if (it == state->panels.end() || it->second.widgets.empty())
                return SAO_SDK_ERR_INTERNAL;
            residual = it->second.widgets.back();
        }
        PanelEntry mutation_view;
        mutation_view.ui_body = body;
        const auto remove_status = remove_widget_node(mutation_view, residual);
        if (remove_status == SAO_SDK_OK) {
            destroy_widget_for_kind(residual.kind, residual.ui_widget, residual.script_canvas);
            std::lock_guard<std::mutex> lock(state->mu);
            const auto it = state->panels.find(panel);
            if (it != state->panels.end()) {
                const auto found = std::find_if(it->second.widgets.begin(), it->second.widgets.end(),
                                                [&](const WidgetEntry& item) {
                                                    return item.sdk_handle == residual.sdk_handle;
                                                });
                if (found != it->second.widgets.end())
                    it->second.widgets.erase(found);
            }
        } else {
            quarantine_widget_cleanup(state, panel, body, residual);
        }
        return SAO_SDK_ERR_INTERNAL;
    }
    if (out_widget != nullptr)
        *out_widget = entry.sdk_handle;
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
    if (!sdk_struct_size_valid(widget_spec->struct_size, SAO_SDK_WIDGET_SPEC_REQUIRED_SIZE,
                               sizeof(SaoSdkWidgetSpec)) ||
        !valid_optional_utf8_c_string(widget_spec->widget_id_utf8) ||
        !valid_optional_utf8_c_string(widget_spec->text_utf8))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    Json parsed;
    const auto parse_status = parse_props(*widget_spec, &parsed);
    if (parse_status != SAO_SDK_OK || !valid_widget_kind(widget_spec->kind))
        return parse_status != SAO_SDK_OK ? parse_status : SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget_spec->kind == SAO_SDK_UI_WIDGET_SCRIPTABLE_CANVAS ||
        (widget_spec->kind == SAO_SDK_UI_WIDGET_TABLE && parsed.contains("columns")))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget_spec->kind == SAO_SDK_UI_WIDGET_SCROLLBAR && !validate_scrollbar_props(parsed))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    const Json wire_props = uses_generic_sdk_props(widget_spec->kind)
                                ? generic_props(*widget_spec, parsed)
                                : parsed;
    std::string wire_json;
    if (!serialize_validated_typed_props(wire_props, wire_json)) return SAO_SDK_ERR_INVALID_ARGUMENT;

    sao_ui_panel_body_handle_t body = nullptr;
    std::shared_ptr<std::mutex> native_mutex;
    WidgetEntry target;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        if (it->second.unregistering || it->second.ui_mode == PanelEntry::UiMode::legacy_spec)
            return SAO_SDK_ERR_BUSY;
        const auto found = std::find_if(it->second.widgets.begin(), it->second.widgets.end(),
                                        [&](const WidgetEntry& item) { return item.sdk_handle == widget; });
        if (found == it->second.widgets.end()) return SAO_SDK_ERR_NOT_FOUND;
        if (found->kind != widget_spec->kind) return SAO_SDK_ERR_INVALID_ARGUMENT;
        target = *found;
        body = it->second.ui_body;
        native_mutex = it->second.native_mutation_mutex;
    }
    std::lock_guard<std::mutex> native_lock(*native_mutex);
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        if (!panel_native_handles_current(it->second, reinterpret_cast<sao_ui_panel_handle_t>(panel), body) ||
            it->second.ui_mode != PanelEntry::UiMode::typed || !widget_entry_current(it->second, target))
            return it->second.unregistering ? SAO_SDK_ERR_BUSY : SAO_SDK_ERR_HANDLE_INVALID;
    }
    SaoUiBodyMutation mutation{};
    mutation.kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    mutation.target = target.layout_node;
    mutation.widget = target.ui_widget;
    mutation.props_json_utf8 = reinterpret_cast<const uint8_t*>(wire_json.data());
    mutation.props_len = wire_json.size();
    const auto update_status = sao_ui_panel_update_body(body, &mutation, 1);
    if (update_status != SAO_STATUS_OK) {
        if (native_widget_update_state_uncertain(update_status)) {
            target.degraded_new_props_json = wire_json;
            quarantine_widget_cleanup(state, panel, body, target);
        }
        return static_cast<sao_sdk_status_t>(update_status);
    }
    sao_sdk_status_t final_status = SAO_SDK_OK;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) {
            final_status = SAO_SDK_ERR_NOT_FOUND;
        } else {
            const auto found = std::find_if(
                it->second.widgets.begin(), it->second.widgets.end(),
                [&](const WidgetEntry& item) { return item.sdk_handle == widget; });
            if (found == it->second.widgets.end()) {
                final_status = SAO_SDK_ERR_NOT_FOUND;
            } else if (it->second.unregistering ||
                       it->second.ui_mode != PanelEntry::UiMode::typed ||
                       !panel_native_handles_current(
                           it->second, reinterpret_cast<sao_ui_panel_handle_t>(panel), body) ||
                       !widget_entry_current(it->second, target)) {
                final_status = it->second.unregistering ? SAO_SDK_ERR_BUSY
                                                        : SAO_SDK_ERR_HANDLE_INVALID;
            } else {
                found->props_json = wire_json;
                found->cleanup_pending = false;
            }
        }
    }
    if (final_status == SAO_SDK_OK)
        return SAO_SDK_OK;

    SaoUiBodyMutation rollback{};
    rollback.kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    rollback.target = target.layout_node;
    rollback.widget = target.ui_widget;
    rollback.props_json_utf8 = target.props_json.empty()
                                    ? nullptr
                                    : reinterpret_cast<const uint8_t*>(target.props_json.data());
    rollback.props_len = target.props_json.size();
    const auto rollback_status =
        static_cast<sao_sdk_status_t>(sao_ui_panel_update_body(body, &rollback, 1));
    if (rollback_status != SAO_SDK_OK) {
        target.degraded_new_props_json = wire_json;
        quarantine_widget_cleanup(state, panel, body, target);
        return rollback_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it != state->panels.end()) {
            const auto found = std::find_if(
                it->second.widgets.begin(), it->second.widgets.end(),
                [&](const WidgetEntry& item) { return item.sdk_handle == widget; });
            if (found != it->second.widgets.end()) {
                found->props_json = target.props_json;
                found->cleanup_pending = false;
            }
        }
    }
    return final_status;
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_remove_widget(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                     sao_sdk_ui_widget_t widget) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr || panel == nullptr || widget == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    sao_ui_panel_body_handle_t body = nullptr;
    std::shared_ptr<std::mutex> native_mutex;
    WidgetEntry target;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        if (it->second.unregistering || it->second.ui_mode == PanelEntry::UiMode::legacy_spec)
            return SAO_SDK_ERR_BUSY;
        const auto found = std::find_if(it->second.widgets.begin(), it->second.widgets.end(),
                                        [&](const WidgetEntry& item) { return item.sdk_handle == widget; });
        if (found == it->second.widgets.end()) return SAO_SDK_ERR_NOT_FOUND;
        target = *found;
        body = it->second.ui_body;
        native_mutex = it->second.native_mutation_mutex;
    }
    std::lock_guard<std::mutex> native_lock(*native_mutex);
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        if (!panel_native_handles_current(it->second, reinterpret_cast<sao_ui_panel_handle_t>(panel), body) ||
            it->second.ui_mode != PanelEntry::UiMode::typed || !widget_entry_current(it->second, target))
            return it->second.unregistering ? SAO_SDK_ERR_BUSY : SAO_SDK_ERR_HANDLE_INVALID;
    }
    PanelEntry mutation_view;
    mutation_view.ui_body = body;
    const auto update_rc = remove_widget_node(mutation_view, target);
    if (update_rc != SAO_SDK_OK)
        return static_cast<sao_sdk_status_t>(update_rc);

    sao_sdk_status_t final_status = SAO_SDK_OK;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) {
            final_status = SAO_SDK_ERR_NOT_FOUND;
        } else if (!panel_native_handles_current(
                       it->second, reinterpret_cast<sao_ui_panel_handle_t>(panel), body) ||
                   it->second.ui_mode != PanelEntry::UiMode::typed ||
                   !widget_entry_current(it->second, target)) {
            final_status = it->second.unregistering ? SAO_SDK_ERR_BUSY
                                                    : SAO_SDK_ERR_HANDLE_INVALID;
        } else {
            const auto found = std::find_if(
                it->second.widgets.begin(), it->second.widgets.end(),
                [&](const WidgetEntry& item) { return item.sdk_handle == widget; });
            if (found == it->second.widgets.end())
                final_status = SAO_SDK_ERR_NOT_FOUND;
            else
                target = *found;
        }
    }
    if (final_status == SAO_SDK_OK) {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) {
            final_status = SAO_SDK_ERR_NOT_FOUND;
        } else {
            const auto found = std::find_if(
                it->second.widgets.begin(), it->second.widgets.end(),
                [&](const WidgetEntry& item) { return item.sdk_handle == widget; });
            if (found == it->second.widgets.end())
                final_status = SAO_SDK_ERR_NOT_FOUND;
            else {
                target = *found;
                it->second.widgets.erase(found);
            }
        }
    }
    if (final_status == SAO_SDK_OK) {
        destroy_widget_for_kind(target.kind, target.ui_widget, target.script_canvas);
        return SAO_SDK_OK;
    }

    sao_ui_layout_node_handle_t restored_node = nullptr;
    const auto restore_status = restore_widget_node(body, target, &restored_node);
    if (restore_status != SAO_SDK_OK) {
        quarantine_widget_cleanup(state, panel, body, target);
        return restore_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->panels.find(panel);
        if (it != state->panels.end()) {
            const auto found = std::find_if(
                it->second.widgets.begin(), it->second.widgets.end(),
                [&](const WidgetEntry& item) { return item.sdk_handle == widget; });
            if (found != it->second.widgets.end())
                found->layout_node = restored_node;
        }
    }
    return final_status;
}

sao_sdk_status_t SAO_SDK_CALL ui_set_overlay(void* ctx_impl, const char* surface_id_utf8,
                                             const uint8_t* spec_json_utf8, size_t spec_len) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease) return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (surface_id_utf8 == nullptr || surface_id_utf8[0] == '\0')
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (spec_json_utf8 == nullptr && spec_len != 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    sao_sdk_overlay_token_t previous = 0;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto it = state->overlays.find(surface_id_utf8);
        if (it != state->overlays.end()) previous = reinterpret_cast<uint64_t>(it->second);
    }
    if (previous != 0) {
        const auto clear_status = provider_overlay_clear(state, previous);
        if (clear_status != SAO_SDK_OK) return clear_status;
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
    if (!lease) return lease.status();
    if (out_token != nullptr) *out_token = 0;
    if (surface_id_utf8 == nullptr || surface_id_utf8[0] == '\0' || hook_fn == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    (void)priority;
    (void)hook_user_data;
    return SAO_SDK_ERR_UNSUPPORTED;
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_render_hook_legacy(void* ctx_impl,
                                                               sao_sdk_hook_token_t token) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease) return lease.status();
    return provider_render_unregister(cast_ctx(ctx_impl), token);
}

sao_sdk_status_t SAO_SDK_CALL ui_request_redraw(void* ctx_impl, const char* surface_id_utf8) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease) return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    const char* surface = surface_id_utf8 == nullptr ? SAO_ENGINE_ALL_SURFACES : surface_id_utf8;
    const auto status = provider_request_redraw(state, surface);
    if (status != SAO_SDK_OK) return status;
    std::lock_guard<std::mutex> lock(state->mu);
    for (auto& item : state->panels) {
        if (surface_id_utf8 == nullptr || item.second.panel_id == surface_id_utf8)
            ++item.second.redraw_count;
    }
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
        sao_sdk_status_t status = SAO_SDK_OK;
        const auto native_mutex = candidate->native_mutation_mutex;
        std::unique_lock<std::mutex> native_lock;
        if (native_mutex != nullptr)
            native_lock = std::unique_lock<std::mutex>(*native_mutex);
        if (candidate->ui_panel != nullptr) {
            status = invoke_callback_barrier([&] {
                return static_cast<sao_sdk_status_t>(
                    sao_ui_panel_set_action_handler(candidate->ui_panel, nullptr, nullptr));
            });
            if (status == SAO_SDK_OK)
                status = unregister_native_panel(candidate->ui_panel);
        }
        if (status != SAO_SDK_OK) {
            if (cleanup_status == SAO_SDK_OK)
                cleanup_status = status;
            failed.splice(failed.end(), pending, candidate);
            continue;
        }
        if (native_lock.owns_lock())
            native_lock.unlock();
        destroy_panel_resources(*candidate);
        pending.erase(candidate);
    }
    if (!failed.empty()) {
        std::lock_guard<std::mutex> lock(state->mu);
        for (auto it = failed.begin(); it != failed.end();) {
            auto current = it++;
            if (current->sdk_handle != nullptr) {
                current->cleanup_pending = true;
                current->unregistering = true;
                state->panels.emplace(current->sdk_handle, std::move(*current));
                failed.erase(current);
            }
        }
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
        SAO_SDK_UI_TABLE_ABI_VERSION,
        sizeof(SaoSdkUiTable),
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
        decltype(((SaoSdkUiTable*)0)->register_ui_panel) slot = nullptr;
        const auto slot_status = sao_sdk_ui_table_copy_slot(
            public_context, offsetof(SaoSdkUiTable, register_ui_panel), sizeof(slot), SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR, &slot);
        if (slot_status != SAO_SDK_OK)
            return slot_status;
        return slot(lease.state(), descriptor, out_panel);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_unregister_ui_panel(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        decltype(((SaoSdkUiTable*)0)->unregister_ui_panel) slot = nullptr;
        const auto slot_status = sao_sdk_ui_table_copy_slot(
            public_context, offsetof(SaoSdkUiTable, unregister_ui_panel), sizeof(slot), SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR, &slot);
        if (slot_status != SAO_SDK_OK)
            return slot_status;
        return slot(lease.state(), panel);
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
        decltype(((SaoSdkUiTable*)0)->panel_add_widget) slot = nullptr;
        const auto slot_status = sao_sdk_ui_table_copy_slot(
            public_context, offsetof(SaoSdkUiTable, panel_add_widget), sizeof(slot), SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR, &slot);
        if (slot_status != SAO_SDK_OK)
            return slot_status;
        return slot(lease.state(), panel, widget_spec, out_widget);
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
        decltype(((SaoSdkUiTable*)0)->panel_update_widget) slot = nullptr;
        const auto slot_status = sao_sdk_ui_table_copy_slot(
            public_context, offsetof(SaoSdkUiTable, panel_update_widget), sizeof(slot), SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR, &slot);
        if (slot_status != SAO_SDK_OK)
            return slot_status;
        return slot(lease.state(), panel, widget, widget_spec);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_remove_widget(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel, sao_sdk_ui_widget_t widget) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        decltype(((SaoSdkUiTable*)0)->panel_remove_widget) slot = nullptr;
        const auto slot_status = sao_sdk_ui_table_copy_slot(
            public_context, offsetof(SaoSdkUiTable, panel_remove_widget), sizeof(slot), SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR, &slot);
        if (slot_status != SAO_SDK_OK)
            return slot_status;
        return slot(lease.state(), panel, widget);
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
        decltype(((SaoSdkUiTable*)0)->register_render_hook_clock) slot = nullptr;
        const auto slot_status = sao_sdk_ui_table_copy_slot(
            public_context, offsetof(SaoSdkUiTable, register_render_hook_clock), sizeof(slot), 0u,
            &slot);
        if (slot_status != SAO_SDK_OK)
            return slot_status;
        return slot(lease.state(), hook_point, callback, user_data, out_hook_handle);
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
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        decltype(((SaoSdkUiTable*)0)->unregister_render_hook) slot = nullptr;
        const auto slot_status = sao_sdk_ui_table_copy_slot(
            public_context, offsetof(SaoSdkUiTable, unregister_render_hook), sizeof(slot), 0u,
            &slot);
        if (slot_status != SAO_SDK_OK)
            return slot_status;
        return slot(lease.state(), hook_handle);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_request_redraw(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        decltype(((SaoSdkUiTable*)0)->request_redraw) slot = nullptr;
        const auto slot_status = sao_sdk_ui_table_copy_slot(
            public_context, offsetof(SaoSdkUiTable, request_redraw), sizeof(slot), 0u, &slot);
        if (slot_status != SAO_SDK_OK)
            return slot_status;
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
        return slot(lease.state(), surface.empty() ? nullptr : surface.c_str());
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_request_redraw_surface(const struct SaoSdkContext* ctx, const char* surface_id_utf8) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        decltype(((SaoSdkUiTable*)0)->request_redraw) slot = nullptr;
        const auto slot_status = sao_sdk_ui_table_copy_slot(
            public_context, offsetof(SaoSdkUiTable, request_redraw), sizeof(slot), 0u, &slot);
        if (slot_status != SAO_SDK_OK)
            return slot_status;
        return slot(lease.state(), surface_id_utf8);
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

extern "C" SAO_SDK_API sao_ui_panel_body_handle_t SAO_SDK_CALL
sao_sdk_test_panel_body(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    try {
        if (panel == nullptr)
            return nullptr;
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return nullptr;
        auto* state = lease.state();
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->panels.find(panel);
        return it == state->panels.end() ? nullptr : it->second.ui_body;
    } catch (...) {
        return nullptr;
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
        size_t count = lease.state()->panel_cleanup_pending.size();
        for (const auto& item : lease.state()->panels)
            if (item.second.cleanup_pending)
                ++count;
        return count;
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
