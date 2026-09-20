// csmini_lexer.cpp — C# tokenizer.
//
// Emits a flat token stream for a brace-scoped language: no INDENT/DEDENT,
// whitespace and newlines are trivia.  Keywords are emitted as `keyword`
// tokens carrying their text so the parser can match contextually (`var`,
// `in`, `nameof` stay usable as type names / identifiers where C# allows).
#include "csmini_lexer.h"

#include <cctype>
#include <cstdlib>
#include <unordered_set>

namespace sao::plugins::csmini {
namespace {

const std::unordered_set<std::string_view>& keywords() {
    static const std::unordered_set<std::string_view> kws = {
        "class", "public", "private", "protected", "internal", "static",
        "void", "int", "long", "double", "float", "decimal", "bool",
        "string", "char", "byte", "sbyte", "short", "ushort", "uint",
        "ulong", "object", "var", "dynamic", "if", "else", "for",
        "foreach", "in", "while", "do", "return", "true", "false",
        "null", "new", "using", "namespace", "break", "continue",
        "this", "readonly", "const", "throw", "try", "catch",
        "finally", "is", "as", "typeof", "nameof", "default",
        "switch", "case", "enum", "struct", "interface", "abstract",
        "virtual", "override", "sealed", "partial", "async", "await",
        "unsafe", "fixed", "stackalloc", "ref", "out", "params",
        "delegate", "event", "operator", "implicit", "explicit",
        "get", "set", "init", "value", "where", "select", "from",
        "join", "orderby", "group", "into", "let", "yield", "checked",
        "unchecked", "lock", "goto", "base", "extern", "record",
        "required", "nint", "nuint", "scoped", "file",
    };
    return kws;
}

struct lexer {
    std::string_view src;
    std::string file;
    std::size_t p = 0;
    uint32_t line = 1, col = 1;
    std::vector<token> out;

    [[noreturn]] void fail(const std::string& msg) {
        cs_error e;
        e.kind = "SyntaxError";
        e.message = msg;
        e.file = file;
        e.pos = {line, col};
        throw e;
    }
    char cur() const { return p < src.size() ? src[p] : '\0'; }
    char at(std::size_t k) const {
        return p + k < src.size() ? src[p + k] : '\0';
    }
    void adv() {
        if (p < src.size()) {
            if (src[p] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
            ++p;
        }
    }
    src_pos pos() const { return {line, col}; }
    void push(tok_kind k, src_pos pos, std::string text = {}) {
        token t;
        t.kind = k;
        t.pos = pos;
        t.text = std::move(text);
        out.push_back(std::move(t));
    }

    static bool is_ident_start(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
               (static_cast<unsigned char>(c) >= 0x80);
    }
    static bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
    static bool is_digit(char c) { return c >= '0' && c <= '9'; }

    // ── strings ─────────────────────────────────────────────────────
    void lex_string(bool verbatim) {
        // cur() is the opening quote; `verbatim` set for @"…".
        adv();
        std::string text;
        for (;;) {
            const char c = cur();
            if (c == '\0')
                fail("unterminated string literal");
            if (verbatim) {
                if (c == '"') {
                    if (at(1) == '"') {
                        text.push_back('"');
                        adv();
                        adv();
                        continue;
                    }
                    adv();
                    break;
                }
                text.push_back(c);
                adv();
                continue;
            }
            if (c == '"') {
                adv();
                break;
            }
            if (c == '\n')
                fail("unterminated string literal");
            if (c == '\\') {
                adv();
                const char e = cur();
                if (e == '\0')
                    fail("unterminated escape");
                adv();
                switch (e) {
                case 'n': text.push_back('\n'); break;
                case 't': text.push_back('\t'); break;
                case 'r': text.push_back('\r'); break;
                case '0': text.push_back('\0'); break;
                case 'a': text.push_back('\a'); break;
                case 'b': text.push_back('\b'); break;
                case 'f': text.push_back('\f'); break;
                case 'v': text.push_back('\v'); break;
                case '\\': text.push_back('\\'); break;
                case '\'': text.push_back('\''); break;
                case '"': text.push_back('"'); break;
                case 'u': {
                    int64_t cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = cur();
                        if (!std::isxdigit(static_cast<unsigned char>(h)))
                            fail("bad \\u escape");
                        cp = cp * 16 + hex(h);
                        adv();
                    }
                    append_cp(text, cp);
                    break;
                }
                case 'x': {
                    int64_t cp = 0;
                    int n = 0;
                    while (n < 4 && std::isxdigit(static_cast<unsigned char>(cur()))) {
                        cp = cp * 16 + hex(cur());
                        adv();
                        ++n;
                    }
                    if (n == 0)
                        fail("bad \\x escape");
                    append_cp(text, cp);
                    break;
                }
                default:
                    fail(std::string("bad escape \\") + e);
                }
                continue;
            }
            text.push_back(c);
            adv();
        }
        push(verbatim ? tok_kind::verbatim_string : tok_kind::string, pos(), text);
        out.back().verbatim = verbatim;
    }

