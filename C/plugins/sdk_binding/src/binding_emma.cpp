// binding_emma.cpp — Wave 4 首切片
//
// activate() 概念: Emma 侧直接注入 ctx 到全局符号表 (interpreter->globals).
// Wave 4 只记账, 真 Emma scope 集成留 Wave 5。

#include "sao/plugins/sdk_binding/binding_emma.h"

#include <cstring>

namespace sao::plugins::sdk_binding {

namespace detail {
plugin_binding_handle_t make_binding(void* ctx,
                                     void* lang_state,
                                     int lang);
void free_binding(plugin_binding_handle_t plugin);
} // namespace detail

// 保留旧 stub
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_register_ctx(emma_interpreter_ptr interp,
                                      plugin_context_ptr ctx) {
    if (interp == nullptr || ctx == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.runtime = interp;
    request.context = reinterpret_cast<void*>(ctx);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::emma, language_binding_operation::context_bind,
        &request);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_value_to_json(emma_value_ptr value, char** out_json_utf8) {
    if (out_json_utf8 != nullptr) *out_json_utf8 = nullptr;
    if (value == nullptr || out_json_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.value = value;
    request.out_object = reinterpret_cast<void**>(out_json_utf8);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::emma, language_binding_operation::value_to_json,
        &request);
}

extern "C" SAO_PLUGINS_API emma_value_ptr SAO_PLUGINS_CALL
sao_plugins_binding_emma_json_to_value(emma_interpreter_ptr interp,
                                       const char* utf8_json) {
    if (interp == nullptr || utf8_json == nullptr) return nullptr;
    language_binding_request request{};
    request.runtime = interp;
    request.input = reinterpret_cast<const uint8_t*>(utf8_json);
    request.input_size = std::strlen(utf8_json);
    request.out_object = &request.value;
    return sao_plugins_binding_dispatch_provider(
               language_host_kind::emma,
               language_binding_operation::json_to_value,
               &request) == SAO_OK
               ? reinterpret_cast<emma_value_ptr>(request.value)
               : nullptr;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_wrap_callback(emma_interpreter_ptr interp,
                                       emma_value_ptr callable,
                                       void** out_sdk_callback_ptr,
                                       void** out_user_data) {
    if (out_sdk_callback_ptr != nullptr) *out_sdk_callback_ptr = nullptr;
    if (out_user_data != nullptr) *out_user_data = nullptr;
    if (interp == nullptr || callable == nullptr ||
        out_sdk_callback_ptr == nullptr || out_user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    language_binding_request request{};
    request.runtime = interp;
    request.value = callable;
    request.out_callback = out_sdk_callback_ptr;
    request.out_user_data = out_user_data;
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::emma,
        language_binding_operation::callback_wrap,
        &request);
}

// Wave 4 新增: activate / deactivate

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_activate(plugin_context_ptr plugin_ctx,
                                  emma_interpreter_ptr interp,
                                  plugin_binding_handle_t* out_plugin) {
    if (out_plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    return sao_plugins_binding_plugin_load(
        language_host_kind::emma, reinterpret_cast<void*>(plugin_ctx), interp,
        out_plugin);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_deactivate(plugin_binding_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    return sao_plugins_binding_plugin_unload(plugin);
}

} // namespace sao::plugins::sdk_binding
