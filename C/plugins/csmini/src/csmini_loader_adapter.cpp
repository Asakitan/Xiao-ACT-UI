// csmini_loader_adapter.cpp — engine_kind::csharp 的 composite production
// adapter：csmini（默认）+ coreclr 委托（cs_runtime:coreclr / 子集外 /
// .dll entry）。镜像 pymini_loader_adapter.cpp。
//
// 路由顺序（load_plugin 内决定，一次一插件）：
//   1. manifest.cs_runtime == "csmini"  → csmini
//   2. manifest.cs_runtime == "coreclr" → coreclr 委托（无 host → UNSUPPORTED）
//   3. entry 以 .dll 结尾              → coreclr REQUIRED（无 host → UNSUPPORTED）
//   4. 其它（auto/缺失）→ 读取 entry 源码并跑 csmini_preflight_subset：
//        子集内 → csmini；子集外 → coreclr 委托（无 → UNSUPPORTED，错误串
//        携带命中的特性名）。
//
// coreclr 委托复用 csharp_host 的内部 component 链（与
// cs_loader_adapter 的 assembly 序列一致），仅本 TU 在
// SAO_PLUGINS_ENABLE_CORECLR 下接触 csharp_host internals。
#include "sao/plugins/csmini/csmini_host.h"

#include "csmini_host_internal.h"

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/script_ctx/ctx_surface.h"

#if defined(SAO_PLUGINS_ENABLE_CORECLR)
#include "sao/plugins/csharp_host/cs_host.h"
#include "sao/plugins/sdk_binding/binding_csharp.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_provider.h"

#include "cs_component_internal.h"
#include "cs_sdk_bridge_internal.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sao::plugins::csmini {

using loader::host_adapter_vtable;
using loader::plugin_context_t;
using loader::plugin_handle_t;
using loader::plugin_manifest;

// adapter owner — concrete def of the opaque public handle
struct csmini_adapter_owner_s {
    // per-plugin record
    enum class route_e { csmini, coreclr };
    struct rec {
        route_e route = route_e::csmini;
        std::string plugin_id;
        std::string last_error;
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
        csharp_host::managed_component_s* component = nullptr;
        plugin_context_t* context = nullptr;
        SaoSdkContext* sdk_context = nullptr;
        sdk_binding::plugin_binding_handle_t binding = nullptr;
        csharp_host::sdk_bridge_session* sdk_session = nullptr;
        bool on_load_succeeded = false;
#endif
    };
    bool active = false;
    bool adapter_registered = false;
    std::wstring dotnet_root;
    std::vector<std::wstring> extra_dirs;
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
    csharp_host::cs_host_handle_t host = nullptr;
#endif
    std::unordered_map<plugin_handle_t, rec> plugins;
    std::unordered_map<plugin_handle_t, std::string> last_errors;
    std::string last_error;
};

// ── utf8 ↔ wide helpers ──────────────────────────────────────────────
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

// error-string helper owned by free via sao_plugins_csmini_free_string
char* dup_err(const std::string& s) {
    if (s.empty())
        return nullptr;
    char* b = static_cast<char*>(std::malloc(s.size() + 1));
    if (!b)
        return nullptr;
    std::memcpy(b, s.data(), s.size() + 1);
    return b;
}

using rec = csmini_adapter_owner_s::rec;
using route_t = csmini_adapter_owner_s::route_e;

std::mutex g_mu;
csmini_adapter_owner_s* g_owner = nullptr;

csmini_adapter_owner_s* active_owner(void* ud) {
    return static_cast<csmini_adapter_owner_s*>(ud);
}

void set_err(csmini_adapter_owner_s* o, std::string e) {
    if (o)
        o->last_error = std::move(e);
}
void retain_error(csmini_adapter_owner_s* o, plugin_handle_t plugin,
                  std::string e) {
    set_err(o, e);
    if (o)
        o->last_errors[plugin] = std::move(e);
}
void clear_error(csmini_adapter_owner_s* o, plugin_handle_t plugin) {
    if (o)
        o->last_errors.erase(plugin);
}

// ── coreclr delegate wrappers (only under SAO_PLUGINS_ENABLE_CORECLR) ──
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
using csharp_host::managed_hook;

