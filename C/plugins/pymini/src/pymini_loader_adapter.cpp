// pymini_loader_adapter.cpp — engine_kind::python 的 composite production
// adapter：pymini（默认）+ pyhost 委托（py_runtime:cpython / 子集外）。
//
// 路由顺序（load_plugin 内决定，一次一插件）：
//   1. manifest.py_runtime == "pymini" → pymini
//   2. manifest.py_runtime == "cpython" → pyhost 委托（无 → UNSUPPORTED）
//   3. 其它（auto/缺失）→ 读取 entry 源码并跑 pymini_preflight_subset：
//        子集内 → pymini；子集外 → pyhost 委托（无 → UNSUPPORTED，错误串
//        携带命中的特性名）。
//
// pyhost 委托经由本文件新增的 sao_plugins_pyhost_gil_scope_enter/leave
// 配对包裹每次 load/hook/unload 调用，pymini TU 不接触 Python.h。
//
// 每条插件记录保留 {route, pyh}；hook/unload 按 route 分支到 pymini_host_*
// 或 pyhost 对应调用。
#include "sao/plugins/pymini/pymini_host.h"

#include "pymini_interp.h"

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"

#if defined(SAO_PLUGINS_ENABLE_PYTHON)
#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/python_host/py_module_bridge.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sao::plugins::pymini {

using loader::host_adapter_vtable;
using loader::plugin_context_t;
using loader::plugin_handle_t;
using loader::plugin_manifest;

// adapter owner — concrete def of the opaque public handle
struct pymini_adapter_owner_s {
    // per-plugin record
    enum class route_e { pymini, cpython };
    struct rec {
        route_e route = route_e::pymini;
        std::string plugin_id;
        std::string last_error;
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
        python_host::py_plugin_handle_t pyh = nullptr;
#endif
    };
    bool active = false;
    bool adapter_registered = false;
    std::wstring python_home;
    std::vector<std::wstring> extra_dirs;
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
    python_host::py_host_handle_t host = nullptr;
#endif
    std::unordered_map<plugin_handle_t, rec> plugins;
    std::string last_error;
};

// ── utf8 ↔ wide helpers (independent of pymini_host's anon-namespace) ──
namespace {

std::wstring widen_u8(const std::string& u8) {
    if (u8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(),
                                      static_cast<int>(u8.size()), nullptr,
                                      0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()),
                        w.data(), n);
    return w;
}
std::string narrow_w(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}
std::string read_all(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string out(static_cast<std::size_t>(sz.QuadPart), '\0');
    DWORD rd = 0;
    if (sz.QuadPart > 0)
        ReadFile(h, out.data(), static_cast<DWORD>(sz.QuadPart), &rd,
                 nullptr);
    CloseHandle(h);
    return out;
}

// error-string helper owned by free via sao_plugins_pymini_free_string
char* dup_err(const std::string& s) {
    if (s.empty())
        return nullptr;
    char* b = static_cast<char*>(std::malloc(s.size() + 1));
    if (!b)
        return nullptr;
    std::memcpy(b, s.data(), s.size() + 1);
    return b;
}

// per-plugin record + route enum (aliases onto the owner's nested rec)
using rec = pymini_adapter_owner_s::rec;
using route_t = pymini_adapter_owner_s::route_e;

std::mutex g_mu;
pymini_adapter_owner_s* g_owner = nullptr;

pymini_adapter_owner_s* active_owner(void* ud) {
    return static_cast<pymini_adapter_owner_s*>(ud);
}

void set_err(pymini_adapter_owner_s* o, std::string e) {
    if (o)
        o->last_error = std::move(e);
}

// ── pyhost delegate wrappers (only compiled with SAO_PLUGINS_ENABLE_PYTHON) ──
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
struct gil_scope {
    void* s = nullptr;
    explicit gil_scope(bool take) {
        if (take)
            s = python_host::sao_plugins_pyhost_gil_scope_enter();
    }
    ~gil_scope() {
        if (s)
            python_host::sao_plugins_pyhost_gil_scope_leave(s);
    }
    bool held() const { return s != nullptr; }
    gil_scope(const gil_scope&) = delete;
    gil_scope& operator=(const gil_scope&) = delete;
};

