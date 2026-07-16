// lua_sandbox.cpp — stub
#include "sao/plugins/lua_host/lua_sandbox.h"

namespace sao::plugins::lua_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_arm(lua_State* /*L*/,
                                const lua_sandbox_config* /*cfg*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::lua_host
