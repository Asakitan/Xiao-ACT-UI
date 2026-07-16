// cs_module_bridge.h — 通过 NativeInterop 暴露 SDK
//
// 托管侧代码用 [LibraryImport] 声明到本 host 导出的 sao_csharp_ctx_* 函数,
// 把 IntPtr(ctx) 从 native 一路带到 managed。native 层做 handle 校验。
//
// 我们在托管侧提供一个 SAO.PluginContext 类, 里面暴露 static + instance 方法
// (Log / Subscribe / RegisterUiPanel / ...), 内部走 P/Invoke。老 C# 插件
// (对齐 example_csharp_plugin/plugin.cs) 拿到的 dynamic ctx 就是本类的实例,
// 支持 ctx.log(...) / ctx.register_ui_panel(...) 两种命名。
//
// 关键: 反向 P/Invoke 到 sao_plugins.dll 的 sao_csharp_ctx_* 导出符号
// (由 sdk_binding/binding_csharp.cpp 实现)。托管侧代码 (SaoPluginContext.cs)
// 由本 module_bridge 在插件加载前编译到 domain。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::csharp_host {

typedef struct cs_domain_s* cs_domain_handle_t;

// 每 domain 注入一个 ctx 的 IntPtr (作为 [ThreadStatic] 或 AsyncLocal)。
// 内部先编译一份 SaoPluginContext.cs (embedded 源码) 到 domain, 然后设
// SAO.PluginContext._nativeHandle 静态字段为 ctx_handle。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_inject_ctx(cs_domain_handle_t domain,
                              void* ctx_handle);

// 返回随包的 SaoPluginContext.cs 源码 (embedded, 编译时嵌进 sao_plugins.dll)。
// 供 cs_compile 在编译插件前预先注入。
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_cshost_embedded_ctx_source(void);

// 返回随包的 SaoPlugin.dll (预编译版本, 若插件不 include 源码时 fallback)。
// 供 cs_host 在 domain 创建时优先加载。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_load_embedded_sdk_dll(cs_domain_handle_t domain);

// 从托管侧一次性获取所有 SDK 方法的 native function pointer 表。
// 托管侧: [LibraryImport("sao_plugins", EntryPoint="sao_plugins_cshost_get_sdk_table")]
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_sdk_table(void** out_fn_ptrs, size_t* inout_count);

} // namespace sao::plugins::csharp_host
