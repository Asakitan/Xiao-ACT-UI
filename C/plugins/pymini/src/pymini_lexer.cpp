// pymini_lexer.cpp — tokenizer: indentation, string prefixes, f-strings,
// comments, implicit/explicit line joining, numbers.
#include "pymini_lexer.h"

#include <cctype>
#include <cstdlib>

namespace sao::plugins::pymini {
namespace {

[[noreturn]] void lex_fail(const std::string& msg, src_pos pos,
                          const std::string& file,
                          const std::string& kind = "SyntaxError") {
    py_error e{};
    e.kind = kind;
    e.message = msg;
    e.pos = pos;
    e.file = file;
    throw e;
}

struct lexer {
    std::string_view src;
    std::string file;
    std::size_t pos = 0;
    uint32_t line = 1;
    uint32_t col = 0;
    int depth = 0;                     // bracket depth (implicit join)
    std::vector<int> indents{0};       // indent stack
    std::vector<token> out;
    std::vector<token> pending;        // queued dedents/newlines

    char peek(std::size_t off = 0) const {
        return pos + off < src.size() ? src[pos + off] : '\0';
    }
    char get() {
        const char c = pos < src.size() ? src[pos++] : '\0';
        if (c == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
        return c;
    }
    bool expect(const char* s) {
        const std::size_t n = std::strlen(s);
        if (src.substr(pos, n) == s) {
            for (std::size_t i = 0; i < n; ++i)
                get();
            return true;
        }
        return false;
    }
    src_pos here() const { return {line, col}; }

    void emit(tok_kind k, std::string text = {}) {
        out.push_back({k, std::move(text), 0, 0.0, here()});
    }

    // Measure leading indent of a physical line: returns column count
    // (tab=8 rounding); -1 if line is blank/comment-only.
    int measure_indent() {
        int coln = 0;
        std::size_t save = pos;
        uint32_t svline = line, svcol = col;
        bool only_ws = true;
        while (true) {
            const char c = peek();
            if (c == ' ' ) { ++coln; get(); }
            else if (c == '\t') { coln = (coln / 8 + 1) * 8; get(); }
            else if (c == '#') { only_ws = true; break; }
            else if (c == '\r' || c == '\n') { only_ws = true; break; }
            else if (c == '\0') { only_ws = true; break; }
            else { only_ws = false; break; }
        }
        if (only_ws) {
            // don't emit INDENT/DEDENT for blank/comment lines; caller
            // decides whether to just consume through EOL
            return -1;
        }
        pos = save;
        line = svline;
        col = svcol;
        return coln;
    }

    // Consume the whitespace + comments of the next logical line start and
    // return its indent (or -1 → blank line, keep looping).
    int line_start_indent() {
        while (true) {
            // measure WITHOUT consuming: measure_indent restores pos for
            // non-blank lines and returns -1 for blank/comment-only ones
            const int ind = measure_indent();
            if (peek() == '\0')
                return -2;   // EOF
            if (ind == -1) {
                // skip this blank/comment line up to and including its
                // newline — but NOT the next line's leading indent.
                // '\r' ends the line just like the main loop's `c=='\r'`
                // arm (bare-CR endings and CRLF alike).
                while (peek() != '\n' && peek() != '\r' && peek() != '\0')
                    get();
                if (peek() == '\r')
                    get();
                if (peek() == '\n')
                    get();
                continue;
            }
            // consume exactly the measured indent so the main loop starts
            // at the first non-space token of the line
            for (int coln = 0; coln < ind;) {
                if (peek() == ' ') { ++coln; get(); }
                else { coln = (coln / 8 + 1) * 8; get(); }
            }
            return ind;
        }
    }

    // ── strings ───────────────────────────────────────────────────────────
    std::string decode_escapes(std::string_view raw, src_pos at) {
        return decode_escapes_impl(raw, at, file);
    }

    static std::string decode_escapes_impl(std::string_view raw, src_pos at,
                                           const std::string& file) {
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const char c = raw[i];
            if (c != '\\' || i + 1 >= raw.size()) {
                out += c;
                continue;
            }
            const char e = raw[++i];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'v': out += '\v'; break;
            case 'a': out += '\a'; break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                // up to 3 octal digits (including this one)
                int val = e - '0';
                int used = 1;
                while (used < 3 && i + 1 < raw.size() &&
                       raw[i + 1] >= '0' && raw[i + 1] <= '7') {
                    val = val * 8 + (raw[++i] - '0');
                    ++used;
                }
                out += static_cast<char>(val & 0xff);
                break;
            }
            case 'x': {
                int val = 0;
                int used = 0;
                while (used < 2 && i + 1 < raw.size() &&
                       std::isxdigit(static_cast<unsigned char>(raw[i + 1]))) {
                    const char h = raw[++i];
                    val = val * 16 +
                          (h >= '0' && h <= '9' ? h - '0'
                           : h >= 'a' && h <= 'f' ? h - 'a' + 10
                           : h - 'A' + 10);
                    ++used;
                }
                if (used == 0)
                    lex_fail("invalid \\x escape", at, file);
                out += static_cast<char>(val & 0xff);
                break;
            }
            case 'u': {
                unsigned long val = 0;
                int used = 0;
                while (used < 4 && i + 1 < raw.size() &&
                       std::isxdigit(static_cast<unsigned char>(raw[i + 1]))) {
                    const char h = raw[++i];
                    val = val * 16 +
                          (h >= '0' && h <= '9' ? h - '0'
                           : h >= 'a' && h <= 'f' ? h - 'a' + 10
                           : h - 'A' + 10);
                    ++used;
                }
                if (used < 4)
                    lex_fail("invalid \\u escape", at, file);
                // emit utf8
                if (val < 0x80) {
                    out += static_cast<char>(val);
                } else if (val < 0x800) {
                    out += static_cast<char>(0xc0 | (val >> 6));
                    out += static_cast<char>(0x80 | (val & 0x3f));
                } else {
                    out += static_cast<char>(0xe0 | (val >> 12));
                    out += static_cast<char>(0x80 | ((val >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (val & 0x3f));
                }
                break;
            }
            case '\\': out += '\\'; break;
            case '\'': out += '\''; break;
            case '"': out += '"'; break;
            case '\n': break;            // line continuation inside string
            default:
                out += '\\';
                out += e;
                break;
            }
        }
        return out;
    }

