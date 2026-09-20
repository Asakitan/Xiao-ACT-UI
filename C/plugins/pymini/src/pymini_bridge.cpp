// pymini_bridge.cpp — script_ctx engine provider for `.py` load_local
// payloads.  Priority 10: the low-cost native subset engine handles helper
// scripts before any optional heavier python host gets a probe slot.
//
// Helper-module ownership: each helper exec creates ONE shared interpreter
// for the *calling* plugin session (keyed by `sao_plugins_ctx_plugin_id`),
// then executes the file inside it.  The helper's `ctx` alias binds the same
// plugin context, so a candy-helper calling `ctx.get_snapshot()` acts on
// behalf of the plugin that loaded it — matching the v1 semantic exactly.
// Helpers therefore share module space (`sys.modules`) with the plugin entry
// module — identical to CPython's intra-plugin import behaviour.
#include "pymini_interp.h"

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/script_ctx/runtime_bridge.h"
#include "sao/plugins/script_ctx/ctx_surface.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace sao::plugins::pymini {
namespace script = sao::plugins::script_ctx;
using loader::plugin_context_t;

namespace {

// defined below — converts a runtime script exception signal into a
// "TypeError: msg" string for out_err/out_error.
std::string describe_sig(const sig_raise& sig, interpreter* i);

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
        ReadFile(h, out.data(), static_cast<DWORD>(sz.QuadPart), &rd,
                 nullptr);
    CloseHandle(h);
    if (!r)
        return {};
    if (ok)
        *ok = true;
    return out;
}

// ── per-plugin interpreter for helper modules ────────────────────────
// Separate from pymini_host's entry-module interpreters only by key
// namespace — a helper interpreter is created lazily on first
// ctx.load_local("x.py") for plugins whose main module never went through
// pymini (e.g. lua plugin loading a python helper).
struct helper_state {
    std::unique_ptr<interpreter> interp;
    PyRef ctx_obj;
    std::unordered_set<std::string> modules;
};
std::mutex g_helper_mu;
std::unordered_map<std::string, std::unique_ptr<helper_state>> g_helpers;

std::wstring plugin_root_of(plugin_context_t* ctx) {
    const wchar_t* p = sao_plugins_ctx_path(ctx);
    return p ? std::wstring(p) : std::wstring();
}

// script_value ↔ PyRef (standalone marshal; mirrors pymini_ctx.cpp's pair —
// keep in sync).  Local copies because this TU must not reach into the
// ctx-builder internals.
script::script_value_ptr py2sv(const PyRef& v) {
    if (!v)
        return script::script_value::null_value();
    switch (v->kind) {
    case py_kind::none_:
        return script::script_value::null_value();
    case py_kind::boolean:
        return script::script_value::make_boolean(as_bool(v)->v);
    case py_kind::integer:
        return script::script_value::make_integer(as_int(v)->v);
    case py_kind::number:
        return script::script_value::make_number(as_float(v)->v);
    case py_kind::string:
        return script::script_value::make_string(as_str(v)->v);
    case py_kind::bytes_:
        return script::script_value::make_bytes(as_bytes(v)->v);
    case py_kind::list:
    case py_kind::tuple_: {
        std::vector<script::script_value_ptr> items;
        for (const auto& x :
             v->kind == py_kind::list ? as_list(v)->v : as_tuple(v)->v)
            items.push_back(py2sv(x));
        return script::script_value::make_list(std::move(items));
    }
    case py_kind::dict: {
        std::vector<std::pair<std::string, script::script_value_ptr>> obj;
        for (const auto& [k, x] : as_dict(v)->items)
            if (auto* ks = as_str(k))
                obj.emplace_back(ks->v, py2sv(x));
        return script::script_value::make_map(std::move(obj));
    }
    default:
        return script::script_value::null_value();
    }
}
PyRef sv2py(interpreter& i, const script::script_value_ptr& v) {
    (void)i;
    if (!v)
        return py_none();
    using k = script::script_value::kind;
    switch (v->k) {
    case k::null:
        return py_none();
    case k::boolean:
        return py_bool(v->boolean);
    case k::integer:
        return py_int(v->integer);
    case k::number:
        return py_float(v->number);
    case k::string:
        return py_str(v->text);
    case k::bytes:
        return py_bytes(v->text);
    case k::list: {
        std::vector<PyRef> items;
        for (const auto& x : v->items)
            items.push_back(sv2py(i, x));
        return py_list(std::move(items));
    }
    case k::map: {
        auto d = py_dict();
        for (const auto& [k2, x] : v->object)
            dict_set(as_dict(d), py_str(k2), sv2py(i, x));
        return d;
    }
    case k::function:
        if (!v->call)
            return py_none();
        return py_builtin("sv_fn",
                          [v](interpreter& i2, const py_args& a2) {
                              std::vector<script::script_value_ptr> args;
                              for (const auto& p : a2.pos)
                                  args.push_back(py2sv(p));
                              script::script_value_ptr out;
                              std::string err;
                              if (v->call(args, &out, &err) != 0)
                                  i2.raise_exc("RuntimeError",
                                               "cross-language call failed: " +
                                                   err,
                                               {});
                              return sv2py(i2, out);
                          });
    }
    return py_none();
}