// locate hostfxr.dll under a dotnet root (<root>\host\fxr\<ver>\hostfxr.dll);
// newest directory wins. empty root → nullptr (host init auto-probes).
std::wstring find_hostfxr(const std::wstring& dotnet_root) {
    if (dotnet_root.empty())
        return {};
    const std::wstring base = dotnet_root + L"\\host\\fxr";
    std::wstring best;
    WIN32_FIND_DATAW d{};
    HANDLE h = FindFirstFileExW((base + L"\\*").c_str(), FindExInfoBasic, &d,
                                FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                d.cFileName[0] != L'.') {
                std::wstring cand =
                    base + L"\\" + d.cFileName + L"\\hostfxr.dll";
                if (GetFileAttributesW(cand.c_str()) !=
                    INVALID_FILE_ATTRIBUTES)
                    best = std::move(cand);
            }
        } while (FindNextFileW(h, &d));
        FindClose(h);
    }
    if (best.empty()) {
        // caller may have passed a hostfxr dir or the fxr version dir itself
        std::wstring cand = dotnet_root + L"\\hostfxr.dll";
        if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES)
            best = std::move(cand);
    }
    return best;
}

// replicate the cs_loader_adapter assembly sequence for one plugin.
int32_t csh_load(csmini_adapter_owner_s* o, plugin_context_t* lctx,
                 const plugin_manifest* manifest, rec* r,
                 std::string* err) {
    csharp_host::managed_component_s* component = nullptr;
    SaoSdkContext* sdk_context = nullptr;
    sdk_binding::plugin_binding_handle_t binding = nullptr;
    csharp_host::sdk_bridge_session* sdk_session = nullptr;
    int32_t status =
        csharp_host::cshost_component_load(o->host, *manifest, &component,
                                         *err);
    if (status == SAO_OK) {
        status = sao_sdk_context_create(manifest->source_path.c_str(),
                                        manifest->plugin_id.c_str(),
                                        &sdk_context);
        if (status == SAO_OK)
            status = sao_sdk_context_bind_platform_services(sdk_context);
        if (status != SAO_OK && err)
            *err = "C# SDK context initialization failed";
    }
    if (status == SAO_OK) {
        status = csharp_host::cshost_component_attach_contexts(
            component, sdk_context, lctx);
        if (status == SAO_OK)
            status = csharp_host::cshost_sdk_bridge_prepare(
                component, lctx, sdk_context, component);
        if (status == SAO_OK) {
            status = sdk_binding::sao_plugins_binding_csharp_activate(
                reinterpret_cast<sdk_binding::plugin_context_ptr>(lctx),
                reinterpret_cast<sdk_binding::csharp_domain_ptr>(component),
                &binding);
        }
        csharp_host::cshost_sdk_bridge_cancel(component);
        if (status == SAO_OK) {
            sdk_session = csharp_host::cshost_sdk_session_find(component);
            status = sdk_session == nullptr
                         ? SAO_ERR_NOT_INITIALIZED
                         : csharp_host::cshost_sdk_bridge_set_binding(
                               sdk_session, binding);
        }
        if (status != SAO_OK && err)
            *err = "C# SDK binding provider activation failed";
    }
    if (status == SAO_OK) {
        status = csharp_host::cshost_component_initialize(
            component, sdk_context, lctx, *err);
    }
    if (status != SAO_OK) {
        // teardown partial state best-effort (no record committed yet)
        if (binding != nullptr)
            (void)sdk_binding::sao_plugins_binding_csharp_deactivate(
                binding);
        if (sdk_context != nullptr)
            (void)sao_sdk_context_try_destroy(sdk_context);
        if (component != nullptr) {
            std::string close_err;
            (void)csharp_host::cshost_component_close(component, close_err);
        }
        return status;
    }
    r->component = component;
    r->context = lctx;
    r->sdk_context = sdk_context;
    r->binding = binding;
    r->sdk_session = sdk_session;
    return SAO_OK;
}