int32_t pyh_load(pymini_adapter_owner_s* o, plugin_context_t* lctx,
                 const std::wstring& dir, const std::string& entry,
                 const std::string& id, rec* r, std::string* err) {
    gil_scope gil(true);
    if (!gil.held()) {
        if (err)
            *err = "cannot acquire CPython GIL";
        return SAO_ERR_OS_CALL_FAILED;
    }
    python_host::py_plugin_handle_t pyh = nullptr;
    const int32_t st = python_host::sao_plugins_pyhost_load_plugin(
        o->host, dir.c_str(), entry.c_str(), id.c_str(), nullptr, &pyh);
    if (st != SAO_OK || !pyh) {
        if (err) {
            char* pe = nullptr;
            if (pyh)
                (void)python_host::sao_plugins_pyhost_get_last_error(pyh, &pe);
            *err = pe ? pe : "pyhost load failed";
            if (pe)
                python_host::sao_plugins_pyhost_free_string(pe);
        }
        return st;
    }
    // bind the canonical loader ctx (the pyhost ctx PyObject ↔ plugin_context_t)
    const int32_t bs = python_host::sao_plugins_pyhost_ctx_bind_loader_context(
        python_host::sao_plugins_pyhost_get_ctx_pyobject(pyh),
        lctx);
    if (bs != SAO_OK) {
        (void)python_host::sao_plugins_pyhost_unload_plugin(pyh);
        if (err)
            *err = "pyhost ctx_bind failed";
        return bs;
    }
    r->pyh = pyh;
    return SAO_OK;
}
int32_t pyh_hook(rec& r, const char* which, std::string* err) {
    if (!r.pyh)
        return SAO_ERR_HANDLE_INVALID;
    gil_scope gil(true);
    if (!gil.held()) {
        if (err)
            *err = "cannot acquire CPython GIL";
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (std::strcmp(which, "on_load") == 0)
        return python_host::sao_plugins_pyhost_call_on_load(r.pyh);
    if (std::strcmp(which, "on_enable") == 0)
        return python_host::sao_plugins_pyhost_call_on_enable(r.pyh);
    if (std::strcmp(which, "on_disable") == 0)
        return python_host::sao_plugins_pyhost_call_on_disable(r.pyh);
    return SAO_ERR_INVALID_ARGUMENT;
}
int32_t pyh_on_unload(rec& r, bool* allow, std::string* err) {
    if (!r.pyh) {
        *allow = true;
        return SAO_OK;
    }
    gil_scope gil(true);
    if (!gil.held()) {
        if (err)
            *err = "cannot acquire CPython GIL";
        *allow = false;
        return SAO_ERR_OS_CALL_FAILED;
    }
    bool a = true;
    const int32_t st =
        python_host::sao_plugins_pyhost_call_on_unload(r.pyh, &a);
    *allow = a;
    return st;
}
int32_t pyh_unload(rec& r, std::string* err) {
    if (!r.pyh)
        return SAO_OK;
    gil_scope gil(true);
    if (!gil.held()) {
        if (err)
            *err = "cannot acquire CPython GIL";
        return SAO_ERR_OS_CALL_FAILED;
    }
    const int32_t st = python_host::sao_plugins_pyhost_unload_plugin(r.pyh);
    r.pyh = nullptr;
    return st;
}
#endif

// ── route decision ───────────────────────────────────────────────────
route_t decide_route(const plugin_manifest* m,
                     pymini_adapter_owner_s* o,
                     const std::wstring& abs_entry,
                     std::string* note) {
    const std::string hint = m && !m->py_runtime.empty()
                                 ? m->py_runtime
                                 : std::string("auto");
    if (hint == "pymini")
        return route_t::pymini;
    if (hint == "cpython") {
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
        if (o->host)
            return route_t::cpython;
#endif
        if (note)
            *note = "py_runtime:cpython requested but no pyhost available";
        return route_t::pymini;         // caller checks note → UNSUPPORTED
    }
    // auto: read entry + preflight subset scan
    const std::string src = read_all(abs_entry);
    std::string why;
    if (pymini_preflight_subset(src, &why))
        return route_t::pymini;
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
    if (o->host)
        return route_t::cpython;
#endif
    if (note)
        *note = "requires cpython-only feature: " + why;
    return route_t::pymini;             // caller checks note → UNSUPPORTED
}

// ── vtable functions ─────────────────────────────────────────────────
int32_t SAO_PLUGINS_CALL adapter_load(
    plugin_handle_t plugin, const plugin_manifest* manifest,
    void* host_user_data) {
    pymini_adapter_owner_s* o = active_owner(host_user_data);
    if (!o || !o->active)
        return SAO_ERR_NOT_INITIALIZED;
    if (!plugin || !manifest)
        return SAO_ERR_INVALID_ARGUMENT;

    plugin_context_t* lctx = nullptr;
    if (loader::sao_plugins_lifecycle_get_context(plugin, &lctx) != SAO_OK ||
        !lctx)
        return SAO_ERR_NOT_INITIALIZED;

    const std::wstring dir = widen_u8(manifest->source_path);
    const std::wstring abs_entry = dir + L'\\' + widen_u8(manifest->entry);

    std::string note;
    const route_t route = decide_route(manifest, o, abs_entry, &note);
    if (!note.empty()) {
        // decided route wasn't genuinely available
        set_err(o, note);
        return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    }

    std::lock_guard<std::mutex> g(g_mu);
    if (o->plugins.count(plugin))
        return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
    rec r;
    r.route = route;
    r.plugin_id = manifest->plugin_id;
    o->plugins.emplace(plugin, r);
    // copy into a stable ref we can pass by pointer without re-locking twice
    rec& slot = o->plugins[plugin];

    std::string err;
    if (route == route_t::pymini) {
        std::string perr;
        const int rc = pymini_host_load_plugin(
            lctx, manifest->plugin_id.c_str(), dir.c_str(),
            widen_u8(manifest->entry).c_str(), o->extra_dirs, &perr);
        if (rc != 0) {
            o->plugins.erase(plugin);
            set_err(o, perr);
            return SAO_ERR_OS_CALL_FAILED;
        }
    } else {
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
        std::string perr;
        const int32_t st = pyh_load(o, lctx, dir, manifest->entry,
                                    manifest->plugin_id, &slot, &perr);
        if (st != SAO_OK) {
            o->plugins.erase(plugin);
            set_err(o, perr);
            return st;
        }
#else
        o->plugins.erase(plugin);
        set_err(o, "pyhost delegate not built (SAO_PLUGINS_ENABLE_PYTHON off)");
        return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
#endif
    }
    return SAO_OK;
}

int32_t call_named(plugin_handle_t plugin, void* ud, const char* hook,
                   bool* allow_unload = nullptr) {
    pymini_adapter_owner_s* o = active_owner(ud);
    if (!o)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = o->plugins.find(plugin);
    if (it == o->plugins.end())
        return SAO_ERR_HANDLE_INVALID;
    rec& r = it->second;
    std::string err;
    if (r.route == route_t::pymini) {
        if (allow_unload) {
            const int rc = pymini_host_call_hook_allow_unload(
                r.plugin_id.c_str(), hook, allow_unload, &err);
            if (rc == 0 || rc == 1)
                return SAO_OK;
            set_err(o, err);
            return SAO_ERR_OS_CALL_FAILED;
        }
        const int rc =
            pymini_host_call_hook(r.plugin_id.c_str(), hook, nullptr, &err);
        if (rc == 0 || rc == 1)
            return SAO_OK;
        set_err(o, err);
        return SAO_ERR_OS_CALL_FAILED;
    }
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
    if (allow_unload)
        return pyh_on_unload(r, allow_unload, &err);
    return pyh_hook(r, hook, &err);
#else
    (void)allow_unload;
    return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
#endif
}

int32_t SAO_PLUGINS_CALL adapter_on_load(plugin_handle_t plugin,
                                         void* ud) {
    return call_named(plugin, ud, "on_load");
}
int32_t SAO_PLUGINS_CALL adapter_on_enable(plugin_handle_t plugin,
                                           void* ud) {
    return call_named(plugin, ud, "on_enable");
}
int32_t SAO_PLUGINS_CALL adapter_on_disable(plugin_handle_t plugin,
                                            void* ud) {
    return call_named(plugin, ud, "on_disable");
}
int32_t SAO_PLUGINS_CALL adapter_on_unload(plugin_handle_t plugin,
                                           bool* allow, void* ud) {
    return call_named(plugin, ud, "on_unload", allow);
}

int32_t SAO_PLUGINS_CALL adapter_unload(plugin_handle_t plugin,
                                        void* ud) {
    pymini_adapter_owner_s* o = active_owner(ud);
    if (!o)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = o->plugins.find(plugin);
    if (it == o->plugins.end())
        return SAO_ERR_HANDLE_INVALID;
    rec r = it->second;
    o->plugins.erase(it);
    std::string err;
    if (r.route == route_t::pymini) {
        return pymini_host_unload_plugin(r.plugin_id.c_str(), &err);
    }
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
    return pyh_unload(r, &err);
#else
    return SAO_OK;
#endif
}

host_adapter_vtable make_vtable(pymini_adapter_owner_s* o) {
    host_adapter_vtable t{};
    t.load_plugin = adapter_load;
    t.call_on_load = adapter_on_load;
    t.call_on_enable = adapter_on_enable;
    t.call_on_disable = adapter_on_disable;
    t.call_on_unload = adapter_on_unload;
    t.unload_plugin = adapter_unload;
    t.host_user_data = o;
    return t;
}

} // namespace

