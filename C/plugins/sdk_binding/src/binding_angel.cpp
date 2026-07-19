// binding_angel.cpp — Wave 4 首切片
//
// activate() 概念: RegisterGlobalProperty 塞 ctx 到 g_ctx。Wave 4 只记账,
// 真 AS RegisterGlobalProperty 集成留 Wave 5。

#include "sao/plugins/sdk_binding/binding_angel.h"

#include <cstring>

namespace sao::plugins::sdk_binding {

namespace detail {
plugin_binding_handle_t make_binding(void* ctx, void* lang_state, int lang);
void free_binding(plugin_binding_handle_t plugin);
} // namespace detail

// 保留旧 stub
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_register_sdk(asIScriptEngine* engine) {
    if (engine == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.runtime = engine;
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::angel, language_binding_operation::runtime_register, &request);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_bind_ctx(asIScriptEngine* engine, plugin_context_ptr ctx) {
    if (engine == nullptr || ctx == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.runtime = engine;
    request.context = reinterpret_cast<void*>(ctx);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::angel, language_binding_operation::context_bind, &request);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_wrap_callback(asIScriptEngine* engine, asIScriptFunction* fn,
                                        void** out_sdk_callback_ptr, void** out_user_data) {
    if (out_sdk_callback_ptr != nullptr)
        *out_sdk_callback_ptr = nullptr;
    if (out_user_data != nullptr)
        *out_user_data = nullptr;
    if (engine == nullptr || fn == nullptr || out_sdk_callback_ptr == nullptr ||
        out_user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    language_binding_request request{};
    request.runtime = engine;
    request.value = fn;
    request.out_callback = out_sdk_callback_ptr;
    request.out_user_data = out_user_data;
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::angel, language_binding_operation::callback_wrap, &request);
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_angel_release_callback(void* user_data) {
    sao_plugins_binding_release_callback(language_host_kind::angel, user_data);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_dict_to_json(void* as_dictionary_ptr, char** out_json_utf8) {
    if (out_json_utf8 != nullptr)
        *out_json_utf8 = nullptr;
    if (as_dictionary_ptr == nullptr || out_json_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    language_binding_request request{};
    request.value = as_dictionary_ptr;
    request.out_object = reinterpret_cast<void**>(out_json_utf8);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::angel, language_binding_operation::value_to_json, &request);
}

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_binding_angel_json_to_dict(asIScriptEngine* engine, const char* utf8_json) {
    if (engine == nullptr || utf8_json == nullptr)
        return nullptr;
    language_binding_request request{};
    request.runtime = engine;
    request.input = reinterpret_cast<const uint8_t*>(utf8_json);
    request.input_size = std::strlen(utf8_json);
    request.out_object = &request.value;
    return sao_plugins_binding_dispatch_provider(language_host_kind::angel,
                                                 language_binding_operation::json_to_value,
                                                 &request) == SAO_OK
               ? request.value
               : nullptr;
}

// Wave 4 新增: activate / deactivate

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_binding_angel_activate(
    plugin_context_ptr plugin_ctx, asIScriptEngine* engine, plugin_binding_handle_t* out_plugin) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    return sao_plugins_binding_plugin_load(language_host_kind::angel,
                                           reinterpret_cast<void*>(plugin_ctx), engine, out_plugin);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_deactivate(plugin_binding_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return sao_plugins_binding_plugin_unload(plugin);
}

} // namespace sao::plugins::sdk_binding
