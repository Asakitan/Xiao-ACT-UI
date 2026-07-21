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
    sdk_method_id target;
};

constexpr method_mapping kDefaultMappings[] = {
    {"plugin_id", sdk_method_id::prop_plugin_id},
    {"path", sdk_method_id::prop_path},
    {"web_path", sdk_method_id::prop_web_path},
    {"assets_path", sdk_method_id::prop_assets_path},
    {"subscribe", sdk_method_id::method_subscribe},
    {"unsubscribe", sdk_method_id::method_unsubscribe},
    {"emit", sdk_method_id::method_emit},
    {"publish", sdk_method_id::method_emit},
    {"get_setting", sdk_method_id::method_get_setting},
    {"setting", sdk_method_id::method_setting},
    {"set_setting", sdk_method_id::method_set_setting},
    {"set_defaults", sdk_method_id::method_set_defaults},
    {"register_ui_panel", sdk_method_id::method_register_ui_panel},
    {"register_script", sdk_method_id::method_register_ui_panel},
    {"set_overlay", sdk_method_id::method_set_overlay},
    {"register_hotkey", sdk_method_id::method_register_hotkey},
    {"add_hotkey", sdk_method_id::method_register_hotkey},
    {"register_menu_category",
     sdk_method_id::method_register_menu_category},
    {"register_menu_surface", sdk_method_id::method_register_menu_surface},
    {"register_action_handler", sdk_method_id::method_register_action_handler},
    {"request_redraw", sdk_method_id::method_request_redraw},
    {"set_interval", sdk_method_id::method_set_interval},
    {"set_timeout", sdk_method_id::method_set_timeout},
    {"clear_timer", sdk_method_id::method_clear_timer},
    {"notify", sdk_method_id::method_notify},
    {"dismiss_notify", sdk_method_id::method_dismiss_notify},
    {"toast", sdk_method_id::method_toast},
    {"register_report_view", sdk_method_id::method_register_report_view},
    {"register_timer", sdk_method_id::method_register_timer},
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

uint16_t lookup_default(std::string_view name) noexcept {
    for (const auto& mapping : kDefaultMappings) {
        if (name == mapping.legacy_name) {
            return static_cast<uint16_t>(mapping.target);
        }
    }
    return kUnknownMethod;
}

bool special_method(std::string_view name) noexcept {
    return std::any_of(std::begin(kSpecialMethods), std::end(kSpecialMethods),
                       [name](const char* method) { return name == method; });
}

bool fixed_fail_closed_method(std::string_view name) noexcept {
    return name == "register_menu_surface" || name == "register_action_handler";
}

bool fixed_menu_action_method(std::string_view name) noexcept {
    return name == "register_menu_category" || fixed_fail_closed_method(name);
}

int32_t write_json_result(const ordered_json& value,
                          sdk_binding::sdk_context_call_request* request) {
    const std::string serialized = value.dump();
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
    const int32_t status = sdk_binding::sao_plugins_sdk_context_method_status(
        ctx, mapping.target);
    return ordered_json{
        {"legacy", mapping.legacy_name},
        {"target", sdk_binding::sao_plugins_binding_method_name(mapping.target)},
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
        for (const auto& mapping : kDefaultMappings) {
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
        lookup_default(method_name) != kUnknownMethod) {
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
        const auto source = ordered_json::parse(v1_filters_json);
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
    if (fixed_menu_action_method(old_name) && new_method_id != lookup_default(old_name)) {
        return SAO_ERR_INVALID_ARGUMENT;
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
    if (fixed_menu_action_method(old_name))
        return lookup_default(old_name);
    auto& aliases = registry();
    {
        std::lock_guard lock(aliases.mutex);
        const auto found = aliases.aliases.find(old_name);
        if (found != aliases.aliases.end()) return found->second;
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
