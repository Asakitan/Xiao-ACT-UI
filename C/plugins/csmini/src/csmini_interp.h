// csmini_interp.h — tree-walk evaluator + interpreter object (mirrors
// pymini_interp.h).
//
// Scope model: flat locals chain per method frame — each block pushes a
// scope node; name resolution walks scope → this members → class statics →
// module globals → builtins. Lambda frames retain their lexical scope node.
//
// Control flow: C++ exceptions carry signals —
//   sig_raise{exc CsRef}, sig_return{value}, sig_break, sig_continue.
// Script-visible exceptions are CsExcObj values inside sig_raise.
#pragma once

#include "csmini_ast.h"
#include "csmini_parser.h"
#include "csmini_value.h"

namespace sao::plugins::loader {
struct plugin_context_s;
struct context_runtime_state;
typedef plugin_context_s plugin_context_t;
}

namespace sao::plugins::csmini {

// ── signals ───────────────────────────────────────────────────────────────
struct cs_signal {
    virtual ~cs_signal() = default;
};
struct sig_raise : cs_signal {
    CsRef exc;
    explicit sig_raise(CsRef e) : exc(std::move(e)) {}
};
struct sig_return : cs_signal {
    CsRef value;
    explicit sig_return(CsRef v) : value(std::move(v)) {}
};
struct sig_break : cs_signal {};
struct sig_continue : cs_signal {};

// ── scope / frame ─────────────────────────────────────────────────────────
struct cs_scope {
    cs_scope* parent = nullptr;
    std::unordered_map<std::string, CsRef> vars;
};

struct frame {
    cs_scope* scope = nullptr;                  // current innermost scope
    CsRef this_ref;                             // receiver (instance methods)
    CsRef class_ref;                            // owner class (statics + inst)
    std::string fn_name;
    std::string file;
    uint32_t line = 0;                          // current line (diagnostics)
    frame* caller = nullptr;
    std::shared_ptr<void> anchor;
};

// ── interpreter ───────────────────────────────────────────────────────────
class interpreter {
  public:
    struct config {
        loader::plugin_context_t* ctx = nullptr; // loader plugin ctx (may be null in probes)
        std::string plugin_id;
        std::wstring plugin_root;                // sandbox root dir
        std::vector<std::wstring> module_dirs;   // reserved (namespace-free imports)
        std::function<void(const std::string&)> log_hook;   // Console.WriteLine→ctx.log
        int max_call_depth = 900;
    };

    explicit interpreter(config c);
    ~interpreter();
    interpreter(const interpreter&) = delete;
    interpreter& operator=(const interpreter&) = delete;

    // ── modules ──
    // exec a .cs source inside a module globals dict; returns the dict.
    CsRef exec_module_source(const std::string& logical_name,
                             const std::string& file_utf8,
                             std::string_view source);
    CsRef find_loaded(const std::string& name);

    // ── eval/exec/call ──
    CsRef eval(const ast_expr* e, frame& f);
    void exec(const ast_stmt* s, frame& f);
    void exec_body(const std::vector<stmt_ptr>& body, frame& f);
    CsRef call(const CsRef& callable, const cs_args& args, src_pos pos = {});
    CsRef call0(const CsRef& callable, src_pos pos = {});
    CsRef call1(const CsRef& callable, CsRef a, src_pos pos = {});
    CsRef call_method(const CsRef& obj, const std::string& name,
                      const cs_args& args, src_pos pos = {});
    bool maybe_call_method(const CsRef& obj, const std::string& name,
                           const cs_args& args, CsRef* out);

    // ── attributes ──
    CsRef getattr(const CsRef& obj, const std::string& name, bool* found = nullptr);
    bool setattr(CsRef obj, const std::string& name, CsRef value);
    bool hasattr(const CsRef& obj, const std::string& name);

    // ── ops ──
    CsRef binary(tok_kind op, const CsRef& a, const CsRef& b, src_pos pos = {});
    CsRef unary(tok_kind op, const CsRef& a, src_pos pos = {});
    bool is_true(const CsRef& v) { return cs_truthy(v); }
    CsRef subscript_get(const CsRef& obj, const CsRef& index, src_pos pos = {});
    void subscript_set(const CsRef& obj, const CsRef& index, CsRef value, src_pos pos = {});
    CsRef assign_target(ast_expr* target, CsRef value, frame& f);
    CsRef incdec_target(ast_expr* target, int64_t delta, bool post, frame& f);
    CsRef cast_value(const std::string& type_text, const CsRef& v, src_pos pos = {});
    CsRef new_instance_named(const std::string& tn,
                             const std::vector<expr_ptr>& arg_exprs,
                             src_pos pos, frame& f);
    CsRef new_instance_eval(const std::string& tn, cs_args& a, src_pos pos,
                            frame& f);

