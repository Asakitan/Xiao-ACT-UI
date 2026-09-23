// pymini_parser.cpp — recursive-descent parser → AST.
//
// Covers the plugin-observed Python subset: statements, functions, classes,
// exceptions, imports, comprehensions, lambdas, decorators, f-strings,
// slicing, unpacking.  Rejected constructs are flagged at parse time as
// explicit "unsupported" errors so the adapter arbitration can delegate.
#include "pymini_parser.h"

namespace sao::plugins::pymini {
namespace {

using TK = tok_kind;

bool is_keyword(const token& t, const char* kw) {
    return t.kind == TK::name && t.text == kw;
}

struct parser {
    const std::vector<token>& toks;
    std::string file;
    feature_flags* flags = nullptr;
    std::size_t pos = 0;
    int function_depth = 0;

    const token& peek(std::size_t off = 0) const {
        const std::size_t idx = pos + off;
        static const token eof_tok{TK::eof_, {}, 0, 0.0, {0, 0}};
        return idx < toks.size() ? toks[idx] : eof_tok;
    }
    const token& get() { return toks[pos < toks.size() ? pos++ : toks.size() - 1]; }
    src_pos here() const { return peek().pos; }

    bool at(TK k) const { return peek().kind == k; }
    bool at(TK k, std::size_t off) const { return peek(off).kind == k; }
    bool at_kw(const char* kw) const { return is_keyword(peek(), kw); }
    bool at_kw(const char* kw, std::size_t off) const {
        return is_keyword(peek(off), kw);
    }
    bool at_comp_for() const {
        return at_kw("for") || (at_kw("async") && at_kw("for", 1));
    }

    [[noreturn]] void fail(const std::string& msg) {
        py_error e{};
        e.kind = "SyntaxError";
        e.message = msg;
        e.pos = here();
        e.file = file;
        throw e;
    }
    [[noreturn]] void unsupported(const std::string& what) {
        py_error e{};
        e.kind = "UnsupportedSyntax";
        e.message = what + " is outside the pymini subset";
        e.pos = here();
        e.file = file;
        e.features.push_back(what);
        throw e;
    }

    bool eat(TK k) {
        if (at(k)) {
            get();
            return true;
        }
        return false;
    }
    bool eat_kw(const char* kw) {
        if (at_kw(kw)) {
            get();
            return true;
        }
        return false;
    }
    void expect(TK k, const char* what) {
        if (!eat(k))
            fail(std::string("expected ") + what);
    }
    std::string expect_name(const char* what = "name") {
        if (!at(TK::name))
            fail(std::string("expected ") + what);
        return get().text;
    }
    void skip_newlines() {
        while (at(TK::newline))
            get();
    }

    expr_ptr mk(et t, src_pos p) {
        auto e = std::make_unique<ast_expr>();
        e->tag = t;
        e->pos = p;
        return e;
    }
    stmt_ptr ms(st t, src_pos p) {
        auto s = std::make_shared<ast_stmt>();
        s->tag = t;
        s->pos = p;
        return s;
    }
    expr_ptr mk_name(std::string n, src_pos p) {
        auto e = mk(et::name, p);
        e->name = std::move(n);
        return e;
    }
    expr_ptr mk_lit(PyRef v, src_pos p) {
        auto e = mk(et::literal, p);
        e->const_value = std::move(v);
        return e;
    }

    static bool expr_has_yield(const ast_expr* e) {
        if (e == nullptr)
            return false;
        if (e->tag == et::yield_ || e->tag == et::yield_from)
            return true;
        if (e->tag == et::lambda_) {
            for (const auto& param : e->params)
                if (expr_has_yield(param.default_value.get()))
                    return true;
            return false;
        }
        if (expr_has_yield(e->base.get()) || expr_has_yield(e->index.get()) ||
            expr_has_yield(e->orelse.get()) || expr_has_yield(e->step.get()))
            return true;
        for (const auto& part : e->parts)
            if (expr_has_yield(part.get()))
                return true;
        for (const auto& arg : e->call_args)
            if (expr_has_yield(arg.value.get()))
                return true;
        for (const auto& clause : e->generators) {
            if (expr_has_yield(clause.target.get()) ||
                expr_has_yield(clause.iter.get()))
                return true;
            for (const auto& cond : clause.ifs)
                if (expr_has_yield(cond.get()))
                    return true;
        }
        return false;
    }

    static bool stmt_has_yield(const ast_stmt* s) {
        if (s == nullptr)
            return false;
        if (s->tag == st::funcdef || s->tag == st::classdef) {
            for (const auto& decorator : s->decorators)
                if (expr_has_yield(decorator.get()))
                    return true;
            for (const auto& base : s->bases)
                if (expr_has_yield(base.get()))
                    return true;
            for (const auto& [name, base] : s->kw_bases) {
                (void)name;
                if (expr_has_yield(base.get()))
                    return true;
            }
            for (const auto& param : s->params)
                if (expr_has_yield(param.default_value.get()))
                    return true;
            return false;
        }
        if (expr_has_yield(s->value.get()) || expr_has_yield(s->value2.get()))
            return true;
        for (const auto& target : s->targets)
            if (expr_has_yield(target.get()))
                return true;
        for (const auto& item : s->with_items)
            if (expr_has_yield(item.ctx_expr.get()) ||
                expr_has_yield(item.target.get()))
                return true;
        for (const auto& arm : s->except_arms) {
            if (expr_has_yield(arm.type.get()))
                return true;
            for (const auto& child : arm.body)
                if (stmt_has_yield(child.get()))
                    return true;
        }
        for (const auto* body : {&s->body, &s->orelse, &s->final})
            for (const auto& child : *body)
                if (stmt_has_yield(child.get()))
                    return true;
        return false;
    }

    static bool body_has_yield(const std::vector<stmt_ptr>& body) {
        for (const auto& s : body)
            if (stmt_has_yield(s.get()))
                return true;
        return false;
    }

    // ── modules / statements ──────────────────────────────────────────────
    ast_module parse_module() {
        ast_module m;
        m.file = file;
        skip_newlines();
        while (!at(TK::eof_)) {
            m.body.push_back(parse_statement());
            skip_newlines();
        }
        return m;
    }

