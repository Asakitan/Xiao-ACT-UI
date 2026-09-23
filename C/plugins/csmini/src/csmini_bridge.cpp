// csmini_bridge.cpp — script_ctx engine provider for `.cs` load_local
// payloads (mirrors pymini_bridge.cpp).
//
// Priority 10: the low-cost native C#-subset engine handles helper scripts
// before any optional heavier managed host gets a probe slot.
//
// Helper-module ownership: each helper exec creates ONE shared interpreter
// for the *calling* plugin context's loader-owned lifetime.
// The helper's `ctx` alias binds the same plugin context — a candy-helper
// calling `ctx.get_snapshot()` acts on behalf of the plugin that loaded it.
#include "csmini_host_internal.h"

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_context_lifetime_internal.h"
#include "sao/plugins/script_ctx/runtime_bridge.h"
#include "sao/plugins/script_ctx/ctx_surface.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>
#include <unordered_map>

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
    const auto close = [](void* value) { CloseHandle(value); };
    std::unique_ptr<void, decltype(close)> file(h, close);
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > MAXDWORD)
        return {};
    std::string out(static_cast<std::size_t>(sz.QuadPart), '\0');
    DWORD rd = 0;
    const bool r =
        sz.QuadPart <= 0 ||
        ReadFile(h, out.data(), static_cast<DWORD>(sz.QuadPart), &rd,
                 nullptr);
    if (!r || rd != static_cast<DWORD>(sz.QuadPart))
        return {};
    if (ok)
        *ok = true;
    return out;
}

// ── per-plugin interpreter for helper modules ────────────────────────
struct helper_state : loader::context_runtime_resource {
    std::weak_ptr<loader::context_runtime_state> lifetime;
    std::mutex mutex;
    std::unique_ptr<interpreter> interp;
    CsRef ctx_obj;
    std::unordered_map<CsObj*, CsRef> values;
    bool initialized = false;

    void retire() noexcept override {
        if (!interp)
            return;
        csmini_drop_callbacks(*interp);
        ctx_obj.reset();
        values.clear();
        interp.reset();
    }
    ~helper_state() override { retire(); }
};
const char k_helper_key = 0;

struct helper_invocation {
    std::shared_ptr<helper_state> state;
    loader::context_runtime_lease lease;

    explicit helper_invocation(const std::weak_ptr<helper_state>& owner)
        : state(owner.lock()), lease(state ? state->lifetime.lock() : nullptr) {}
    explicit operator bool() const noexcept { return static_cast<bool>(lease); }
};

std::wstring plugin_root_of(plugin_context_t* ctx) {
    const wchar_t* p = sao_plugins_ctx_path(ctx);
    return p ? std::wstring(p) : std::wstring();
}

// script_value ↔ CsRef marshal
CsRef sv2cs(const script::script_value_ptr& v, const std::shared_ptr<helper_state>& owner);

