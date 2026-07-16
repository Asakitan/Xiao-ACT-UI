// lua_error.cpp — stub
#include "sao/plugins/lua_host/lua_error.h"

namespace sao::plugins::lua_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_take_error(lua_State* /*L*/, char** out_utf8) {
    if (out_utf8 != nullptr) *out_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::lua_host
