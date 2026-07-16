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
#endif

namespace sao::plugins::lua_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_install_stdlib(lua_State* L, const lua_stdlib_config* cfg) {
    if (L == nullptr || cfg == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    if (cfg->base)      { luaL_requiref(L, LUA_GNAME,        luaopen_base,       1); lua_pop(L, 1); }
    if (cfg->coroutine) { luaL_requiref(L, LUA_COLIBNAME,    luaopen_coroutine,  1); lua_pop(L, 1); }
    if (cfg->string_)   { luaL_requiref(L, LUA_STRLIBNAME,   luaopen_string,     1); lua_pop(L, 1); }
    if (cfg->table_)    { luaL_requiref(L, LUA_TABLIBNAME,   luaopen_table,      1); lua_pop(L, 1); }
    if (cfg->math)      { luaL_requiref(L, LUA_MATHLIBNAME,  luaopen_math,       1); lua_pop(L, 1); }
    if (cfg->utf8)      { luaL_requiref(L, LUA_UTF8LIBNAME,  luaopen_utf8,       1); lua_pop(L, 1); }
    if (cfg->io)        { luaL_requiref(L, LUA_IOLIBNAME,    luaopen_io,         1); lua_pop(L, 1); }
    if (cfg->os)        { luaL_requiref(L, LUA_OSLIBNAME,    luaopen_os,         1); lua_pop(L, 1); }
    if (cfg->package_)  { luaL_requiref(L, LUA_LOADLIBNAME,  luaopen_package,    1); lua_pop(L, 1); }
    if (cfg->debug_)    { luaL_requiref(L, LUA_DBLIBNAME,    luaopen_debug,      1); lua_pop(L, 1); }
    return SAO_OK;
#else
    (void)cfg;
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
