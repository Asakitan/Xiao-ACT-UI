// csmini_value.h — csmini object/value model (mirrors pymini_value.h).
//
// Uniform shared_ptr value graph: every value is `CsRef`
// (shared_ptr<CsObj>).  Scalars are heap objects too — null/true/false are
// process singletons.  Cycles (globals dict ↔ functions, class ↔ methods)
// are expected; the interpreter breaks them at teardown by clearing scope
// and class dicts explicitly.
//
// Builtin signature: CsRef fn(interpreter&, args).
#pragma once

#include "csmini_common.h"

#include <functional>
#include <variant>

namespace sao::plugins::csmini {

class interpreter;
struct CsObj;
using CsRef = std::shared_ptr<CsObj>;

// Positional args for native callables (no kwargs in the C# subset).
struct cs_args {
    std::vector<CsRef> pos;
    std::size_t size() const noexcept { return pos.size(); }
};

enum class cs_kind : uint8_t {
    null_, boolean, integer, number, string, char_,
    array, dict,
    func, builtin, bound_method,
    class_, instance, native_obj, exception_,
};

struct CsObj {
    cs_kind kind;
    explicit CsObj(cs_kind k) : kind(k) {}
    CsObj(const CsObj&) = delete;
    CsObj& operator=(const CsObj&) = delete;
    virtual ~CsObj() = default;
};

// ── scalar objects ────────────────────────────────────────────────────────
struct CsNullObj : CsObj { CsNullObj() : CsObj(cs_kind::null_) {} };
struct CsBoolObj : CsObj {
    bool v;
    explicit CsBoolObj(bool x) : CsObj(cs_kind::boolean), v(x) {}
};
struct CsIntObj : CsObj {
    int64_t v;
    explicit CsIntObj(int64_t x) : CsObj(cs_kind::integer), v(x) {}
};
struct CsFloatObj : CsObj {
    double v;
    explicit CsFloatObj(double x) : CsObj(cs_kind::number), v(x) {}
};
struct CsStrObj : CsObj {
    std::string v;
    explicit CsStrObj(std::string x) : CsObj(cs_kind::string), v(std::move(x)) {}
};
// char literal — stored as its unicode code point.
struct CsCharObj : CsObj {
    int64_t v;
    explicit CsCharObj(int64_t x) : CsObj(cs_kind::char_), v(x) {}
};

// ── containers ────────────────────────────────────────────────────────────
// C# array / List<T> — same object, List<T> methods attach by name.
struct CsArrayObj : CsObj {
    std::vector<CsRef> v;
    CsArrayObj() : CsObj(cs_kind::array) {}
    explicit CsArrayObj(std::vector<CsRef> x) : CsObj(cs_kind::array), v(std::move(x)) {}
};

// Dictionary<K,V> / object literal — insertion-ordered pairs keyed by any
// value (equality via cs_eq); string keys are the JSON-shaped common case.
struct CsDictObj : CsObj {
    std::vector<std::pair<CsRef, CsRef>> items;
    CsDictObj() : CsObj(cs_kind::dict) {}
};

// ── callables ─────────────────────────────────────────────────────────────
using cs_native_fn = std::function<CsRef(interpreter&, const cs_args&)>;

struct CsBuiltinObj : CsObj {
    std::string name;
    cs_native_fn fn;
    CsBuiltinObj(std::string n, cs_native_fn f)
        : CsObj(cs_kind::builtin), name(std::move(n)), fn(std::move(f)) {}
};

struct cs_param {
    std::string type;                       // declared type text (runtime-ignored)
    std::string name;
    bool has_default = false;
    // default initializer expression index into `default_exprs` — kept on the
    // function object so param nodes stay trivially copyable.
};

struct ast_stmt;
struct CsFuncObj : CsObj {
    std::string name;
    std::vector<cs_param> params;
    std::vector<struct ast_expr*> default_exprs;  // aligned w/ params (null → required)
    std::shared_ptr<std::vector<std::shared_ptr<ast_stmt>>> body;  // null → expr-body
    std::shared_ptr<ast_stmt> expr_body;          // `=> expr` member body
    std::shared_ptr<void> anchor;                 // keeps owning ast_program alive
    bool is_static = false;
    bool is_ctor = false;
    struct CsClassObj* owner_class = nullptr;    // raw ptr: class keeps fn alive
    CsFuncObj() : CsObj(cs_kind::func) {}
};

struct CsBoundMethodObj : CsObj {
    CsRef self;                                 // instance (or class for static)
    CsRef fn;
    CsBoundMethodObj(CsRef s, CsRef f) : CsObj(cs_kind::bound_method), self(std::move(s)), fn(std::move(f)) {}
};

// ── class machinery ───────────────────────────────────────────────────────
// attrs: shared member bag — static members (methods/static fields/consts)
// and instance members coexist; `inst_only` marks instance-declared names so
// `Class.member` can reject them.  Field initializers for instance fields
// are evaluated once per `new` against the fresh instance.
struct CsClassObj : CsObj {
    std::string name;
    std::string ns;                               // enclosing namespace dotted (may be "")
    CsRef attrs;                                // CsDictObj: name → member
    std::unordered_set<std::string> inst_only;  // instance fields/methods
    std::vector<std::pair<std::string, struct ast_expr*>> inst_field_inits;
    std::shared_ptr<void> anchor;                 // keeps owning ast_program alive
    CsClassObj() : CsObj(cs_kind::class_) {}
};

struct CsInstanceObj : CsObj {
    CsRef klass;                                // CsClassObj
    CsRef attrs;                                // CsDictObj: field values
    CsInstanceObj(CsRef k, CsRef a) : CsObj(cs_kind::instance), klass(std::move(k)), attrs(std::move(a)) {}
};

// Bag of named members for native facades (ctx, Console, Math, List<T> type
// objects, System namespaces, typeof() placeholders).  `ci` enables
// case-insensitive member lookup — ctx sets it so `ctx.Log` and `ctx.log`
// both resolve.
struct CsNativeObj : CsObj {
    std::string name;
    CsRef members;                              // CsDictObj
    bool ci = false;
    CsRef payload;                              // opaque tagged payload (engine handle etc.)
    CsNativeObj(std::string n, CsRef m, bool i = false)
        : CsObj(cs_kind::native_obj), name(std::move(n)), members(std::move(m)), ci(i) {}
};

// ── exceptions ────────────────────────────────────────────────────────────
struct CsExcObj : CsObj {
    std::string type_name;                      // "Exception", "InvalidOperationException"...
    std::string message;
    std::vector<std::string> trace;             // "file:line in fn" frames
    CsExcObj(std::string t, std::string m)
        : CsObj(cs_kind::exception_), type_name(std::move(t)), message(std::move(m)) {}
};

// ── singletons + factories ────────────────────────────────────────────────
CsRef cs_null();
CsRef cs_true();
CsRef cs_false();
CsRef cs_bool(bool v);
CsRef cs_int(int64_t v);
CsRef cs_float(double v);
CsRef cs_str(std::string_view v);
CsRef cs_str(const char* v);
CsRef cs_char(int64_t v);
CsRef cs_array(std::vector<CsRef> v = {});
CsRef cs_dict();
CsRef cs_builtin(std::string name, cs_native_fn fn);
CsRef cs_exc(const std::string& type, const std::string& msg);
CsRef cs_native(std::string name, bool ci = false);

// fast casts (return nullptr when wrong kind)
inline CsStrObj* as_str(const CsRef& r) { return r && r->kind == cs_kind::string ? static_cast<CsStrObj*>(r.get()) : nullptr; }
inline CsIntObj* as_int(const CsRef& r) { return r && r->kind == cs_kind::integer ? static_cast<CsIntObj*>(r.get()) : nullptr; }
inline CsFloatObj* as_float(const CsRef& r) { return r && r->kind == cs_kind::number ? static_cast<CsFloatObj*>(r.get()) : nullptr; }
inline CsBoolObj* as_bool(const CsRef& r) { return r && r->kind == cs_kind::boolean ? static_cast<CsBoolObj*>(r.get()) : nullptr; }
inline CsCharObj* as_char(const CsRef& r) { return r && r->kind == cs_kind::char_ ? static_cast<CsCharObj*>(r.get()) : nullptr; }
inline CsArrayObj* as_array(const CsRef& r) { return r && r->kind == cs_kind::array ? static_cast<CsArrayObj*>(r.get()) : nullptr; }
inline CsDictObj* as_dict(const CsRef& r) { return r && r->kind == cs_kind::dict ? static_cast<CsDictObj*>(r.get()) : nullptr; }
inline CsExcObj* as_exc(const CsRef& r) { return r && r->kind == cs_kind::exception_ ? static_cast<CsExcObj*>(r.get()) : nullptr; }
inline CsClassObj* as_class(const CsRef& r) { return r && r->kind == cs_kind::class_ ? static_cast<CsClassObj*>(r.get()) : nullptr; }
inline CsInstanceObj* as_inst(const CsRef& r) { return r && r->kind == cs_kind::instance ? static_cast<CsInstanceObj*>(r.get()) : nullptr; }
inline CsFuncObj* as_func(const CsRef& r) { return r && r->kind == cs_kind::func ? static_cast<CsFuncObj*>(r.get()) : nullptr; }
inline CsBuiltinObj* as_builtin(const CsRef& r) { return r && r->kind == cs_kind::builtin ? static_cast<CsBuiltinObj*>(r.get()) : nullptr; }
inline CsNativeObj* as_native(const CsRef& r) { return r && r->kind == cs_kind::native_obj ? static_cast<CsNativeObj*>(r.get()) : nullptr; }

bool cs_is_null(const CsRef& r);
bool cs_truthy(const CsRef& r);               // C# truthiness (only bool/null/non-null)
int64_t cs_to_int(const CsRef& r, bool* ok);  // numeric/coercion to int64
double cs_to_float(const CsRef& r, bool* ok);
std::string cs_to_str(interpreter& i, const CsRef& r);   // ToString() semantics
const char* cs_type_name(const CsRef& r);     // "int"/"string"/"List" style name

// dictionary helpers (ordered pairs keyed by any value)
CsRef dict_get(const CsDictObj* d, const CsRef& key);
bool dict_set(CsDictObj* d, CsRef key, CsRef value);
bool dict_del(CsDictObj* d, const CsRef& key);
CsRef dict_get_ci(const CsDictObj* d, const CsRef& key);  // case-insensitive (ctx)
// CsRef conveniences — resolve the dict kind then forward; no-op false/null
// on non-dict refs so call sites can pass `cs_dict()` results directly.
inline bool dict_set(const CsRef& d, CsRef key, CsRef value) {
    if (auto* dd = as_dict(d))
        return dict_set(dd, std::move(key), std::move(value));
    return false;
}
inline CsRef dict_get(const CsRef& d, const CsRef& key) {
    if (auto* dd = as_dict(d))
        return dict_get(dd, key);
    return CsRef{};
}
inline CsRef dict_get_ci(const CsRef& d, const CsRef& key) {
    if (auto* dd = as_dict(d))
        return dict_get_ci(dd, key);
    return CsRef{};
}
bool cs_eq(const CsRef& a, const CsRef& b);

} // namespace sao::plugins::csmini