    // ── instantiation / hooks ──
    CsRef instantiate(CsRef klass, const cs_args& args, src_pos pos);
    CsRef class_static(const CsRef& klass, const std::string& name, bool* found = nullptr);
    CsRef instance_member(const CsRef& obj, const std::string& name, bool* found = nullptr);

    // ── exceptions ──
    [[noreturn]] void raise_exc(const std::string& type, const std::string& msg,
                                src_pos pos = {});
    [[noreturn]] void raise_obj(CsRef exc, src_pos pos = {});
    bool exc_matches(const CsRef& exc_val, const std::string& type_text);
    void record_feature(const std::string& f);

    // ── scope/binding ──
    CsRef scope_get(const frame& f, const std::string& name, bool* found);
    void scope_set(frame& f, const std::string& name, CsRef value);
    bool scope_assign(frame& f, const std::string& name, CsRef value);
    void bind_param_frame(frame& callee, const CsFuncObj* fn, const cs_args& args,
                          src_pos pos);
    // invoke a CsFuncObj under a callee frame (this_ref may be null for
    // statics); used by call()/bound-method dispatch.
    CsRef invoke_func(CsFuncObj* fn, CsRef this_ref, const cs_args& args,
                      src_pos pos);
    // map a raw class pointer back to its owning CsRef (globals scan).
    CsRef find_owner_class(CsClassObj* co);

    // ── interpreter internals shared across TUs ──
    CsRef globals;                            // module dict (CsDictObj)
    std::unordered_set<std::string> features; // out-of-subset features hit
    config cfg;
    std::weak_ptr<loader::context_runtime_state> context_lifetime;
    std::recursive_mutex gil;
    frame* cur_frame = nullptr;
    uint32_t call_depth = 0;
    std::function<void(const std::string&)> on_log;
    cs_scope* alloc_scope(cs_scope* parent);
    CsRef eval_expr_in_frame(const ast_expr* e, frame* f);

  private:
    std::vector<std::unique_ptr<cs_scope>> scope_pool_;
    std::vector<CsRef> exc_stack_;              // caught-exc stack for bare `throw;`
    frame root_frame_{};
    cs_scope* global_scope_ = nullptr;
};

// ── free helpers used across csmini TUs ───────────────────────────────────
// builtins installation (csmini_builtins.cpp) — injects the facade objects
// (Console/Math/Convert/DateTime/JsonSerializer/System/collections + the
// primitive-type facades) into `i.globals`; required before user source runs.
void csmini_install_builtins(interpreter& i);
void csmini_install_stdlib(interpreter& i);     // csmini_stdlib.cpp extras
void csmini_install_stdlib2(interpreter& i);    // csmini_stdlib2.cpp extras
// value-member tables (csmini_stdlib.cpp): resolves string/array/dict/prim
// member access without an interpreter-owned instance.
CsRef csmini_value_member(interpreter& i, const CsRef& obj,
                          const std::string& name, bool* found);
CsRef csmini_value_callable_member(interpreter& i, const CsRef& obj,
                                   const std::string& name, bool* found);
CsRef csmini_enumerable_call(interpreter& i, std::string_view name,
                             const CsRef& source, const cs_args& args);
CsRef csmini_make_task(CsRef result, bool value_task = false);
CsRef csmini_await_value(interpreter& i, const CsRef& value,
                         src_pos pos = {});
// ctx object bound to `i.cfg` — defined in csmini_ctx.cpp.
CsRef csmini_make_ctx(interpreter& i);
// Drops every registered cs callback box (host teardown order: loader
// unregister first, then this, then interpreter destruction).
void csmini_drop_callbacks(interpreter& i);
// `new Exception(...)`/typed-exception construction — csmini_stdlib2.cpp.
CsRef csmini_new_exception_type(interpreter& i, const std::string& type_text,
                                const cs_args& args, src_pos pos);

// guard
inline cs_guard::cs_guard(interpreter& i) : interp_(i) { interp_.gil.lock(); }
inline cs_guard::~cs_guard() { interp_.gil.unlock(); }

} // namespace sao::plugins::csmini
