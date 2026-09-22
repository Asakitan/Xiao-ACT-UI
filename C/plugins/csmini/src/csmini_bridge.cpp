// csmini_bridge.cpp — script_ctx engine provider for `.cs` load_local
// payloads (mirrors pymini_bridge.cpp).
//
// Priority 10: the low-cost native C#-subset engine handles helper scripts
// before any optional heavier managed host gets a probe slot.
//
// Helper-module ownership: each helper exec creates ONE shared interpreter
// for the *calling* plugin session (keyed by `sao_plugins_ctx_plugin_id`).
// The helper's `ctx` alias binds the same plugin context — a candy-helper
// calling `ctx.get_snapshot()` acts on behalf of the plugin that loaded it.
#include "csmini_host_internal.h"

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

namespace sao::plugins::csmini {
namespace script = sao::plugins::script_ctx;
using loader::plugin_context_t;

namespace {

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
struct helper_state {
    std::unique_ptr<interpreter> interp;
    CsRef ctx_obj;
    std::unordered_set<std::string> modules;
};
std::mutex g_helper_mu;
std::unordered_map<std::string, std::unique_ptr<helper_state>> g_helpers;

std::wstring plugin_root_of(plugin_context_t* ctx) {
    const wchar_t* p = sao_plugins_ctx_path(ctx);
    return p ? std::wstring(p) : std::wstring();
}

// script_value ↔ CsRef marshal
script::script_value_ptr cs2sv(const CsRef& v) {
    if (!v)
        return script::script_value::null_value();
    switch (v->kind) {
    case cs_kind::null_:
        return script::script_value::null_value();
    case cs_kind::boolean:
        return script::script_value::make_boolean(as_bool(v)->v);
    case cs_kind::integer:
        return script::script_value::make_integer(as_int(v)->v);
    case cs_kind::number:
        return script::script_value::make_number(as_float(v)->v);
    case cs_kind::char_:
        return script::script_value::make_string(
            std::string(1, static_cast<char>(as_char(v)->v)));
    case cs_kind::string:
        return script::script_value::make_string(as_str(v)->v);
    case cs_kind::array: {
        std::vector<script::script_value_ptr> items;
        for (const auto& x : as_array(v)->v)
            items.push_back(cs2sv(x));
        return script::script_value::make_list(std::move(items));
    }
    case cs_kind::dict: {
        std::vector<std::pair<std::string, script::script_value_ptr>> obj;
        for (const auto& [k, x] : as_dict(v)->items)
            if (auto* ks = as_str(k))
                obj.emplace_back(ks->v, cs2sv(x));
        return script::script_value::make_map(std::move(obj));
    }
    default:
        return script::script_value::null_value();
    }
}
CsRef sv2cs(const script::script_value_ptr& v) {
    if (!v)
        return cs_null();
    using k = script::script_value::kind;
    switch (v->k) {
    case k::null:
        return cs_null();
    case k::boolean:
        return cs_bool(v->boolean);
    case k::integer:
        return cs_int(v->integer);
    case k::number:
        return cs_float(v->number);
    case k::string:
        return cs_str(v->text);
    case k::list: {
        auto a = cs_array();
        for (const auto& x : v->items)
            as_array(a)->v.push_back(sv2cs(x));
        return a;
    }
    case k::map: {
        auto d = cs_dict();
        for (const auto& [k2, x] : v->object)
            dict_set(as_dict(d), cs_str(k2), sv2cs(x));
        return d;
    }
    case k::function:
        if (!v->call)
            return cs_null();
        return cs_builtin("sv_fn",
                          [v](interpreter& i2, const cs_args& a2) {
                              std::vector<script::script_value_ptr> args;
                              for (const auto& p : a2.pos)
                                  args.push_back(cs2sv(p));
                              script::script_value_ptr out;
                              std::string err;
                              if (v->call(args, &out, &err) != 0)
                                  i2.raise_exc(
                                      "RuntimeError",
                                      "cross-language call failed: " + err);
                              return sv2cs(out);
                          });
    }
    return cs_null();
}

// ── script_module facade over a loaded module globals dict ───────────
struct csmini_script_module : script::script_module {
    std::string id;
    interpreter* i;
    CsRef mod_dict;

