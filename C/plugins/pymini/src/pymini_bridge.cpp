// pymini_bridge.cpp — script_ctx engine provider for `.py` load_local
// payloads.  Priority 10: the low-cost native subset engine handles helper
// scripts before any optional heavier python host gets a probe slot.
//
// Helpers share a separate interpreter, not the entry module's sys.modules.
#include "pymini_interp.h"

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
// Helper interpreters are separate from pymini_host entry interpreters.
struct helper_state : loader::context_runtime_resource {
    std::weak_ptr<loader::context_runtime_state> lifetime;
    std::mutex mutex;
    std::unique_ptr<interpreter> interp;
    PyRef ctx_obj;
    std::unordered_map<PyObj*, PyRef> values;
    bool initialized = false;

    void retire() noexcept override {
        if (!interp)
            return;
        pymini_drop_callbacks(*interp);
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

// script_value ↔ PyRef (standalone marshal; mirrors pymini_ctx.cpp's pair —
// keep in sync).  Local copies because this TU must not reach into the
// ctx-builder internals.
PyRef sv2py(interpreter& i, const script::script_value_ptr& v,
             const std::shared_ptr<helper_state>& owner);

script::script_value_ptr py2sv(const PyRef& v, const std::shared_ptr<helper_state>& owner) {
    if (!v)
        return script::script_value::null_value();
    switch (v->kind) {
    case py_kind::none_:
        return script::script_value::null_value();
    case py_kind::ellipsis_:
        return script::script_value::make_string("Ellipsis");
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
            items.push_back(py2sv(x, owner));
        return script::script_value::make_list(std::move(items));
    }
    case py_kind::dict: {
        std::vector<std::pair<std::string, script::script_value_ptr>> obj;
        for (const auto& [k, x] : as_dict(v)->items)
            if (auto* ks = as_str(k))
                obj.emplace_back(ks->v, py2sv(x, owner));
        return script::script_value::make_map(std::move(obj));
    }
    case py_kind::func:
    case py_kind::builtin:
    case py_kind::bound_method:
        owner->values.emplace(v.get(), v);
        return script::script_value::make_function(
            [key = v.get(), weak = std::weak_ptr<helper_state>(owner)](
                                const std::vector<script::script_value_ptr>& args,
                                script::script_value_ptr* out, std::string* error) -> int32_t {
                if (out == nullptr)
                    return SAO_ERR_INVALID_ARGUMENT;
                helper_invocation invocation(weak);
                if (!invocation) {
                    if (error) *error = "pymini helper is retired";
                    return SAO_ERR_HANDLE_INVALID;
                }
                const auto& state = invocation.state;
                auto* runtime = state->interp.get();
                try {
                    gil_guard guard(*runtime);
                    const PyRef callable = state->values.at(key);
                    py_args converted;
                    for (const auto& arg : args)
                        converted.pos.push_back(sv2py(*runtime, arg, state));
                    *out = py2sv(runtime->call(callable, converted, {}), state);
                    return SAO_OK;
                } catch (const py_error& e) {
                    if (error) *error = e.kind + ": " + e.message;
                } catch (const sig_raise& signal) {
                    if (error) *error = describe_sig(signal, runtime);
                } catch (const std::exception& e) {
                    if (error) *error = e.what();
                } catch (...) {
                    if (error) *error = "pymini local callback failed";
                }
                return SAO_ERR_OS_CALL_FAILED;
            });
    default:
        return script::script_value::null_value();
    }
}
PyRef sv2py(interpreter& i, const script::script_value_ptr& v,
             const std::shared_ptr<helper_state>& owner) {
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
            items.push_back(sv2py(i, x, owner));
        return py_list(std::move(items));
    }
    case k::map: {
        auto d = py_dict();
        for (const auto& [k2, x] : v->object)
            dict_set(as_dict(d), py_str(k2), sv2py(i, x, owner));
        return d;
    }
    case k::function:
        if (!v->call)
            return py_none();
        return py_builtin("sv_fn",
                          [v, weak = std::weak_ptr<helper_state>(owner)](
                              interpreter& i2, const py_args& a2) {
                              helper_invocation invocation(weak);
                              if (!invocation)
                                  i2.raise_exc("RuntimeError", "pymini helper is retired", {});
                              std::vector<script::script_value_ptr> args;
                              for (const auto& p : a2.pos)
                                  args.push_back(py2sv(p, invocation.state));
                              script::script_value_ptr out;
                              std::string err;
                              if (v->call(args, &out, &err) != 0)
                                  i2.raise_exc("RuntimeError",
                                               "cross-language call failed: " +
                                                   err,
                                               {});
                              return sv2py(i2, out, invocation.state);
                          });
    }
    return py_none();
}

