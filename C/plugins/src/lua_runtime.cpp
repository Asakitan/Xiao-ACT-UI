#include "sao_plugins/lua_runtime.h"

#include "logging.h"

#include <new>

#include "sao_plugins/sao_status.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

struct sao_plugins_lua_s {
    lua_State* state;
};

namespace {

constexpr char kComponent[] = "plugins.lua";

int32_t finish(int32_t status, const char* message) noexcept {
    sao::legacy_plugins::emit_log(status == SAO_OK ? sao::legacy_plugins::kLogLevelInfo
                                                   : sao::legacy_plugins::kLogLevelError,
                                  kComponent, status, message);
    return status;
}

int32_t fail(int32_t status, const char* message) noexcept {
    sao::legacy_plugins::emit_log(sao::legacy_plugins::kLogLevelError, kComponent, status, message);
    return status;
}

} // namespace

extern "C" int32_t SAO_PLUGINS_CALL sao_plugins_lua_create(sao_plugins_lua_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "lua_create requires an output handle");
    }
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        *out_handle = nullptr;
        return fail(SAO_ERR_OS_CALL_FAILED, "lua_create failed to allocate a Lua state");
    }
    *out_handle = new (std::nothrow) sao_plugins_lua_s{state};
    if (*out_handle == nullptr) {
        lua_close(state);
        return fail(SAO_ERR_OS_CALL_FAILED, "lua_create failed to allocate its facade handle");
    }
    return finish(SAO_OK, "lua_create completed");
}

extern "C" void SAO_PLUGINS_CALL sao_plugins_lua_destroy(sao_plugins_lua_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
    if (handle->state != nullptr) {
        lua_close(handle->state);
    }
    delete handle;
    (void)finish(SAO_OK, "lua_destroy completed");
}

extern "C" int32_t SAO_PLUGINS_CALL sao_plugins_lua_run_script(sao_plugins_lua_handle_t handle,
                                                               const char* script_utf8,
                                                               size_t script_len) {
    if (handle == nullptr || handle->state == nullptr || script_utf8 == nullptr) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "lua_run_script received invalid arguments");
    }
    if (luaL_loadbuffer(handle->state, script_utf8, script_len, "sao_plugin") != LUA_OK) {
        lua_pop(handle->state, 1);
        return fail(SAO_ERR_INVALID_ARGUMENT, "lua_run_script failed to compile the script");
    }
    if (lua_pcall(handle->state, 0, LUA_MULTRET, 0) != LUA_OK) {
        lua_pop(handle->state, 1);
        return fail(SAO_ERR_OS_CALL_FAILED, "lua_run_script failed while executing the script");
    }
    return SAO_OK;
}
