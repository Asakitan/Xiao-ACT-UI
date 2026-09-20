// csmini_parser.cpp — recursive-descent C# subset parser → AST.
//
// Grammar shape (brace language — indentation-free):
//   program    := (using_decl | namespace_decl | class_decl)*
//   class_decl := class name { member* }
//   member     := modifiers* (type name (field | method) | ctor) | attr-flagged
//   statements := local_decl | assign | expr | if | for | foreach | while |
//                 do | return | break | continue | block | try | throw
//   expressions:= C# precedence chain — assign → ?: → ?? → || → && → | → ^ →
//                 & → ==/!= → rel/is/as → shift → add → mul → unary → postfix
//                 → primary (literal/name/this/new/cast/typeof/interp)
#include "csmini_parser.h"

#include <unordered_set>

namespace sao::plugins::csmini {
namespace {

constexpr tok_kind compound_ops[] = {
    tok_kind::plus_eq, tok_kind::minus_eq, tok_kind::star_eq,
    tok_kind::slash_eq, tok_kind::percent_eq, tok_kind::amp_eq,
    tok_kind::pipe_eq, tok_kind::caret_eq, tok_kind::lshift_eq,
    tok_kind::rshift_eq, tok_kind::nullcoalesce_eq,
};

bool is_assign_op(tok_kind k) {
    if (k == tok_kind::assign)
        return true;
    for (tok_kind c : compound_ops)
        if (k == c)
            return true;
    return false;
}

struct parser {
    const std::vector<token>& toks;
    std::string file;
    feature_flags* flags;
    std::size_t p = 0;

    const token& cur() const { return toks[p]; }
    const token& at(std::size_t k) const { return toks[(std::min)(p + k, toks.size() - 1)]; }
    bool eof() const { return cur().kind == tok_kind::eof_; }
    src_pos pos() const { return cur().pos; }
    bool is(tok_kind k) const { return cur().kind == k; }
    bool is_kw(std::string_view w) const {
        return cur().kind == tok_kind::keyword && cur().text == w;
    }
    bool is_name(std::string_view w) const {
        return cur().kind == tok_kind::name && cur().text == w;
    }
    void adv() { if (!eof()) ++p; }
    [[noreturn]] void fail(const std::string& msg, src_pos pos = {}) {
        cs_error e;
        e.kind = "SyntaxError";
        e.message = msg;
        e.file = file;
        e.pos = pos.line ? pos : cur().pos;
        throw e;
    }
    void note_feature(std::string f) {
        // flag census — dedup before append
        if (flags) {
            for (const auto& x : flags->unsupported)
                if (x == f)
                    return;
            flags->unsupported.push_back(std::move(f));
        }
    }
    bool take(tok_kind k) {
        if (is(k)) {
            adv();
            return true;
        }
        return false;
    }
    bool take_kw(std::string_view w) {
        if (is_kw(w)) {
            adv();
            return true;
        }
        return false;
    }
    void expect(tok_kind k, const char* what) {
        if (!take(k))
            fail(std::string("expected ") + what);
    }
    const token& expect_name(const char* what = "identifier") {
        if (!is(tok_kind::name))
            fail(std::string("expected ") + what);
        const token& t = cur();
        adv();
        return t;
    }

    expr_ptr mk(et tag, src_pos pos = {}) {
        auto e = std::make_unique<ast_expr>();
        e->tag = tag;
        e->pos = pos;
        return e;
    }
    stmt_ptr ms(st tag, src_pos pos = {}) {
        auto s = std::make_shared<ast_stmt>();
        s->tag = tag;
        s->pos = pos;
        return s;
    }

    // ── type names ────────────────────────────────────────────────────
    // type_text parses a best-effort type spelling (ident.path, generics,
    // arrays, nullable) and returns it; fails with restore when the tokens
    // don't form one.  Used by decl detection and casts.
    bool type_token_start(std::size_t k) const {
        const token& t = at(k);
        if (t.kind == tok_kind::name)
            return true;
        if (t.kind == tok_kind::keyword) {
            static const std::unordered_set<std::string_view> type_kws = {
                "int", "long", "double", "float", "decimal", "bool",
                "string", "char", "byte", "sbyte", "short", "ushort",
                "uint", "ulong", "object", "var", "dynamic", "void",
                "nint", "nuint",
            };
            return type_kws.count(t.text) != 0;
        }
        return false;
    }
    // scan a type spelling starting at `start`; returns end index or npos.
    std::size_t type_end(std::size_t start) const {
        std::size_t k = start;
        if (!type_token_start(k))
            return std::string::npos;
        ++k;
        // qualified name .seg*
        for (;;) {
            if (k + 1 < toks.size() && at(k).kind == tok_kind::dot &&
                (at(k + 1).kind == tok_kind::name ||
                 at(k + 1).kind == tok_kind::keyword)) {
                k += 2;
                continue;
            }
            break;
        }
        // generic args <A,B> — depth-matched
        if (k < toks.size() && at(k).kind == tok_kind::lt) {
            int depth = 0;
            for (; k < toks.size(); ++k) {
                const tok_kind kk = at(k).kind;
                if (kk == tok_kind::lt || kk == tok_kind::lshift)
                    ++depth;
                else if (kk == tok_kind::gt) {
                    if (--depth == 0) {
                        ++k;
                        break;
                    }
                } else if (kk == tok_kind::rshift) {
                    depth -= 2;
                    if (depth <= 0) {
                        ++k;
                        break;
                    }
                } else if (kk == tok_kind::eof_)
                    return std::string::npos;
            }
            if (depth > 0)
                return std::string::npos;
        }
        // arrays + nullable suffixes
        for (;;) {
            if (k < toks.size() && at(k).kind == tok_kind::lbracket) {
                const std::size_t bracket = k;
                ++k;
                while (k < toks.size() && at(k).kind == tok_kind::comma)
                    ++k;
                if (k < toks.size() && at(k).kind == tok_kind::rbracket) {
                    ++k;
                    continue;
                }
                // non-empty `[` (size arg of `new int[5]`, index expr, …) is
                // not part of the type spelling — stop BEFORE it so callers
                // see the trailing `[` themselves.
                return bracket;
            }
            if (k < toks.size() && at(k).kind == tok_kind::qmark) {
                ++k;
                continue;
            }
            break;
        }
        return k;
    }
    std::string type_text(std::size_t start, std::size_t end) const {
        std::string out;
        for (std::size_t k = start; k < end; ++k) {
            const token& t = at(k);
            if (t.kind == tok_kind::name || t.kind == tok_kind::keyword)
                out += t.text;
            else if (t.kind == tok_kind::dot)
                out.push_back('.');
            else if (t.kind == tok_kind::lt)
                out.push_back('<');
            else if (t.kind == tok_kind::gt)
                out.push_back('>');
            else if (t.kind == tok_kind::lbracket)
                out.push_back('[');
            else if (t.kind == tok_kind::rbracket)
                out.push_back(']');
            else if (t.kind == tok_kind::comma)
                out.push_back(',');
            else if (t.kind == tok_kind::qmark)
                out.push_back('?');
            else if (t.kind == tok_kind::lshift)
                out += "<<";
            else if (t.kind == tok_kind::rshift)
                out += ">>";
        }
        return out;
    }