// ── script_module facade over a loaded module object ──────────────────
struct pymini_script_module : script::script_module {
    std::string id;
    std::weak_ptr<helper_state> state;
    PyObj* module_key = nullptr;

    const std::string& module_id() const noexcept override { return id; }
    std::vector<std::string> member_names() const override {
        std::vector<std::string> out;
        helper_invocation invocation(state);
        if (!invocation)
            return out;
        auto* i = invocation.state->interp.get();
        gil_guard g(*i);
        const PyRef mod = invocation.state->values.at(module_key);
        auto* md = as_module(mod);
        if (!md)
            return out;
        for (const auto& [k, v] : as_dict(md->dict)->items)
            if (auto* ks = as_str(k))
                if (!ks->v.empty() && ks->v[0] != '_')
                    out.push_back(ks->v);
        return out;
    }
    int32_t get(const std::string& name, script::script_value_ptr* out,
                std::string* err) override {
        helper_invocation invocation(state);
        if (!invocation) {
            if (err) *err = "pymini helper is retired";
            return SAO_ERR_HANDLE_INVALID;
        }
        auto* i = invocation.state->interp.get();
        gil_guard g(*i);
        const PyRef mod = invocation.state->values.at(module_key);
        auto* md = as_module(mod);
        if (!md || !out)
            return -1;
        try {
            PyRef v = dict_get(as_dict(md->dict), py_str(name));
            if (!v) {
                if (err) *err = "no such member: " + name;
                return SAO_ERR_INVALID_ARGUMENT;
            }
            *out = py2sv(v, invocation.state);
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
            if (err) *err = "pymini helper is retired";
            return SAO_ERR_HANDLE_INVALID;
        }
        auto* i = invocation.state->interp.get();
        gil_guard g(*i);
        const PyRef mod = invocation.state->values.at(module_key);
        auto* md = as_module(mod);
        if (!md || !out)
            return -1;
        try {
            PyRef fn = dict_get(as_dict(md->dict), py_str(name));
            if (!fn) {
                if (err)
                    *err = "no such member: " + name;
                return -3;
            }
            py_args a;
            for (const auto& x : args)
                a.pos.push_back(sv2py(*i, x, invocation.state));
            PyRef r = i->call(fn, a, {});
            *out = py2sv(r, invocation.state);
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
            return SAO_ERR_UNKNOWN;
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
int32_t ops_probe(plugin_context_t* /*ctx*/, const wchar_t* abs_path,
                  std::string& note, void*) noexcept try {
    bool ok = false;
    const std::string src = read_all(abs_path, &ok);
    if (!ok) {
        note = "unreadable";
        return SAO_ERR_OS_CALL_FAILED;
    }
    return pymini_preflight_subset(src, &note) ? SAO_OK : SAO_ERR_NOT_IMPLEMENTED;
} catch (const py_error& error) {
    try { note = error.kind + ": " + error.message; }
    catch (...) { note.clear(); }
    return error.kind == "SyntaxError" || error.kind == "IndentationError" ||
           error.kind == "TabError" ? SAO_ERR_INVALID_ARGUMENT : SAO_ERR_OS_CALL_FAILED;
} catch (...) {
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
        s->interp = std::make_unique<interpreter>(interpreter::config{});
        interpreter& i = *s->interp;
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
        s->ctx_obj = pymini_make_ctx(i);
        dict_set(as_dict(i.builtins_dict), py_str("ctx"), s->ctx_obj);
        dict_set(as_dict(i.builtins_dict), py_str("act_ctx"), s->ctx_obj);
        s->initialized = true;
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
        sm->state = helper;
        sm->module_key = mod.get();
        s->values.emplace(mod.get(), mod);
        *out_module = std::move(sm);
        return 0;
    } catch (const py_error& e) {
        if (out_error)
            *out_error = e.kind + ": " + e.message;
        return SAO_ERR_OS_CALL_FAILED;
    } catch (const sig_raise& sig) {
        if (out_error)
            *out_error = describe_sig(sig, s->interp.get());
        return SAO_ERR_OS_CALL_FAILED;
    } catch (const std::exception& e) {
        if (out_error)
            *out_error = e.what();
        return SAO_ERR_OS_CALL_FAILED;
    }
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
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
