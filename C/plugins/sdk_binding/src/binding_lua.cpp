// binding_lua.cpp — Wave 4 首切片
//
// activate() 概念: lua_newuserdata 塞 ctx_ptr + metatable。Wave 4 只记账,
// 真 Lua userdata 集成留 Wave 5。

#include "sao/plugins/sdk_binding/binding_lua.h"

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
sao_plugins_binding_lua_register_sdk(lua_State* L, plugin_context_ptr ctx) {
    if (L == nullptr || ctx == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.runtime = L;
    request.context = reinterpret_cast<void*>(ctx);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::lua, language_binding_operation::runtime_register,
        &request);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_stack_to_json(lua_State* L, int index, char** out_json_utf8) {
    if (out_json_utf8 != nullptr) *out_json_utf8 = nullptr;
    if (L == nullptr || out_json_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.runtime = L;
    request.index = index;
    request.out_object = reinterpret_cast<void**>(out_json_utf8);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::lua, language_binding_operation::value_to_json,
        &request);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_json_to_stack(lua_State* L, const char* utf8_json) {
    if (L == nullptr || utf8_json == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.runtime = L;
    request.input = reinterpret_cast<const uint8_t*>(utf8_json);
    request.input_size = std::strlen(utf8_json);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::lua, language_binding_operation::json_to_value,
        &request);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_wrap_callback(lua_State* L,
                                      int fn_index,
                                      void** out_sdk_callback_ptr,
                                      void** out_user_data) {
    if (out_sdk_callback_ptr != nullptr) *out_sdk_callback_ptr = nullptr;
    if (out_user_data != nullptr) *out_user_data = nullptr;
    if (L == nullptr || out_sdk_callback_ptr == nullptr || out_user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    language_binding_request request{};
    request.runtime = L;
    request.index = fn_index;
    request.out_callback = out_sdk_callback_ptr;
    request.out_user_data = out_user_data;
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::lua, language_binding_operation::callback_wrap,
        &request);
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_lua_release_callback(void* /*user_data*/) {
    // no-op
}

// Wave 4 新增: activate / deactivate

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_activate(plugin_context_ptr plugin_ctx,
                                 lua_State* L,
                                 plugin_binding_handle_t* out_plugin) {
    if (out_plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    return sao_plugins_binding_plugin_load(
        language_host_kind::lua, reinterpret_cast<void*>(plugin_ctx), L,
        out_plugin);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_deactivate(plugin_binding_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    return sao_plugins_binding_plugin_unload(plugin);
}

} // namespace sao::plugins::sdk_binding