    // ── program ───────────────────────────────────────────────────────
    ast_program program() {
        ast_program prog;
        while (!eof()) {
            if (is_kw("using")) {
                using_decl(prog);
                continue;
            }
            if (is_kw("namespace")) {
                ns_decl(prog, "");
                continue;
            }
            class_decl(prog, "");
        }
        return prog;
    }

    void using_decl(ast_program& prog) {
        adv();  // using
        if (is_kw("static")) {
            adv();
            note_feature("using_static");
        }
        if (is(tok_kind::name) && at(1).kind == tok_kind::assign) {
            // using X = Y.Z — alias
            adv();
            adv();
            std::string path = dotted_name();
            prog.using_aliases.push_back(std::move(path));
            note_feature("using_alias");
            expect(tok_kind::semicolon, "';'");
            return;
        }
        std::string path = dotted_name();
        if (!path.empty())
            prog.usings.push_back(path);
        expect(tok_kind::semicolon, "';' after using");
    }

    std::string dotted_name() {
        std::string out;
        if (!is(tok_kind::name) && !is(tok_kind::keyword))
            return out;
        out = cur().text;
        adv();
        while (take(tok_kind::dot)) {
            if (!is(tok_kind::name) && !is(tok_kind::keyword))
                fail("expected name after '.'");
            out.push_back('.');
            out += cur().text;
            adv();
        }
        return out;
    }

    void ns_decl(ast_program& prog, const std::string& outer) {
        adv();  // namespace
        std::string ns = dotted_name();
        if (!outer.empty())
            ns = outer + (ns.empty() ? "" : ".") + ns;
        if (take(tok_kind::semicolon)) {
            // file-scoped namespace — rest of file is the body
            while (!eof())
                class_decl(prog, ns);
            return;
        }
        expect(tok_kind::lbrace, "'{' after namespace");
        while (!is(tok_kind::rbrace) && !eof()) {
            if (is_kw("namespace")) {
                ns_decl(prog, ns);
                continue;
            }
            if (is_kw("using")) {
                using_decl(prog);
                continue;
            }
            class_decl(prog, ns);
        }
        expect(tok_kind::rbrace, "'}' after namespace");
    }

    // attribute list — record feature + skip `[...]` (multiple allowed).
    void skip_attributes() {
        while (is(tok_kind::lbracket)) {
            // attribute lists occur at member/decl level — an `[` that is
            // array-index syntax never reaches this spot (this is only
            // called where a member/decl starts).
            note_feature("attributes");
            int depth = 0;
            do {
                if (is(tok_kind::lbracket))
                    ++depth;
                else if (is(tok_kind::rbracket))
                    --depth;
                else if (is(tok_kind::eof_))
                    fail("unterminated attribute list");
                adv();
            } while (depth > 0);
        }
    }

    // declaration-level modifiers: public/private/protected/internal are
    // consumed silently; sealed/abstract/partial are out-of-subset feature
    // notes but still consumed; `static class` is also a feature.
    void skip_decl_modifiers() {
        for (;;) {
            if (take_kw("public") || take_kw("private") ||
                take_kw("protected") || take_kw("internal"))
                continue;
            if (is_kw("sealed") || is_kw("abstract") || is_kw("static") ||
                is_kw("partial") || is_kw("extern") || is_kw("volatile")) {
                note_feature("modifiers");
                adv();
                continue;
            }
            break;
        }
    }

    // ── class members ─────────────────────────────────────────────────
    void class_decl(ast_program& prog, const std::string& ns) {
        skip_attributes();
        skip_decl_modifiers();
        bool is_class = take_kw("class");
        if (!is_class) {
            if (is_kw("interface") || is_kw("enum") || is_kw("struct") ||
                is_kw("record"))
                note_feature("type_kinds");
            else if (is_kw("delegate"))
                note_feature("delegates");
            else
                fail("expected 'class'");
            adv();
        }
        ast_class cls;
        cls.ns = ns;
        cls.pos = pos();
        cls.name = expect_name("class name").text;
        // skip generic params <T,...>
        if (is(tok_kind::lt))
            skip_angle();
        // base list `: Base, IFoo`
        if (take(tok_kind::colon)) {
            note_feature("inheritance");
            for (;;) {
                (void)dotted_name();
                if (is(tok_kind::lt))
                    skip_angle();
                if (!take(tok_kind::comma))
                    break;
            }
        }
        if (is_kw("where"))
            note_feature("generics");   // where T : constraint
        expect(tok_kind::lbrace, "'{' after class name");
        while (!is(tok_kind::rbrace) && !eof())
            member(cls);
        expect(tok_kind::rbrace, "'}' after class body");
        prog.classes.push_back(std::move(cls));
    }

    void skip_angle() {
        // skip a `<...>` span (depth-matched; >> counts as two closes)
        int depth = 0;
        for (;;) {
            const tok_kind k = cur().kind;
            if (k == tok_kind::lt || k == tok_kind::lshift)
                ++depth;
            else if (k == tok_kind::gt) {
                if (--depth == 0) {
                    adv();
                    return;
                }
            } else if (k == tok_kind::rshift) {
                depth -= 2;
                if (depth <= 0) {
                    adv();
                    return;
                }
            } else if (k == tok_kind::eof_)
                fail("unterminated generic list");
            adv();
        }
    }

