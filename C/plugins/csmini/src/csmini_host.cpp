// csmini_host.cpp — per-plugin csmini interpreter registry (mirrors
// pymini_host.cpp).
//
// One interpreter per loaded csharp plugin (route csmini).  Lifecycle:
//   load  → config{ctx, plugin_id, plugin_root, module_dirs}
//        → builtins + stdlib + ctx injection
//        → compat facades (SaoAuto.Plugins.PluginContext / sao_sdk)
//        → exec entry .cs → locate plugin class (class named `Plugin` or
//          first class exposing a static `OnLoad`)
//   hook  → static method invoke on the plugin class; OnLoad(ctx) arity-
//          aware; others get payload-dict-or-nothing; OnUnload → bool veto
//   unload→ fire OnUnload once (unless the veto call already ran it)
//        → csmini_drop_callbacks → destroy interpreter
#include "csmini_host_internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <mutex>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace sao::plugins::csmini {

namespace {

struct host_state {
    std::unique_ptr<interpreter> interp;
    CsRef ctx_obj;
    CsRef entry_globals;        // dict returned by exec_module_source
    CsRef plugin_class;         // CsClassObj holding the lifecycle hooks
    std::wstring entry_abs;
    std::uint64_t entry_size = 0;
    std::int64_t entry_mtime = 0;
    bool unload_hook_fired = false;
};

std::mutex g_hosts_mu;
std::unordered_map<std::string, std::unique_ptr<host_state>> g_hosts;

// typeid(...) on a polymorphic type warns under /GR- (C4541); keep the
// concrete exception class name when RTTI is on and a stable tag otherwise.
std::string exception_tag(const std::exception& e) {
#if defined(_CPPRTTI)
    return std::string("[") + typeid(e).name() + "]";
#else
    (void)e;
    return "[std::exception]";
#endif
}

std::string narrow(const std::wstring& w) {
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
std::wstring widen(const std::string& u8) {
    if (u8.empty())
        return {};
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()),
                            nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()),
                        w.data(), n);
    return w;
}
std::string read_all(const std::wstring& path, bool* ok = nullptr) {
    if (ok)
        *ok = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string out(static_cast<std::size_t>(sz.QuadPart), '\0');
    DWORD rd = 0;
    const bool r =
        sz.QuadPart <= 0 ||
        ReadFile(h, out.data(), static_cast<DWORD>(sz.QuadPart), &rd, nullptr);
    CloseHandle(h);
    if (!r)
        return {};
    if (ok)
        *ok = true;
    return out;
}
bool file_info(const std::wstring& p, std::uint64_t* size,
               std::int64_t* mtime) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
        return false;
    if (size)
        *size = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) |
                d.nFileSizeLow;
    if (mtime)
        *mtime = (static_cast<std::int64_t>(d.ftLastWriteTime.dwHighDateTime)
                  << 32) |
                 d.ftLastWriteTime.dwLowDateTime;
    return true;
}

// ── compat facades ───────────────────────────────────────────────────
// C# plugins historically referenced `SaoAuto.Plugins.PluginContext` /
// `sao_sdk` for the ctx object — provide minimal native bags so those
// spellings resolve in the subset.
CsRef build_compat_facade(interpreter& i, CsRef ctx_obj, const char* name) {
    auto mod = cs_native(name, true);          // ci — C# naming variance
    auto* dd = as_dict(as_native(mod)->members);
    dict_set(dd, cs_str("ctx"), ctx_obj);
    dict_set(dd, cs_str("context"), ctx_obj);
    dict_set(dd, cs_str("Context"), ctx_obj);
    // SaoAuto.Plugins.{ctx,PluginContext}
    {
        auto pd = cs_native("SaoAuto.Plugins", true);
        auto* pdd = as_dict(as_native(pd)->members);
        dict_set(pdd, cs_str("ctx"), ctx_obj);
        dict_set(pdd, cs_str("context"), ctx_obj);
        dict_set(pdd, cs_str("PluginContext"), ctx_obj);
        dict_set(pdd, cs_str("get_context"),
                 cs_builtin("get_context",
                            [ctx_obj](interpreter&, const cs_args&) {
                                return ctx_obj;
                            }));
        dict_set(dd, cs_str("Plugins"), pd);
    }
    const char* aliases[] = {"ui",         "engine",    "event_bus",
                             "mem",        "plugin_id", "path",
                             "web_path",   "assets_path", "should_stop",
                             "owner"};
    for (const char* n : aliases) {
        bool f = false;
        dict_set(dd, cs_str(n), i.getattr(ctx_obj, n, &f));
    }
    return mod;
}

