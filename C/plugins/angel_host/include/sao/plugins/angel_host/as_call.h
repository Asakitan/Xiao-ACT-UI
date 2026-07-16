// as_call.h — 从 C++ 调 AngelScript 函数 (on_load / on_enable / ...)
//
// AngelScript 侧的函数由 asIScriptContext::Prepare + SetArg + Execute 调用。
// 生命周期钩子的签名:
//   void on_load(PluginContext@ c)         — 拿 ctx handle
//   void on_enable()                        — 无参
//   void on_disable()                       — 无参
//   void on_unload()                        — 无参
//
// 面板 hook:
//   dictionary@ render_panel(dictionary@ payload)
//   dictionary@ on_panel_action(string action_id, dictionary@ payload)
//
// 对齐 Python 源: angel_runtime.py 的 hook 提取 (interp.get_function) + call。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

class asIScriptEngine;
class asIScriptContext;
class asIScriptModule;
class asIScriptFunction;

namespace sao::plugins::angel_host {

typedef struct as_plugin_s* as_plugin_handle_t;

// 加载 .as 脚本 (Build 到 module)。
// 内部:
//   1. CScriptBuilder builder
//   2. builder.StartNewModule(engine, module_name)
//   3. builder.AddSectionFromFile(entry_relative)
//   4. builder.BuildModule()
//   5. 抽 on_load / on_enable / on_disable / on_unload 函数指针
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_load_script(asIScriptEngine* engine,
                               const wchar_t* plugin_dir,
                               const char* entry_relative,
                               const char* plugin_id_utf8,
                               void* ctx_ptr,       // plugin_context_t*
                               as_plugin_handle_t* out_plugin);

// 生命周期 hook 调用。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_load(as_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_enable(as_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_disable(as_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_unload(as_plugin_handle_t plugin,
                                  bool* out_allow_unload);

// 通用 hook: args_json 是 hook 参数的 json 数组; 返回 json (归属调用方 free)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_hook(as_plugin_handle_t plugin,
                             const char* hook_name,
                             const char* args_json_utf8,
                             char** out_result_json_utf8);

// 查该插件是否有指定 hook (module->GetFunctionByName)。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_has_hook(as_plugin_handle_t plugin, const char* hook_name);

// 调 asIScriptFunction* (由 callback wrap 保存的) 反向 hook。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_function(asIScriptEngine* engine,
                                 asIScriptFunction* fn,
                                 const char* args_json_utf8,
                                 char** out_result_json_utf8);

// 卸载 (Release module + context)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unload_script(as_plugin_handle_t plugin);

// 拿到 module 指针 (给 sdk_binding 用)。
extern "C" SAO_PLUGINS_API asIScriptModule* SAO_PLUGINS_CALL
sao_plugins_ashost_get_module(as_plugin_handle_t plugin);

} // namespace sao::plugins::angel_host