    const std::string& module_id() const noexcept override { return id; }
    std::vector<std::string> member_names() const override {
        std::vector<std::string> out;
        auto* md = as_dict(mod_dict);
        if (!md)
            return out;
        cs_guard g(*i);
        for (const auto& [k, v] : md->items)
            if (auto* ks = as_str(k))
                if (!ks->v.empty() && ks->v[0] != '_')
                    out.push_back(ks->v);
        return out;
    }
    int32_t get(const std::string& name, script::script_value_ptr* out,
                std::string* err) override {
        auto* md = as_dict(mod_dict);
        if (!md || !out)
            return -1;
        cs_guard g(*i);
        try {
            CsRef v = dict_get(md, cs_str(name));
            *out = cs2sv(v);
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
        auto* md = as_dict(mod_dict);
        if (!md || !out)
            return -1;
        cs_guard g(*i);
        try {
            CsRef fn = dict_get(md, cs_str(name));
            if (!fn) {
                if (err)
                    *err = "no such member: " + name;
                return -3;
            }
            cs_args a;
            for (const auto& x : args)
                a.pos.push_back(sv2cs(x));
            CsRef r = i->call(fn, a, {});
            *out = cs2sv(r);
            return 0;
        } catch (const cs_error& e) {
            if (err)
                *err = e.kind + ": " + e.message;
            return -4;
        } catch (const sig_raise& sig) {
            if (err)
                *err = csmini_describe_sig(sig, i);
            return -4;
        } catch (const std::exception& e) {
            if (err)
                *err = e.what();
            return SAO_ERR_UNKNOWN;
        }
    }
};

bool ops_probe(plugin_context_t* /*ctx*/, const wchar_t* abs_path,
               std::string& note, void*) noexcept {
    bool ok = false;
    const std::string src = read_all(abs_path, &ok);
    if (!ok) {
        note = "unreadable";
        return false;
    }
    return csmini_preflight_subset(src, &note);
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
        // Same plugin_id reloaded → the cached interpreter still references
        // the FREED plugin_context_t via cfg.ctx and the ctx-capturing
        // log_hook lambda. Erase and rebuild — reuse would be a UAF.
        g_helpers.erase(it);
        it = g_helpers.end();
    }
    if (it == g_helpers.end()) {
        auto ns = std::make_unique<helper_state>();
        interpreter::config icfg{};
        icfg.ctx = ctx;
        icfg.plugin_id = pid;
        icfg.plugin_root = plugin_root_of(ctx);
        icfg.module_dirs.push_back(icfg.plugin_root);
        for (const wchar_t* sdir : {L"vendor", L"libs", L"lib", L"dotnet"})
            icfg.module_dirs.push_back(icfg.plugin_root + L'\\' + sdir);
        icfg.log_hook = [ctx](const std::string& msg) {
            sao_plugins_ctx_log(ctx, msg.c_str());
        };
        ns->interp = std::make_unique<interpreter>(std::move(icfg));
        interpreter& i = *ns->interp;
        i.on_log = i.cfg.log_hook;
        try {
            cs_guard gg(i);
            csmini_install_builtins(i);
            csmini_install_stdlib(i);
            csmini_install_stdlib2(i);
            ns->ctx_obj = csmini_make_ctx(i);
            dict_set(as_dict(i.globals), cs_str("ctx"), ns->ctx_obj);
        } catch (...) {
            if (out_error)
                *out_error = "csmini interpreter init failed";
            return SAO_ERR_UNKNOWN;
        }
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
        cs_guard gg(*s->interp);
        CsRef mod =
            s->interp->exec_module_source(logical_name, narrow(abs_path),
                                          src);
        if (as_dict(mod))
            dict_set(as_dict(mod), cs_str("ctx"), s->ctx_obj);
        auto sm = std::make_shared<csmini_script_module>();
        sm->id = logical_name;
        sm->i = s->interp.get();
        sm->mod_dict = mod;
        s->modules.insert(logical_name);
        *out_module = std::move(sm);
        return 0;
    } catch (const cs_error& e) {
        if (out_error)
            *out_error = e.kind + ": " + e.message;
        return -3;
    } catch (const sig_raise& sig) {
        if (out_error)
            *out_error = csmini_describe_sig(sig, s->interp.get());
        return -3;
    } catch (const std::exception& e) {
        if (out_error)
            *out_error = e.what();
        return -4;
    }
}

const char* const k_exts[] = {"cs", nullptr};

script::script_engine_ops g_ops = {
    /*engine_name_utf8=*/"csmini",
    /*priority=*/10,
    /*extensions_utf8=*/k_exts,
    /*probe=*/ops_probe,
    /*load_module=*/ops_load_module,
    /*user_data=*/nullptr,
};

} // namespace

int csmini_register_script_engine() noexcept {
    return script::runtime_bridge_register(&g_ops);
}
int csmini_unregister_script_engine() noexcept {
    return script::runtime_bridge_unregister(&g_ops);
}

} // namespace sao::plugins::csmini
