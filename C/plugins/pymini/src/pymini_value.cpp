// pymini_value.cpp — scalar factories, truthiness, repr/str, hash/eq/cmp,
// ordered-dict helpers.
#include "pymini_value.h"
#include "pymini_interp.h"   // complete interpreter type (getattr/call0/truthy)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace sao::plugins::pymini {
namespace {

const PyRef& none_ref() {
    static const PyRef v = std::make_shared<PyNoneObj>();
    return v;
}
const PyRef& ellipsis_ref() {
    static const PyRef v = std::make_shared<PyEllipsisObj>();
    return v;
}
const PyRef& true_ref() {
    static const PyRef v = std::make_shared<PyBoolObj>(true);
    return v;
}
const PyRef& false_ref() {
    static const PyRef v = std::make_shared<PyBoolObj>(false);
    return v;
}

std::string repr_string(std::string_view s) {
    // repr('...') with escape handling (closest to Python).
    std::string out = "'";
    for (const char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\x%02x", c & 0xff);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '\'';
    return out;
}

std::string repr_bytes(std::string_view s) {
    std::string out = "b'";
    for (const char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 ||
                static_cast<unsigned char>(c) >= 0x7f) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\x%02x", c & 0xff);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '\'';
    return out;
}

// Format a double like Python's repr(float): shortest round-trip with
// `.0` appended for integral values.
std::string repr_float(double v) {
    if (std::isnan(v))
        return "nan";
    if (std::isinf(v))
        return v > 0 ? "inf" : "-inf";
    if (v == std::floor(v) && std::abs(v) < 1e16) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld.0", static_cast<long long>(v));
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    std::string out = buf;
    if (out.find('.') == std::string::npos && out.find('e') == std::string::npos &&
        out.find('E') == std::string::npos && out.find("inf") == std::string::npos &&
        out.find("nan") == std::string::npos) {
        out += ".0";
    }
    return out;
}

} // namespace

// ── singletons / factories ────────────────────────────────────────────────
PyRef py_none() { return none_ref(); }
PyRef py_ellipsis() { return ellipsis_ref(); }
PyRef py_true() { return true_ref(); }
PyRef py_false() { return false_ref(); }
PyRef py_bool(bool v) { return v ? true_ref() : false_ref(); }
PyRef py_int(int64_t v) { return std::make_shared<PyIntObj>(v); }
PyRef py_float(double v) { return std::make_shared<PyFloatObj>(v); }
PyRef py_str(std::string_view v) { return std::make_shared<PyStrObj>(std::string(v)); }
PyRef py_str(const char* v) { return std::make_shared<PyStrObj>(v == nullptr ? "" : v); }
PyRef py_bytes(std::string v) { return std::make_shared<PyBytesObj>(std::move(v)); }
PyRef py_tuple(std::vector<PyRef> v) { return std::make_shared<PyTupleObj>(std::move(v)); }
PyRef py_list(std::vector<PyRef> v) { return std::make_shared<PyListObj>(std::move(v)); }
PyRef py_set() { return std::make_shared<PySetObj>(); }
PyRef py_dict() { return std::make_shared<PyDictObj>(); }
PyRef py_builtin(std::string name, py_native_fn fn) {
    return std::make_shared<PyBuiltinObj>(std::move(name), std::move(fn));
}

// ── truthiness / unboxing ─────────────────────────────────────────────────
bool py_is_none(const PyRef& r) {
    return !r || r->kind == py_kind::none_;
}

bool py_truthy(const PyRef& r) {
    if (!r)
        return false;
    switch (r->kind) {
    case py_kind::none_:    return false;
    case py_kind::boolean:  return static_cast<PyBoolObj*>(r.get())->v;
    case py_kind::integer:  return static_cast<PyIntObj*>(r.get())->v != 0;
    case py_kind::number:   return static_cast<PyFloatObj*>(r.get())->v != 0.0;
    case py_kind::string:   return !static_cast<PyStrObj*>(r.get())->v.empty();
    case py_kind::bytes_:   return !static_cast<PyBytesObj*>(r.get())->v.empty();
    case py_kind::tuple_:   return !static_cast<PyTupleObj*>(r.get())->v.empty();
    case py_kind::list:     return !static_cast<PyListObj*>(r.get())->v.empty();
    case py_kind::set:
    case py_kind::frozenset: return !static_cast<PySetObj*>(r.get())->items.empty();
    case py_kind::dict:     return !static_cast<PyDictObj*>(r.get())->items.empty();
    case py_kind::range_: {
        const auto* o = static_cast<PyRangeObj*>(r.get());
        if (o->step > 0)
            return o->start < o->stop;
        return o->start > o->stop;
    }
    default:                return true;   // objects truthy unless __bool__ handled by interp
    }
}

bool py_is_int_like(const PyRef& r) {
    return r && (r->kind == py_kind::boolean || r->kind == py_kind::integer);
}
bool py_is_number(const PyRef& r) {
    return r && (r->kind == py_kind::boolean || r->kind == py_kind::integer ||
                 r->kind == py_kind::number);
}
double py_num(const PyRef& r) {
    if (!r)
        return 0.0;
    switch (r->kind) {
    case py_kind::boolean:  return static_cast<PyBoolObj*>(r.get())->v ? 1.0 : 0.0;
    case py_kind::integer:  return static_cast<double>(static_cast<PyIntObj*>(r.get())->v);
    case py_kind::number:   return static_cast<PyFloatObj*>(r.get())->v;
    default:                return 0.0;
    }
}

int64_t py_to_int(const PyRef& r, bool* ok) {
    if (ok)
        *ok = true;
    if (!r) {
        if (ok)
            *ok = false;
        return 0;
    }
    switch (r->kind) {
    case py_kind::boolean:  return static_cast<PyBoolObj*>(r.get())->v ? 1 : 0;
    case py_kind::integer:  return static_cast<PyIntObj*>(r.get())->v;
    case py_kind::number:   return static_cast<int64_t>(static_cast<PyFloatObj*>(r.get())->v);
    case py_kind::string: {
        try {
            const auto& s = static_cast<PyStrObj*>(r.get())->v;
            std::size_t used = 0;
            const long long v = std::stoll(s, &used, 0);
            if (used != s.size()) {
                if (ok)
                    *ok = false;
                return 0;
            }
            return static_cast<int64_t>(v);
        } catch (...) {
            if (ok)
                *ok = false;
            return 0;
        }
    }
    default:
        if (ok)
            *ok = false;
        return 0;
    }
}

double py_to_float(const PyRef& r, bool* ok) {
    if (ok)
        *ok = true;
    if (!r) {
        if (ok)
            *ok = false;
        return 0.0;
    }
    if (py_is_number(r))
        return py_num(r);
    if (r->kind == py_kind::string) {
        try {
            const auto& s = static_cast<PyStrObj*>(r.get())->v;
            std::size_t used = 0;
            const double v = std::stod(s, &used);
            if (used != s.size()) {
                if (ok)
                    *ok = false;
                return 0.0;
            }
            return v;
        } catch (...) {
            if (ok)
                *ok = false;
            return 0.0;
        }
    }
    if (ok)
        *ok = false;
    return 0.0;
}

// ── str / repr ────────────────────────────────────────────────────────────
// Deep repr walks nested containers; instance repr delegates to __repr__
// via the interpreter (interp calls back through py_repr).
std::string py_repr(interpreter& i, const PyRef& r) {
    if (!r)
        return "None";
    switch (r->kind) {
    case py_kind::none_:    return "None";
    case py_kind::ellipsis_: return "Ellipsis";
    case py_kind::boolean:  return static_cast<PyBoolObj*>(r.get())->v ? "True" : "False";
    case py_kind::integer: {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%lld",
                      static_cast<long long>(static_cast<PyIntObj*>(r.get())->v));
        return buf;
    }
    case py_kind::number:   return repr_float(static_cast<PyFloatObj*>(r.get())->v);
    case py_kind::string:   return repr_string(static_cast<PyStrObj*>(r.get())->v);
    case py_kind::bytes_:   return repr_bytes(static_cast<PyBytesObj*>(r.get())->v);
    case py_kind::tuple_: {
        const auto& v = static_cast<PyTupleObj*>(r.get())->v;
        std::string out = "(";
        for (std::size_t k = 0; k < v.size(); ++k) {
            if (k)
                out += ", ";
            out += py_repr(i, v[k]);
        }
        if (v.size() == 1)
            out += ',';
        out += ')';
        return out;
    }
    case py_kind::list: {
        const auto& v = static_cast<PyListObj*>(r.get())->v;
        std::string out = "[";
        for (std::size_t k = 0; k < v.size(); ++k) {
            if (k)
                out += ", ";
            out += py_repr(i, v[k]);
        }
        out += ']';
        return out;
    }
    case py_kind::set:
    case py_kind::frozenset: {
        const auto& v = static_cast<PySetObj*>(r.get())->items;
        if (v.empty())
            return "set()";
        std::string out = "{";
        for (std::size_t k = 0; k < v.size(); ++k) {
            if (k)
                out += ", ";
            out += py_repr(i, v[k]);
        }
        out += '}';
        return out;
    }
    case py_kind::dict: {
        const auto& items = static_cast<PyDictObj*>(r.get())->items;
        std::string out = "{";
        for (std::size_t k = 0; k < items.size(); ++k) {
            if (k)
                out += ", ";
            out += py_repr(i, items[k].first);
            out += ": ";
            out += py_repr(i, items[k].second);
        }
        out += '}';
        return out;
    }
    case py_kind::range_: {
        const auto* o = static_cast<PyRangeObj*>(r.get());
        char buf[80];
        if (o->step == 1)
            std::snprintf(buf, sizeof(buf), "range(%lld, %lld)",
                          (long long)o->start, (long long)o->stop);
        else
            std::snprintf(buf, sizeof(buf), "range(%lld, %lld, %lld)",
                          (long long)o->start, (long long)o->stop, (long long)o->step);
        return buf;
    }
    case py_kind::exception_: {
        const auto* e = static_cast<PyExcObj*>(r.get());
        std::string out = e->type_name + "(";
        const auto* a = as_tuple(e->args);
        if (a != nullptr) {
            for (std::size_t k = 0; k < a->v.size(); ++k) {
                if (k)
                    out += ", ";
                out += py_repr(i, a->v[k]);
            }
        }
        out += ')';
        return out;
    }
    case py_kind::builtin:
        return "<built-in function " + static_cast<PyBuiltinObj*>(r.get())->name + ">";
    case py_kind::func:
        return "<function " + static_cast<PyFuncObj*>(r.get())->name + ">";
    case py_kind::bound_method:
        return "<bound method " + py_repr(i, static_cast<PyBoundMethodObj*>(r.get())->fn) + ">";
    case py_kind::module: {
        const auto* m = static_cast<PyModuleObj*>(r.get());
        return "<module '" + m->name + "'>";
    }
    case py_kind::class_:
        return "<class '" + static_cast<PyClassObj*>(r.get())->name + "'>";
    case py_kind::instance: {
        // user-defined __repr__ wins; default mirrors CPython's layout
        if (PyRef fn = i.getattr(r, "__repr__")) {
            const PyRef v = i.call0(fn, {});
            if (const auto* s = as_str(v))
                return s->v;
        }
        const auto* in = static_cast<PyInstanceObj*>(r.get());
        const auto* c = in && in->klass ? as_class(in->klass) : nullptr;
        char buf[80];
        std::snprintf(buf, sizeof(buf), "<%s object at %p>",
                      c ? c->name.c_str() : "object", r.get());
        return buf;
    }
    case py_kind::slice_: {
        const auto* s = static_cast<PySliceObj*>(r.get());
        return "slice(" + py_repr(i, s->start) + ", " + py_repr(i, s->stop) + ", " +
               py_repr(i, s->step) + ")";
    }
    default: {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "<%s object at %p>", "object", r.get());
        return buf;
    }
    }
}