    // interpolated string $"…" — raw inner content preserved for the
    // parser's split_interp re-entry (handles {{ }} {expr} \-escapes).
    void lex_interp(bool verbatim) {
        adv();  // opening quote
        const src_pos start = pos();
        std::string raw;
        int braces = 0;
        for (;;) {
            const char c = cur();
            if (c == '\0')
                fail("unterminated interpolated string");
            if (verbatim) {
                if (c == '"') {
                    if (at(1) == '"') {
                        raw.push_back('"');
                        adv();
                        adv();
                        continue;
                    }
                    adv();
                    break;
                }
            } else {
                if (c == '"') {
                    adv();
                    break;
                }
                if (c == '\n')
                    fail("unterminated interpolated string");
                if (c == '\\') {
                    raw.push_back(c);
                    adv();
                    if (cur() == '\0')
                        fail("unterminated escape");
                    raw.push_back(cur());
                    adv();
                    continue;
                }
            }
            if (c == '{') {
                if (at(1) == '{') {
                    raw.push_back('{');
                    raw.push_back('{');
                    adv();
                    adv();
                    continue;
                }
                ++braces;
            } else if (c == '}' && braces > 0) {
                --braces;
            }
            raw.push_back(c);
            adv();
        }
        push(tok_kind::interp_string, start, raw);
        out.back().verbatim = verbatim;
    }

    static int hex(char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return c - 'A' + 10;
    }
    static void append_cp(std::string& out, int64_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    void lex_char() {
        adv();  // opening '
        std::string body;
        for (;;) {
            const char c = cur();
            if (c == '\0' || c == '\n')
                fail("unterminated char literal");
            if (c == '\'') {
                adv();
                break;
            }
            if (c == '\\') {
                body.push_back(c);
                adv();
                if (cur() == '\0')
                    fail("bad char escape");
                body.push_back(cur());
                adv();
                continue;
            }
            body.push_back(c);
            adv();
        }
        // decode to a code point
        int64_t cp;
        if (body.size() == 1) {
            cp = static_cast<unsigned char>(body[0]);
        } else if (body.size() >= 2 && body[0] == '\\') {
            switch (body[1]) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case '0': cp = '\0'; break;
            case '\\': cp = '\\'; break;
            case '\'': cp = '\''; break;
            case '"': cp = '"'; break;
            case 'u': {
                cp = 0;
                if (body.size() != 6)
                    fail("bad char \\u escape");
                for (std::size_t k = 2; k < body.size(); ++k) {
                    if (!std::isxdigit(
                            static_cast<unsigned char>(body[k])))
                        fail("bad char \\u escape");
                    cp = cp * 16 + hex(body[k]);
                }
                break;
            }
            case 'x': {
                cp = 0;
                if (body.size() < 3)
                    fail("bad char \\x escape");
                for (std::size_t k = 2; k < body.size(); ++k) {
                    if (!std::isxdigit(
                            static_cast<unsigned char>(body[k])))
                        fail("bad char \\x escape");
                    cp = cp * 16 + hex(body[k]);
                }
                break;
            }
            default:
                fail("bad char escape");
            }
        } else {
            fail("multi-char literal");
        }
        push(tok_kind::char_, pos(), {});
        out.back().int_value = cp;
    }