host_state* find(const std::string& id) {
    auto it = g_hosts.find(id);
    return it == g_hosts.end() ? nullptr : it->second.get();
}

// lifecycle hook spellings — loader names plus their C# PascalCase forms.
struct hook_alias {
    const char* loader_name;               // "on_load"
    const char* cs_name;                   // "OnLoad"
};
const hook_alias k_hooks[] = {
    {"on_load", "OnLoad"},       {"on_enable", "OnEnable"},
    {"on_disable", "OnDisable"}, {"on_unload", "OnUnload"},
    {"on_pause", "OnPause"},     {"on_resume", "OnResume"},
    {"on_tick", "OnTick"},
};
const char* cs_hook_name(const char* hook) {
    for (const auto& h : k_hooks)
        if (hook && std::strcmp(hook, h.loader_name) == 0)
            return h.cs_name;
    return hook;                            // already C#-shaped
}

} // namespace

std::string csmini_describe_sig(const sig_raise& sig, interpreter*) {
    if (const auto* e = as_exc(sig.exc)) {
        std::string s =
            e->type_name + (e->message.empty() ? "" : ": " + e->message);
        if (!e->trace.empty())
            s += " [" + e->trace.back() + "]";
        return s;
    }
    return std::string("script raised a non-exception value");
}

namespace {

// generic hook invoke — resolves `hook` as a static method on the plugin
// class (exact then C# spelling).  returns 0 ok / 1 absent / <0 raised.
int call_hook_locked(host_state& s, const char* hook,
                     const char* payload_json, std::string* out_err,
                     CsRef* out_ret = nullptr) {
    if (out_ret)
        *out_ret = nullptr;
    if (!s.interp || !s.plugin_class)
        return 1;
    interpreter& i = *s.interp;
    auto* co = as_class(s.plugin_class);
    if (!co)
        return 1;
    CsRef fn = dict_get(as_dict(co->attrs), cs_str(cs_hook_name(hook)));
    if (!fn || fn->kind != cs_kind::func)
        return 1;
    cs_guard g(i);
    try {
        cs_args a;
        // arity-aware: OnLoad(ctx) gets the ctx object; hooks with ≥1 param
        // get the payload dict ({} when no payload json was given); zero-
        // param hooks get nothing.
        std::size_t positional = as_func(fn)->params.size();
        if (std::strcmp(cs_hook_name(hook), "OnLoad") == 0 ||
            std::strcmp(cs_hook_name(hook), "on_load") == 0) {
            if (positional >= 1)
                a.pos.push_back(s.ctx_obj);
        } else if (positional >= 1) {
            // {} payload — parse json into a dict (subset json → dict)
            CsRef payload = cs_dict();
            if (payload_json && *payload_json) {
                try {
                    payload =
                        csmini_json_to_cs(nlohmann::json::parse(payload_json));
                } catch (...) {
                    payload = cs_dict();
                }
            }
            a.pos.push_back(payload);
        }
        CsRef r = i.call(fn, a, {});
        if (std::strcmp(hook, "on_unload") == 0 ||
            std::strcmp(cs_hook_name(hook), "OnUnload") == 0)
            s.unload_hook_fired = true;
        if (out_ret)
            *out_ret = r;
        return 0;
    } catch (const cs_error& e) {
        if (out_err)
            *out_err = e.kind + ": " + e.message;
        return -1;
    } catch (const sig_raise& sig) {
        if (out_err)
            *out_err = csmini_describe_sig(sig, &i);
        return -1;
    } catch (const std::exception& e) {
        if (out_err)
            *out_err = exception_tag(e) + " " + e.what();
        return -2;
    }
}

// locate the plugin class inside a module globals dict: prefer a class
// named "Plugin"; else the first class exposing a static OnLoad member.
CsRef find_plugin_class(CsRef globals) {
    auto* d = as_dict(globals);
    if (!d)
        return nullptr;
    if (CsRef v = dict_get(d, cs_str("Plugin"))) {
        if (as_class(v))
            return v;
    }
    for (const auto& [k, v] : d->items) {
        auto* co = as_class(v);
        if (!co)
            continue;
        if (CsRef m = dict_get(as_dict(co->attrs), cs_str("OnLoad"))) {
            if (auto* f = as_func(m); f && f->is_static)
                return v;
        }
    }
    return nullptr;
}

} // namespace