    stmt_ptr parse_statement() {
        if (at(TK::name) || at(TK::at))
            return parse_compound_or_simple();
        return parse_simple_line();
    }

    stmt_ptr parse_compound_or_simple() {
        if (at(TK::at))
            return parse_decorated();
        if (at_kw("if")) return parse_if();
        if (at_kw("while")) return parse_while();
        if (at_kw("for")) return parse_for(false);
        if (at_kw("try")) return parse_try();
        if (at_kw("with")) return parse_with(false);
        if (at_kw("def")) return parse_funcdef({}, false);
        if (at_kw("class")) return parse_classdef();
        if (at_kw("async")) {
            if (flags)
                flags->uses_async = true;
            get();
            if (at_kw("def"))
                return parse_funcdef({}, true);
            if (at_kw("for"))
                return parse_for(true);
            if (at_kw("with"))
                return parse_with(true);
            fail("unexpected 'async'");
        }
        if (at_kw("match"))
            unsupported("match statement");
        return parse_simple_line();
    }

    // a line of small statements ending with NEWLINE or EOF/dedent
    stmt_ptr parse_simple_line() {
        auto first = parse_small_stmt();
        if (at(TK::semicolon)) {
            auto block = ms(st::suite, first->pos);
            block->body.push_back(first);
            while (eat(TK::semicolon)) {
                if (at(TK::newline) || at(TK::eof_) || at(TK::dedent))
                    break;
                block->body.push_back(parse_small_stmt());
            }
            if (at(TK::newline))
                get();
            if (block->body.size() == 1)
                return block->body.front();
            return block;
        }
        if (at(TK::newline))
            get();
        return first;
    }

    stmt_ptr parse_small_stmt() {
        const src_pos atpos = here();
        if (at_kw("import")) return parse_import();
        if (at_kw("from")) return parse_import_from();
        if (at_kw("del")) {
            get();
            auto s = ms(st::del_, atpos);
            s->targets.push_back(parse_expr_list_allow_star());
            while (eat(TK::comma)) {
                if (at(TK::newline) || at(TK::semicolon))
                    break;
                s->targets.push_back(parse_expr());
            }
            return s;
        }
        if (at_kw("pass")) {
            get();
            return ms(st::pass_, atpos);
        }
        if (at_kw("break")) {
            get();
            return ms(st::break_, atpos);
        }
        if (at_kw("continue")) {
            get();
            return ms(st::continue_, atpos);
        }
        if (at_kw("return")) {
            get();
            auto s = ms(st::return_, atpos);
            if (!at(TK::newline) && !at(TK::semicolon) && !at(TK::eof_) &&
                !at(TK::dedent))
                s->value = parse_expr_or_tuple();
            return s;
        }
        if (at_kw("raise")) {
            get();
            auto s = ms(st::raise_, atpos);
            if (!at(TK::newline) && !at(TK::semicolon) && !at(TK::dedent)) {
                s->value = parse_expr();
                if (eat_kw("from"))
                    s->value2 = parse_expr();
            }
            return s;
        }
        if (at_kw("assert")) {
            get();
            auto s = ms(st::assert_, atpos);
            s->value = parse_expr();
            if (eat(TK::comma))
                s->value2 = parse_expr();
            return s;
        }
        if (at_kw("global")) {
            get();
            auto s = ms(st::global_, atpos);
            s->names.push_back(expect_name());
            while (eat(TK::comma))
                s->names.push_back(expect_name());
            return s;
        }
        if (at_kw("nonlocal")) {
            get();
            auto s = ms(st::nonlocal_, atpos);
            s->names.push_back(expect_name());
            while (eat(TK::comma))
                s->names.push_back(expect_name());
            if (flags)
                flags->uses_nonlocal = true;
            return s;
        }
        // expr / assignment family
        auto lhs = parse_expr_or_tuple_or_star();
        if (at(TK::assign)) {
            auto s = ms(st::assign, atpos);
            s->targets.push_back(std::move(lhs));
            while (eat(TK::assign)) {
                if (at(TK::assign) || at(TK::newline) || at(TK::semicolon))
                    break;
                s->targets.push_back(parse_expr_or_tuple_or_star());
            }
            // last "target" is actually the value
            if (s->targets.size() < 2)
                fail("invalid assignment");
            s->value = std::move(s->targets.back());
            s->targets.pop_back();
            return s;
        }
        // annotated assign:  x: T [= v]
        if (at(TK::colon) && lhs->tag == et::name) {
            get();
            auto s = ms(st::ann_assign, atpos);
            s->value = parse_expr();          // annotation — ignored
            if (eat(TK::assign))
                s->value2 = parse_expr_or_tuple();
            s->targets.push_back(std::move(lhs));
            return s;
        }
        static const struct { TK tok; } aug_ops[] = {
            {TK::plus_eq}, {TK::minus_eq}, {TK::star_eq}, {TK::dstar_eq},
            {TK::slash_eq}, {TK::dslash_eq}, {TK::percent_eq}, {TK::amp_eq},
            {TK::pipe_eq}, {TK::caret_eq}, {TK::lshift_eq}, {TK::rshift_eq},
            {TK::at_eq},
        };
        for (const auto& o : aug_ops) {
            if (at(o.tok)) {
                get();
                auto s = ms(st::aug_assign, atpos);
                s->aug_op = o.tok;
                s->targets.push_back(std::move(lhs));
                s->value = parse_expr_or_tuple();
                return s;
            }
        }
        auto s = ms(st::expr_stmt, atpos);
        s->value = std::move(lhs);
        return s;
    }