// ── script_module facade over a loaded module object ──────────────────
struct pymini_script_module : script::script_module {
    std::string id;
    interpreter* i;
    PyRef mod;

    const std::string& module_id() const noexcept override { return id; }
    std::vector<std::string> member_names() const override {
        std::vector<std::string> out;
        auto* md = as_module(mod);
        if (!md)
            return out;
        gil_guard g(*i);
        for (const auto& [k, v] : as_dict(md->dict)->items)
            if (auto* ks = as_str(k))
                if (!ks->v.empty() && ks->v[0] != '_')
                    out.push_back(ks->v);
        return out;
    }
    int32_t get(const std::string& name, script::script_value_ptr* out,
                std::string* err) override {
        auto* md = as_module(mod);
        if (!md || !out)
            return -1;
        gil_guard g(*i);
        try {
            PyRef v = dict_get(as_dict(md->dict), py_str(name));
            *out = py2sv(v);
            return 0;
        } catch (const std::exception& e) {
            if (err)
                *err = e.what();
            return -2;
        }
    }
    int32_t call(const std::string& name,
                 const std::vector<script::script_value_ptr>& args,
                 script::script_value_ptr* out,
                 std::string* err) override {
        auto* md = as_module(mod);
        if (!md || !out)
            return -1;
        gil_guard g(*i);
        try {
            PyRef fn = dict_get(as_dict(md->dict), py_str(name));
            if (!fn) {
                if (err)
                    *err = "no such member: " + name;
                return -3;
            }
            py_args a;
            for (const auto& x : args)
                a.pos.push_back(sv2py(*i, x));
            PyRef r = i->call(fn, a, {});
            *out = py2sv(r);
            return 0;
        } catch (const py_error& e) {
            if (err)
                *err = e.kind + ": " + e.message;
            return -4;
        } catch (const sig_raise& sig) {
            if (err)
                *err = describe_sig(sig, i);
            return -4;
        } catch (const std::exception& e) {
            if (err)
                *err = e.what();
            return -5;
        }
    }
};

// convert a runtime script exception signal into "TypeError: msg (file:line
// in fn)" — sig_raise is a py_signal, not a std::exception, so boundaries
// catching only py_error/std::exception would leak it through the C ABI.
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
        std::string s = e->type_name + (msg.empty() ? "" : ": " + msg);
        if (!e->trace.empty())
            s += " [" + e->trace.back() + "]";
        return s;
    }
    return std::string("script raised a non-exception value");
}

// probe delegates to the shared subset preflight — declines so a heavier
// python provider may take over (note carries the feature name).
bool ops_probe(plugin_context_t* /*ctx*/, const wchar_t* abs_path,
               std::string& note, void*) noexcept {
    bool ok = false;
    const std::string src = read_all(abs_path, &ok);
    if (!ok) {
        note = "unreadable";
        return false;
    }
    return pymini_preflight_subset(src, &note);
}