    // modifiers → returns bitmask flag for static/const/readonly.
    void member(ast_class& cls) {
        skip_attributes();
        ast_member m;
        m.pos = pos();
        for (;;) {
            if (take_kw("static")) {
                m.is_static = true;
                continue;
            }
            if (take_kw("const")) {
                m.is_const = true;
                m.is_static = true;          // const implies static in C#
                continue;
            }
            if (take_kw("readonly")) {
                m.is_readonly = true;
                continue;
            }
            if (take_kw("public") || take_kw("private") ||
                take_kw("protected") || take_kw("internal"))
                continue;
            if (take_kw("new"))
                continue;
            if (is_kw("abstract") || is_kw("virtual") || is_kw("override") ||
                is_kw("sealed") || is_kw("extern") || is_kw("volatile")) {
                note_feature("modifiers");
                adv();
                continue;
            }
            if (take_kw("partial")) {
                note_feature("partial");
                continue;
            }
            if (take_kw("unsafe")) {
                note_feature("unsafe");
                continue;
            }
            if (take_kw("async")) {
                note_feature("async");
                continue;
            }
            break;
        }
        // ctor: name matches class, no return type
        if (is_name(cls.name) && at(1).kind == tok_kind::lparen) {
            m.kind = member_kind::ctor;
            m.name = cur().text;
            adv();
            parse_params(m);
            // ctor init `: this(args)` / `: base(args)`
            if (take(tok_kind::colon)) {
                if (take_kw("this")) {
                    // capture chained args — replay as a synthetic call on the
                    // ctor itself is not needed for the subset; evaluate as
                    // an ordinary call chain at runtime (expr kept).
                    auto call = mk(et::call, pos());
                    auto base = mk(et::this_, pos());
                    call->base = std::move(base);
                    call->name = m.name;
                    call->call_args = arg_list();
                    m.init = std::move(call);
                } else if (take_kw("base")) {
                    note_feature("inheritance");
                    (void)arg_list();
                } else {
                    fail("expected this/base initializer");
                }
            }
            method_body(m);
            cls.members.push_back(std::move(m));
            return;
        }
        // return/field type (offsets are relative to p)
        const std::size_t tend = type_end(0);
        if (tend == std::string::npos || tend == 0)
            fail("expected member type");
        m.type_name = type_text(0, tend);
        p += tend;
        m.name = expect_name("member name").text;
        if (is(tok_kind::lparen)) {
            m.kind = member_kind::method;
            parse_params(m);
            method_body(m);
            cls.members.push_back(std::move(m));
            return;
        }
        // field(s)
        m.kind = member_kind::field;
        for (;;) {
            if (take(tok_kind::assign))
                m.init = assign_expr();
            if (take(tok_kind::arrow)) {
                // property with expression body `Type Name => expr` — treat
                // as computed member (method-like, evaluated per access).
                m.kind = member_kind::method;
                m.expr_body = assign_expr();
            } else if (is(tok_kind::lbrace)) {
                // `{ get; set; }` property — out-of-subset; consume block.
                note_feature("properties");
                adv();
                int depth = 1;
                while (depth > 0 && !eof()) {
                    if (is(tok_kind::lbrace))
                        ++depth;
                    else if (is(tok_kind::rbrace))
                        --depth;
                    adv();
                }
                cls.members.push_back(std::move(m));
                return;
            }
            if (take(tok_kind::comma)) {
                // `int a = 1, b = 2;` — push current then start a sibling
                cls.members.push_back(std::move(m));
                m = ast_member{};
                m.kind = member_kind::field;
                m.is_static = cls.members.back().is_static;
                m.is_const = cls.members.back().is_const;
                m.is_readonly = cls.members.back().is_readonly;
                m.type_name = cls.members.back().type_name;
                m.pos = pos();
                m.name = expect_name("field name").text;
                continue;
            }
            break;
        }
        expect(tok_kind::semicolon, "';' after member");
        cls.members.push_back(std::move(m));
    }

    void parse_params(ast_member& m) {
        expect(tok_kind::lparen, "'('");
        while (!is(tok_kind::rparen) && !eof()) {
            // param modifiers
            if (is_kw("this")) {
                note_feature("extensions");
                adv();
            }
            for (;;) {
                if (is_kw("ref") || is_kw("out") || is_kw("in") ||
                    is_kw("params") || is_kw("scoped")) {
                    note_feature("ref_out_in");
                    adv();
                    continue;
                }
                if (take_kw("readonly"))
                    continue;
                break;
            }
            const std::size_t tend = type_end(0);
            ast_param pa;
            if (tend == std::string::npos || tend == 0) {
                // bare name (no type) — tolerate
                pa.name = expect_name("param name").text;
            } else {
                pa.type = type_text(0, tend);
                p += tend;
                pa.name = expect_name("param name").text;
            }
            if (take(tok_kind::assign))
                pa.default_value = assign_expr();
            m.params.push_back(std::move(pa));
            if (!take(tok_kind::comma))
                break;
        }
        expect(tok_kind::rparen, "')'");
    }

    void method_body(ast_member& m) {
        if (take(tok_kind::arrow)) {
            m.expr_body = assign_expr();
            expect(tok_kind::semicolon, "';' after expression body");
            return;
        }
        if (take(tok_kind::semicolon))
            return;                       // abstract/extern — no body
        m.body = block_stmt()->body;
    }

    // ── statements ────────────────────────────────────────────────────
    stmt_ptr block_stmt() {
        auto s = ms(st::block, pos());
        expect(tok_kind::lbrace, "'{'");
        while (!is(tok_kind::rbrace) && !eof())
            s->body.push_back(statement());
        expect(tok_kind::rbrace, "'}'");
        return s;
    }

    // try a local-decl start: type_end then `name`/`[`/`(` check.
    bool looks_like_local_decl() const {
        if (!type_token_start(0))
            return false;
        const std::size_t e = type_end(0);
        if (e == std::string::npos)
            return false;
        return e < toks.size() && at(e).kind == tok_kind::name;
    }