// ═══ public registry API ═════════════════════════════════════════════
int csmini_host_load_plugin(loader::plugin_context_t* ctx,
                            const char* plugin_id,
                            const wchar_t* plugin_root,
                            const wchar_t* entry_rel,
                            const std::vector<std::wstring>& extra_dirs,
                            std::string* out_err) {
    if (!plugin_id || !*plugin_id || !plugin_root || !entry_rel)
        return -1;
    std::lock_guard<std::mutex> g(g_hosts_mu);
    if (g_hosts.count(plugin_id))
        return -2;

    auto s = std::make_unique<host_state>();
    interpreter::config icfg{};
    icfg.ctx = ctx;
    icfg.plugin_id = plugin_id;
    icfg.plugin_root = plugin_root;
    icfg.module_dirs.push_back(plugin_root);
    for (const auto& d : extra_dirs)
        icfg.module_dirs.push_back(d);
    icfg.log_hook = [ctx](const std::string& msg) {
        if (ctx)
            sao_plugins_ctx_log(ctx, msg.c_str());
    };
    s->interp = std::make_unique<interpreter>(std::move(icfg));
    interpreter& i = *s->interp;
    i.on_log = i.cfg.log_hook;

    cs_guard gg(i);
    try {
        csmini_install_builtins(i);
        csmini_install_stdlib(i);
        csmini_install_stdlib2(i);
        s->ctx_obj = csmini_make_ctx(i);
        // compat facades resolvable by name in globals
        auto* gmd = as_dict(i.globals);
        dict_set(gmd, cs_str("ctx"), s->ctx_obj);
        dict_set(gmd, cs_str("PluginContext"), s->ctx_obj);
        dict_set(gmd, cs_str("SaoAuto"),
                 build_compat_facade(i, s->ctx_obj, "SaoAuto"));
        dict_set(gmd, cs_str("sao_sdk"),
                 build_compat_facade(i, s->ctx_obj, "sao_sdk"));
        dict_set(gmd, cs_str("act_platform"),
                 build_compat_facade(i, s->ctx_obj, "act_platform"));

        // entry source
        std::wstring abs = plugin_root;
        abs += L'\\';
        abs += entry_rel;
        for (auto& ch : abs)
            if (ch == L'/')
                ch = L'\\';
        s->entry_abs = abs;
        file_info(abs, &s->entry_size, &s->entry_mtime);
        bool read_ok = false;
        const std::string src = read_all(abs, &read_ok);
        if (!read_ok) {
            if (out_err)
                *out_err = "entry file unreadable: " + narrow(abs);
            return -3;
        }
        std::string logical = narrow(entry_rel);
        for (auto& ch : logical)
            if (ch == '\\' || ch == '/')
                ch = '.';
        while (!logical.empty() && logical.back() == '.')
            logical.pop_back();

        CsRef mod = i.exec_module_source(logical, narrow(abs), src);
        s->entry_globals = mod;
        s->plugin_class = find_plugin_class(s->entry_globals);
        if (!s->plugin_class) {
            if (out_err) {
                *out_err =
                    "no plugin class found (expected a class with static "
                    "OnLoad)";
            }
            return -4;
        }
    } catch (const cs_error& e) {
        if (out_err)
            *out_err = e.kind + ": " + e.message + " (" + e.file + ":" +
                       std::to_string(e.pos.line) + ":" +
                       std::to_string(e.pos.col) + ")";
        g_hosts.erase(plugin_id);
        return -4;
    } catch (const sig_raise& sig) {
        if (out_err)
            *out_err = csmini_describe_sig(sig, &i);
        g_hosts.erase(plugin_id);
        return -4;
    } catch (const std::exception& e) {
        if (out_err)
            *out_err = exception_tag(e) + " " + e.what();
        g_hosts.erase(plugin_id);
        return SAO_ERR_UNKNOWN;
    }
    g_hosts[plugin_id] = std::move(s);
    return 0;
}