    // ── suites ────────────────────────────────────────────────────────────
    std::vector<stmt_ptr> parse_suite() {
        // ':' then either NEWLINE INDENT stmts DEDENT or inline simple stmts
        expect(TK::colon, "':'");
        std::vector<stmt_ptr> out;
        if (at(TK::newline)) {
            get();
            expect(TK::indent, "indented block");
            skip_newlines();
            while (!at(TK::dedent) && !at(TK::eof_)) {
                out.push_back(parse_statement());
                skip_newlines();
            }
            eat(TK::dedent);
        } else {
            // inline suite: simple statements on the same line
            out.push_back(parse_small_stmt());
            while (eat(TK::semicolon)) {
                if (at(TK::newline) || at(TK::eof_) || at(TK::dedent))
                    break;
                out.push_back(parse_small_stmt());
            }
            if (at(TK::newline))
                get();
        }
        return out;
    }

    stmt_ptr parse_if() {
        const src_pos atpos = here();
        get(); // if
        auto s = ms(st::if_, atpos);
        s->value = parse_expr();
        s->body = parse_suite();
        stmt_ptr tail = s;
        while (at_kw("elif")) {
            get();
            auto arm = ms(st::if_, here());
            arm->value = parse_expr();
            arm->body = parse_suite();
            tail->orelse.push_back(arm);
            tail = arm;
        }
        if (eat_kw("else"))
            tail->orelse = parse_suite();
        return s;
    }

    stmt_ptr parse_while() {
        const src_pos atpos = here();
        get();
        auto s = ms(st::while_, atpos);
        s->value = parse_expr();
        s->body = parse_suite();
        if (eat_kw("else"))
            s->orelse = parse_suite();
        return s;
    }

    stmt_ptr parse_for(bool is_async) {
        const src_pos atpos = here();
        get();
        auto s = ms(st::for_, atpos);
        s->is_async = is_async;
        s->targets.push_back(parse_target_list());
        if (!eat_kw("in"))
            fail("expected 'in' after for target");
        s->value = parse_expr_or_tuple();
        s->body = parse_suite();
        if (eat_kw("else"))
            s->orelse = parse_suite();
        return s;
    }

    // target list without trailing commas converting to tuple
    expr_ptr parse_target_list() {
        // elements stay BELOW parse_compare: full parse_expr eats the `in`
        // delimiter after the last target as a containment test
        // (`for x in y` → target `x in y`), leaving nothing for the
        // caller's eat_kw("in").  parse_bitor still covers names, attrs,
        // subscripts and (a,b)/(a,b) paren/bracket targets.
        auto first = parse_bitor();
        if (at(TK::comma)) {
            auto tup = mk(et::tuple_lit, first->pos);
            tup->parts.push_back(std::move(first));
            while (eat(TK::comma)) {
                if (at_kw("in"))
                    break;
                tup->parts.push_back(parse_bitor());
            }
            return tup;
        }
        return first;
    }

    stmt_ptr parse_try() {
        const src_pos atpos = here();
        get();
        auto s = ms(st::try_, atpos);
        s->body = parse_suite();
        bool saw_except = false;
        while (at_kw("except")) {
            saw_except = true;
            get();
            except_arm arm{};
            if (!at(TK::colon)) {
                arm.type = parse_expr();
                if (eat_kw("as"))
                    arm.name = expect_name();
            }
            arm.body = parse_suite();
            s->except_arms.push_back(std::move(arm));
        }
        if (eat_kw("else")) {
            s->orelse = parse_suite();
        }
        if (eat_kw("finally")) {
            s->final = parse_suite();
        }
        if (!saw_except && s->final.empty())
            fail("expected 'except' or 'finally' after try");
        return s;
    }

    stmt_ptr parse_with(bool is_async) {
        const src_pos atpos = here();
        get();
        if (flags)
            flags->uses_with = true;
        auto s = ms(st::with_, atpos);
        s->is_async = is_async;
        while (true) {
            with_item item{};
            item.ctx_expr = parse_expr();
            if (eat_kw("as"))
                item.target = parse_expr();
            s->with_items.push_back(std::move(item));
            if (!eat(TK::comma))
                break;
        }
        s->body = parse_suite();
        return s;
    }

    std::vector<ast_param_decl> parse_params() {
        std::vector<ast_param_decl> out;
        bool seen_varargs = false;
        bool seen_kwarg = false;
        bool seen_default = false;
        bool seen_positional_only = false;
        (void)seen_positional_only;
        while (true) {
            if (at(TK::dstar)) {
                get();
                ast_param_decl p;
                p.name = expect_name("**kwargs name");
                p.kwarg = true;
                out.push_back(std::move(p));
                seen_kwarg = true;
            } else if (at(TK::star)) {
                get();
                if (at(TK::name)) {
                    ast_param_decl p;
                    p.name = get().text;
                    p.varargs = true;
                    out.push_back(std::move(p));
                }
                seen_varargs = true;
            } else if (at(TK::slash)) {
                get();
                seen_positional_only = true;   // positional-only marker: accept+ignore
            } else if (at(TK::name)) {
                ast_param_decl p;
                p.name = get().text;
                p.kwonly = seen_varargs;
                if (eat(TK::assign)) {
                    p.default_value = parse_expr();
                    seen_default = true;
                } else if (seen_default && !seen_varargs)
                    fail("non-default argument follows default argument");
                out.push_back(std::move(p));
            } else {
                break;
            }
            if (!eat(TK::comma))
                break;
            if (at(TK::rparen))
                break;
        }
        return out;
    }

    stmt_ptr parse_decorated() {
        const src_pos atpos = here();
        std::vector<expr_ptr> decorators;
        while (eat(TK::at)) {
            if (flags)
                flags->uses_decorator = true;
            auto d = parse_expr();
            decorators.push_back(std::move(d));
            if (at(TK::newline))
                get();
        }
        if (at_kw("async") && at_kw("def", 1)) {
            if (flags)
                flags->uses_async = true;
            get();
            return parse_funcdef(std::move(decorators), true);
        }
        if (at_kw("def"))
            return parse_funcdef(std::move(decorators), false);
        if (at_kw("class"))
            return parse_classdef(std::move(decorators));
        fail("expected 'def' or 'class' after decorator");
    }

