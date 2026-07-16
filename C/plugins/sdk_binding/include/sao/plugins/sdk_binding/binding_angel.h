// binding_angel.h — SDK C ABI → AngelScript 真 SDK 桥
//
// **重要升级说明**: 本 host 用真的 angelscript.h + asIScriptEngine, **不再**是
// Python 侧的手写子集解释器。因此绑定层要用 asIScriptEngine::RegisterObjectType
// 把 PluginContext 注册为强类型 AS 类, 每个方法用 RegisterObjectMethod 注册。
//
// 对齐 Python 源: angel_runtime.py 的 _AngelScriptInterpreter (但 C++ 侧是升级
// 而非 1:1 移植 —— 加了真静态类型 / 类 / handle 语义)。
//
// AngelScript 侧访问模式 (对齐 example_angelscript_plugin/plugin.as):
//     void on_load(PluginContext@ c) {
//         g_ctx = c;
//         c.log("hello from AS");
//         c.register_ui_panel("id", { {"title", "Demo"} },
//                             render_panel, on_panel_action);
//     }
//
// 关键映射:
//   - Python dict → AS dictionary (via addon)
//   - Python list → AS array<T> or array<any>
//   - Python str → AS string (via std::string addon)
//   - callback → asIScriptFunction* + 反向调用 (asIScriptContext::Prepare/Execute)
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/sdk_binding/binding_common.h"

// 前置声明避免 angelscript.h 泄漏
class asIScriptEngine;
class asIScriptContext;
class asIScriptObject;
class asIScriptFunction;

namespace sao::plugins::sdk_binding {

typedef struct plugin_context_s* plugin_context_ptr;

// ── SDK 类型 / 方法注册 ─────────────────────────────

// 在 asIScriptEngine 上注册 SDK 类型 (PluginContext, dictionary, ui, ...)。
// angel_host 在 CreateScriptEngine 后立刻调这个。内部按顺序:
//   1. RegisterStdString (从 addon)
//   2. RegisterScriptArray
//   3. RegisterScriptDictionary
//   4. RegisterObjectType("PluginContext", 0, asOBJ_REF | asOBJ_NOCOUNT)
//   5. 遍历 sdk_method_id 对每一项 RegisterObjectMethod
//   6. 注册 UI 子对象 / mem 子对象 / engine 子对象
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_register_sdk(asIScriptEngine* engine);

// 每插件绑定一个 ctx handle 到 AS 侧的全局变量 g_ctx (即 PluginContext@)。
// 之后 AS 脚本里可直接 g_ctx.log(...) 或函数 on_load(PluginContext@ c) 拿到。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_bind_ctx(asIScriptEngine* engine,
                                   plugin_context_ptr ctx);

// 把 AS 函数 handle (asIScriptFunction*) 包装成 SDK 事件回调。
// 内部 AddRef 引用计数, 反调用时 Prepare + SetArgAddress + Execute。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_wrap_callback(asIScriptEngine* engine,
                                        asIScriptFunction* fn,
                                        void** out_sdk_callback_ptr,
                                        void** out_user_data);

// 释放 wrap_callback 分配的引用 (Release + delete user_data)。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_angel_release_callback(void* user_data);

// ── 值转换 ────────────────────────────────────────────

// AS dictionary/CScriptAny → utf-8 json (归属调用方 free)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_dict_to_json(void* as_dictionary_ptr,
                                       char** out_json_utf8);

// utf-8 json → AS dictionary (new-ref, 归属 AS 侧引用计数)。
extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_binding_angel_json_to_dict(asIScriptEngine* engine,
                                       const char* utf8_json);

// AS string → utf-8 (借用引用, 不拷贝, 用于 asCALL_THISCALL 的 self)。
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_binding_angel_string_view(void* as_string_ptr, size_t* out_len);

// ── SDK 方法在 AS 侧的 C 实现 ─── (由 RegisterObjectMethod 调用)
//
// 每条 as_ctx_<method> 都是 asCALL_THISCALL_ASGLOBAL / asCALL_CDECL_OBJFIRST
// 变体, 第一个参数是 PluginContext*, 内部 dispatch 到 sao_plugins_ctx_*.
//
// 完整暴露 (对齐 sdk_method_id 每一项):

extern "C" void as_ctx_log(void* self, const void* str);
extern "C" uint32_t as_ctx_subscribe(void* self, const void* topic,
                                     asIScriptFunction* cb);
extern "C" uint32_t as_ctx_subscribe_once(void* self, const void* topic,
                                          asIScriptFunction* cb);
extern "C" bool as_ctx_unsubscribe(void* self, uint32_t token);
extern "C" void as_ctx_emit(void* self, const void* topic, void* payload_dict);

