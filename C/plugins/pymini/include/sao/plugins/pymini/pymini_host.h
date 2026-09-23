// Registration is native-only; the CPython delegate is initialized on its first load.
#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::pymini {

// opaque owner handle — one per registered composite adapter.
struct pymini_adapter_owner_s;
using pymini_adapter_owner_t = pymini_adapter_owner_s*;

// registration-time config
struct pymini_adapter_config {
    // Kept verbatim until a plugin selects CPython; registration never probes or initializes it.
    const wchar_t* python_home = nullptr;
    // helper 模块解释器的额外 sys.path 根（vendor/libs 之外）。
    const wchar_t* const* extra_module_dirs = nullptr;
    uint32_t extra_module_dirs_count = 0;
};

// 注册 engine_kind::python 的 production adapter（composite pymini|pyhost
// delegate）。owner 必须先经 unregister 前把所有 lifecycle 插件卸载。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_register_loader_adapter(
    const pymini_adapter_config* cfg,
    pymini_adapter_owner_t* out_owner);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_unregister_loader_adapter(pymini_adapter_owner_t owner);

// Read-only routing query; out_reason is released with sao_plugins_pymini_free_string.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_adapter_requires_python(
    pymini_adapter_owner_t owner, const loader::plugin_manifest* manifest,
    bool* out_required, char** out_reason);

// 注册 `.py` 的 script_ctx provider（runtime_bridge priority=10）。
// 与 adapter 独立，可单独调用/注销。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_register_script_engine(void);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_unregister_script_engine(void);

// A null loader handle queries owner errors; non-null handles never inherit another plugin's error.
extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_pymini_adapter_plugin_count(pymini_adapter_owner_t owner);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_adapter_get_last_error(pymini_adapter_owner_t owner,
                                          void* loader_plugin_handle,
                                          char** out_utf8);
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pymini_free_string(char* value);
} // namespace sao::plugins::pymini
