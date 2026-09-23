// 原生 .cs 子集与预编译托管 .dll 的复合宿主；不提供源编译器。
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
    // 非空根为唯一探测位置；空根使用默认探测，实际托管加载才初始化 hostfxr。
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
// 无执行、仅文件布局预检；reason 用 sao_plugins_csmini_free_string 释放。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_adapter_requires_dotnet(
    csmini_adapter_owner_t owner, const loader::plugin_manifest* manifest,
    bool* out_required, bool* out_available, char** out_reason);
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
