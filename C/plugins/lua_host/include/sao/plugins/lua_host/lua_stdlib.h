// lua_stdlib.h — Lua 标准库注入 (按沙箱选装)
//
// Lua 5.4 官方 stdlib 表:
//   - base          (LUA_GNAME)     print, type, ipairs, pairs, error, pcall, ...
//   - coroutine     (LUA_COLIBNAME)  create, resume, yield, ...
//   - string        (LUA_STRLIBNAME) format, gsub, find, match, sub, ...
//   - table         (LUA_TABLIBNAME) insert, remove, concat, sort, ...
//   - math          (LUA_MATHLIBNAME) sin, cos, sqrt, floor, ...
//   - io            (LUA_IOLIBNAME)  open, read, write, close (需要 allow_fs)
//   - os            (LUA_OSLIBNAME)  time, date, execute, exit (execute 需要 allow_process)
//   - package       (LUA_LOADLIBNAME) require, loadlib, path, cpath (需要 allow_require)
//   - utf8          (LUA_UTF8LIBNAME) char, codepoint, len, offset
//   - debug         (LUA_DBLIBNAME)  getinfo, traceback (通常留给 dev tools)
//
// 沙箱默认策略 (对齐 project_script_plugin_ui_pitfalls):
//   - base / string / table / math / coroutine / utf8: 默认开
//   - io / os / package: 按 permissions 开
//   - debug: 只在开发模式开
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

struct lua_State;

namespace sao::plugins::lua_host {

struct lua_stdlib_config {
    bool base = true;
    bool coroutine = true;
    bool string_ = true;
    bool table_ = true;
    bool math = true;
    bool utf8 = true;
    bool io = false;
    bool os = false;
    bool package_ = false;
    bool debug_ = false;
};

// 装官方 stdlib: luaL_openlibs 的白名单版本 (按 sandbox_config 决定装哪些)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_install_stdlib(lua_State* L, const lua_stdlib_config* cfg);

// 兼容旧签名 (仍导出以避免破坏 loader 里旧代码)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_install_stdlib_flags(lua_State* L,
                                          bool base,
                                          bool string_lib,
                                          bool table_lib,
                                          bool math,
                                          bool coroutine,
                                          bool io,
                                          bool os,
                                          bool package_lib);

// 单独装某个官方库 (给测试 / 调试用)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_install_one_stdlib(lua_State* L, const char* libname);

// 装 sao 扩展 stdlib:
//   - sao_json.encode / sao_json.decode
//   - sao_log(msg)
//   - sao_time()
//   - sao_sleep(s) — 慎用
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_install_sao_stdlib(lua_State* L);

} // namespace sao::plugins::lua_host
