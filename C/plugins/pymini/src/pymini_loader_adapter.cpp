// Full-graph routing precedes execution; execution failures never change engines.
#include "sao/plugins/pymini/pymini_host.h"

#include "pymini_interp.h"

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/script_ctx/ctx_surface.h"

#if defined(SAO_PLUGINS_ENABLE_PYTHON)
#include "sao/plugins/python_host/py_error.h"
#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/python_host/py_module_bridge.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
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
        bool ready = false;
        int32_t load_status = SAO_ERR_NOT_INITIALIZED;
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
    bool host_init_attempted = false;
    int32_t host_init_status = SAO_ERR_NOT_INITIALIZED;
    std::string host_init_error;
    DWORD host_thread = 0;
#endif
    std::unordered_map<plugin_handle_t, rec> plugins;
    std::unordered_map<plugin_handle_t, std::string> last_errors;
    std::string last_error;
};

// ── utf8 ↔ wide helpers (independent of pymini_host's anon-namespace) ──
namespace {

std::wstring widen_u8(const std::string& u8) {
    if (u8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(),
                                      static_cast<int>(u8.size()), nullptr,
                                      0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()),
                        w.data(), n);
    return w;
}
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
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
#endif

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

void set_err(pymini_adapter_owner_s* o, plugin_handle_t plugin, const std::string& e) {
    if (e.empty())
        o->last_errors.erase(plugin);
    else
        o->last_errors[plugin] = e;
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

int32_t cpython_thread_status(pymini_adapter_owner_s* o, std::string* err) {
    if (o->host_thread != 0 && o->host_thread != GetCurrentThreadId()) {
        *err = "CPython cold-init host requires its initialization thread; host bridge has no detach API";
        return loader::SAO_PLUGINS_ERR_BUSY;
    }
    return SAO_OK;
}

int32_t ensure_cpython_host(pymini_adapter_owner_s* o, std::string* err) {
    if (o->host_init_attempted) {
        *err = o->host_init_error;
        return o->host_init_status == SAO_OK ? cpython_thread_status(o, err)
                                            : o->host_init_status;
    }
    o->host_init_error = "CPython host initialization failed before plugin execution";
    o->host_init_status = SAO_ERR_OS_CALL_FAILED;
    o->host_init_attempted = true;
    if (o->python_home.empty()) {
        o->host_init_status = loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        o->host_init_error = "CPython required but python_home is unconfigured";
    } else {
        python_host::py_host_config hc{};
        hc.python_home = o->python_home.c_str();
        std::vector<const wchar_t*> extra_ptrs;
        extra_ptrs.reserve(o->extra_dirs.size());
        for (const auto& dir : o->extra_dirs)
            extra_ptrs.push_back(dir.c_str());
        hc.extra_module_dirs = extra_ptrs.data();
        hc.extra_module_dirs_count = static_cast<uint32_t>(extra_ptrs.size());
        // A cold init retains the GIL; the existing bridge only releases ensured scopes.
        gil_scope initial_gil(true);
        o->host_init_status = python_host::sao_plugins_pyhost_init(&hc, &o->host);
        if (o->host_init_status == SAO_OK && !o->host)
            o->host_init_status = SAO_ERR_OS_CALL_FAILED;
        if (o->host_init_status == SAO_OK) {
            if (!initial_gil.held())
                o->host_thread = GetCurrentThreadId();
            o->host_init_error.clear();
        } else {
            o->host_init_error += ": python_home=" + narrow_w(o->python_home) +
                                  ", status=" + std::to_string(o->host_init_status);
            char* detail = nullptr;
            (void)python_host::sao_plugins_pyhost_take_error(&detail);
            if (detail) {
                o->host_init_error += ": ";
                o->host_init_error += detail;
                python_host::sao_plugins_pyhost_free_string(detail);
            }
        }
    }
    *err = o->host_init_error;
    return o->host_init_status;
}

void pyh_error(rec& r, std::string* err) {
    char* text = nullptr;
    if (r.pyh)
        (void)python_host::sao_plugins_pyhost_get_last_error(r.pyh, &text);
    if (text) {
        *err = text;
        python_host::sao_plugins_pyhost_free_string(text);
    }
}

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
    r->pyh = pyh;
    if (st != SAO_OK || !pyh) {
        // pyhost keeps a published handle on load failure so last_error stays
        // introspectable — harvest it, then release the handle so
        // pyhost_shutdown does not see a leaked active_plugin.
        if (err) {
            char* pe = nullptr;
            if (pyh)
                (void)python_host::sao_plugins_pyhost_get_last_error(pyh, &pe);
            *err = pe ? pe : "pyhost load failed";
            if (pe)
                python_host::sao_plugins_pyhost_free_string(pe);
        }
        if (pyh && python_host::sao_plugins_pyhost_unload_plugin(pyh) == SAO_OK)
            r->pyh = nullptr;
        return st == SAO_OK ? SAO_ERR_OS_CALL_FAILED : st;
    }
    // bind the canonical loader ctx (the pyhost ctx PyObject ↔ plugin_context_t)
    const int32_t bs = python_host::sao_plugins_pyhost_ctx_bind_loader_context(
        python_host::sao_plugins_pyhost_get_ctx_pyobject(pyh),
        lctx);
    if (bs != SAO_OK) {
        if (python_host::sao_plugins_pyhost_unload_plugin(pyh) == SAO_OK)
            r->pyh = nullptr;
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
    int32_t st = SAO_ERR_INVALID_ARGUMENT;
    if (std::strcmp(which, "on_load") == 0)
        st = python_host::sao_plugins_pyhost_call_on_load(r.pyh);
    else if (std::strcmp(which, "on_enable") == 0)
        st = python_host::sao_plugins_pyhost_call_on_enable(r.pyh);
    else if (std::strcmp(which, "on_disable") == 0)
        st = python_host::sao_plugins_pyhost_call_on_disable(r.pyh);
    if (st != SAO_OK)
        pyh_error(r, err);
    return st;
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
    if (st != SAO_OK)
        pyh_error(r, err);
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
    if (st == SAO_OK)
        r.pyh = nullptr;
    else
        pyh_error(r, err);
    return st;
}
#endif

// ── route decision ───────────────────────────────────────────────────
int32_t classify_plugin(const plugin_manifest* m, const pymini_adapter_owner_s* o,
                        route_t* route, std::string* reason) {
    *route = route_t::pymini;
    reason->clear();
    if (!m || m->language != loader::engine_kind::python || !m->native_entry.empty() ||
        m->source_path.empty() || m->entry.empty() ||
        m->source_path.find('\0') != std::string::npos ||
        m->entry.find('\0') != std::string::npos) {
        *reason = "expected a Python manifest with source_path and entry";
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const auto root = widen_u8(m->source_path);
    const auto entry = widen_u8(m->entry);
    if (root.empty() || entry.empty()) {
        *reason = "invalid UTF-8 in plugin path";
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!m->py_runtime.empty() && m->py_runtime != "auto" &&
        m->py_runtime != "pymini" && m->py_runtime != "cpython") {
        *reason = "invalid py_runtime: " + m->py_runtime;
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        if (m->py_runtime == "cpython") {
            const std::filesystem::path relative(entry);
            if (relative.has_root_path() || entry.find(L':') != std::wstring::npos) {
                *reason = "CPython entry must be a relative path";
                return SAO_ERR_INVALID_ARGUMENT;
            }
            for (const auto& part : relative)
                if (part == L"..") {
                    *reason = "CPython entry escapes plugin root";
                    return SAO_ERR_INVALID_ARGUMENT;
                }
            const auto path = std::filesystem::path(root) / relative;
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                             nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                *reason = m->entry + ": CPython entry read failed, win32=" +
                          std::to_string(GetLastError());
                return SAO_ERR_OS_CALL_FAILED;
            }
            CloseHandle(file);
            *route = route_t::cpython;
            *reason = "py_runtime:cpython requested";
            return SAO_OK;
        }
        const bool native = pymini_preflight_plugin(root, entry, o->extra_dirs, reason);
        if (native) {
            reason->clear();
            return SAO_OK;
        }
        if (m->py_runtime == "pymini") {
            *reason = "py_runtime:pymini does not support " + *reason;
            return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        }
        *route = route_t::cpython;
        return SAO_OK;
    } catch (const py_error& e) {
        *reason = (e.file.empty() ? m->entry : e.file) + ":" +
                  std::to_string(e.pos.line) + ": " + e.kind + ": " + e.message;
        return e.code != SAO_OK && e.code != loader::SAO_PLUGINS_ERR_UNSUPPORTED
                   ? e.code : SAO_ERR_OS_CALL_FAILED;
    } catch (const std::exception& e) {
        *reason = e.what();
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        *reason = "plugin preflight failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// ── vtable functions ─────────────────────────────────────────────────
int32_t SAO_PLUGINS_CALL adapter_load(
    plugin_handle_t plugin, const plugin_manifest* manifest,
    void* host_user_data) try {
    pymini_adapter_owner_s* o = active_owner(host_user_data);
    std::lock_guard<std::mutex> g(g_mu);
    if (!o || o != g_owner || !o->active)
        return SAO_ERR_NOT_INITIALIZED;
    if (!plugin || !manifest)
        return SAO_ERR_INVALID_ARGUMENT;
    if (o->plugins.count(plugin))
        return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
    try {
        set_err(o, plugin, {});
        route_t route;
        std::string reason;
        int32_t status = classify_plugin(manifest, o, &route, &reason);
        if (status != SAO_OK) {
            set_err(o, plugin, reason);
            return status;
        }
        if (route == route_t::cpython) {
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
            std::string host_error;
            status = ensure_cpython_host(o, &host_error);
            if (status != SAO_OK) {
                set_err(o, plugin, reason + "; " + host_error);
                return status;
            }
#else
            set_err(o, plugin, reason + "; CPython delegate not built");
            return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
#endif
        }
        plugin_context_t* lctx = nullptr;
        if (loader::sao_plugins_lifecycle_get_context(plugin, &lctx) != SAO_OK || !lctx) {
            set_err(o, plugin, "loader plugin context unavailable");
            return SAO_ERR_NOT_INITIALIZED;
        }
        sao::plugins::script_ctx::ctx_surface_advisory_check(
            loader::engine_kind::python, lctx, manifest);
        rec candidate;
        candidate.route = route;
        candidate.plugin_id = manifest->plugin_id;
        rec& slot = o->plugins.emplace(plugin, std::move(candidate)).first->second;
        const std::wstring dir = widen_u8(manifest->source_path);
        std::string err;
        if (route == route_t::pymini) {
            const int rc = pymini_host_load_plugin(
                lctx, manifest->plugin_id.c_str(), dir.c_str(),
                widen_u8(manifest->entry).c_str(), o->extra_dirs, &err);
            status = rc == 0 ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
        } else {
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
            status = pyh_load(o, lctx, dir, manifest->entry, manifest->plugin_id, &slot, &err);
#endif
        }
        if (status != SAO_OK) {
            bool retained = route == route_t::pymini &&
                            pymini_host_is_loaded(manifest->plugin_id.c_str());
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
            retained = retained || slot.pyh != nullptr;
#endif
            if (!retained)
                o->plugins.erase(plugin);
            else
                slot.load_status = status;
            set_err(o, plugin, err.empty() ? "plugin load failed" : err);
            // Resident failures enter on_load so lifecycle rollback owns their cleanup.
            return retained ? SAO_OK : status;
        }
        slot.ready = true;
        set_err(o, plugin, {});
        return SAO_OK;
    } catch (const std::exception& e) {
        set_err(o, plugin, e.what());
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        set_err(o, plugin, "plugin load failed before completion");
        return SAO_ERR_OS_CALL_FAILED;
    }
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t call_named(plugin_handle_t plugin, void* ud, const char* hook,
                   bool* allow_unload = nullptr) try {
    pymini_adapter_owner_s* o = active_owner(ud);
    if (!o)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard<std::mutex> g(g_mu);
    if (o != g_owner || !o->active)
        return SAO_ERR_NOT_INITIALIZED;
    auto it = o->plugins.find(plugin);
    if (it == o->plugins.end()) {
        if (allow_unload && o->last_errors.count(plugin)) {
            *allow_unload = true;
            return SAO_OK;
        }
        return SAO_ERR_HANDLE_INVALID;
    }
    rec& r = it->second;
    if (!r.ready) {
        if (allow_unload) {
            *allow_unload = true;
            return SAO_OK;
        }
        return r.load_status;
    }
    std::string err;
    int32_t status = SAO_OK;
    try {
        if (r.route == route_t::pymini) {
            int rc = 0;
            if (allow_unload) {
                rc = pymini_host_call_hook_allow_unload(
                    r.plugin_id.c_str(), hook, allow_unload, &err);
            } else {
                rc = pymini_host_call_hook(r.plugin_id.c_str(), hook, nullptr, &err);
            }
            status = rc == 0 || rc == 1 ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
        } else {
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
            status = cpython_thread_status(o, &err);
            if (status == SAO_OK)
                status = allow_unload ? pyh_on_unload(r, allow_unload, &err)
                                      : pyh_hook(r, hook, &err);
#else
            status = loader::SAO_PLUGINS_ERR_UNSUPPORTED;
#endif
        }
    } catch (const std::exception& e) {
        status = SAO_ERR_OS_CALL_FAILED;
        err = e.what();
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
        err = "plugin hook failed";
    }
    if (status != SAO_OK && err.empty())
        err = std::string(hook) + " failed: status=" + std::to_string(status);
    if (status != SAO_OK || !allow_unload)
        set_err(o, plugin, status == SAO_OK ? std::string{} : err);
    return status;
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
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
    if (!allow)
        return SAO_ERR_INVALID_ARGUMENT;
    *allow = false;
    return call_named(plugin, ud, "on_unload", allow);
}

int32_t SAO_PLUGINS_CALL adapter_unload(plugin_handle_t plugin,
                                        void* ud) try {
    pymini_adapter_owner_s* o = active_owner(ud);
    if (!o)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard<std::mutex> g(g_mu);
    if (o != g_owner)
        return SAO_ERR_HANDLE_INVALID;
    auto it = o->plugins.find(plugin);
    if (it == o->plugins.end())
        return o->last_errors.count(plugin) ? SAO_OK : SAO_ERR_HANDLE_INVALID;
    rec& r = it->second;
    std::string err;
    int32_t status = SAO_OK;
    try {
        if (r.route == route_t::pymini) {
            if (pymini_host_is_loaded(r.plugin_id.c_str()))
                status = pymini_host_unload_plugin(r.plugin_id.c_str(), &err);
        } else {
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
            status = cpython_thread_status(o, &err);
            if (status == SAO_OK)
                status = pyh_unload(r, &err);
#endif
        }
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
        err = "plugin unload failed";
    }
    if (status == SAO_OK) {
        o->plugins.erase(it);
    } else {
        set_err(o, plugin, err.empty() ? "plugin unload failed" : err);
    }
    return status;
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t SAO_PLUGINS_CALL adapter_last_error(void* user_data,
                                            loader::plugin_handle_t plugin, char** out_utf8) {
    return sao_plugins_pymini_adapter_get_last_error(
        static_cast<pymini_adapter_owner_t>(user_data), plugin, out_utf8);
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
    t.get_last_error = adapter_last_error;
    t.free_error_string = sao_plugins_pymini_free_string;
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
        if (owner != g_owner)
            return SAO_ERR_HANDLE_INVALID;
        if (!owner->plugins.empty())
            return loader::SAO_PLUGINS_ERR_BUSY;
        int32_t st = SAO_OK;
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
        if (owner->host) {
            st = cpython_thread_status(owner, &owner->last_error);
            if (st != SAO_OK)
                return st;
        }
#endif
        if (owner->adapter_registered) {
            st = loader::sao_plugins_lifecycle_unregister_host_adapter(
                loader::engine_kind::python);
            if (st != SAO_OK) {
                return st;
            }
            owner->adapter_registered = false;
        }
        owner->active = false;
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
        if (owner->host) {
            gil_scope scope(true);
            if (!scope.held()) {
                owner->last_error = "cannot acquire CPython GIL for shutdown";
                return SAO_ERR_OS_CALL_FAILED;
            }
            st = python_host::sao_plugins_pyhost_shutdown(owner->host);
            if (st != SAO_OK) {
                owner->last_error = "CPython shutdown failed: status=" + std::to_string(st);
                return st;
            }
            owner->host = nullptr;
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
    try {
        std::lock_guard<std::mutex> g(g_mu);
        return owner && owner == g_owner ? owner->plugins.size() : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_adapter_requires_python(
    pymini_adapter_owner_t owner, const plugin_manifest* manifest,
    bool* out_required, char** out_reason) {
    if (out_required)
        *out_required = false;
    if (out_reason)
        *out_reason = nullptr;
    if (!owner || !manifest || !out_required || !out_reason)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> g(g_mu);
        if (owner != g_owner || !owner->active)
            return SAO_ERR_HANDLE_INVALID;
        route_t route;
        std::string reason;
        const int32_t status = classify_plugin(manifest, owner, &route, &reason);
        *out_reason = dup_err(reason);
        if (!reason.empty() && !*out_reason)
            return SAO_ERR_OS_CALL_FAILED;
        *out_required = status == SAO_OK && route == route_t::cpython;
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_adapter_get_last_error(
    pymini_adapter_owner_t owner, void* loader_plugin_handle,
    char** out_utf8) {
    if (out_utf8)
        *out_utf8 = nullptr;
    if (!owner || !out_utf8)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> g(g_mu);
        if (owner != g_owner)
            return SAO_ERR_HANDLE_INVALID;
        const std::string* error = &owner->last_error;
        if (loader_plugin_handle) {
            const auto found = owner->last_errors.find(
                static_cast<plugin_handle_t>(loader_plugin_handle));
            if (found == owner->last_errors.end())
                return SAO_ERR_HANDLE_INVALID;
            error = &found->second;
        }
        if (error->empty())
            return SAO_ERR_HANDLE_INVALID;
        *out_utf8 = dup_err(*error);
        return *out_utf8 ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
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
