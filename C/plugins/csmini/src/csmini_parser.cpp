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

#include <algorithm>
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
    const token& at(std::size_t k) const { return toks[p + (std::min)(k, toks.size() - 1 - p)]; }
    bool eof() const { return cur().kind == tok_kind::eof_; }
    src_pos pos() const { return cur().pos; }
    bool is(tok_kind k) const { return cur().kind == k; }
    bool is_kw(std::string_view w) const {
        return cur().kind == tok_kind::keyword && cur().text == w;
    }
    bool is_name(std::string_view w) const {
        return identifier(cur()) && cur().text == w;
    }
    static bool identifier(const token& t) {
        static const std::unordered_set<std::string_view> contextual = {
            "var", "dynamic", "nameof", "partial", "async", "await",
            "get", "set", "init", "value", "where", "select", "from",
            "join", "orderby", "group", "into", "let", "yield", "record",
            "required", "nint", "nuint", "scoped", "file",
        };
        return t.kind == tok_kind::name ||
               (t.kind == tok_kind::keyword && contextual.count(t.text));
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
    [[noreturn]] void unsupported(const char* feature) {
        note_feature(feature);
        cs_error e;
        e.kind = "UnsupportedFeature";
        e.message = std::string("unsupported C# subset feature: ") + feature;
        e.file = file;
        e.pos = pos();
        e.features.push_back(feature);
        throw e;
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
        if (!identifier(cur()))
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
        if (identifier(t))
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
        if (identifier(cur()) && at(1).kind == tok_kind::assign) {
            // using X = Y.Z — alias
            adv();
            adv();
            std::string path = dotted_name();
            if (path.empty())
                fail("expected using alias target");
            prog.using_aliases.push_back(std::move(path));
            note_feature("using_alias");
            expect(tok_kind::semicolon, "';'");
            return;
        }
        std::string path = dotted_name();
        if (path.empty())
            fail("expected namespace after using");
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
        if (ns.empty())
            fail("expected namespace name");
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

    static bool metadata_only_attribute(std::string_view name) {
        const auto dot = name.rfind('.');
        if (dot != std::string_view::npos)
            name.remove_prefix(dot + 1);
        constexpr std::string_view suffix = "Attribute";
        if (name.size() > suffix.size() && name.ends_with(suffix))
            name.remove_suffix(suffix.size());
        static const std::unordered_set<std::string_view> allowed = {
            "Browsable", "Category", "CompilerGenerated", "DebuggerDisplay",
            "DebuggerNonUserCode", "DebuggerStepThrough", "Description",
            "DisplayName", "EditorBrowsable", "ExcludeFromCodeCoverage",
            "GeneratedCode", "Obsolete", "Serializable", "SuppressMessage",
        };
        return allowed.count(name) != 0;
    }

    void skip_attributes() {
        while (is(tok_kind::lbracket)) {
            std::size_t scan = p + 1;
            int square = 1;
            int paren = 0;
            int brace = 0;
            bool expect_name = true;
            while (scan < toks.size() && square > 0) {
                const token& current = toks[scan];
                if (square == 1 && paren == 0 && brace == 0 && expect_name) {
                    if (!identifier(current))
                        fail("expected attribute name", current.pos);
                    std::string name = current.text;
                    ++scan;
                    while (scan + 1 < toks.size() &&
                           toks[scan].kind == tok_kind::dot &&
                           identifier(toks[scan + 1])) {
                        name += "." + toks[scan + 1].text;
                        scan += 2;
                    }
                    if (scan < toks.size() &&
                        toks[scan].kind == tok_kind::colon)
                        unsupported("attribute_targets");
                    if (!metadata_only_attribute(name))
                        unsupported("runtime_attributes");
                    expect_name = false;
                    continue;
                }
                if (current.kind == tok_kind::eof_)
                    fail("unterminated attribute list", current.pos);
                if (current.kind == tok_kind::lbracket)
                    ++square;
                else if (current.kind == tok_kind::rbracket)
                    --square;
                else if (current.kind == tok_kind::lparen)
                    ++paren;
                else if (current.kind == tok_kind::rparen && paren > 0)
                    --paren;
                else if (current.kind == tok_kind::lbrace)
                    ++brace;
                else if (current.kind == tok_kind::rbrace && brace > 0)
                    --brace;
                else if (current.kind == tok_kind::comma && square == 1 &&
                         paren == 0 && brace == 0)
                    expect_name = true;
                ++scan;
            }
            if (square != 0)
                fail("unterminated attribute list");
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

    bool skip_decl_modifiers() {
        bool partial = false;
        for (;;) {
            if (take_kw("public") || take_kw("private") ||
                take_kw("protected") || take_kw("internal"))
                continue;
            if (take_kw("static"))
                continue;
            if (take_kw("partial")) {
                partial = true;
                continue;
            }
            if (take_kw("unsafe")) {
                note_feature("unsafe");
                continue;
            }
            if (take_kw("sealed") || take_kw("abstract"))
                continue;
            if (is_kw("extern") || is_kw("volatile")) {
                note_feature("modifiers");
                adv();
                continue;
            }
            break;
        }
        return partial;
    }

    // ── class members ─────────────────────────────────────────────────
    void class_decl(ast_program& prog, const std::string& ns) {
        skip_attributes();
        const bool partial = skip_decl_modifiers();
        bool is_class = take_kw("class");
        if (!is_class) {
            if (is_kw("interface") || is_kw("enum") || is_kw("struct") ||
                is_kw("record"))
                unsupported("type_kinds");
            else if (is_kw("delegate"))
                unsupported("delegate_declarations");
            else
                fail("expected 'class'");
        }
        ast_class cls;
        cls.ns = ns;
        cls.pos = pos();
        cls.is_partial = partial;
        cls.name = expect_name("class name").text;
        // skip generic params <T,...>
        if (is(tok_kind::lt))
            skip_angle();
        // base list `: Base, IFoo`
        if (take(tok_kind::colon)) {
            note_feature("inheritance");
            for (;;) {
                if (dotted_name().empty())
                    fail("expected base type");
                if (is(tok_kind::lt))
                    skip_angle();
                if (!take(tok_kind::comma))
                    break;
            }
        }
        if (is_kw("where"))
            unsupported("generics");
        expect(tok_kind::lbrace, "'{' after class name");
        while (!is(tok_kind::rbrace) && !eof())
            member(cls);
        expect(tok_kind::rbrace, "'}' after class body");
        for (const auto& existing : prog.classes) {
            if (existing.name == cls.name && existing.ns == cls.ns) {
                if (existing.is_partial || cls.is_partial)
                    unsupported("partial_multiple_declarations");
                fail("duplicate class declaration");
            }
        }
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
            if (take_kw("virtual") || take_kw("override") ||
                take_kw("sealed"))
                continue;
            if (take_kw("abstract")) {
                m.is_abstract = true;
                continue;
            }
            if (take_kw("extern")) {
                m.is_extern = true;
                note_feature("extern_methods");
                continue;
            }
            if (is_kw("volatile")) {
                note_feature("modifiers");
                adv();
                continue;
            }
            if (is_kw("partial") && type_token_start(1) &&
                type_end(1) != std::string::npos &&
                identifier(at(type_end(1)))) {
                adv();
                continue;
            }
            if (take_kw("unsafe")) {
                note_feature("unsafe");
                continue;
            }
            if (is_kw("async") && type_token_start(1) &&
                type_end(1) != std::string::npos &&
                identifier(at(type_end(1)))) {
                adv();
                m.is_async = true;
                continue;
            }
            break;
        }
        if (is_kw("event"))
            unsupported("events");
        if (is_kw("delegate"))
            unsupported("delegate_declarations");
        if (is_kw("implicit") || is_kw("explicit"))
            unsupported("operators");
        if (is_kw("ref"))
            unsupported("ref_out_in");
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
        if (m.is_async) {
            std::string async_type = m.type_name;
            if (const auto angle = async_type.find('<');
                angle != std::string::npos)
                async_type.erase(angle);
            if (const auto dot = async_type.rfind('.');
                dot != std::string::npos)
                async_type.erase(0, dot + 1);
            if (async_type != "void" && async_type != "Task" &&
                async_type != "ValueTask")
                note_feature("async_return_type");
        }
        if (is(tok_kind::star))
            unsupported("unsafe");
        if (is_kw("operator"))
            unsupported("operators");
        m.name = expect_name("member name").text;
        if (is(tok_kind::lt))
            unsupported("generics");
        if (is(tok_kind::lparen)) {
            m.kind = member_kind::method;
            parse_params(m);
            method_body(m);
            cls.members.push_back(std::move(m));
            return;
        }
        if (take(tok_kind::arrow)) {
            m.kind = member_kind::property;
            m.property_has_get = true;
            auto ret = ms(st::return_, m.pos);
            ret->value = assign_expr();
            m.property_get_body.push_back(std::move(ret));
            expect(tok_kind::semicolon, "';' after expression property");
            cls.members.push_back(std::move(m));
            return;
        }
        if (is(tok_kind::lbrace)) {
            parse_property(m);
            if (take(tok_kind::assign)) {
                m.init = assign_expr();
                expect(tok_kind::semicolon, "';' after property initializer");
            } else {
                take(tok_kind::semicolon);
            }
            cls.members.push_back(std::move(m));
            return;
        }
        // field(s)
        m.kind = member_kind::field;
        for (;;) {
            if (take(tok_kind::assign))
                m.init = assign_expr();
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

    void parse_property(ast_member& m) {
        m.kind = member_kind::property;
        expect(tok_kind::lbrace, "'{' in property");
        while (!is(tok_kind::rbrace) && !eof()) {
            skip_attributes();
            while (take_kw("public") || take_kw("private") ||
                   take_kw("protected") || take_kw("internal")) {
            }
            const bool getter = take_kw("get");
            const bool setter = !getter && (take_kw("set") || take_kw("init"));
            if (!getter && !setter) {
                fail("expected get or set accessor");
            }
            if (getter ? m.property_has_get : m.property_has_set)
                fail("duplicate property accessor");
            if (getter)
                m.property_has_get = true;
            else
                m.property_has_set = true;
            if (take(tok_kind::semicolon)) {
                if (getter)
                    m.property_auto_get = true;
                else
                    m.property_auto_set = true;
                continue;
            }
            std::vector<stmt_ptr> body;
            if (take(tok_kind::arrow)) {
                if (getter) {
                    auto ret = ms(st::return_, pos());
                    ret->value = assign_expr();
                    body.push_back(std::move(ret));
                } else {
                    auto expr_stmt = ms(st::expr_stmt, pos());
                    expr_stmt->value = assign_expr();
                    body.push_back(std::move(expr_stmt));
                }
                expect(tok_kind::semicolon, "';' after property accessor");
            } else {
                body = block_stmt()->body;
            }
            if (getter)
                m.property_get_body = std::move(body);
            else
                m.property_set_body = std::move(body);
        }
        expect(tok_kind::rbrace, "'}' after property");
        if (!m.property_has_get && !m.property_has_set)
            fail("property requires an accessor");
        if (m.is_abstract && (m.property_auto_get || m.property_auto_set))
            unsupported("abstract_without_body");
    }

    void parse_params(ast_member& m) {
        expect(tok_kind::lparen, "'('");
        while (!is(tok_kind::rparen) && !eof()) {
            skip_attributes();
            // param modifiers
            if (is_kw("this")) {
                note_feature("extensions");
                adv();
            }
            for (;;) {
                if (is_kw("ref") || is_kw("out") || is_kw("in") ||
                                        is_kw("params") || (is_kw("scoped") &&
                                        ((type_token_start(1) && type_end(1) != std::string::npos &&
                                            identifier(at(type_end(1)))) ||
                                         (at(1).kind == tok_kind::keyword && at(1).text == "ref")))) {
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
                if (is(tok_kind::star))
                    unsupported("unsafe");
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
        if (take(tok_kind::semicolon)) {
            if (m.is_abstract)
                unsupported("abstract_without_body");
            if (m.is_extern)
                unsupported("extern_methods");
            unsupported("method_without_body");
        }
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
        if (is_kw("await") && unary_starts(1))
            return false;
        if (!type_token_start(0))
            return false;
        const std::size_t e = type_end(0);
        if (e == std::string::npos)
            return false;
        return e < toks.size() && identifier(at(e));
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
        if (is_kw("switch"))
            return switch_stmt();
        if (is_kw("goto"))
            unsupported("goto");
        if (is_kw("lock"))
            return lock_stmt();
        if (is_kw("yield") && at(1).kind == tok_kind::keyword &&
            (at(1).text == "return" || at(1).text == "break"))
            unsupported("generators");
        if (is_kw("using"))
            unsupported("using_statement");
        if (is_kw("ref"))
            unsupported("ref_out_in");
        if (is_kw("checked") || is_kw("unchecked")) {
            adv();
            note_feature("checked");
            if (is(tok_kind::lbrace))
                return block_stmt();
            auto s = statement();
            return s;
        }
        if (is_kw("fixed"))
            unsupported("unsafe");
        if (is_kw("unsafe")) {
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

    stmt_ptr lock_stmt() {
        const src_pos here = pos();
        adv();
        expect(tok_kind::lparen, "'(' after lock");
        auto s = ms(st::lock_, here);
        s->value = assign_expr();
        expect(tok_kind::rparen, "')' after lock expression");
        s->body = stmt_or_block();
        return s;
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

    static bool switch_literal(const ast_expr* value) {
        if (!value)
            return false;
        if (value->tag == et::literal)
            return true;
        return value->tag == et::unop &&
               (value->op == tok_kind::plus || value->op == tok_kind::minus) &&
               switch_literal(value->base.get());
    }

    stmt_ptr switch_stmt() {
        const src_pos here = pos();
        adv();
        expect(tok_kind::lparen, "'(' after switch");
        auto s = ms(st::switch_, here);
        s->value = assign_expr();
        expect(tok_kind::rparen, "')' after switch value");
        expect(tok_kind::lbrace, "'{' after switch");
        bool saw_default = false;
        while (!is(tok_kind::rbrace) && !eof()) {
            switch_arm arm;
            bool saw_label = false;
            while (is_kw("case") || is_kw("default")) {
                saw_label = true;
                if (take_kw("default")) {
                    if (saw_default)
                        fail("duplicate default label");
                    saw_default = true;
                    arm.is_default = true;
                    expect(tok_kind::colon, "':' after default");
                    continue;
                }
                adv();
                if (is_kw("var") || is_kw("when") || is_name("_"))
                    unsupported("switch_patterns");
                expr_ptr label = assign_expr();
                if (!switch_literal(label.get()) || !is(tok_kind::colon))
                    unsupported("switch_patterns");
                adv();
                arm.labels.push_back(std::move(label));
            }
            if (!saw_label)
                fail("expected case or default label");
            while (!is(tok_kind::rbrace) && !is_kw("case") &&
                   !is_kw("default"))
                arm.body.push_back(statement());
            s->switch_arms.push_back(std::move(arm));
        }
        expect(tok_kind::rbrace, "'}' after switch");
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
                    if (tend == std::string::npos)
                        fail("expected catch type");
                    arm.type_name = type_text(0, tend);
                    p += tend;
                    if (identifier(cur()))
                        arm.name = cur().text, adv();
                }
                expect(tok_kind::rparen, "')' after catch type");
            }
            if (is_kw("when") || is_name("when")) {
                adv();
                note_feature("exception_filters");
                expect(tok_kind::lparen, "'('");
                arm.filter = assign_expr();   // evaluated at match time
                expect(tok_kind::rparen, "')'");
            }
            arm.body = block_stmt()->body;
            s->catches.push_back(std::move(arm));
        }
        const bool has_finally = take_kw("finally");
        if (has_finally)
            s->final = block_stmt()->body;
        if (s->catches.empty() && !has_finally)
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
        if (lambda_start())
            return lambda_expr();
        expr_ptr lhs = cond_expr();
        if (is_kw("switch"))
            unsupported("switch_expressions");
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

    bool lambda_start() const {
        if (identifier(cur()) && at(1).kind == tok_kind::arrow)
            return true;
        if (!is(tok_kind::lparen))
            return false;
        int depth = 0;
        for (std::size_t k = 0; at(k).kind != tok_kind::eof_; ++k) {
            if (at(k).kind == tok_kind::lparen)
                ++depth;
            else if (at(k).kind == tok_kind::rparen && --depth == 0)
                return at(k + 1).kind == tok_kind::arrow;
        }
        return false;
    }

    expr_ptr lambda_expr() {
        const src_pos here = pos();
        auto lambda = mk(et::lambda_, here);
        if (take(tok_kind::lparen)) {
            while (!is(tok_kind::rparen) && !eof()) {
                if (is_kw("ref") || is_kw("out") || is_kw("in") ||
                    is_kw("params") || is_kw("scoped"))
                    unsupported("lambda_parameter_modifiers");
                ast_lambda_param param;
                const std::size_t tend = type_end(0);
                if (tend != std::string::npos && tend > 0 &&
                    identifier(at(tend)) &&
                    (at(tend + 1).kind == tok_kind::comma ||
                     at(tend + 1).kind == tok_kind::rparen)) {
                    param.type = type_text(0, tend);
                    p += tend;
                }
                param.name = expect_name("lambda parameter").text;
                if (take(tok_kind::assign))
                    unsupported("lambda_parameter_defaults");
                lambda->lambda_params.push_back(std::move(param));
                if (!take(tok_kind::comma))
                    break;
            }
            expect(tok_kind::rparen, "')' after lambda parameters");
        } else {
            ast_lambda_param param;
            param.name = expect_name("lambda parameter").text;
            lambda->lambda_params.push_back(std::move(param));
        }
        expect(tok_kind::arrow, "'=>' after lambda parameters");
        if (is(tok_kind::lbrace)) {
            lambda->lambda_body = block_stmt()->body;
        } else {
            auto ret = ms(st::return_, here);
            ret->value = assign_expr();
            lambda->lambda_body.push_back(std::move(ret));
        }
        return lambda;
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
        if (is_kw("await") && unary_starts(1) &&
            at(1).kind != tok_kind::plus && at(1).kind != tok_kind::minus &&
            at(1).kind != tok_kind::plus2 && at(1).kind != tok_kind::minus2) {
            adv();
            auto e = mk(et::await_, here);
            e->base = unary();
            return e;
        }
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
        if (identifier(t))
            return true;
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
            if (identifier(cur()) && at(1).kind == tok_kind::colon &&
                at(2).kind != tok_kind::colon) {
                adv();
                adv();
            }
            if (is_kw("ref") || is_kw("out") || is_kw("in"))
                unsupported("ref_out_in");
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
                    if (!sub_p.eof())
                        sub_p.fail("unexpected token after interpolation expression");
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
            if (t.text == "typeof" || (t.text == "nameof" && at(1).kind == tok_kind::lparen) ||
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
            if (t.text == "throw") {
                note_feature("throw_expr");
                auto n = mk(et::throw_unsupported, here);
                n->name = "throw_expr";
                adv();
                n->base = unary();
                return n;
            }
            // primitive type keywords usable as expression roots
            // (string.Format / int.Parse / object etc.)
            if (t.text == "stackalloc")
                unsupported("unsafe");
            if (t.text == "delegate")
                unsupported("anonymous_delegates");
            if (t.text == "from" && identifier(at(1)) &&
                at(2).kind == tok_kind::keyword && at(2).text == "in")
                unsupported("query_syntax");
            if (t.text == "from" && type_token_start(1)) {
                const auto end = type_end(1);
                if (end != std::string::npos && identifier(at(end)) &&
                    at(end + 1).kind == tok_kind::keyword && at(end + 1).text == "in")
                    unsupported("query_syntax");
            }
            if (is_type_keyword(t.text) || identifier(t)) {
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
                if (tend == std::string::npos)
                    fail("expected type in default expression");
                e->name = type_text(0, tend);
                p += tend;
                expect(tok_kind::rparen, "')'");
            }
            return e;
        }
        if (take_kw("sizeof")) {
            note_feature("unsafe");
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
            // inferred array `new[] {a, b}`
            adv();
            expect(tok_kind::rbracket, "']' after new[");
            e->name = "[]";
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
                } else if (identifier(cur()) && at(1).kind == tok_kind::assign) {
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

struct subset_classifier {
    feature_flags& flags;
    std::unordered_set<std::string> bindings;
    std::unordered_set<std::string> classes;

    void note(const std::string& feature) {
        if (std::find(flags.unsupported.begin(), flags.unsupported.end(), feature) ==
            flags.unsupported.end())
            flags.unsupported.push_back(feature);
    }
    bool path_feature(std::string_view path) {
        if (path == "System.Runtime.CompilerServices.Unsafe" ||
            path.starts_with("System.Runtime.CompilerServices.Unsafe.")) {
            note("unsafe");
            return true;
        }
        if (path == "System.Threading" || path == "System.Threading.Tasks" ||
            path == "System.Runtime.CompilerServices")
            return false;
        const auto task_surface = [&](std::string_view short_name,
                                      std::string_view qualified_name,
                                      bool value_task) -> int {
            std::string_view member;
            if (path == short_name || path == qualified_name)
                return 0;
            if (path.starts_with(short_name) &&
                path.size() > short_name.size() &&
                path[short_name.size()] == '.')
                member = path.substr(short_name.size() + 1);
            else if (path.starts_with(qualified_name) &&
                     path.size() > qualified_name.size() &&
                     path[qualified_name.size()] == '.')
                member = path.substr(qualified_name.size() + 1);
            else
                return -1;
            const bool supported =
                member == "CompletedTask" ||
                member.starts_with("CompletedTask.") ||
                member == "FromResult" ||
                (!value_task && (member == "Delay" || member == "WhenAll"));
            if (!supported)
                note("async_continuations");
            return supported ? 0 : 1;
        };
        const int task = task_surface("Task", "System.Threading.Tasks.Task", false);
        if (task >= 0)
            return task != 0;
        const int value_task = task_surface(
            "ValueTask", "System.Threading.Tasks.ValueTask", true);
        if (value_task >= 0)
            return value_task != 0;
        static constexpr std::pair<std::string_view, std::string_view> prefixes[] = {
            {"System.IO", "io"}, {"System.Net", "net"},
            {"System.Reflection", "reflection"},
            {"System.Runtime.InteropServices", "interop"},
            {"System.Threading", "threading"}, {"System.Diagnostics", "diagnostics"},
            {"System.Windows", "ui"},
            {"Microsoft", "interop"}, {"Newtonsoft", "external"},
        };
        for (const auto& [prefix, feature] : prefixes) {
            if (path == prefix || (path.starts_with(prefix) &&
                path.size() > prefix.size() && path[prefix.size()] == '.')) {
                note(std::string(feature));
                return true;
            }
        }
        if (path.starts_with("System."))
            path.remove_prefix(7);
        const auto leaf = path.substr(0, path.find('.'));
        if (leaf == "ParallelEnumerable") note("parallel_linq");
        else if (leaf == "TaskCompletionSource" || leaf == "TaskFactory" ||
                 leaf == "SynchronizationContext") note("async_continuations");
        else if (leaf == "Marshal" || leaf == "GCHandle" || leaf == "IntPtr" ||
                 leaf == "UIntPtr") note("unsafe");
        else return false;
        return true;
    }
    void type_feature(std::string_view type) {
        static const std::unordered_set<std::string_view> generic_types = {
            "List", "Dictionary", "SortedList", "SortedDictionary",
            "HashSet", "Queue", "Stack", "KeyValuePair", "Tuple", "Nullable",
            "Action", "Func", "Predicate", "Converter", "Comparison",
            "IEnumerable", "IEnumerator", "ICollection", "IList", "ISet",
            "IReadOnlyCollection", "IReadOnlyList", "IReadOnlySet",
            "Task", "ValueTask", "Lazy", "IComparable", "IEquatable",
        };
        while (!type.empty()) {
            const auto end = type.find_first_of("<>,[]?");
            const auto name = type.substr(0, end);
            if (!name.empty() && !classes.count(std::string(name)))
                path_feature(name);
            if (end == std::string_view::npos)
                break;
            if (type[end] == '<') {
                const auto dot = name.rfind('.');
                const auto leaf = dot == std::string_view::npos ? name : name.substr(dot + 1);
                if (!generic_types.count(leaf) &&
                    !classes.count(std::string(name)) &&
                    !classes.count(std::string(leaf)))
                    note("generics");
            }
            type.remove_prefix(end + 1);
        }
    }
    void expression(const ast_expr* e) {
        if (!e)
            return;
        if (e->tag == et::new_expr || e->tag == et::cast ||
            e->tag == et::default_ || e->tag == et::typeof_)
            type_feature(e->name);
        if (e->tag == et::member || e->tag == et::name) {
            std::string path;
            const ast_expr* root = e;
            while (root && root->tag == et::member) {
                path = "." + root->name + path;
                root = root->base.get();
            }
            if (root && root->tag == et::name && !bindings.count(root->name))
                path_feature(root->name + path);
        }
        expression(e->base.get());
        expression(e->index.get());
        expression(e->orelse.get());
        for (const auto& part : e->parts) expression(part.get());
        for (const auto& arg : e->call_args) expression(arg.get());
        if (e->tag == et::lambda_) {
            const auto outer = bindings;
            for (const auto& param : e->lambda_params) {
                type_feature(param.type);
                bindings.insert(param.name);
            }
            statements(e->lambda_body);
            bindings = outer;
        }
    }
    void statements(const std::vector<stmt_ptr>& body) {
        const auto outer = bindings;
        for (const auto& s : body)
            for (const auto& entry : s->names)
                bindings.insert(entry.first);
        for (const auto& s : body) {
            const auto enclosing = bindings;
            for (const auto& init : s->init)
                for (const auto& entry : init->names)
                    bindings.insert(entry.first);
            type_feature(s->type_name);
            expression(s->value.get());
            expression(s->value2.get());
            for (const auto& entry : s->names) expression(entry.second.get());
            if (!s->name.empty()) bindings.insert(s->name);
            statements(s->body);
            statements(s->orelse);
            statements(s->iter);
            statements(s->init);
            statements(s->final);
            for (const auto& arm : s->catches) {
                const auto before_catch = bindings;
                if (!arm.name.empty()) bindings.insert(arm.name);
                type_feature(arm.type_name);
                expression(arm.filter.get());
                statements(arm.body);
                bindings = before_catch;
            }
            for (const auto& arm : s->switch_arms) {
                for (const auto& label : arm.labels) expression(label.get());
                statements(arm.body);
            }
            bindings = enclosing;
        }
        bindings = outer;
    }
    void program(const ast_program& prog) {
        static const std::unordered_set<std::string> allowed_usings = {
            "System", "System.Text", "System.Text.Json", "System.Collections",
            "System.Collections.Generic", "System.ComponentModel", "System.Buffers",
            "System.Linq", "System.Threading.Tasks",
            "System.Runtime.CompilerServices", "csmini",
        };
        for (const auto& path : prog.usings) {
            flags.using_roots.push_back(path.substr(0, path.find('.')));
            if (!path_feature(path) && !allowed_usings.count(path))
                note("namespace_" + path.substr(0, path.find('.')));
        }
        for (const auto& cls : prog.classes) {
            bindings.insert(cls.name);
            classes.insert(cls.name);
            if (!cls.ns.empty()) classes.insert(cls.ns + "." + cls.name);
        }
        const auto global_bindings = bindings;
        for (const auto& cls : prog.classes) {
            bindings = global_bindings;
            for (const auto& m : cls.members) bindings.insert(m.name);
            const auto class_bindings = bindings;
            for (const auto& m : cls.members) {
                bindings = class_bindings;
                for (const auto& param : m.params) bindings.insert(param.name);
                type_feature(m.type_name);
                expression(m.init.get());
                expression(m.expr_body.get());
                statements(m.property_get_body);
                statements(m.property_set_body);
                for (const auto& param : m.params) {
                    type_feature(param.type);
                    expression(param.default_value.get());
                }
                statements(m.body);
            }
        }
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
    std::vector<tok_kind> closers;
    for (const auto& t : toks) {
        if (t.kind == tok_kind::lparen) closers.push_back(tok_kind::rparen);
        else if (t.kind == tok_kind::lbracket) closers.push_back(tok_kind::rbracket);
        else if (t.kind == tok_kind::lbrace) closers.push_back(tok_kind::rbrace);
        else if (t.kind == tok_kind::rparen || t.kind == tok_kind::rbracket ||
                 t.kind == tok_kind::rbrace) {
            if (closers.empty() || closers.back() != t.kind)
                p.fail("mismatched closing delimiter", t.pos);
            closers.pop_back();
        }
    }
    if (!closers.empty())
        p.fail("unclosed delimiter", toks.back().pos);
    ast_program prog = p.program();
    subset_classifier classifier{*flags, {}, {}};
    classifier.program(prog);
    return prog;
}

bool csmini_preflight_subset(std::string_view src, std::string* out_reason) {
    if (out_reason)
        out_reason->clear();
    feature_flags flags;
    try {
        (void)parse_source(src, "<preflight>", &flags);
    } catch (const cs_error& e) {
        if (e.kind != "UnsupportedFeature" || e.features.empty())
            throw;
        if (out_reason)
            *out_reason = e.features.front();
        return false;
    }
    if (flags.unsupported.empty())
        return true;
    if (out_reason)
        *out_reason = flags.unsupported.front();
    return false;
}

} // namespace sao::plugins::csmini
