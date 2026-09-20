// pymini_calls.cpp — call protocol, parameter binding, attribute resolution.
#include "pymini_interp.h"

namespace sao::plugins::pymini {

// ── call ──────────────────────────────────────────────────────────────────
PyRef interpreter::call(const PyRef& callable, const py_args& args,
                        src_pos pos) {
    gil_guard g(*this);
    if (callable == nullptr)
        raise_exc("TypeError", "NoneType object is not callable", pos);
    switch (callable->kind) {
    case py_kind::builtin: {
        const auto* b = as_builtin(callable);
        return b->fn(*this, args);
    }
    case py_kind::bound_method: {
        const auto* bm = static_cast<PyBoundMethodObj*>(callable.get());
        py_args a2;
        a2.pos.reserve(args.pos.size() + 1);
        a2.pos.push_back(bm->self);
        for (const auto& p : args.pos)
            a2.pos.push_back(p);
        a2.kw = args.kw;
        return call(bm->fn, a2, pos);
    }
    case py_kind::func: {
        const auto* fn = as_func(callable);
        frame cf;
        cf.locals = py_dict();
        cf.globals = fn->globals_dict;
        cf.captured = fn->closure;
        cf.fn_name = fn->name;
        cf.file = cur_frame ? cur_frame->file : std::string("<module>");
        if (cf.file.empty())
            cf.file = "<module>";
        cf.caller = cur_frame;
        collect_scope_decls(fn->body, cf);
        bind_param_frame(cf, fn, args, pos);
        frame* saved = cur_frame;
        cur_frame = &cf;
        struct fr_guard {
            interpreter* i;
            frame* saved;
            ~fr_guard() { i->cur_frame = saved; }
        } fg{this, saved};
        try {
            exec_body(fn->body, cf);
        } catch (const sig_return& r) {
            return r.value;
        }
        return py_none();
    }
    case py_kind::staticmethod_: {
        return call(static_cast<PyStaticObj*>(callable.get())->fn, args, pos);
    }
    case py_kind::classmethod_: {
        // direct call on classmethod obj → bind cls if determinable
        auto* cm = static_cast<PyClassMethodObj*>(callable.get());
        return call(cm->fn, args, pos);
    }
    case py_kind::class_: {
        auto* cls = as_class(callable);
        // builtin/script exception class → PyExcObj with concrete name
        for (const auto* mc : cls->mro) {
            if (auto* ad = as_dict(mc->attrs)) {
                if (dict_get(ad, py_str("__call_exc__"))) {
                    PyRef e = make_exc(cls->name, "");
                    as_exc(e)->args = py_tuple(args.pos);
                    return e;
                }
                break;   // marker check only on first mro entry w/ attrs
            }
        }
        // instantiate: create instance, run __init__ if present
        auto inst = std::make_shared<PyInstanceObj>(callable, py_dict());
        if (PyRef init = getattr(inst, "__init__")) {
            call(init, args, pos);
        } else if (!args.pos.empty() || !args.kw.empty()) {
            raise_exc("TypeError",
                      cls->name + "() takes no arguments", pos);
        }
        return inst;
    }
    case py_kind::property_: {
        raise_exc("TypeError", "property object is not callable", pos);
    }
    case py_kind::instance: {
        if (PyRef c = getattr(callable, "__call__"))
            return call(c, args, pos);
        break;
    }
    case py_kind::module_proxy:
    case py_kind::module:
        raise_exc("TypeError", "module object is not callable", pos);
    default:
        break;
    }
    raise_exc("TypeError",
              "'" + std::string(py_type_name(callable)) +
                  "' object is not callable",
              pos);
}

PyRef interpreter::call0(const PyRef& callable, src_pos pos) {
    return call(callable, {}, pos);
}

PyRef interpreter::call1(const PyRef& callable, PyRef a, src_pos pos) {
    return call(callable, py_args{{std::move(a)}}, pos);
}

PyRef interpreter::call_method(const PyRef& obj, const std::string& name,
                               const py_args& args, src_pos pos) {
    PyRef m = getattr(obj, name);
    if (!m)
        raise_exc("AttributeError",
                  "'" + std::string(py_type_name(obj)) +
                      "' object has no attribute '" + name + "'",
                  pos);
    return call(m, args, pos);
}

bool interpreter::maybe_call_method(const PyRef& obj, const std::string& name,
                                    const py_args& args, PyRef* out) {
    PyRef m = getattr(obj, name);
    if (!m)
        return false;
    *out = call(m, args, {});
    return true;
}

// ── param binding ─────────────────────────────────────────────────────────
void interpreter::bind_param_frame(frame& callee, const PyFuncObj* fn,
                                   const py_args& args, src_pos pos) {
    auto* locals = as_dict(callee.locals);
    const std::size_t n_named = fn->params.size();

    std::vector<bool> filled(n_named, false);
    std::size_t pos_i = 0;

    // 1) positional parameters
    for (std::size_t i = 0; i < n_named; ++i) {
        const py_param& p = fn->params[i];
        if (p.varargs || p.kwarg || p.kwonly)
            continue;
        if (pos_i < args.pos.size()) {
            dict_set(locals, py_str(p.name), args.pos[pos_i++]);
            filled[i] = true;
        }
    }
    // overflow → *args
    PyRef varargs_list = py_list();
    bool have_varargs = false;
    for (std::size_t i = 0; i < n_named; ++i) {
        if (fn->params[i].varargs) {
            have_varargs = true;
            dict_set(locals, py_str(fn->params[i].name), varargs_list);
            filled[i] = true;
        }
    }
    if (pos_i < args.pos.size() && !have_varargs)
        raise_exc("TypeError",
                  fn->name + "() takes " +
                      std::to_string(n_named) + " positional arguments but " +
                      std::to_string(args.pos.size()) + " were given",
                  pos);
    while (pos_i < args.pos.size())
        as_list(varargs_list)->v.push_back(args.pos[pos_i++]);

    // 2) keyword arguments
    PyRef kwrest = py_dict();
    bool have_kwarg = false;
    for (std::size_t i = 0; i < n_named; ++i)
        if (fn->params[i].kwarg) {
            have_kwarg = true;
            dict_set(locals, py_str(fn->params[i].name), kwrest);
            filled[i] = true;
        }
    for (const auto& [kw, val] : args.kw) {
        std::size_t slot = SIZE_MAX;
        for (std::size_t i = 0; i < n_named; ++i) {
            if (!fn->params[i].varargs && !fn->params[i].kwarg &&
                fn->params[i].name == kw) {
                slot = i;
                break;
            }
        }
        if (slot == SIZE_MAX) {
            if (have_kwarg) {
                dict_set(as_dict(kwrest), py_str(kw), val);
                continue;
            }
            raise_exc("TypeError",
                      fn->name + "() got an unexpected keyword argument '" +
                          kw + "'",
                      pos);
        }
        if (filled[slot])
            raise_exc("TypeError",
                      fn->name + "() got multiple values for argument '" +
                          kw + "'",
                      pos);
        dict_set(locals, py_str(kw), val);
        filled[slot] = true;
    }

    // 3) defaults + required check
    for (std::size_t i = 0; i < n_named; ++i) {
        const py_param& p = fn->params[i];
        if (filled[i] || p.varargs || p.kwarg)
            continue;
        if (p.default_value) {
            dict_set(locals, py_str(p.name), p.default_value);
            continue;
        }
        raise_exc("TypeError",
                  fn->name + "() missing required argument: '" + p.name + "'",
                  pos);
    }
}

// ── attribute protocol ────────────────────────────────────────────────────
// Search order (Python data-model approximation):
//   instance: instance dict → class mro attrs (descriptor unwrap) → __getattr__
//   class:    own attrs → bases mro (descriptors bound to the class)
//   module:   module dict
//   others:   kind-specific method tables handled by builtins via getset
//             descriptors registered as builtin objects on value kinds
//             (methods like "x.upper" are looked up via builtins helper).
PyRef interpreter::getattr(const PyRef& obj, const std::string& name,
                           bool* found) {
    gil_guard g(*this);
    if (found)
        *found = false;
    if (obj == nullptr)
        return nullptr;

    auto unwrap_get = [&](const PyRef& attr, const PyRef& self,
                          const PyRef& cls) -> PyRef {
        // descriptor protocol: property/staticmethod/classmethod/func
        if (attr == nullptr)
            return nullptr;
        switch (attr->kind) {
        case py_kind::staticmethod_:
            return static_cast<PyStaticObj*>(attr.get())->fn;
        case py_kind::classmethod_: {
            PyRef fn = static_cast<PyClassMethodObj*>(attr.get())->fn;
            return std::make_shared<PyBoundMethodObj>(cls, std::move(fn));
        }
        case py_kind::property_: {
            auto* p = static_cast<PyPropertyObj*>(attr.get());
            if (self && p->fget)
                return call(p->fget, py_args{{self}}, {});
            if (self)
                raise_exc("AttributeError", "unreadable attribute", {});
            return attr;    // class access gives the descriptor itself
        }
        case py_kind::func:
            if (self)
                return std::make_shared<PyBoundMethodObj>(self, attr);
            return attr;
        default:
            return attr;
        }
    };

    switch (obj->kind) {
    case py_kind::instance: {
        const auto* in = as_inst(obj);
        const auto* cls0 = as_class(in->klass);
        // __dict__ is a data descriptor — return it before any other lookup
        if (name == "__dict__") {
            if (found)
                *found = true;
            return in->attrs;
        }
        if (name == "__class__") {
            if (found)
                *found = true;
            return in->klass;
        }
        // 1) data descriptors beat instance dict (property with fget)
        if (cls0) {
            for (const auto* mc : cls0->mro) {
                if (auto* ad = as_dict(mc->attrs)) {
                    if (PyRef v = dict_get(ad, py_str(name));
                        v && v->kind == py_kind::property_) {
                        if (found)
                            *found = true;
                        return unwrap_get(v, obj, in->klass);
                    }
                }
            }
        }
        // 2) instance dict
        if (auto* d = as_dict(in->attrs)) {
            if (PyRef v = dict_get(d, py_str(name))) {
                if (found)
                    *found = true;
                return v;
            }
        }
        // 3) non-data descriptors + class attrs
        if (cls0) {
            for (const auto* mc : cls0->mro) {
                if (auto* ad = as_dict(mc->attrs)) {
                    if (PyRef v = dict_get(ad, py_str(name))) {
                        if (found)
                            *found = true;
                        return unwrap_get(v, obj, in->klass);
                    }
                }
            }
        }
        // __getattr__ fallback
        if (name != "__getattr__") {
            if (const auto* cls = as_class(in->klass)) {
                for (const auto* mc : cls->mro) {
                    if (auto* ad = as_dict(mc->attrs)) {
                        if (PyRef ga = dict_get(ad, py_str("__getattr__"))) {
                            PyRef bound = unwrap_get(ga, obj, in->klass);
                            PyRef r =
                                call(bound, py_args{{py_str(name)}}, {});
                            if (found)
                                *found = true;
                            return r;
                        }
                    }
                }
            }
        }
        return nullptr;
    }
    case py_kind::class_: {
        const auto* cls = as_class(obj);
        if (name == "__dict__") {
            if (found)
                *found = true;
            return cls->attrs;              // mappingproxy → plain dict (subset)
        }
        for (const auto* mc : cls->mro) {
            if (auto* ad = as_dict(mc->attrs)) {
                if (PyRef v = dict_get(ad, py_str(name))) {
                    if (found)
                        *found = true;
                    return unwrap_get(v, nullptr, obj);
                }
            }
        }
        if (name == "__name__")
            return py_str(cls->name);
        return nullptr;
    }
    case py_kind::module: {
        const auto* m = as_module(obj);
        if (name == "__name__")
            return py_str(m->name);
        if (name == "__file__")
            return py_str(m->path);
        if (name == "__package__")
            return py_str(m->package);
        if (name == "__dict__")
            return m->dict;
        if (auto* d = as_dict(m->dict)) {
            if (PyRef v = dict_get(d, py_str(name))) {
                if (found)
                    *found = true;
                return v;
            }
        }
        return nullptr;
    }
    case py_kind::module_proxy: {
        const auto* m = static_cast<PyModuleProxyObj*>(obj.get());
        if (name == "__name__")
            return py_str(m->name);
        if (name == "__dict__")
            return m->dict;
        if (auto* d = as_dict(m->dict)) {
            if (PyRef v = dict_get(d, py_str(name))) {
                if (found)
                    *found = true;
                return v;
            }
        }
        return nullptr;
    }
    case py_kind::super_: {
        // search mro AFTER klass
        const auto* sup = static_cast<PySuperObj*>(obj.get());
        const auto* cls = as_class(sup->klass);
        if (cls == nullptr)
            return nullptr;
        bool past = false;
        const auto* self_cls = as_class(sup->self->kind == py_kind::instance
                                            ? as_inst(sup->self)->klass
                                            : sup->self);
        if (self_cls) {
            for (const auto* mc : self_cls->mro) {
                if (mc == cls) {
                    past = true;
                    continue;
                }
                if (!past)
                    continue;
                if (auto* ad = as_dict(mc->attrs)) {
                    if (PyRef v = dict_get(ad, py_str(name))) {
                        if (found)
                            *found = true;
                        const PyRef bind_self =
                            sup->self->kind == py_kind::instance ? sup->self
                                                                 : PyRef{};
                        return unwrap_get(v, bind_self, sup->self);
                    }
                }
            }
        }
        return nullptr;
    }
    case py_kind::func: {
        const auto* fn = as_func(obj);
        if (name == "__name__")
            return py_str(fn->name);
        if (name == "__doc__")
            return fn->docstring ? fn->docstring : py_none();
        return nullptr;
    }
    case py_kind::bound_method: {
        const auto* bm = static_cast<PyBoundMethodObj*>(obj.get());
        if (name == "__func__")
            return bm->fn;
        if (name == "__self__")
            return bm->self;
        return getattr(bm->fn, name, found);
    }
    case py_kind::builtin: {
        const auto* b = as_builtin(obj);
        if (name == "__name__")
            return py_str(b->name);
        // fall through to the member table — e.g. dict.fromkeys resolves
        // against the constructor builtin object itself.
        break;
    }
    case py_kind::exception_: {
        const auto* e = as_exc(obj);
        if (name == "args")
            return e->args ? e->args : py_tuple({});
        if (name == "__class__")
            return dict_get(as_dict(builtins_dict), py_str(e->type_name));
        if (name == "__context__")
            return e->context ? e->context : py_none();
        return nullptr;
    }
    case py_kind::dict: {
        // special member: __dict__ on a dict exposes itself for struct-like
        // access patterns
        if (name == "__dict__")
            return obj;
        break;
    }
    default:
        break;
    }
    // builtin type methods (str/list/dict/…) — resolved via builtins' member
    // table installed on the interpreter (see pymini_builtins.cpp).
    if (PyRef v = getattr_builtin_member(*this, obj, name)) {
        if (found)
            *found = true;
        return v;
    }
    return nullptr;
}

// getattr_builtin_member — defined in pymini_builtins.cpp; resolves
// str/list/dict/... member methods for builtin kinds.

bool interpreter::setattr(PyRef obj, const std::string& name, PyRef value) {
    gil_guard g(*this);
    if (obj == nullptr)
        return false;
    switch (obj->kind) {
    case py_kind::instance: {
        const auto* in = as_inst(obj);
        // data descriptors first (property with setter)
        if (const auto* cls = as_class(in->klass)) {
            for (const auto* mc : cls->mro) {
                if (auto* ad = as_dict(mc->attrs)) {
                    if (PyRef v = dict_get(ad, py_str(name))) {
                        if (v->kind == py_kind::property_) {
                            auto* p = static_cast<PyPropertyObj*>(v.get());
                            if (p->fset == nullptr)
                                raise_exc("AttributeError",
                                          "can't set attribute", {});
                            call(p->fset, py_args{{obj, value}}, {});
                            return true;
                        }
                        break;   // first hit decides; non-descriptor → shadow
                    }
                }
            }
            // __setattr__ override — walk full mro until found
            for (const auto* mc : cls->mro) {
                if (auto* ad = as_dict(mc->attrs)) {
                    if (PyRef sa = dict_get(ad, py_str("__setattr__"))) {
                        PyRef bound =
                            std::make_shared<PyBoundMethodObj>(obj, sa);
                        call(bound, py_args{{py_str(name), value}}, {});
                        return true;
                    }
                }
            }
        }
        dict_set(as_dict(in->attrs), py_str(name), std::move(value));
        return true;
    }
    case py_kind::class_: {
        auto* cls = as_class(obj);
        dict_set(as_dict(cls->attrs), py_str(name), std::move(value));
        return true;
    }
    case py_kind::module: {
        auto* m = as_module(obj);
        dict_set(as_dict(m->dict), py_str(name), std::move(value));
        return true;
    }
    case py_kind::module_proxy: {
        auto* m = static_cast<PyModuleProxyObj*>(obj.get());
        dict_set(as_dict(m->dict), py_str(name), std::move(value));
        return true;
    }
    default:
        return false;
    }
}

bool interpreter::delattr(PyRef obj, const std::string& name) {
    gil_guard g(*this);
    if (obj == nullptr)
        return false;
    switch (obj->kind) {
    case py_kind::instance: {
        auto* in = as_inst(obj);
        if (dict_del(as_dict(in->attrs), py_str(name)))
            return true;
        raise_exc("AttributeError", name, {});
    }
    case py_kind::class_: {
        auto* cls = as_class(obj);
        if (dict_del(as_dict(cls->attrs), py_str(name)))
            return true;
        raise_exc("AttributeError", name, {});
    }
    case py_kind::module: {
        auto* m = as_module(obj);
        if (dict_del(as_dict(m->dict), py_str(name)))
            return true;
        raise_exc("AttributeError", name, {});
    }
    default:
        raise_exc("AttributeError", name, {});
    }
}

// hasattr uses getattr-with-found flag.
bool interpreter::hasattr(const PyRef& obj, const std::string& name) {
    bool found = false;
    try {
        getattr(obj, name, &found);
    } catch (const sig_raise&) {
        return false;
    }
    return found;
}

// apply_call_args — used by ctx bindings & future use-cases; builds py_args
// from a call expression's call_arg descriptors.
PyRef interpreter::apply_call_args(const PyRef& callable,
                                   ast_expr* const* args_nodes,
                                   std::size_t n_args,
                                   const call_arg* args_desc, frame& f) {
    (void)args_nodes;
    py_args args;
    for (std::size_t i = 0; i < n_args; ++i) {
        const call_arg& a = args_desc[i];
        PyRef v = eval(a.value.get(), f);
        if (a.dstar) {
            auto* d = as_dict(v);
            if (d == nullptr)
                raise_exc("TypeError", "argument after ** must be a mapping",
                          {});
            for (auto& [k, val] : d->items)
                if (auto* ks = as_str(k))
                    args.kw.emplace_back(ks->v, val);
        } else if (a.star) {
            for_each(v, [&](PyRef item) {
                args.pos.push_back(std::move(item));
                return true;
            });
        } else if (!a.kw.empty()) {
            args.kw.emplace_back(a.kw, std::move(v));
        } else {
            args.pos.push_back(std::move(v));
        }
    }
    return call(callable, args, {});
}

} // namespace sao::plugins::pymini