    stmt_ptr parse_funcdef(std::vector<expr_ptr> decorators, bool is_async) {
        const src_pos atpos = here();
        get(); // def
        auto s = ms(st::funcdef, atpos);
        s->is_async = is_async;
        s->name = expect_name("function name");
        s->decorators = std::move(decorators);
        expect(TK::lparen, "'('");
        s->params = parse_params();
        expect(TK::rparen, "')'");
        if (eat(TK::arrow))
            (void)parse_expr();          // return annotation — ignored
        ++function_depth;
        s->body = parse_suite();
        --function_depth;
        s->is_generator = body_has_yield(s->body);
        return s;
    }

    stmt_ptr parse_classdef(std::vector<expr_ptr> decorators = {}) {
        const src_pos atpos = here();
        get(); // class
        if (flags)
            flags->uses_class = true;
        auto s = ms(st::classdef, atpos);
        s->name = expect_name("class name");
        s->decorators = std::move(decorators);
        if (eat(TK::lparen)) {
            if (!at(TK::rparen)) {
                while (true) {
                    // keyword base/class option (metaclass=…, etc.)
                    if (at(TK::name) && at(TK::assign, 1)) {
                        const std::string kw = get().text;
                        get(); // =
                        s->kw_bases.emplace_back(kw, parse_expr());
                    } else {
                        s->bases.push_back(parse_expr());
                    }
                    if (!eat(TK::comma))
                        break;
                    if (at(TK::rparen))
                        break;
                }
            }
            expect(TK::rparen, "')'");
        }
        const int saved_function_depth = function_depth;
        function_depth = 0;
        s->body = parse_suite();
        function_depth = saved_function_depth;
        return s;
    }

    stmt_ptr parse_import() {
        const src_pos atpos = here();
        get(); // import
        if (flags)
            flags->uses_absolute_import = true;
        auto s = ms(st::import, atpos);
        while (true) {
            std::string dotted = expect_name("module name");
            while (eat(TK::dot))
                dotted += "." + expect_name();
            std::string alias;
            if (eat_kw("as"))
                alias = expect_name("alias");
            s->imports.emplace_back(dotted, alias);
            if (flags) {
                const auto root = dotted.substr(0, dotted.find('.'));
                flags->imported_roots.push_back(root);
            }
            if (!eat(TK::comma))
                break;
        }
        return s;
    }

    stmt_ptr parse_import_from() {
        const src_pos atpos = here();
        get(); // from
        auto s = ms(st::import_from, atpos);
        int level = 0;
        while (at(TK::dot) || at(TK::ellipsis)) {
            if (eat(TK::ellipsis))
                level += 3;
            else {
                get();
                ++level;
            }
        }
        s->from_level = level;
        if (level > 0 && flags)
            flags->uses_relative_import = true;
        if (!at_kw("import")) {
            std::string dotted;
            if (at(TK::name))
                dotted = get().text;
            while (at(TK::dot)) {
                get();
                if (at(TK::name))
                    dotted += "." + get().text;
                else
                    fail("expected module name after '.'");
            }
            s->from_module = dotted;
            if (flags && !dotted.empty() && level == 0) {
                flags->imported_roots.push_back(
                    dotted.substr(0, dotted.find('.')));
                flags->uses_absolute_import = true;
            }
        }
        if (!eat_kw("import"))
            fail("expected 'import' after from ...");
        if (eat(TK::lparen)) {
            while (!at(TK::rparen)) {
                const std::string n = expect_name("imported name");
                std::string alias;
                if (eat_kw("as"))
                    alias = expect_name("alias");
                s->imports.emplace_back(n, alias);
                if (!eat(TK::comma))
                    break;
            }
            expect(TK::rparen, "')'");
        } else if (at(TK::star)) {
            get();
            s->imports.emplace_back("*", "");
        } else {
            while (true) {
                const std::string n = expect_name("imported name");
                std::string alias;
                if (eat_kw("as"))
                    alias = expect_name("alias");
                s->imports.emplace_back(n, alias);
                if (!eat(TK::comma))
                    break;
            }
        }
        return s;
    }

    // ── expressions (precedence climbing) ──────────────────────────────────
    // full expression incl. tuple construction (a, b)
    expr_ptr parse_expr_or_tuple() {
        auto first = parse_expr();
        if (!at(TK::comma))
            return first;
        auto tup = mk(et::tuple_lit, first->pos);
        tup->parts.push_back(std::move(first));
        while (eat(TK::comma)) {
            if (at(TK::newline) || at(TK::rparen) || at(TK::rbracket) ||
                at(TK::rbrace) || at(TK::semicolon) || at(TK::eof_) ||
                at(TK::assign) || at_kw("in") || at(TK::colon))
                break;
            tup->parts.push_back(parse_expr());
        }
        return tup;
    }

    expr_ptr parse_expr_or_tuple_or_star() {
        // for assignment LHS/RHS:  a, b = c  /  *a, b = c
        if (at(TK::star)) {
            const src_pos atpos = here();
            get();
            auto s = mk(et::star_, atpos);
            s->base = parse_expr();
            if (at(TK::comma)) {
                auto tup = mk(et::tuple_lit, atpos);
                tup->parts.push_back(std::move(s));
                while (eat(TK::comma)) {
                    if (at(TK::assign) || at(TK::newline))
                        break;
                    tup->parts.push_back(parse_expr());
                }
                return tup;
            }
            return s;
        }
        auto first = parse_expr();
        if (at(TK::comma)) {
            auto tup = mk(et::tuple_lit, first->pos);
            tup->parts.push_back(std::move(first));
            while (eat(TK::comma)) {
                if (at(TK::assign) || at(TK::newline) || at(TK::semicolon) ||
                    at(TK::rparen) || at_kw("in"))
                    break;
                if (at(TK::star)) {
                    get();
                    auto s = mk(et::star_, first->pos);
                    s->base = parse_expr();
                    tup->parts.push_back(std::move(s));
                } else {
                    tup->parts.push_back(parse_expr());
                }
            }
            return tup;
        }
        return first;
    }

    expr_ptr parse_expr_list_allow_star() { return parse_expr_or_tuple_or_star(); }

