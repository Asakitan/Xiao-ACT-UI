// pymini_host.cpp — per-plugin pymini interpreter registry.
//
// One interpreter per loaded legacy python plugin.  Owns the lifecycle:
//   load  → build config{ctx, plugin_id, plugin_root, module_dirs}
//        → register C++ stdlib factories + act_platform/sao_sdk shims
//        → inject `ctx` into the entry module globals before exec
//        → exec the entry .py source
//   hook  → interpreter::call0/call(payload) into globals
//   unload→ call on_unload → pymini_drop_callbacks → destroy interpreter
//
// The interpreter never escapes this file; the script_ctx bridge and the
// loader adapter go through the narrow API at the bottom.
#include "pymini_interp.h"

#include "sao/plugins/loader/plugin_context.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace sao::plugins::pymini {

namespace {

struct host_state {
    std::unique_ptr<interpreter> interp;
    PyRef ctx_obj;
    PyRef entry_globals;        // dict returned by exec_module_source
    std::wstring entry_abs;
    std::uint64_t entry_size = 0;
    std::int64_t entry_mtime = 0;
    bool unload_hook_fired = false;  // veto call already ran on_unload
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

// ── compat module shims ─────────────────────────────────────────────
// `import act_platform` / `from act_platform import plugins` resolves into a
// synthesized module exposing the legacy entry points that v1 plugins used:
//   act_platform.plugins.ctx                 → host ctx object
//   act_platform.plugins.PluginContext       → class-like, instantiating or
//                                              .get() returns the ctx object
//   act_platform.create_context(...)         → the ctx object
//   act_platform.ui.<method>(…)              → ctx.ui.<method>
// `import sao_sdk` maps to the same ctx object plus `ctx`/`ui`/`ctx_plugins`
// aliases several plugins import.
PyRef build_compat_module(interpreter& i, PyRef ctx_obj,
                          const char* which) {
    auto proxy = std::make_shared<PyModuleProxyObj>();
    proxy->name = which;
    auto d = py_dict();
    proxy->dict = d;
    auto* dd = as_dict(d);

    dict_set(dd, py_str("ctx"), ctx_obj);
    dict_set(dd, py_str("context"), ctx_obj);

    // create_context(*args, **kw) → ctx_obj
    dict_set(dd, py_str("create_context"),
             py_builtin("create_context",
                        [ctx_obj](interpreter&, const py_args&) {
                            return ctx_obj;
                        }));
    dict_set(dd, py_str("get_context"),
             py_builtin("get_context",
                        [ctx_obj](interpreter&, const py_args&) {
                            return ctx_obj;
                        }));

    // act_platform.plugins namespace: {ctx, PluginContext, Context}
    {
        auto pd = py_dict();
        auto* pdd = as_dict(pd);
        dict_set(pdd, py_str("ctx"), ctx_obj);
        dict_set(pdd, py_str("context"), ctx_obj);
        PyRef cls = py_builtin("PluginContext",
                               [ctx_obj](interpreter& i2, const py_args&) {
                                   return ctx_obj;      // instantiation → ctx
                               });
        dict_set(pdd, py_str("PluginContext"), cls);
        dict_set(pdd, py_str("Context"), cls);
        dict_set(pdd, py_str("create_context"),
                 py_builtin("create_context",
                            [ctx_obj](interpreter&, const py_args&) {
                                return ctx_obj;
                            }));
        dict_set(dd, py_str("plugins"), pd);
    }

    // act_platform.ui → ctx.ui (the spec-builder namespace object)
    {
        PyRef ui = i.getattr(ctx_obj, "ui");
        dict_set(dd, py_str("ui"), ui ? ui : py_none());
    }
    // act_platform.open → ctx.open (sandboxed)
    {
        PyRef op = i.getattr(ctx_obj, "open");
        dict_set(dd, py_str("open"), op ? op : py_none());
    }
    // sao_sdk also sees ctx/ui/engine/event_bus at top level
    {
        const char* aliases[] = {"ui", "engine", "event_bus", "mem",
                                 "plugin_id", "path", "web_path",
                                 "assets_path", "should_stop", "owner"};
        for (const char* n : aliases)
            dict_set(dd, py_str(n), i.getattr(ctx_obj, n));
    }
    return proxy;
}

// ── state helpers ───────────────────────────────────────────────────
host_state* find(const std::string& id) {
    auto it = g_hosts.find(id);
    return it == g_hosts.end() ? nullptr : it->second.get();
}

// known lifecycle hooks legacy modules define
const char* const k_hooks[] = {"on_load",  "on_enable", "on_disable",
                               "on_unload", "on_pause",  "on_resume",
                               nullptr};

// convert a runtime script exception signal into "TypeError: msg (file:line
// in fn)" for out_err — sig_raise is a py_signal, not a std::exception, so
// every boundary that only catches py_error/std::exception would otherwise
// leak it through the C ABI.
std::string describe_sig(const sig_raise& sig, interpreter* i) {
    if (const auto* e = as_exc(sig.exc)) {
        std::string msg;
        if (i != nullptr) {
            if (const auto* t = as_tuple(e->args);
                t != nullptr && !t->v.empty()) {
                try {
                    msg = py_to_str(*i, t->v[0]);
                } catch (...) {
                }
            }
        }
        std::string s =
            e->type_name + (msg.empty() ? "" : ": " + msg);
        if (!e->trace.empty())
            s += " [" + e->trace.back() + "]";
        return s;
    }
    return std::string("script raised a non-exception value");
}

// generic hook invoke — finds `hook` in module globals; calls it with no
// args or (payload_dict) when payload_json nonempty.  returns:
//    0 = hook ran ok
//    1 = hook absent (not an error)
//   <0 = hook raised
int call_hook_locked(host_state& s, const char* hook,
                     const char* payload_json, std::string* out_err,
                     PyRef* out_ret = nullptr) {
    if (out_ret)
        *out_ret = nullptr;
    if (!s.interp || !as_dict(s.entry_globals))
        return 1;
    interpreter& i = *s.interp;
    PyRef fn = dict_get(as_dict(s.entry_globals), py_str(hook));
    if (!fn)
        return 1;
    gil_guard g(i);
    try {
        py_args a;
        // arity-aware: hooks are called with the number of positional args
        // their signature declares. on_load(ctx) gets the ctx object; other
        // hooks that take a parameter get the payload ({} when none given);
        // zero-param hooks get no args.
        std::size_t positional = 0;
        if (auto* fo = as_func(fn)) {
            for (const auto& p : fo->params) {
                if (!p.varargs && !p.kwarg && !p.kwonly)
                    ++positional;
            }
        }
        if (std::strcmp(hook, "on_load") == 0) {
            // v1 signature on_load(ctx)
            if (positional >= 1)
                a.pos.push_back(s.ctx_obj);
        } else if (positional >= 1) {
            // {} not None — legacy bodies call payload.get(...) directly
            PyRef payload = py_dict();
            if (payload_json && *payload_json) {
                // decode payload through the plugin's own json module
                PyRef json_mod = i.find_loaded("json");
                if (!json_mod)
                    json_mod = i.import_dotted("json", nullptr);
                if (json_mod) {
                    PyRef loads = i.getattr(json_mod, "loads");
                    if (loads)
                        payload = i.call1(loads, py_str(payload_json), {});
                }
            }
            a.pos.push_back(payload);
        }
        PyRef r = i.call(fn, a, {});
        if (std::strcmp(hook, "on_unload") == 0)
            s.unload_hook_fired = true;
        if (out_ret)
            *out_ret = r;
        return 0;
    } catch (const py_error& e) {
        if (out_err)
            *out_err = e.kind + ": " + e.message;
        return -1;
    } catch (const sig_raise& sig) {
        if (out_err)
            *out_err = describe_sig(sig, s.interp.get());
        return -1;
    } catch (const std::exception& e) {
        if (out_err)
            *out_err = exception_tag(e) + " " + e.what();
        return -2;
    }
}

} // namespace

// ═══ public registry API (used by pymini_loader_adapter.cpp) ═════════
int pymini_host_load_plugin(loader::plugin_context_t* ctx,
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
    s->interp = std::make_unique<interpreter>(interpreter::config{});
    interpreter& i = *s->interp;
    i.cfg.ctx = ctx;
    i.cfg.plugin_id = plugin_id;
    i.cfg.plugin_root = plugin_root;
    i.cfg.module_dirs.push_back(plugin_root);
    // vendored / bundled python dirs — probing searches each for `x.py` or
    // `x/__init__.py`.
    const wchar_t* const sub[] = {L"", L"vendor", L"vendors", L"libs",
                                  L"lib",   L"site-packages",
                                  L"python"};
    for (const wchar_t* sdir : sub) {
        std::wstring d = plugin_root;
        if (*sdir) {
            d += L'\\';
            d += sdir;
        }
        i.cfg.module_dirs.push_back(d);
    }
    for (const auto& d : extra_dirs)
        i.cfg.module_dirs.push_back(d);
    i.cfg.log_hook = [ctx](const std::string& msg) {
        if (ctx)
            sao_plugins_ctx_log(ctx, msg.c_str());
    };
    i.on_log = i.cfg.log_hook;

    pymini_register_stdlib(i);

    // compat shims registered into sys_modules (and stdlib_factories) so
    // `import act_platform` / `import sao_sdk` resolve.
    s->ctx_obj = pymini_make_ctx(i);
    {
        PyRef ap = build_compat_module(i, s->ctx_obj, "act_platform");
        dict_set(as_dict(i.sys_modules), py_str("act_platform"), ap);
        PyRef sdk = build_compat_module(i, s->ctx_obj, "sao_sdk");
        dict_set(as_dict(i.sys_modules), py_str("sao_sdk"), sdk);
    }

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

    gil_guard gg(i);
    try {
        PyRef mod = i.exec_module_source(logical, narrow(abs), src);
        if (auto* m = as_module(mod)) {
            s->entry_globals = m->dict;
            // post-inject `ctx` + shortcut names into module globals
            if (as_dict(m->dict)) {
                dict_set(as_dict(m->dict), py_str("ctx"), s->ctx_obj);
                dict_set(as_dict(m->dict), py_str("act_ctx"),
                         s->ctx_obj);
            }
        } else {
            s->entry_globals = mod;
        }
    } catch (const py_error& e) {
        if (out_err)
            *out_err = e.kind + ": " + e.message + " (" + e.file + ":" +
                       std::to_string(e.pos.line) + ":" +
                       std::to_string(e.pos.col) + ")";
        g_hosts.erase(plugin_id);
        return -4;
    } catch (const sig_raise& sig) {
        if (out_err)
            *out_err = describe_sig(sig, &i);
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

int pymini_host_call_hook(const char* plugin_id, const char* hook,
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

// variant with return capture (adapter call_on_unload needs the truthiness)
int pymini_host_call_hook_ret(const char* plugin_id, const char* hook,
                              const char* payload_json, PyRef* out_ret,
                              std::string* out_err) {
    if (!plugin_id || !hook)
        return -1;
    std::lock_guard<std::mutex> g(g_hosts_mu);
    host_state* s = find(plugin_id);
    if (!s)
        return -2;
    return call_hook_locked(*s, hook, payload_json, out_err, out_ret);
}

// veto hook: runs the hook and computes truthiness on the owning interpreter
// so instance __bool__/__len__ are honored (bare py_truthy cannot see them).
int pymini_host_call_hook_allow_unload(const char* plugin_id, const char* hook,
                                       bool* allow_unload,
                                       std::string* out_err) {
    if (!plugin_id || !hook || !allow_unload)
        return -1;
    std::lock_guard<std::mutex> g(g_hosts_mu);
    host_state* s = find(plugin_id);
    if (!s)
        return -2;
    PyRef ret;
    const int rc = call_hook_locked(*s, hook, nullptr, out_err, &ret);
    if (rc < 0)
        return rc;
    if (rc == 1 || !ret) {          // hook absent → allow
        *allow_unload = true;
        return rc;
    }
    gil_guard g2(*s->interp);
    try {
        *allow_unload = s->interp->truthy(ret);
        return 0;
    } catch (const py_error& e) {
        if (out_err)
            *out_err = e.kind + ": " + e.message;
    } catch (const sig_raise& sig) {
        if (out_err)
            *out_err = describe_sig(sig, s->interp.get());
    } catch (const std::exception& e) {
        if (out_err)
            *out_err = exception_tag(e) + " " + e.what();
    }
    return -3;
}

int pymini_host_unload_plugin(const char* plugin_id,
                              std::string* out_err) {
    if (!plugin_id)
        return -1;
    std::lock_guard<std::mutex> g(g_hosts_mu);
    auto it = g_hosts.find(plugin_id);
    if (it == g_hosts.end())
        return -2;
    std::unique_ptr<host_state> s = std::move(it->second);
    g_hosts.erase(it);
    // fire on_unload best-effort (skipped when the veto call already ran
    // the hook — pyhost parity: one invocation per unload), then drop
    // callbacks, then destroy.
    if (!s->unload_hook_fired)
        (void)call_hook_locked(*s, "on_unload", nullptr, out_err);
    pymini_drop_callbacks(*s->interp);
    return 0;
}

// enumeration for diagnostics / adapter shutdown
std::vector<std::string> pymini_host_loaded_ids() {
    std::lock_guard<std::mutex> g(g_hosts_mu);
    std::vector<std::string> out;
    out.reserve(g_hosts.size());
    for (const auto& [k, v] : g_hosts)
        out.push_back(k);
    return out;
}

bool pymini_host_is_loaded(const char* plugin_id) {
    if (!plugin_id)
        return false;
    std::lock_guard<std::mutex> g(g_hosts_mu);
    return g_hosts.count(plugin_id) != 0;
}

// module cache probe — adapter uses this to decide whether to re-exec after
// an on-disk file changed (dev/live-fix loops).
int pymini_host_entry_changed(const char* plugin_id, bool* changed) {
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
        *changed = true;                 // missing counts as changed
        return 0;
    }
    *changed = size != s->entry_size || mtime != s->entry_mtime;
    return 0;
}

// Foreign-engine bridge support: expose the ctx object + entry globals to
// pymini_bridge.cpp without giving it interpreter ownership.
int pymini_host_ctx_object(const char* plugin_id, PyRef* out) {
    std::lock_guard<std::mutex> g(g_hosts_mu);
    host_state* s = find(plugin_id ? plugin_id : "");
    if (!s || !out)
        return -1;
    *out = s->ctx_obj;
    return 0;
}
int pymini_host_module_attr(const char* plugin_id, const char* name,
                            PyRef* out) {
    std::lock_guard<std::mutex> g(g_hosts_mu);
    host_state* s = find(plugin_id ? plugin_id : "");
    if (!s || !name || !out)
        return -1;
    if (!as_dict(s->entry_globals))
        return -2;
    *out = dict_get(as_dict(s->entry_globals), py_str(name));
    return *out ? 0 : 1;
}

} // namespace sao::plugins::pymini
