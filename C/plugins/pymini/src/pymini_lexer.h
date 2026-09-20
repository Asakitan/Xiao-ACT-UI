// pymini_lexer.h — Python tokenizer with INDENT/DEDENT and f-strings.
#pragma once

#include "pymini_common.h"

namespace sao::plugins::pymini {

enum class tok_kind : uint8_t {
    // literals & names
    name, number, string, bytes_, fstring,
    // structure
    newline, indent, dedent, eof_,
    // punctuation
    lparen, rparen, lbracket, rbracket, lbrace, rbrace,
    comma, colon, semicolon, dot, arrow, at, ellipsis,
    // operators
    plus, minus, star, dstar, slash, dslash, percent, tilde, caret, amp, pipe,
    lshift, rshift,
    assign, plus_eq, minus_eq, star_eq, dstar_eq, slash_eq, dslash_eq,
    percent_eq, amp_eq, pipe_eq, caret_eq, lshift_eq, rshift_eq, at_eq,
    eq, ne, lt, le, gt, ge,
};

struct token {
    tok_kind kind;
    std::string text;          // name/literal payload
    int64_t int_value = 0;     // decoded integer
    double num_value = 0.0;    // decoded float
    src_pos pos;
    bool raw_str = false;      // r/R-prefix literal — escape-decoded already
                               // for str/bytes; for fstring the parser
                               // decides per literal chunk
};

// Decode \n \xNN \uNNNN-style escape sequences in a string-literal body.
// `file` names the module for diagnostics; throws py_error{SyntaxError} on
// malformed escapes. Exported so the f-string parser can decode literal
// chunks with the same rules the string lexer uses.
std::string pymini_decode_escapes(std::string_view raw, src_pos at,
                                  const std::string& file);

// Tokenize `source` (utf8). On error throws py_error{SyntaxError}.
// `file` names the module for diagnostics.
std::vector<token> lex_source(std::string_view source, const std::string& file);

// Scan-time feature flags for the preflight/unsupported check (cheap
// AST-independent probe feeding the composite adapter's arbitration).
struct feature_flags {
    bool uses_async = false;
    bool uses_yield = false;
    bool uses_match = false;
    bool uses_walrus = false;
    bool uses_ctypes = false;
    bool uses_eval_exec = false;
    bool uses_nonlocal = false;
    bool uses_global = false;
    bool uses_class = false;
    bool uses_decorator = false;
    bool uses_fstring = false;
    bool uses_comprehension = false;
    bool uses_lambda = false;
    bool uses_star_args = false;
    bool uses_with = false;
    bool uses_yield_from = false;
    bool uses_relative_import = false;
    bool uses_absolute_import = false;
    std::vector<std::string> imported_roots;   // top-level module names imported
    std::vector<std::string> unsupported;      // human-readable unsupported list
};

} // namespace sao::plugins::pymini