    void lex_number() {
        const src_pos start = pos();
        std::string digits;
        bool is_float = false;
        if (cur() == '0' && (at(1) == 'x' || at(1) == 'X')) {
            adv();
            adv();
            while (std::isxdigit(static_cast<unsigned char>(cur())) || cur() == '_') {
                if (cur() != '_')
                    digits.push_back(cur());
                adv();
            }
            if (digits.empty())
                fail("bad hex literal");
            token t;
            t.kind = tok_kind::number;
            t.pos = start;
            t.int_value = std::strtoll(digits.c_str(), nullptr, 16);
            out.push_back(std::move(t));
            // optional u/l suffixes on hex ints
            while (cur() == 'u' || cur() == 'U' || cur() == 'l' || cur() == 'L')
                adv();
            return;
        }
        while (is_digit(cur()) || cur() == '_') {
            if (cur() != '_')
                digits.push_back(cur());
            adv();
        }
        if (cur() == '.' && is_digit(at(1))) {
            is_float = true;
            digits.push_back('.');
            adv();
            while (is_digit(cur()) || cur() == '_') {
                if (cur() != '_')
                    digits.push_back(cur());
                adv();
            }
        }
        if (cur() == 'e' || cur() == 'E') {
            is_float = true;
            digits.push_back('e');
            adv();
            if (cur() == '+' || cur() == '-') {
                digits.push_back(cur());
                adv();
            }
            bool any = false;
            while (is_digit(cur()) || cur() == '_') {
                if (cur() != '_') {
                    digits.push_back(cur());
                    any = true;
                }
                adv();
            }
            if (!any)
                fail("bad exponent");
        }
        // suffixes f/F d/D m/M → double; l/L u/U → integer marker (folded)
        if (cur() == 'f' || cur() == 'F' || cur() == 'd' || cur() == 'D' ||
            cur() == 'm' || cur() == 'M') {
            is_float = true;
            adv();
        } else {
            while (cur() == 'u' || cur() == 'U' || cur() == 'l' || cur() == 'L')
                adv();
        }
        token t;
        t.kind = tok_kind::number;
        t.pos = start;
        t.is_float = is_float;
        if (is_float) {
            t.num_value = std::strtod(digits.c_str(), nullptr);
        } else {
            try {
                t.int_value = std::stoll(digits, nullptr, 10);
            } catch (...) {
                t.int_value = 0;
                t.is_float = true;
                t.num_value = std::strtod(digits.c_str(), nullptr);   // overflow → double
            }
        }
        out.push_back(std::move(t));
    }

    void skip_line_comment() {
        while (cur() != '\0' && cur() != '\n')
            adv();
    }
    void skip_block_comment() {
        for (;;) {
            if (cur() == '\0')
                fail("unterminated /* */ comment");
            if (cur() == '*' && at(1) == '/') {
                adv();
                adv();
                return;
            }
            adv();
        }
    }
    void skip_directive() {
        // #region/#if/#define lines — treated as trivia up to newline.
        skip_line_comment();
    }