extern "C" uint32_t as_ctx_on_damage(void* self, asIScriptFunction* cb);
extern "C" uint32_t as_ctx_on_heal(void* self, asIScriptFunction* cb);
extern "C" uint32_t as_ctx_on_skill(void* self, asIScriptFunction* cb);
extern "C" uint32_t as_ctx_on_boss(void* self, asIScriptFunction* cb);
extern "C" uint32_t as_ctx_on_snapshot(void* self, asIScriptFunction* cb);
extern "C" uint32_t as_ctx_on_encounter_finalized(void* self, asIScriptFunction* cb);

extern "C" void* as_ctx_get_snapshot(void* self);
extern "C" void* as_ctx_snapshot_value(void* self, const void* path);
extern "C" void* as_ctx_recent_events(void* self, uint32_t limit, const void* topic);

extern "C" void* as_ctx_get_setting(void* self, const void* key);
extern "C" void as_ctx_set_setting(void* self, const void* key, void* value);
extern "C" void as_ctx_set_defaults(void* self, void* defaults);

extern "C" void as_ctx_register_ui_panel(void* self, const void* panel_id,
                                          void* metadata,
                                          asIScriptFunction* render_cb,
                                          asIScriptFunction* action_cb);
extern "C" uint32_t as_ctx_register_render_hook(void* self, const void* surface,
                                                 float priority,
                                                 asIScriptFunction* cb);
extern "C" void as_ctx_set_overlay(void* self, const void* surface, void* spec);
extern "C" void as_ctx_clear_overlay(void* self, const void* surface);
extern "C" void as_ctx_request_redraw(void* self, const void* surface,
                                       const void* reason);
extern "C" void as_ctx_register_hotkey(void* self, const void* hotkey_id,
                                        const void* default_key,
                                        const void* label,
                                        asIScriptFunction* cb);
extern "C" void as_ctx_register_engine(void* self, const void* name,
                                        void* engine_obj);
extern "C" void as_ctx_register_data_source(void* self, const void* source_id,
                                             void* metadata,
                                             asIScriptFunction* start,
                                             asIScriptFunction* stop);
extern "C" void as_ctx_register_parser_adapter(void* self, const void* id,
                                                void* metadata,
                                                asIScriptFunction* handler);
extern "C" void as_ctx_register_exporter(void* self, const void* id,
                                          void* metadata,
                                          asIScriptFunction* handler);
extern "C" void as_ctx_register_formatter(void* self, const void* id,
                                           void* metadata,
                                           asIScriptFunction* handler);
extern "C" void as_ctx_register_trigger_type(void* self, const void* id,
                                              void* metadata,
                                              asIScriptFunction* handler);
extern "C" void as_ctx_register_report_view(void* self, const void* id,
                                             void* metadata,
                                             asIScriptFunction* handler);
extern "C" void as_ctx_register_timer(void* self, const void* id,
                                        void* metadata,
                                        asIScriptFunction* handler);
extern "C" void as_ctx_register_menu_category(void* self, const void* name,
                                                const void* icon,
                                                asIScriptFunction* builder,
                                                float priority);
extern "C" void as_ctx_register_menu_surface(void* self, const void* id,
                                               void* descriptor,
                                               float priority);
extern "C" void as_ctx_register_action_handler(void* self,
                                                 asIScriptFunction* handler);

extern "C" void* as_ctx_set_interval(void* self, asIScriptFunction* cb, double s);
extern "C" void* as_ctx_set_timeout(void* self, asIScriptFunction* cb, double s);
extern "C" bool as_ctx_clear_timer(void* self, const void* token);
extern "C" void as_ctx_run_on_ui(void* self, asIScriptFunction* cb);

extern "C" void as_ctx_notify(void* self, const void* title, const void* msg,
                                 double duration_s, const void* kind);
extern "C" void as_ctx_toast(void* self, const void* message);
extern "C" void as_ctx_open_window(void* self, const void* panel_id,
                                       uint32_t w, uint32_t h);

extern "C" void as_ctx_create_compositor_layer(void* self, const void* name,
                                                  uint32_t w, uint32_t h,
                                                  int32_t x, int32_t y, int32_t z);
extern "C" void as_ctx_upload_compositor_frame(void* self, const void* name,
                                                 void* buffer);
extern "C" void as_ctx_destroy_compositor_layer(void* self, const void* name);

extern "C" void* as_ctx_get_engine(void* self, const void* name);
extern "C" void* as_ctx_call_engine(void* self, const void* engine_name,
                                    const void* method, void* args);
extern "C" void as_ctx_ensure_requirements(void* self, bool install);
extern "C" void* as_ctx_load_local(void* self, const void* relative);

// ── Wave 4 新增: 激活 AngelScript 侧 binding ─────────────────
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_activate(plugin_context_ptr plugin_ctx,
                                   asIScriptEngine* engine,
                                   plugin_binding_handle_t* out_plugin);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_angel_deactivate(plugin_binding_handle_t plugin);

} // namespace sao::plugins::sdk_binding