// generic managed hook invoke — optional hooks skip when absent.
int32_t csh_invoke(csmini_adapter_owner_s* o, plugin_handle_t ph, rec& r,
                   managed_hook hook, const char* hook_name, bool optional,
                   int32_t* managed_result) {
    if (!r.component)
        return SAO_ERR_HANDLE_INVALID;
    if (!csharp_host::cshost_component_has_hook(r.component, hook)) {
        if (optional) {
            if (managed_result)
                *managed_result = 0;
            return SAO_OK;
        }
        retain_error(o, ph, std::string("required managed hook missing: ") +
                                hook_name);
        return SAO_ERR_HANDLE_INVALID;
    }
    std::string error;
    const int32_t status = csharp_host::cshost_component_invoke(
        r.component, hook, nullptr, 0, managed_result, error);
    if (status != SAO_OK)
        retain_error(o, ph,
                     error.empty()
                         ? std::string("managed hook failed: ") + hook_name
                         : std::string(hook_name) + ": " + error);
    return status;
}

int32_t csh_hook(csmini_adapter_owner_s* o, plugin_handle_t ph, rec& r,
                 const char* which, std::string* err) {
    if (!r.component)
        return SAO_ERR_HANDLE_INVALID;
    int32_t managed_result = 0;
    int32_t status = SAO_OK;
    if (std::strcmp(which, "on_load") == 0) {
        std::string error;
        status = csharp_host::cshost_component_on_load(
            r.component, &managed_result, error);
        if (status == SAO_OK && managed_result != 0) {
            status = SAO_ERR_OS_CALL_FAILED;
            error = "OnLoad returned " + std::to_string(managed_result);
        }
        if (status != SAO_OK) {
            retain_error(o, ph, error.empty() ? "OnLoad failed" : error);
            return status;
        }
        r.on_load_succeeded = true;
        clear_error(o, ph);
        return SAO_OK;
    }
    if (std::strcmp(which, "on_enable") == 0)
        status = csh_invoke(o, ph, r, managed_hook::on_enable, "OnEnable",
                            true, &managed_result);
    else if (std::strcmp(which, "on_disable") == 0)
        status = csh_invoke(o, ph, r, managed_hook::on_disable, "OnDisable",
                            true, &managed_result);
    else
        return SAO_ERR_INVALID_ARGUMENT;
    if (status != SAO_OK)
        return status;
    if (managed_result != 0) {
        retain_error(o, ph, std::string(which) + " returned " +
                                std::to_string(managed_result));
        return SAO_ERR_OS_CALL_FAILED;
    }
    clear_error(o, ph);
    return SAO_OK;
}

int32_t csh_on_unload(csmini_adapter_owner_s* o, plugin_handle_t ph, rec& r,
                      bool* allow, std::string* err) {
    if (!r.component) {
        *allow = true;
        return SAO_OK;
    }
    if (!csharp_host::cshost_component_has_hook(r.component,
                                              managed_hook::on_unload)) {
        *allow = true;
        clear_error(o, ph);
        return SAO_OK;
    }
    int32_t managed_result = 0;
    std::string error;
    const int32_t status = csharp_host::cshost_component_invoke(
        r.component, managed_hook::on_unload, nullptr, 0, &managed_result,
        error);
    if (!r.on_load_succeeded) {
        // rollback path — ignore veto/result, allow unload
        *allow = true;
        if (status != SAO_OK || managed_result != 0)
            retain_error(o, ph, "OnUnload rollback result ignored: " +
                                    (error.empty()
                                         ? std::to_string(managed_result)
                                         : error));
        return SAO_OK;
    }
    if (status != SAO_OK) {
        retain_error(o, ph,
                     error.empty() ? "OnUnload failed" : error);
        return status;
    }
    if (managed_result == 0) {
        *allow = true;
        clear_error(o, ph);
        return SAO_OK;
    }
    if (managed_result == 1) {
        retain_error(o, ph, "OnUnload vetoed unload");
        *allow = false;
        return SAO_OK;
    }
    retain_error(o, ph, "OnUnload returned invalid result " +
                            std::to_string(managed_result));
    return SAO_ERR_OS_CALL_FAILED;
}

