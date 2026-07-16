// emma_lexer.cpp — Emma tokenizer 状态机实装
//
// 对齐 Python emma_runtime.py _tokenize / _TOKEN_RE。C++ 侧手写状态机, 支持
// Python 侧全部 token 类型 + 两种注释:
//   - `--` (Python 侧唯一注释, 权威)
//   - `#`  (task Wave 3 需求, 兼容 script/shebang 风格)
//
// 关键字集合与 Python _KEYWORDS 一致:
//   let / fn / end / if / elif / else / while / for / in / return /
//   and / or / not / true / false / nil / break / continue
//
// 操作符表:
//   .. == != <= >= => + - * / % = < > ! : ; , . ( ) [ ] { }
//
// 数字支持 0xHEX / int / float 含指数
// 字符串支持双/单引号 + \n \t \\ \" \' 转义 (对齐 Python _unescape)

#include "sao/plugins/emma_host/emma_lexer.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sao::plugins::emma_host {

namespace {

bool is_ident_start(char c) {
    return (c == '_') || std::isalpha(static_cast<unsigned char>(c));
}

bool is_ident_cont(char c) {
    return (c == '_') || std::isalnum(static_cast<unsigned char>(c));
}

bool is_digit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool is_hex_digit(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

// 对齐 python _KEYWORDS
bool is_keyword(const std::string& s) {
    static const char* const KW[] = {
        "let", "fn", "end", "if", "elif", "else", "while", "for", "in",
        "return", "and", "or", "not", "true", "false", "nil",
        "break", "continue"
    };
    for (const char* kw : KW) {
        if (s == kw) return true;
    }
    return false;
}

// 从字符串字面量剥掉外层引号 + 反转义 (对齐 python _unescape)
std::string unescape(const std::string& raw_with_quotes) {
    if (raw_with_quotes.size() < 2) return raw_with_quotes;
    // 去掉外层引号
    std::string s = raw_with_quotes.substr(1, raw_with_quotes.size() - 2);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            switch (n) {
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case '\\': out.push_back('\\'); break;
                case '"':  out.push_back('"'); break;
                case '\'': out.push_back('\''); break;
                case '0':  out.push_back('\0'); break;
                default:   out.push_back(n); break;   // 未知转义原样保留 (对齐 python)
            }
            ++i;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// 尝试匹配二字符运算符; 未命中返 0
int try_match_two_char_op(char c, char n) {
    // .. == != <= >= =>
    if (c == '.' && n == '.') return 2;
    if (c == '=' && n == '=') return 2;
    if (c == '!' && n == '=') return 2;
    if (c == '<' && n == '=') return 2;
    if (c == '>' && n == '=') return 2;
    if (c == '=' && n == '>') return 2;
    return 0;
}

// 判断单字符是否是我们支持的运算符 / 分隔符
bool is_single_char_op(char c) {
    switch (c) {
        case '+': case '-': case '*': case '/': case '%':
        case '=': case '<': case '>': case '!':
        case '(': case ')': case '[': case ']': case '{': case '}':
        case ',': case '.': case ':': case ';':
            return true;
        default:
            return false;
    }
}

} // namespace

std::vector<token> tokenize_source(std::string_view source) {
    std::vector<token> out;
    out.reserve(source.size() / 8 + 8);

    uint32_t line = 1;
    uint32_t col = 1;
    size_t i = 0;
    const size_t n = source.size();

    while (i < n) {
        char c = source[i];

        // ── 空白 (非 \n) ──
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i; ++col;
            continue;
        }

        // ── 换行 (Emma 里 newline 只是分隔符, 但语义上不产 token; 对齐 python) ──
        if (c == '\n') {
            ++i; ++line; col = 1;
            continue;
        }

        // ── 注释 ──
        // Python 权威用 `--`. Task 明说要 `#`. 两种都支持。
        if (c == '-' && i + 1 < n && source[i + 1] == '-') {
            // 吞到行尾
            while (i < n && source[i] != '\n') { ++i; ++col; }
            continue;
        }
        if (c == '#') {
            while (i < n && source[i] != '\n') { ++i; ++col; }
            continue;
        }

        // ── 字符串 (双/单引号, 支持转义) ──
        if (c == '"' || c == '\'') {
            char quote = c;
            uint32_t start_line = line;
            uint32_t start_col = col;
            size_t start = i;
            ++i; ++col;    // 吞开引号
            while (i < n) {
                char sc = source[i];
                if (sc == '\\' && i + 1 < n) {
                    if (source[i + 1] == '\n') { ++line; col = 1; }
                    else ++col;
                    i += 2; col += 1;
                    continue;
                }
                if (sc == quote) { ++i; ++col; break; }
                if (sc == '\n') { ++line; col = 1; }
                else ++col;
                ++i;
            }
            token t;
            t.kind = token_kind::string;
            t.value = unescape(std::string(source.substr(start, i - start)));
            t.line = start_line;
            t.column = start_col;
            out.push_back(std::move(t));
            continue;
        }

        // ── 数字 ──
        if (is_digit(c)) {
            uint32_t start_line = line;
            uint32_t start_col = col;
            size_t start = i;
            // 0x 前缀?
            if (c == '0' && i + 1 < n && (source[i + 1] == 'x' || source[i + 1] == 'X')) {
                i += 2; col += 2;
                while (i < n && is_hex_digit(source[i])) { ++i; ++col; }
            } else {
                while (i < n && is_digit(source[i])) { ++i; ++col; }
                // 小数点
                if (i < n && source[i] == '.' && i + 1 < n && is_digit(source[i + 1])) {
                    ++i; ++col;
                    while (i < n && is_digit(source[i])) { ++i; ++col; }
                }
                // 指数
                if (i < n && (source[i] == 'e' || source[i] == 'E')) {
                    ++i; ++col;
                    if (i < n && (source[i] == '+' || source[i] == '-')) { ++i; ++col; }
                    while (i < n && is_digit(source[i])) { ++i; ++col; }
                }
            }
            token t;
            t.kind = token_kind::number;
            t.value = std::string(source.substr(start, i - start));
            t.line = start_line;
            t.column = start_col;
            out.push_back(std::move(t));
            continue;
        }

        // ── 标识符 / 关键字 ──
        if (is_ident_start(c)) {
            uint32_t start_line = line;
            uint32_t start_col = col;
            size_t start = i;
            ++i; ++col;
            while (i < n && is_ident_cont(source[i])) { ++i; ++col; }
            std::string v(source.substr(start, i - start));
            token t;
            t.kind = is_keyword(v) ? token_kind::keyword : token_kind::ident;
            t.value = std::move(v);
            t.line = start_line;
            t.column = start_col;
            out.push_back(std::move(t));
            continue;
        }

        // ── 两字符运算符 ──
        if (i + 1 < n) {
            int match = try_match_two_char_op(c, source[i + 1]);
            if (match == 2) {
                token t;
                t.kind = token_kind::op;
                t.value = std::string(source.substr(i, 2));
                t.line = line;
                t.column = col;
                out.push_back(std::move(t));
                i += 2; col += 2;
                continue;
            }
        }

        // ── 单字符运算符 ──
        if (is_single_char_op(c)) {
            token t;
            t.kind = token_kind::op;
            t.value = std::string(1, c);
            t.line = line;
            t.column = col;
            out.push_back(std::move(t));
            ++i; ++col;
            continue;
        }

        // ── 未知字符: 报个 op token 让 parser 报错; 不 hard fail ──
        // (对齐 python 里 _tokenize 未匹配就跳过, 但我们至少留个痕迹方便定位)
        token t;
        t.kind = token_kind::op;
        t.value = std::string(1, c);
        t.line = line;
        t.column = col;
        out.push_back(std::move(t));
        ++i; ++col;
    }

    // EOF
    token eof;
    eof.kind = token_kind::eof;
    eof.line = line;
    eof.column = col;
    out.push_back(std::move(eof));

    return out;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_tokenize(const char* utf8_source,
                          size_t source_len,
                          token** out_tokens,
                          size_t* out_count) {
    if (utf8_source == nullptr || out_tokens == nullptr || out_count == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::vector<token> toks = tokenize_source(std::string_view(utf8_source, source_len));
    token* buf = static_cast<token*>(std::malloc(sizeof(token) * toks.size()));
    if (buf == nullptr) return SAO_ERR_OS_CALL_FAILED;
    // token 含 std::string, 用 placement new
    for (size_t k = 0; k < toks.size(); ++k) {
        new (&buf[k]) token(std::move(toks[k]));
    }
    *out_tokens = buf;
    *out_count = toks.size();
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_emma_tokens_free(token* tokens, size_t count) {
    if (tokens == nullptr) return;
    for (size_t k = 0; k < count; ++k) {
        tokens[k].~token();
    }
    std::free(tokens);
}

} // namespace sao::plugins::emma_host
