// lua_call.h — 从 C++ 调 Lua 函数 (hook 分派)
//
// Lua 侧的 on_load / on_enable / on_disable / on_unload 是 global functions。
// 加载后从 lua globals 抽出 luaL_ref 存 registry, 生命周期钩子时 lua_rawgeti
// 拿回来执行。
//
// 对齐 Python 源: lua_runtime.py 里 lua.globals()[hook_name] 取 + call, 只是
// C++ 侧走真 Lua 5.4 C API 不经过 lupa。
//
// Lua 侧访问模式 (对齐 example_lua_plugin/plugin.lua):
//     local click_count = 0
//     function on_load(ctx)
//         ctx:log("hello from lua")
//         ctx:register_ui_panel("id", { title = "Demo" }, render, on_action)
//         ctx:subscribe("damage", function(payload) ... end)
//     end
//     function render(payload) ... end
//     function on_action(action_id, payload) ... end
//     function on_unload() end
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

struct lua_State;

namespace sao::plugins::lua_host {

typedef struct lua_plugin_s* lua_plugin_handle_t;

// 加载 .lua 脚本 (luaL_loadfile + lua_pcall 顶层, 把定义的 on_load 等 hook 存
// LUA_REGISTRYINDEX)。
// 内部:
//   1. lua_getglobal(L, "package"), 前插 <plugin>/libs / vendor 到 package.path
//   2. luaL_loadfile(L, entry_absolute)
//   3. lua_pcall(L, 0, LUA_MULTRET, 0) — 跑顶层
//   4. lua_getglobal 抽 on_load / on_enable / on_disable / on_unload,
//      luaL_ref 存 registry ref
//   5. 存 registry refs 到 lua_plugin_handle_t
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_load_script(lua_State* L,
                                const wchar_t* plugin_dir,
                                const char* entry_relative,
                                const char* plugin_id_utf8,
                                void* ctx_ptr,     // plugin_context_t*
                                lua_plugin_handle_t* out_plugin);

// 生命周期 hook 调用。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_load(lua_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_enable(lua_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_disable(lua_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_unload(lua_plugin_handle_t plugin,
                                   bool* out_allow_unload);

// 通用 hook 调用: args_json 是 json 数组; 结果 json (归属调用方 free)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_hook(lua_plugin_handle_t plugin,
                              const char* hook_name,
                              const char* args_json_utf8,
                              char** out_result_json_utf8);

// 查该插件是否有指定 hook。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_luahost_has_hook(lua_plugin_handle_t plugin, const char* hook_name);

// 卸载 (luaL_unref 释放 registry 里存的 hook)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_unload_script(lua_plugin_handle_t plugin);

// 调 lua 里的任意函数 by ref (由 wrap_callback 保存的 luaL_ref)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_function_by_ref(lua_State* L,
                                         int registry_ref,
                                         const char* args_json_utf8,
                                         char** out_result_json_utf8);

} // namespace sao::plugins::lua_host
