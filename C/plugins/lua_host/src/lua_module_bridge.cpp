// lua_module_bridge.cpp — SDK 表注册 (stub; wave3 未涉及, 后续 wave 实装)

#include "sao/plugins/lua_host/lua_module_bridge.h"

namespace sao::plugins::lua_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_ctx(lua_State* /*L*/, void* /*ctx_handle*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_ui(lua_State* /*L*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_mem(lua_State* /*L*/, void* /*ctx_handle*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_engine(lua_State* /*L*/, void* /*ctx_handle*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_python_compat(lua_State* /*L*/, bool /*enable*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::lua_host
