// pymini_ast.h — AST node types (tagged structs, shared payloads).
//
// The parser produces `ast_module{body}`; the interpreter walks nodes
// directly (no separate IR).  Nodes are owned via unique_ptr / shared_ptr:
// function bodies are `shared_ptr` so PyFuncObj can retain them for later
// calls without dangling.
#pragma once

#include "pymini_common.h"
#include "pymini_lexer.h"
#include "pymini_value.h"

namespace sao::plugins::pymini {

// ── expressions ───────────────────────────────────────────────────────────
struct ast_expr;
struct ast_stmt;
using expr_ptr = std::unique_ptr<ast_expr>;
using stmt_ptr = std::shared_ptr<ast_stmt>;   // shared: PyFuncObj retains bodies

// param declaration shared by lambda/def (default is an expr).
struct ast_param_decl {
    std::string name;
    expr_ptr default_value;
    bool varargs = false;
    bool kwonly = false;
    bool kwarg = false;                     // **kwargs
};

enum class et : uint8_t {
    invalid,
    literal,        // const_value
    name,           // name
    attr,           // base.name
    subscript,      // base[index]  (index may be slice_lit)
    slice_lit,      // a:b:c inside subscript — lower/upper/step
    call,           // base(call_args)
    binop,          // base op rhs (op in tok_kind)
    unop,           // op on base
    boolop,         // `and`/`or` chain — parts; op = and/or keyword marker via boolop_and
    compare,        // base <op> parts[0] <op> parts[1] … — ops[]
    ifexp,          // base if cond else orelse
    lambda_,        // params + body (body = base)
    list_lit, tuple_lit, set_lit,   // parts
    dict_lit,       // pairs: key/value alternating in parts (None key = **merge)
    star_,          // *base
    fstring_,       // parts: literal str items + expr items (formatted)
    comprehension,  // elt=base, generators[]
    ellipses_,      // ...
};

// one comprehension clause: `for x in it [if cond]* [for y in j2]...`
struct comp_clause {
    expr_ptr target;
    expr_ptr iter;
    std::vector<expr_ptr> ifs;
    bool is_async = false;          // parsed but rejected at eval
};

struct call_arg {
    expr_ptr value;                 // expr or star/kwarg expr
    std::string kw;                 // keyword name or "" (positional)
    bool star = false;              // *args
    bool dstar = false;             // **kwargs
};

struct ast_expr {
    et tag = et::invalid;
    src_pos pos{};
    PyRef const_value;                      // literal
    std::string name;                       // name / attr-name / fstring raw
    tok_kind op = tok_kind::eof_;
    expr_ptr base;                          // primary operand
    expr_ptr index;                         // subscript index / ifexp cond / slice nodes
    expr_ptr orelse;                        // ifexp else / slice upper
    expr_ptr step;                          // slice step
    std::vector<expr_ptr> parts;            // lists/compare rhs/fstring parts
    std::vector<tok_kind> ops;              // compare ops
    std::vector<call_arg> call_args;        // call args
    std::vector<comp_clause> generators;    // comprehension clauses
    std::vector<ast_param_decl> params;     // lambda params
    bool fstring_raw_flag = false;          // fstring fragment flag (unused)
};

// ── statements ────────────────────────────────────────────────────────────
enum class st : uint8_t {
    invalid,
    expr_stmt,
    assign,         // targets = value
    aug_assign,     // target op= value
    ann_assign,     // target: ann = value (annotation ignored)
    del_,
    pass_,
    break_,
    continue_,
    return_,
    raise_,
    assert_,
    global_,
    nonlocal_,
    if_,
    while_,
    for_,
    try_,
    with_,
    funcdef,
    classdef,
    import,         // import a.b as c
    import_from,    // from .x import a as b
    suite,          // `;`-joined simple statements on one line
};

struct except_arm {
    expr_ptr type;                          // may be null → bare except
    std::string name;                       // `as name`
    std::vector<stmt_ptr> body;
};

struct with_item {
    expr_ptr ctx_expr;
    expr_ptr target;                        // `as target` may be null
};

struct ast_stmt {
    st tag = st::invalid;
    src_pos pos{};
    std::string name;                       // def/class/alias/for target names
    expr_ptr value;                         // expr/test/return/raise/assert
    expr_ptr value2;                        // aug rhs source / assert msg / assert expr2
    std::vector<expr_ptr> targets;          // assign targets / del names / for target
    tok_kind aug_op = tok_kind::eof_;
    std::vector<stmt_ptr> body;             // suite
    std::vector<stmt_ptr> orelse;           // else: / elif-else
    std::vector<stmt_ptr> final;            // finally:
    std::vector<except_arm> except_arms;    // try
    std::vector<with_item> with_items;
    // funcdef / classdef
    std::vector<ast_param_decl> params;
    std::vector<expr_ptr> decorators;
    std::vector<expr_ptr> bases;
    std::vector<std::pair<std::string, expr_ptr>> kw_bases; // class k=v
    bool is_async = false;
    // import / import_from
    std::vector<std::pair<std::string, std::string>> imports; // (dotted_name, alias)
    std::string from_module;                // import_from base ("pkg.mod")
    int from_level = 0;                     // leading dots
    // global/nonlocal
    std::vector<std::string> names;
};

struct ast_module {
    std::vector<stmt_ptr> body;
    std::string file;
};

} // namespace sao::plugins::pymini