// teardown mirroring adapter_unload's managed-chain sequence.
int32_t csh_unload(csmini_adapter_owner_s* o, plugin_handle_t ph, rec& r,
                   std::string* err) {
    csharp_host::managed_component_s* component = r.component;
    SaoSdkContext* sdk_context = r.sdk_context;
    sdk_binding::plugin_binding_handle_t binding = r.binding;
    csharp_host::sdk_bridge_session* sdk_session = r.sdk_session;
    if (sdk_session != nullptr) {
        const int32_t status =
            csharp_host::cshost_sdk_session_quiesce(sdk_session);
        if (status != SAO_OK) {
            if (err)
                *err = "C# managed SDK session quiesce failed";
            return status;
        }
    }
    if (sdk_context != nullptr) {
        int32_t status = sao_sdk_context_try_destroy(sdk_context);
        if (status == SAO_SDK_ERR_BUSY)
            status = loader::SAO_PLUGINS_ERR_BUSY;
        if (status != SAO_OK) {
            if (sdk_session != nullptr)
                (void)csharp_host::cshost_sdk_session_resume(sdk_session);
            if (err)
                *err = "C# SDK context teardown failed";
            return status;
        }
        if (sdk_session != nullptr)
            (void)csharp_host::cshost_sdk_session_clear_sdk_context(
                sdk_session, sdk_context);
        r.sdk_context = nullptr;
    }
    if (binding != nullptr) {
        int32_t status =
            csharp_host::cshost_sdk_session_release_callbacks(sdk_session);
        if (status == SAO_OK)
            status = sdk_binding::sao_plugins_binding_csharp_deactivate(
                binding);
        if (status != SAO_OK) {
            if (err)
                *err = "C# SDK binding provider teardown failed";
            return status;
        }
        (void)csharp_host::cshost_sdk_bridge_set_binding(sdk_session,
                                                         nullptr);
        r.binding = nullptr;
    }
    if (sdk_session != nullptr) {
        const int32_t status =
            csharp_host::cshost_sdk_bridge_finish(sdk_session);
        if (status != SAO_OK) {
            if (err)
                *err = "C# managed callback release failed";
            return status;
        }
        r.sdk_session = nullptr;
    }
    std::string close_err;
    const int32_t close_status =
        csharp_host::cshost_component_close(component, close_err);
    r.component = nullptr;
    if (close_status != SAO_OK) {
        if (err)
            *err = std::move(close_err);
        return close_status;
    }
    return SAO_OK;
}
#endif // SAO_PLUGINS_ENABLE_CORECLR

// ── route decision ───────────────────────────────────────────────────
route_t decide_route(const plugin_manifest* m,
                     csmini_adapter_owner_s* o,
                     const std::wstring& abs_entry,
                     std::string* note) {
    const std::string hint = m && !m->cs_runtime.empty()
                                 ? m->cs_runtime
                                 : std::string("auto");
    if (hint == "csmini")
        return route_t::csmini;
    if (hint == "coreclr") {
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
        if (o->host)
            return route_t::coreclr;
#endif
        if (note)
            *note =
                "cs_runtime:coreclr requested but no .NET runtime host "
                "available";
        return route_t::csmini;         // caller checks note → UNSUPPORTED
    }
    // .dll entry always requires the managed host (fail-closed, no implicit
    // fallback to the subset interpreter).
    const std::string entry_l = m ? m->entry : std::string();
    if (entry_l.size() >= 4 &&
        (entry_l.compare(entry_l.size() - 4, 4, ".dll") == 0 ||
         entry_l.compare(entry_l.size() - 4, 4, ".DLL") == 0)) {
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
        if (o->host)
            return route_t::coreclr;
#endif
        if (note)
            *note =
                "entry is a managed assembly (.dll) — requires .NET runtime";
        return route_t::csmini;
    }
    // auto: read entry + preflight subset scan
    const std::string src = read_all(abs_entry);
    std::string why;
    if (csmini_preflight_subset(src, &why))
        return route_t::csmini;
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
    if (o->host)
        return route_t::coreclr;
#endif
    if (note)
        *note = "requires coreclr-only feature: " + why;
    return route_t::csmini;             // caller checks note → UNSUPPORTED
}