    // Lex one string literal: prefixes already consumed is false — caller
    // detected the opening quote at `pos`. Returns (is_fstring, content_raw).
    // `triple`/`quote`/`raw`/`fstr` computed by caller.
    // detected the opening quote at `pos`. Returns (is_fstring, content_raw).
    // `triple`/`quote`/`raw`/`fstr` computed by caller.
    std::string scan_string_body(char quote, bool triple, src_pos at) {
        std::string raw;
        if (triple) {
            while (true) {
                const char c = peek();
                if (c == '\0')
                    lex_fail("unterminated triple-quoted string", at, file);
                if (c == '\\') {
                    // keep the escape sequence intact so a \" inside a
                    // triple-quoted body can't terminate the literal
                    raw += get();
                    if (peek() != '\0')
                        raw += get();
                    continue;
                }
                if (c == quote && peek(1) == quote && peek(2) == quote) {
                    get(); get(); get();
                    break;
                }
                raw += get();
            }
        } else {
            while (true) {
                const char c = peek();
                if (c == '\0' || c == '\n')
                    lex_fail("unterminated string", at, file);
                get();
                if (c == quote)
                    break;
                if (c == '\\') {
                    raw += c;
                    if (peek() == '\0')
                        break;
                    raw += get();
                    continue;
                }
                raw += c;
            }
        }
        return raw;
    }

