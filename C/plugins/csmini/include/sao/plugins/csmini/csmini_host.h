// csmini_host.h — 纯 C++ C# 子集解释器宿主 (adapter 注册入口)。
//
// csmini 是 engine_kind::csharp 的默认生产适配器：legacy `plugin.cs` /
// `ctx.*` 插件清单按其 `cs_runtime` 提示与源码子集预检路由：
//   cs_runtime: "csmini"  → 本机解释器执行
//   cs_runtime: "coreclr" → 委托 sao_plugins_cshost_* (当可提供时)
//   cs_runtime 缺失/auto → 源码预检：子集内 → csmini；子集外 → coreclr 委托,
//                          无 host → SAO_PLUGINS_ERR_UNSUPPORTED + 特性名
//   entry 以 .dll 结尾    → 始终 coreclr REQUIRED (无 host → UNSUPPORTED)
//
// 卸载顺序：lifecycle 先移除全部注册项 → csmini_drop_callbacks →
// interpreter 析构；helper/load_local 解释器归属调用方插件生命周期。
#pragma once
#include <cstdint>
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao_plugins/sao_status.h"
namespace sao::plugins::csmini {
// opaque owner handle — one per registered composite adapter.
struct csmini_adapter_owner_s;
using csmini_adapter_owner_t = csmini_adapter_owner_s*;
// registration-time config
struct csmini_adapter_config {
    // 可选 coreclr 委托：非空时可用 sao_plugins_cshost_* 承接
    // `cs_runtime:coreclr` / 子集外 / .dll entry 插件。空 → 该路由直接
    // UNSUPPORTED（携带特性名）。期望 dotnet 安装根或 hostfxr 目录。
    const wchar_t* dotnet_root = nullptr;
    // helper 模块解释器与 coreclr 委托的额外程序集/模块目录。
    const wchar_t* const* extra_assembly_dirs = nullptr;
    uint32_t extra_assembly_dirs_count = 0;
};
// 注册 engine_kind::csharp 的 production adapter（composite
// csmini|coreclr delegate）。owner 必须先经 unregister 前把所有
// lifecycle 插件卸载。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_register_loader_adapter(
    const csmini_adapter_config* cfg,
    csmini_adapter_owner_t* out_owner);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_unregister_loader_adapter(csmini_adapter_owner_t owner);
// 注册 `.cs` 的 script_ctx provider（runtime_bridge priority=10）。
// 与 adapter 独立，可单独调用/注销。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_register_script_engine(void);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_unregister_script_engine(void);
// 内省：当前 csmini 托管的插件数 + 最后一个失败信息。
extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_csmini_adapter_plugin_count(csmini_adapter_owner_t owner);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_adapter_get_last_error(csmini_adapter_owner_t owner,
                                        void* loader_plugin_handle,
                                        char** out_utf8);
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_csmini_free_string(char* value);
} // namespace sao::plugins::csmini