    expr_ptr parse_expr() {
        if (at_kw("yield"))
            return parse_yield_expr();
        if (at_kw("lambda"))
            return parse_lambda();
        auto cond = parse_or();
        if (at_kw("if")) {
            get();
            auto test = parse_or();
            if (!eat_kw("else"))
                fail("expected 'else' in conditional expression");
            auto els = parse_expr();
            auto e = mk(et::ifexp, cond->pos);
            e->base = std::move(cond);      // true value
            e->index = std::move(test);     // condition
            e->orelse = std::move(els);
            cond = std::move(e);
        }
        if (eat(TK::walrus)) {
            if (cond->tag != et::name)
                fail("assignment expression target must be a name");
            auto named = mk(et::named_expr, cond->pos);
            named->name = cond->name;
            named->base = parse_expr();
            if (flags)
                flags->uses_walrus = true;
            return named;
        }
        return cond;
    }

    expr_ptr parse_yield_expr() {
        const src_pos atpos = here();
        get();
        if (function_depth == 0)
            fail("'yield' outside function");
        if (flags)
            flags->uses_yield = true;
        auto e = mk(et::yield_, atpos);
        if (eat_kw("from")) {
            e->tag = et::yield_from;
            if (flags)
                flags->uses_yield_from = true;
            e->base = parse_expr();
            return e;
        }
        if (!at(TK::newline) && !at(TK::semicolon) && !at(TK::dedent) &&
            !at(TK::eof_) && !at(TK::rparen) && !at(TK::rbracket) &&
            !at(TK::rbrace))
            e->base = parse_expr_or_tuple();
        return e;
    }

    expr_ptr parse_lambda() {
        const src_pos atpos = here();
        get(); // lambda
        if (flags)
            flags->uses_lambda = true;
        auto e = mk(et::lambda_, atpos);
        // params mirror parse_params but allow pos-only defaults
        while (!at(TK::colon)) {
            ast_param_decl p;
            if (at(TK::star)) {
                get();
                p.varargs = true;
                if (at(TK::name))
                    p.name = get().text;
            } else if (at(TK::dstar)) {
                get();
                p.kwarg = true;
                p.name = expect_name();
            } else {
                p.name = expect_name();
                if (eat(TK::assign))
                    p.default_value = parse_or();
            }
            e->params.push_back(std::move(p));
            if (!eat(TK::comma))
                break;
        }
        expect(TK::colon, "':' after lambda");
        ++function_depth;
        e->base = parse_expr();
        --function_depth;
        e->is_generator = expr_has_yield(e->base.get());
        return e;
    }

    expr_ptr parse_or() {
        auto left = parse_and();
        while (at_kw("or")) {
            get();
            auto right = parse_and();
            auto e = mk(et::boolop, left->pos);
            e->op = TK::pipe;               // marker: or
            e->parts.push_back(std::move(left));
            e->parts.push_back(std::move(right));
            left = std::move(e);
        }
        return left;
    }

    expr_ptr parse_and() {
        auto left = parse_not();
        while (at_kw("and")) {
            get();
            auto right = parse_not();
            auto e = mk(et::boolop, left->pos);
            e->op = TK::amp;                // marker: and
            e->parts.push_back(std::move(left));
            e->parts.push_back(std::move(right));
            left = std::move(e);
        }
        return left;
    }

    expr_ptr parse_not() {
        if (eat_kw("not")) {
            auto e = mk(et::unop, peek().pos);
            e->op = TK::tilde;              // marker: not
            e->name = "not";              // distinguishes `not` from `~`
            e->base = parse_not();
            return e;
        }
        return parse_compare();
    }

    expr_ptr parse_compare() {
        auto left = parse_bitor();
        static const TK ops[] = {TK::eq, TK::ne, TK::lt, TK::le, TK::gt, TK::ge};
        expr_ptr chain;
        while (true) {
            TK op = TK::eof_;
            for (const TK o : ops)
                if (at(o))
                    op = o;
            bool is_is = false, is_in = false, is_not = false;
            if (op == TK::eof_) {
                if (at_kw("is") && at_kw("not", 1)) {
                    is_is = true;
                    is_not = true;
                } else if (at_kw("is")) {
                    is_is = true;
                } else if (at_kw("not") && at_kw("in", 1)) {
                    is_in = true;
                    is_not = true;
                } else if (at_kw("in")) {
                    is_in = true;
                }
            }
            if (op == TK::eof_ && !is_is && !is_in)
                break;
            if (is_not) {
                get(); get();   // consume `is not` / `not in`
            } else {
                get();          // single op or `is`/`in`
            }
            // encoding: is→lshift, is-not→rshift, in→dslash, not-in→percent
            tok_kind encoded = op;
            if (is_is)
                encoded = is_not ? TK::rshift : TK::lshift;
            else if (is_in)
                encoded = is_not ? TK::percent : TK::dslash;
            if (!chain) {
                chain = mk(et::compare, left->pos);
                chain->base = std::move(left);
            }
            chain->ops.push_back(encoded);
            chain->parts.push_back(parse_bitor());
            left = nullptr;   // consumed into chain
        }
        return chain ? std::move(chain) : std::move(left);
    }

