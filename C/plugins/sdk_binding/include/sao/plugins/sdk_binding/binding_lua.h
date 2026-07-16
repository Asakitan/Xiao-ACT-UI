// binding_lua.h — SDK C ABI → Lua 5.4 真 C API 桥
//
// **重要升级说明**: 相比 Python 侧的 lupa 双跳 (Python -> lupa -> LuaJIT/5.x),
// C++ 侧直接用真 Lua 5.4 C API (lua_State*, luaL_register)。也不必再 lock 到
// lua54 绑定 (Python 侧因 lupa 顶层默认 5.5 beta 属性缓存 bug 才要锁; 见项目
// 记忆 project_script_plugin_ui_pitfalls)。
//
// 对齐 Python 源: lua_runtime.py 的 _LuaProxy (每个 ctx 方法都手动包 Lua callback)。
//
// Lua 侧访问模式:
//     ctx:log("hello")                    -- colon call (auto-passes self)
//     ctx:register_ui_panel("id", { title = "Demo" }, render, on_action)
//     ctx:subscribe("damage", function(payload) ... end)
//
// 关键坑 (对齐项目记忆 project_script_plugin_ui_pitfalls):
//   - Lua 表 → Python 转换按数字 key 1..N → list, 其他 → dict
//   - Python 对象上的 method 传给 Lua 时, Lua 侧必须用点号调用 (dot)
//     不是冒号 (colon), 否则会重复传 self (lua54 quirk)
//   - Lua 5.5 beta 有属性缓存 bug → C++ 侧只支持 5.4 (LUA_VERSION_NUM 504)
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/sdk_binding/binding_common.h"

// Lua 前置声明
struct lua_State;

namespace sao::plugins::sdk_binding {

typedef struct plugin_context_s* plugin_context_ptr;

// ── SDK 表注册 ────────────────────────────────────────

// 在 lua_State 上注册 SDK 表 (作为 ctx 全局)。
// 内部用 lua_newtable + 遍历 sdk_method_id 每一项 lua_pushcfunction 注册。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_register_sdk(lua_State* L, plugin_context_ptr ctx);

// 装 ctx.ui 子表 (Lua 侧 ctx.ui.panel / ctx.ui.text / ...)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_register_ui(lua_State* L);

// 装 ctx.mem 子表 (只读内存 API)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_register_mem(lua_State* L, plugin_context_ptr ctx);

// 装 ctx.engine 子表 (跨插件 engine 访问)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_register_engine(lua_State* L, plugin_context_ptr ctx);

// ── 值转换 ────────────────────────────────────────────

// Lua 栈项 (index) → utf-8 json。
// 表按数字键 1..N 连续 → JSON array, 否则 → JSON object。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_stack_to_json(lua_State* L,
                                      int index,
                                      char** out_json_utf8);

// utf-8 json → Lua 栈顶新项 (成功后栈 +1)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_json_to_stack(lua_State* L, const char* utf8_json);

// 把栈项 (index) 里的 Lua 函数 ref 包装成 SDK 回调 (存 lua_ref, 之后
// luaL_ref 回收)。回调用 out_sdk_callback_ptr + out_user_data 存到平台侧
// (register_hotkey / subscribe 等的 callback + user_data 槽)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_wrap_callback(lua_State* L,
                                      int fn_index,
                                      void** out_sdk_callback_ptr,
                                      void** out_user_data);

// 释放 wrap_callback 分配的 ref (unload 时调, 或 register_XXX 返回失败时清理)。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_lua_release_callback(void* user_data);

// ── SDK 方法在 Lua 侧的 C 实现 ── (每一条 lua_CFunction 一对一映射)
//
// 每条实现从 Lua 栈拿参数, 调 sao_plugins_ctx_* API, 结果 push 回栈。
// 用 luaL_check* 校验参数类型, 错误抛 lua_error。
//
// 命名约定: lua_ctx_<method_name>, 内部用 lua_pushcclosure 挂到 ctx 表。

