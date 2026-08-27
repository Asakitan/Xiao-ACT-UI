#include "sao/plugins/compat/py_v1_ctx_shim.h"

#include "sao/sdk/sao_sdk.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace sao::plugins::compat {
namespace {

using ordered_json = nlohmann::ordered_json;
using sdk_binding::sdk_method_id;

constexpr uint16_t kUnknownMethod = 0xffffu;

struct alias_registry {
    std::mutex mutex;
    std::unordered_map<std::string, uint16_t> aliases;
};

struct method_mapping {
    const char* legacy_name;
    uint16_t target;
};

constexpr method_mapping kMethodMappings[] = {
    {"plugin_id", static_cast<uint16_t>(sdk_method_id::prop_plugin_id)},
    {"path", static_cast<uint16_t>(sdk_method_id::prop_path)},
    {"web_path", static_cast<uint16_t>(sdk_method_id::prop_web_path)},
    {"assets_path", static_cast<uint16_t>(sdk_method_id::prop_assets_path)},
    {"should_stop", kUnknownMethod},
    {"get_plugin_id", static_cast<uint16_t>(sdk_method_id::prop_plugin_id)},
    {"get_base_dir", static_cast<uint16_t>(sdk_method_id::prop_path)},
    {"log", kUnknownMethod}, {"log_info", kUnknownMethod},
    {"log_warn", kUnknownMethod}, {"log_error", kUnknownMethod},
    {"subscribe", static_cast<uint16_t>(sdk_method_id::method_subscribe)},
    {"subscribe_once", kUnknownMethod},
    {"unsubscribe", static_cast<uint16_t>(sdk_method_id::method_unsubscribe)},
    {"on", kUnknownMethod}, {"on_damage", kUnknownMethod}, {"on_heal", kUnknownMethod},
    {"on_skill", kUnknownMethod}, {"on_boss", kUnknownMethod},
    {"on_snapshot", kUnknownMethod}, {"on_encounter_finalized", kUnknownMethod},
    {"emit", static_cast<uint16_t>(sdk_method_id::method_emit)},
    {"publish", static_cast<uint16_t>(sdk_method_id::method_emit)},
    {"time", static_cast<uint16_t>(sdk_method_id::method_time)},
    {"get_snapshot", kUnknownMethod}, {"snapshot_value", kUnknownMethod},
    {"recent_events", kUnknownMethod},
    {"get_setting", static_cast<uint16_t>(sdk_method_id::method_get_setting)},
    {"setting", static_cast<uint16_t>(sdk_method_id::method_setting)},
    {"set_setting", static_cast<uint16_t>(sdk_method_id::method_set_setting)},
    {"set_defaults", static_cast<uint16_t>(sdk_method_id::method_set_defaults)},
    {"register_parser_adapter", kUnknownMethod}, {"register_exporter", kUnknownMethod},
    {"register_formatter", kUnknownMethod}, {"register_trigger_type", kUnknownMethod},
    {"register_report_view", kUnknownMethod}, {"register_timer", kUnknownMethod},
    {"register_ui_panel", static_cast<uint16_t>(sdk_method_id::method_register_ui_panel)},
    {"register_script", static_cast<uint16_t>(sdk_method_id::method_register_ui_panel)},
    {"register_render_hook", kUnknownMethod},
    {"set_overlay", static_cast<uint16_t>(sdk_method_id::method_set_overlay)},
    {"clear_overlay", kUnknownMethod},
    {"register_hotkey", static_cast<uint16_t>(sdk_method_id::method_register_hotkey)},
    {"add_hotkey", static_cast<uint16_t>(sdk_method_id::method_register_hotkey)},
    {"register_engine", kUnknownMethod}, {"register", kUnknownMethod},
    {"register_data_source", kUnknownMethod}, {"register_menu_category", kUnknownMethod},
    {"register_menu_surface", kUnknownMethod}, {"register_action_handler", kUnknownMethod},
    {"add_menu_item", kUnknownMethod},
    {"request_redraw", static_cast<uint16_t>(sdk_method_id::method_request_redraw)},
    {"set_interval", static_cast<uint16_t>(sdk_method_id::method_set_interval)},
    {"set_timeout", kUnknownMethod},
    {"clear_timer", static_cast<uint16_t>(sdk_method_id::method_clear_timer)},
    {"run_on_ui", kUnknownMethod}, {"notify", static_cast<uint16_t>(sdk_method_id::method_notify)},
    {"dismiss_notify", kUnknownMethod}, {"toast", static_cast<uint16_t>(sdk_method_id::method_toast)},
    {"open_file", kUnknownMethod}, {"open_window", kUnknownMethod},
    {"create_compositor_layer", kUnknownMethod}, {"upload_compositor_frame", kUnknownMethod},
    {"set_compositor_layer_mmf_source", kUnknownMethod},
    {"set_compositor_layer_shared_texture_source", kUnknownMethod},
    {"set_compositor_layer_position", kUnknownMethod}, {"set_compositor_layer_visible", kUnknownMethod},
    {"set_compositor_layer_input", kUnknownMethod}, {"destroy_compositor_layer", kUnknownMethod},
    {"compositor_gpu_interop_available", kUnknownMethod},
    {"compositor_layer_shared_texture_active", kUnknownMethod},
    {"compositor_display_refresh_hz", kUnknownMethod},
    {"get_engine", kUnknownMethod}, {"get", kUnknownMethod}, {"get_engine_method", kUnknownMethod},
    {"require_engine", kUnknownMethod}, {"call_engine", kUnknownMethod},
    {"call_runtime", kUnknownMethod}, {"ensure_requirements", kUnknownMethod},
    {"load_local", kUnknownMethod}, {"register_thread", kUnknownMethod},
    {"set_owner_attr", kUnknownMethod}, {"owner_attr", kUnknownMethod},
};

constexpr const char* kSpecialMethods[] = {"metadata"};

alias_registry& registry() {
    static alias_registry instance;
    return instance;
}

SaoSdkContext* sdk_context(plugin_context_ptr ctx) noexcept {
    return reinterpret_cast<SaoSdkContext*>(ctx);
}

bool valid_context(plugin_context_ptr ctx) noexcept {
    const auto* sdk = sdk_context(ctx);
    return sdk != nullptr && sdk->ctx_impl != nullptr &&
           (sdk->abi_version >> 16u) == SAO_SDK_ABI_VERSION_MAJOR;
}

char* duplicate_string(std::string_view value) noexcept {
    auto* output = static_cast<char*>(std::malloc(value.size() + 1));
    if (output == nullptr) return nullptr;
    std::memcpy(output, value.data(), value.size());
    output[value.size()] = '\0';
    return output;
}

const method_mapping* find_builtin_mapping(std::string_view name) noexcept {
    for (const auto& mapping : kMethodMappings) {
        if (name == mapping.legacy_name) return &mapping;
    }
    return nullptr;
}

uint16_t lookup_default(std::string_view name) noexcept {
    const auto* mapping = find_builtin_mapping(name);
    return mapping == nullptr ? kUnknownMethod : mapping->target;
}

bool real_dispatch_method(sdk_method_id method) noexcept {
    switch (method) {
    case sdk_method_id::prop_plugin_id:
    case sdk_method_id::prop_path:
    case sdk_method_id::prop_web_path:
    case sdk_method_id::prop_assets_path:
    case sdk_method_id::method_subscribe:
    case sdk_method_id::method_unsubscribe:
    case sdk_method_id::method_emit:
    case sdk_method_id::method_get_setting:
    case sdk_method_id::method_setting:
    case sdk_method_id::method_set_setting:
    case sdk_method_id::method_set_defaults:
    case sdk_method_id::method_register_ui_panel:
    case sdk_method_id::method_set_overlay:
    case sdk_method_id::method_register_hotkey:
    case sdk_method_id::method_request_redraw:
    case sdk_method_id::method_set_interval:
    case sdk_method_id::method_clear_timer:
    case sdk_method_id::method_notify:
    case sdk_method_id::method_toast:
    case sdk_method_id::method_time:
        return true;
    default:
        return false;
    }
}

bool special_method(std::string_view name) noexcept {
    return std::any_of(std::begin(kSpecialMethods), std::end(kSpecialMethods),
                       [name](const char* method) { return name == method; });
}

bool fixed_fail_closed_method(std::string_view name) noexcept {
    const auto* mapping = find_builtin_mapping(name);
    return mapping != nullptr && mapping->target == kUnknownMethod;
}

int32_t write_json_result(const ordered_json& value,
                          sdk_binding::sdk_context_call_request* request) {
    const std::string serialized = value.dump();
    if (!sdk_binding::sao_plugins_binding_validate_json_text(
            reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size()))
        return SAO_ERR_INVALID_ARGUMENT;
    if (request->out_required != nullptr) {
        *request->out_required = serialized.size() + 1;
    }
    if (request->out_result_json_utf8 == nullptr &&
        request->out_required == nullptr) {
        return SAO_OK;
    }
    if (request->out_result_json_utf8 == nullptr ||
        request->out_capacity < serialized.size() + 1) {
        if (request->out_result_json_utf8 != nullptr &&
            request->out_capacity > 0) {
            request->out_result_json_utf8[0] = '\0';
        }
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(request->out_result_json_utf8, serialized.data(),
                serialized.size());
    request->out_result_json_utf8[serialized.size()] = '\0';
    return SAO_OK;
}

ordered_json metadata(const SaoSdkContext* ctx) {
    const char* base_dir = nullptr;
    (void)sao_sdk_context_get_base_dir(ctx, &base_dir);
    return ordered_json{
        {"plugin_id", ctx->plugin_id_utf8 == nullptr ? "" :
                          ctx->plugin_id_utf8},
        {"plugin_version", ctx->plugin_version_utf8 == nullptr ? "" :
                               ctx->plugin_version_utf8},
        {"path", base_dir == nullptr ? "" : base_dir},
    };
}

const char* support_name(int32_t status) noexcept {
    return status == SAO_OK ? "supported" : "unsupported";
}

ordered_json method_report(const SaoSdkContext* ctx,
                           const method_mapping& mapping) {
    if (mapping.target == kUnknownMethod) {
        return ordered_json{
            {"legacy", mapping.legacy_name},
            {"target", ""},
            {"status", "unsupported"},
            {"status_code", loader::SAO_PLUGINS_ERR_UNSUPPORTED},
        };
    }
    const auto target = static_cast<sdk_method_id>(mapping.target);
    const int32_t status = sdk_binding::sao_plugins_sdk_context_method_status(ctx, target);
    return ordered_json{
        {"legacy", mapping.legacy_name},
        {"target", sdk_binding::sao_plugins_binding_method_name(target)},
        {"status", support_name(status)},
        {"status_code", status},
    };
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_arm_v1_ctx_shim(plugin_context_ptr ctx) {
    return valid_context(ctx) ? SAO_OK : SAO_ERR_INVALID_ARGUMENT;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_v1_report(plugin_context_ptr ctx,
                             char** out_report_json_utf8) {
    if (out_report_json_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_report_json_utf8 = nullptr;
    if (!valid_context(ctx)) return SAO_ERR_INVALID_ARGUMENT;
    try {
        const auto* sdk = sdk_context(ctx);
        ordered_json methods = ordered_json::array();
        methods.push_back({
            {"legacy", "metadata"},
            {"target", "SaoSdkContext"},
            {"status", "supported"},
            {"status_code", SAO_OK},
        });
        for (const auto& mapping : kMethodMappings) {
            methods.push_back(method_report(sdk, mapping));
        }
        ordered_json report{
            {"compat_abi", "legacy_v1"},
            {"metadata", metadata(sdk)},
            {"provider_status", sao_sdk_context_provider_status(sdk)},
            {"methods", std::move(methods)},
        };
        const std::string serialized = report.dump();
        *out_report_json_utf8 = duplicate_string(serialized);
        return *out_report_json_utf8 == nullptr ? SAO_ERR_OS_CALL_FAILED
                                                 : SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_v1_call(
    plugin_context_ptr ctx,
    const char* method_name,
    sdk_binding::sdk_context_call_request* request) {
    if (!valid_context(ctx) || method_name == nullptr ||
        method_name[0] == '\0' || request == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (std::strcmp(method_name, "metadata") == 0) {
        try {
            return write_json_result(metadata(sdk_context(ctx)), request);
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    if (fixed_fail_closed_method(method_name)) {
        return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    const uint16_t mapped = sao_plugins_compat_ctx_v1_lookup_alias(method_name);
    if (mapped == kUnknownMethod ||
        mapped >= static_cast<uint16_t>(sdk_method_id::method_count_)) {
        return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    return sdk_binding::sao_plugins_sdk_context_dispatch(
        sdk_context(ctx), static_cast<sdk_method_id>(mapped), request);
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_compat_is_v1_method(const char* method_name) {
    if (method_name == nullptr || method_name[0] == '\0') return false;
    if (special_method(method_name) ||
        find_builtin_mapping(method_name) != nullptr) {
        return true;
    }
    auto& aliases = registry();
    std::lock_guard lock(aliases.mutex);
    return aliases.aliases.find(method_name) != aliases.aliases.end();
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_convert_open_file_filters(
    const char* v1_filters_json,
    char** out_v2_pipe_string) {
    if (out_v2_pipe_string == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_v2_pipe_string = nullptr;
    if (v1_filters_json == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        size_t raw_size = 0;
        if (!sdk_binding::sao_plugins_binding_bounded_json_c_string(
            v1_filters_json, raw_size) ||
            !sdk_binding::sao_plugins_binding_validate_json_text(
                reinterpret_cast<const uint8_t*>(v1_filters_json), raw_size))
            return SAO_ERR_INVALID_ARGUMENT;
        const auto source = ordered_json::parse(v1_filters_json, v1_filters_json + raw_size);
        if (!source.is_array()) return SAO_ERR_INVALID_ARGUMENT;
        std::string output;
        for (const auto& item : source) {
            std::string label;
            std::string pattern;
            if (item.is_array() && item.size() >= 2 &&
                item[0].is_string() && item[1].is_string()) {
                label = item[0].get<std::string>();
                pattern = item[1].get<std::string>();
            } else if (item.is_object()) {
                label = item.value("label", item.value("name", ""));
                pattern = item.value(
                    "pattern", item.value("spec", item.value("filter", "")));
            } else {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            if (label.empty() || pattern.empty() ||
                label.find('|') != std::string::npos ||
                pattern.find('|') != std::string::npos) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            if (!output.empty()) output.push_back('|');
            output += label;
            output.push_back('|');
            output += pattern;
        }
        if (output.size() > sdk_binding::kMaximumBindingJsonBytes)
            return SAO_ERR_INVALID_ARGUMENT;
        const auto output_json = ordered_json(output).dump();
        if (!sdk_binding::sao_plugins_binding_validate_json_text(
                reinterpret_cast<const uint8_t*>(output_json.data()), output_json.size()))
            return SAO_ERR_INVALID_ARGUMENT;
        *out_v2_pipe_string = duplicate_string(output);
        return *out_v2_pipe_string == nullptr ? SAO_ERR_OS_CALL_FAILED
                                               : SAO_OK;
    } catch (...) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_ctx_v1_wrap(plugin_context_ptr new_ctx,
                               plugin_context_ptr* out_v1_ctx) {
    if (out_v1_ctx == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_v1_ctx = nullptr;
    if (!valid_context(new_ctx)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_plugins_compat_arm_v1_ctx_shim(new_ctx);
    if (status != SAO_OK) return status;
    *out_v1_ctx = new_ctx;
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_ctx_v1_register_alias(const char* old_name,
                                         uint16_t new_method_id) {
    if (old_name == nullptr || old_name[0] == '\0' ||
        new_method_id >= static_cast<uint16_t>(sdk_method_id::method_count_)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!real_dispatch_method(static_cast<sdk_method_id>(new_method_id)))
        return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    if (find_builtin_mapping(old_name) != nullptr) {
        return lookup_default(old_name) == new_method_id ? SAO_OK : SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        auto& aliases = registry();
        std::lock_guard lock(aliases.mutex);
        aliases.aliases[old_name] = new_method_id;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API uint16_t SAO_PLUGINS_CALL
sao_plugins_compat_ctx_v1_lookup_alias(const char* old_name) {
    if (old_name == nullptr || old_name[0] == '\0') return kUnknownMethod;
    if (fixed_fail_closed_method(old_name))
        return kUnknownMethod;
    auto& aliases = registry();
    {
        std::lock_guard lock(aliases.mutex);
        const auto found = aliases.aliases.find(old_name);
        if (found != aliases.aliases.end() &&
            real_dispatch_method(static_cast<sdk_method_id>(found->second))) return found->second;
    }
    return lookup_default(old_name);
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_compat_ctx_v1_clear_aliases(void) {
    auto& aliases = registry();
    std::lock_guard lock(aliases.mutex);
    aliases.aliases.clear();
}

} // namespace sao::plugins::compat