    // ── numbers ───────────────────────────────────────────────────────────
    void scan_number() {
        const src_pos at = here();
        std::string text;
        bool is_float = false;
        bool imaginary = false;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            text += get(); text += get();
            while (std::isxdigit(static_cast<unsigned char>(peek())) ||
                   peek() == '_')
                text += get();
            const std::size_t digits = text.size() - 2;
            const char nc = peek();
            if (digits == 0 || std::isalnum(static_cast<unsigned char>(nc)) ||
                nc == '_' || nc == '.')
                lex_fail("invalid hexadecimal literal", at, file);
            token t{tok_kind::number, std::move(text), 0, 0.0, at};
            // strip underscores + 0x
            std::string hex;
            for (std::size_t i = 2; i < t.text.size(); ++i)
                if (t.text[i] != '_')
                    hex += t.text[i];
            t.int_value = std::strtoll(hex.c_str(), nullptr, 16);
            out.push_back(std::move(t));
            return;
        }
        if (peek() == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
            text += get(); text += get();
            while ((peek() >= '0' && peek() <= '7') || peek() == '_')
                text += get();
            const std::size_t digits = text.size() - 2;
            const char nc = peek();
            if (digits == 0 || std::isalnum(static_cast<unsigned char>(nc)) ||
                nc == '_' || nc == '.')
                lex_fail("invalid octal literal", at, file);
            token t{tok_kind::number, std::move(text), 0, 0.0, at};
            std::string oct;
            for (std::size_t i = 2; i < t.text.size(); ++i)
                if (t.text[i] != '_')
                    oct += t.text[i];
            t.int_value = std::strtoll(oct.c_str(), nullptr, 8);
            out.push_back(std::move(t));
            return;
        }
        if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
            text += get(); text += get();
            while (peek() == '0' || peek() == '1' || peek() == '_')
                text += get();
            const std::size_t digits = text.size() - 2;
            const char nc = peek();
            if (digits == 0 || std::isalnum(static_cast<unsigned char>(nc)) ||
                nc == '_' || nc == '.')
                lex_fail("invalid binary literal", at, file);
            token t{tok_kind::number, std::move(text), 0, 0.0, at};
            std::string bin;
            for (std::size_t i = 2; i < t.text.size(); ++i)
                if (t.text[i] != '_')
                    bin += t.text[i];
            t.int_value = std::strtoll(bin.c_str(), nullptr, 2);
            out.push_back(std::move(t));
            return;
        }
        while (std::isdigit(static_cast<unsigned char>(peek())) ||
               peek() == '_')
            text += get();
        // a trailing dot also makes it a float (`x = 1.` is legal)
        if (peek() == '.') {
            is_float = true;
            text += get();
            while (std::isdigit(static_cast<unsigned char>(peek())) ||
                   peek() == '_')
                text += get();
        }
        if (peek() == 'e' || peek() == 'E') {
            const char sign = peek(1);
            if (std::isdigit(static_cast<unsigned char>(peek(1))) ||
                ((sign == '+' || sign == '-') &&
                 std::isdigit(static_cast<unsigned char>(peek(2))))) {
                is_float = true;
                text += get();
                if (peek() == '+' || peek() == '-')
                    text += get();
                while (std::isdigit(static_cast<unsigned char>(peek())) ||
                       peek() == '_')
                    text += get();
            }
        }
        if (peek() == 'j' || peek() == 'J') {
            imaginary = true;
            text += get();
        }
        if (imaginary)
            lex_fail("complex literals not supported", at, file);
        token t{tok_kind::number, std::move(text), 0, 0.0, at};
        std::string digits;
        for (const char c : t.text)
            if (c != '_')
                digits += c;
        if (is_float)
            t.num_value = std::strtod(digits.c_str(), nullptr);
        else {
            errno = 0;
            const long long v = std::strtoll(digits.c_str(), nullptr, 10);
            if (errno == ERANGE)
                t.num_value = std::strtod(digits.c_str(), nullptr);
            else
                t.int_value = v;
        }
        out.push_back(std::move(t));
    }

