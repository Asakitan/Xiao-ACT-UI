// pymini_host.h — 纯 C++ Python 子集解释器宿主 (adapter 注册入口)。
//
// pymini 是 engine_kind::python 的默认生产适配器：legacy `plugin.py` /
// `ctx.*` 插件清单按其 `py_runtime` 提示与源码子集预检路由：
//   py_runtime: "pymini"  → 本机解释器执行
//   py_runtime: "cpython" → 委托 sao_plugins_pyhost_* (当可提供时)
//   py_runtime 缺失/auto → 源码预检：子集内 → pymini；子集外 → pyhost 委托，
//                          无 pyhost → SAO_PLUGINS_ERR_UNSUPPORTED + 特性名
//
// 卸载顺序：lifecycle 先移除全部注册项 → pymini_drop_callbacks → interpreter
// 析构；helper/load_local 解释器归属调用方插件生命周期。
#pragma once

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
    // 可选 cpython 委托：非空时可用 sao_plugins_pyhost_* 承接 `py_runtime:
    // cpython` / 子集外插件。空 → 该路由直接 UNSUPPORTED（携带特性名）。
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

// 注册 `.py` 的 script_ctx provider（runtime_bridge priority=10）。
// 与 adapter 独立，可单独调用/注销。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_register_script_engine(void);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_unregister_script_engine(void);

// 内省：当前 pymini 托管的插件数 + 最后一个失败信息。
extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_pymini_adapter_plugin_count(pymini_adapter_owner_t owner);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_adapter_get_last_error(pymini_adapter_owner_t owner,
                                          void* loader_plugin_handle,
                                          char** out_utf8);
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pymini_free_string(char* value);
} // namespace sao::plugins::pymini
