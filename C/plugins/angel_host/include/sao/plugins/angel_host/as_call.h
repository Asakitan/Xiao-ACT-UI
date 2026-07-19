// as_call.h — 从 C++ 调 AngelScript 函数 (on_load / on_enable / ...)
//
// AngelScript 侧的函数由 asIScriptContext::Prepare + SetArg + Execute 调用。
// 生命周期钩子的签名:
//   void on_load(PluginContext@ c)         — 拿 ctx handle
//   void on_enable()                        — 无参
//   void on_disable()                       — 无参
//   void/bool on_unload()                   — false 阻断卸载
//
// 面板 hook:
//   dictionary@ render_panel(dictionary@ payload)
//   dictionary@ on_panel_action(string action_id, dictionary@ payload)
//
// 对齐 Python 源: angel_runtime.py 的 hook 提取 (interp.get_function) + call。
#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/plugins/angel_host/as_host.h"
#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

class asIScriptEngine;
class asIScriptContext;
class asIScriptModule;
class asIScriptFunction;
struct SaoSdkContext;

namespace sao::plugins::angel_host {

typedef struct as_plugin_s* as_plugin_handle_t;
typedef struct as_loader_adapter_owner_s* as_loader_adapter_owner_t;
typedef struct as_loader_adapter_plugin_lease_s* as_loader_adapter_plugin_lease_t;

// 加载 .as 脚本 (Build 到 module)。
// 内部:
//   1. 注册 generic bridge/stdlib 并 Build 独立 module
//   2. 通过 sdk_binding Angel activate 接入 canonical loader context
//   3. 保存 binding handle，卸载时先 deactivate callbacks 再销毁 module
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_load_script(asIScriptEngine* engine, const wchar_t* plugin_dir,
                               const char* entry_relative, const char* plugin_id_utf8,
                               void* ctx_ptr, // loader canonical plugin_context_t*
                               as_plugin_handle_t* out_plugin);

// 生命周期 hook 调用。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_load(as_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_enable(as_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_disable(as_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_unload(as_plugin_handle_t plugin, bool* out_allow_unload);

// 通用 hook: args_json 是 hook 参数的 json 数组; 返回 json (归属调用方 free)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_hook(as_plugin_handle_t plugin, const char* hook_name,
                             const char* args_json_utf8, char** out_result_json_utf8);

// 查该插件是否有指定 hook (module->GetFunctionByName)。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_has_hook(as_plugin_handle_t plugin, const char* hook_name);

// 调 asIScriptFunction* (由 callback wrap 保存的) 反向 hook。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_function(asIScriptEngine* engine, asIScriptFunction* fn,
                                 const char* args_json_utf8, char** out_result_json_utf8);

// 稳定 rundown 后卸载；先 sdk_binding deactivate，再 Release context/module。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unload_script(as_plugin_handle_t plugin);

// 借用 module 指针。调用方必须持有 adapter lease 或自行串行化 unload。
extern "C" SAO_PLUGINS_API asIScriptModule* SAO_PLUGINS_CALL
sao_plugins_ashost_get_module(as_plugin_handle_t plugin);

// 拿到 engine 与 loader canonical context（同样遵守上述借用期约束）。
extern "C" SAO_PLUGINS_API asIScriptEngine* SAO_PLUGINS_CALL
sao_plugins_ashost_get_engine(as_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_ashost_get_bound_context(as_plugin_handle_t plugin);

// 注册 loader::engine_kind::angelscript 的 generic adapter。每个插件独占
// engine/module/context/SaoSdkContext；注销前 loader 必须已卸载全部插件。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ashost_register_loader_adapter(
    const as_host_config* cfg, as_loader_adapter_owner_t* out_owner);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unregister_loader_adapter(as_loader_adapter_owner_t owner);

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_plugin_count(as_loader_adapter_owner_t owner);

// 返回 adapter 为该 loader plugin 保留的最后一次 AngelScript 错误。
// 输出由 sao_plugins_ashost_free_string 释放。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_get_last_error(as_loader_adapter_owner_t owner,
                                                 void* loader_plugin_handle, char** out_utf8);

// Adapter 内省 lease。lease 未释放前会阻止对应插件卸载，因此以下两个
// 指针在 lease 生命周期内保持有效。每次成功 acquire 必须配对 release。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_acquire_plugin(as_loader_adapter_owner_t owner,
                                                 void* loader_plugin_handle,
                                                 as_loader_adapter_plugin_lease_t* out_lease);

extern "C" SAO_PLUGINS_API as_plugin_handle_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_lease_script(as_loader_adapter_plugin_lease_t lease);

extern "C" SAO_PLUGINS_API SaoSdkContext* SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_lease_sdk_context(as_loader_adapter_plugin_lease_t lease);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_release_plugin(as_loader_adapter_plugin_lease_t lease);

} // namespace sao::plugins::angel_host
