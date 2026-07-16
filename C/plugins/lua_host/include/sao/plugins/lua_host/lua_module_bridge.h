// lua_module_bridge.h — 用 luaL_register 注册 SDK 全局表
//
// 把 sdk_binding/binding_lua 的 lua_ctx_* 函数按 sdk_method_id 全数
// 注册到 lua_State, 通过 userdata + metatable 挂到全局 ``ctx``。
//
// Lua 侧访问模式 (对齐 example_lua_plugin/plugin.lua):
//     ctx:log("hi")              -- 冒号自动传 self (userdata)
//     ctx:register_ui_panel(...)  -- 同上
//
// 关键坑 (对齐项目记忆 project_script_plugin_ui_pitfalls):
//   - lupa 在 Python 侧: Python 对象的方法必须用点号 (ctx.register(...))
//     而非冒号 (会重复 self)
//   - 但真 Lua 5.4 C API 侧, ctx 是 userdata + __index metamethod, 所以
//     冒号语法 ctx:register(...) 是正确的 (auto self)
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

struct lua_State;

namespace sao::plugins::lua_host {

// 在 lua_State 上创建 ctx 全局。
// 内部:
//   1. lua_newuserdata(sizeof(void*)) — 存 plugin_context_t*
//   2. luaL_newmetatable("SaoPluginContext") — 建 metatable
//   3. lua_pushcfunction(__index) — 用 sdk_binding 的 lua_ctx_* 表
//   4. lua_setmetatable — 关联
//   5. lua_setglobal("ctx") — 挂全局
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_ctx(lua_State* L,
                                 void* ctx_handle);   // plugin_context_t*

// 注册 ctx.ui 子表 (panel / text / kv / row / button / badge / bar /
// divider / table / section)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_ui(lua_State* L);

// 注册 ctx.mem 子表 (read_u32 / read_u64 / read_ptr_chain / module_base)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_mem(lua_State* L, void* ctx_handle);

// 注册 ctx.engine 子表 (跨插件 engine 访问 __index 派发)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_engine(lua_State* L, void* ctx_handle);

// 注册 python + require_python 别名 (对齐 lua_runtime.py 的 lupa 全局)。
// 只在 permissions 含 "unsafe" 时开; 让老 Lua 插件 python.print / require_python
// 之类的调用能兼容 (转发到 sao_log)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_python_compat(lua_State* L, bool enable);

} // namespace sao::plugins::lua_host
