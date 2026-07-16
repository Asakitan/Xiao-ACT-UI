// cs_call.h — 从 C++ 调 C# 方法
//
// C# 侧的生命周期钩子在 Plugin 类上 (PascalCase 命名, 对齐
// example_csharp_plugin/plugin.cs):
//   public static void OnLoad(dynamic ctx)
//   public static void OnEnable()
//   public static void OnDisable()
//   public static void OnUnload()
//   public static object RenderPanel(object payload)
//   public static object OnPanelAction(string actionId, object payload)
//
// 类可以是 static (所有方法 static) 或 instance (自动 new 一个实例存起来)。
// hostfxr 加载路径:
//   1. get_hostfxr_path
//   2. hostfxr_initialize_for_dotnet_command_line + runtimeconfig.json
//   3. load_assembly_and_get_function_pointer
//   4. 反射 assembly.GetType(class_name).GetMethod(method_name)
//   5. MethodInfo.Invoke(instance, args)
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::csharp_host {

typedef struct cs_assembly_s* cs_assembly_handle_t;
typedef struct cs_plugin_s* cs_plugin_handle_t;

// 加载 assembly + 反射拿 hook 方法, 存 handle 里。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_prepare_plugin(cs_assembly_handle_t assembly,
                                   const char* plugin_id_utf8,
                                   void* ctx_ptr,
                                   cs_plugin_handle_t* out_plugin);

// 生命周期 hook。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_call_on_load(cs_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_call_on_enable(cs_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_call_on_disable(cs_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_call_on_unload(cs_plugin_handle_t plugin,
                                  bool* out_allow_unload);

// 反射调 static 或 instance 方法。
// class_full_name = "Namespace.Plugin", method = "OnLoad", args_json 是参数 json 数组。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_call_method(cs_assembly_handle_t assembly,
                               const char* class_full_name_utf8,
                               const char* method_name_utf8,
                               const char* args_json_utf8,
                               char** out_result_json_utf8);

// 传统 hook 分派: 优先找 PascalCase (OnLoad), fallback snake_case (on_load)。
// 对齐 csharp_runtime.py 里的 PascalCase-primary + snake_case-fallback 语义。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_call_hook(cs_assembly_handle_t assembly,
                             const char* hook_name,
                             const char* args_json_utf8,
                             char** out_result_json_utf8);

// 查该插件是否有指定 hook (支持 PascalCase / snake_case 别名)。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_cshost_has_hook(cs_plugin_handle_t plugin, const char* hook_name);

// 通过 GCHandle 调 delegate (由 wrap_delegate 保存的)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_invoke_delegate(void* delegate_gc_handle,
                                   const char* args_json_utf8,
                                   char** out_result_json_utf8);

// 卸载 plugin (释放 GCHandles + LoadContext.Unload assembly)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_release_plugin(cs_plugin_handle_t plugin);

} // namespace sao::plugins::csharp_host
