// binding_emma.cpp — Emma SDK binding provider facade
//
// Emma context、值转换和回调包装转发到 provider；插件生命周期走共享 binding。

#include "sao/plugins/sdk_binding/binding_emma.h"

#include <cstring>

namespace sao::plugins::sdk_binding {

namespace detail {
plugin_binding_handle_t make_binding(void* ctx, void* lang_state, int lang);
void free_binding(plugin_binding_handle_t plugin);
} // namespace detail

// 保留旧签名并转发到 provider。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_register_ctx(emma_interpreter_ptr interp, plugin_context_ptr ctx) {
    if (interp == nullptr || ctx == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.runtime = interp;
    request.context = reinterpret_cast<void*>(ctx);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::emma, language_binding_operation::context_bind, &request);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_value_to_json(emma_value_ptr value, char** out_json_utf8) {
    if (out_json_utf8 != nullptr)
        *out_json_utf8 = nullptr;
    if (value == nullptr || out_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.value = value;
    request.out_object = reinterpret_cast<void**>(out_json_utf8);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::emma, language_binding_operation::value_to_json, &request);
}

extern "C" SAO_PLUGINS_API emma_value_ptr SAO_PLUGINS_CALL
sao_plugins_binding_emma_json_to_value(emma_interpreter_ptr interp, const char* utf8_json) {
    if (interp == nullptr || utf8_json == nullptr)
        return nullptr;
    language_binding_request request{};
    request.runtime = interp;
    request.input = reinterpret_cast<const uint8_t*>(utf8_json);
    request.input_size = std::strlen(utf8_json);
    request.out_object = &request.value;
    return sao_plugins_binding_dispatch_provider(language_host_kind::emma,
                                                 language_binding_operation::json_to_value,
                                                 &request) == SAO_OK
               ? reinterpret_cast<emma_value_ptr>(request.value)
               : nullptr;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_wrap_callback(emma_interpreter_ptr interp, emma_value_ptr callable,
                                       void** out_sdk_callback_ptr, void** out_user_data) {
    if (out_sdk_callback_ptr != nullptr)
        *out_sdk_callback_ptr = nullptr;
    if (out_user_data != nullptr)
        *out_user_data = nullptr;
    if (interp == nullptr || callable == nullptr || out_sdk_callback_ptr == nullptr ||
        out_user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    language_binding_request request{};
    request.runtime = interp;
    request.value = callable;
    request.out_callback = out_sdk_callback_ptr;
    request.out_user_data = out_user_data;
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::emma, language_binding_operation::callback_wrap, &request);
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_emma_release_callback(void* user_data) {
    sao_plugins_binding_release_callback(language_host_kind::emma, user_data);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_binding_emma_get_method_table(
    sdk_method_id*, emma_ctx_method_fn*, size_t* inout_count) {
    if (inout_count == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *inout_count = 0;
    return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

// 共享插件 binding 的 activate / deactivate

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_activate(plugin_context_ptr plugin_ctx, emma_interpreter_ptr interp,
                                  plugin_binding_handle_t* out_plugin) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    return sao_plugins_binding_plugin_load(language_host_kind::emma,
                                           reinterpret_cast<void*>(plugin_ctx), interp, out_plugin);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_deactivate(plugin_binding_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return sao_plugins_binding_plugin_unload(plugin);
}

} // namespace sao::plugins::sdk_binding
