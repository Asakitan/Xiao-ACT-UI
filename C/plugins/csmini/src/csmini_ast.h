// csmini_ast.h — C# AST node types (mirrors pymini_ast.h role).
//
// The parser produces `ast_program{usings, classes}`; the interpreter walks
// nodes directly.  Method bodies are `shared_ptr` so CsFuncObj can retain
// them for later calls without dangling.
#pragma once

#include "csmini_common.h"
#include "csmini_lexer.h"
#include "csmini_value.h"

namespace sao::plugins::csmini {

struct ast_expr;
struct ast_stmt;
using expr_ptr = std::unique_ptr<ast_expr>;
using stmt_ptr = std::shared_ptr<ast_stmt>;   // shared: CsFuncObj retains bodies

// ── expressions ───────────────────────────────────────────────────────────
enum class et : uint8_t {
    invalid,
    literal,        // const_value
    name,           // name (variable/member/type/namespace)
    member,         // base.name
    index,          // base[index]
    call,           // base(args)
    binop,          // base op parts[0] (op in tok_kind)
    unop,           // op on base (postfix flag in `post`)
    ternary,        // index ? base : orelse
    interp,         // parts: literal str const_values + expr items
    new_expr,       // type_name + call_args + init entries (parts)
    cast,           // (type_name)base
    this_,          // this
    typeof_,        // typeof(type_name) — placeholder-safe
    default_,       // default / default(type_name)
    throw_unsupported, // `is`/`as`/`nameof` etc — evaluated via base
};

struct ast_expr {
    et tag = et::invalid;
    src_pos pos{};
    CsRef const_value;                          // literal
    std::string name;                           // name / member-name / type
    tok_kind op = tok_kind::eof_;
    expr_ptr base;                              // primary operand
    expr_ptr index;                             // subscript index / ternary cond
    expr_ptr orelse;                            // ternary else
    std::vector<expr_ptr> parts;                // binop rhs / interp parts / init
    std::vector<expr_ptr> call_args;
    std::vector<std::string> init_names;        // object-init member names (may be "")
    bool post = false;                          // postfix ++/-- vs prefix
};

// ── statements ────────────────────────────────────────────────────────────
enum class st : uint8_t {
    invalid,
    expr_stmt,
    local_decl,     // type name = value?  (value nullable; multiple names in `names`)
    assign,         // target = value (compound via aug_op)
    incdec,         // target++/-- (op plus2/minus2, post flag)
    if_,
    for_,
    foreach_,       // type name in value
    while_,
    do_,
    return_,
    break_,
    continue_,
    block,
    try_,
    throw_,
};

struct catch_arm {
    std::string type_name;                      // "Exception" or "" (bare)
    std::string name;                           // bound variable or ""
    expr_ptr filter;                            // `when (expr)` — may be null
    std::vector<stmt_ptr> body;
};

struct ast_stmt {
    st tag = st::invalid;
    src_pos pos{};
    std::string type_name;                      // declared type text
    std::string name;                           // decl target name
    std::vector<std::pair<std::string, expr_ptr>> names;  // multi-decl a,b,c = ...
    expr_ptr value;                             // init / test / return / throw / iterable
    expr_ptr value2;                            // for-cond / assign rhs
    tok_kind aug_op = tok_kind::eof_;
    bool post = false;                          // postfix ++/--
    std::vector<stmt_ptr> body;
    std::vector<stmt_ptr> orelse;               // else block
    std::vector<stmt_ptr> iter;                 // for-iterators
    std::vector<stmt_ptr> init;                 // for-init (block variant)
    std::vector<catch_arm> catches;
    std::vector<stmt_ptr> final;                // finally
};

// ── declarations ──────────────────────────────────────────────────────────
struct ast_param {
    std::string type;
    std::string name;
    expr_ptr default_value;                     // may be null
};

enum class member_kind : uint8_t { field, method, ctor };

struct ast_member {
    member_kind kind = member_kind::field;
    src_pos pos{};
    // modifiers: only `static`/`const`/`readonly` affect runtime; the rest
    // (accessibility/virtual/abstract/…) are parsed but runtime-ignored.
    bool is_static = false;
    bool is_const = false;
    bool is_readonly = false;
    std::string type_name;                      // return/field type text
    std::string name;
    expr_ptr init;                              // field initializer
    std::vector<ast_param> params;
    std::vector<stmt_ptr> body;                 // method body (empty → expression body / none)
    expr_ptr expr_body;                         // `=> expr` member body
};

struct ast_class {
    std::string name;
    std::string ns;                             // enclosing namespace dotted
    std::vector<ast_member> members;
    src_pos pos{};
};

struct ast_program {
    std::vector<std::string> usings;            // `using X.Y.Z` dotted paths
    std::vector<std::string> using_aliases;     // `using X = ...` (feature-marked)
    std::vector<ast_class> classes;
};

} // namespace sao::plugins::csmini