    expr_ptr parse_bitor() {
        auto left = parse_bitxor();
        while (at(TK::pipe)) {
            get();
            auto right = parse_bitxor();
            auto e = mk(et::binop, left->pos);
            e->op = TK::pipe;
            e->base = std::move(left);
            e->index = std::move(right);
            left = std::move(e);
        }
        return left;
    }
    expr_ptr parse_bitxor() {
        auto left = parse_bitand();
        while (at(TK::caret)) {
            get();
            auto right = parse_bitand();
            auto e = mk(et::binop, left->pos);
            e->op = TK::caret;
            e->base = std::move(left);
            e->index = std::move(right);
            left = std::move(e);
        }
        return left;
    }
    expr_ptr parse_bitand() {
        auto left = parse_shift();
        while (at(TK::amp)) {
            get();
            auto right = parse_shift();
            auto e = mk(et::binop, left->pos);
            e->op = TK::amp;
            e->base = std::move(left);
            e->index = std::move(right);
            left = std::move(e);
        }
        return left;
    }
    expr_ptr parse_shift() {
        auto left = parse_arith();
        while (at(TK::lshift) || at(TK::rshift)) {
            const TK op = get().kind;
            auto right = parse_arith();
            auto e = mk(et::binop, left->pos);
            e->op = op;
            e->base = std::move(left);
            e->index = std::move(right);
            left = std::move(e);
        }
        return left;
    }
    expr_ptr parse_arith() {
        auto left = parse_term();
        while (at(TK::plus) || at(TK::minus)) {
            const TK op = get().kind;
            auto right = parse_term();
            auto e = mk(et::binop, left->pos);
            e->op = op;
            e->base = std::move(left);
            e->index = std::move(right);
            left = std::move(e);
        }
        return left;
    }
    expr_ptr parse_term() {
        auto left = parse_factor();
        while (at(TK::star) || at(TK::slash) || at(TK::dslash) ||
               at(TK::percent) || at(TK::at)) {
            const TK op = get().kind;
            auto right = parse_factor();
            auto e = mk(et::binop, left->pos);
            e->op = op;
            e->base = std::move(left);
            e->index = std::move(right);
            left = std::move(e);
        }
        return left;
    }
    expr_ptr parse_factor() {
        if (at_kw("await")) {
            const src_pos atpos = here();
            get();
            if (flags)
                flags->uses_async = true;
            auto e = mk(et::await_, atpos);
            e->base = parse_factor();
            return e;
        }
        if (at(TK::plus) || at(TK::minus) || at(TK::tilde)) {
            const src_pos atpos = here();
            const TK op = get().kind;
            auto e = mk(et::unop, atpos);
            e->op = op;
            e->base = parse_factor();
            return e;
        }
        return parse_power();
    }
    expr_ptr parse_power() {
        auto base = parse_atom_trailer();
        if (at(TK::dstar)) {
            get();
            auto e = mk(et::binop, base->pos);
            e->op = TK::dstar;
            e->base = std::move(base);
            e->index = parse_factor();      // right-assoc
            return e;
        }
        return base;
    }
    expr_ptr parse_atom_trailer() {
        auto e = parse_atom();
        while (true) {
            if (at(TK::dot)) {
                get();
                auto n = mk(et::attr, e->pos);
                n->base = std::move(e);
                n->name = expect_name("attribute");
                e = std::move(n);
            } else if (at(TK::lparen)) {
                get();
                auto c = mk(et::call, e->pos);
                c->base = std::move(e);
                c->call_args = parse_call_args();
                e = std::move(c);
            } else if (at(TK::lbracket)) {
                get();
                auto s = mk(et::subscript, e->pos);
                s->base = std::move(e);
                s->index = parse_subscript_index();
                expect(TK::rbracket, "']'");
                e = std::move(s);
            } else {
                break;
            }
        }
        return e;
    }

    std::vector<call_arg> parse_call_args() {
        std::vector<call_arg> out;
        bool genexp_allowed = true;
        while (!at(TK::rparen)) {
            call_arg a{};
            if (at(TK::dstar)) {
                get();
                a.dstar = true;
                a.value = parse_expr();
                if (flags)
                    flags->uses_star_args = true;
            } else if (at(TK::star)) {
                get();
                a.star = true;
                a.value = parse_expr();
                if (flags)
                    flags->uses_star_args = true;
            } else if (at(TK::name) && at(TK::assign, 1)) {
                a.kw = get().text;
                get(); // =
                a.value = parse_expr();
            } else {
                a.value = parse_expr();
                // genexp: f(x for x in y) — detect trailing `for`
                if (genexp_allowed && at_comp_for()) {
                    auto g = mk(et::comprehension, a.value->pos);
                    g->base = std::move(a.value);
                    g->generators = parse_comp_for();
                    a.value = std::move(g);
                }
            }
            out.push_back(std::move(a));
            genexp_allowed = false;
            if (!eat(TK::comma))
                break;
        }
        expect(TK::rparen, "')'");
        return out;
    }

    expr_ptr parse_subscript_index() {
        // handles a, a:b, a:b:c, slices + tuple of indices
        auto parse_one = [&]() -> expr_ptr {
            if (at(TK::colon)) {
                auto sl = mk(et::slice_lit, here());
                sl->base = nullptr;
                // consume ':' then upper/step
                get();
                if (!at(TK::colon) && !at(TK::rbracket) && !at(TK::comma))
                    sl->index = parse_expr();
                if (at(TK::colon)) {
                    get();
                    if (!at(TK::rbracket) && !at(TK::comma))
                        sl->step = parse_expr();
                }
                return sl;
            }
            auto first = parse_expr();
            if (at(TK::colon)) {
                auto sl = mk(et::slice_lit, first->pos);
                sl->base = std::move(first);
                get();
                if (!at(TK::colon) && !at(TK::rbracket) && !at(TK::comma))
                    sl->index = parse_expr();
                if (at(TK::colon)) {
                    get();
                    if (!at(TK::rbracket) && !at(TK::comma))
                        sl->step = parse_expr();
                }
                return sl;
            }
            return first;
        };
        auto first = parse_one();
        if (at(TK::comma)) {
            auto tup = mk(et::tuple_lit, first->pos);
            tup->parts.push_back(std::move(first));
            while (eat(TK::comma)) {
                if (at(TK::rbracket))
                    break;
                tup->parts.push_back(parse_one());
            }
            return tup;
        }
        return first;
    }

    // comprehension: for target in iter [for ...] [if cond]* — returns clauses
    std::vector<comp_clause> parse_comp_for() {
        std::vector<comp_clause> out;
        while (at_comp_for()) {
            comp_clause cl{};
            if (eat_kw("async")) {
                cl.is_async = true;
                if (flags)
                    flags->uses_async = true;
            }
            if (!eat_kw("for"))
                fail("expected 'for' in comprehension");
            cl.target = parse_target_list();
            if (!eat_kw("in"))
                fail("expected 'in' in comprehension");
            cl.iter = parse_or();
            while (at_kw("if")) {
                get();
                cl.ifs.push_back(parse_or());
            }
            out.push_back(std::move(cl));
        }
        if (flags)
            flags->uses_comprehension = true;
        return out;
    }

