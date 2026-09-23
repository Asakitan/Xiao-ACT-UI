// pymini_interp.h — tree-walk evaluator + interpreter object.
//
// Scope model: LEGB via captured scope dicts rather than CPython cells —
// each function body runs in a fresh locals dict; closures capture the
// enclosing locals dicts (chain).  `global` writes to the module dict,
// `nonlocal` searches the captured chain.  Class bodies run in a temp dict
// that becomes the class attrs.
//
// Control flow: C++ exceptions carry signals —
//   sig_raise{exc PyRef}, sig_return{value}, sig_break, sig_continue.
// Script-visible exceptions are PyExcObj values inside sig_raise.
#pragma once

#include "pymini_ast.h"
#include "pymini_value.h"

namespace sao::plugins::loader {
struct plugin_context_s;
struct context_runtime_state;
typedef plugin_context_s plugin_context_t;
}

namespace sao::plugins::pymini {

// ── signals ───────────────────────────────────────────────────────────────
struct py_signal {
    virtual ~py_signal() = default;
};
struct sig_raise : py_signal {
    PyRef exc;
    explicit sig_raise(PyRef e) : exc(std::move(e)) {}
};
struct sig_return : py_signal {
    PyRef value;
    explicit sig_return(PyRef v) : value(std::move(v)) {}
};
struct sig_break : py_signal {};
struct sig_continue : py_signal {};

// ── frame ─────────────────────────────────────────────────────────────────
struct frame {
    PyRef locals;                                 // PyDictObj
    PyRef globals;                                // PyDictObj (module dict)
    std::vector<PyRef> captured;                  // enclosing scope chain (closures)
    std::unordered_set<std::string> globals_set;  // `global` decls
    std::unordered_set<std::string> nonlocals_set;// `nonlocal` decls
    std::string fn_name;
    std::string file;
    uint32_t line = 0;                            // current line (diagnostics)
    bool in_class_body = false;
    frame* caller = nullptr;
    frame* named_expr_scope = nullptr;
    std::vector<PyRef>* yield_sink = nullptr;
};

// ── interpreter ───────────────────────────────────────────────────────────
struct import_request;    // fwd — defined in pymini_imports.h

class interpreter {
  public:
    struct config {
        loader::plugin_context_t* ctx = nullptr;  // loader plugin ctx (may be null in probes)
        std::string plugin_id;
        std::wstring plugin_root;                 // sandbox root dir
        std::vector<std::wstring> module_dirs;    // search dirs (engine/libs/vendor…)
        std::function<void(const std::string&)> log_hook;   // print→ctx.log
        int max_call_depth = 900;
    };

    explicit interpreter(config c);
    ~interpreter();
    interpreter(const interpreter&) = delete;
    interpreter& operator=(const interpreter&) = delete;

    // ── modules ──
    PyRef exec_module_source(const std::string& logical_name,
                             const std::string& file_utf8,
                             std::string_view source,
                             PyRef prepared_module = {});
    PyRef import_dotted(const std::string& dotted, frame* from,
                        int level = 0);
    PyRef import_from(PyRef module, const std::string& name);
    PyRef find_loaded(const std::string& name);

    // ── eval/exec/call ──
    PyRef eval(const ast_expr* e, frame& f);
    void exec(const ast_stmt* s, frame& f);
    void exec_body(const std::vector<stmt_ptr>& body, frame& f);
    void emit_yield(frame& f, PyRef value, src_pos pos);
    PyRef call(const PyRef& callable, const py_args& args, src_pos pos = {});
    PyRef call0(const PyRef& callable, src_pos pos = {});   // no args
    PyRef call1(const PyRef& callable, PyRef a, src_pos pos = {});
    PyRef call_method(const PyRef& obj, const std::string& name,
                      const py_args& args, src_pos pos = {});
    bool maybe_call_method(const PyRef& obj, const std::string& name,
                           const py_args& args, PyRef* out);