// ── vtable functions ─────────────────────────────────────────────────
int32_t SAO_PLUGINS_CALL adapter_load(
    plugin_handle_t plugin, const plugin_manifest* manifest,
    void* host_user_data) {
    csmini_adapter_owner_s* o = active_owner(host_user_data);
    if (!o || !o->active)
        return SAO_ERR_NOT_INITIALIZED;
    if (!plugin || !manifest)
        return SAO_ERR_INVALID_ARGUMENT;

    plugin_context_t* lctx = nullptr;
    if (loader::sao_plugins_lifecycle_get_context(plugin, &lctx) != SAO_OK ||
        !lctx)
        return SAO_ERR_NOT_INITIALIZED;

    sao::plugins::script_ctx::ctx_surface_advisory_check(
        loader::engine_kind::csharp, lctx, manifest);

    const std::wstring dir = widen_u8(manifest->source_path);
    const std::wstring abs_entry = dir + L'\\' + widen_u8(manifest->entry);

    std::string note;
    const route_t route = decide_route(manifest, o, abs_entry, &note);
    if (!note.empty()) {
        set_err(o, note);
        retain_error(o, plugin, note);
        return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    }

    std::lock_guard<std::mutex> g(g_mu);
    if (o->plugins.count(plugin))
        return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
    rec r;
    r.route = route;
    r.plugin_id = manifest->plugin_id;
    o->plugins.emplace(plugin, r);
    rec& slot = o->plugins[plugin];

    std::string err;
    if (route == route_t::csmini) {
        const int rc = csmini_host_load_plugin(
            lctx, manifest->plugin_id.c_str(), dir.c_str(),
            widen_u8(manifest->entry).c_str(), o->extra_dirs, &err);
        if (rc != 0) {
            o->plugins.erase(plugin);
            retain_error(o, plugin, err);
            return SAO_ERR_OS_CALL_FAILED;
        }
        return SAO_OK;
    }
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
    const int32_t st = csh_load(o, lctx, manifest, &slot, &err);
    if (st != SAO_OK) {
        o->plugins.erase(plugin);
        retain_error(o, plugin, err);
        return st;
    }
    return SAO_OK;
#else
    o->plugins.erase(plugin);
    retain_error(o, plugin,
                 "coreclr delegate not built "
                 "(SAO_PLUGINS_ENABLE_CORECLR off)");
    return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
#endif
}

int32_t call_named(plugin_handle_t plugin, void* ud, const char* hook,
                   bool* allow_unload = nullptr) {
    csmini_adapter_owner_s* o = active_owner(ud);
    if (!o)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = o->plugins.find(plugin);
    if (it == o->plugins.end())
        return SAO_ERR_HANDLE_INVALID;
    rec& r = it->second;
    std::string err;
    if (r.route == route_t::csmini) {
        if (allow_unload) {
            CsRef ret;
            const int rc = csmini_host_call_hook_ret(
                r.plugin_id.c_str(), hook, nullptr, &ret, &err);
            if (rc == 0) {
                // bool OnUnload veto — missing/false return allows unload
                *allow_unload =
                    ret && as_bool(ret) ? as_bool(ret)->v : true;
            } else if (rc == 1) {
                *allow_unload = true;      // hook absent → allow
            } else {
                retain_error(o, plugin, err);
                return SAO_ERR_OS_CALL_FAILED;
            }
            return SAO_OK;
        }
        const int rc =
            csmini_host_call_hook(r.plugin_id.c_str(), hook, nullptr, &err);
        if (rc == 0 || rc == 1)
            return SAO_OK;
        retain_error(o, plugin, err);
        return SAO_ERR_OS_CALL_FAILED;
    }
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
    if (allow_unload)
        return csh_on_unload(o, plugin, r, allow_unload, &err);
    return csh_hook(o, plugin, r, hook, &err);
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
    csmini_adapter_owner_s* o = active_owner(ud);
    if (!o)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = o->plugins.find(plugin);
    if (it == o->plugins.end())
        return SAO_ERR_HANDLE_INVALID;
    rec r = it->second;
    o->plugins.erase(it);
    std::string err;
    if (r.route == route_t::csmini)
        return csmini_host_unload_plugin(r.plugin_id.c_str(), &err);
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
    return csh_unload(o, plugin, r, &err);
#else
    return SAO_OK;
#endif
}

int32_t SAO_PLUGINS_CALL adapter_last_error(void* user_data,
                                            loader::plugin_handle_t plugin, char** out_utf8) {
    return sao_plugins_csmini_adapter_get_last_error(
        static_cast<csmini_adapter_owner_t>(user_data), plugin, out_utf8);
}