int csmini_host_call_hook(const char* plugin_id, const char* hook,
                          const char* payload_json,
                          std::string* out_err) {
    if (!plugin_id || !hook)
        return -1;
    std::lock_guard<std::mutex> g(g_hosts_mu);
    host_state* s = find(plugin_id);
    if (!s)
        return -2;
    return call_hook_locked(*s, hook, payload_json, out_err);
}

int csmini_host_call_hook_ret(const char* plugin_id, const char* hook,
                              const char* payload_json, CsRef* out_ret,
                              std::string* out_err) {
    if (!plugin_id || !hook)
        return -1;
    std::lock_guard<std::mutex> g(g_hosts_mu);
    host_state* s = find(plugin_id);
    if (!s)
        return -2;
    return call_hook_locked(*s, hook, payload_json, out_err, out_ret);
}

int csmini_host_unload_plugin(const char* plugin_id,
                              std::string* out_err) {
    if (!plugin_id)
        return -1;
    std::lock_guard<std::mutex> g(g_hosts_mu);
    auto it = g_hosts.find(plugin_id);
    if (it == g_hosts.end())
        return -2;
    std::unique_ptr<host_state> s = std::move(it->second);
    g_hosts.erase(it);
    if (!s->unload_hook_fired)
        (void)call_hook_locked(*s, "on_unload", nullptr, out_err);
    csmini_drop_callbacks(*s->interp);
    return 0;
}

bool csmini_host_is_loaded(const char* plugin_id) {
    if (!plugin_id)
        return false;
    std::lock_guard<std::mutex> g(g_hosts_mu);
    return g_hosts.count(plugin_id) != 0;
}
std::vector<std::string> csmini_host_loaded_ids() {
    std::lock_guard<std::mutex> g(g_hosts_mu);
    std::vector<std::string> out;
    out.reserve(g_hosts.size());
    for (const auto& [k, v] : g_hosts)
        out.push_back(k);
    return out;
}
int csmini_host_entry_changed(const char* plugin_id, bool* changed) {
    if (!plugin_id || !changed)
        return -1;
    std::lock_guard<std::mutex> g(g_hosts_mu);
    host_state* s = find(plugin_id);
    if (!s) {
        *changed = false;
        return -2;
    }
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    if (!file_info(s->entry_abs, &size, &mtime)) {
        *changed = true;
        return 0;
    }
    *changed = size != s->entry_size || mtime != s->entry_mtime;
    return 0;
}
int csmini_host_ctx_object(const char* plugin_id, CsRef* out) {
    std::lock_guard<std::mutex> g(g_hosts_mu);
    host_state* s = find(plugin_id ? plugin_id : "");
    if (!s || !out)
        return -1;
    *out = s->ctx_obj;
    return 0;
}
int csmini_host_module_attr(const char* plugin_id, const char* name,
                            CsRef* out) {
    std::lock_guard<std::mutex> g(g_hosts_mu);
    host_state* s = find(plugin_id ? plugin_id : "");
    if (!s || !name || !out)
        return -1;
    if (!as_dict(s->entry_globals))
        return -2;
    *out = dict_get(as_dict(s->entry_globals), cs_str(name));
    return *out ? 0 : 1;
}

} // namespace sao::plugins::csmini
