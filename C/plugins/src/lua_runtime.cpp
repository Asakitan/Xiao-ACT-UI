#include "sao_plugins/lua_runtime.h"

#include <new>

#include "sao_plugins/sao_status.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

struct sao_plugins_lua_s {
    lua_State* state;
};

extern "C" int32_t SAO_PLUGINS_CALL sao_plugins_lua_create(sao_plugins_lua_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        *out_handle = nullptr;
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_handle = new (std::nothrow) sao_plugins_lua_s{state};
    if (*out_handle == nullptr) {
        lua_close(state);
        return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
}

extern "C" void SAO_PLUGINS_CALL sao_plugins_lua_destroy(sao_plugins_lua_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
    if (handle->state != nullptr) {
        lua_close(handle->state);
    }
    delete handle;
}

extern "C" int32_t SAO_PLUGINS_CALL sao_plugins_lua_run_script(
    sao_plugins_lua_handle_t handle,
    const char* script_utf8,
    size_t script_len) {
    if (handle == nullptr || handle->state == nullptr || script_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (luaL_loadbuffer(handle->state, script_utf8, script_len,
                        "sao_plugin") != LUA_OK) {
        lua_pop(handle->state, 1);
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (lua_pcall(handle->state, 0, LUA_MULTRET, 0) != LUA_OK) {
        lua_pop(handle->state, 1);
        return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
}