script::script_value_ptr cs2sv(const CsRef& v, const std::shared_ptr<helper_state>& owner) {
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
    case cs_kind::guid:
        return script::script_value::make_string(
            cs_guid_format(as_guid(v)->bytes));
    case cs_kind::array: {
        std::vector<script::script_value_ptr> items;
        for (const auto& x : as_array(v)->v)
            items.push_back(cs2sv(x, owner));
        return script::script_value::make_list(std::move(items));
    }
    case cs_kind::dict: {
        std::vector<std::pair<std::string, script::script_value_ptr>> obj;
        for (const auto& [k, x] : as_dict(v)->items)
            if (auto* ks = as_str(k))
                obj.emplace_back(ks->v, cs2sv(x, owner));
        return script::script_value::make_map(std::move(obj));
    }
    case cs_kind::class_: {
        owner->values.emplace(v.get(), v);
        std::vector<std::pair<std::string, script::script_value_ptr>> members;
        const auto* klass = as_class(v);
        for (const auto& [key, member] : as_dict(klass->attrs)->items) {
            const auto* name = as_str(key);
            if (name == nullptr || name->v.starts_with("@") || klass->inst_only.count(name->v))
                continue;
            members.emplace_back(name->v, cs2sv(member, owner));
        }
        return script::script_value::make_map(std::move(members));
    }
    case cs_kind::func:
    case cs_kind::builtin:
    case cs_kind::bound_method:
        owner->values.emplace(v.get(), v);
        if (auto* fn = as_func(v); fn && fn->owner_class) {
            const CsRef klass = owner->interp->find_owner_class(fn->owner_class);
            owner->values.emplace(klass.get(), klass);
        }
        return script::script_value::make_function(
            [key = v.get(), weak = std::weak_ptr<helper_state>(owner)](
                                const std::vector<script::script_value_ptr>& args,
                                script::script_value_ptr* out, std::string* error) -> int32_t {
                if (out == nullptr)
                    return SAO_ERR_INVALID_ARGUMENT;
                helper_invocation invocation(weak);
                if (!invocation) {
                    if (error) *error = "csmini helper is retired";
                    return SAO_ERR_HANDLE_INVALID;
                }
                const auto& state = invocation.state;
                auto* runtime = state->interp.get();
                try {
                    cs_guard guard(*runtime);
                    const CsRef callable = state->values.at(key);
                    cs_args converted;
                    for (const auto& arg : args)
                        converted.pos.push_back(sv2cs(arg, state));
                    *out = cs2sv(runtime->call(callable, converted, {}), state);
                    return SAO_OK;
                } catch (const cs_error& e) {
                    if (error) *error = e.kind + ": " + e.message;
                } catch (const sig_raise& signal) {
                    if (error) *error = csmini_describe_sig(signal, runtime);
                } catch (const std::exception& e) {
                    if (error) *error = e.what();
                } catch (...) {
                    if (error) *error = "csmini local callback failed";
                }
                return SAO_ERR_OS_CALL_FAILED;
            });
    default:
        return script::script_value::null_value();
    }
}
CsRef sv2cs(const script::script_value_ptr& v, const std::shared_ptr<helper_state>& owner) {
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
            as_array(a)->v.push_back(sv2cs(x, owner));
        return a;
    }
    case k::map: {
        auto d = cs_dict();
        for (const auto& [k2, x] : v->object)
            dict_set(as_dict(d), cs_str(k2), sv2cs(x, owner));
        return d;
    }
    case k::function:
        if (!v->call)
            return cs_null();
        return cs_builtin("sv_fn",
                          [v, weak = std::weak_ptr<helper_state>(owner)](
                              interpreter& i2, const cs_args& a2) {
                              helper_invocation invocation(weak);
                              if (!invocation)
                                  i2.raise_exc("RuntimeError", "csmini helper is retired", {});
                              std::vector<script::script_value_ptr> args;
                              for (const auto& p : a2.pos)
                                  args.push_back(cs2sv(p, invocation.state));
                              script::script_value_ptr out;
                              std::string err;
                              if (v->call(args, &out, &err) != 0)
                                  i2.raise_exc(
                                      "RuntimeError",
                                      "cross-language call failed: " + err);
                              return sv2cs(out, invocation.state);
                          });
    }
    return cs_null();
}

// ── script_module facade over a loaded module globals dict ───────────
struct csmini_script_module : script::script_module {
    std::string id;
    std::weak_ptr<helper_state> state;
    CsObj* module_key = nullptr;