// ═══ public C-ABI exports ═══════════════════════════════════════════
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_register_loader_adapter(
    const pymini_adapter_config* cfg,
    pymini_adapter_owner_t* out_owner) {
    if (!out_owner)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_owner = nullptr;
    try {
        std::lock_guard<std::mutex> g(g_mu);
        if (g_owner)
            return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        auto o = std::make_unique<pymini_adapter_owner_s>();
        if (cfg && cfg->python_home)
            o->python_home = cfg->python_home;
        if (cfg && cfg->extra_module_dirs)
            for (uint32_t k = 0; k < cfg->extra_module_dirs_count; ++k)
                if (cfg->extra_module_dirs[k])
                    o->extra_dirs.emplace_back(
                        cfg->extra_module_dirs[k]);

#if defined(SAO_PLUGINS_ENABLE_PYTHON)
        // lazy init: only try to create a pyhost when the caller told us a
        // python_home AND the distribution probes available.
        if (!o->python_home.empty() &&
            python_host::sao_plugins_pyhost_available(
                o->python_home.c_str())) {
            python_host::py_host_config hc{};
            hc.python_home = o->python_home.c_str();
            // init inside a gil scope so Py_Initialize's exit state release
            // matches pyhost's own register path.
            gil_scope scope(true);
            const int32_t init_st =
                python_host::sao_plugins_pyhost_init(&hc, &o->host);
            if (init_st != SAO_OK || o->host == nullptr) {
                o->host = nullptr;
                set_err(o.get(), "python_home probes available but pyhost init failed");
            }
        }
#endif

        const auto table = make_vtable(o.get());
        const int32_t st =
            loader::sao_plugins_lifecycle_register_host_adapter(
                loader::engine_kind::python, &table);
        if (st != SAO_OK)
            return st;
        o->adapter_registered = true;
        o->active = true;
        g_owner = o.get();
        *out_owner = o.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_unregister_loader_adapter(
    pymini_adapter_owner_t owner) {
    if (!owner)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> g(g_mu);
        if (owner != g_owner || !owner->active)
            return SAO_ERR_HANDLE_INVALID;
        if (!owner->plugins.empty())
            return loader::SAO_PLUGINS_ERR_BUSY;
        owner->active = false;
        int32_t st = SAO_OK;
        if (owner->adapter_registered) {
            st = loader::sao_plugins_lifecycle_unregister_host_adapter(
                loader::engine_kind::python);
            if (st != SAO_OK) {
                owner->active = true;
                return st;
            }
            owner->adapter_registered = false;
        }
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
        if (owner->host) {
            gil_scope scope(true);
            st = python_host::sao_plugins_pyhost_shutdown(owner->host);
            owner->host = nullptr;
            if (st != SAO_OK)
                return st;
        }
#endif
        g_owner = nullptr;
        delete owner;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_pymini_adapter_plugin_count(pymini_adapter_owner_t owner) {
    if (!owner)
        return 0;
    std::lock_guard<std::mutex> g(g_mu);
    return owner->plugins.size();
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_adapter_get_last_error(
    pymini_adapter_owner_t owner, void* loader_plugin_handle,
    char** out_utf8) {
    if (out_utf8)
        *out_utf8 = nullptr;
    if (!owner || !out_utf8)
        return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> g(g_mu);
    (void)loader_plugin_handle;           // errors tracked owner-globally
    *out_utf8 = dup_err(owner->last_error);
    return *out_utf8 ? SAO_OK : SAO_ERR_HANDLE_INVALID;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pymini_free_string(char* value) {
    std::free(value);
}

// script_ctx bridge hookup — forward to the TU that owns the ops struct.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_register_script_engine(void) {
    return pymini_register_script_engine();
}
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_unregister_script_engine(void) {
    return pymini_unregister_script_engine();
}

} // namespace sao::plugins::pymini