std::string py_to_str(interpreter& i, const PyRef& r) {
    if (!r)
        return "";
    if (r->kind == py_kind::string)
        return static_cast<PyStrObj*>(r.get())->v;
    if (r->kind == py_kind::boolean)
        return static_cast<PyBoolObj*>(r.get())->v ? "True" : "False";
    if (r->kind == py_kind::none_)
        return "None";
    if (r->kind == py_kind::instance) {
        // str() consults __str__ first, falling back to __repr__
        if (PyRef fn = i.getattr(r, "__str__")) {
            const PyRef v = i.call0(fn, {});
            if (const auto* s = as_str(v))
                return s->v;
        }
    }
    return py_repr(i, r);   // good enough for non-str scalars/containers
}

// ── hash / equality / ordering ────────────────────────────────────────────
int64_t py_hash(const PyRef& r, bool* ok) {
    if (ok)
        *ok = true;
    if (!r) {
        if (ok)
            *ok = false;
        return 0;
    }
    switch (r->kind) {
    case py_kind::none_:    return 0x6e6f6e65;
    case py_kind::ellipsis_: return 0x656c6c6970736973;
    case py_kind::boolean:  return static_cast<PyBoolObj*>(r.get())->v ? 1 : 0;
    case py_kind::integer:  return static_cast<PyIntObj*>(r.get())->v;
    case py_kind::number: {
        const double d = static_cast<PyFloatObj*>(r.get())->v;
        if (d == std::floor(d) && std::abs(d) < 9.0e15)
            return static_cast<int64_t>(d);
        return static_cast<int64_t>(std::hash<double>{}(d) & 0x7fffffffffffffff);
    }
    case py_kind::string:
        return static_cast<int64_t>(
            std::hash<std::string>{}(static_cast<PyStrObj*>(r.get())->v) &
            0x7fffffffffffffff);
    case py_kind::bytes_:
        return static_cast<int64_t>(
            std::hash<std::string>{}(static_cast<PyBytesObj*>(r.get())->v) &
            0x7fffffffffffffff);
    case py_kind::tuple_: {
        int64_t h = 0x345678;
        for (const auto& el : static_cast<PyTupleObj*>(r.get())->v) {
            bool ok2 = false;
            const int64_t eh = py_hash(el, &ok2);
            if (!ok2) {
                if (ok)
                    *ok = false;
                return 0;
            }
            h = (h * 1000003) ^ eh;
        }
        return h;
    }
    default:
        if (ok)
            *ok = false;
        return 0;
    }
}