    stmt_ptr statement() {
        const src_pos here = pos();
        if (is(tok_kind::lbrace))
            return block_stmt();
        if (is_kw("if"))
            return if_stmt();
        if (is_kw("for"))
            return for_stmt();
        if (is_kw("foreach"))
            return foreach_stmt();
        if (is_kw("while"))
            return while_stmt();
        if (is_kw("do"))
            return do_stmt();
        if (is_kw("return"))
            return return_stmt();
        if (is_kw("break")) {
            adv();
            expect(tok_kind::semicolon, "';'");
            return ms(st::break_, here);
        }
        if (is_kw("continue")) {
            adv();
            expect(tok_kind::semicolon, "';'");
            return ms(st::continue_, here);
        }
        if (is_kw("try"))
            return try_stmt();
        if (is_kw("throw"))
            return throw_stmt();
        if (is_kw("switch") || is_kw("goto") || is_kw("lock")) {
            note_feature("statements");
            adv();
            // consume the statement best-effort (matched braces or to ';')
            if (is(tok_kind::lbrace)) {
                int depth = 0;
                do {
                    if (is(tok_kind::lbrace))
                        ++depth;
                    else if (is(tok_kind::rbrace))
                        --depth;
                    adv();
                } while (depth > 0 && !eof());
            } else {
                while (!is(tok_kind::semicolon) && !eof())
                    adv();
                take(tok_kind::semicolon);
            }
            return ms(st::block, here);
        }
        if (is_kw("checked") || is_kw("unchecked")) {
            adv();
            note_feature("checked");
            if (is(tok_kind::lbrace))
                return block_stmt();
            auto s = statement();
            return s;
        }
        if (is_kw("unsafe") || is_kw("fixed")) {
            adv();
            note_feature("unsafe");
            return statement();
        }
        if (is(tok_kind::semicolon)) {
            adv();
            return ms(st::block, here);
        }
        // local decl vs expression-statement
        if (looks_like_local_decl())
            return local_decl_stmt();
        return expr_or_assign_stmt();
    }

    stmt_ptr local_decl_stmt() {
        const src_pos here = pos();
        auto s = ms(st::local_decl, here);
        const std::size_t tend = type_end(0);
        s->type_name = type_text(0, tend);
        p += tend;
        for (;;) {
            const std::string n = expect_name("variable name").text;
            expr_ptr init;
            if (take(tok_kind::assign))
                init = assign_expr();
            s->names.emplace_back(n, std::move(init));
            if (!take(tok_kind::comma))
                break;
        }
        expect(tok_kind::semicolon, "';' after local declaration");
        return s;
    }

    stmt_ptr expr_or_assign_stmt() {
        const src_pos here = pos();
        expr_ptr e = expr();
        // ++/--/assignment already folded by expr(); just expect ';'
        expect(tok_kind::semicolon, "';' after expression");
        auto s = ms(st::expr_stmt, here);
        s->value = std::move(e);
        return s;
    }

    stmt_ptr if_stmt() {
        const src_pos here = pos();
        adv();  // if
        expect(tok_kind::lparen, "'('");
        expr_ptr cond = assign_expr();
        expect(tok_kind::rparen, "')'");
        auto s = ms(st::if_, here);
        s->value = std::move(cond);
        s->body = stmt_or_block();
        if (take_kw("else"))
            s->orelse = stmt_or_block();
        return s;
    }

    std::vector<stmt_ptr> stmt_or_block() {
        std::vector<stmt_ptr> out;
        if (is(tok_kind::lbrace)) {
            out = block_stmt()->body;
        } else {
            out.push_back(statement());
        }
        return out;
    }

    stmt_ptr for_stmt() {
        const src_pos here = pos();
        adv();  // for
        expect(tok_kind::lparen, "'('");
        auto s = ms(st::for_, here);
        // init: local-decl-like or expr list
        if (is(tok_kind::semicolon)) {
            adv();
        } else if (looks_like_local_decl()) {
            auto d = local_decl_stmt();
            s->init.push_back(std::move(d));
        } else {
            for (;;) {
                auto is_ = ms(st::expr_stmt, pos());
                is_->value = expr();
                s->init.push_back(std::move(is_));
                if (!take(tok_kind::comma))
                    break;
            }
            expect(tok_kind::semicolon, "';' after for-init");
        }
        if (!is(tok_kind::semicolon))
            s->value = assign_expr();     // condition
        expect(tok_kind::semicolon, "';' after for-cond");
        if (!is(tok_kind::rparen)) {
            for (;;) {
                auto it = ms(st::expr_stmt, pos());
                it->value = expr();
                s->iter.push_back(std::move(it));
                if (!take(tok_kind::comma))
                    break;
            }
        }
        expect(tok_kind::rparen, "')'");
        s->body = stmt_or_block();
        return s;
    }

    stmt_ptr foreach_stmt() {
        const src_pos here = pos();
        adv();  // foreach
        expect(tok_kind::lparen, "'('");
        auto s = ms(st::foreach_, here);
        const std::size_t tend = type_end(0);
        if (tend != std::string::npos && tend > 0) {
            s->type_name = type_text(0, tend);
            p += tend;
        }
        s->name = expect_name("loop variable").text;
        if (!take_kw("in"))
            fail("expected 'in' in foreach");
        s->value = assign_expr();         // iterable
        expect(tok_kind::rparen, "')'");
        s->body = stmt_or_block();
        return s;
    }

    stmt_ptr while_stmt() {
        const src_pos here = pos();
        adv();  // while
        expect(tok_kind::lparen, "'('");
        auto s = ms(st::while_, here);
        s->value = assign_expr();
        expect(tok_kind::rparen, "')'");
        s->body = stmt_or_block();
        return s;
    }

    stmt_ptr do_stmt() {
        const src_pos here = pos();
        adv();  // do
        auto s = ms(st::do_, here);
        s->body = stmt_or_block();
        if (!take_kw("while"))
            fail("expected 'while' after do-block");
        expect(tok_kind::lparen, "'('");
        s->value = assign_expr();
        expect(tok_kind::rparen, "')'");
        expect(tok_kind::semicolon, "';'");
        return s;
    }

    stmt_ptr return_stmt() {
        const src_pos here = pos();
        adv();  // return
        auto s = ms(st::return_, here);
        if (!is(tok_kind::semicolon))
            s->value = assign_expr();
        expect(tok_kind::semicolon, "';'");
        return s;
    }

    stmt_ptr try_stmt() {
        const src_pos here = pos();
        adv();  // try
        auto s = ms(st::try_, here);
        s->body = block_stmt()->body;
        while (take_kw("catch")) {
            catch_arm arm;
            if (take(tok_kind::lparen)) {
                // catch (Type e) / catch (Type) / catch when-tagged? (skip `when`)
                if (!is(tok_kind::rparen)) {
                    const std::size_t tend = type_end(0);
                    arm.type_name =
                        tend == std::string::npos ? "" : type_text(0, tend);
                    p += tend;
                    if (is(tok_kind::name))
                        arm.name = cur().text, adv();
                }
                expect(tok_kind::rparen, "')' after catch type");
            }
            if (take_kw("when")) {
                note_feature("exception_filters");
                expect(tok_kind::lparen, "'('");
                arm.filter = assign_expr();   // evaluated at match time
                expect(tok_kind::rparen, "')'");
            }
            arm.body = block_stmt()->body;
            s->catches.push_back(std::move(arm));
        }
        if (take_kw("finally"))
            s->final = block_stmt()->body;
        if (s->catches.empty() && s->final.empty())
            fail("try requires catch or finally");
        return s;
    }

