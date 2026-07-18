// py_host.h — CPython 3.11+ 生命周期管理
//
// 对齐 Python 源: 主平台 Python 版本自己就是 CPython, 无需宿主。C++ 平台
// 里插入嵌入式 CPython 是把 "Python 插件" 作为一种引擎宿主统一进 5 语言
// 派发, 所以本 host 的职责是:
//   1. Py_Initialize + PyConfig 配置 (isolated=1, no_site=1, PYTHONHOME 指向
//      随包 embedded 分发)
//   2. 所有插件共用 CPython 主解释器；调用由主解释器 GIL 串行化
//   3. 用 sdk_binding/binding_python 注册 sao_sdk 内置模块
//   4. spec_from_file_location 加载 plugin.py, 提取 on_load/on_enable 等 hook
//   5. 最后一个 host handle shutdown 时按所有权决定是否 Py_Finalize
//
// **能直接加载旧 Python 插件**: 老 plugin.py 的 ``on_load(ctx)`` 拿到的 ctx
// 是本 host 用 sao_sdk.wrap_ctx(handle) 构造的 PyObject, 方法名/签名/返回值
// 全部与 Python 平台的 PluginContext 一致 (对齐 act_platform/plugins.py
// PluginContext class ~70 方法), 所以插件代码**一个字不用改**就能跑。
//
// 加载流程 (对齐 Python PluginManager.load_plugin, 见 plugins.py 2285-2320):
//   1. _prepare_plugin_sys_path — 前插 <plugin>/vendor + <plugin>/libs +
//      <plugin>/engine 到 sys.path (由 compat/libs_vendor_bridge 协同)
//   2. importlib.util.spec_from_file_location(f"act_plugin_{id}", plugin.py)
//   3. spec.loader.exec_module(module)
//   4. getattr(module, "on_load"), 调用 on_load(ctx)
//   5. getattr(module, "on_enable", None), 调用 on_enable() 若存在
//
// 不用 pybind11。手写 PyModuleDef 是有意的 (pybind11 拉包大, 且我们只暴露
// 一小组 C ABI 方法)。
#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::python_host {

typedef struct py_host_s* py_host_handle_t;
typedef struct py_plugin_s* py_plugin_handle_t;
typedef struct py_loader_adapter_owner_s* py_loader_adapter_owner_t;

// Python 宿主全局配置。
struct py_host_config {
    // 嵌入式 Python 分发路径 (含 python3XX.dll, DLLs/, Lib/)
    const wchar_t* python_home;
    // 隔离模式 (isolated / no_site / no_user_site) — 对齐 PyConfig 字段
    bool isolated = true;
    bool no_site = true;
    bool ignore_pypath_env = true;  // 拒 PYTHONPATH 环境变量泄漏
    // Test-only: permit deterministic local dependency shims.  Production
    // hosts must leave this false so missing runtime dependencies fail.
    bool controlled_test_shim = false;
    // 平台已知随包 site 目录 (随包 CPython 分发的 Lib/site-packages)
    const wchar_t* platform_site_dir;
    // stdout / stderr 重定向 utf-8 目标 (nullptr = 不重定向, 走 CRT)
    void (*stdout_callback)(const char* utf8, void* ud);
    void (*stderr_callback)(const char* utf8, void* ud);
    void* callback_user_data;
    // 是否在 Py_Initialize 前调 sao_plugins_binding_python_register_module
    // 把 sao_sdk 内置模块挂到 sys.modules (默认 true)。
    bool register_sao_sdk = true;
};

// 初始化 CPython (进程内单例, 第二次调用 no-op)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_init(const py_host_config* cfg, py_host_handle_t* out_host);

// Py_Finalize (最后一个 handle 释放时)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_shutdown(py_host_handle_t host);

// 查询本 host 加载的 CPython 版本 (X_Y, e.g. "3.11")。
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_pyhost_version(py_host_handle_t host);

// 探测嵌入式 Python 分发是否可用。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_pyhost_available(const wchar_t* python_home);

// ── 加载 / 卸载单个插件 (被 lifecycle.load 派发进来) ──