extern "C" int lua_ctx_log(lua_State* L);
extern "C" int lua_ctx_subscribe(lua_State* L);
extern "C" int lua_ctx_subscribe_once(lua_State* L);
extern "C" int lua_ctx_unsubscribe(lua_State* L);
extern "C" int lua_ctx_emit(lua_State* L);
extern "C" int lua_ctx_on_damage(lua_State* L);
extern "C" int lua_ctx_on_heal(lua_State* L);
extern "C" int lua_ctx_on_skill(lua_State* L);
extern "C" int lua_ctx_on_boss(lua_State* L);
extern "C" int lua_ctx_on_snapshot(lua_State* L);
extern "C" int lua_ctx_on_encounter_finalized(lua_State* L);
extern "C" int lua_ctx_get_snapshot(lua_State* L);
extern "C" int lua_ctx_snapshot_value(lua_State* L);
extern "C" int lua_ctx_recent_events(lua_State* L);

extern "C" int lua_ctx_get_setting(lua_State* L);
extern "C" int lua_ctx_setting(lua_State* L);           // alias
extern "C" int lua_ctx_set_setting(lua_State* L);
extern "C" int lua_ctx_set_defaults(lua_State* L);

extern "C" int lua_ctx_register_parser_adapter(lua_State* L);
extern "C" int lua_ctx_register_exporter(lua_State* L);
extern "C" int lua_ctx_register_formatter(lua_State* L);
extern "C" int lua_ctx_register_trigger_type(lua_State* L);
extern "C" int lua_ctx_register_report_view(lua_State* L);
extern "C" int lua_ctx_register_timer(lua_State* L);
extern "C" int lua_ctx_register_ui_panel(lua_State* L);
extern "C" int lua_ctx_register_render_hook(lua_State* L);
extern "C" int lua_ctx_set_overlay(lua_State* L);
extern "C" int lua_ctx_clear_overlay(lua_State* L);
extern "C" int lua_ctx_register_hotkey(lua_State* L);
extern "C" int lua_ctx_register_engine(lua_State* L);
extern "C" int lua_ctx_register_data_source(lua_State* L);
extern "C" int lua_ctx_register_menu_category(lua_State* L);
extern "C" int lua_ctx_register_menu_surface(lua_State* L);
extern "C" int lua_ctx_register_action_handler(lua_State* L);
extern "C" int lua_ctx_request_redraw(lua_State* L);

extern "C" int lua_ctx_set_interval(lua_State* L);
extern "C" int lua_ctx_set_timeout(lua_State* L);
extern "C" int lua_ctx_clear_timer(lua_State* L);
extern "C" int lua_ctx_run_on_ui(lua_State* L);

extern "C" int lua_ctx_notify(lua_State* L);
extern "C" int lua_ctx_dismiss_notify(lua_State* L);
extern "C" int lua_ctx_toast(lua_State* L);
extern "C" int lua_ctx_open_file(lua_State* L);
extern "C" int lua_ctx_open_window(lua_State* L);

extern "C" int lua_ctx_create_compositor_layer(lua_State* L);
extern "C" int lua_ctx_upload_compositor_frame(lua_State* L);
extern "C" int lua_ctx_set_compositor_layer_position(lua_State* L);
extern "C" int lua_ctx_set_compositor_layer_visible(lua_State* L);
extern "C" int lua_ctx_destroy_compositor_layer(lua_State* L);

extern "C" int lua_ctx_get_engine(lua_State* L);
extern "C" int lua_ctx_require_engine(lua_State* L);
extern "C" int lua_ctx_call_engine(lua_State* L);
extern "C" int lua_ctx_call_runtime(lua_State* L);
extern "C" int lua_ctx_ensure_requirements(lua_State* L);
extern "C" int lua_ctx_load_local(lua_State* L);

// ── Wave 4 新增: 激活 Lua 侧 binding ─────────────────────────
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_activate(plugin_context_ptr plugin_ctx,
                                 lua_State* L,
                                 plugin_binding_handle_t* out_plugin);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_lua_deactivate(plugin_binding_handle_t plugin);

} // namespace sao::plugins::sdk_binding