    const std::string& module_id() const noexcept override { return id; }
    std::vector<std::string> member_names() const override {
        std::vector<std::string> out;
        helper_invocation invocation(state);
        if (!invocation)
            return out;
        auto* i = invocation.state->interp.get();
        cs_guard g(*i);
        const CsRef mod_dict = invocation.state->values.at(module_key);
        auto* md = as_dict(mod_dict);
        if (!md)
            return out;
        for (const auto& [k, v] : md->items)
            if (auto* ks = as_str(k))
                if (!ks->v.empty() && ks->v[0] != '_')
                    out.push_back(ks->v);
        return out;
    }
    int32_t get(const std::string& name, script::script_value_ptr* out,
                std::string* err) override {
        helper_invocation invocation(state);
        if (!invocation) {
            if (err) *err = "csmini helper is retired";
            return SAO_ERR_HANDLE_INVALID;
        }
        auto* i = invocation.state->interp.get();
        cs_guard g(*i);
        const CsRef mod_dict = invocation.state->values.at(module_key);
        auto* md = as_dict(mod_dict);
        if (!md || !out)
            return -1;
        try {
            CsRef v = dict_get(md, cs_str(name));
            if (!v) {
                if (err) *err = "no such member: " + name;
                return SAO_ERR_INVALID_ARGUMENT;
            }
            *out = cs2sv(v, invocation.state);
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
        helper_invocation invocation(state);
        if (!invocation) {
            if (err) *err = "csmini helper is retired";
            return SAO_ERR_HANDLE_INVALID;
        }
        auto* i = invocation.state->interp.get();
        cs_guard g(*i);
        const CsRef mod_dict = invocation.state->values.at(module_key);
        auto* md = as_dict(mod_dict);
        if (!md || !out)
            return -1;
        try {
            CsRef fn = dict_get(md, cs_str(name));
            if (!fn) {
                if (err)
                    *err = "no such member: " + name;
                return -3;
            }
            cs_args a;
            for (const auto& x : args)
                a.pos.push_back(sv2cs(x, invocation.state));
            CsRef r = i->call(fn, a, {});
            *out = cs2sv(r, invocation.state);
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

int32_t ops_probe(plugin_context_t* /*ctx*/, const wchar_t* abs_path,
                  std::string& note, void*) noexcept try {
    bool ok = false;
    const std::string src = read_all(abs_path, &ok);
    if (!ok) {
        note = "unreadable";
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (csmini_preflight_subset(src, &note))
        return SAO_OK;
    return SAO_ERR_NOT_IMPLEMENTED;
} catch (const cs_error& e) {
    note = e.file + ":" + std::to_string(e.pos.line) + ": " + e.kind + ": " + e.message;
    return SAO_ERR_INVALID_ARGUMENT;
} catch (...) {
    note = "csmini helper preflight failed";
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t ops_load_module(plugin_context_t* ctx, const wchar_t* abs_path,
                        const std::string& logical_name,
                        std::shared_ptr<script::script_module>* out_module,
                        std::string* out_error, void*) noexcept try {
    if (!ctx || !abs_path || !out_module)
        return -1;
    loader::context_runtime_lease invocation(ctx);
    if (!invocation)
        return SAO_ERR_HANDLE_INVALID;
    const std::string pid =
        sao_plugins_ctx_plugin_id(ctx)
            ? sao_plugins_ctx_plugin_id(ctx)
            : "";
    auto helper = std::static_pointer_cast<helper_state>(invocation.resource(&k_helper_key));
    if (!helper) {
        auto ns = std::make_shared<helper_state>();
        ns->lifetime = invocation.state();
        helper = std::static_pointer_cast<helper_state>(invocation.resource(&k_helper_key, ns));
    }
    if (!helper)
        return SAO_ERR_HANDLE_INVALID;
    std::unique_lock lock(helper->mutex);
    auto* s = helper.get();
    if (!s->interp) {
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
        s->interp = std::make_unique<interpreter>(std::move(icfg));
        interpreter& i = *s->interp;
        i.on_log = i.cfg.log_hook;
        try {
            cs_guard gg(i);
            csmini_install_builtins(i);
            csmini_install_stdlib(i);
            csmini_install_stdlib2(i);
            s->ctx_obj = csmini_make_ctx(i);
            dict_set(as_dict(i.globals), cs_str("ctx"), s->ctx_obj);
            s->initialized = true;
        } catch (...) {
            if (out_error)
                *out_error = "csmini interpreter init failed";
            return SAO_ERR_UNKNOWN;
        }
    }
    if (!s->initialized)
        return SAO_ERR_OS_CALL_FAILED;
    lock.unlock();
    bool read_ok = false;
    const std::string src = read_all(abs_path, &read_ok);
    if (!read_ok) {
        if (out_error)
            *out_error = "unreadable: " + narrow(abs_path);
        return SAO_ERR_OS_CALL_FAILED;
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
        sm->state = helper;
        sm->module_key = mod.get();
        s->values.emplace(mod.get(), mod);
        *out_module = std::move(sm);
        return 0;
    } catch (const cs_error& e) {
        if (out_error)
            *out_error = e.kind + ": " + e.message;
        return SAO_ERR_OS_CALL_FAILED;
    } catch (const sig_raise& sig) {
        if (out_error)
            *out_error = csmini_describe_sig(sig, s->interp.get());
        return SAO_ERR_OS_CALL_FAILED;
    } catch (const std::exception& e) {
        if (out_error)
            *out_error = e.what();
        return SAO_ERR_OS_CALL_FAILED;
    }
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
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
