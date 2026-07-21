// binding_csharp.cpp — C# SDK binding provider facade
//
// C# context、静态调用和 delegate 包装转发到 provider；插件生命周期走共享 binding。

#include "sao/plugins/sdk_binding/binding_csharp.h"

#include "sao/plugins/loader/plugin_context.h"

#include <cstring>

namespace sao::plugins::sdk_binding {

namespace detail {
plugin_binding_handle_t make_binding(void* ctx, void* lang_state, int lang);
void free_binding(plugin_binding_handle_t plugin);
} // namespace detail

// 保留旧签名并转发到 provider。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_csharp_bind_ctx(csharp_domain_ptr domain, plugin_context_ptr ctx) {
    if (domain == nullptr || ctx == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.runtime = domain;
    request.context = reinterpret_cast<void*>(ctx);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::csharp, language_binding_operation::context_bind, &request);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_binding_csharp_call_static(
    csharp_domain_ptr domain, const char* assembly_qualified_class, const char* method_name,
    const char* args_json_utf8, char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr)
        *out_result_json_utf8 = nullptr;
    if (domain == nullptr || assembly_qualified_class == nullptr || method_name == nullptr ||
        args_json_utf8 == nullptr || out_result_json_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    language_binding_request request{};
    request.runtime = domain;
    request.name_utf8 = assembly_qualified_class;
    request.secondary_name_utf8 = method_name;
    request.input = reinterpret_cast<const uint8_t*>(args_json_utf8);
    request.input_size = std::strlen(args_json_utf8);
    request.out_object = reinterpret_cast<void**>(out_result_json_utf8);
    return sao_plugins_binding_dispatch_provider(language_host_kind::csharp,
                                                 language_binding_operation::static_call, &request);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_csharp_wrap_delegate(csharp_domain_ptr domain, void* delegate_gc_handle,
                                         void** out_sdk_callback_ptr, void** out_user_data) {
    if (out_sdk_callback_ptr != nullptr)
        *out_sdk_callback_ptr = nullptr;
    if (out_user_data != nullptr)
        *out_user_data = nullptr;
    if (domain == nullptr || delegate_gc_handle == nullptr || out_sdk_callback_ptr == nullptr ||
        out_user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    language_binding_request request{};
    request.runtime = domain;
    request.value = delegate_gc_handle;
    request.out_callback = out_sdk_callback_ptr;
    request.out_user_data = out_user_data;
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::csharp, language_binding_operation::callback_wrap, &request);
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_csharp_release_delegate(void* user_data) {
    sao_plugins_binding_release_callback(language_host_kind::csharp, user_data);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_csharp_ctx_register_menu_category(
    void* ctx, const char* name, const char* icon, void* builder_delegate, float priority) {
    return loader::sao_plugins_ctx_register_menu_category(
        static_cast<loader::plugin_context_t*>(ctx), name, icon, builder_delegate, priority,
        nullptr);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_menu_surface(void*, const char*, const char*, float) {
    return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_csharp_ctx_register_action_handler(void*,
                                                                                           void*) {
    return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

// 共享插件 binding 的 activate / deactivate

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_binding_csharp_activate(
    plugin_context_ptr plugin_ctx, csharp_domain_ptr domain, plugin_binding_handle_t* out_plugin) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    return sao_plugins_binding_plugin_load(language_host_kind::csharp,
                                           reinterpret_cast<void*>(plugin_ctx), domain, out_plugin);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_csharp_deactivate(plugin_binding_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return sao_plugins_binding_plugin_unload(plugin);
}

} // namespace sao::plugins::sdk_binding
