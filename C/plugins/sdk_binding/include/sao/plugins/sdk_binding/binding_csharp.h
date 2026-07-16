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
// 声明这些函数. native 侧对应 sao_plugins_ctx_* 一一映射.
//
// 命名约定: sao_csharp_ctx_<method>. 与 sao_plugins_ctx_<method> 同签名,
// 只是导出符号名以 sao_csharp_ 前缀区分, 便于 hostfxr LibraryImport 显式绑定。
//
// 完整暴露 (对齐 sdk_method_id 每一项):

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_csharp_ctx_log(void* ctx, const char* message);

extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_csharp_ctx_subscribe(void* ctx, const char* topic, void* cb, void* user_data);

extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_csharp_ctx_subscribe_once(void* ctx, const char* topic, void* cb, void* user_data);

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_csharp_ctx_unsubscribe(void* ctx, uint32_t token);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_emit(void* ctx, const char* topic, const char* payload_json);

extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_csharp_ctx_on_damage(void* ctx, void* cb, void* user_data);
extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_csharp_ctx_on_heal(void* ctx, void* cb, void* user_data);
extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_csharp_ctx_on_skill(void* ctx, void* cb, void* user_data);
extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_csharp_ctx_on_boss(void* ctx, void* cb, void* user_data);
extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_csharp_ctx_on_snapshot(void* ctx, void* cb, void* user_data);
extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_csharp_ctx_on_encounter_finalized(void* ctx, void* cb, void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_get_snapshot(void* ctx, char** out_json);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_snapshot_value(void* ctx, const char* path, char** out_json);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_recent_events(void* ctx, uint32_t limit, const char* topic,
                              char** out_json);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_get_setting(void* ctx, const char* key, char** out_json);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_set_setting(void* ctx, const char* key, const char* value_json);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_set_defaults(void* ctx, const char* defaults_json);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_ui_panel(void* ctx, const char* panel_id,
                                 const char* meta_json, void* render_delegate,
                                 void* action_delegate);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_render_hook(void* ctx, const char* surface,
                                     float priority, void* delegate,
                                     uint32_t* out_token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_set_overlay(void* ctx, const char* surface, const char* spec_json);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_clear_overlay(void* ctx, const char* surface);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_request_redraw(void* ctx, const char* surface, const char* reason);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_hotkey(void* ctx, const char* id, const char* default_key,
                                const char* label, void* delegate);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_engine(void* ctx, const char* name, void* engine);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_data_source(void* ctx, const char* id, const char* meta_json,
                                     void* start_delegate, void* stop_delegate);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_menu_category(void* ctx, const char* name, const char* icon,
                                        void* builder_delegate, float priority);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_menu_surface(void* ctx, const char* id,
                                       const char* descriptor_json, float priority);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_parser_adapter(void* ctx, const char* id,
                                        const char* meta_json, void* delegate);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_exporter(void* ctx, const char* id, const char* meta_json,
                                  void* delegate);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_formatter(void* ctx, const char* id, const char* meta_json,
                                   void* delegate);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_trigger_type(void* ctx, const char* id, const char* meta_json,
                                      void* delegate);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_report_view(void* ctx, const char* id, const char* meta_json,
                                     void* delegate);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_timer(void* ctx, const char* id, const char* meta_json,
                               void* delegate);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_register_action_handler(void* ctx, void* delegate);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_set_interval(void* ctx, void* delegate, double seconds,
                              char** out_token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_set_timeout(void* ctx, void* delegate, double seconds,
                             char** out_token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_clear_timer(void* ctx, const char* token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_run_on_ui(void* ctx, void* delegate);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_notify(void* ctx, const char* title, const char* message,
                        double duration_s, const char* kind);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_dismiss_notify(void* ctx);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_toast(void* ctx, const char* message);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_open_file(void* ctx, const char* filters_json, const char* title,
                          const wchar_t* initial_dir, intptr_t hwnd_owner,
                          wchar_t** out_path);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_open_window(void* ctx, const char* panel_id,
                            uint32_t width, uint32_t height);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_create_compositor_layer(void* ctx, const char* name,
                                        uint32_t w, uint32_t h,
                                        int32_t x, int32_t y, int32_t z,
                                        bool click_through, bool high_fps,
                                        uint32_t target_fps);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_upload_compositor_frame(void* ctx, const char* name,
                                        const uint8_t* bgra, size_t len,
                                        uint32_t w, uint32_t h);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_set_compositor_layer_mmf_source(void* ctx, const char* name,
                                                 const char* mmf_name);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_set_compositor_layer_shared_texture_source(void* ctx, const char* name,
                                                            intptr_t handle,
                                                            uint32_t w, uint32_t h);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_set_compositor_layer_position(void* ctx, const char* name,
                                                int32_t x, int32_t y);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_set_compositor_layer_visible(void* ctx, const char* name, bool visible);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_destroy_compositor_layer(void* ctx, const char* name);
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_csharp_ctx_compositor_gpu_interop_available(void* ctx);

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_csharp_ctx_get_engine(void* ctx, const char* name);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_ensure_requirements(void* ctx, bool install, char** out_json);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_csharp_ctx_load_local(void* ctx, const char* relative, wchar_t** out_path);

// ── Wave 4 新增: 激活 C# 侧 binding (Wave 4 阶段 stub) ───────
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_csharp_activate(plugin_context_ptr plugin_ctx,
                                    csharp_domain_ptr domain,
                                    plugin_binding_handle_t* out_plugin);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_csharp_deactivate(plugin_binding_handle_t plugin);

} // namespace sao::plugins::sdk_binding