int32_t ops_load_module(plugin_context_t* ctx, const wchar_t* abs_path,
                        const std::string& logical_name,
                        std::shared_ptr<script::script_module>* out_module,
                        std::string* out_error, void*) noexcept {
    if (!ctx || !abs_path || !out_module)
        return -1;
    const std::string pid =
        sao_plugins_ctx_plugin_id(ctx)
            ? sao_plugins_ctx_plugin_id(ctx)
            : "";
    std::lock_guard<std::mutex> g(g_helper_mu);
    helper_state* s = nullptr;
    auto it = g_helpers.find(pid);
    if (it != g_helpers.end() && it->second->interp->cfg.ctx != ctx) {
        // A reload under the same plugin_id means the cached interpreter is
        // still bound to the FREED plugin_context_t (cfg.ctx, ctx_obj, and
        // the ctx-capturing log_hook). Reusing it is a use-after-free —
        // erase and rebuild against the live context.
        g_helpers.erase(it);
        it = g_helpers.end();
    }
    if (it == g_helpers.end()) {
        auto ns = std::make_unique<helper_state>();
        ns->interp = std::make_unique<interpreter>(interpreter::config{});
        interpreter& i = *ns->interp;
        i.cfg.ctx = ctx;
        i.cfg.plugin_id = pid;
        i.cfg.plugin_root = plugin_root_of(ctx);
        i.cfg.module_dirs.push_back(i.cfg.plugin_root);
        const wchar_t* const sub[] = {L"vendor", L"libs", L"lib",
                                      L"site-packages", L"python"};
        for (const wchar_t* sdir : sub)
            i.cfg.module_dirs.push_back(i.cfg.plugin_root + L'\\' + sdir);
        i.cfg.log_hook = [ctx](const std::string& msg) {
            sao_plugins_ctx_log(ctx, msg.c_str());
        };
        i.on_log = i.cfg.log_hook;
        pymini_register_stdlib(i);
        ns->ctx_obj = pymini_make_ctx(i);
        s = ns.get();
        g_helpers.emplace(pid, std::move(ns));
    } else {
        s = it->second.get();
    }
    bool read_ok = false;
    const std::string src = read_all(abs_path, &read_ok);
    if (!read_ok) {
        if (out_error)
            *out_error = "unreadable: " + narrow(abs_path);
        return -2;
    }
    try {
        gil_guard gg(*s->interp);
        PyRef mod =
            s->interp->exec_module_source(logical_name, narrow(abs_path),
                                          src);
        if (auto* m = as_module(mod))
            if (as_dict(m->dict)) {
                dict_set(as_dict(m->dict), py_str("ctx"), s->ctx_obj);
                dict_set(as_dict(m->dict), py_str("act_ctx"), s->ctx_obj);
            }
        auto sm = std::make_shared<pymini_script_module>();
        sm->id = logical_name;
        sm->i = s->interp.get();
        sm->mod = mod;
        s->modules.insert(logical_name);
        *out_module = std::move(sm);
        return 0;
    } catch (const py_error& e) {
        if (out_error)
            *out_error = e.kind + ": " + e.message;
        return -3;
    } catch (const sig_raise& sig) {
        if (out_error)
            *out_error = describe_sig(sig, s->interp.get());
        return -3;
    } catch (const std::exception& e) {
        if (out_error)
            *out_error = e.what();
        return -4;
    }
}

const char* const k_exts[] = {"py", nullptr};

script::script_engine_ops g_ops = {
    /*engine_name_utf8=*/"pymini",
    /*priority=*/10,
    /*extensions_utf8=*/k_exts,
    /*probe=*/ops_probe,
    /*load_module=*/ops_load_module,
    /*user_data=*/nullptr,
};

} // namespace

// Public bridge entry — called once by plugins_provider on startup.
int pymini_register_script_engine() noexcept {
    return script::runtime_bridge_register(&g_ops);
}
int pymini_unregister_script_engine() noexcept {
    return script::runtime_bridge_unregister(&g_ops);
}

} // namespace sao::plugins::pymini