    stmt_ptr throw_stmt() {
        const src_pos here = pos();
        adv();  // throw
        auto s = ms(st::throw_, here);
        if (!is(tok_kind::semicolon))
            s->value = assign_expr();
        expect(tok_kind::semicolon, "';'");
        return s;
    }

    // ── expressions ───────────────────────────────────────────────────
    expr_ptr expr() { return assign_expr(); }

    expr_ptr assign_expr() {
        expr_ptr lhs = cond_expr();
        const tok_kind k = cur().kind;
        if (is_assign_op(k)) {
            adv();
            expr_ptr rhs = assign_expr();          // right-assoc
            auto n = mk(et::binop, lhs->pos);
            n->op = k;
            n->base = std::move(lhs);
            n->parts.push_back(std::move(rhs));
            return n;
        }
        return lhs;
    }

    expr_ptr cond_expr() {
        expr_ptr c = binary(0);
        if (take(tok_kind::qmark)) {
            auto t = mk(et::ternary, c->pos);
            t->index = std::move(c);
            t->base = assign_expr();
            expect(tok_kind::colon, "':' in conditional");
            t->orelse = assign_expr();
            return t;
        }
        return c;
    }

    // precedence table via index
    int prec(tok_kind k) const {
        switch (k) {
        case tok_kind::nullcoalesce: return 1;
        case tok_kind::or2: return 2;
        case tok_kind::and2: return 3;
        case tok_kind::pipe: return 4;
        case tok_kind::caret: return 5;
        case tok_kind::amp: return 6;
        case tok_kind::eq: case tok_kind::ne: return 7;
        case tok_kind::lt: case tok_kind::le: case tok_kind::gt:
        case tok_kind::ge: return 8;
        case tok_kind::lshift: case tok_kind::rshift: return 9;
        case tok_kind::plus: case tok_kind::minus: return 10;
        case tok_kind::star: case tok_kind::slash: case tok_kind::percent:
            return 11;
        default:
            return -1;
        }
    }

    expr_ptr binary(int min_prec) {
        expr_ptr lhs = unary();
        for (;;) {
            // `is` / `as` keywords act at relational precedence
            if (is_kw("is") || is_kw("as")) {
                if (8 < min_prec)
                    break;
                note_feature("type_test");
                const bool is_as = is_kw("as");
                adv();          // exactly one keyword consumed
                auto n = mk(et::throw_unsupported, lhs->pos);
                n->name = is_as ? "as" : "is";
                n->base = std::move(lhs);
                auto t = unary();
                n->parts.push_back(std::move(t));
                lhs = std::move(n);
                continue;
            }
            const tok_kind k = cur().kind;
            const int pr = prec(k);
            if (pr < min_prec || pr < 0)
                break;
            adv();
            expr_ptr rhs = binary(pr + 1);
            auto n = mk(et::binop, lhs->pos);
            n->op = k;
            n->base = std::move(lhs);
            n->parts.push_back(std::move(rhs));
            lhs = std::move(n);
        }
        return lhs;
    }

    expr_ptr unary() {
        const src_pos here = pos();
        if (take(tok_kind::bang)) {
            auto e = mk(et::unop, here);
            e->op = tok_kind::bang;
            e->base = unary();
            return e;
        }
        if (take(tok_kind::minus)) {
            auto e = mk(et::unop, here);
            e->op = tok_kind::minus;
            e->base = unary();
            return e;
        }
        if (take(tok_kind::plus)) {
            auto e = mk(et::unop, here);
            e->op = tok_kind::plus;
            e->base = unary();
            return e;
        }
        if (take(tok_kind::tilde)) {
            auto e = mk(et::unop, here);
            e->op = tok_kind::tilde;
            e->base = unary();
            return e;
        }
        if (take(tok_kind::plus2)) {
            auto e = mk(et::unop, here);
            e->op = tok_kind::plus2;
            e->base = unary();
            return e;
        }
        if (take(tok_kind::minus2)) {
            auto e = mk(et::unop, here);
            e->op = tok_kind::minus2;
            e->base = unary();
            return e;
        }
        // cast: (T)expr — tentative type scan
        if (is(tok_kind::lparen)) {
            const std::size_t save = p;
            adv();
            const std::size_t tend = type_end(0);
            if (tend != std::string::npos && tend > 0 &&
                at(tend).kind == tok_kind::rparen &&
                unary_starts(tend + 1)) {
                auto e = mk(et::cast, here);
                e->name = type_text(0, tend);
                p += tend + 1;
                e->base = unary();
                return e;
            }
            p = save;
        }
        return postfix();
    }

    bool unary_starts(std::size_t k) const {
        const token& t = at(k);
        switch (t.kind) {
        case tok_kind::name: case tok_kind::number: case tok_kind::string:
        case tok_kind::char_: case tok_kind::interp_string:
        case tok_kind::verbatim_string:
        case tok_kind::lparen: case tok_kind::bang: case tok_kind::minus:
        case tok_kind::plus: case tok_kind::tilde: case tok_kind::plus2:
        case tok_kind::minus2:
            return true;
        case tok_kind::keyword:
            return t.text == "new" || t.text == "this" || t.text == "typeof" ||
                   t.text == "nameof" || t.text == "default" ||
                   t.text == "checked" || t.text == "unchecked" ||
                   t.text == "true" || t.text == "false" || t.text == "null";
        default:
            return false;
        }
    }

