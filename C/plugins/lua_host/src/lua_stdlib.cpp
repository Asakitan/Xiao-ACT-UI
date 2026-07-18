// lua_stdlib.cpp — Lua 标准库装载
//
// SAO_HAS_LUA gate: 真装 luaL_open* 白名单; 未 gated 时全 stub。

#include "sao/plugins/lua_host/lua_stdlib.h"

#include <cstring>

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include "lua_sandbox_internal.h"
#include "lua_state_internal.h"
#endif

namespace sao::plugins::lua_host {

#if defined(SAO_HAS_LUA)
namespace {

int install_stdlib_body(lua_State* state) {
    const auto* config = static_cast<const lua_stdlib_config*>(
        lua_touserdata(state, 1));
    if (config->base) {
        luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
        lua_pop(state, 1);
    }
    if (config->coroutine) {
        luaL_requiref(state, LUA_COLIBNAME, luaopen_coroutine, 1);
        lua_pop(state, 1);
    }
    if (config->string_) {
        luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(state, 1);
    }
    if (config->table_) {
        luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(state, 1);
    }
    if (config->math) {
        luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(state, 1);
    }
    if (config->utf8) {
        luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(state, 1);
    }
    if (config->io) {
        luaL_requiref(state, LUA_IOLIBNAME, luaopen_io, 1);
        lua_pop(state, 1);
    }
    if (config->os) {
        luaL_requiref(state, LUA_OSLIBNAME, luaopen_os, 1);
        lua_pop(state, 1);
    }
    if (config->package_) {
        luaL_requiref(state, LUA_LOADLIBNAME, luaopen_package, 1);
        lua_pop(state, 1);
    }
    if (config->debug_) {
        luaL_requiref(state, LUA_DBLIBNAME, luaopen_debug, 1);
        lua_pop(state, 1);
    }
    return 0;
}

} // namespace
#endif

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_install_stdlib(lua_State* state,
                                   const lua_stdlib_config* config) {
    if (state == nullptr || config == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    try {
        detail::state_operation operation;
        int32_t status =
            detail::acquire_state_operation(state, operation);
        if (status != SAO_OK) return status;
        if (!detail::sandbox_stdlib_allowed_locked(state)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const int base = lua_gettop(state);
        if (detail::protected_trampoline(
                state, install_stdlib_body,
                const_cast<lua_stdlib_config*>(config), 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            return SAO_ERR_OS_CALL_FAILED;
        }
        lua_settop(state, base);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    (void)config;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_install_stdlib_flags(lua_State* L,
                                          bool base,
                                          bool string_lib,
                                          bool table_lib,
                                          bool math,
                                          bool coroutine,
                                          bool io,
                                          bool os,
                                          bool package_lib) {
    lua_stdlib_config cfg{};
    cfg.base = base;
    cfg.string_ = string_lib;
    cfg.table_ = table_lib;
    cfg.math = math;
    cfg.coroutine = coroutine;
    cfg.io = io;
    cfg.os = os;
    cfg.package_ = package_lib;
    return sao_plugins_luahost_install_stdlib(L, &cfg);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_install_one_stdlib(lua_State* L, const char* libname) {
    if (L == nullptr || libname == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    lua_stdlib_config cfg{};
    // 全 off, 按 libname 打开一个
    cfg.base = cfg.coroutine = cfg.string_ = cfg.table_ = cfg.math =
        cfg.utf8 = cfg.io = cfg.os = cfg.package_ = cfg.debug_ = false;
    if (std::strcmp(libname, LUA_GNAME) == 0)         cfg.base = true;
    else if (std::strcmp(libname, LUA_COLIBNAME) == 0) cfg.coroutine = true;
    else if (std::strcmp(libname, LUA_STRLIBNAME) == 0) cfg.string_ = true;
    else if (std::strcmp(libname, LUA_TABLIBNAME) == 0) cfg.table_ = true;
    else if (std::strcmp(libname, LUA_MATHLIBNAME) == 0) cfg.math = true;
    else if (std::strcmp(libname, LUA_UTF8LIBNAME) == 0) cfg.utf8 = true;
    else if (std::strcmp(libname, LUA_IOLIBNAME) == 0) cfg.io = true;
    else if (std::strcmp(libname, LUA_OSLIBNAME) == 0) cfg.os = true;
    else if (std::strcmp(libname, LUA_LOADLIBNAME) == 0) cfg.package_ = true;
    else if (std::strcmp(libname, LUA_DBLIBNAME) == 0)   cfg.debug_ = true;
    else return SAO_ERR_INVALID_ARGUMENT;
    return sao_plugins_luahost_install_stdlib(L, &cfg);
#else
    (void)libname;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_install_sao_stdlib(lua_State* /*L*/) {
    // 后续 wave 实装 sao_json / sao_log / sao_time / sao_sleep
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::lua_host
