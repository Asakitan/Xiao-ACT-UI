// csmini_lexer.h — C# tokenizer (brace-scoped, mirrors pymini_lexer.h role).
#pragma once

#include "csmini_common.h"

namespace sao::plugins::csmini {

enum class tok_kind : uint8_t {
    // literals & names
    name, number, string, char_, interp_string, verbatim_string, keyword,
    // structure
    eof_,
    // punctuation
    lparen, rparen, lbracket, rbracket, lbrace, rbrace,
    comma, colon, semicolon, dot, qmark, arrow,        // => (expr-bodied)
    nullcond,                                        // ?.
    nullcoalesce,                                    // ??
    nullcoalesce_eq,                                 // ??=
    // operators
    plus, minus, star, slash, percent, tilde, caret, amp, pipe,
    lshift, rshift, bang,
    plus2, minus2,                                   // ++  --
    and2, or2,                                       // &&  ||
    assign, plus_eq, minus_eq, star_eq, slash_eq, percent_eq,
    amp_eq, pipe_eq, caret_eq, lshift_eq, rshift_eq,
    eq, ne, lt, le, gt, ge,
};

struct token {
    tok_kind kind;
    std::string text;                                // name/keyword/literal payload
    int64_t int_value = 0;                           // decoded int/char
    double num_value = 0;                            // decoded float
    bool is_float = false;                           // number parsed as double
    bool verbatim = false;                           // @"..." / $@"..." raw mode
    src_pos pos;
};

// Tokenize `source` (utf8).  On error throws cs_error{kind:"SyntaxError"}.
// `file` names the module for diagnostics.
std::vector<token> lex_source(std::string_view source, const std::string& file);

// interpolated-string parts: alternate literal chunks and `{expr}` sources
// (is_expr flag).  Returned by split_interp for parser re-entry; `verbatim`
// switches escape handling to "" / {{ }} doubling only.
struct interp_part {
    bool is_expr;
    std::string text;
};
std::vector<interp_part> split_interp(std::string_view raw, bool verbatim = false);

// Scan-time feature flags for the preflight/unsupported check.
struct feature_flags {
    std::vector<std::string> unsupported;            // feature names
    std::vector<std::string> using_roots;            // `using X` first segments
};

} // namespace sao::plugins::csmini