    expr_ptr postfix() {
        expr_ptr e = primary();
        // once a `?.`/`?[` appears, the rest of the spine is the conditional
        // continuation — mark every downstream accessor so eval can bail on
        // the first null result instead of running the remaining chain.
        bool cond = false;
        for (;;) {
            const src_pos here = pos();
            if (take(tok_kind::dot)) {
                auto m = mk(et::member, here);
                m->base = std::move(e);
                m->name = member_name();
                m->post = cond;            // continuation of a ?. chain
                e = std::move(m);
                continue;
            }
            if (take(tok_kind::nullcond)) {
                auto m = mk(et::member, here);
                m->base = std::move(e);
                m->name = member_name();
                m->post = true;            // null-conditional flag
                cond = true;
                e = std::move(m);
                continue;
            }
            if (take(tok_kind::qmark)) {
                if (take(tok_kind::lbracket)) {
                    auto m = mk(et::index, here);
                    m->base = std::move(e);
                    m->index = assign_expr();
                    m->post = true;        // conditional index
                    cond = true;
                    expect(tok_kind::rbracket, "']'");
                    e = std::move(m);
                    continue;
                }
                p -= 1;                    // not `?[` — rewind (ternary `?`)
                break;
            }
            if (take(tok_kind::lbracket)) {
                auto m = mk(et::index, here);
                m->base = std::move(e);
                m->index = assign_expr();
                m->post = cond;            // continuation of a ?. chain
                expect(tok_kind::rbracket, "']'");
                e = std::move(m);
                continue;
            }
            if (take(tok_kind::lparen)) {
                p -= 1;
                auto c = mk(et::call, here);
                c->base = std::move(e);
                c->call_args = arg_list();
                c->post = cond;            // continuation of a ?. chain
                e = std::move(c);
                continue;
            }
            if (take(tok_kind::plus2)) {
                auto u = mk(et::unop, here);
                u->op = tok_kind::plus2;
                u->post = true;
                u->base = std::move(e);
                e = std::move(u);
                continue;
            }
            if (take(tok_kind::minus2)) {
                auto u = mk(et::unop, here);
                u->op = tok_kind::minus2;
                u->post = true;
                u->base = std::move(e);
                e = std::move(u);
                continue;
            }
            if (is(tok_kind::bang) && at(1).kind != tok_kind::assign) {
                // null-forgiving `!` postfix — transparent
                adv();
                continue;
            }
            break;
        }
        return e;
    }

    std::string member_name() {
        if (!is(tok_kind::name) && !is(tok_kind::keyword))
            fail("expected member name");
        const std::string n = cur().text;
        adv();
        return n;
    }

    std::vector<expr_ptr> arg_list() {
        std::vector<expr_ptr> args;
        expect(tok_kind::lparen, "'('");
        while (!is(tok_kind::rparen) && !eof()) {
            // named args `name:` — parse but discard the name (positional)
            if (is(tok_kind::name) && at(1).kind == tok_kind::colon &&
                at(2).kind != tok_kind::colon) {
                adv();
                adv();
            }
            args.push_back(assign_expr());
            if (!take(tok_kind::comma))
                break;
        }
        expect(tok_kind::rparen, "')'");
        return args;
    }

    expr_ptr primary() {
        const src_pos here = pos();
        const token& t = cur();
        switch (t.kind) {
        case tok_kind::number: {
            auto e = mk(et::literal, here);
            if (t.is_float)
                e->const_value = cs_float(t.num_value);
            else
                e->const_value = cs_int(t.int_value);
            adv();
            return e;
        }
        case tok_kind::string:
        case tok_kind::verbatim_string: {
            auto e = mk(et::literal, here);
            e->const_value = cs_str(t.text);
            adv();
            return e;
        }
        case tok_kind::char_: {
            auto e = mk(et::literal, here);
            e->const_value = cs_char(t.int_value);
            adv();
            return e;
        }
        case tok_kind::interp_string: {
            auto e = mk(et::interp, here);
            adv();
            for (const interp_part& part : split_interp(t.text, t.verbatim)) {
                if (!part.is_expr) {
                    auto lit = mk(et::literal, here);
                    lit->const_value = cs_str(part.text);
                    e->parts.push_back(std::move(lit));
                } else {
                    // re-enter the expression parser on the island source
                    const std::vector<token> sub =
                        lex_source(part.text, file);
                    parser sub_p{sub, file, flags, 0};
                    e->parts.push_back(sub_p.assign_expr());
                }
            }
            return e;
        }
        case tok_kind::name: {
            auto e = mk(et::name, here);
            e->name = t.text;
            adv();
            return e;
        }
        case tok_kind::lparen: {
            adv();
            expr_ptr inner = assign_expr();
            expect(tok_kind::rparen, "')'");
            return inner;
        }
        case tok_kind::lbracket: {
            // collection expression `[a, b]` (C# 12) — array literal
            auto e = mk(et::new_expr, here);
            e->name = "";
            adv();
            while (!is(tok_kind::rbracket) && !eof()) {
                e->parts.push_back(assign_expr());
                if (!take(tok_kind::comma))
                    break;
            }
            expect(tok_kind::rbracket, "']'");
            return e;
        }
        case tok_kind::keyword: {
            if (t.text == "true" || t.text == "false") {
                auto e = mk(et::literal, here);
                e->const_value = cs_bool(t.text == "true");
                adv();
                return e;
            }
            if (t.text == "null") {
                auto e = mk(et::literal, here);
                e->const_value = cs_null();
                adv();
                return e;
            }
            if (t.text == "this") {
                auto e = mk(et::this_, here);
                adv();
                return e;
            }
            if (t.text == "base") {
                note_feature("inheritance");
                auto e = mk(et::this_, here);
                adv();
                return e;
            }
            if (t.text == "new")
                return new_expr();
            if (t.text == "typeof" || t.text == "nameof" ||
                t.text == "default" || t.text == "sizeof")
                return special_expr();
            if (t.text == "checked" || t.text == "unchecked") {
                adv();
                note_feature("checked");
                expect(tok_kind::lparen, "'('");
                expr_ptr inner = assign_expr();
                expect(tok_kind::rparen, "')'");
                return inner;
            }
            if (t.text == "switch") {
                // switch expression `x switch { ... }` — out-of-subset
                note_feature("switch_expr");
                adv();
                auto e = mk(et::literal, here);
                e->const_value = cs_null();
                return e;
            }
            if (t.text == "await") {
                note_feature("async");
                adv();
                return unary();
            }
            if (t.text == "throw") {
                auto n = mk(et::throw_unsupported, here);
                n->name = "throw_expr";
                adv();
                n->base = unary();
                return n;
            }
            // primitive type keywords usable as expression roots
            // (string.Format / int.Parse / object etc.)
            if (is_type_keyword(t.text)) {
                auto e = mk(et::name, here);
                e->name = t.text;
                adv();
                return e;
            }
            fail(std::string("unexpected keyword '") + t.text + "'");
        }
        default:
            fail(std::string("unexpected token in expression"));
        }
    }

