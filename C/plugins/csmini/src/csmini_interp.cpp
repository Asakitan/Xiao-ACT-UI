// csmini_interp.cpp — interpreter object + tree-walk evaluator (statements +
// expression dispatch; operators live in csmini_ops.cpp, call machinery in
// csmini_calls.cpp).
#include "csmini_interp.h"

#include "csmini_parser.h"

#include <unordered_set>

namespace sao::plugins::csmini {
namespace {

CsDictObj* g(const CsRef& d) { return as_dict(d); }

bool is_type_start_token(tok_kind k) {
    return k == tok_kind::name || k == tok_kind::keyword;
}

// assignment-operator tag set — mirrors parser.cpp's is_assign_op so the
// evaluator can fold `binop` nodes that carry compound assigns (`+=` etc.
// arrive as binop when produced through the expression chain or `??=`).
bool is_assign_op(tok_kind k) {
    switch (k) {
    case tok_kind::assign:
    case tok_kind::plus_eq:
    case tok_kind::minus_eq:
    case tok_kind::star_eq:
    case tok_kind::slash_eq:
    case tok_kind::percent_eq:
    case tok_kind::amp_eq:
    case tok_kind::pipe_eq:
    case tok_kind::caret_eq:
    case tok_kind::lshift_eq:
    case tok_kind::rshift_eq:
    case tok_kind::nullcoalesce_eq:
        return true;
    default:
        return false;
    }
}

} // namespace

interpreter::interpreter(config c) : cfg(std::move(c)) {
    globals = cs_dict();
    global_scope_ = alloc_scope(nullptr);
    root_frame_.scope = global_scope_;
    root_frame_.fn_name = "<module>";
    root_frame_.file = "";
    cur_frame = &root_frame_;
}
interpreter::~interpreter() {
    // break value cycles (globals dict ↔ functions/classes) before pool death
    if (auto* d = as_dict(globals))
        d->items.clear();
    if (global_scope_)
        global_scope_->vars.clear();
}

cs_scope* interpreter::alloc_scope(cs_scope* parent) {
    scope_pool_.push_back(std::make_unique<cs_scope>());
    cs_scope* s = scope_pool_.back().get();
    s->parent = parent;
    return s;
}

// ── module execution ──────────────────────────────────────────────────────
CsRef interpreter::exec_module_source(const std::string& logical_name,
                                      const std::string& file_utf8,
                                      std::string_view source) {
    feature_flags flags{};
    auto prog =
        std::make_shared<ast_program>(parse_source(source, file_utf8, &flags));
    for (const auto& f : flags.unsupported)
        features.insert(f);
    cs_guard g_(*this);
    frame* prev = cur_frame;
    root_frame_.file = file_utf8;
    cur_frame = &root_frame_;
    try {
        // hoist class declarations first (top-level order still matters for
        // static field initializers — evaluate class bodies in source order,
        // but register names upfront so forward references resolve).
        for (const ast_class& cls : prog->classes) {
            auto cobj = std::make_shared<CsClassObj>();
            cobj->name = cls.name;
            cobj->ns = cls.ns;
            cobj->attrs = cs_dict();
            dict_set(g(globals), cs_str(cls.name), cobj);
            if (!cls.ns.empty())
                dict_set(g(globals), cs_str(cls.ns + "." + cls.name), cobj);
        }
        for (const ast_class& cls : prog->classes) {
            CsRef cobj = dict_get(g(globals), cs_str(cls.name));
            auto* co = as_class(cobj);
            co->anchor = prog;
            for (const ast_member& m : cls.members) {
                const std::string key = m.name;
                if (m.kind == member_kind::ctor) {
                    // ctor table keyed by '@ctor:N|T1|T2' — arg-count bucket
                    // plus param-type signature for same-arity overloads.
                    auto fn = std::make_shared<CsFuncObj>();
                    fn->name = cls.name;
                    fn->is_ctor = true;
                    fn->owner_class = co;
                    fn->anchor = prog;
                    fn->params.reserve(m.params.size());
                    for (const ast_param& pa : m.params) {
                        cs_param p2{pa.type, pa.name, pa.default_value != nullptr};
                        fn->params.push_back(std::move(p2));
                        fn->default_exprs.push_back(pa.default_value.get());
                    }
                    fn->body = std::make_shared<std::vector<stmt_ptr>>(m.body);
                    // '@ctor:N|T1|T2' — arity + param-type signature keeps
                    // same-arity overloads (Foo(int) / Foo(string)) distinct.
                    std::string slot = "@ctor:" + std::to_string(m.params.size());
                    for (const ast_param& pa : m.params)
                        slot += "|" + pa.type;
                    dict_set(g(co->attrs), cs_str(slot), fn);
                    continue;
                }
                if (m.kind == member_kind::method) {
                    auto fn = std::make_shared<CsFuncObj>();
                    fn->name = m.name;
                    fn->is_static = m.is_static;
                    fn->owner_class = co;
                    fn->anchor = prog;
                    for (const ast_param& pa : m.params) {
                        cs_param p2{pa.type, pa.name, pa.default_value != nullptr};
                        fn->params.push_back(std::move(p2));
                        fn->default_exprs.push_back(pa.default_value.get());
                    }
                    if (m.expr_body) {
                        auto ret = std::make_shared<ast_stmt>();
                        ret->tag = st::return_;
                        ret->pos = m.expr_body->pos;
                        ret->value =
                            std::move(const_cast<ast_member&>(m).expr_body);
                        fn->expr_body = ret;
                    } else {
                        fn->body = std::make_shared<std::vector<stmt_ptr>>(m.body);
                    }
                    dict_set(g(co->attrs), cs_str(key), fn);
                    if (!m.is_static)
                        co->inst_only.insert(key);
                    continue;
                }
                // field
                if (m.is_static || m.is_const) {
                    // evaluate static field initializer now (module scope)
                    CsRef v = cs_null();
                    if (m.init) {
                        frame f;
                        f.scope = global_scope_;
                        f.class_ref = cobj;
                        f.fn_name = "<static-init>";
                        f.file = file_utf8;
                        v = eval(m.init.get(), f);
                    }
                    dict_set(g(co->attrs), cs_str(key), v);
                } else {
                    co->inst_only.insert(key);
                    co->inst_field_inits.emplace_back(key, m.init.get());
                }
            }
        }
    } catch (...) {
        cur_frame = prev;
        throw;
    }
    cur_frame = prev;
    return globals;
}

CsRef interpreter::find_loaded(const std::string& name) {
    return dict_get(g(globals), cs_str(name));
}

// ── scope ─────────────────────────────────────────────────────────────────
CsRef interpreter::scope_get(const frame& f, const std::string& name,
                             bool* found) {
    if (found)
        *found = true;
    for (cs_scope* s = f.scope; s; s = s->parent) {
        auto it = s->vars.find(name);
        if (it != s->vars.end())
            return it->second;
    }
    // this members
    if (f.this_ref) {
        bool hit = false;
        if (CsRef v = instance_member(f.this_ref, name, &hit); hit)
            return v;
    }
    // class statics / methods via owner class
    if (auto* co = f.class_ref ? as_class(f.class_ref) : nullptr) {
        if (CsRef v = dict_get(g(co->attrs), cs_str(name))) {
            if (co->inst_only.count(name)) {
                if (f.this_ref)
                    return v;
                if (found)
                    *found = false;
                return nullptr;
            }
            // sibling instance method resolved bare inside an instance frame →
            // bind `this` so `Helper()` works unqualified.
            if (v->kind == cs_kind::func && f.this_ref &&
                !as_func(v)->is_static)
                return std::make_shared<CsBoundMethodObj>(f.this_ref, v);
            return v;
        }
    }
    // globals / builtins
    if (CsRef v = dict_get(g(globals), cs_str(name)))
        return v;
    if (found)
        *found = false;
    return nullptr;
}

void interpreter::scope_set(frame& f, const std::string& name, CsRef value) {
    f.scope->vars[name] = std::move(value);
}

bool interpreter::scope_assign(frame& f, const std::string& name,
                               CsRef value) {
    for (cs_scope* s = f.scope; s; s = s->parent) {
        auto it = s->vars.find(name);
        if (it != s->vars.end()) {
            it->second = std::move(value);
            return true;
        }
    }
    return false;
}

// ── attributes ────────────────────────────────────────────────────────────
CsRef interpreter::class_static(const CsRef& klass, const std::string& name,
                                bool* found) {
    if (found)
        *found = false;
    auto* co = as_class(klass);
    if (!co)
        return nullptr;
    if (co->inst_only.count(name)) {
        // static member access to an instance member → null + !found so
        // callers can surface a better error.
        return nullptr;
    }
    if (CsRef v = dict_get(g(co->attrs), cs_str(name))) {
        if (found)
            *found = true;
        return v;
    }
    return nullptr;
}

CsRef interpreter::instance_member(const CsRef& obj, const std::string& name,
                                   bool* found) {
    if (found)
        *found = false;
    auto* inst = as_inst(obj);
    if (!inst)
        return nullptr;
    if (auto* d = as_dict(inst->attrs)) {
        if (CsRef v = dict_get(d, cs_str(name))) {
            if (found)
                *found = true;
            return v;
        }
    }
    auto* co = as_class(inst->klass);
    if (co) {
        if (CsRef v = dict_get(g(co->attrs), cs_str(name))) {
            if (v->kind == cs_kind::func && !as_func(v)->is_static) {
                if (found)
                    *found = true;
                return std::make_shared<CsBoundMethodObj>(obj, v);
            }
            if (v->kind == cs_kind::func && as_func(v)->is_static) {
                if (found)
                    *found = true;
                return v;
            }
            if (!co->inst_only.count(name)) {
                if (found)
                    *found = true;
                return v;
            }
            if (found)
                *found = true;
            return v;
        }
    }
    return nullptr;
}

CsRef interpreter::getattr(const CsRef& obj, const std::string& name,
                           bool* found) {
    if (found)
        *found = false;
    if (!obj)
        return nullptr;
    switch (obj->kind) {
    case cs_kind::native_obj: {
        auto* n = as_native(obj);
        CsRef v = n->ci ? dict_get_ci(g(n->members), cs_str(name))
                        : dict_get(g(n->members), cs_str(name));
        if (v && found)
            *found = true;
        return v;
    }
    case cs_kind::class_:
        return class_static(obj, name, found);
    case cs_kind::instance:
        return instance_member(obj, name, found);
    case cs_kind::exception_: {
        auto* e = as_exc(obj);
        if (name == "Message" || name == "message") {
            if (found)
                *found = true;
            return cs_str(e->message);
        }
        if (name == "ToString") {
            if (found)
                *found = true;
            return cs_builtin("Exception.ToString",
                              [r = obj](interpreter& i, const cs_args&) {
                                  return cs_str(cs_to_str(i, r));
                              });
        }
        return nullptr;
    }
    default:
        // string/array/dict/numeric member tables
        return csmini_value_member(*this, obj, name, found);
    }
}

bool interpreter::setattr(CsRef obj, const std::string& name, CsRef value) {
    if (!obj)
        return false;
    if (auto* inst = as_inst(obj)) {
        dict_set(g(inst->attrs), cs_str(name), std::move(value));
        return true;
    }
    if (auto* co = as_class(obj)) {
        dict_set(g(co->attrs), cs_str(name), std::move(value));
        return true;
    }
    if (auto* n = as_native(obj)) {
        dict_set(g(n->members), cs_str(name), std::move(value));
        return true;
    }
    if (auto* d = as_dict(obj)) {
        dict_set(d, cs_str(name), std::move(value));
        return true;
    }
    return false;
}

bool interpreter::hasattr(const CsRef& obj, const std::string& name) {
    bool found = false;
    (void)getattr(obj, name, &found);
    return found;
}

// ── instantiation ─────────────────────────────────────────────────────────
CsRef interpreter::instantiate(CsRef klass, const cs_args& args, src_pos pos) {
    auto* co = as_class(klass);
    if (!co)
        raise_exc("TypeError", "not a class: " + cs_to_str(*this, klass), pos);
    auto inst =
        std::make_shared<CsInstanceObj>(klass, cs_dict());
    // instance field initializers (source order)
    {
        frame f;
        f.scope = alloc_scope(global_scope_);
        f.this_ref = inst;
        f.class_ref = klass;
        f.fn_name = co->name + ".<init>";
        f.file = cur_frame ? cur_frame->file : "";
        for (const auto& [fname, init_expr] : co->inst_field_inits) {
            CsRef v = init_expr ? eval(init_expr, f) : cs_null();
            dict_set(g(inst->attrs), cs_str(fname), std::move(v));
        }
    }
    // pick a ctor by arg-count fit (declaration order per bucket key),
    // preferring the overload whose param-type text matches the arg kinds
    // so Foo(int) / Foo(string) same-arity overloads resolve correctly.
    CsFuncObj* ctor = nullptr;
    CsFuncObj* fallback = nullptr;
    for (const auto& [slot, fv] : g(co->attrs)->items) {
        auto* ks = as_str(slot);
        if (!ks || ks->v.rfind("@ctor:", 0) != 0)
            continue;
        auto* cf = as_func(fv);
        if (!cf)
            continue;
        std::size_t required = 0;
        for (const auto& pa : cf->params)
            if (!pa.has_default)
                ++required;
        if (args.size() < required || args.size() > cf->params.size())
            continue;
        if (!fallback)
            fallback = cf;
        bool type_ok = true;
        for (std::size_t k = 0; k < args.size(); ++k) {
            const std::string& pt = cf->params[k].type;
            if (pt.empty() || pt == "var" || pt == "object" ||
                pt == "dynamic" || pt == "System.Object")
                continue;
            const CsRef& av = args.pos[k];
            const cs_kind ak = av ? av->kind : cs_kind::null_;
            if (ak == cs_kind::null_)
                continue;           // null fits any reference param
            auto intish = [](const std::string& t) {
                return t == "int" || t == "long" || t == "short" ||
                       t == "byte" || t == "sbyte" || t == "ushort" ||
                       t == "uint" || t == "ulong" || t == "nint" ||
                       t == "nuint" || t == "char" || t == "Int32" ||
                       t == "Int64" || t == "Int16" || t == "Byte" ||
                       t == "Char";
            };
            auto floatish = [](const std::string& t) {
                return t == "double" || t == "float" || t == "decimal" ||
                       t == "Double" || t == "Single" || t == "Decimal";
            };
            if (intish(pt)) {
                if (ak == cs_kind::integer || ak == cs_kind::char_)
                    continue;
                type_ok = false;
                break;
            }
            if (floatish(pt)) {
                if (ak == cs_kind::number || ak == cs_kind::integer)
                    continue;
                type_ok = false;
                break;
            }
            if (pt == "string" || pt == "String") {
                if (ak == cs_kind::string)
                    continue;
                type_ok = false;
                break;
            }
            if (pt == "bool" || pt == "Boolean") {
                if (ak == cs_kind::boolean)
                    continue;
                type_ok = false;
                break;
            }
            if (ak == cs_kind::instance) {
                auto* ico = as_class(as_inst(av)->klass);
                if (ico) {
                    const std::string qn =
                        ico->ns.empty() ? ico->name
                                        : ico->ns + "." + ico->name;
                    if (pt == ico->name || pt == qn)
                        continue;
                }
                type_ok = false;
                break;
            }
            type_ok = false;
            break;
        }
        if (type_ok) {
            ctor = cf;
            break;
        }
    }
    if (!ctor)
        ctor = fallback;
    if (ctor) {
        frame f;
        f.scope = alloc_scope(global_scope_);
        f.this_ref = inst;
        f.class_ref = klass;
        f.fn_name = co->name + "." + co->name;
        f.file = cur_frame ? cur_frame->file : "";
        f.caller = cur_frame;
        bind_param_frame(f, ctor, args, pos);
        try {
            exec_body(*ctor->body, f);
        } catch (const sig_return&) {
            // ctor return is void — swallow
        }
    }
    return inst;
}

// ── exceptions ────────────────────────────────────────────────────────────
void interpreter::raise_exc(const std::string& type, const std::string& msg,
                            src_pos pos) {
    auto e = cs_exc(type, msg);
    auto* eo = as_exc(e);
    for (frame* f = cur_frame; f; f = f->caller) {
        std::string loc = f->file + ":" + std::to_string(f->line) +
                          " in " + f->fn_name;
        eo->trace.push_back(std::move(loc));
    }
    (void)pos;
    throw sig_raise(std::move(e));
}
void interpreter::raise_obj(CsRef exc, src_pos pos) {
    (void)pos;
    throw sig_raise(std::move(exc));
}
bool interpreter::exc_matches(const CsRef& exc_val,
                              const std::string& type_text) {
    if (type_text.empty())
        return true;                        // bare `catch {}` / `catch`
    auto* e = as_exc(exc_val);
    if (!e)
        return false;
    const std::string& tn = type_text;
    if (e->type_name == tn)
        return true;
    // coarse match: "Exception"/"System.Exception" catch-all parents and
    // ns-stripped comparisons.
    if (tn == "Exception" || tn == "System.Exception" || tn == "object")
        return true;
    const auto dot = tn.rfind('.');
    if (dot != std::string::npos && tn.substr(dot + 1) == e->type_name)
        return true;
    return false;
}
void interpreter::record_feature(const std::string& f) { features.insert(f); }

// ── statement execution ───────────────────────────────────────────────────
void interpreter::exec_body(const std::vector<stmt_ptr>& body, frame& f) {
    cs_scope* saved = f.scope;
    try {
        for (const auto& s : body) {
            f.line = s ? s->pos.line : 0;
            exec(s.get(), f);
        }
    } catch (...) {
        f.scope = saved;
        throw;
    }
    f.scope = saved;
}

void interpreter::exec(const ast_stmt* s, frame& f) {
    if (!s)
        return;
    switch (s->tag) {
    case st::block: {
        cs_scope* inner = alloc_scope(f.scope);
        cs_scope* saved = f.scope;
        f.scope = inner;
        try {
            for (const auto& x : s->body) {
                f.line = x ? x->pos.line : 0;
                exec(x.get(), f);
            }
        } catch (...) {
            f.scope = saved;
            throw;
        }
        f.scope = saved;                // scopes are pooled — no free needed
        return;
    }
    case st::expr_stmt: {
        if (s->value)
            (void)eval(s->value.get(), f);
        return;
    }
    case st::local_decl: {
        for (const auto& [n, init] : s->names) {
            CsRef v = init ? eval(init.get(), f) : cs_null();
            scope_set(f, n, std::move(v));
        }
        return;
    }
    case st::assign: {
        CsRef rhs = s->value2 ? eval(s->value2.get(), f) : cs_null();
        if (s->aug_op != tok_kind::eof_ && s->aug_op != tok_kind::assign) {
            CsRef cur_v = eval(s->value.get(), f);
            rhs = binary(s->aug_op, cur_v, rhs, s->pos);
        }
        (void)assign_target(s->value.get(), rhs, f);
        return;
    }
    case st::incdec: {
        (void)incdec_target(s->value.get(),
                            s->aug_op == tok_kind::minus2 ? -1 : 1, s->post,
                            f);
        return;
    }
    case st::if_: {
        if (is_true(eval(s->value.get(), f))) {
            for (const auto& x : s->body)
                exec(x.get(), f);
        } else {
            for (const auto& x : s->orelse)
                exec(x.get(), f);
        }
        return;
    }
    case st::for_: {
        cs_scope* inner = alloc_scope(f.scope);
        cs_scope* saved = f.scope;
        f.scope = inner;
        try {
            for (const auto& x : s->init)
                exec(x.get(), f);
            while (true) {
                if (s->value && !is_true(eval(s->value.get(), f)))
                    break;
                try {
                    for (const auto& x : s->body)
                        exec(x.get(), f);
                } catch (const sig_continue&) {
                } catch (const sig_break&) {
                    break;
                }
                for (const auto& x : s->iter)
                    exec(x.get(), f);
            }
        } catch (...) {
            f.scope = saved;
            throw;
        }
        f.scope = saved;
        return;
    }
    case st::foreach_: {
        CsRef it = eval(s->value.get(), f);
        auto* arr = as_array(it);
        auto* d = as_dict(it);
        if (!arr && !d) {
            // try members: IEnumerable-style objects expose GetEnumerator —
            // subset: strings iterate chars.
            if (auto* str = as_str(it)) {
                arr = nullptr;
                cs_scope* inner = alloc_scope(f.scope);
                cs_scope* saved = f.scope;
                f.scope = inner;
                try {
                    for (std::size_t k = 0; k < str->v.size(); ++k) {
                        // utf8-aware would decode; chars iterate bytewise
                        // only for ascii here (subset).
                        scope_set(f, s->name,
                                  cs_char(static_cast<unsigned char>(str->v[k])));
                        try {
                            for (const auto& x : s->body)
                                exec(x.get(), f);
                        } catch (const sig_continue&) {
                            continue;
                        } catch (const sig_break&) {
                            break;
                        }
                    }
                } catch (...) {
                    f.scope = saved;
                    throw;
                }
                f.scope = saved;
                return;
            }
            raise_exc("InvalidOperationException",
                      "foreach over non-enumerable " +
                          std::string(cs_type_name(it)),
                      s->pos);
        }
        // snapshot the container — the loop body may mutate the live
        // array/dict (Add/Remove/indexer set) which would invalidate the
        // iteration otherwise.
        cs_scope* inner = alloc_scope(f.scope);
        cs_scope* saved = f.scope;
        f.scope = inner;
        try {
            if (arr) {
                const std::vector<CsRef> snapshot = arr->v;
                for (const CsRef& x : snapshot) {
                    scope_set(f, s->name, x);
                    try {
                        for (const auto& b : s->body)
                            exec(b.get(), f);
                    } catch (const sig_continue&) {
                        continue;
                    } catch (const sig_break&) {
                        break;
                    }
                }
            } else {
                const std::vector<std::pair<CsRef, CsRef>> snapshot = d->items;
                for (const auto& [k, v] : snapshot) {
                    // KeyValuePair<K,V> → dict {Key,Value}
                    auto kv = cs_dict();
                    dict_set(g(kv), cs_str("Key"), k);
                    dict_set(g(kv), cs_str("Value"), v);
                    scope_set(f, s->name, kv);
                    try {
                        for (const auto& b : s->body)
                            exec(b.get(), f);
                    } catch (const sig_continue&) {
                        continue;
                    } catch (const sig_break&) {
                        break;
                    }
                }
            }
        } catch (...) {
            f.scope = saved;
            throw;
        }
        f.scope = saved;
        return;
    }
    case st::while_: {
        while (is_true(eval(s->value.get(), f))) {
            try {
                for (const auto& x : s->body)
                    exec(x.get(), f);
            } catch (const sig_continue&) {
                continue;
            } catch (const sig_break&) {
                break;
            }
        }
        return;
    }
    case st::do_: {
        do {
            try {
                for (const auto& x : s->body)
                    exec(x.get(), f);
            } catch (const sig_continue&) {
            } catch (const sig_break&) {
                return;
            }
        } while (is_true(eval(s->value.get(), f)));
        return;
    }
    case st::return_: {
        throw sig_return(s->value ? eval(s->value.get(), f) : cs_null());
    }
    case st::break_:
        throw sig_break{};
    case st::continue_:
        throw sig_continue{};
    case st::throw_: {
        if (s->value) {
            CsRef v = eval(s->value.get(), f);
            if (v && v->kind == cs_kind::exception_)
                throw sig_raise(std::move(v));
            // record the script type so `catch (MyErr)` still matches a
            // thrown class instance — the message keeps the value text.
            std::string tn = "Exception";
            if (auto* inst = as_inst(v))
                if (auto* co = as_class(inst->klass))
                    tn = co->name;
            throw sig_raise(cs_exc(tn, cs_to_str(*this, v)));
        }
        // bare `throw;` — rethrow the currently-active caught exception
        // unchanged (the catch arm pushed it on exc_stack_).
        if (!exc_stack_.empty())
            throw sig_raise(exc_stack_.back());
        throw sig_raise(cs_exc("Exception", "rethrow"));
    }
    case st::try_: {
        // `finally` runs on EVERY exit path — normal fall-through, sig_raise
        // (matched or not), sig_return, sig_break, sig_continue, and
        // exceptions thrown from the catch arm itself.
        try {
            for (const auto& x : s->body)
                exec(x.get(), f);
        } catch (const sig_raise& sig) {
            const catch_arm* matched = nullptr;
            for (const auto& arm : s->catches) {
                if (!exc_matches(sig.exc, arm.type_name))
                    continue;
                if (arm.filter) {
                    // `when (expr)` — eval with the catch var bound; filter
                    // exceptions treat the arm as non-matching (C# rule).
                    cs_scope* fscope = alloc_scope(f.scope);
                    cs_scope* fsaved = f.scope;
                    f.scope = fscope;
                    if (!arm.name.empty())
                        scope_set(f, arm.name, sig.exc);
                    bool ok = false;
                    try {
                        ok = is_true(eval(arm.filter.get(), f));
                    } catch (...) {
                        ok = false;
                    }
                    f.scope = fsaved;
                    if (!ok)
                        continue;
                }
                matched = &arm;
                break;
            }
            if (!matched) {
                for (const auto& x : s->final)
                    exec(x.get(), f);
                throw;
            }
            cs_scope* inner = alloc_scope(f.scope);
            cs_scope* saved = f.scope;
            f.scope = inner;
            exc_stack_.push_back(sig.exc);   // for bare `throw;` in the arm
            try {
                if (!matched->name.empty())
                    scope_set(f, matched->name, sig.exc);
                for (const auto& x : matched->body)
                    exec(x.get(), f);
            } catch (...) {
                exc_stack_.pop_back();
                f.scope = saved;
                for (const auto& x : s->final)
                    exec(x.get(), f);
                throw;
            }
            exc_stack_.pop_back();
            f.scope = saved;
        } catch (...) {
            // sig_return / sig_break / sig_continue / foreign errors
            for (const auto& x : s->final)
                exec(x.get(), f);
            throw;
        }
        for (const auto& x : s->final)
            exec(x.get(), f);
        return;
    }
    default:
        return;
    }
}

// ── expression evaluation ─────────────────────────────────────────────────
CsRef interpreter::eval(const ast_expr* e, frame& f) {
    if (!e)
        return cs_null();
    f.line = e->pos.line;
    switch (e->tag) {
    case et::literal:
        return e->const_value;
    case et::name: {
        bool found = false;
        CsRef v = scope_get(f, e->name, &found);
        if (!found)
            raise_exc("NameError", "unknown identifier '" + e->name + "'",
                      e->pos);
        return v;
    }
    case et::this_:
        if (f.this_ref)
            return f.this_ref;
        if (f.class_ref)
            return f.class_ref;
        return cs_null();
    case et::member: {
        CsRef base = eval(e->base.get(), f);
        if (cs_is_null(base) && e->post)
            return cs_null();                    // ?. null-conditional
        bool found = false;
        CsRef v = getattr(base, e->name, &found);
        if (!found) {
            // unknown member → null + note (C# would fail at compile time;
            // the subset keeps runtime behavior graceful for ctx natives)
            if (auto* n = as_native(base); n && n->ci)
                raise_exc("MissingMemberException",
                          "no such member '" + e->name + "' on " + n->name,
                          e->pos);
            raise_exc("MissingMemberException",
                      "no such member '" + e->name + "' on " +
                          std::string(cs_type_name(base)),
                      e->pos);
        }
        return v;
    }
    case et::index: {
        CsRef base = eval(e->base.get(), f);
        if (cs_is_null(base) && e->post)
            return cs_null();
        CsRef ix = eval(e->index.get(), f);
        return subscript_get(base, ix, e->pos);
    }
    case et::call: {
        CsRef fn = eval(e->base.get(), f);
        if (cs_is_null(fn) && e->post)
            return cs_null();                    // ?.-chain: skip call + args
        cs_args a;
        a.pos.reserve(e->call_args.size());
        for (const auto& arg : e->call_args)
            a.pos.push_back(eval(arg.get(), f));
        return call(fn, a, e->pos);
    }
    case et::binop: {
        // assignment/compound handled at stmt level — here pure operators
        if (is_assign_op(e->op)) {
            if (e->op == tok_kind::nullcoalesce_eq) {
                // `x ??= v` — lazy: rhs only evaluates when lhs is null
                CsRef cur_v = eval(e->base.get(), f);
                if (!cs_is_null(cur_v))
                    return cur_v;
                CsRef rhs = eval(e->parts[0].get(), f);
                return assign_target(e->base.get(), rhs, f);
            }
            CsRef rhs = eval(e->parts[0].get(), f);
            if (e->op != tok_kind::assign) {
                CsRef cur_v = eval(e->base.get(), f);
                rhs = binary(e->op, cur_v, rhs, e->pos);
            }
            return assign_target(e->base.get(), rhs, f);
        }
        if (e->op == tok_kind::nullcoalesce) {
            CsRef l = eval(e->base.get(), f);
            if (!cs_is_null(l))
                return l;
            return eval(e->parts[0].get(), f);
        }
        if (e->op == tok_kind::and2) {
            CsRef l = eval(e->base.get(), f);
            if (!is_true(l))
                return cs_false();
            CsRef r = eval(e->parts[0].get(), f);
            return cs_bool(is_true(r));
        }
        if (e->op == tok_kind::or2) {
            CsRef l = eval(e->base.get(), f);
            if (is_true(l))
                return cs_true();
            CsRef r = eval(e->parts[0].get(), f);
            return cs_bool(is_true(r));
        }
        CsRef l = eval(e->base.get(), f);
        CsRef r = eval(e->parts[0].get(), f);
        return binary(e->op, l, r, e->pos);
    }
    case et::unop: {
        if (e->op == tok_kind::plus2 || e->op == tok_kind::minus2) {
            return incdec_target(e->base.get(),
                                 e->op == tok_kind::minus2 ? -1 : 1, e->post,
                                 f);
        }
        CsRef v = eval(e->base.get(), f);
        return unary(e->op, v, e->pos);
    }
    case et::ternary: {
        if (is_true(eval(e->index.get(), f)))
            return eval(e->base.get(), f);
        return eval(e->orelse.get(), f);
    }
    case et::interp: {
        std::string out;
        for (const auto& part : e->parts) {
            CsRef v = eval(part.get(), f);
            out += cs_to_str(*this, v);
        }
        return cs_str(out);
    }
    case et::cast: {
        CsRef v = eval(e->base.get(), f);
        return cast_value(e->name, v, e->pos);
    }
    case et::typeof_: {
        // placeholder-safe System.Type marker (UNIMPLEMENTED-safe per subset)
        auto t = cs_native("System.Type", false);
        dict_set(g(as_native(t)->members), cs_str("Name"), cs_str(e->name));
        dict_set(g(as_native(t)->members), cs_str("FullName"),
                 cs_str(e->name));
        return t;
    }
    case et::default_: {
        if (e->name == "int" || e->name == "long" || e->name == "uint" ||
            e->name == "ulong" || e->name == "short" || e->name == "ushort" ||
            e->name == "byte" || e->name == "sbyte" || e->name == "char")
            return cs_int(0);
        if (e->name == "double" || e->name == "float" || e->name == "decimal")
            return cs_float(0.0);
        if (e->name == "bool")
            return cs_false();
        return cs_null();
    }
    case et::new_expr: {
        // sized array `new T[n]`
        if (e->index) {
            bool ok = false;
            const int64_t n = cs_to_int(eval(e->index.get(), f), &ok);
            auto arr = cs_array();
            if (ok && n > 0)
                for (int64_t k = 0; k < n; ++k)
                    as_array(arr)->v.push_back(cs_null());
            return arr;
        }
        CsRef obj;
        const std::string& tn = e->name;
        if (tn.empty() || tn == "[]") {
            obj = cs_array();
        } else {
            obj = new_instance_named(tn, e->call_args, e->pos, f);
        }
        // initializer entries:
        //   slot ""     → collection add (Add / array push / dict pair)
        //   slot "@key" → pending indexer key
        //   slot "@val" → indexer value (obj[key] = val)
        //   slot name   → member init obj.name = value
        CsRef pending_key;
        for (std::size_t k = 0; k < e->parts.size(); ++k) {
            const std::string slot =
                k < e->init_names.size() ? e->init_names[k] : "";
            CsRef v = eval(e->parts[k].get(), f);
            if (slot == "@key") {
                pending_key = v;
                continue;
            }
            if (slot == "@val") {
                if (auto* d = as_dict(obj)) {
                    dict_set(d, pending_key, v);
                } else if (auto* arr = as_array(obj)) {
                    bool ok = false;
                    const int64_t ix = cs_to_int(pending_key, &ok);
                    if (ok && ix >= 0) {
                        while (arr->v.size() <= static_cast<std::size_t>(ix))
                            arr->v.push_back(cs_null());
                        arr->v[static_cast<std::size_t>(ix)] = v;
                    }
                } else {
                    subscript_set(obj, pending_key, v, e->pos);
                }
                continue;
            }
            if (slot.empty()) {
                if (auto* arr = as_array(obj)) {
                    arr->v.push_back(v);
                } else if (auto* d = as_dict(obj)) {
                    // `{ {"k",v} }` pair-style init: 2-seq → k=v
                    if (auto* pair = as_array(v); pair && pair->v.size() == 2)
                        dict_set(d, pair->v[0], pair->v[1]);
                    else
                        dict_set(d, v, cs_true());
                } else {
                    CsRef add = getattr(obj, "Add");
                    if (add)
                        call1(add, v, e->pos);
                }
                continue;
            }
            if (!setattr(obj, slot, v)) {
                if (auto* d = as_dict(obj))
                    dict_set(d, cs_str(slot), v);
            }
        }
        return obj;
    }
    case et::throw_unsupported: {
        if (e->name == "throw_expr") {
            CsRef v = eval(e->base.get(), f);
            if (v && v->kind == cs_kind::exception_)
                throw sig_raise(std::move(v));
            throw sig_raise(cs_exc("Exception", cs_to_str(*this, v)));
        }
        raise_exc("NotSupportedException", "unsupported feature: " + e->name,
                  e->pos);
    }
    default:
        raise_exc("NotSupportedException", "unknown expression tag", e->pos);
    }
}

CsRef interpreter::eval_expr_in_frame(const ast_expr* e, frame* f) {
    return eval(e, *f);
}

CsRef interpreter::new_instance_named(const std::string& tn,
                                      const std::vector<expr_ptr>& arg_exprs,
                                      src_pos pos, frame& f) {
    cs_args a;
    a.pos.reserve(arg_exprs.size());
    for (const auto& x : arg_exprs)
        a.pos.push_back(eval(x.get(), f));
    return new_instance_eval(tn, a, pos, f);
}

CsRef interpreter::new_instance_eval(const std::string& tn, cs_args& a,
                                     src_pos pos, frame& f) {
    // builtin types: List<T>/Dictionary<K,V>/array/extension of exceptions
    if (tn.empty() || tn == "[]")
        return cs_array();
    // array spellings `T[]`/`T[,]` — element type is erased in the value
    // model; the new_expr init loop pushes collection-initializer parts.
    {
        bool arr_ty = false;
        std::size_t hi = tn.size();
        while (hi >= 2 && tn[hi - 1] == ']') {
            const auto lb = tn.rfind('[', hi - 1);
            if (lb == std::string::npos)
                break;
            bool rank = true;
            for (std::size_t k = lb + 1; k + 1 < hi; ++k)
                if (tn[k] != ',')
                    rank = false;
            if (!rank)
                break;
            hi = lb;
            arr_ty = true;
        }
        if (arr_ty)
            return cs_array();
    }
    if (tn.rfind("List<", 0) == 0 || tn == "List")
        return cs_array();
    if (tn.rfind("Dictionary<", 0) == 0 || tn == "Dictionary" ||
        tn.rfind("SortedDictionary<", 0) == 0 ||
        tn.rfind("SortedList<", 0) == 0)
        return cs_dict();
    if (tn == "object" || tn == "dynamic" || tn == "var")
        return cs_dict();
    if (tn == "string" || tn == "String") {
        std::string out;
        for (const auto& x : a.pos)
            out += cs_to_str(*this, x);
        return cs_str(out);
    }
    // exception family → CsExcObj
    {
        static const std::unordered_set<std::string> exc_types = {
            "Exception",           "System.Exception",
            "InvalidOperationException", "ArgumentException",
            "ArgumentNullException",     "ArgumentOutOfRangeException",
            "NotSupportedException",     "NotImplementedException",
            "NullReferenceException",    "FormatException",
            "OverflowException",         "InvalidCastException",
            "KeyNotFoundException",      "IndexOutOfRangeException",
            "DivideByZeroException",     "ArithmeticException",
            "InvalidDataException",      "InvalidProgramException",
            "ApplicationException",      "MissingMemberException",
            "MissingFieldException",     "TypeLoadException",
            "IOException",               "TimeoutException",
            "OperationCanceledException",
        };
        std::string short_name = tn;
        const auto dot = tn.rfind('.');
        if (dot != std::string::npos)
            short_name = tn.substr(dot + 1);
        if (exc_types.count(tn) || exc_types.count(short_name) ||
            (short_name.size() > 9 &&
             short_name.rfind("Exception") == short_name.size() - 9)) {
            const std::string msg =
                a.pos.empty() ? "" : cs_to_str(*this, a.pos[0]);
            auto e = cs_exc(short_name.empty() ? "Exception" : short_name, msg);
            auto* eo = as_exc(e);
            for (frame* fr = cur_frame; fr; fr = fr->caller)
                eo->trace.push_back(fr->file + ":" +
                                    std::to_string(fr->line) + " in " +
                                    fr->fn_name);
            return e;
        }
    }
    // declared class
    if (CsRef klass = dict_get(g(globals), cs_str(tn))) {
        return instantiate(klass, a, pos);
    }
    // resolve through builtin facade types (e.g. System.Exception path)
    bool found = false;
    if (CsRef v = scope_get(f, tn, &found); found)
        return instantiate(v, a, pos);
    raise_exc("TypeLoadException", "cannot instantiate '" + tn + "'", pos);
}

} // namespace sao::plugins::csmini
