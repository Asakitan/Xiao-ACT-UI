// pymini_interp.cpp — interpreter core: scopes, exec, eval, exceptions.
#include "pymini_interp.h"

#include "pymini_parser.h"

#include <condition_variable>

namespace sao::plugins::pymini {

// builtin exception parent chain (collapse builtin classes to names).
static const std::unordered_map<std::string, std::string>& builtin_exc_base() {
    static const std::unordered_map<std::string, std::string> m = {
        {"Exception", "BaseException"},
        {"ValueError", "Exception"}, {"TypeError", "Exception"},
        {"LookupError", "Exception"}, {"KeyError", "LookupError"},
        {"IndexError", "LookupError"},
        {"AttributeError", "Exception"}, {"NameError", "Exception"},
        {"UnboundLocalError", "NameError"},
        {"RuntimeError", "Exception"}, {"NotImplementedError", "RuntimeError"},
        {"RecursionError", "RuntimeError"},
        {"ImportError", "Exception"}, {"ModuleNotFoundError", "ImportError"},
        {"StopIteration", "Exception"}, {"StopAsyncIteration", "Exception"},
        {"ArithmeticError", "Exception"}, {"ZeroDivisionError", "ArithmeticError"},
        {"OverflowError", "ArithmeticError"},
        {"OSError", "Exception"}, {"IOError", "OSError"},
        {"FileNotFoundError", "OSError"}, {"PermissionError", "OSError"},
        {"IsADirectoryError", "OSError"}, {"NotADirectoryError", "OSError"},
        {"TimeoutError", "OSError"}, {"FileExistsError", "OSError"},
        {"EOFError", "Exception"},
        {"SyntaxError", "Exception"}, {"IndentationError", "SyntaxError"},
        {"TabError", "IndentationError"},
        {"UnicodeError", "ValueError"}, {"UnicodeDecodeError", "UnicodeError"},
        {"UnicodeEncodeError", "UnicodeError"},
        {"AssertionError", "Exception"},
        {"GeneratorExit", "BaseException"}, {"KeyboardInterrupt", "BaseException"},
        {"SystemExit", "BaseException"}, {"MemoryError", "Exception"},
        {"UnsupportedSyntax", "Exception"}, {"BufferError", "Exception"},
        {"Warning", "Exception"}, {"UserWarning", "Warning"},
        {"DeprecationWarning", "Warning"}, {"ResourceWarning", "Warning"},
        {"FutureWarning", "Warning"}, {"PendingDeprecationWarning", "Warning"},
        {"BytesWarning", "Warning"}, {"RuntimeWarning", "Warning"},
        {"SyntaxWarning", "Warning"}, {"ImportWarning", "Warning"},
        {"UnicodeWarning", "Warning"},
    };
    return m;
}

PyRef make_exc(const std::string& type, const std::string& msg) {
    auto e = std::make_shared<PyExcObj>();
    e->type_name = type;
    e->args = py_tuple({py_str(msg)});
    e->context = py_none();
    return e;
}

const char* py_type_name(const PyRef& r) {
    if (!r)
        return "NoneType";
    switch (r->kind) {
    case py_kind::none_:        return "NoneType";
    case py_kind::boolean:      return "bool";
    case py_kind::integer:      return "int";
    case py_kind::number:       return "float";
    case py_kind::string:       return "str";
    case py_kind::bytes_:       return "bytes";
    case py_kind::tuple_:       return "tuple";
    case py_kind::list:         return "list";
    case py_kind::set:          return "set";
    case py_kind::frozenset:    return "frozenset";
    case py_kind::dict:         return "dict";
    case py_kind::func:         return "function";
    case py_kind::builtin:      return "builtin_function_or_method";
    case py_kind::bound_method: return "method";
    case py_kind::module:       return "module";
    case py_kind::class_:       return "type";
    case py_kind::instance:     return "instance";
    case py_kind::exception_:   return "exception";
    case py_kind::range_:       return "range";
    case py_kind::slice_:       return "slice";
    case py_kind::file_:        return "file";
    case py_kind::super_:       return "super";
    case py_kind::iterator:     return "iterator";
    case py_kind::staticmethod_: return "staticmethod";
    case py_kind::classmethod_: return "classmethod";
    case py_kind::property_:    return "property";
    case py_kind::queue_:       return "queue";
    case py_kind::deque_:       return "deque";
    case py_kind::counter_:     return "counter";
    case py_kind::module_proxy: return "module_proxy";
    case py_kind::thread_:      return "thread";
    case py_kind::lock_:        return "lock";
    case py_kind::event_:       return "event";
    case py_kind::enumerate_:   return "enumerate";
    case py_kind::map_:         return "map";
    case py_kind::filter_:      return "filter";
    case py_kind::zip_:         return "zip";
    case py_kind::reversed_:    return "reversed";
    case py_kind::cell:         return "cell";
    default:                    return "object";
    }
}

// deep clone of an expression tree (lambda body capture).
expr_ptr clone_expr(const ast_expr* e) {
    if (e == nullptr)
        return nullptr;
    auto c = std::make_unique<ast_expr>();
    c->tag = e->tag;
    c->pos = e->pos;
    c->const_value = e->const_value;
    c->name = e->name;
    c->op = e->op;
    c->base = clone_expr(e->base.get());
    c->index = clone_expr(e->index.get());
    c->orelse = clone_expr(e->orelse.get());
    c->step = clone_expr(e->step.get());
    for (const auto& p : e->parts)
        c->parts.push_back(clone_expr(p.get()));
    c->ops = e->ops;
    for (const auto& a : e->call_args) {
        call_arg na;
        na.value = clone_expr(a.value.get());
        na.kw = a.kw;
        na.star = a.star;
        na.dstar = a.dstar;
        c->call_args.push_back(std::move(na));
    }
    for (const auto& g : e->generators) {
        comp_clause nc;
        nc.target = clone_expr(g.target.get());
        nc.iter = clone_expr(g.iter.get());
        for (const auto& cond : g.ifs)
            nc.ifs.push_back(clone_expr(cond.get()));
        c->generators.push_back(std::move(nc));
    }
    for (const auto& p : e->params) {
        ast_param_decl np;
        np.name = p.name;
        np.default_value = clone_expr(p.default_value.get());
        np.varargs = p.varargs;
        np.kwonly = p.kwonly;
        np.kwarg = p.kwarg;
        c->params.push_back(std::move(np));
    }
    c->fstring_raw_flag = e->fstring_raw_flag;
    return c;
}

interpreter::interpreter(config c) : cfg(std::move(c)) {
    builtins_dict = py_dict();
    sys_modules = py_dict();
}

interpreter::~interpreter() {
    // break module↔function cycles for teardown
    try {
        if (auto* mods = as_dict(sys_modules)) {
            for (auto& [k, m] : mods->items) {
                if (auto* mo = as_module(m)) {
                    if (auto* d = as_dict(mo->dict))
                        d->items.clear();
                }
            }
            mods->items.clear();
        }
        if (auto* b = as_dict(builtins_dict))
            b->items.clear();
    } catch (...) {
    }
}

// ── modules ───────────────────────────────────────────────────────────────
PyRef interpreter::create_module_object(const std::string& name,
                                        const std::string& path_utf8,
                                        const std::string& package) {
    auto m = std::make_shared<PyModuleObj>();
    m->name = name;
    m->dict = py_dict();
    m->path = path_utf8;
    m->package = package.empty() ? name : package;
    auto* d = as_dict(m->dict);
    dict_set(d, py_str("__name__"), py_str(name));
    dict_set(d, py_str("__file__"), py_str(path_utf8));
    dict_set(d, py_str("__package__"), py_str(m->package));
    dict_set(d, py_str("__builtins__"), builtins_dict);
    return m;
}

void interpreter::register_module(PyRef module) {
    if (auto* m = as_module(module))
        dict_set(as_dict(sys_modules), py_str(m->name), std::move(module));
}

PyRef interpreter::find_loaded(const std::string& name) {
    return dict_get(as_dict(sys_modules), py_str(name));
}

// The import machinery itself lives in pymini_imports.cpp; declared here so
// exec_module_source can enqueue a module while pending.
PyRef interpreter::exec_module_source(const std::string& logical_name,
                                      const std::string& file_utf8,
                                      std::string_view source) {
    gil_guard g(*this);
    auto mod = create_module_object(logical_name, file_utf8);
    register_module(mod);
    pending.push_back(mod);
    try {
        ast_module m = parse_source(source, file_utf8);
        frame f;
        auto* modp = as_module(mod);
        f.locals = modp->dict;
        f.globals = modp->dict;
        f.fn_name = "<module>";
        f.file = file_utf8;
        collect_scope_decls(m.body, f);
        exec_body(m.body, f);
    } catch (...) {
        pending.pop_back();
        // keep half-imported module OUT of sys.modules (Python behaviour)
        dict_del(as_dict(sys_modules), py_str(logical_name));
        throw;
    }
    pending.pop_back();
    return mod;
}

// ── scopes ────────────────────────────────────────────────────────────────
PyRef interpreter::scope_get(const frame& f, const std::string& name,
                             bool* found) {
    const PyRef key = py_str(name);
    // locals
    if (auto v = dict_get(as_dict(f.locals), key)) {
        if (found)
            *found = true;
        return v;
    }
    // closures (enclosing scopes, innermost→out)
    for (const auto& cap : f.captured) {
        if (auto v = dict_get(as_dict(cap), key)) {
            if (found)
                *found = true;
            return v;
        }
    }
    // globals
    if (auto v = dict_get(as_dict(f.globals), key)) {
        if (found)
            *found = true;
        return v;
    }
    // builtins
    if (auto v = dict_get(as_dict(builtins_dict), key)) {
        if (found)
            *found = true;
        return v;
    }
    if (found)
        *found = false;
    return nullptr;
}

void interpreter::scope_set(frame& f, const std::string& name, PyRef value) {
    const PyRef key = py_str(name);
    if (f.globals_set.count(name)) {
        dict_set(as_dict(f.globals), key, std::move(value));
        return;
    }
    if (f.nonlocals_set.count(name)) {
        for (const auto& cap : f.captured) {
            auto* d = as_dict(cap);
            if (d != nullptr && dict_get(d, key)) {
                dict_set(d, key, std::move(value));
                return;
            }
        }
        raise_exc("SyntaxError",
                  "no binding for nonlocal '" + name + "' found", {});
    }
    dict_set(as_dict(f.locals), key, std::move(value));
}

void interpreter::collect_scope_decls(const std::vector<stmt_ptr>& body,
                                      frame& f) {
    const std::function<void(const ast_stmt*)> scan = [&](const ast_stmt* s) {
        if (s == nullptr)
            return;
        if (s->tag == st::global_) {
            for (const auto& n : s->names)
                f.globals_set.insert(n);
            return;
        }
        if (s->tag == st::nonlocal_) {
            for (const auto& n : s->names)
                f.nonlocals_set.insert(n);
            return;
        }
        // nested funcdef/classdef bodies have their own scope
        if (s->tag == st::funcdef || s->tag == st::classdef)
            return;
        for (const auto& b : s->body)
            scan(b.get());
        for (const auto& b : s->orelse)
            scan(b.get());
        for (const auto& b : s->final)
            scan(b.get());
        for (const auto& a : s->except_arms)
            for (const auto& b : a.body)
                scan(b.get());
    };
    for (const auto& s : body)
        scan(s.get());
}

void interpreter::scope_del(frame& f, const std::string& name) {
    const PyRef key = py_str(name);
    if (f.globals_set.count(name)) {
        if (dict_del(as_dict(f.globals), key))
            return;
        raise_exc("NameError", "name '" + name + "' is not defined", {});
    }
    if (f.nonlocals_set.count(name)) {
        for (const auto& cap : f.captured) {
            auto* d = as_dict(cap);
            if (d != nullptr && dict_get(d, key)) {
                dict_del(d, key);
                return;
            }
        }
        raise_exc("NameError", "no binding for nonlocal '" + name +
                                   "' found", {});
    }
    // plain `del x` removes from the local scope only — a same-named
    // global/builtin must not be deleted as a side effect.
    auto* d = as_dict(f.locals);
    if (dict_del(d, key))
        return;
    raise_exc("NameError", "name '" + name + "' is not defined", {});
}

// ── exceptions ────────────────────────────────────────────────────────────
void interpreter::record_frame(PyExcObj* exc) {
    if (exc == nullptr || cur_frame == nullptr)
        return;
    exc->trace.push_back(cur_frame->file + ":" +
                         std::to_string(cur_frame->line) + " in " +
                         cur_frame->fn_name);
}

void interpreter::raise_exc(const std::string& type, const std::string& msg,
                            src_pos pos) {
    auto e = make_exc(type, msg);
    record_frame(as_exc(e));
    (void)pos;
    active_exc = e;
    throw sig_raise{std::move(e)};
}

void interpreter::raise_obj(PyRef exc, src_pos pos) {
    // `raise SomeClass` → instantiate; `raise exc_obj` → use directly;
    // script exception instances (class instances) are also valid.
    if (exc && exc->kind == py_kind::class_) {
        exc = call0(exc, pos);
    }
    if (auto* e = as_exc(exc)) {
        record_frame(e);
        active_exc = exc;
        throw sig_raise{std::move(exc)};
    }
    if (exc && exc->kind == py_kind::instance) {
        active_exc = exc;
        throw sig_raise{std::move(exc)};
    }
    raise_exc("TypeError", "exceptions must derive from BaseException", pos);
}

bool interpreter::exc_matches(const PyRef& exc_val, const PyRef& type_val) {
    // type_val may be: a class object, a builtin exception marker, or a
    // tuple of those.  exc_val is either a PyExcObj or a script instance of
    // an exception class.
    if (!exc_val || !type_val)
        return false;
    if (auto* t = as_tuple(type_val)) {
        for (const auto& item : t->v)
            if (exc_matches(exc_val, item))
                return true;
        return false;
    }

    // names in the exception's own chain
    std::unordered_set<std::string> have;
    auto add_chain = [&have](const std::string& start) {
        std::string cur = start;
        for (int i = 0; i < 32 && !cur.empty(); ++i) {
            have.insert(cur);
            const auto it = builtin_exc_base().find(cur);
            if (it == builtin_exc_base().end())
                break;
            cur = it->second;
        }
    };
    if (const auto* e = as_exc(exc_val)) {
        add_chain(e->type_name);
    } else if (const auto* in = as_inst(exc_val)) {
        if (in->klass) {
            if (const auto* c = as_class(in->klass)) {
                for (const auto* mc : c->mro)
                    if (mc)
                        add_chain(mc->name);
            }
        }
    } else {
        return false;
    }

    // wanted names: a class object contributes its name; a builtin marker
    // contributes its registered name; PyExcObj used as a filter contributes
    // its type_name.
    auto want_name = [](const PyRef& v) -> std::string {
        if (const auto* c = as_class(v))
            return c->name;
        if (const auto* e = as_exc(v))
            return e->type_name;
        if (const auto* b = as_builtin(v))
            return b->name;
        return {};
    };
    const std::string w = want_name(type_val);
    if (w.empty())
        return false;
    return have.count(w) != 0;
}

// ── exec ──────────────────────────────────────────────────────────────────
void interpreter::exec_body(const std::vector<stmt_ptr>& body, frame& f) {
    for (const auto& s : body) {
        f.line = s->pos.line;
        exec(s.get(), f);
    }
}

void interpreter::exec(const ast_stmt* s, frame& f) {
    gil_guard g(*this);
    if (static_cast<int64_t>(++call_depth) > cfg.max_call_depth) {
        --call_depth;
        raise_exc("RecursionError", "maximum recursion depth exceeded", s->pos);
    }
    struct depth_guard {
        uint32_t& d;
        ~depth_guard() { --d; }
    } guard{call_depth};

    switch (s->tag) {
    case st::suite:
        exec_body(s->body, f);
        return;
    case st::expr_stmt: {
        (void)eval(s->value.get(), f);
        return;
    }
    case st::pass_:
        return;
    case st::break_:
        throw sig_break{};
    case st::continue_:
        throw sig_continue{};
    case st::return_: {
        PyRef v = py_none();
        if (s->value)
            v = eval(s->value.get(), f);
        throw sig_return{std::move(v)};
    }
    case st::raise_: {
        PyRef exc;
        if (s->value)
            exc = eval(s->value.get(), f);
        if (!exc) {
            raise_exc("RuntimeError", "raise outside except block", s->pos);
        }
        if (s->value2) {
            // raise X from Y
            const PyRef cause = eval(s->value2.get(), f);
            if (auto* e = as_exc(exc); e != nullptr && as_exc(cause) != nullptr)
                e->context = cause;
        }
        raise_obj(exc, s->pos);
    }
    case st::assert_: {
        if (!truthy(eval(s->value.get(), f))) {
            std::string msg;
            if (s->value2)
                msg = py_to_str(*this, eval(s->value2.get(), f));
            raise_exc("AssertionError", msg, s->pos);
        }
        return;
    }
    case st::del_: {
        for (const auto& t : s->targets)
            del_target(t.get(), f);
        return;
    }
    case st::assign: {
        PyRef value = eval(s->value.get(), f);
        for (const auto& t : s->targets)
            bind_unpack(t.get(), value, f);
        return;
    }
    case st::aug_assign: {
        // evaluate existing target + rhs; write back
        const auto& t = s->targets.front();
        PyRef cur = eval(t.get(), f);
        PyRef rhs = eval(s->value.get(), f);
        // translate x+=y → x = x + y via binop
        tok_kind op = s->aug_op;
        switch (op) {
        case tok_kind::plus_eq:   op = tok_kind::plus; break;
        case tok_kind::minus_eq:  op = tok_kind::minus; break;
        case tok_kind::star_eq:   op = tok_kind::star; break;
        case tok_kind::dstar_eq:  op = tok_kind::dstar; break;
        case tok_kind::slash_eq:  op = tok_kind::slash; break;
        case tok_kind::dslash_eq: op = tok_kind::dslash; break;
        case tok_kind::percent_eq: op = tok_kind::percent; break;
        case tok_kind::amp_eq:    op = tok_kind::amp; break;
        case tok_kind::pipe_eq:   op = tok_kind::pipe; break;
        case tok_kind::caret_eq:  op = tok_kind::caret; break;
        case tok_kind::lshift_eq: op = tok_kind::lshift; break;
        case tok_kind::rshift_eq: op = tok_kind::rshift; break;
        case tok_kind::at_eq:     op = tok_kind::at; break;
        default:
            raise_exc("SyntaxError", "unsupported augmented assignment", s->pos);
        }
        PyRef result = binary(op, cur, rhs, s->pos);
        bind_unpack(t.get(), result, f);
        return;
    }
    case st::ann_assign: {
        // annotation ignored at runtime (Python stores in __annotations__)
        if (s->value2) {
            PyRef v = eval(s->value2.get(), f);
            for (const auto& t : s->targets)
                bind_unpack(t.get(), v, f);
        }
        return;
    }
    case st::global_:
    case st::nonlocal_:
        // handled at frame construction; here they are no-ops
        return;
    case st::if_: {
        // elif chain encoded as nested orelse if_ nodes
        bool ran = false;
        const ast_stmt* cur = s;
        while (cur != nullptr) {
            if (truthy(eval(cur->value.get(), f))) {
                exec_body(cur->body, f);
                ran = true;
                break;
            }
            if (cur->orelse.empty())
                break;
            if (cur->orelse.size() == 1 && cur->orelse.front()->tag == st::if_ &&
                cur->orelse.front()->value != nullptr) {
                cur = cur->orelse.front().get();     // elif
                continue;
            }
            exec_body(cur->orelse, f);                // else
            ran = true;
            break;
        }
        (void)ran;
        return;
    }
    case st::while_: {
        while (truthy(eval(s->value.get(), f))) {
            try {
                exec_body(s->body, f);
            } catch (const sig_break&) {
                return;
            } catch (const sig_continue&) {
            }
        }
        exec_body(s->orelse, f);
        return;
    }
    case st::for_: {
        PyRef iterable = eval(s->value.get(), f);
        PyRef it = iter(iterable);
        PyRef item;
        bool broke = false;
        try {
            while (iter_next(it, &item)) {
                for (const auto& t : s->targets)
                    bind_unpack(t.get(), item, f);
                try {
                    exec_body(s->body, f);
                } catch (const sig_continue&) {
                }
            }
        } catch (const sig_break&) {
            broke = true;
        }
        if (!broke)
            exec_body(s->orelse, f);
        return;
    }
    case st::try_: {
        bool body_clean = false;
        try {
            try {
                exec_body(s->body, f);
                body_clean = true;
            } catch (const sig_raise& sig) {
                const PyRef exc = sig.exc;
                bool handled = false;
                for (auto& arm : s->except_arms) {
                    bool match = arm.type == nullptr;
                    if (!match)
                        match = exc_matches(exc, eval(arm.type.get(), f));
                    if (!match)
                        continue;
                    handled = true;
                    active_exc = exc;         // traceback.format_exc reads it
                    if (!arm.name.empty()) {
                        scope_set(f, arm.name, exc);
                        try {
                            exec_body(arm.body, f);
                        } catch (...) {
                            scope_del(f, arm.name);
                            throw;
                        }
                        scope_del(f, arm.name);   // `as` name cleared
                    } else {
                        exec_body(arm.body, f);
                    }
                    break;
                }
                if (!handled)
                    throw;
            }
            // else: runs only when the try body completed cleanly
            if (body_clean && !s->orelse.empty())
                exec_body(s->orelse, f);
        } catch (...) {
            // finally: always runs, even during exception propagation
            if (!s->final.empty())
                exec_body(s->final, f);
            throw;
        }
        if (!s->final.empty())
            exec_body(s->final, f);
        return;
    }
    case st::with_: {
        exec_with_item(s, f, 0);
        return;
    }
    case st::funcdef: {
        PyFuncObj* fn = nullptr;
        {
            auto fno = std::make_shared<PyFuncObj>();
            fno->name = s->name;
            fno->body = s->body;
            // capture: enclosing locals become the closure chain.
            // Methods defined inside a class body do NOT capture class attrs
            // (Python: class scope is not an enclosing scope for methods).
            if (!f.in_class_body && f.locals != f.globals)
                fno->closure.push_back(f.locals);
            for (const auto& cap : f.captured)
                fno->closure.push_back(cap);
            fno->globals_dict = f.globals;
            // params: evaluate defaults now
            for (const auto& p : s->params) {
                py_param pp;
                pp.name = p.name;
                pp.varargs = p.varargs;
                pp.kwonly = p.kwonly;
                pp.kwarg = p.kwarg;
                if (p.default_value)
                    pp.default_value = eval(p.default_value.get(), f);
                fno->params.push_back(std::move(pp));
            }
            // docstring: first stmt expr of str literal
            if (!s->body.empty() && s->body.front()->tag == st::expr_stmt &&
                s->body.front()->value &&
                s->body.front()->value->tag == et::literal &&
                s->body.front()->value->const_value &&
                s->body.front()->value->const_value->kind == py_kind::string)
                fno->docstring = s->body.front()->value->const_value;
            fn = fno.get();
            PyRef func = fno;
            // decorators (innermost first)
            for (auto it = s->decorators.rbegin(); it != s->decorators.rend();
                 ++it) {
                PyRef d = eval(it->get(), f);
                func = call(d, py_args{{func}}, s->pos);
            }
            scope_set(f, s->name, func);
        }
        return;
    }
    case st::classdef: {
        // bases
        std::vector<PyRef> bases;
        for (const auto& b : s->bases)
            bases.push_back(eval(b.get(), f));
        for (const auto& [kw, v] : s->kw_bases) {
            (void)kw;
            (void)eval(v.get(), f);   // consume for side effects; metaclass ignored
        }
        auto cls = std::make_shared<PyClassObj>();
        cls->name = s->name;
        cls->attrs = py_dict();
        cls->bases = std::move(bases);
        if (cls->bases.empty())
            cls->bases.push_back(dict_get(as_dict(builtins_dict), py_str("object")));
        // body exec in class attrs dict — class scope IS visible to the
        // class body itself (enclosing lookup: locals=captured front chain).
        {
            frame cf;
            cf.locals = cls->attrs;
            cf.globals = f.globals;
            // enclosing function locals first, then its own outer chain
            if (f.locals != f.globals || !f.captured.empty())
                cf.captured.push_back(f.locals);
            for (const auto& cap : f.captured)
                cf.captured.push_back(cap);
            cf.fn_name = s->name;
            cf.file = f.file;
            cf.in_class_body = true;
            cf.caller = cur_frame;
            collect_scope_decls(s->body, cf);
            exec_body(s->body, cf);
        }
        // inject `__class__` cell into every method for 0-arg super()
        {
            PyRef cell = py_dict();
            dict_set(as_dict(cell), py_str("__class__"), cls);
            auto inject = [&](PyRef fn_obj) {
                if (auto* fn = as_func(fn_obj)) {
                    fn->closure.push_back(cell);
                } else if (auto* sm = static_cast<PyStaticObj*>(
                               fn_obj && fn_obj->kind == py_kind::staticmethod_
                                   ? fn_obj.get()
                                   : nullptr)) {
                    if (auto* sf = as_func(sm->fn))
                        sf->closure.push_back(cell);
                } else if (auto* cm = static_cast<PyClassMethodObj*>(
                               fn_obj && fn_obj->kind == py_kind::classmethod_
                                   ? fn_obj.get()
                                   : nullptr)) {
                    if (auto* sf = as_func(cm->fn))
                        sf->closure.push_back(cell);
                } else if (auto* pp = static_cast<PyPropertyObj*>(
                               fn_obj && fn_obj->kind == py_kind::property_
                                   ? fn_obj.get()
                                   : nullptr)) {
                    for (const PyRef& f2 : {pp->fget, pp->fset, pp->fdel})
                        if (auto* sf = as_func(f2))
                            sf->closure.push_back(cell);
                }
            };
            for (auto& [k, v] : as_dict(cls->attrs)->items)
                inject(v);
        }
        // MRO: simple DFS (keeps class order, dedupes)
        {
            std::unordered_set<PyClassObj*> seen;
            std::function<void(PyClassObj*)> dfs = [&](PyClassObj* c) {
                if (seen.insert(c).second) {
                    cls->mro.push_back(c);
                    for (const auto& b : c->bases)
                        if (auto* bc = as_class(b))
                            dfs(bc);
                }
            };
            dfs(cls.get());
        }
        // __init_subclass__: implicit classmethod invoked on the parent
        // bases with the new subclass.  Called after mro so bases see a
        // finished class (Enum member wrapping relies on this).
        {
            for (std::size_t mi = 1; mi < cls->mro.size(); ++mi) {
                auto* base = cls->mro[mi];
                auto* bd = as_dict(base->attrs);
                PyRef hook = bd ? dict_get(bd, py_str("__init_subclass__"))
                                : nullptr;
                if (hook) {
                    call(hook, py_args{{cls}}, s->pos);
                    break;                    // first (nearest) base wins
                }
            }
        }
        PyRef clsv = cls;
        for (auto it = s->decorators.rbegin(); it != s->decorators.rend(); ++it) {
            PyRef d = eval(it->get(), f);
            clsv = call(d, py_args{{clsv}}, s->pos);
        }
        scope_set(f, s->name, clsv);
        return;
    }
    case st::import:
    case st::import_from: {
        exec_import(s, f);
        return;
    }
    default:
        raise_exc("RuntimeError", "unhandled statement kind", s->pos);
    }
}

// ── eval ──────────────────────────────────────────────────────────────────
PyRef interpreter::eval(const ast_expr* e, frame& f) {
    gil_guard g(*this);
    if (static_cast<int64_t>(++call_depth) > cfg.max_call_depth) {
        --call_depth;
        raise_exc("RecursionError", "maximum recursion depth exceeded", e->pos);
    }
    struct g2 {
        uint32_t& d;
        ~g2() { --d; }
    } guard{call_depth};

    switch (e->tag) {
    case et::literal:
        return e->const_value ? e->const_value : py_none();
    case et::ellipses_:
        return dict_get(as_dict(builtins_dict), py_str("Ellipsis"));
    case et::name: {
        bool found = false;
        PyRef v = scope_get(f, e->name, &found);
        if (!found)
            raise_exc("NameError", "name '" + e->name + "' is not defined", e->pos);
        return v;
    }
    case et::attr: {
        PyRef base = eval(e->base.get(), f);
        PyRef v = getattr(base, e->name);
        if (!v)
            raise_exc("AttributeError",
                      "'" + std::string(py_type_name(base)) +
                          "' object has no attribute '" + e->name + "'",
                      e->pos);
        return v;
    }
    case et::subscript: {
        PyRef base = eval(e->base.get(), f);
        PyRef idx = eval(e->index.get(), f);
        return subscript_get(base, idx, e->pos);
    }
    case et::slice_lit: {
        PyRef lo = e->base ? eval(e->base.get(), f) : py_none();
        PyRef hi = e->index ? eval(e->index.get(), f) : py_none();
        PyRef st = e->step ? eval(e->step.get(), f) : py_none();
        return std::make_shared<PySliceObj>(std::move(lo), std::move(hi),
                                            std::move(st));
    }
    case et::call: {
        PyRef fn = eval(e->base.get(), f);
        py_args args;
        args.pos.reserve(e->call_args.size());
        for (const auto& a : e->call_args) {
            PyRef v = eval(a.value.get(), f);
            if (a.dstar) {
                auto* d = as_dict(v);
                if (d == nullptr)
                    raise_exc("TypeError", "argument after ** must be a mapping",
                              e->pos);
                for (auto& [k, val] : d->items) {
                    if (auto* ks = as_str(k))
                        args.kw.emplace_back(ks->v, val);
                    else
                        raise_exc("TypeError", "keywords must be strings", e->pos);
                }
            } else if (a.star) {
                // expand in place — `f(1, *m, 2)` must keep source order
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
        return call(fn, args, e->pos);
    }
    case et::binop: {
        // `in`-family not produced by binop
        PyRef a = eval(e->base.get(), f);
        PyRef b = eval(e->index.get(), f);
        return binary(e->op, a, b, e->pos);
    }
    case et::unop: {
        PyRef a = eval(e->base.get(), f);
        if (e->name == "not")                    // parser marks `not` via name
            return py_bool(!truthy(a));
        return unary(e->op, a, e->pos);
    }
    case et::boolop: {
        // parts chain; op = amp (and) / pipe (or)
        PyRef result;
        for (auto& p : e->parts) {
            result = eval(p.get(), f);
            const bool t = truthy(result);
            if (e->op == tok_kind::pipe && t)
                return result;
            if (e->op == tok_kind::amp && !t)
                return result;
        }
        return result ? result : py_none();
    }
    case et::compare: {
        // pairwise chain: base op parts[0] op parts[1] …
        PyRef lhs = eval(e->base.get(), f);
        bool all = true;
        PyRef rhs;
        for (std::size_t i = 0; i < e->ops.size(); ++i) {
            rhs = eval(e->parts[i].get(), f);
            PyRef r = compare(e->ops[i], lhs, rhs, e->pos);
            if (!truthy(r)) {
                all = false;
                // still must NOT evaluate later operands (Python short-circuits)
                break;
            }
            lhs = rhs;
        }
        return py_bool(all);
    }
    case et::ifexp: {
        if (truthy(eval(e->index.get(), f)))
            return eval(e->base.get(), f);
        return eval(e->orelse.get(), f);
    }
    case et::lambda_: {
        auto fn = std::make_shared<PyFuncObj>();
        fn->name = "<lambda>";
        fn->globals_dict = f.globals;
        if (!f.in_class_body && f.locals != f.globals)
            fn->closure.push_back(f.locals);
        for (const auto& cap : f.captured)
            fn->closure.push_back(cap);
        for (const auto& p : e->params) {
            py_param pp;
            pp.name = p.name;
            pp.varargs = p.varargs;
            pp.kwonly = p.kwonly;
            pp.kwarg = p.kwarg;
            if (p.default_value)
                pp.default_value = eval(p.default_value.get(), f);
            fn->params.push_back(std::move(pp));
        }
        // body = cloned single expr → wrapped in a return stmt owned by fn
        auto rs = std::make_shared<ast_stmt>();
        rs->tag = st::return_;
        rs->pos = e->pos;
        rs->value = clone_expr(e->base.get());
        fn->body.push_back(std::move(rs));
        return fn;
    }
    case et::list_lit: {
        std::vector<PyRef> items;
        items.reserve(e->parts.size());
        for (const auto& p : e->parts) {
            if (p->tag == et::star_) {
                PyRef it = eval(p->base.get(), f);
                for_each(it, [&](PyRef v) {
                    items.push_back(std::move(v));
                    return true;
                });
            } else {
                items.push_back(eval(p.get(), f));
            }
        }
        return py_list(std::move(items));
    }
    case et::tuple_lit: {
        std::vector<PyRef> items;
        items.reserve(e->parts.size());
        for (const auto& p : e->parts) {
            if (p->tag == et::star_) {
                PyRef it = eval(p->base.get(), f);
                for_each(it, [&](PyRef v) {
                    items.push_back(std::move(v));
                    return true;
                });
            } else {
                items.push_back(eval(p.get(), f));
            }
        }
        return py_tuple(std::move(items));
    }
    case et::set_lit: {
        auto s = std::make_shared<PySetObj>();
        for (const auto& p : e->parts) {
            if (p->tag == et::star_) {
                PyRef it = eval(p->base.get(), f);
                for_each(it, [&](PyRef v) {
                    set_add(*this, s->items, v);
                    return true;
                });
            } else {
                set_add(*this, s->items, eval(p.get(), f));
            }
        }
        return s;
    }
    case et::dict_lit: {
        auto d = py_dict();
        for (std::size_t i = 0; i + 1 < e->parts.size(); i += 2) {
            const auto& k = e->parts[i];
            const auto& v = e->parts[i + 1];
            if (k->tag == et::literal && k->name == "**merge") {
                PyRef src = eval(v.get(), f);
                auto* sd = as_dict(src);
                if (sd != nullptr)
                    for (auto& [mk, mv] : sd->items)
                        dict_set(*this, as_dict(d), mk, mv);
                else {
                    PyRef items_fn = getattr(src, "items");
                    if (items_fn)
                        for_each(call0(items_fn, e->pos), [&](PyRef pair) {
                            auto* p = as_tuple(pair);
                            if (p != nullptr && p->v.size() == 2)
                                dict_set(*this, as_dict(d), p->v[0], p->v[1]);
                            return true;
                        });
                }
                continue;
            }
            dict_set(*this, as_dict(d), eval(k.get(), f), eval(v.get(), f));
        }
        return d;
    }
    case et::star_:
        raise_exc("SyntaxError", "starred expression misplaced", e->pos);
    case et::fstring_: {
        std::string out;
        for (const auto& p : e->parts) {
            if (p->tag == et::literal) {
                if (auto* s = as_str(eval(p.get(), f)))
                    out += s->v;
            } else {
                PyRef v = eval(p.get(), f);
                out += py_to_str(*this, v);
            }
        }
        return py_str(out);
    }
    case et::comprehension:
        return eval_comprehension(e, f);
    default:
        raise_exc("RuntimeError", "unhandled expression kind", e->pos);
    }
}

// ── del target ────────────────────────────────────────────────────────────
void interpreter::del_target(const ast_expr* t, frame& f) {
    if (t->tag == et::name) {
        // del name → search locals then globals for the del (Python: locals
        // only for simple name; still try captured chain for nonlocal decl)
        scope_del(f, t->name);
        return;
    }
    if (t->tag == et::attr) {
        PyRef base = eval(t->base.get(), f);
        delattr(base, t->name);
        return;
    }
    if (t->tag == et::subscript) {
        PyRef base = eval(t->base.get(), f);
        PyRef idx = eval(t->index.get(), f);
        subscript_del(base, idx, t->pos);
        return;
    }
    if (t->tag == et::tuple_lit || t->tag == et::list_lit) {
        for (const auto& p : t->parts)
            del_target(p.get(), f);
        return;
    }
    raise_exc("SyntaxError", "cannot delete target", t->pos);
}

// ── bind_unpack ───────────────────────────────────────────────────────────
void interpreter::bind_unpack(const ast_expr* target, PyRef value, frame& f) {
    switch (target->tag) {
    case et::name: {
        // `global`/`nonlocal` redirection: statements marked them by
        // inserting the name into the frame's special dicts at def-entry —
        // handled by scope_set which redirects when the stmt registered the
        // name. Here just locals.
        scope_set(f, target->name, std::move(value));
        return;
    }
    case et::attr: {
        PyRef base = eval(target->base.get(), f);
        if (!setattr(base, target->name, std::move(value)))
            raise_exc("AttributeError", "cannot set attribute '" + target->name +
                                            "'", target->pos);
        return;
    }
    case et::subscript: {
        PyRef base = eval(target->base.get(), f);
        PyRef idx = eval(target->index.get(), f);
        subscript_set(base, idx, std::move(value), target->pos);
        return;
    }
    case et::tuple_lit:
    case et::list_lit: {
        std::vector<PyRef> items;
        for_each(value, [&](PyRef v) {
            items.push_back(std::move(v));
            return true;
        });
        // find optional single star — `a, *b, *c = xs` is a SyntaxError,
        // silently overwriting star_idx would lose the earlier star.
        std::size_t star_idx = SIZE_MAX;
        for (std::size_t i = 0; i < target->parts.size(); ++i)
            if (target->parts[i]->tag == et::star_) {
                if (star_idx != SIZE_MAX)
                    raise_exc("SyntaxError",
                              "multiple starred expressions in assignment",
                              target->pos);
                star_idx = i;
            }
        if (star_idx == SIZE_MAX) {
            if (items.size() != target->parts.size())
                raise_exc("ValueError",
                          "not enough values to unpack (expected " +
                              std::to_string(target->parts.size()) + ", got " +
                              std::to_string(items.size()) + ")",
                          target->pos);
            for (std::size_t i = 0; i < target->parts.size(); ++i)
                bind_unpack(target->parts[i].get(), items[i], f);
        } else {
            const std::size_t pre = star_idx;
            const std::size_t post = target->parts.size() - star_idx - 1;
            if (items.size() < pre + post)
                raise_exc("ValueError",
                          "not enough values to unpack (expected at least " +
                              std::to_string(pre + post) + ")",
                          target->pos);
            std::size_t i = 0;
            for (; i < pre; ++i)
                bind_unpack(target->parts[i].get(), items[i], f);
            std::vector<PyRef> starred;
            for (; i < items.size() - post; ++i)
                starred.push_back(items[i]);
            auto star_target = static_cast<const ast_expr*>(
                target->parts[star_idx].get())->base.get();
            bind_unpack(star_target, py_list(std::move(starred)), f);
            for (std::size_t j = 0; j < post; ++j, ++i)
                bind_unpack(target->parts[star_idx + 1 + j].get(), items[i], f);
        }
        return;
    }
    case et::star_:
        bind_unpack(target->base.get(), value, f);
        return;
    case et::literal:
        raise_exc("SyntaxError", "cannot assign to literal", target->pos);
    default:
        raise_exc("SyntaxError", "cannot assign to this target", target->pos);
    }
}

// exec_import lives in pymini_imports.cpp helpers
void interpreter::exec_import(const ast_stmt* s, frame& f) {
    if (s->tag == st::import) {
        for (const auto& [dotted, alias] : s->imports) {
            PyRef leaf = import_dotted(dotted, &f, 0);
            if (alias.empty()) {
                // `import a.b.c` binds `a` (top package)
                const auto dot = dotted.find('.');
                if (dot == std::string::npos) {
                    scope_set(f, dotted, leaf);
                } else {
                    PyRef top = find_loaded(dotted.substr(0, dot));
                    scope_set(f, dotted.substr(0, dot),
                              top ? top : leaf);
                }
            } else {
                scope_set(f, alias, leaf);
            }
        }
        return;
    }
    // from X import a as b
    PyRef m = import_dotted(s->from_module, &f, s->from_level);
    if (as_module(m) == nullptr)
        raise_exc("ImportError", "import target is not a module", s->pos);
    for (const auto& [name, alias] : s->imports) {
        if (name == "*") {
            // star import: copy public names
            auto* md = as_dict(as_module(m)->dict);
            if (md != nullptr)
                for (auto& [k, v] : md->items) {
                    if (auto* ks = as_str(k)) {
                        if (!ks->v.empty() && ks->v.front() != '_')
                            scope_set(f, ks->v, v);
                    }
                }
            continue;
        }
        PyRef v = getattr(m, name);
        if (!v) {
            // submodule fallback: package.name
            std::string full = s->from_module.empty() ? name
                                                      : s->from_module + "." + name;
            try {
                v = import_dotted(full, &f, s->from_level);
            } catch (const sig_raise&) {
                raise_exc("ImportError",
                          "cannot import name '" + name + "' from '" +
                              s->from_module + "'",
                          s->pos);
            }
        }
        scope_set(f, alias.empty() ? name : alias, std::move(v));
    }
}

// with-item recursion: with_items[idx] entered, then tail items or body.
void interpreter::exec_with_item(const ast_stmt* s, frame& f,
                                 std::size_t idx) {
    if (idx >= s->with_items.size()) {
        exec_body(s->body, f);
        return;
    }
    PyRef ctx_mgr = eval(s->with_items[idx].ctx_expr.get(), f);
    PyRef enter_fn = getattr(ctx_mgr, "__enter__");
    PyRef exit_fn = getattr(ctx_mgr, "__exit__");
    if (!enter_fn || !exit_fn)
        raise_exc("TypeError",
                  "context manager object lacks __enter__/__exit__", s->pos);
    PyRef entered = call0(enter_fn, s->pos);
    if (s->with_items[idx].target)
        bind_unpack(s->with_items[idx].target.get(), entered, f);
    try {
        exec_with_item(s, f, idx + 1);
    } catch (const sig_raise& sig) {
        // __exit__(exc_type, exc_value, traceback) — truthy suppresses
        PyRef type_obj;
        if (const auto* e = as_exc(sig.exc)) {
            type_obj = dict_get(as_dict(builtins_dict), py_str(e->type_name));
        } else if (const auto* in = as_inst(sig.exc)) {
            type_obj = in->klass;
        }
        if (!type_obj)
            type_obj = sig.exc;
        PyRef r =
            call(exit_fn, py_args{{type_obj, sig.exc, py_none()}}, s->pos);
        if (!truthy(r))
            throw;
        return;
    } catch (...) {
        // break/continue/return/native errors still trigger __exit__
        call(exit_fn, py_args{{py_none(), py_none(), py_none()}}, s->pos);
        throw;
    }
    call(exit_fn, py_args{{py_none(), py_none(), py_none()}}, s->pos);
}

// eval_comprehension — separate scopes per Python 3.
PyRef interpreter::eval_comprehension(const ast_expr* e, frame& f) {
    PyRef result;
    PyListObj* out_list = nullptr;
    PySetObj* out_set = nullptr;
    PyDictObj* out_dict = nullptr;
    if (e->name == "dict") {
        result = py_dict();
        out_dict = as_dict(result);
    } else if (e->name == "set") {
        result = py_set();
        out_set = as_set(result);
    } else {
        result = py_list();
        out_list = as_list(result);
    }

    frame cf;
    cf.locals = py_dict();            // comprehension scope — vars don't leak
    cf.globals = f.globals;
    if (!f.in_class_body && f.locals != f.globals)
        cf.captured.push_back(f.locals);
    for (const auto& cap : f.captured)
        cf.captured.push_back(cap);
    cf.fn_name = "<comprehension>";
    cf.file = f.file;
    cf.caller = cur_frame;

    std::function<void(std::size_t)> run = [&](std::size_t idx) {
        if (idx >= e->generators.size()) {
            PyRef v = eval(e->base.get(), cf);
            if (out_list)
                out_list->v.push_back(v);
            else if (out_set)
                set_add(*this, out_set->items, v);
            else if (out_dict) {
                auto* kv = as_tuple(v);
                if (kv && kv->v.size() == 2)
                    dict_set(*this, out_dict, kv->v[0], kv->v[1]);
            }
            return;
        }
        const auto& cl = e->generators[idx];
        // the FIRST comprehension iterable is evaluated in the ENCLOSING
        // scope (Python semantics — class bodies rely on it); later clauses
        // evaluate inside the comprehension frame.
        PyRef iterable = eval(cl.iter.get(), idx == 0 ? f : cf);
        for_each(iterable, [&](PyRef item) {
            bind_unpack(cl.target.get(), item, cf);
            for (const auto& cond : cl.ifs)
                if (!truthy(eval(cond.get(), cf)))
                    return true;
            run(idx + 1);
            return true;
        });
    };
    run(0);
    return result;
}

// set_add helper used above (dedup by py_eq)
void set_add(interpreter& i, std::vector<PyRef>& items, const PyRef& v) {
    for (const auto& el : items)
        if (py_eq(i, el, v))
            return;
    items.push_back(v);
}

} // namespace sao::plugins::pymini