    static bool is_type_keyword(std::string_view w) {
        static const std::unordered_set<std::string_view> type_kws = {
            "int", "long", "double", "float", "decimal", "bool", "string",
            "char", "byte", "sbyte", "short", "ushort", "uint", "ulong",
            "object", "var", "dynamic", "void", "nint", "nuint",
        };
        return type_kws.count(w) != 0;
    }

    expr_ptr special_expr() {
        const src_pos here = pos();
        if (take_kw("typeof")) {
            auto e = mk(et::typeof_, here);
            expect(tok_kind::lparen, "'('");
            // keep the type spelling verbatim (typeof is a placeholder value)
            int depth = 1;
            std::string inner;
            while (depth > 0 && !eof()) {
                if (is(tok_kind::lparen)) {
                    ++depth;
                    inner.push_back('(');
                } else if (is(tok_kind::rparen)) {
                    if (--depth == 0) {
                        adv();
                        break;
                    }
                    inner.push_back(')');
                } else {
                    if (!inner.empty() &&
                        (is(tok_kind::name) || is(tok_kind::keyword) ||
                         is(tok_kind::dot)))
                        inner.push_back(' ');
                    inner += cur().text;
                }
                adv();
            }
            e->name = std::move(inner);
            return e;
        }
        if (take_kw("nameof")) {
            auto e = mk(et::literal, here);
            expect(tok_kind::lparen, "'('");
            // capture the last identifier-ish token before matching ')'
            int depth = 1;
            std::string last;
            while (depth > 0 && !eof()) {
                if (is(tok_kind::lparen))
                    ++depth;
                else if (is(tok_kind::rparen)) {
                    if (--depth == 0) {
                        adv();
                        break;
                    }
                } else if (is(tok_kind::name) || is(tok_kind::keyword)) {
                    last = cur().text;
                }
                adv();
            }
            e->const_value = cs_str(last);
            return e;
        }
        if (take_kw("default")) {
            auto e = mk(et::default_, here);
            if (take(tok_kind::lparen)) {
                const std::size_t tend = type_end(0);
                e->name =
                    tend == std::string::npos ? "" : type_text(0, tend);
                p += tend;
                expect(tok_kind::rparen, "')'");
            }
            return e;
        }
        if (take_kw("sizeof")) {
            auto e = mk(et::throw_unsupported, here);
            e->name = "sizeof";
            expect(tok_kind::lparen, "'('");
            e->base = assign_expr();
            expect(tok_kind::rparen, "')'");
            return e;
        }
        fail("internal: bad special expr");
    }

