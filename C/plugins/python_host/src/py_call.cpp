// py_call.cpp — legacy 直接脚本入口 (load_script / unload_script)
//
// 这两个入口是 v1 时代的无-host 句柄形态：进程内 CPython singleton
// (sao_plugins_pyhost_init) 建立后，对 singleton 借用一个可复用 host
// handle 派发到 py_host.cpp 的正规 load_plugin / unload_plugin，
// 生命周期协议（manifest 兼容、sys.path 前插/回滚、sys.modules 摘除、
// hook DECREF、SDK ctx teardown、GIL 串行化）全部由那条正规路径执行。
// 非 embed 构建返回 SAO_ERR_NOT_IMPLEMENTED 作为明确的 capability 信号；
// embed 编译但 singleton 未初始化时返回 SAO_ERR_NOT_INITIALIZED。
#include "sao/plugins/python_host/py_call.h"
#include "sao/plugins/python_host/py_host.h"

namespace sao::plugins::python_host {

#if defined(SAO_HAS_PYTHON_EMBED)
namespace detail {

// Defined in py_host.cpp: publishes (once per singleton lifetime) a
// reusable borrowed handle on the process-wide host singleton.  The
// returned handle must NOT be passed to sao_plugins_pyhost_shutdown —
// runtime teardown stays owned by the real init/shutdown callers.
int32_t ensure_legacy_script_host(py_host_handle_t* out_host) noexcept;

} // namespace detail
#endif

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_load_script(const wchar_t* plugin_dir,
                               const char* entry_relative,
                               const char* plugin_id_utf8,
                               py_plugin_handle_t* out_plugin) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    if (plugin_dir == nullptr || plugin_dir[0] == L'\0')
        return SAO_ERR_INVALID_ARGUMENT;
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)entry_relative;
    (void)plugin_id_utf8;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        py_host_handle_t host = nullptr;
        const int32_t host_status = detail::ensure_legacy_script_host(&host);
        if (host_status != SAO_OK)
            return host_status;
        // sao_plugins_pyhost_load_plugin requires the GIL held by the
        // caller; gil_scope_enter returns nullptr when the runtime is not
        // up (imports unresolved or interpreter finalized).
        void* gil = sao_plugins_pyhost_gil_scope_enter();
        if (gil == nullptr)
            return SAO_ERR_NOT_INITIALIZED;
        const int32_t status =
            sao_plugins_pyhost_load_plugin(host, plugin_dir, entry_relative,
                                           plugin_id_utf8, nullptr, out_plugin);
        sao_plugins_pyhost_gil_scope_leave(gil);
        if (status != SAO_OK)
            *out_plugin = nullptr;
        return status;
    } catch (...) {
        *out_plugin = nullptr;
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_unload_script(py_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        // Conditional scope: when the interpreter is already finalized the
        // returned scope is nullptr and unload_plugin still runs its
        // registry-level teardown without touching PyObject pointers.
        void* gil = sao_plugins_pyhost_gil_scope_enter();
        const int32_t status = sao_plugins_pyhost_unload_plugin(plugin);
        sao_plugins_pyhost_gil_scope_leave(gil);
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}

} // namespace sao::plugins::python_host