    void run() {
        while (p < src.size()) {
            const char c = cur();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                adv();
                continue;
            }
            if (c == '/' && at(1) == '/') {
                skip_line_comment();
                continue;
            }
            if (c == '/' && at(1) == '*') {
                adv();
                adv();
                skip_block_comment();
                continue;
            }
            const src_pos here = pos();
            if (c == '#') {
                skip_directive();
                continue;
            }
            if (c == '@' && at(1) == '"') {
                adv();
                lex_string(true);
                continue;
            }
            if (c == '$' && at(1) == '"') {
                adv();
                lex_interp(false);
                continue;
            }
            if (c == '$' && at(1) == '@' && at(2) == '"') {
                adv();
                adv();
                lex_interp(true);
                continue;
            }
            if (c == '@' && at(1) == '$' && at(2) == '"') {
                adv();
                adv();
                lex_interp(true);
                continue;
            }
            if (c == '"') {
                lex_string(false);
                continue;
            }
            if (c == '\'') {
                lex_char();
                continue;
            }
            if (is_digit(c) || (c == '.' && is_digit(at(1)))) {
                if (c == '.' && at(1) == '.') {
                    push(tok_kind::dot, here);
                    adv();
                    push(tok_kind::dot, pos());
                    adv();
                    continue;
                }
                lex_number();
                continue;
            }
            if (is_ident_start(c)) {
                std::string name;
                while (is_ident(cur())) {
                    name.push_back(cur());
                    adv();
                }
                if (keywords().count(name)) {
                    push(tok_kind::keyword, here, name);
                } else {
                    push(tok_kind::name, here, name);
                }
                continue;
            }
            // ── operators — longest match first ──
            const auto two = [&](char a, char b) { return c == a && at(1) == b; };
            const auto three = [&](char a, char b, char d) {
                return c == a && at(1) == b && at(2) == d;
            };
            if (three('?', '?', '=')) {
                push(tok_kind::nullcoalesce_eq, here);
                adv(); adv(); adv();
                continue;
            }
            if (two('?', '?')) {
                push(tok_kind::nullcoalesce, here);
                adv(); adv();
                continue;
            }
            if (two('?', '.')) {
                push(tok_kind::nullcond, here);
                adv(); adv();
                continue;
            }
            if (two('=', '>')) {
                push(tok_kind::arrow, here);
                adv(); adv();
                continue;
            }
            if (two('+', '+')) {
                push(tok_kind::plus2, here);
                adv(); adv();
                continue;
            }
            if (two('-', '-')) {
                push(tok_kind::minus2, here);
                adv(); adv();
                continue;
            }
            if (two('&', '&')) {
                push(tok_kind::and2, here);
                adv(); adv();
                continue;
            }
            if (two('|', '|')) {
                push(tok_kind::or2, here);
                adv(); adv();
                continue;
            }
            if (two('=', '=')) {
                push(tok_kind::eq, here);
                adv(); adv();
                continue;
            }
            if (two('!', '=')) {
                push(tok_kind::ne, here);
                adv(); adv();
                continue;
            }
            if (two('<', '=')) {
                push(tok_kind::le, here);
                adv(); adv();
                continue;
            }
            if (two('>', '=')) {
                push(tok_kind::ge, here);
                adv(); adv();
                continue;
            }
            if (three('<', '<', '=')) {
                push(tok_kind::lshift_eq, here);
                adv(); adv(); adv();
                continue;
            }
            if (three('>', '>', '=')) {
                push(tok_kind::rshift_eq, here);
                adv(); adv(); adv();
                continue;
            }
            if (two('<', '<')) {
                push(tok_kind::lshift, here);
                adv(); adv();
                continue;
            }
            if (two('>', '>')) {
                push(tok_kind::rshift, here);
                adv(); adv();
                continue;
            }
            if (two('+', '=')) {
                push(tok_kind::plus_eq, here);
                adv(); adv();
                continue;
            }
            if (two('-', '=')) {
                push(tok_kind::minus_eq, here);
                adv(); adv();
                continue;
            }
            if (two('*', '=')) {
                push(tok_kind::star_eq, here);
                adv(); adv();
                continue;
            }
            if (two('/', '=')) {
                push(tok_kind::slash_eq, here);
                adv(); adv();
                continue;
            }
            if (two('%', '=')) {
                push(tok_kind::percent_eq, here);
                adv(); adv();
                continue;
            }
            if (two('&', '=')) {
                push(tok_kind::amp_eq, here);
                adv(); adv();
                continue;
            }
            if (two('|', '=')) {
                push(tok_kind::pipe_eq, here);
                adv(); adv();
                continue;
            }
            if (two('^', '=')) {
                push(tok_kind::caret_eq, here);
                adv(); adv();
                continue;
            }
            switch (c) {
            case '(': push(tok_kind::lparen, here); adv(); continue;
            case ')': push(tok_kind::rparen, here); adv(); continue;
            case '[': push(tok_kind::lbracket, here); adv(); continue;
            case ']': push(tok_kind::rbracket, here); adv(); continue;
            case '{': push(tok_kind::lbrace, here); adv(); continue;
            case '}': push(tok_kind::rbrace, here); adv(); continue;
            case ',': push(tok_kind::comma, here); adv(); continue;
            case ':': push(tok_kind::colon, here); adv(); continue;
            case ';': push(tok_kind::semicolon, here); adv(); continue;
            case '.': push(tok_kind::dot, here); adv(); continue;
            case '?': push(tok_kind::qmark, here); adv(); continue;
            case '!': push(tok_kind::bang, here); adv(); continue;
            case '+': push(tok_kind::plus, here); adv(); continue;
            case '-': push(tok_kind::minus, here); adv(); continue;
            case '*': push(tok_kind::star, here); adv(); continue;
            case '/': push(tok_kind::slash, here); adv(); continue;
            case '%': push(tok_kind::percent, here); adv(); continue;
            case '~': push(tok_kind::tilde, here); adv(); continue;
            case '^': push(tok_kind::caret, here); adv(); continue;
            case '&': push(tok_kind::amp, here); adv(); continue;
            case '|': push(tok_kind::pipe, here); adv(); continue;
            case '<': push(tok_kind::lt, here); adv(); continue;
            case '>': push(tok_kind::gt, here); adv(); continue;
            case '=': push(tok_kind::assign, here); adv(); continue;
            default:
                fail(std::string("unexpected character '") + c + "'");
            }
        }
        token end{};
        end.kind = tok_kind::eof_;
        end.pos = {line, col};
        out.push_back(std::move(end));
    }
};

} // namespace