    expr_ptr new_expr() {
        const src_pos here = pos();
        adv();  // new
        auto e = mk(et::new_expr, here);
        // optional full type spelling; `new(args)` alone → anonymous bag
        const std::size_t tend = type_end(0);
        if (tend != std::string::npos && tend > 0) {
            e->name = type_text(0, tend);
            p += tend;
        }
        if (is(tok_kind::lbracket) && e->name.empty()) {
            // `new[] {a, b}` / `new[]` handled below via init block
            adv();
        }
        if (is(tok_kind::lparen)) {
            // sized array `new T[n]` handled when `[` follows a type
            // (`type_end` already consumed `[]` suffixes) — distinguish
            // `new int[5]` (size) from `new int[]` (init set) below.
            e->call_args = arg_list();
        }
        if (is(tok_kind::lbracket)) {
            // `new T[` — sized or rank array: `[n]` | `[,]` — the trailing
            // init `{...}` comes next; empty `[]` was consumed by type_end.
            if (!is(tok_kind::rbracket)) {
                e->index = assign_expr();         // sized dimension
            }
            expect(tok_kind::rbracket, "']'");
            while (is(tok_kind::lbracket)) {
                // additional rank/jag dims — consume best effort
                adv();
                if (!is(tok_kind::rbracket))
                    (void)assign_expr();
                expect(tok_kind::rbracket, "']'");
            }
            if (e->name.empty())
                e->name = "[]";
        }
        if (is(tok_kind::lbrace)) {
            // collection / object initializer
            adv();
            while (!is(tok_kind::rbrace) && !eof()) {
                if (take(tok_kind::lbracket)) {
                    // indexer init `{ ["k"] = v }` — two parts tagged "@key"/"@val"
                    e->init_names.push_back("@key");
                    e->parts.push_back(assign_expr());
                    expect(tok_kind::rbracket, "']'");
                    expect(tok_kind::assign, "'='");
                    e->init_names.push_back("@val");
                    e->parts.push_back(assign_expr());
                } else if (is(tok_kind::name) && at(1).kind == tok_kind::assign) {
                    // member init `{ Name = v }`
                    e->init_names.push_back(cur().text);
                    adv();
                    adv();
                    e->parts.push_back(assign_expr());
                } else {
                    // collection add `{ a, b }`
                    e->init_names.emplace_back();
                    e->parts.push_back(assign_expr());
                }
                if (!take(tok_kind::comma))
                    break;
            }
            expect(tok_kind::rbrace, "'}' after initializer");
        }
        return e;
    }
};

} // namespace

ast_program parse_source(std::string_view source, const std::string& file,
                         feature_flags* flags) {
    feature_flags local{};
    if (flags == nullptr)
        flags = &local;
    const std::vector<token> toks = lex_source(source, file);
    parser p{toks, file, flags, 0};
    return p.program();
}

// ── subset preflight (shared by bridge + loader adapter) ─────────────
// Cheap token scan: false when the source reaches outside the subset —
// unsafe/pointers, ref/out/in param mods, generics beyond List/Dictionary,
// LINQ, async, partial, extension methods, attributes, out-of-subset usings.
bool csmini_preflight_subset(std::string_view src, std::string* out_reason) {
    auto fail = [&](std::string why) {
        if (out_reason)
            *out_reason = std::move(why);
        return false;
    };
    std::vector<token> toks;
    try {
        toks = lex_source(src, "<preflight>");
    } catch (const cs_error& e) {
        return fail(std::string("lex: ") + e.message);
    }
    auto& T = toks;
    const auto is_kw = [&](std::size_t k, std::string_view w) {
        return T[k].kind == tok_kind::keyword && T[k].text == w;
    };
    // allowed `using` roots (everything else → namespace_<seg>)
    static const std::unordered_set<std::string> allowed_usings = {
        "",                                          // global
        "System",
        "System.Text",
        "System.Text.Json",
        "System.Collections",
        "System.Collections.Generic",
        "System.ComponentModel",
        "System.Buffers",
        "csmini",
    };
    static const std::unordered_map<std::string, std::string> ns_features = {
        {"System.IO", "io"},
        {"System.Net", "net"},
        {"System.Reflection", "reflection"},
        {"System.Runtime", "interop"},
        {"System.Threading", "threading"},
        {"System.Diagnostics", "diagnostics"},
        {"Microsoft", "interop"},
        {"System.Windows", "ui"},
        {"Newtonsoft", "external"},
        {"System.Runtime.InteropServices", "interop"},
    };
    for (std::size_t k = 0; k < T.size(); ++k) {
        const token& t = T[k];
        if (t.kind != tok_kind::keyword && t.kind != tok_kind::lbracket &&
            t.kind != tok_kind::name)
            continue;
        const std::string_view w = t.text;
        if (t.kind == tok_kind::keyword) {
            if (w == "unsafe" || w == "fixed" || w == "stackalloc")
                return fail("unsafe");
            if (w == "async" || w == "await")
                return fail("async");
            if (w == "partial")
                return fail("partial");
            if (w == "yield")
                return fail("generators");
            if (w == "delegate" || w == "event")
                return fail("delegates");
            if (w == "switch")
                return fail("switch");
            if (w == "goto")
                return fail("goto");
            if (w == "lock")
                return fail("lock");
            if (w == "checked" || w == "unchecked")
                return fail("checked");
            if (w == "interface" || w == "enum" || w == "struct" ||
                w == "record")
                return fail("type_kinds");
            if (w == "abstract" || w == "virtual" || w == "override" ||
                w == "sealed" || w == "extern" || w == "volatile")
                return fail("modifiers");
            if (w == "operator" || w == "implicit" || w == "explicit")
                return fail("operators");
            if (w == "is" || w == "as")
                return fail("type_test");
            if (w == "ref" || w == "out" || w == "params" || w == "scoped")
                return fail("ref_out_in");
            if (w == "in") {
                // `in` is legit inside `foreach (T x in y)` only — walk back
                // to the nearest `(` and require `foreach` before it.
                int depth = 1;
                bool ok_in = false;
                for (std::size_t b = k; b-- > 0;) {
                    if (T[b].kind == tok_kind::rparen ||
                        T[b].kind == tok_kind::rbracket)
                        ++depth;
                    else if (T[b].kind == tok_kind::lparen ||
                             T[b].kind == tok_kind::lbracket) {
                        if (--depth == 0) {
                            if (b > 0 && is_kw(b - 1, "foreach"))
                                ok_in = true;
                            break;
                        }
                    }
                }
                if (!ok_in)
                    return fail("ref_out_in");
            }
            if (w == "select" || w == "join" || w == "orderby" ||
                w == "group" || w == "into" || w == "let" || w == "from" ||
                w == "where")
                return fail("linq");
            if (w == "this") {
                // `this` as the first param of a static method → extension
                if (k + 1 < T.size() && is_kw(k + 1, "this"))
                    continue;
                if (k > 0 && T[k - 1].kind == tok_kind::lparen) {
                    // cheap extension check: `(` then `this` directly
                    return fail("extensions");
                }
            }
            if (w == "using") {
                // using X.Y.Z; — check the dotted path
                std::size_t j = k + 1;
                std::string path;
                while (j < T.size() &&
                       (T[j].kind == tok_kind::name ||
                        T[j].kind == tok_kind::keyword ||
                        T[j].kind == tok_kind::dot)) {
                    if (T[j].kind == tok_kind::dot)
                        path.push_back('.');
                    else
                        path += T[j].text;
                    ++j;
                }
                if (path.empty() || path.back() == '.')
                    path.pop_back();
                if (auto it = ns_features.find(path); it != ns_features.end())
                    return fail(it->second);
                if (path.substr(0, 7) == "System." &&
                    ns_features.count(path.substr(0, path.rfind('.')))) {
                    // nested System.X.Y where System.X is flagged
                    std::string seg = path.substr(0, path.rfind('.'));
                    if (auto it2 = ns_features.find(seg); it2 != ns_features.end())
                        return fail(it2->second);
                }
                if (!allowed_usings.count(path))
                    return fail(std::string("namespace_") +
                                path.substr(0, path.find('.')));
            }
            if (w == "new") {
                // `new X<T>` where X ∉ {List,Dictionary} → generics
                std::size_t j = k + 1;
                if (j < T.size() && T[j].kind == tok_kind::name) {
                    const std::string tn = T[j].text;
                    if (j + 1 < T.size() && T[j + 1].kind == tok_kind::lt &&
                        tn != "List" && tn != "Dictionary" &&
                        tn != "SortedList" && tn != "SortedDictionary" &&
                        tn != "HashSet" && tn != "Queue" && tn != "Stack" &&
                        tn != "KeyValuePair" && tn != "Tuple" &&
                        tn != "Nullable")
                        return fail("generics");
                }
            }
            if (w == "this") {
                // `this.X` member access is fine — skip the marker above;
                // only param-position `this` flags extensions (handled).
            }
            continue;
        }
        if (t.kind == tok_kind::lbracket) {
            // attribute list `[Name]`/`[Name(...)]` at decl/member level:
            // heuristic — `[` when previous token ends a declaration scope.
            if (k + 1 < T.size() &&
                (T[k + 1].kind == tok_kind::name ||
                 T[k + 1].kind == tok_kind::keyword)) {
                const std::string& w2 = T[k + 1].text;
                const bool attr_name =
                    !w2.empty() && (w2[0] >= 'A' && w2[0] <= 'Z');
                const tok_kind pk = k > 0 ? T[k - 1].kind : tok_kind::eof_;
                const bool decl_ctx =
                    pk == tok_kind::rbrace || pk == tok_kind::semicolon ||
                    pk == tok_kind::eof_ || pk == tok_kind::rparen ||
                    pk == tok_kind::gt || pk == tok_kind::lbrace;
                if (attr_name && decl_ctx)
                    return fail("attributes");
            }
            continue;
        }
        if (t.kind == tok_kind::name) {
            if (t.text == "Enumerable" || t.text == "ParallelEnumerable")
                return fail("linq");
            if (t.text == "unsafe" || t.text == "Marshal" ||
                t.text == "GCHandle" || t.text == "IntPtr" ||
                t.text == "UIntPtr")
                return fail("unsafe");
            if (t.text == "Task" || t.text == "ValueTask")
                return fail("async");
            if (t.text == "Guid")
                return fail("guid");
            continue;
        }
    }
    return true;
}

} // namespace sao::plugins::csmini