    expr_ptr parse_atom() {
        const src_pos atpos = here();
        const token& t = peek();
        switch (t.kind) {
        case TK::number: {
            get();
            if (t.num_value != 0.0 ||
                (t.text.find_first_of(".eE") != std::string::npos &&
                 t.int_value == 0)) {
                return mk_lit(py_float(t.num_value), atpos);
            }
            return mk_lit(py_int(t.int_value), atpos);
        }
        case TK::string: {
            get();
            std::string acc = t.text;
            while (at(TK::string))
                acc += get().text;           // implicit concat
            return mk_lit(py_str(acc), atpos);
        }
        case TK::bytes_: {
            get();
            std::string acc = t.text;
            while (at(TK::bytes_))
                acc += get().text;         // implicit concat b"a" b"b"
            return mk_lit(py_bytes(acc), atpos);
        }
        case TK::fstring: {
            const bool raw_f = t.raw_str;
            get();
            return parse_fstring(t.text, atpos, raw_f);
        }
        case TK::ellipsis: {
            get();
            return mk(et::ellipses_, atpos);
        }
        case TK::lparen: {
            get();
            if (at(TK::rparen)) {
                get();
                return mk(et::tuple_lit, atpos);
            }
            auto inner = parse_expr_or_tuple_or_star();
            // genexp inside bare parens: (x for x in y)
            if (inner->tag != et::tuple_lit && at_comp_for()) {
                auto g = mk(et::comprehension, atpos);
                g->base = std::move(inner);
                g->generators = parse_comp_for();
                expect(TK::rparen, "')'");
                return g;
            }
            expect(TK::rparen, "')'");
            return inner;
        }
        case TK::lbracket: {
            get();
            auto lst = mk(et::list_lit, atpos);
            if (!at(TK::rbracket)) {
                // first element is one expr (optionally starred) — commas
                // belong to the LIST, so parse_expr_or_tuple_or_star would
                // wrongly fold `[1, 2, 3]` into a single (1,2,3) element.
                expr_ptr first;
                if (at(TK::star)) {
                    get();
                    auto s = mk(et::star_, here());
                    s->base = parse_expr();
                    first = std::move(s);
                } else {
                    first = parse_expr();
                }
                if (at_comp_for()) {
                    lst->tag = et::comprehension;
                    lst->base = std::move(first);
                    lst->generators = parse_comp_for();
                } else {
                    lst->parts.push_back(std::move(first));
                    while (eat(TK::comma)) {
                        if (at(TK::rbracket))
                            break;
                        if (at(TK::star)) {
                            get();
                            auto s = mk(et::star_, here());
                            s->base = parse_expr();
                            lst->parts.push_back(std::move(s));
                        } else {
                            lst->parts.push_back(parse_expr());
                        }
                    }
                }
            }
            expect(TK::rbracket, "']'");
            return lst;
        }
        case TK::lbrace: {
            get();
            auto d = mk(et::dict_lit, atpos);
            bool is_dict = false;
            bool decided = false;
            if (!at(TK::rbrace)) {
                if (at(TK::dstar)) {
                    // {**a, **b, k:v, …}
                    is_dict = true;
                    decided = true;
                } else {
                    auto first = parse_expr();
                    if (at(TK::colon)) {
                        is_dict = true;
                        decided = true;
                        get();
                        auto v = parse_expr();
                        d->parts.push_back(std::move(first));
                        d->parts.push_back(std::move(v));
                    } else if (at_comp_for()) {
                        d->tag = et::comprehension;
                        d->name = "set"; // marker: set comprehension
                        d->base = std::move(first);
                        d->generators = parse_comp_for();
                        decided = true;
                    } else {
                        // set literal — may still become a set comprehension
                        d->tag = et::set_lit;
                        d->parts.push_back(std::move(first));
                        while (eat(TK::comma)) {
                            if (at(TK::rbrace) || at_comp_for())
                                break;
                            if (at(TK::star)) {
                                get();
                                auto s = mk(et::star_, here());
                                s->base = parse_expr();
                                d->parts.push_back(std::move(s));
                            } else {
                                d->parts.push_back(parse_expr());
                            }
                        }
                        if (at_comp_for()) {
                            // {a, b for x in y} is a SyntaxError — a comp
                            // has exactly one output element
                            if (d->parts.size() > 1)
                                fail("invalid syntax");
                            d->tag = et::comprehension;
                            d->name = "set";
                            d->base = std::move(d->parts.front());
                            d->parts.clear();
                            d->generators = parse_comp_for();
                        }
                        decided = true;
                    }
                }
                // entries after the first require a ',' separator BEFORE the
                // next key — the previous code ate the comma at the END of
                // each iteration, so the next parse_expr() saw ',' and threw
                // "unexpected token ''" for any dict with 2+ pairs.
                bool need_comma = (d->parts.size() >= 2);
                while (decided && is_dict && !at(TK::rbrace)) {
                    // `{k:v for …}` comprehension follows the FIRST pair
                    // only — a `for` after 2+ pairs is a syntax error, so
                    // fall through to the comma requirement then.
                    if (at_comp_for() && d->parts.size() == 2)
                        break;   // dict comprehension — handled after loop
                    if (need_comma) {
                        expect(TK::comma, "',' in dict literal");
                        if (at(TK::rbrace))
                            break;   // trailing comma before '}' is legal
                    }
                    if (at(TK::dstar)) {
                        get();
                        auto kv = parse_expr();
                        auto marker = mk(et::literal, atpos);
                        marker->name = "**merge";
                        d->parts.push_back(std::move(marker));
                        d->parts.push_back(std::move(kv));
                    } else {
                        auto k = parse_expr();
                        expect(TK::colon, "':' in dict literal");
                        auto v = parse_expr();
                        d->parts.push_back(std::move(k));
                        d->parts.push_back(std::move(v));
                    }
                    need_comma = true;
                }
                if (decided && is_dict && at_comp_for()) {
                    d->tag = et::comprehension;
                    d->name = "dict";
                    auto kv = mk(et::tuple_lit, atpos);
                    kv->parts.push_back(std::move(d->parts[0]));
                    kv->parts.push_back(std::move(d->parts[1]));
                    d->parts.clear();
                    d->base = std::move(kv);
                    d->generators = parse_comp_for();
                }
            }
            expect(TK::rbrace, "'}'");
            return d;
        }
        case TK::name:
            get();
            if (t.text == "None")
                return mk_lit(py_none(), atpos);
            if (t.text == "True")
                return mk_lit(py_true(), atpos);
            if (t.text == "False")
                return mk_lit(py_false(), atpos);
            return mk_name(t.text, atpos);
        default:
            fail(std::string("unexpected token '") + t.text + "'");
        }
    }