std::vector<token> lex_source(std::string_view source, const std::string& file) {
    lexer lx{source, file};
    lx.run();
    return std::move(lx.out);
}

// Split interpolated-string raw content into literal chunks + `{expr}`
// sources; handles {{ }} escapes everywhere plus \-escapes in non-verbatim.
std::vector<interp_part> split_interp(std::string_view raw, bool verbatim) {
    std::vector<interp_part> out;
    std::string lit;
    auto flush = [&]() {
        if (!lit.empty()) {
            out.push_back({false, lit});
            lit.clear();
        }
    };
    std::size_t p = 0;
    while (p < raw.size()) {
        const char c = raw[p];
        if (!verbatim && c == '\\' && p + 1 < raw.size()) {
            const char e = raw[p + 1];
            p += 2;
            switch (e) {
            case 'n': lit.push_back('\n'); break;
            case 't': lit.push_back('\t'); break;
            case 'r': lit.push_back('\r'); break;
            case '0': lit.push_back('\0'); break;
            case '{': lit.push_back('{'); break;
            case '}': lit.push_back('}'); break;
            case '"': lit.push_back('"'); break;
            case '\'': lit.push_back('\''); break;
            case '\\': lit.push_back('\\'); break;
            default:
                lit.push_back('\\');
                lit.push_back(e);
                break;
            }
            continue;
        }
        if (c == '{') {
            if (p + 1 < raw.size() && raw[p + 1] == '{') {
                lit.push_back('{');
                p += 2;
                continue;
            }
            // expression island — find matching brace; nested braces
            // count but string/char literals inside the island are skipped
            // so `"}"` / `'}'` don't corrupt the depth.
            flush();
            int depth = 1;
            bool in_str = false, in_chr = false, esc = false;
            std::size_t start = ++p;
            while (p < raw.size() && depth > 0) {
                const char ic = raw[p];
                if (esc) {
                    esc = false;
                    ++p;
                    continue;
                }
                if (in_str) {
                    if (ic == '\\')
                        esc = true;
                    else if (ic == '"')
                        in_str = false;
                    ++p;
                    continue;
                }
                if (in_chr) {
                    if (ic == '\\')
                        esc = true;
                    else if (ic == '\'')
                        in_chr = false;
                    ++p;
                    continue;
                }
                if (ic == '"') {
                    in_str = true;
                    ++p;
                    continue;
                }
                if (ic == '\'') {
                    in_chr = true;
                    ++p;
                    continue;
                }
                if (ic == '{')
                    ++depth;
                else if (ic == '}')
                    --depth;
                if (depth > 0)
                    ++p;
            }
            std::string expr_src(raw.substr(start, p - start));
            // strip C# format specifier / alignment ",1:format" tail — the
            // subset renders through cs_to_str only.  A top-level `,` or `:`
            // (not nested in (){}[] / literals and not the `?:` else-arm)
            // ends the expression source.
            {
                int cut_depth = 0, ternaries = 0;
                bool cut_str = false, cut_chr = false, cut_esc = false;
                std::size_t cut = expr_src.size();
                for (std::size_t k = 0; k < expr_src.size(); ++k) {
                    const char ch = expr_src[k];
                    if (cut_esc) {
                        cut_esc = false;
                        continue;
                    }
                    if (ch == '\\' && (cut_str || cut_chr)) {
                        cut_esc = true;
                        continue;
                    }
                    if (cut_str) {
                        if (ch == '"')
                            cut_str = false;
                        continue;
                    }
                    if (cut_chr) {
                        if (ch == '\'')
                            cut_chr = false;
                        continue;
                    }
                    if (ch == '"') { cut_str = true; continue; }
                    if (ch == '\'') { cut_chr = true; continue; }
                    if (ch == '(' || ch == '[' || ch == '{') ++cut_depth;
                    if (ch == ')' || ch == ']' || ch == '}') --cut_depth;
                    if (cut_depth != 0)
                        continue;
                    if (ch == '?') {
                        // `?.`/`?[`/`??` are null-conditional/coalesce, not
                        // ternary — they must not consume the format `:`.
                        if (k + 1 < expr_src.size() &&
                            (expr_src[k + 1] == '.' ||
                             expr_src[k + 1] == '[' ||
                             expr_src[k + 1] == '?')) {
                            if (expr_src[k + 1] == '?')
                                ++k;
                            continue;
                        }
                        ++ternaries;
                    } else if (ch == ':') {
                        if (ternaries > 0) {
                            --ternaries;
                        } else {
                            cut = k;
                            break;
                        }
                    } else if (ch == ',') {
                        cut = k;
                        break;
                    }
                }
                expr_src = expr_src.substr(0, cut);
            }
            out.push_back({true, expr_src});
            ++p;
            continue;
        }
        if (c == '}') {
            if (p + 1 < raw.size() && raw[p + 1] == '}') {
                lit.push_back('}');
                p += 2;
                continue;
            }
            lit.push_back('}');
            ++p;
            continue;
        }
        if (verbatim && c == '"' && p + 1 < raw.size() && raw[p + 1] == '"') {
            lit.push_back('"');
            p += 2;
            continue;
        }
        lit.push_back(c);
        ++p;
    }
    flush();
    return out;
}

} // namespace sao::plugins::csmini
