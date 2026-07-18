#include "sao/plugins/lua_host/lua_error.h"

#include <cstdlib>
#include <cstring>
#include <string>

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "lua_state_internal.h"
#endif

namespace sao::plugins::lua_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_take_error(lua_State* state, char** out_utf8) {
    if (out_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_utf8 = nullptr;
#if defined(SAO_HAS_LUA)
    if (state == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        detail::state_operation operation;
        const int32_t status =
            detail::acquire_state_operation(state, operation);
        if (status != SAO_OK) return status;
        if (lua_gettop(state) == 0) {
            std::string error = detail::take_state_error_locked(state);
            if (error.empty()) {
                return SAO_ERR_HANDLE_INVALID;
            }
            char* copy = static_cast<char*>(std::malloc(error.size() + 1));
            if (copy == nullptr) return SAO_ERR_OS_CALL_FAILED;
            if (!error.empty()) {
                std::memcpy(copy, error.data(), error.size());
            }
            copy[error.size()] = '\0';
            *out_utf8 = copy;
            return SAO_OK;
        }
        const int original_top = lua_gettop(state);
        size_t length = 0;
        const char* message = lua_type(state, -1) == LUA_TSTRING
                      ? lua_tolstring(state, -1, &length)
                      : nullptr;
        if (message == nullptr) {
            if (detail::protected_tostring(state, -1) != LUA_OK) {
                detail::capture_state_error_locked(state, -1);
                lua_settop(state, original_top - 1);
                return SAO_ERR_OS_CALL_FAILED;
            }
            message = lua_tolstring(state, -1, &length);
        }
        if (message == nullptr) {
            lua_settop(state, original_top - 1);
            return SAO_ERR_OS_CALL_FAILED;
        }
        char* copy = static_cast<char*>(std::malloc(length + 1));
        if (copy == nullptr) {
            lua_settop(state, original_top - 1);
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (length != 0) std::memcpy(copy, message, length);
        copy[length] = '\0';
        lua_settop(state, original_top - 1);
        *out_utf8 = copy;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    (void)state;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

} // namespace sao::plugins::lua_host