    // f-string: split `raw` on {…} fields; produce parts = literals & exprs.
    expr_ptr parse_fstring(const std::string& raw, src_pos atpos,
                           bool raw_str = false) {
        if (flags)
            flags->uses_fstring = true;
        auto fs = mk(et::fstring_, atpos);
        std::size_t i = 0;
        std::string lit;
        auto flush_lit = [&] {
            if (!lit.empty()) {
                auto e = mk(et::literal, atpos);
                // decode \n \xNN etc — same rules as plain string literals;
                // rf-strings keep the backslashes verbatim
                e->const_value = py_str(
                    raw_str ? lit : pymini_decode_escapes(lit, atpos, file));
                fs->parts.push_back(std::move(e));
                lit.clear();
            }
        };
        while (i < raw.size()) {
            const char c = raw[i];
            if (c == '{') {
                if (i + 1 < raw.size() && raw[i + 1] == '{') {
                    lit += '{';
                    i += 2;
                    continue;
                }
                flush_lit();
                // parse field: expr[!conv][:spec]
                std::size_t depth = 1;
                std::size_t j = i + 1;
                std::string expr_src;
                while (j < raw.size() && depth > 0) {
                    const char d = raw[j];
                    if (d == '{')
                        ++depth;
                    else if (d == '}')
                        --depth;
                    if (depth == 0)
                        break;
                    expr_src += d;
                    ++j;
                }
                if (depth != 0)
                    fail("unterminated '{' in f-string");
                ++i;   // past '{'
                i = j + 1;
                // split expr / !conv / :spec atpos top level (respect parens)
                std::string expr_part, conv, spec;
                int pd = 0;
                for (std::size_t k = 0; k < expr_src.size(); ++k) {
                    const char d = expr_src[k];
                    if (d == '(' || d == '[' || d == '{')
                        ++pd;
                    else if (d == ')' || d == ']' || d == '}')
                        --pd;
                    // conversion marker is `!` followed by exactly one of
                    // {r,s,a} then `:` or end — `!=`/other ops stay in expr
                    const bool is_conv =
                        pd <= 0 && d == '!' && conv.empty() && spec.empty() &&
                        k + 1 < expr_src.size() &&
                        (expr_src[k + 1] == 'r' || expr_src[k + 1] == 's' ||
                         expr_src[k + 1] == 'a') &&
                        (k + 2 >= expr_src.size() ||
                         expr_src[k + 2] == ':' || expr_src[k + 2] == '=');
                    if (is_conv) {
                        conv = expr_src.substr(k + 1, 1);
                        expr_part = expr_src.substr(0, k);
                        if (k + 2 < expr_src.size() && expr_src[k + 2] == ':')
                            spec = expr_src.substr(k + 3);
                        else if (k + 1 < expr_src.size() && expr_src[k + 1] == ':')
                            spec = expr_src.substr(k + 2);
                        break;
                    }
                    if (pd <= 0 && d == ':' && spec.empty()) {
                        expr_part = expr_src.substr(0, k);
                        spec = expr_src.substr(k + 1);
                        break;
                    }
                    expr_part += d;
                }
                if (expr_part.empty())
                    expr_part = expr_src;
                auto field = parse_sub_expr(expr_part);
                if (!conv.empty()) {
                    auto cexpr = mk(et::call, atpos);
                    cexpr->base = mk_name(conv == "r" ? "repr"
                                          : conv == "a" ? "ascii" : "str", atpos);
                    call_arg a{};
                    a.value = std::move(field);
                    cexpr->call_args.push_back(std::move(a));
                    field = std::move(cexpr);
                }
                if (!spec.empty()) {
                    // format(expr, spec) — spec may contain nested {expr}
                    auto inner = std::move(field);
                    auto fexpr = mk(et::call, atpos);
                    fexpr->base = mk_name("format", atpos);
                    call_arg a1{};
                    a1.value = std::move(inner);
                    call_arg a2{};
                    if (spec.find('{') != std::string::npos)
                        a2.value = parse_fstring(spec, atpos, raw_str);
                    else
                        a2.value = mk_lit(
                            py_str(raw_str ? spec
                                           : pymini_decode_escapes(
                                                 spec, atpos, file)),
                            atpos);
                    fexpr->call_args.push_back(std::move(a1));
                    fexpr->call_args.push_back(std::move(a2));
                    field = std::move(fexpr);
                }
                fs->parts.push_back(std::move(field));
                continue;
            }
            if (c == '}' && i + 1 < raw.size() && raw[i + 1] == '}') {
                lit += '}';
                i += 2;
                continue;
            }
            if (c == '}')
                fail("single '}' in f-string");
            lit += c;
            ++i;
        }
        flush_lit();
        if (fs->parts.empty())
            fs->parts.push_back(mk_lit(py_str(""), atpos));
        return fs;
    }

    // re-lex + parse a sub-expression (used by f-string fields).
    expr_ptr parse_sub_expr(const std::string& src) {
        auto sub = lex_source(src, file);
        parser p2{sub, file, flags};
        auto e = p2.parse_expr();
        return e;
    }
};

} // namespace

ast_module parse_source(std::string_view source, const std::string& file,
                        feature_flags* flags) {
    const auto toks = lex_source(source, file);
    parser p{toks, file, flags};
    return p.parse_module();
}

} // namespace sao::plugins::pymini
