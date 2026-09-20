// pymini_value.h — pymini object/value model.
//
// Uniform shared_ptr value graph (CPython-style): every value is `PyRef`
// (shared_ptr<PyObj>).  Scalars are heap objects too — None/True/False are
// process singletons.  Cycles (module dict ↔ functions, class ↔ methods) are
// expected; the interpreter breaks them at teardown by clearing module and
// class dicts explicitly.
//
// Builtins signature: PyRef fn(interpreter&, args, kwargs).
#pragma once

#include "pymini_common.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <variant>

namespace sao::plugins::pymini {

class interpreter;
struct PyObj;
using PyRef = std::shared_ptr<PyObj>;

// Positional + keyword args for native callables.
struct py_args {
    std::vector<PyRef> pos;
    std::vector<std::pair<std::string, PyRef>> kw;
    std::size_t size() const noexcept { return pos.size(); }
};

enum class py_kind : uint8_t {
    none_, boolean, integer, number, string, bytes_,
    tuple_, list, set, frozenset, dict,
    slice_, iterator, cell,
    func, bound_method, builtin, staticmethod_, classmethod_, property_,
    class_, instance, super_, module, exception_,
    file_, thread_, lock_, event_, queue_, deque_, counter_,
    module_proxy, range_, map_, filter_, zip_, enumerate_, reversed_,
};

struct PyObj {
    py_kind kind;
    explicit PyObj(py_kind k) : kind(k) {}
    PyObj(const PyObj&) = delete;
    PyObj& operator=(const PyObj&) = delete;
    virtual ~PyObj() = default;
};

// ── scalar objects ────────────────────────────────────────────────────────
struct PyNoneObj : PyObj { PyNoneObj() : PyObj(py_kind::none_) {} };
struct PyBoolObj : PyObj { bool v; explicit PyBoolObj(bool x) : PyObj(py_kind::boolean), v(x) {} };
struct PyIntObj : PyObj { int64_t v; explicit PyIntObj(int64_t x) : PyObj(py_kind::integer), v(x) {} };
struct PyFloatObj : PyObj { double v; explicit PyFloatObj(double x) : PyObj(py_kind::number), v(x) {} };
struct PyStrObj : PyObj { std::string v; explicit PyStrObj(std::string x) : PyObj(py_kind::string), v(std::move(x)) {} };
struct PyBytesObj : PyObj { std::string v; explicit PyBytesObj(std::string x) : PyObj(py_kind::bytes_), v(std::move(x)) {} };
struct PySliceObj : PyObj {
    PyRef start, stop, step;    // any may be PyNone
    PySliceObj(PyRef a, PyRef b, PyRef c)
        : PyObj(py_kind::slice_), start(std::move(a)), stop(std::move(b)), step(std::move(c)) {}
};

// ── containers ────────────────────────────────────────────────────────────
struct PyTupleObj : PyObj {
    std::vector<PyRef> v;
    PyTupleObj() : PyObj(py_kind::tuple_) {}
    explicit PyTupleObj(std::vector<PyRef> x) : PyObj(py_kind::tuple_), v(std::move(x)) {}
};
struct PyListObj : PyObj {
    std::vector<PyRef> v;
    PyListObj() : PyObj(py_kind::list) {}
    explicit PyListObj(std::vector<PyRef> x) : PyObj(py_kind::list), v(std::move(x)) {}
};

// set: insertion-ordered vector + hash index (keys via py_hash/py_eq).
struct PySetObj : PyObj {
    std::vector<PyRef> items;
    PySetObj() : PyObj(py_kind::set) {}
    PySetObj(py_kind k, std::vector<PyRef> x) : PyObj(k), items(std::move(x)) {}
};

// dict: insertion-ordered items (Python 3.7+ semantics — plugins rely on it).
struct PyDictObj : PyObj {
    std::vector<std::pair<PyRef, PyRef>> items;
    PyDictObj() : PyObj(py_kind::dict) {}
};

struct PyIterObj : PyObj {                  // generic sequence/mapping iterator
    PyRef src;                              // sequence/set/dict being iterated
    std::size_t pos = 0;
    bool over_dict_keys = true;             // dict: keys vs items
    PyIterObj(PyRef s, bool keys_only) : PyObj(py_kind::iterator), src(std::move(s)), over_dict_keys(keys_only) {}
};

struct PyRangeObj : PyObj {                 // lazy range
    int64_t start = 0, stop = 0, step = 1;
    PyRangeObj(int64_t a, int64_t b, int64_t c) : PyObj(py_kind::range_), start(a), stop(b), step(c) {}
};

struct PyEnumZipObj : PyObj {               // enumerate/zip/reversed lazy views
    PyRef src;
    int64_t start = 0;
    PyRef second;                           // zip's other iterable (or none)
    PyEnumZipObj(py_kind k, PyRef s, int64_t i) : PyObj(k), src(std::move(s)), start(i) {}
};

// ── callables ─────────────────────────────────────────────────────────────
using py_native_fn = std::function<PyRef(interpreter&, const py_args&)>;

struct PyBuiltinObj : PyObj {
    std::string name;
    py_native_fn fn;
    PyBuiltinObj(std::string n, py_native_fn f)
        : PyObj(py_kind::builtin), name(std::move(n)), fn(std::move(f)) {}
};

struct PyCellObj : PyObj { PyRef v; PyCellObj() : PyObj(py_kind::cell) {} };

struct py_param {
    std::string name;
    PyRef default_value;                    // nullptr → required
    bool varargs = false;                   // *args
    bool kwonly = false;
    bool kwarg = false;                     // **kwargs
};

struct PyFuncObj : PyObj {
    std::string name;
    std::vector<py_param> params;
    std::vector<std::shared_ptr<struct ast_stmt>> body;   // exec'd by interp
    std::vector<PyRef> closure;                           // captured cells
    PyRef globals_dict;                                   // module dict at def time
    PyRef docstring;                                      // str or none
    PyFuncObj() : PyObj(py_kind::func) {}
};

struct PyBoundMethodObj : PyObj {
    PyRef self;
    PyRef fn;                               // func or builtin
    PyBoundMethodObj(PyRef s, PyRef f) : PyObj(py_kind::bound_method), self(std::move(s)), fn(std::move(f)) {}
};

struct PyStaticObj : PyObj { PyRef fn; explicit PyStaticObj(PyRef f) : PyObj(py_kind::staticmethod_), fn(std::move(f)) {} };
struct PyClassMethodObj : PyObj { PyRef fn; explicit PyClassMethodObj(PyRef f) : PyObj(py_kind::classmethod_), fn(std::move(f)) {} };
struct PyPropertyObj : PyObj {
    PyRef fget, fset, fdel;                 // any may be nullptr
    PyPropertyObj(PyRef g, PyRef s, PyRef d) : PyObj(py_kind::property_), fget(std::move(g)), fset(std::move(s)), fdel(std::move(d)) {}
};

// ── class machinery ───────────────────────────────────────────────────────
struct PyClassObj : PyObj {
    std::string name;
    std::vector<PyRef> bases;               // PyClassObj refs
    PyRef attrs;                            // PyDictObj
    std::vector<PyClassObj*> mro;           // cached linearization (raw ptrs OK: bases keep owners alive via attrs/mro parents)
    PyRef docstring;
    PyClassObj() : PyObj(py_kind::class_) {}
};

struct PyInstanceObj : PyObj {
    PyRef klass;                            // PyClassObj (kept as PyRef → no raw lifetime issue)
    PyRef attrs;                            // PyDictObj
    PyInstanceObj(PyRef k, PyRef a) : PyObj(py_kind::instance), klass(std::move(k)), attrs(std::move(a)) {}
};

struct PySuperObj : PyObj {
    PyRef klass;                            // starting class in mro
    PyRef self;                             // bound instance
    PySuperObj(PyRef k, PyRef s) : PyObj(py_kind::super_), klass(std::move(k)), self(std::move(s)) {}
};

// ── modules ───────────────────────────────────────────────────────────────
struct PyModuleObj : PyObj {
    std::string name;                       // dotted name
    PyRef dict;                             // PyDictObj namespace
    std::string path;                       // utf8 file path (or "")
    std::string package;                    // __package__ (or "")
    PyModuleObj() : PyObj(py_kind::module) {}
};

// Cross-language proxy: wraps script_ctx::script_module (lazy binding — the
// concrete payload lives in pymini_bridge.cpp; here it is an opaque
// shared_ptr<void> + call table).
struct PyModuleProxyObj : PyObj {
    std::string name;
    PyRef dict;                             // PyDictObj of function wrappers + data
    PyModuleProxyObj() : PyObj(py_kind::module_proxy) {}
};

// ── exceptions ────────────────────────────────────────────────────────────
struct PyExcObj : PyObj {
    std::string type_name;                  // "ValueError", "PluginError", ...
    PyRef args;                             // PyTupleObj
    PyRef context;                          // __context__ (exc or none)
    std::vector<std::string> trace;         // "file:line in fn" frames
    PyExcObj() : PyObj(py_kind::exception_) {}
};

// ── files / threads / sync ────────────────────────────────────────────────
struct PyFileObj : PyObj {                  // sandboxed file object
    struct impl;
    std::unique_ptr<impl> pimpl;
    std::string mode;
    PyFileObj();
    ~PyFileObj() override;
};

struct PyThreadObj : PyObj {
    struct impl;
    std::unique_ptr<impl> pimpl;
    PyThreadObj();
    ~PyThreadObj() override;
};

struct PyLockObj : PyObj {                  // threading.Lock / RLock
    std::recursive_mutex mutex;
    PyLockObj() : PyObj(py_kind::lock_) {}
};

struct PyEventObj : PyObj {                 // threading.Event
    std::mutex m;
    std::condition_variable cv;
    bool flag = false;
    PyEventObj() : PyObj(py_kind::event_) {}
};

struct PyQueueObj : PyObj {                 // queue.Queue / LifoQueue / PriorityQueue
    std::mutex m;
    std::condition_variable not_empty;
    std::deque<PyRef> items;
    std::size_t maxsize = 0;                // 0 = unbounded
    bool lifo = false;                      // LifoQueue pops from back
    PyQueueObj(bool l) : PyObj(py_kind::queue_), lifo(l) {}
};

struct PyDequeObj : PyObj {                 // collections.deque
    std::deque<PyRef> items;
    std::size_t maxlen = 0;                 // 0 = unbounded
    PyDequeObj() : PyObj(py_kind::deque_) {}
};

struct PyCounterObj : PyObj {               // collections.Counter
    PyRef dict;                             // PyDictObj counts
    explicit PyCounterObj(PyRef d) : PyObj(py_kind::counter_), dict(std::move(d)) {}
};

// ── singletons + factories ────────────────────────────────────────────────
PyRef py_none();
PyRef py_true();
PyRef py_false();
PyRef py_bool(bool v);
PyRef py_int(int64_t v);
PyRef py_float(double v);
PyRef py_str(std::string_view v);
PyRef py_str(const char* v);
PyRef py_bytes(std::string v);
PyRef py_tuple(std::vector<PyRef> v);
PyRef py_list(std::vector<PyRef> v = {});
PyRef py_set();
PyRef py_dict();
PyRef py_builtin(std::string name, py_native_fn fn);

// fast casts (return nullptr when wrong kind)
inline PyStrObj* as_str(const PyRef& r) { return r && r->kind == py_kind::string ? static_cast<PyStrObj*>(r.get()) : nullptr; }
inline PyIntObj* as_int(const PyRef& r) { return r && r->kind == py_kind::integer ? static_cast<PyIntObj*>(r.get()) : nullptr; }
inline PyFloatObj* as_float(const PyRef& r) { return r && r->kind == py_kind::number ? static_cast<PyFloatObj*>(r.get()) : nullptr; }
inline PyBoolObj* as_bool(const PyRef& r) { return r && r->kind == py_kind::boolean ? static_cast<PyBoolObj*>(r.get()) : nullptr; }
inline PyBytesObj* as_bytes(const PyRef& r) { return r && r->kind == py_kind::bytes_ ? static_cast<PyBytesObj*>(r.get()) : nullptr; }
inline PyListObj* as_list(const PyRef& r) { return r && r->kind == py_kind::list ? static_cast<PyListObj*>(r.get()) : nullptr; }
inline PyTupleObj* as_tuple(const PyRef& r) { return r && r->kind == py_kind::tuple_ ? static_cast<PyTupleObj*>(r.get()) : nullptr; }
inline PySetObj* as_set(const PyRef& r) { return r && (r->kind == py_kind::set || r->kind == py_kind::frozenset) ? static_cast<PySetObj*>(r.get()) : nullptr; }
inline PyDictObj* as_dict(const PyRef& r) { return r && r->kind == py_kind::dict ? static_cast<PyDictObj*>(r.get()) : nullptr; }
inline PyExcObj* as_exc(const PyRef& r) { return r && r->kind == py_kind::exception_ ? static_cast<PyExcObj*>(r.get()) : nullptr; }
inline PyModuleObj* as_module(const PyRef& r) { return r && r->kind == py_kind::module ? static_cast<PyModuleObj*>(r.get()) : nullptr; }
inline PyClassObj* as_class(const PyRef& r) { return r && r->kind == py_kind::class_ ? static_cast<PyClassObj*>(r.get()) : nullptr; }
inline PyInstanceObj* as_inst(const PyRef& r) { return r && r->kind == py_kind::instance ? static_cast<PyInstanceObj*>(r.get()) : nullptr; }
inline PyFuncObj* as_func(const PyRef& r) { return r && r->kind == py_kind::func ? static_cast<PyFuncObj*>(r.get()) : nullptr; }
inline PyBuiltinObj* as_builtin(const PyRef& r) { return r && r->kind == py_kind::builtin ? static_cast<PyBuiltinObj*>(r.get()) : nullptr; }
inline PyLockObj* as_lock(const PyRef& r) { return r && r->kind == py_kind::lock_ ? static_cast<PyLockObj*>(r.get()) : nullptr; }
inline PyEventObj* as_event(const PyRef& r) { return r && r->kind == py_kind::event_ ? static_cast<PyEventObj*>(r.get()) : nullptr; }
inline PyQueueObj* as_queue(const PyRef& r) { return r && r->kind == py_kind::queue_ ? static_cast<PyQueueObj*>(r.get()) : nullptr; }
inline PyDequeObj* as_deque(const PyRef& r) { return r && r->kind == py_kind::deque_ ? static_cast<PyDequeObj*>(r.get()) : nullptr; }
inline PyCounterObj* as_counter(const PyRef& r) { return r && r->kind == py_kind::counter_ ? static_cast<PyCounterObj*>(r.get()) : nullptr; }
inline PyFileObj* as_file(const PyRef& r) { return r && r->kind == py_kind::file_ ? static_cast<PyFileObj*>(r.get()) : nullptr; }
inline PyThreadObj* as_thread(const PyRef& r) { return r && r->kind == py_kind::thread_ ? static_cast<PyThreadObj*>(r.get()) : nullptr; }

bool py_is_none(const PyRef& r);
bool py_truthy(const PyRef& r);             // Python truthiness
int64_t py_to_int(const PyRef& r, bool* ok);   // int()/index coersion
double py_to_float(const PyRef& r, bool* ok);
std::string py_to_str(interpreter& i, const PyRef& r);   // str() semantics
std::string py_repr(interpreter& i, const PyRef& r);     // repr() semantics

// dictionary helpers (ordered dict model)
PyRef dict_get(const PyDictObj* d, const PyRef& key);
bool dict_set(PyDictObj* d, PyRef key, PyRef value);
bool dict_del(PyDictObj* d, const PyRef& key);
PyRef dict_get(const PyRef& dref, const PyRef& key);      // convenience on PyRef
bool dict_set(const PyRef& dref, PyRef key, PyRef value);
// interpreter-aware variants: instance keys (and tuples containing them)
// compare via the full __eq__ chain instead of identity.
PyRef dict_get(interpreter& i, const PyDictObj* d, const PyRef& key);
bool dict_set(interpreter& i, PyDictObj* d, PyRef key, PyRef value);
bool dict_del(interpreter& i, PyDictObj* d, const PyRef& key);
PyRef dict_get(interpreter& i, const PyRef& dref, const PyRef& key);
bool dict_set(interpreter& i, const PyRef& dref, PyRef key, PyRef value);

int64_t py_hash(const PyRef& r, bool* ok);  // Python equality hash
bool py_eq(interpreter& i, const PyRef& a, const PyRef& b); // == semantics
int py_cmp(interpreter& i, const PyRef& a, const PyRef& b, bool* ok); // <0/=0/>0, ok=false when unordered

// numeric unboxing with bool<int<float promotion
bool py_is_int_like(const PyRef& r);        // bool or int
bool py_is_number(const PyRef& r);          // bool/int/float
double py_num(const PyRef& r);              // assuming py_is_number

} // namespace sao::plugins::pymini