    // ── attributes ──
    PyRef getattr(const PyRef& obj, const std::string& name, bool* found = nullptr);
    bool setattr(PyRef obj, const std::string& name, PyRef value);
    bool hasattr(const PyRef& obj, const std::string& name);
    bool delattr(PyRef obj, const std::string& name);

    // ── iteration ──
    PyRef iter(const PyRef& obj);
    bool iter_next(const PyRef& it, PyRef* out);
    void for_each(const PyRef& iterable,
                  const std::function<bool(PyRef)>& visitor); // false→break

    // ── ops ──
    PyRef binary(tok_kind op, const PyRef& a, const PyRef& b, src_pos pos = {});
    PyRef unary(tok_kind op, const PyRef& a, src_pos pos = {});
    bool truthy(const PyRef& r);            // py_truthy + instance __bool__/__len__
    PyRef compare(tok_kind encoded_op, const PyRef& a, const PyRef& b, src_pos pos = {});
    bool contains(const PyRef& item, const PyRef& container);
    PyRef subscript_get(const PyRef& obj, const PyRef& index, src_pos pos = {});
    void subscript_set(const PyRef& obj, const PyRef& index, PyRef value, src_pos pos = {});
    void subscript_del(const PyRef& obj, const PyRef& index, src_pos pos = {});
    PyRef apply_call_args(const PyRef& callable, ast_expr* const* args_nodes,
                          std::size_t n_args, const call_arg* args_desc, frame& f);

    // ── exceptions ──
    [[noreturn]] void raise_exc(const std::string& type, const std::string& msg,
                                src_pos pos = {});
    [[noreturn]] void raise_obj(PyRef exc, src_pos pos = {});
    bool exc_matches(const PyRef& exc_val, const PyRef& type_expr_val);
    void record_frame(PyExcObj* exc);

    // ── internals shared with imports/calls TUs ──
    void exec_import(const ast_stmt* s, frame& f);
    void exec_with_item(const ast_stmt* s, frame& f, std::size_t item_index);
    void del_target(const ast_expr* t, frame& f);
    PyRef eval_comprehension(const ast_expr* e, frame& f);
    void collect_scope_decls(const std::vector<stmt_ptr>& body, frame& f);

    // ── scope/binding ──
    PyRef scope_get(const frame& f, const std::string& name, bool* found);
    void scope_set(frame& f, const std::string& name, PyRef value);
    void scope_del(frame& f, const std::string& name);
    void bind_unpack(const ast_expr* target, PyRef value, frame& f);
    void bind_param_frame(frame& callee, const PyFuncObj* fn, const py_args& args,
                          src_pos pos);

    // ── builtins/module tables ──
    PyRef builtins_dict;                      // dict
    PyRef sys_modules;                        // dict: name→module
    std::vector<PyRef> pending;               // module exec stack (import reentry)
    config cfg;
    std::weak_ptr<loader::context_runtime_state> context_lifetime;
    std::recursive_mutex gil;
    frame* cur_frame = nullptr;
    uint32_t call_depth = 0;
    std::function<void(const std::string&)> on_log;
    PyRef active_exc;                         // last raised/current-handled exc

    // module factories populated by pymini_stdlib.cpp
    using module_factory = PyRef (*)(interpreter&);
    std::unordered_map<std::string, module_factory> stdlib_factories;

    // hooks from ctx/imports
    PyRef create_module_object(const std::string& name,
                               const std::string& path_utf8 = {},
                               const std::string& package = {});
    void register_module(PyRef module);       // sys.modules[name] = m

