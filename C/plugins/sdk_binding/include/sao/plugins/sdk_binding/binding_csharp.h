// binding_csharp.h — SDK C ABI → hostfxr + Roslyn 桥
//
// C# 侧走反向 P/Invoke: 在托管代码里声明 [LibraryImport] 到本 host 导出的
// sao_plugins_ctx_* 函数, 把 IntPtr(ctx) 从 native 一路带到 managed 侧。
// managed 用 nint 存 ctx, native 层做 handle 校验。
//
// Roslyn 编译: 用 Microsoft.CodeAnalysis 编译 .cs 到内存 assembly, LoadContext
// 加载, 反射拿 OnLoad(dynamic ctx) 类方法调用。
//
// 对齐 Python 源: csharp_runtime.py 的 pythonnet 路径 (但 C++ 侧走 hostfxr,
// 不依赖 pythonnet)。
//
// C# 插件侧访问模式 (对齐 example_csharp_plugin/plugin.cs):
//     public class Plugin {
//         static dynamic _ctx;
//         public static void OnLoad(dynamic ctx) {
//             _ctx = ctx;
//             ctx.log("hello from C#");
//             ctx.register_ui_panel("id", new Dictionary<string,object> {
//                 {"title", "Demo"}
//             }, (Func<object,object>)RenderPanel,
//                (Func<string,object,object>)OnPanelAction);
//         }
//     }
//
// 关键:
//   - 托管侧的 dynamic 是 SaoPluginContext (managed wrapper class), 内部持
//     IntPtr(ctx), 每个方法反向 P/Invoke 到 native
//   - Delegate 传给 native 时用 GCHandle.Alloc(Normal) + GetFunctionPointerForDelegate
//   - Delegate 存 GCHandle 供 native 侧持有, unload 时 GCHandle.Free
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/sdk_binding/binding_common.h"

namespace sao::plugins::sdk_binding {

typedef struct plugin_context_s* plugin_context_ptr;
typedef struct csharp_domain_s* csharp_domain_ptr;   // 来自 csharp_host/cs_host.h

// ── SDK 注入到 C# domain ────────────────────────────

// 每插件的 CLR AssemblyLoadContext / domain (隔离)。
// 内部: 从 embedded manifest 里的 SaoPluginContext.cs 编译一份 wrapper class,
// LoadContext.LoadFromStream 到 domain, 用反射设 _ctx_native_handle 静态字段。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_csharp_bind_ctx(csharp_domain_ptr domain,
                                    plugin_context_ptr ctx);

// 通过反射调 C# 方法 (类名 "Plugin", 方法名 "OnLoad", 参数 dynamic ctx)。
// assembly_qualified_class 是 "Namespace.Class, AssemblyName" 形式。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_csharp_call_static(csharp_domain_ptr domain,
                                       const char* assembly_qualified_class,
                                       const char* method_name,
                                       const char* args_json_utf8,
                                       char** out_result_json_utf8);

// 把托管 Delegate 存成 GCHandle 用作 SDK 回调, 由 native 侧持有裸函数指针
// (hostfxr Marshal.GetFunctionPointerForDelegate)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_csharp_wrap_delegate(csharp_domain_ptr domain,
                                         void* delegate_gc_handle,
                                         void** out_sdk_callback_ptr,
                                         void** out_user_data);

// 释放 GCHandle (unload 时调)。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_csharp_release_delegate(void* user_data);

// ── native → managed 反向 API 表 ──
//
// 托管侧 wrapper (SaoPluginContext.cs) 通过 [LibraryImport("sao_plugins_binding")]
// 声明这些函数. 完整的 sdk_method_id 表面由 cs_module_bridge 的 SdkBridge
// fn-ptr 结构承载; 本节仅保留三个已实现的导出符号:
//   - register_menu_category: 真实现, 转发到 loader ctx API
//   - register_menu_surface / register_action_handler: 占位 stub, 返回 UNSUPPORTED
//
// 命名约定: sao_csharp_ctx_<method>. 与 sao_plugins_ctx_<method> 同签名,
// 只是导出符号名以 sao_csharp_ 前缀区分, 便于 hostfxr LibraryImport 显式绑定。

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_menu_category(void* ctx, const char* name, const char* icon,
                                        void* builder_delegate, float priority);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_menu_surface(void* ctx, const char* id,
                                       const char* descriptor_json, float priority);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_action_handler(void* ctx, void* delegate);

// ── 激活 C# 侧 binding ──────────────────────────────────────
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_csharp_activate(plugin_context_ptr plugin_ctx,
                                    csharp_domain_ptr domain,
                                    plugin_binding_handle_t* out_plugin);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_csharp_deactivate(plugin_binding_handle_t plugin);

} // namespace sao::plugins::sdk_binding