// 直接调用本节 API 时，调用方必须持有 CPython 主解释器 GIL；这些函数
// 不在内部 acquire/release GIL。production loader adapter 负责为每次
// load/hook/unload 调用获取 GIL，并允许从原生 worker thread 进入。

// 载入未修改的 plugin.py。
// 内部:
//   1. sys.path 前插 <plugin>/vendor / libs / engine
//   2. importlib.util.spec_from_file_location(f"act_plugin_{id}", <entry>)
//   3. spec.loader.exec_module(module)
//   4. 抽 on_load / on_enable / on_disable / on_unload 存到 py_plugin_handle_t
// ctx_ptr 若非 nullptr，必须指向仍由调用方持有的有效 SaoSdkContext；传入
// 其他 ABI 的 opaque context 不会被复用，host 将创建自己的 SaoSdkContext。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_load_plugin(py_host_handle_t host,
                               const wchar_t* plugin_dir,
                               const char* entry_relative,
                               const char* plugin_id_utf8,
                               void* ctx_ptr,
                               py_plugin_handle_t* out_plugin);

// 调 on_load(ctx) 传入本插件的 ctx PyObject (来自 wrap_ctx)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_call_on_load(py_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_call_on_enable(py_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_call_on_disable(py_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_call_on_unload(py_plugin_handle_t plugin,
                                  bool* out_allow_unload);

// 卸载 (从 sys.modules 删, DECREF hook, 恢复 sys.path)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_unload_plugin(py_plugin_handle_t plugin);

// 查该插件是否有指定 hook。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_pyhost_has_hook(py_plugin_handle_t plugin, const char* hook_name);

// 调任意 hook (未来 SDK 扩展预留)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_call_hook(py_plugin_handle_t plugin,
                             const char* hook_name,
                             const char* args_json_utf8,
                             char** out_result_json_utf8);

// 内省: 拿本插件在 on_load 时得到的 ctx PyObject (借用引用)。
// 返回 void* 避 include Python.h; 调用方 cast 为 PyObject* 只读用。
extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_get_ctx_pyobject(py_plugin_handle_t plugin);

// 内省: 拿本插件 module PyObject (借用引用)。同上, 只读。
extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_get_module_pyobject(py_plugin_handle_t plugin);

// 内省: 拿本插件 manifest (loader::plugin_manifest*, 借用只读).
// 返 nullptr 若 plugin 未持有 manifest.
extern "C" SAO_PLUGINS_API const void* SAO_PLUGINS_CALL
sao_plugins_pyhost_get_manifest(py_plugin_handle_t plugin);

// Internal integration-test bridge to the plugin-owned native SDK context.
// The returned pointer is borrowed and becomes invalid at unload.
extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_get_sdk_context(py_plugin_handle_t plugin);

// 内省: 上次 load 时抓到的 Python traceback (若有), 归属调用方 free。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_get_last_error(py_plugin_handle_t plugin,
                                  char** out_utf8);

// 注册 loader::engine_kind::python 的 production adapter。cfg 必须提供
// 显式 bundled python_home；owner 注销前必须先通过 loader 卸载全部插件。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_register_loader_adapter(
    const py_host_config* cfg,
    py_loader_adapter_owner_t* out_owner);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_unregister_loader_adapter(
    py_loader_adapter_owner_t owner);

// adapter 内部 loader plugin_handle_t → py_plugin_handle_t 映射内省。
extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_pyhost_loader_adapter_plugin_count(
    py_loader_adapter_owner_t owner);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_loader_adapter_get_last_error(
    py_loader_adapter_owner_t owner,
    void* loader_plugin_handle,
    char** out_utf8);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_loader_adapter_get_requirements_report(
    py_loader_adapter_owner_t owner,
    void* loader_plugin_handle,
    char** out_json_utf8);

// requirements.txt 只做静态报告，不安装、不启动外部进程。返回路径优先级为
// engine、libs、vendor，与 Python oracle 的最终 sys.path 顺序一致。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_report_requirements(const wchar_t* plugin_dir,
                                       char** out_json_utf8);

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_free_string(char* value);

} // namespace sao::plugins::python_host
