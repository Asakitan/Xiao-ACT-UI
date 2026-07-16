#include "sao/plugins/sdk_binding/binding_common.h"

#include "sao/sdk/sao_sdk.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace sao::plugins::sdk_binding {
namespace {

using ordered_json = nlohmann::ordered_json;

bool valid_context(const SaoSdkContext* ctx) noexcept {
    return ctx != nullptr && ctx->ctx_impl != nullptr &&
           (ctx->abi_version >> 16u) == SAO_SDK_ABI_VERSION_MAJOR;
}

int32_t unsupported() noexcept {
    return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

int32_t normalize_status(int32_t status) noexcept {
    return status == SAO_SDK_ERR_UNSUPPORTED ? unsupported() : status;
}

int32_t write_result(const ordered_json& value,
                     sdk_context_call_request* request) {
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

ordered_json parse_arguments(const sdk_context_call_request* request,
                             bool* valid) {
    *valid = false;
    if (request->args_size == 0) {
        *valid = true;
        return ordered_json::object();
    }
    if (request->args_json_utf8 == nullptr) return {};
    try {
        auto arguments = ordered_json::parse(
            request->args_json_utf8,
            request->args_json_utf8 + request->args_size);
        *valid = arguments.is_object();
        return arguments;
    } catch (...) {
        return {};
    }
}

bool read_string(const ordered_json& arguments,
                 const char* key,
                 std::string* output,
                 bool allow_empty = false) {
    const auto found = arguments.find(key);
    if (found == arguments.end() || !found->is_string()) return false;
    *output = found->get<std::string>();
    return allow_empty || !output->empty();
}

bool parse_hotkey(std::string_view text,
                  uint32_t* out_key,
                  uint32_t* out_modifiers) {
    if (out_key == nullptr || out_modifiers == nullptr) return false;
    *out_key = 0;
    *out_modifiers = 0;
    std::string token;
    for (size_t index = 0; index <= text.size(); ++index) {
        const char character = index == text.size() ? '\0' : text[index];
        if (character != '+' && character != '\0') {
            if (!std::isspace(static_cast<unsigned char>(character))) {
                token.push_back(static_cast<char>(std::toupper(
                    static_cast<unsigned char>(character))));
            }
            continue;
        }
        if (token == "CTRL" || token == "CONTROL") {
            *out_modifiers |= 1u << 0u;
        } else if (token == "ALT") {
            *out_modifiers |= 1u << 1u;
        } else if (token == "SHIFT") {
            *out_modifiers |= 1u << 2u;
        } else if (token == "WIN" || token == "WINDOWS") {
            *out_modifiers |= 1u << 3u;
        } else if (token.size() >= 2 && token[0] == 'F') {
            char* end = nullptr;
            const long number = std::strtol(token.c_str() + 1, &end, 10);
            if (end != token.c_str() + token.size() ||
                number < 1 || number > 24) {
                return false;
            }
            *out_key = 0x70u + static_cast<uint32_t>(number - 1);
        } else if (token.size() == 1 &&
                   std::isalnum(static_cast<unsigned char>(token[0]))) {
            *out_key = static_cast<uint32_t>(token[0]);
        } else {
            return false;
        }
        token.clear();
    }
    return *out_key != 0;
}

int32_t config_get(const SaoSdkContext* ctx,
                   const std::string& key,
                   const ordered_json& fallback,
                   sdk_context_call_request* request) {
    bool bool_value = false;
    auto status = sao_sdk_config_get_bool(ctx, key.c_str(), &bool_value);
    if (status == SAO_SDK_OK) return write_result(bool_value, request);

    int64_t int_value = 0;
    status = sao_sdk_config_get_int(ctx, key.c_str(), &int_value);
    if (status == SAO_SDK_OK) return write_result(int_value, request);

    double double_value = 0.0;
    status = sao_sdk_config_get_double(ctx, key.c_str(), &double_value);
    if (status == SAO_SDK_OK) return write_result(double_value, request);

    size_t required = 0;
    status = sao_sdk_config_get_string(ctx, key.c_str(), nullptr, 0, &required);
    if (status == SAO_SDK_ERR_BUFFER_TOO_SMALL && required > 0) {
        std::string string_value(required, '\0');
        status = sao_sdk_config_get_string(ctx, key.c_str(),
                                           string_value.data(),
                                           string_value.size(), &required);
        if (status == SAO_SDK_OK) {
            string_value.resize(std::strlen(string_value.c_str()));
            return write_result(string_value, request);
        }
    }
    if (status == SAO_SDK_ERR_NOT_FOUND) {
        return write_result(fallback, request);
    }
    return status;
}

int32_t config_set(const SaoSdkContext* ctx,
                   const std::string& key,
                   const ordered_json& value) {
    if (value.is_boolean()) {
        return sao_sdk_config_set_bool(ctx, key.c_str(), value.get<bool>());
    }
    if (value.is_number_integer()) {
        return sao_sdk_config_set_int(ctx, key.c_str(), value.get<int64_t>());
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (!std::isfinite(number)) return SAO_ERR_INVALID_ARGUMENT;
        return sao_sdk_config_set_double(ctx, key.c_str(), number);
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        return sao_sdk_config_set_string(ctx, key.c_str(), text.c_str());
    }
    return unsupported();
}

bool config_value_supported(const ordered_json& value) {
    return value.is_boolean() || value.is_number_integer() ||
           (value.is_number_float() &&
            std::isfinite(value.get<double>())) || value.is_string();
}

int32_t dispatch_property(const SaoSdkContext* ctx,
                          sdk_method_id method,
                          sdk_context_call_request* request) {
    if (method == sdk_method_id::prop_plugin_id) {
        return write_result(ctx->plugin_id_utf8 == nullptr
                                ? std::string{}
                                : std::string(ctx->plugin_id_utf8),
                            request);
    }
    const char* base_dir = nullptr;
    const int32_t status = sao_sdk_context_get_base_dir(ctx, &base_dir);
    if (status != SAO_SDK_OK) return status;
    std::string path = base_dir == nullptr ? "" : base_dir;
    if (method == sdk_method_id::prop_web_path) path += "/web";
    if (method == sdk_method_id::prop_assets_path) path += "/assets";
    return write_result(path, request);
}

int32_t dispatch_settings(const SaoSdkContext* ctx,
                          sdk_method_id method,
                          const ordered_json& arguments,
                          sdk_context_call_request* request) {
    if (method == sdk_method_id::method_set_defaults) {
        const auto found = arguments.find("defaults");
        const auto& defaults = found == arguments.end() ? arguments : *found;
        if (!defaults.is_object()) return SAO_ERR_INVALID_ARGUMENT;
        for (const auto& [key, value] : defaults.items()) {
            if (key.empty() || !config_value_supported(value)) {
                return unsupported();
            }
        }
        for (const auto& [key, value] : defaults.items()) {
            bool existing = false;
            int64_t existing_int = 0;
            double existing_double = 0.0;
            size_t existing_string = 0;
            if (sao_sdk_config_get_bool(ctx, key.c_str(), &existing) == SAO_SDK_OK ||
                sao_sdk_config_get_int(ctx, key.c_str(), &existing_int) == SAO_SDK_OK ||
                sao_sdk_config_get_double(ctx, key.c_str(), &existing_double) == SAO_SDK_OK ||
                sao_sdk_config_get_string(ctx, key.c_str(), nullptr, 0,
                                          &existing_string) ==
                    SAO_SDK_ERR_BUFFER_TOO_SMALL) {
                continue;
            }
            const int32_t status = config_set(ctx, key, value);
            if (status != SAO_SDK_OK) return status;
        }
        return write_result(nullptr, request);
    }

    std::string key;
    if (!read_string(arguments, "key", &key)) return SAO_ERR_INVALID_ARGUMENT;
    if (method == sdk_method_id::method_get_setting ||
        method == sdk_method_id::method_setting) {
        const auto fallback = arguments.find("default");
        return config_get(ctx, key,
                          fallback == arguments.end() ? ordered_json(nullptr)
                                                      : *fallback,
                          request);
    }
    const auto value = arguments.find("value");
    if (value == arguments.end()) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = config_set(ctx, key, *value);
    return status == SAO_SDK_OK ? write_result(nullptr, request) : status;
}

int32_t dispatch_event(const SaoSdkContext* ctx,
                       sdk_method_id method,
                       const ordered_json& arguments,
                       sdk_context_call_request* request) {
    if (method == sdk_method_id::method_unsubscribe) {
        const auto token = arguments.find("token");
        if (token == arguments.end() || !token->is_number_unsigned()) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const int32_t status = sao_sdk_event_unsubscribe(
            ctx, token->get<sao_sdk_subscription_t>());
        return status == SAO_SDK_OK ? write_result(true, request) : status;
    }

    std::string topic;
    if (!read_string(arguments, "topic", &topic)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (method == sdk_method_id::method_emit) {
        const auto payload = arguments.find("payload");
        const std::string serialized =
            (payload == arguments.end() ? ordered_json::object() : *payload).dump();
        const int32_t status = sao_sdk_event_publish(
            ctx, topic.c_str(),
            reinterpret_cast<const uint8_t*>(serialized.data()),
            serialized.size());
        return status == SAO_SDK_OK ? write_result(true, request) : status;
    }
    if (request->event_callback == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    sao_sdk_subscription_t token = 0;
    const int32_t status = sao_sdk_event_subscribe(
        ctx, topic.c_str(), request->event_callback,
        request->callback_user_data, &token);
    return status == SAO_SDK_OK ? write_result(token, request) : status;
}

int32_t dispatch_ui(const SaoSdkContext* ctx,
                    sdk_method_id method,
                    const ordered_json& arguments,
                    sdk_context_call_request* request) {
    if (method == sdk_method_id::method_request_redraw) {
        std::string surface;
        if (!read_string(arguments, "surface", &surface, true)) {
            surface.clear();
        }
        const int32_t status = sao_sdk_ui_request_redraw(
            ctx, surface.empty() ? nullptr : surface.c_str());
        return status == SAO_SDK_OK ? write_result(true, request) : status;
    }
    if (method == sdk_method_id::method_set_overlay) {
        std::string surface;
        if (!read_string(arguments, "surface", &surface)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto spec = arguments.find("spec");
        if (spec == arguments.end()) return SAO_ERR_INVALID_ARGUMENT;
        const std::string serialized = spec->dump();
        const int32_t status = sao_sdk_ui_set_overlay(
            ctx, surface.c_str(),
            reinterpret_cast<const uint8_t*>(serialized.data()),
            serialized.size());
        return status == SAO_SDK_OK ? write_result(true, request) : status;
    }

    std::string panel_id;
    if (!read_string(arguments, "id", &panel_id)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const auto metadata = arguments.find("metadata");
    const std::string title =
        metadata != arguments.end() && metadata->is_object()
            ? metadata->value("title", panel_id)
            : panel_id;
    const auto spec = arguments.find("spec");
    const std::string serialized =
        (spec == arguments.end() ? ordered_json::object() : *spec).dump();
    sao_sdk_ui_panel_t panel = nullptr;
    const int32_t status = sao_sdk_ui_register_panel(
        ctx, panel_id.c_str(), title.c_str(),
        reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size(),
        request->panel_action_callback,
        request->callback_user_data, &panel);
    return status == SAO_SDK_OK
               ? write_result(reinterpret_cast<uintptr_t>(panel), request)
               : status;
}

int32_t dispatch_hotkey(const SaoSdkContext* ctx,
                        const ordered_json& arguments,
                        sdk_context_call_request* request) {
    std::string binding_id;
    std::string default_key;
    if (!read_string(arguments, "id", &binding_id) ||
        !read_string(arguments, "default_key", &default_key) ||
        request->hotkey_callback == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    uint32_t virtual_key = 0;
    uint32_t modifiers = 0;
    if (!parse_hotkey(default_key, &virtual_key, &modifiers)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    SaoSdkHotkeySpec spec{};
    spec.binding_id_utf8 = binding_id.c_str();
    spec.virtual_key = virtual_key;
    spec.modifiers = modifiers;
    sao_sdk_hotkey_id_t token = 0;
    const int32_t status = sao_sdk_register_hotkey(
        ctx, &spec, request->hotkey_callback,
        request->callback_user_data, &token);
    return status == SAO_SDK_OK ? write_result(token, request) : status;
}

int32_t dispatch_timer(const SaoSdkContext* ctx,
                       sdk_method_id method,
                       const ordered_json& arguments,
                       sdk_context_call_request* request) {
    if (method == sdk_method_id::method_clear_timer) {
        const auto token = arguments.find("token");
        if (token == arguments.end() || !token->is_number_unsigned()) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const int32_t status = sao_sdk_timer_unregister(
            ctx, token->get<sao_sdk_timer_token_t>());
        return status == SAO_SDK_OK ? write_result(true, request) : status;
    }
    const auto seconds = arguments.find("seconds");
    if (seconds == arguments.end() || !seconds->is_number() ||
        request->timer_callback == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const double value = seconds->get<double>();
    if (!std::isfinite(value) || value <= 0.0 ||
        value > static_cast<double>(std::numeric_limits<uint32_t>::max()) /
                    1000.0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const uint32_t interval_ms = static_cast<uint32_t>(
        std::max(1.0, std::ceil(value * 1000.0)));
    sao_sdk_timer_token_t token = 0;
    const int32_t status = sao_sdk_timer_register(
        ctx, interval_ms, request->timer_callback,
        request->callback_user_data, &token);
    return status == SAO_SDK_OK ? write_result(token, request) : status;
}

int32_t dispatch_notification(const SaoSdkContext* ctx,
                              sdk_method_id method,
                              const ordered_json& arguments,
                              sdk_context_call_request* request) {
    std::string title;
    std::string message;
    double duration_s = 3.0;
    if (method == sdk_method_id::method_notify) {
        if (!read_string(arguments, "title", &title, true) ||
            !read_string(arguments, "message", &message)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        duration_s = arguments.value("duration_s", 60.0);
    } else if (!read_string(arguments, "message", &message)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(duration_s) || duration_s < 0.0 ||
        duration_s > static_cast<double>(std::numeric_limits<uint32_t>::max()) /
                         1000.0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const std::string text = title.empty() ? message : title + ": " + message;
    SaoSdkNotifySpec spec{};
    spec.text_utf8 = text.c_str();
    spec.duration_ms = static_cast<uint32_t>(std::ceil(duration_s * 1000.0));
    spec.argb_color = arguments.value("argb_color", 0xffffffffu);
    sao_sdk_notify_token_t token = 0;
    const int32_t status = sao_sdk_notify_show(ctx, &spec, &token);
    return status == SAO_SDK_OK ? write_result(token, request) : status;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_sdk_context_method_status(const SaoSdkContext* ctx,
                                      sdk_method_id method_id) {
    if (!valid_context(ctx)) return SAO_ERR_INVALID_ARGUMENT;
    switch (method_id) {
    case sdk_method_id::prop_plugin_id:
    case sdk_method_id::prop_path:
    case sdk_method_id::prop_web_path:
    case sdk_method_id::prop_assets_path:
        return SAO_OK;
    case sdk_method_id::method_subscribe:
    case sdk_method_id::method_unsubscribe:
    case sdk_method_id::method_emit:
        return ctx->event != nullptr ? SAO_OK : unsupported();
    case sdk_method_id::method_get_setting:
    case sdk_method_id::method_setting:
    case sdk_method_id::method_set_setting:
    case sdk_method_id::method_set_defaults:
        return ctx->config != nullptr ? SAO_OK : unsupported();
    case sdk_method_id::method_register_ui_panel:
        return ctx->ui != nullptr && ctx->ui->register_panel != nullptr
                   ? SAO_OK
                   : unsupported();
    case sdk_method_id::method_register_hotkey:
    case sdk_method_id::method_set_overlay:
    case sdk_method_id::method_request_redraw:
    case sdk_method_id::method_set_interval:
    case sdk_method_id::method_clear_timer:
    case sdk_method_id::method_notify:
    case sdk_method_id::method_toast:
        return normalize_status(sao_sdk_context_provider_status(ctx));
    default:
        return unsupported();
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_sdk_context_dispatch(const SaoSdkContext* ctx,
                                 sdk_method_id method_id,
                                 sdk_context_call_request* request) {
    if (request == nullptr || !valid_context(ctx) ||
        (request->args_size > 0 && request->args_json_utf8 == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (request->out_required != nullptr) *request->out_required = 0;
    const int32_t availability =
        sao_plugins_sdk_context_method_status(ctx, method_id);
    if (availability != SAO_OK) return availability;
    bool valid = false;
    const auto arguments = parse_arguments(request, &valid);
    if (!valid) return SAO_ERR_INVALID_ARGUMENT;
    try {
        int32_t status = unsupported();
        switch (method_id) {
        case sdk_method_id::prop_plugin_id:
        case sdk_method_id::prop_path:
        case sdk_method_id::prop_web_path:
        case sdk_method_id::prop_assets_path:
            status = dispatch_property(ctx, method_id, request);
            break;
        case sdk_method_id::method_get_setting:
        case sdk_method_id::method_setting:
        case sdk_method_id::method_set_setting:
        case sdk_method_id::method_set_defaults:
            status = dispatch_settings(ctx, method_id, arguments, request);
            break;
        case sdk_method_id::method_subscribe:
        case sdk_method_id::method_unsubscribe:
        case sdk_method_id::method_emit:
            status = dispatch_event(ctx, method_id, arguments, request);
            break;
        case sdk_method_id::method_register_ui_panel:
        case sdk_method_id::method_set_overlay:
        case sdk_method_id::method_request_redraw:
            status = dispatch_ui(ctx, method_id, arguments, request);
            break;
        case sdk_method_id::method_register_hotkey:
            status = dispatch_hotkey(ctx, arguments, request);
            break;
        case sdk_method_id::method_set_interval:
        case sdk_method_id::method_clear_timer:
            status = dispatch_timer(ctx, method_id, arguments, request);
            break;
        case sdk_method_id::method_notify:
        case sdk_method_id::method_toast:
            status = dispatch_notification(ctx, method_id, arguments, request);
            break;
        default:
            break;
        }
        return normalize_status(status);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::sdk_binding
