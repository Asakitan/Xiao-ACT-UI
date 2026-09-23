// csmini_calls.cpp — call machinery: builtins / user methods / bound
// methods / class instantiation (mirrors pymini_calls.cpp role).
//
// Frame model: each call pushes frame{scope=this+params, this_ref,
// class_ref, caller}; a depth guard catches StackOverflow.
#include "csmini_interp.h"

namespace sao::plugins::csmini {

// resolve an owner_class raw pointer back to the live CsRef — the class
// object owns its methods, so a reverse scan via globals is safe + rare.
// Classes register under both `name` and `ns.name`; when the class carries
// a namespace prefer the qualified key — same-name classes in different
// namespaces share the plain `name` slot (last registration wins) while
// each `ns.name` slot is unique.
CsRef interpreter::find_owner_class(CsClassObj* co) {
    if (!co)
        return {};
    if (auto* d = as_dict(globals)) {
        if (!co->ns.empty()) {
            CsRef v = dict_get(d, cs_str(co->ns + "." + co->name));
            if (v && v->kind == cs_kind::class_ && as_class(v) == co)
                return v;
        }
        for (const auto& [k, v] : d->items) {
            if (v && v->kind == cs_kind::class_ && as_class(v) == co)
                return v;
        }
    }
    return CsRef(co, [](CsObj*) {});   // borrowed (non-owning) fallback
}

void interpreter::bind_param_frame(frame& callee, const CsFuncObj* fn,
                                   const cs_args& args, src_pos pos) {
    if (args.size() > fn->params.size()) {
        raise_exc("ArgumentException",
                  fn->name + ": too many arguments (" +
                      std::to_string(args.size()) + " > " +
                      std::to_string(fn->params.size()) + ")",
                  pos);
    }
    std::size_t required = 0;
    for (const auto& p : fn->params)
        if (!p.has_default)
            ++required;
    if (args.size() < required) {
        raise_exc("ArgumentException",
                  fn->name + ": missing required argument(s) (" +
                      std::to_string(args.size()) + " < " +
                      std::to_string(required) + ")",
                  pos);
    }
    for (std::size_t k = 0; k < fn->params.size(); ++k) {
        const cs_param& p = fn->params[k];
        if (k < args.size()) {
            scope_set(callee, p.name, args.pos[k]);
            continue;
        }
        // default value: default_exprs is index-aligned with params (null for
        // required slots) — eval in caller frame so defaults see constants
        // and statics but not callee locals.
        CsRef dv = cs_null();
        if (k < fn->default_exprs.size() && fn->default_exprs[k])
            dv = eval(fn->default_exprs[k], *cur_frame);
        scope_set(callee, p.name, dv);
    }
}

// shared call-body runner: binds callee frame, execs fn body, returns value.
CsRef interpreter::invoke_func(CsFuncObj* fn, CsRef this_ref,
                               const cs_args& args, src_pos pos) {
    if (++call_depth > static_cast<uint32_t>(cfg.max_call_depth)) {
        --call_depth;
        raise_exc("StackOverflowException",
                  "call depth exceeded in " + fn->name, pos);
    }
    frame callee{};
    callee.scope = alloc_scope(fn->is_lambda && fn->closure_scope
                                   ? fn->closure_scope
                                   : global_scope_);
    callee.this_ref = this_ref ? std::move(this_ref) : fn->closure_this;
    callee.class_ref = fn->owner_class ? find_owner_class(fn->owner_class)
                                       : fn->closure_class;
    callee.fn_name = fn->name;
    callee.file = cur_frame ? cur_frame->file : "";
    callee.caller = cur_frame;
    callee.anchor = fn->anchor;
    bind_param_frame(callee, fn, args, pos);
    frame* prev = cur_frame;
    cur_frame = &callee;
    CsRef out;
    try {
        if (fn->expr_body) {
            exec(fn->expr_body.get(), callee);
        } else if (fn->body) {
            exec_body(*fn->body, callee);
        }
    } catch (const sig_return& r) {
        out = r.value;
    } catch (...) {
        cur_frame = prev;
        --call_depth;
        throw;
    }
    cur_frame = prev;
    --call_depth;
    CsRef result = out ? out : cs_null();
    if (fn->is_async) {
        const bool value_task = fn->return_type.rfind("ValueTask", 0) == 0 ||
                                fn->return_type.rfind(
                                    "System.Threading.Tasks.ValueTask", 0) == 0;
        const bool task = value_task ||
                          fn->return_type.rfind("Task", 0) == 0 ||
                          fn->return_type.rfind(
                              "System.Threading.Tasks.Task", 0) == 0;
        if (task)
            return csmini_make_task(std::move(result), value_task);
        return cs_null();
    }
    return result;
}

CsRef interpreter::call(const CsRef& borrowed_callable, const cs_args& args,
                        src_pos pos) {
    const CsRef callable = borrowed_callable;
    if (!callable) {
        raise_exc("NullReferenceException", "call on null", pos);
    }
    switch (callable->kind) {
    case cs_kind::builtin: {
        auto* b = as_builtin(callable);
        return b->fn(*this, args);
    }
    case cs_kind::func: {
        auto* fn = as_func(callable);
        return invoke_func(fn, {}, args, pos);
    }
    case cs_kind::bound_method: {
        auto* bm = static_cast<CsBoundMethodObj*>(callable.get());
        auto* fn = as_func(bm->fn);
        if (!fn)
            raise_exc("InvalidOperationException", "bound non-function", pos);
        return invoke_func(fn, bm->self, args, pos);
    }
    case cs_kind::class_:
        return instantiate(callable, args, pos);
    case cs_kind::native_obj: {
        auto* n = as_native(callable);
        if (n->payload) {
            if (auto* pb = as_builtin(n->payload))
                return pb->fn(*this, args);
        }
        raise_exc("InvalidOperationException",
                  "object '" + n->name + "' is not callable", pos);
    }
    default:
        raise_exc("InvalidOperationException",
                  std::string("type '") + cs_type_name(callable) +
                      "' is not callable",
                  pos);
    }
}

CsRef interpreter::call0(const CsRef& callable, src_pos pos) {
    cs_args a;
    return call(callable, a, pos);
}
CsRef interpreter::call1(const CsRef& callable, CsRef a, src_pos pos) {
    cs_args args;
    args.pos.push_back(std::move(a));
    return call(callable, args, pos);
}

CsRef interpreter::call_method(const CsRef& obj, const std::string& name,
                               const cs_args& args, src_pos pos) {
    bool found = false;
    CsRef m = getattr(obj, name, &found);
    if (!found || !m)
        raise_exc("MissingMemberException",
                  "no method '" + name + "' on " + cs_type_name(obj), pos);
    return call(m, args, pos);
}

bool interpreter::maybe_call_method(const CsRef& obj, const std::string& name,
                                    const cs_args& args, CsRef* out) {
    bool found = false;
    CsRef m = getattr(obj, name, &found);
    if (!found || !m)
        return false;
    CsRef r = call(m, args, {});
    if (out)
        *out = r;
    return true;
}

} // namespace sao::plugins::csmini