namespace {
bool seq_eq(interpreter& i, const std::vector<PyRef>& a,
            const std::vector<PyRef>& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t k = 0; k < a.size(); ++k)
        if (!py_eq(i, a[k], b[k]))
            return false;
    return true;
}
bool set_eq(interpreter& i, const PySetObj* a, const PySetObj* b) {
    if (a->items.size() != b->items.size())
        return false;
    for (const auto& el : a->items) {
        bool found = false;
        for (const auto& el2 : b->items) {
            if (py_eq(i, el, el2)) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}
} // namespace

bool py_eq(interpreter& i, const PyRef& a, const PyRef& b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    // numeric cross-type equality (1 == 1.0 == True)
    if (py_is_number(a) && py_is_number(b))
        return py_num(a) == py_num(b);
    // rich-comparison protocol: for instances, try a.__eq__(b) then the
    // reflected b.__eq__(a); a non-None result decides. Otherwise Python's
    // default __eq__ is identity.
    if (a->kind == py_kind::instance || b->kind == py_kind::instance) {
        if (a->kind == py_kind::instance) {
            if (PyRef fn = i.getattr(a, "__eq__")) {
                const PyRef v = i.call1(fn, b, {});
                if (!py_is_none(v))
                    return i.truthy(v);
            }
        }
        if (b->kind == py_kind::instance) {
            if (PyRef fn = i.getattr(b, "__eq__")) {
                const PyRef v = i.call1(fn, a, {});
                if (!py_is_none(v))
                    return i.truthy(v);
            }
        }
        return a.get() == b.get();
    }
    if (a->kind != b->kind) {
        // set/frozenset interoperate
        if ((a->kind == py_kind::set || a->kind == py_kind::frozenset) &&
            (b->kind == py_kind::set || b->kind == py_kind::frozenset))
            return set_eq(i, as_set(a), as_set(b));
        // list vs tuple: NOT equal in Python
        return false;
    }
    switch (a->kind) {
    case py_kind::none_:    return true;
    case py_kind::ellipsis_: return true;
    case py_kind::boolean:  return as_bool(a)->v == as_bool(b)->v;
    case py_kind::integer:  return as_int(a)->v == as_int(b)->v;
    case py_kind::number:   return as_float(a)->v == as_float(b)->v;
    case py_kind::string:   return as_str(a)->v == as_str(b)->v;
    case py_kind::bytes_:   return as_bytes(a)->v == as_bytes(b)->v;
    case py_kind::list:     return seq_eq(i, as_list(a)->v, as_list(b)->v);
    case py_kind::tuple_:   return seq_eq(i, as_tuple(a)->v, as_tuple(b)->v);
    case py_kind::set:
    case py_kind::frozenset: return set_eq(i, as_set(a), as_set(b));
    case py_kind::dict: {
        const auto* da = as_dict(a);
        const auto* db = as_dict(b);
        if (da->items.size() != db->items.size())
            return false;
        for (const auto& [k, v] : da->items) {
            const PyRef ov = dict_get(i, db, k);
            if (!ov || !py_eq(i, v, ov))
                return false;
        }
        return true;
    }
    case py_kind::range_: {
        const auto* ra = static_cast<PyRangeObj*>(a.get());
        const auto* rb = static_cast<PyRangeObj*>(b.get());
        return ra->start == rb->start && ra->stop == rb->stop && ra->step == rb->step;
    }
    default: {
        // identity + __eq__ fallback handled by interp; raw objects → identity.
        return a.get() == b.get();
    }
    }
}

int py_cmp(interpreter& i, const PyRef& a, const PyRef& b, bool* ok) {
    if (ok)
        *ok = true;
    if (!a || !b) {
        if (ok)
            *ok = false;
        return 0;
    }
    if (py_is_number(a) && py_is_number(b)) {
        const double da = py_num(a);
        const double db = py_num(b);
        return da < db ? -1 : (da > db ? 1 : 0);
    }
    if (a->kind == py_kind::string && b->kind == py_kind::string)
        return as_str(a)->v.compare(as_str(b)->v) < 0 ? -1
             : (as_str(a)->v == as_str(b)->v ? 0 : 1);
    if (a->kind == py_kind::bytes_ && b->kind == py_kind::bytes_)
        return as_bytes(a)->v.compare(as_bytes(b)->v) < 0 ? -1
             : (as_bytes(a)->v == as_bytes(b)->v ? 0 : 1);
    if ((a->kind == py_kind::list || a->kind == py_kind::tuple_) &&
        a->kind == b->kind) {
        const auto& va = a->kind == py_kind::list ? as_list(a)->v : as_tuple(a)->v;
        const auto& vb = b->kind == py_kind::list ? as_list(b)->v : as_tuple(b)->v;
        const std::size_t n = std::min(va.size(), vb.size());
        for (std::size_t k = 0; k < n; ++k) {
            if (!py_eq(i, va[k], vb[k])) {
                bool ok2 = true;
                const int c = py_cmp(i, va[k], vb[k], &ok2);
                if (!ok2) {
                    if (ok)
                        *ok = false;
                    return 0;
                }
                return c;
            }
        }
        return va.size() < vb.size() ? -1 : (va.size() > vb.size() ? 1 : 0);
    }
    if (ok)
        *ok = false;
    return 0;
}

// ── ordered dict helpers ──────────────────────────────────────────────────
namespace {
struct dict_key_eq {
    // keys compare by hash-class + value.  When an interpreter is supplied,
    // instance keys (and tuples nested to any depth that contain instances)
    // defer to py_eq so __eq__-overriding classes compare like CPython;
    // interpreter-less paths keep identity semantics for objects.
    static bool same(interpreter* i, const PyRef& a, const PyRef& b) {
        if (a == b)
            return true;
        if (!a || !b)
            return false;
        if (py_is_number(a) && py_is_number(b))
            return py_num(a) == py_num(b);
        if (a->kind == py_kind::instance || b->kind == py_kind::instance)
            return i != nullptr ? py_eq(*i, a, b) : (a.get() == b.get());
        if (a->kind != b->kind)
            return false;
        switch (a->kind) {
        case py_kind::none_:    return true;
        case py_kind::string:   return as_str(a)->v == as_str(b)->v;
        case py_kind::bytes_:   return as_bytes(a)->v == as_bytes(b)->v;
        case py_kind::tuple_: {
            const auto& va = as_tuple(a)->v;
            const auto& vb = as_tuple(b)->v;
            if (va.size() != vb.size())
                return false;
            for (std::size_t k = 0; k < va.size(); ++k)
                if (!same(i, va[k], vb[k]))
                    return false;
            return true;
        }
        default:
            // unreachable for standard hashables; interpreter path keeps
            // py_eq (e.g. frozenset keys) correct, scalar path is identity.
            return i != nullptr ? py_eq(*i, a, b) : (a.get() == b.get());
        }
    }
};
PyRef dict_get_impl(interpreter* i, const PyDictObj* d, const PyRef& key) {
    for (const auto& [k, v] : d->items)
        if (dict_key_eq::same(i, k, key))
            return v;
    return nullptr;
}
} // namespace

PyRef dict_get(const PyDictObj* d, const PyRef& key) {
    if (d == nullptr)
        return nullptr;
    return dict_get_impl(nullptr, d, key);
}
bool dict_set(PyDictObj* d, PyRef key, PyRef value) {
    if (d == nullptr)
        return false;
    for (auto& [k, v] : d->items) {
        if (dict_key_eq::same(nullptr, k, key)) {
            v = std::move(value);
            return true;
        }
    }
    d->items.emplace_back(std::move(key), std::move(value));
    return true;
}
bool dict_del(PyDictObj* d, const PyRef& key) {
    if (d == nullptr)
        return false;
    for (auto it = d->items.begin(); it != d->items.end(); ++it) {
        if (dict_key_eq::same(nullptr, it->first, key)) {
            d->items.erase(it);
            return true;
        }
    }
    return false;
}

// interpreter-aware overloads — script-visible dict paths use these so
// instance keys honor __eq__.
PyRef dict_get(interpreter& i, const PyDictObj* d, const PyRef& key) {
    if (d == nullptr)
        return nullptr;
    return dict_get_impl(&i, d, key);
}
bool dict_set(interpreter& i, PyDictObj* d, PyRef key, PyRef value) {
    if (d == nullptr)
        return false;
    for (auto& [k, v] : d->items) {
        if (dict_key_eq::same(&i, k, key)) {
            v = std::move(value);
            return true;
        }
    }
    d->items.emplace_back(std::move(key), std::move(value));
    return true;
}
bool dict_del(interpreter& i, PyDictObj* d, const PyRef& key) {
    if (d == nullptr)
        return false;
    for (auto it = d->items.begin(); it != d->items.end(); ++it) {
        if (dict_key_eq::same(&i, it->first, key)) {
            d->items.erase(it);
            return true;
        }
    }
    return false;
}
PyRef dict_get(const PyRef& dref, const PyRef& key) {
    return dict_get(as_dict(dref), key);
}
bool dict_set(const PyRef& dref, PyRef key, PyRef value) {
    return dict_set(as_dict(dref), std::move(key), std::move(value));
}
PyRef dict_get(interpreter& i, const PyRef& dref, const PyRef& key) {
    return dict_get(i, as_dict(dref), key);
}
bool dict_set(interpreter& i, const PyRef& dref, PyRef key, PyRef value) {
    return dict_set(i, as_dict(dref), std::move(key), std::move(value));
}

// ── pimpl dtors live in pymini_stdlib2.cpp (impl struct only exists there) ──

} // namespace sao::plugins::pymini