    // ── main loop ─────────────────────────────────────────────────────────
    void run() {
        bool at_line_start = true;
        while (true) {
            // emit pending queue first
            if (!pending.empty()) {
                out.insert(out.end(), pending.begin(), pending.end());
                pending.clear();
            }
            if (at_line_start && depth == 0) {
                const int ind = line_start_indent();
                if (ind == -2) {
                    // EOF — flush dedents
                    while (indents.size() > 1) {
                        indents.pop_back();
                        emit(tok_kind::dedent);
                    }
                    emit(tok_kind::newline);
                    emit(tok_kind::eof_);
                    return;
                }
                if (ind > indents.back()) {
                    indents.push_back(ind);
                    emit(tok_kind::indent);
                } else if (ind < indents.back()) {
                    while (indents.size() > 1 && ind < indents.back()) {
                        indents.pop_back();
                        emit(tok_kind::dedent);
                    }
                    // the new level must equal an outer level exactly
                    if (ind != indents.back())
                        lex_fail("unindent does not match any outer "
                                 "indentation level",
                                 here(), file, "IndentationError");
                }
                at_line_start = false;
            }

            const char c = peek();
            if (c == '\0') {
                while (indents.size() > 1) {
                    indents.pop_back();
                    emit(tok_kind::dedent);
                }
                emit(tok_kind::newline);
                emit(tok_kind::eof_);
                return;
            }
            if (c == ' ' || c == '\t') {
                get();
                continue;
            }
            if (c == '#') {
                while (peek() != '\n' && peek() != '\0')
                    get();
                continue;
            }
            if (c == '\\') {
                get();
                // explicit line join
                while (peek() == ' ' || peek() == '\t')
                    get();
                if (peek() == '\r')
                    get();
                if (peek() == '\n') {
                    get();
                    continue;
                }
                if (peek() == '\0')
                    continue;
                lex_fail("unexpected character after line continuation", here(), file);
            }
            if (c == '\r') {
                get();
                if (peek() == '\n')
                    get();
                if (depth == 0) {
                    emit(tok_kind::newline);
                    at_line_start = true;
                }
                continue;
            }
            if (c == '\n') {
                get();
                if (depth == 0) {
                    emit(tok_kind::newline);
                    at_line_start = true;
                }
                continue;
            }

            // string prefixes: [rRbBuUfF]{0,2}['"]
            if (std::isalpha(static_cast<unsigned char>(c)) ||
                c == '_') {
                // check for string prefix
                std::size_t save = pos;
                uint32_t svline = line, svcol = col;
                std::string prefix;
                bool is_str = false;
                while (std::isalpha(static_cast<unsigned char>(peek())) &&
                       prefix.size() < 3) {
                    const char p = static_cast<char>(
                        std::tolower(static_cast<unsigned char>(peek())));
                    if (p == 'r' || p == 'b' || p == 'u' || p == 'f')
                        prefix += p, get();
                    else
                        break;
                }
                if (peek() == '\'' || peek() == '"')
                    is_str = true;
                if (is_str && !prefix.empty()) {
                    const src_pos at = here();
                    const char quote = get();
                    const bool triple = peek() == quote && peek(1) == quote;
                    if (triple) {
                        get(); get();
                    }
                    const std::string raw = scan_string_body(quote, triple, at);
                    const bool raw_str = prefix.find('r') != std::string::npos;
                    const bool is_f = prefix.find('f') != std::string::npos;
                    const bool is_b = prefix.find('b') != std::string::npos;
                    token t;
                    t.pos = at;
                    if (is_f) {
                        t.kind = tok_kind::fstring;
                        t.text = raw;      // parser decodes per literal chunk
                        t.raw_str = raw_str;
                    } else {
                        t.kind = is_b ? tok_kind::bytes_ : tok_kind::string;
                        t.text = raw_str ? raw : decode_escapes(raw, at);
                    }
                    out.push_back(std::move(t));
                    continue;
                }
                if (is_str && prefix.empty()) {
                    // plain string handled below (c is quote) — unreachable here
                }
                // not a string prefix → name/keyword
                pos = save;
                line = svline;
                col = svcol;
                const src_pos at = here();
                std::string name;
                while (std::isalnum(static_cast<unsigned char>(peek())) ||
                       peek() == '_')
                    name += get();
                out.push_back({tok_kind::name, std::move(name), 0, 0.0, at});
                continue;
            }

            if (c == '\'' || c == '"') {
                const src_pos at = here();
                const char quote = get();
                const bool triple = peek() == quote && peek(1) == quote;
                if (triple) {
                    get(); get();
                }
                const std::string raw = scan_string_body(quote, triple, at);
                out.push_back({tok_kind::string, decode_escapes(raw, at), 0, 0.0, at});
                continue;
            }

            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
                scan_number();
                continue;
            }

            const src_pos at = here();
            auto two = [&](tok_kind k, const char* s) {
                if (expect(s)) {
                    out.push_back({k, {}, 0, 0.0, at});
                    return true;
                }
                return false;
            };

            switch (c) {
            case '(': get(); ++depth; out.push_back({tok_kind::lparen, {}, 0, 0.0, at}); break;
            case ')': get(); --depth; out.push_back({tok_kind::rparen, {}, 0, 0.0, at}); break;
            case '[': get(); ++depth; out.push_back({tok_kind::lbracket, {}, 0, 0.0, at}); break;
            case ']': get(); --depth; out.push_back({tok_kind::rbracket, {}, 0, 0.0, at}); break;
            case '{': get(); ++depth; out.push_back({tok_kind::lbrace, {}, 0, 0.0, at}); break;
            case '}': get(); --depth; out.push_back({tok_kind::rbrace, {}, 0, 0.0, at}); break;
            case ',': get(); out.push_back({tok_kind::comma, {}, 0, 0.0, at}); break;
            case ';': get(); out.push_back({tok_kind::semicolon, {}, 0, 0.0, at}); break;
            case '.':
                if (expect("...")) {
                    out.push_back({tok_kind::ellipsis, {}, 0, 0.0, at});
                } else {
                    get();
                    out.push_back({tok_kind::dot, {}, 0, 0.0, at});
                }
                break;
            case ':':
                get();
                if (peek() == '=') {
                    get();
                    out.push_back({tok_kind::walrus, {}, 0, 0.0, at});
                } else {
                    out.push_back({tok_kind::colon, {}, 0, 0.0, at});
                }
                break;
            case '@':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::at_eq, {}, 0, 0.0, at}); }
                else out.push_back({tok_kind::at, {}, 0, 0.0, at});
                break;
            case '~': get(); out.push_back({tok_kind::tilde, {}, 0, 0.0, at}); break;
            case '=':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::eq, {}, 0, 0.0, at}); }
                else out.push_back({tok_kind::assign, {}, 0, 0.0, at});
                break;
            case '!':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::ne, {}, 0, 0.0, at}); }
                else lex_fail("unexpected '!'", at, file);
                break;
            case '<':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::le, {}, 0, 0.0, at}); }
                else if (peek() == '<') {
                    get();
                    if (peek() == '=') { get(); out.push_back({tok_kind::lshift_eq, {}, 0, 0.0, at}); }
                    else out.push_back({tok_kind::lshift, {}, 0, 0.0, at});
                } else out.push_back({tok_kind::lt, {}, 0, 0.0, at});
                break;
            case '>':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::ge, {}, 0, 0.0, at}); }
                else if (peek() == '>') {
                    get();
                    if (peek() == '=') { get(); out.push_back({tok_kind::rshift_eq, {}, 0, 0.0, at}); }
                    else out.push_back({tok_kind::rshift, {}, 0, 0.0, at});
                } else out.push_back({tok_kind::gt, {}, 0, 0.0, at});
                break;
            case '+':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::plus_eq, {}, 0, 0.0, at}); }
                else out.push_back({tok_kind::plus, {}, 0, 0.0, at});
                break;
            case '-':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::minus_eq, {}, 0, 0.0, at}); }
                else if (peek() == '>') { get(); out.push_back({tok_kind::arrow, {}, 0, 0.0, at}); }
                else out.push_back({tok_kind::minus, {}, 0, 0.0, at});
                break;
            case '*':
                get();
                if (peek() == '*') {
                    get();
                    if (peek() == '=') { get(); out.push_back({tok_kind::dstar_eq, {}, 0, 0.0, at}); }
                    else out.push_back({tok_kind::dstar, {}, 0, 0.0, at});
                } else if (peek() == '=') { get(); out.push_back({tok_kind::star_eq, {}, 0, 0.0, at}); }
                else out.push_back({tok_kind::star, {}, 0, 0.0, at});
                break;
            case '/':
                get();
                if (peek() == '/') {
                    get();
                    if (peek() == '=') { get(); out.push_back({tok_kind::dslash_eq, {}, 0, 0.0, at}); }
                    else out.push_back({tok_kind::dslash, {}, 0, 0.0, at});
                } else if (peek() == '=') { get(); out.push_back({tok_kind::slash_eq, {}, 0, 0.0, at}); }
                else out.push_back({tok_kind::slash, {}, 0, 0.0, at});
                break;
            case '%':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::percent_eq, {}, 0, 0.0, at}); }
                else out.push_back({tok_kind::percent, {}, 0, 0.0, at});
                break;
            case '&':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::amp_eq, {}, 0, 0.0, at}); }
                else out.push_back({tok_kind::amp, {}, 0, 0.0, at});
                break;
            case '|':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::pipe_eq, {}, 0, 0.0, at}); }
                else out.push_back({tok_kind::pipe, {}, 0, 0.0, at});
                break;
            case '^':
                get();
                if (peek() == '=') { get(); out.push_back({tok_kind::caret_eq, {}, 0, 0.0, at}); }
                else out.push_back({tok_kind::caret, {}, 0, 0.0, at});
                break;
            default:
                lex_fail(std::string("unexpected character '") + c + "'", at, file);
            }
        }
    }
};

} // namespace

std::string pymini_decode_escapes(std::string_view raw, src_pos at,
                                  const std::string& file) {
    return lexer::decode_escapes_impl(raw, at, file);
}

std::vector<token> lex_source(std::string_view source, const std::string& file) {
    lexer lx{source, file};
    lx.run();
    return std::move(lx.out);
}

} // namespace sao::plugins::pymini