    // Known external dependencies, checked only after native/local resolution.
    static const std::unordered_set<std::string>& cpython_only_roots();
};

// free helpers used across pymini TUs.
PyRef file_lines(const PyRef& file_obj);   // stdlib2: remaining lines list

// stdlib factory registration (pymini_stdlib.cpp / pymini_stdlib2.cpp)
void pymini_register_stdlib_factories_1(interpreter& i);
void pymini_register_stdlib_factories_2(interpreter& i);
// builtins installation (pymini_builtins.cpp) — populates builtins_dict with
// builtin fns/types/exception classes; required before user source runs or
// every name lookup misses with NameError.
void pymini_install_builtins(interpreter& i);
// ctx object bound to `i.cfg` — defined in pymini_ctx.cpp.
PyRef pymini_make_ctx(interpreter& i);
// False denotes a known unsupported feature; malformed input throws py_error.
bool pymini_preflight_subset(std::string_view src,
                             std::string* out_reason);
bool pymini_preflight_plugin(const std::wstring& plugin_root,
                              const std::wstring& entry_rel,
                              const std::vector<std::wstring>& extra_dirs,
                              std::string* out_reason);
std::string pymini_configure_imports(interpreter::config& cfg,
                                     const std::wstring& plugin_root,
                                     const std::wstring& entry_rel,
                                     const std::vector<std::wstring>& extra_dirs);
// Drops every registered py callback box (host teardown order: loader
// unregister first, then this, then interpreter destruction).
void pymini_drop_callbacks(interpreter& i);
inline void pymini_register_stdlib(interpreter& i) {
    pymini_register_stdlib_factories_1(i);
    pymini_register_stdlib_factories_2(i);
    // builtins_dict is empty until this runs — every bare-name lookup
    // (str/int/ValueError/...) would NameError without it.
    pymini_install_builtins(i);
}
// sandboxed open() helper (stdlib2); ctx binds the same primitive.
PyRef pymini_open_file(interpreter& i, const std::string& path_utf8,
                       const std::string& mode);
// stdlib2 kinds (counter_/deque_/queue_/file_/range extras) member table;
// returns nullptr when the name is not a builtin member.
PyRef pymini_extra_member(interpreter& i, const PyRef& obj,
                          const std::string& name);
PyRef make_exc(const std::string& type, const std::string& msg);
expr_ptr clone_expr(const ast_expr* e);       // deep clone (lambda bodies)
const char* py_type_name(const PyRef& r);     // str(type())-style name
void set_add(interpreter& i, std::vector<PyRef>& items, const PyRef& v);
// member resolution for builtin kinds (str/list/dict/...); defined in
// pymini_builtins.cpp.
PyRef getattr_builtin_member(interpreter& i, const PyRef& obj,
                             const std::string& name);

// script_ctx engine-provider lifecycle (pymini_bridge.cpp).
int pymini_register_script_engine() noexcept;
int pymini_unregister_script_engine() noexcept;

// per-plugin interpreter host (pymini_host.cpp) — adapter-facing API.
int pymini_host_load_plugin(loader::plugin_context_t* ctx,
                            const char* plugin_id,
                            const wchar_t* plugin_root,
                            const wchar_t* entry_rel,
                            const std::vector<std::wstring>& extra_dirs,
                            std::string* out_err);
int pymini_host_call_hook(const char* plugin_id, const char* hook,
                          const char* payload_json, std::string* out_err);
int pymini_host_call_hook_ret(const char* plugin_id, const char* hook,
                              const char* payload_json, PyRef* out_ret,
                              std::string* out_err);
// hook + interpreter-aware truthiness of the return value in one locked call
// (on_unload veto path — instance __bool__/__len__ honored).
int pymini_host_call_hook_allow_unload(const char* plugin_id, const char* hook,
                                       bool* allow_unload,
                                       std::string* out_err);
int pymini_host_unload_plugin(const char* plugin_id,
                              std::string* out_err);
bool pymini_host_is_loaded(const char* plugin_id);
std::vector<std::string> pymini_host_loaded_ids();
int pymini_host_entry_changed(const char* plugin_id, bool* changed);
int pymini_host_ctx_object(const char* plugin_id, PyRef* out);
int pymini_host_module_attr(const char* plugin_id, const char* name,
                            PyRef* out);

// gil_guard impl (needs interpreter definition).
inline gil_guard::gil_guard(interpreter& i) : interp_(i) { interp_.gil.lock(); }
inline gil_guard::~gil_guard() { interp_.gil.unlock(); }

} // namespace sao::plugins::pymini