host_adapter_vtable make_vtable(csmini_adapter_owner_s* o) {
    host_adapter_vtable t{};
    t.load_plugin = adapter_load;
    t.call_on_load = adapter_on_load;
    t.call_on_enable = adapter_on_enable;
    t.call_on_disable = adapter_on_disable;
    t.call_on_unload = adapter_on_unload;
    t.unload_plugin = adapter_unload;
    t.host_user_data = o;
    t.get_last_error = adapter_last_error;
    t.free_error_string = sao_plugins_csmini_free_string;
    return t;
}

} // namespace

// ═══ public C-ABI exports ═══════════════════════════════════════════
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_register_loader_adapter(
    const csmini_adapter_config* cfg,
    csmini_adapter_owner_t* out_owner) {
    if (!out_owner)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_owner = nullptr;
    try {
        std::lock_guard<std::mutex> g(g_mu);
        if (g_owner)
            return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        auto o = std::make_unique<csmini_adapter_owner_s>();
        if (cfg && cfg->dotnet_root)
            o->dotnet_root = cfg->dotnet_root;
        if (cfg && cfg->extra_assembly_dirs)
            for (uint32_t k = 0; k < cfg->extra_assembly_dirs_count; ++k)
                if (cfg->extra_assembly_dirs[k])
                    o->extra_dirs.emplace_back(
                        cfg->extra_assembly_dirs[k]);

#if defined(SAO_PLUGINS_ENABLE_CORECLR)
        // lazy init: spin up the managed host whenever a dotnet runtime
        // probes available.  An explicit cfg->dotnet_root is preferred for
        // hostfxr discovery; when absent, cshost_init auto-probes
        // DOTNET_ROOT / DOTNET_ROOT_X64 / %ProgramFiles%\dotnet.
        {
            bool avail = false;
            const int32_t probe_st = csharp_host::sao_plugins_cshost_is_available(&avail);
            if (probe_st == SAO_OK && avail) {
                const std::wstring fxr = find_hostfxr(o->dotnet_root);
                csharp_host::cs_host_config hc{};
                hc.hostfxr_path = fxr.empty() ? nullptr : fxr.c_str();
                const int32_t init_st =
                    csharp_host::sao_plugins_cshost_init(&hc, &o->host);
                if (init_st == SAO_OK && o->host != nullptr) {
                    (void)csharp_host::
                        cshost_register_sdk_binding_provider();
                } else {
                    o->host = nullptr;
                    set_err(o.get(),
                            "dotnet runtime probes available but coreclr "
                            "init failed");
                }
            }
        }
#endif

        const auto table = make_vtable(o.get());
        const int32_t st =
            loader::sao_plugins_lifecycle_register_host_adapter(
                loader::engine_kind::csharp, &table);
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
sao_plugins_csmini_unregister_loader_adapter(
    csmini_adapter_owner_t owner) {
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
                loader::engine_kind::csharp);
            if (st != SAO_OK) {
                owner->active = true;
                return st;
            }
            owner->adapter_registered = false;
        }
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
        if (owner->host) {
            (void)csharp_host::cshost_unregister_sdk_binding_provider();
            st = csharp_host::sao_plugins_cshost_shutdown(owner->host);
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
sao_plugins_csmini_adapter_plugin_count(csmini_adapter_owner_t owner) {
    if (!owner)
        return 0;
    std::lock_guard<std::mutex> g(g_mu);
    return owner->plugins.size();
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_adapter_get_last_error(
    csmini_adapter_owner_t owner, void* loader_plugin_handle,
    char** out_utf8) {
    if (out_utf8)
        *out_utf8 = nullptr;
    if (!owner || !out_utf8)
        return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> g(g_mu);
    std::string e = owner->last_error;
    auto it = owner->last_errors.find(
        static_cast<plugin_handle_t>(loader_plugin_handle));
    if (it != owner->last_errors.end())
        e = it->second;
    *out_utf8 = dup_err(e);
    return *out_utf8 ? SAO_OK : SAO_ERR_HANDLE_INVALID;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_csmini_free_string(char* value) {
    std::free(value);
}

// script_ctx bridge hookup — forward to the TU that owns the ops struct.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_register_script_engine(void) {
    return csmini_register_script_engine();
}
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_unregister_script_engine(void) {
    return csmini_unregister_script_engine();
}

} // namespace sao::plugins::csmini
