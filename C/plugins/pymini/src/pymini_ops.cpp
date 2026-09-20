// pymini_ops.cpp — operators, comparisons, subscription, iteration.
#include "pymini_interp.h"

#include <algorithm>
#include <cmath>

namespace sao::plugins::pymini {
namespace {

const char* op_name(tok_kind op) {
    switch (op) {
    case tok_kind::plus:    return "+";
    case tok_kind::minus:   return "-";
    case tok_kind::star:    return "*";
    case tok_kind::dstar:   return "**";
    case tok_kind::slash:   return "/";
    case tok_kind::dslash:  return "//";
    case tok_kind::percent: return "%";
    case tok_kind::lshift:  return "<<";
    case tok_kind::rshift:  return ">>";
    case tok_kind::amp:     return "&";
    case tok_kind::pipe:    return "|";
    case tok_kind::caret:   return "^";
    case tok_kind::at:      return "@";
    default:                return "?";
    }
}

// format one %-conversion: minimal %s %r %d %i %u %f %g %e %x %X %o %c %% plus
// %(name)s dictionary format.  Flags/width/precision parsed loosely.
std::string py_percent_format(interpreter& i, const std::string& fmt,
                              const PyRef& arg, bool* ok) {
    const PyDictObj* dref = nullptr;
    std::vector<PyRef> flat;
    bool single = true;
    if (auto* d = as_dict(arg)) {
        dref = d;
    } else if (auto* t = as_tuple(arg)) {
        flat = t->v;
        single = false;
    } else {
        flat = {arg};
    }
    std::string out;
    std::size_t used = 0;
    auto next_arg = [&]() -> PyRef {
        if (used < flat.size())
            return flat[used++];
        return nullptr;
    };
    for (std::size_t p = 0; p < fmt.size(); ++p) {
        if (fmt[p] != '%') {
            out += fmt[p];
            continue;
        }
        ++p;
        if (p >= fmt.size()) {
            *ok = false;
            return {};
        }
        if (fmt[p] == '%') {
            out += '%';
            continue;
        }
        PyRef val;
        if (fmt[p] == '(') {
            const auto close = fmt.find(')', p);
            if (close == std::string::npos || dref == nullptr) {
                *ok = false;
                return {};
            }
            const std::string key = fmt.substr(p + 1, close - p - 1);
            val = dict_get(dref, py_str(key));
            p = close + 1;
            if (p >= fmt.size()) {
                *ok = false;
                return {};
            }
        } else {
            // skip flags/width/precision
            while (p < fmt.size() &&
                   (fmt[p] == '-' || fmt[p] == '+' || fmt[p] == ' ' ||
                    fmt[p] == '0' || fmt[p] == '#' ||
                    (fmt[p] >= '1' && fmt[p] <= '9') || fmt[p] == '.'))
                ++p;
        }
        if (p >= fmt.size()) {
            *ok = false;
            return {};
        }
        if (!val)
            val = next_arg();
        if (!val) {
            *ok = false;
            return {};
        }
        const char c = fmt[p];
        switch (c) {
        case 's':
            out += py_to_str(i, val);
            break;
        case 'r':
            out += py_repr(i, val);
            break;
        case 'd':
        case 'i':
        case 'u': {
            bool ok2 = false;
            int64_t n = py_to_int(val, &ok2);
            if (!ok2) {
                *ok = false;
                return {};
            }
            out += std::to_string(n);
            break;
        }
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'e':
        case 'E': {
            bool ok2 = false;
            double x = py_to_float(val, &ok2);
            if (!ok2) {
                *ok = false;
                return {};
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), (c == 'f' || c == 'F')
                              ? "%f" : (c == 'g' || c == 'G') ? "%g" : "%e",
                          x);
            out += buf;
            break;
        }
        case 'x':
        case 'X': {
            bool ok2 = false;
            int64_t n = py_to_int(val, &ok2);
            if (!ok2) {
                *ok = false;
                return {};
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), c == 'x' ? "%llx" : "%llX",
                          static_cast<unsigned long long>(n));
            out += buf;
            break;
        }
        case 'o': {
            bool ok2 = false;
            int64_t n = py_to_int(val, &ok2);
            if (!ok2) {
                *ok = false;
                return {};
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%llo",
                          static_cast<unsigned long long>(n));
            out += buf;
            break;
        }
        case 'c': {
            if (auto* s = as_str(val)) {
                out += s->v;
            } else {
                bool ok2 = false;
                int64_t n = py_to_int(val, &ok2);
                if (!ok2 || n < 0 || n > 0x10FFFF) {
                    *ok = false;
                    return {};
                }
                // encode code point as utf8
                auto cp = static_cast<uint32_t>(n);
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }
            break;
        }
        default:
            *ok = false;
            return {};
        }
    }
    if (!single && used < flat.size()) {
        *ok = false;
        return {};
    }
    *ok = true;
    return out;
}

// floor division / modulo with Python sign rules
PyRef int_floordiv(interpreter& i, int64_t a, int64_t b, src_pos pos) {
    if (b == 0)
        i.raise_exc("ZeroDivisionError", "integer division or modulo by zero",
                    pos);
    if (a == INT64_MIN && b == -1)   // INT64_MIN / -1 is UB (would be 2^63)
        i.raise_exc("OverflowError", "integer division result too large",
                    pos);
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        --q;
    return py_int(q);
}

PyRef int_mod(interpreter& i, int64_t a, int64_t b, src_pos pos) {
    if (b == 0)
        i.raise_exc("ZeroDivisionError", "integer modulo by zero", pos);
    if (a == INT64_MIN && b == -1)   // INT64_MIN % -1 is UB
        i.raise_exc("OverflowError", "integer modulo result too large", pos);
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        r += b;
    return py_int(r);
}

double floor_div_f(interpreter& i, double a, double b, src_pos pos) {
    if (b == 0.0)
        i.raise_exc("ZeroDivisionError", "float floor division", pos);
    return std::floor(a / b);
}

double fmod_f(interpreter& i, double a, double b, src_pos pos) {
    if (b == 0.0)
        i.raise_exc("ZeroDivisionError", "float modulo", pos);
    double r = std::fmod(a, b);
    if (r != 0.0 && ((r < 0.0) != (b < 0.0)))
        r += b;
    return r;
}

// index coercion for subscript/slice positions: int/bool or __index__() —
// strings and floats reaching here are TypeErrors (CPython index semantics),
// even though generic py_to_int() would coerce them.
int64_t index_to_int(interpreter& i, const PyRef& r, const char* what,
                     src_pos pos) {
    if (r && (r->kind == py_kind::integer || r->kind == py_kind::boolean))
        return r->kind == py_kind::integer
                   ? static_cast<PyIntObj*>(r.get())->v
                   : (static_cast<PyBoolObj*>(r.get())->v ? 1 : 0);
    if (r && r->kind == py_kind::instance) {
        if (PyRef fn = i.getattr(r, "__index__")) {
            const PyRef v = i.call0(fn, {});
            if (v && (v->kind == py_kind::integer ||
                      v->kind == py_kind::boolean))
                return v->kind == py_kind::integer
                           ? as_int(v)->v
                           : (as_bool(v)->v ? 1 : 0);
            i.raise_exc("TypeError", "__index__ returned non-int", pos);
        }
    }
    i.raise_exc("TypeError", std::string(what) + " must be integers", pos);
}

// slice normalize: returns (start,stop,step) in absolute coords
void normalize_slice(interpreter& i, const PySliceObj* sl, int64_t len,
                     int64_t* lo, int64_t* hi, int64_t* step, src_pos pos) {
    int64_t st = 1;
    if (!py_is_none(sl->step)) {
        st = index_to_int(i, sl->step, "slice indices", pos);
        if (st == 0)
            i.raise_exc("ValueError", "slice step cannot be zero", pos);
    }
    int64_t a, b;
    if (py_is_none(sl->start))
        a = st > 0 ? 0 : len - 1;
    else {
        a = index_to_int(i, sl->start, "slice indices", pos);
        if (a < 0)
            a += len;
        if (a < 0)
            a = st < 0 ? -1 : 0;
        if (a >= len)
            a = st < 0 ? len - 1 : len;
    }
    if (py_is_none(sl->stop))
        b = st > 0 ? len : -1;
    else {
        b = index_to_int(i, sl->stop, "slice indices", pos);
        if (b < 0)
            b += len;
        if (b < 0)
            b = st < 0 ? -1 : 0;
        if (b >= len)
            b = st < 0 ? len - 1 : len;
    }
    *lo = a;
    *hi = b;
    *step = st;
}

int64_t norm_index(interpreter& i, const PyRef& idx, int64_t len, src_pos pos) {
    int64_t n = index_to_int(i, idx, "indices", pos);
    if (n < 0)
        n += len;
    if (n < 0 || n >= len)
        i.raise_exc("IndexError", "index out of range", pos);
    return n;
}

template <typename Seq, typename Factory>
PyRef slice_seq(interpreter& i, const Seq& src, const PySliceObj* sl,
                Factory&& make, src_pos pos) {
    int64_t lo, hi, st;
    normalize_slice(i, sl, static_cast<int64_t>(src.size()), &lo, &hi, &st, pos);
    if (st > 0) {
        Seq out;
        for (int64_t k = lo; k < hi; k += st)
            out.push_back(src[static_cast<std::size_t>(k)]);
        return make(std::move(out));
    }
    Seq out;
    for (int64_t k = lo; k > hi; k += st)
        out.push_back(src[static_cast<std::size_t>(k)]);
    return make(std::move(out));
}

// dunder dispatch for binary ops on script classes
PyRef try_dunder(interpreter& i, const PyRef& obj, const char* name,
                 const PyRef& other, src_pos pos) {
    if (obj == nullptr || obj->kind != py_kind::instance)
        return nullptr;
    PyRef m = i.getattr(obj, name);
    if (!m)
        return nullptr;
    PyRef r = i.call(m, py_args{{other}}, pos);
    if (r && r->kind == py_kind::none_) {   // NotImplemented sentinel kind reuse
        return nullptr;
    }
    return r;
}

// true when operand b's class is a proper subclass of operand a's class —
// CPython then gives b's reflected rich-comparison/arithmetic first shot.
bool proper_subclass(const PyRef& sub_inst, const PyRef& sup_inst) {
    const auto* si = as_inst(sub_inst);
    const auto* su = as_inst(sup_inst);
    if (!si || !su)
        return false;
    const auto* sc = as_class(si->klass);
    const auto* pc = as_class(su->klass);
    if (!sc || !pc || sc == pc)
        return false;
    for (const auto* mc : sc->mro)
        if (mc == pc)
            return true;
    return false;
}

} // namespace

// ── iteration ─────────────────────────────────────────────────────────────
PyRef interpreter::iter(const PyRef& obj) {
    if (obj == nullptr)
        raise_exc("TypeError", "NoneType object is not iterable", {});
    switch (obj->kind) {
    case py_kind::string:
    case py_kind::bytes_:
    case py_kind::list:
    case py_kind::tuple_:
    case py_kind::set:
    case py_kind::frozenset:
    case py_kind::dict:
    case py_kind::range_:
    case py_kind::enumerate_:
    case py_kind::map_:
    case py_kind::filter_:
    case py_kind::zip_:
    case py_kind::reversed_:
        return std::make_shared<PyIterObj>(obj, true);
    case py_kind::file_: {
        // for line in f — materialize remaining lines (subset semantics)
        PyRef lines = file_lines(obj);
        return iter(lines);
    }
    case py_kind::iterator:
        return obj;
    default: {
        if (obj->kind == py_kind::instance) {
            if (PyRef it_fn = getattr(obj, "__iter__")) {
                PyRef r = call0(it_fn, {});
                if (r)
                    return r;
            }
            // __getitem__ protocol
            if (getattr(obj, "__getitem__"))
                return std::make_shared<PyIterObj>(obj, true);
        }
        raise_exc("TypeError",
                  "'" + std::string(py_type_name(obj)) +
                      "' object is not iterable",
                  {});
    }
    }
}

bool interpreter::iter_next(const PyRef& it, PyRef* out) {
    gil_guard g(*this);
    const PyRef src = it->kind == py_kind::iterator
                          ? static_cast<PyIterObj*>(it.get())->src
                          : it;
    std::size_t* pos_ptr =
        it->kind == py_kind::iterator ? &static_cast<PyIterObj*>(it.get())->pos
                                      : nullptr;
    std::size_t pos_local = 0;
    if (pos_ptr == nullptr)
        pos_ptr = &pos_local;

    switch (src->kind) {
    case py_kind::string: {
        const auto* s = as_str(src);
        // iterate code points (utf8 chunks): give 1-char strings
        const std::string& v = s->v;
        if (*pos_ptr >= v.size())
            return false;
        unsigned char c = static_cast<unsigned char>(v[*pos_ptr]);
        std::size_t n = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        *out = py_str(v.substr(*pos_ptr, n));
        *pos_ptr += n;
        return true;
    }
    case py_kind::bytes_: {
        const auto* b = as_bytes(src);
        if (*pos_ptr >= b->v.size())
            return false;
        *out = py_int(static_cast<unsigned char>(b->v[(*pos_ptr)++]));
        return true;
    }
    case py_kind::list: {
        const auto* l = as_list(src);
        if (*pos_ptr >= l->v.size())
            return false;
        *out = l->v[(*pos_ptr)++];
        return true;
    }
    case py_kind::tuple_: {
        const auto* t = as_tuple(src);
        if (*pos_ptr >= t->v.size())
            return false;
        *out = t->v[(*pos_ptr)++];
        return true;
    }
    case py_kind::set:
    case py_kind::frozenset: {
        const auto* s = as_set(src);
        if (*pos_ptr >= s->items.size())
            return false;
        *out = s->items[(*pos_ptr)++];
        return true;
    }
    case py_kind::dict: {
        const auto* d = as_dict(src);
        const bool keys =
            it->kind == py_kind::iterator
                ? static_cast<PyIterObj*>(it.get())->over_dict_keys
                : true;
        if (*pos_ptr >= d->items.size())
            return false;
        auto& pr = d->items[(*pos_ptr)++];
        *out = keys ? pr.first : py_tuple({pr.first, pr.second});
        return true;
    }
    case py_kind::range_: {
        const auto* r = static_cast<PyRangeObj*>(src.get());
        const int64_t idx = static_cast<int64_t>(*pos_ptr);
        const int64_t val = r->start + idx * r->step;
        if ((r->step > 0 && val >= r->stop) || (r->step < 0 && val <= r->stop))
            return false;
        ++(*pos_ptr);
        *out = py_int(val);
        return true;
    }
    case py_kind::enumerate_: {
        auto* ez = static_cast<PyEnumZipObj*>(src.get());
        // materialize lazily via inner iterator persisted in `second`
        if (py_is_none(ez->second))
            ez->second = iter(ez->src);
        PyRef item;
        if (!iter_next(ez->second, &item))
            return false;
        *out = py_tuple({py_int(ez->start++), item});
        return true;
    }
    case py_kind::zip_: {
        auto* ez = static_cast<PyEnumZipObj*>(src.get());
        // src = list of N iterators — each step pulls one item from every
        // source; the row stops when ANY source is exhausted.
        auto* its = as_list(ez->src);
        if (its == nullptr || its->v.empty())
            return false;
        std::vector<PyRef> row;
        row.reserve(its->v.size());
        for (const auto& sub : its->v) {
            PyRef v;
            if (!iter_next(sub, &v))
                return false;
            row.push_back(std::move(v));
        }
        *out = py_tuple(std::move(row));
        return true;
    }
    case py_kind::map_: {
        auto* ez = static_cast<PyEnumZipObj*>(src.get());
        // src = list [fn, it]; second unused
        auto* pr = as_list(ez->src);
        if (pr == nullptr || pr->v.size() != 2)
            return false;
        PyRef item;
        if (!iter_next(pr->v[1], &item))
            return false;
        *out = call1(pr->v[0], item, {});
        return true;
    }
    case py_kind::filter_: {
        auto* ez = static_cast<PyEnumZipObj*>(src.get());
        auto* pr = as_list(ez->src);
        if (pr == nullptr || pr->v.size() != 2)
            return false;
        PyRef item;
        while (iter_next(pr->v[1], &item)) {
            PyRef r = py_is_none(pr->v[0]) ? item
                                          : call1(pr->v[0], item, {});
            if (truthy(r)) {
                *out = item;
                return true;
            }
        }
        return false;
    }
    case py_kind::reversed_: {
        auto* ez = static_cast<PyEnumZipObj*>(src.get());
        // src is the ALREADY-reversed materialized list from b_reversed —
        // walking `start` forward yields the source back to front. (The old
        // countdown re-seeded start==0 every round → infinite iterator, and
        // read the pre-reversed list backwards → double reversal.)
        auto* l = as_list(ez->src);
        if (l == nullptr || ez->start >= static_cast<int64_t>(l->v.size()))
            return false;
        *out = l->v[static_cast<std::size_t>(ez->start++)];
        return true;
    }
    case py_kind::instance: {
        // __next__ protocol
        if (PyRef next_fn = getattr(src, "__next__")) {
            try {
                *out = call0(next_fn, {});
                return true;
            } catch (const sig_raise& sig) {
                if (exc_matches(sig.exc,
                                dict_get(as_dict(builtins_dict),
                                         py_str("StopIteration"))))
                    return false;
                throw;
            }
        }
        // __getitem__ index protocol
        if (getattr(src, "__getitem__")) {
            try {
                *out = subscript_get(src, py_int(static_cast<int64_t>(*pos_ptr)),
                                     {});
                ++(*pos_ptr);
                return true;
            } catch (const sig_raise& sig) {
                if (exc_matches(sig.exc,
                                dict_get(as_dict(builtins_dict),
                                         py_str("IndexError"))))
                    return false;
                throw;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

void interpreter::for_each(const PyRef& iterable,
                           const std::function<bool(PyRef)>& visitor) {
    PyRef it = iter(iterable);
    PyRef item;
    while (iter_next(it, &item)) {
        if (!visitor(item))
            return;
    }
}

// truthiness with instance __bool__/__len__ consulted (CPython data model);
// every builtin kind defers to the scalar rules in py_truthy.
bool interpreter::truthy(const PyRef& r) {
    if (r && r->kind == py_kind::instance) {
        if (PyRef fn = getattr(r, "__bool__"))
            return py_truthy(call0(fn, {}));
        if (PyRef fn = getattr(r, "__len__")) {
            const PyRef v = call0(fn, {});
            bool ok = false;
            const int64_t n = py_to_int(v, &ok);
            if (!ok)
                raise_exc("TypeError", "__len__ should return an integer", {});
            return n != 0;
        }
    }
    return py_truthy(r);
}

// ── unary ─────────────────────────────────────────────────────────────────
PyRef interpreter::unary(tok_kind op, const PyRef& a, src_pos pos) {
    switch (op) {
    case tok_kind::plus:
        if (py_is_number(a))
            return a;
        if (a->kind == py_kind::instance) {
            if (PyRef r = try_dunder(*this, a, "__pos__", py_none(), pos))
                return r;
        }
        break;
    case tok_kind::minus:
        if (a->kind == py_kind::boolean || a->kind == py_kind::integer) {
            bool ok = false;
            const int64_t n = py_to_int(a, &ok);
            return py_int(-n);
        }
        if (a->kind == py_kind::number)
            return py_float(-as_float(a)->v);
        if (a->kind == py_kind::instance) {
            if (PyRef r = try_dunder(*this, a, "__neg__", py_none(), pos))
                return r;
        }
        break;
    case tok_kind::tilde:
        if (py_is_int_like(a)) {
            bool ok = false;
            int64_t n = py_to_int(a, &ok);
            return py_int(~n);
        }
        if (a->kind == py_kind::instance) {
            if (PyRef r = try_dunder(*this, a, "__invert__", py_none(), pos))
                return r;
        }
        break;
    default:
        break;
    }
    raise_exc("TypeError",
              "bad operand type for unary '" + std::string(op_name(op)) +
                  "': '" + py_type_name(a) + "'",
              pos);
}

// ── contains ──────────────────────────────────────────────────────────────
bool interpreter::contains(const PyRef& item, const PyRef& container) {
    switch (container->kind) {
    case py_kind::dict: {
        const auto* d = as_dict(container);
        for (const auto& [k, v] : d->items)
            if (py_eq(*this, k, item))
                return true;
        return false;
    }
    case py_kind::set:
    case py_kind::frozenset: {
        const auto* s = as_set(container);
        for (const auto& v : s->items)
            if (py_eq(*this, v, item))
                return true;
        return false;
    }
    case py_kind::string: {
        const auto* s = as_str(container);
        const auto* sub = as_str(item);
        if (sub == nullptr)
            raise_exc("TypeError", "'in <string>' requires string operand", {});
        return s->v.find(sub->v) != std::string::npos;
    }
    case py_kind::bytes_: {
        const auto* b = as_bytes(container);
        if (const auto* sub = as_bytes(item))
            return b->v.find(sub->v) != std::string::npos;
        bool ok = false;
        const int64_t n = py_to_int(item, &ok);
        if (ok)
            return b->v.find(static_cast<char>(n)) != std::string::npos;
        raise_exc("TypeError", "'in <bytes>' requires bytes or int", {});
    }
    case py_kind::list:
    case py_kind::tuple_: {
        PyRef it2 = iter(container);
        PyRef v;
        while (iter_next(it2, &v))
            if (py_eq(*this, v, item))
                return true;
        return false;
    }
    case py_kind::range_: {
        if (!py_is_int_like(item))
            return false;
        bool ok = false;
        const int64_t n = py_to_int(item, &ok);
        const auto* r = static_cast<PyRangeObj*>(container.get());
        if (r->step > 0) {
            return n >= r->start && n < r->stop &&
                   (n - r->start) % r->step == 0;
        }
        return n <= r->start && n > r->stop &&
               (r->start - n) % (-r->step) == 0;
    }
    case py_kind::instance: {
        if (PyRef fn = getattr(container, "__contains__"))
            return truthy(call(fn, py_args{{item}}, {}));
        // fallback: iterate
        PyRef it2 = iter(container);
        PyRef v;
        while (iter_next(it2, &v))
            if (py_eq(*this, v, item))
                return true;
        return false;
    }
    default:
        raise_exc("TypeError",
                  "argument of type '" + std::string(py_type_name(container)) +
                      "' is not iterable",
                  {});
    }
}

// ── binary ops ────────────────────────────────────────────────────────────
PyRef interpreter::binary(tok_kind op, const PyRef& a, const PyRef& b,
                          src_pos pos) {
    gil_guard g(*this);

    // instance dunder first (script classes): a.__op__ then b.__rop__ — the
    // reflected arm also fires when the LHS is a builtin type whose native
    // arm cannot match (`3 * inst` → inst.__rmul__, like CPython).
    if ((a && a->kind == py_kind::instance) ||
        (b && b->kind == py_kind::instance)) {
        static const std::unordered_map<tok_kind, std::pair<const char*, const char*>> dd = {
            {tok_kind::plus, {"__add__", "__radd__"}},
            {tok_kind::minus, {"__sub__", "__rsub__"}},
            {tok_kind::star, {"__mul__", "__rmul__"}},
            {tok_kind::slash, {"__truediv__", "__rtruediv__"}},
            {tok_kind::dslash, {"__floordiv__", "__rfloordiv__"}},
            {tok_kind::percent, {"__mod__", "__rmod__"}},
            {tok_kind::dstar, {"__pow__", "__rpow__"}},
            {tok_kind::lshift, {"__lshift__", "__rlshift__"}},
            {tok_kind::rshift, {"__rshift__", "__rrshift__"}},
            {tok_kind::amp, {"__and__", "__rand__"}},
            {tok_kind::pipe, {"__or__", "__ror__"}},
            {tok_kind::caret, {"__xor__", "__rxor__"}},
            {tok_kind::at, {"__matmul__", "__rmatmul__"}},
        };
        const auto itd = dd.find(op);
        if (itd != dd.end()) {
            if (a && a->kind == py_kind::instance)
                if (PyRef r = try_dunder(*this, a, itd->second.first, b, pos))
                    return r;
            if (b && b->kind == py_kind::instance)
                if (PyRef r = try_dunder(*this, b, itd->second.second, a, pos))
                    return r;
        }
    }

    const auto ka = a ? a->kind : py_kind::none_;
    const auto kb = b ? b->kind : py_kind::none_;

    // string ops
    if (ka == py_kind::string && kb == py_kind::string && op == tok_kind::plus)
        return py_str(as_str(a)->v + as_str(b)->v);
    if (ka == py_kind::string && op == tok_kind::star && py_is_int_like(b)) {
        bool ok = false;
        int64_t n = py_to_int(b, &ok);
        std::string out;
        for (int64_t k = 0; k < n; ++k)
            out += as_str(a)->v;
        return py_str(out);
    }
    if (kb == py_kind::string && op == tok_kind::star && py_is_int_like(a)) {
        bool ok = false;
        int64_t n = py_to_int(a, &ok);
        std::string out;
        for (int64_t k = 0; k < n; ++k)
            out += as_str(b)->v;
        return py_str(out);
    }
    if (ka == py_kind::string && op == tok_kind::percent) {
        bool ok = false;
        auto out = py_percent_format(*this, as_str(a)->v, b, &ok);
        if (!ok)
            raise_exc("TypeError", "format requires compatible arguments", pos);
        return py_str(out);
    }

    // bytes ops
    if (ka == py_kind::bytes_ && kb == py_kind::bytes_ && op == tok_kind::plus)
        return py_bytes(as_bytes(a)->v + as_bytes(b)->v);
    if (ka == py_kind::bytes_ && op == tok_kind::star && py_is_int_like(b)) {
        bool ok = false;
        int64_t n = py_to_int(b, &ok);
        std::string out;
        for (int64_t k = 0; k < n; ++k)
            out += as_bytes(a)->v;
        return py_bytes(out);
    }
    if (kb == py_kind::bytes_ && op == tok_kind::star && py_is_int_like(a)) {
        bool ok = false;
        int64_t n = py_to_int(a, &ok);
        std::string out;
        for (int64_t k = 0; k < n; ++k)
            out += as_bytes(b)->v;
        return py_bytes(out);
    }

    // list/tuple concat + repeat
    if (op == tok_kind::plus && ka == py_kind::list && kb == py_kind::list) {
        std::vector<PyRef> out = as_list(a)->v;
        const auto& rv = as_list(b)->v;
        out.insert(out.end(), rv.begin(), rv.end());
        return py_list(std::move(out));
    }
    if (op == tok_kind::plus && ka == py_kind::tuple_ && kb == py_kind::tuple_) {
        std::vector<PyRef> out = as_tuple(a)->v;
        const auto& rv = as_tuple(b)->v;
        out.insert(out.end(), rv.begin(), rv.end());
        return py_tuple(std::move(out));
    }
    if (op == tok_kind::star && (ka == py_kind::list || ka == py_kind::tuple_) &&
        py_is_int_like(b)) {
        bool ok = false;
        int64_t n = py_to_int(b, &ok);
        const auto& src = ka == py_kind::list ? as_list(a)->v : as_tuple(a)->v;
        std::vector<PyRef> out;
        for (int64_t k = 0; k < n; ++k)
            out.insert(out.end(), src.begin(), src.end());
        return ka == py_kind::list ? py_list(std::move(out))
                                   : py_tuple(std::move(out));
    }
    if (op == tok_kind::star && kb == py_kind::list && py_is_int_like(a)) {
        bool ok = false;
        int64_t n = py_to_int(a, &ok);
        const auto& src = as_list(b)->v;
        std::vector<PyRef> out;
        for (int64_t k = 0; k < n; ++k)
            out.insert(out.end(), src.begin(), src.end());
        return py_list(std::move(out));
    }
    if (op == tok_kind::star && kb == py_kind::tuple_ && py_is_int_like(a)) {
        bool ok = false;
        int64_t n = py_to_int(a, &ok);
        const auto& src = as_tuple(b)->v;
        std::vector<PyRef> out;
        for (int64_t k = 0; k < n; ++k)
            out.insert(out.end(), src.begin(), src.end());
        return py_tuple(std::move(out));
    }

    // dict merge (py3.9 `|`)
    if (op == tok_kind::pipe && ka == py_kind::dict && kb == py_kind::dict) {
        auto out = py_dict();
        for (auto& [k, v] : as_dict(a)->items)
            dict_set(*this, as_dict(out), k, v);
        for (auto& [k, v] : as_dict(b)->items)
            dict_set(*this, as_dict(out), k, v);
        return out;
    }

    // set algebra
    const bool a_is_set = ka == py_kind::set || ka == py_kind::frozenset;
    const bool b_is_set = kb == py_kind::set || kb == py_kind::frozenset;
    if (a_is_set && b_is_set &&
        (op == tok_kind::pipe || op == tok_kind::amp ||
         op == tok_kind::minus || op == tok_kind::caret)) {
        auto out = std::make_shared<PySetObj>();
        const auto* sa = as_set(a);
        const auto* sb = as_set(b);
        switch (op) {
        case tok_kind::pipe:
            for (const auto& v : sa->items)
                set_add(*this, out->items, v);
            for (const auto& v : sb->items)
                set_add(*this, out->items, v);
            break;
        case tok_kind::amp:
            for (const auto& v : sa->items) {
                bool in = false;
                for (const auto& w : sb->items)
                    if (py_eq(*this, v, w)) {
                        in = true;
                        break;
                    }
                if (in)
                    set_add(*this, out->items, v);
            }
            break;
        case tok_kind::minus:
            for (const auto& v : sa->items) {
                bool in = false;
                for (const auto& w : sb->items)
                    if (py_eq(*this, v, w)) {
                        in = true;
                        break;
                    }
                if (!in)
                    set_add(*this, out->items, v);
            }
            break;
        case tok_kind::caret:
            for (const auto& v : sa->items) {
                bool in = false;
                for (const auto& w : sb->items)
                    if (py_eq(*this, v, w)) {
                        in = true;
                        break;
                    }
                if (!in)
                    set_add(*this, out->items, v);
            }
            for (const auto& v : sb->items) {
                bool in = false;
                for (const auto& w : sa->items)
                    if (py_eq(*this, v, w)) {
                        in = true;
                        break;
                    }
                if (!in)
                    set_add(*this, out->items, v);
            }
            break;
        default:
            break;
        }
        return out;
    }

    // numeric
    if (py_is_number(a) && py_is_number(b)) {
        const bool int_like = py_is_int_like(a) && py_is_int_like(b);
        bool ok1 = false, ok2 = false;
        if (int_like) {
            const int64_t ia = py_to_int(a, &ok1);
            const int64_t ib = py_to_int(b, &ok2);
            switch (op) {
            case tok_kind::plus:    return py_int(ia + ib);
            case tok_kind::minus:   return py_int(ia - ib);
            case tok_kind::star:    return py_int(ia * ib);
            case tok_kind::dslash:  return int_floordiv(*this, ia, ib, pos);
            case tok_kind::percent: return int_mod(*this, ia, ib, pos);
            case tok_kind::lshift:
                if (ib < 0)
                    raise_exc("ValueError", "negative shift count", pos);
                return py_int(ia << (ib > 62 ? 62 : ib));
            case tok_kind::rshift:
                if (ib < 0)
                    raise_exc("ValueError", "negative shift count", pos);
                return py_int(ib > 62 ? (ia < 0 ? -1 : 0) : ia >> ib);
            case tok_kind::amp:     return py_int(ia & ib);
            case tok_kind::pipe:    return py_int(ia | ib);
            case tok_kind::caret:   return py_int(ia ^ ib);
            case tok_kind::slash:
                if (ib == 0)
                    raise_exc("ZeroDivisionError", "division by zero", pos);
                return py_float(static_cast<double>(ia) / ib);
            case tok_kind::dstar:
                if (ib >= 0) {
                    // exponentiation by squaring — stays int for non-negative exp
                    int64_t base = ia, acc = 1, e = ib;
                    while (e > 0) {
                        if (e & 1)
                            acc *= base;
                        base *= base;
                        e >>= 1;
                    }
                    return py_int(acc);
                }
                return py_float(std::pow(static_cast<double>(ia),
                                         static_cast<double>(ib)));
            default: break;
            }
        } else {
            const double xa = py_num(a);
            const double xb = py_num(b);
            switch (op) {
            case tok_kind::plus:    return py_float(xa + xb);
            case tok_kind::minus:   return py_float(xa - xb);
            case tok_kind::star:    return py_float(xa * xb);
            case tok_kind::slash:
                if (xb == 0.0)
                    raise_exc("ZeroDivisionError", "float division by zero", pos);
                return py_float(xa / xb);
            case tok_kind::dslash:  return py_float(floor_div_f(*this, xa, xb, pos));
            case tok_kind::percent: return py_float(fmod_f(*this, xa, xb, pos));
            case tok_kind::dstar:   return py_float(std::pow(xa, xb));
            case tok_kind::lshift:
            case tok_kind::rshift:
            case tok_kind::amp:
            case tok_kind::pipe:
            case tok_kind::caret:
                raise_exc("TypeError",
                          "unsupported operand type(s) for '" +
                              std::string(op_name(op)) + "': 'float'",
                          pos);
            default: break;
            }
        }
    }

    raise_exc("TypeError",
              "unsupported operand type(s) for '" + std::string(op_name(op)) +
                  "': '" + py_type_name(a) + "' and '" + py_type_name(b) + "'",
              pos);
}

// ── compare ───────────────────────────────────────────────────────────────
PyRef interpreter::compare(tok_kind encoded_op, const PyRef& a, const PyRef& b,
                           src_pos pos) {
    switch (encoded_op) {
    case tok_kind::lshift:  // `is`
        return py_bool(a.get() == b.get() ||
                       (py_is_none(a) && py_is_none(b)) ||
                       (a && b && a->kind == py_kind::boolean &&
                        as_bool(a)->v == as_bool(b)->v));
    case tok_kind::rshift:  // `is not`
        return py_bool(!(a.get() == b.get() ||
                        (py_is_none(a) && py_is_none(b)) ||
                        (a && b && a->kind == py_kind::boolean &&
                         as_bool(a)->v == as_bool(b)->v)));
    case tok_kind::dslash:  // `in`
        return py_bool(contains(a, b));
    case tok_kind::percent: // `not in`
        return py_bool(!contains(a, b));
    case tok_kind::eq:
        return py_bool(py_eq(*this, a, b));
    case tok_kind::ne: {
        // __ne__ protocol: a.__ne__(b), then reflected b.__ne__(a); a
        // NotImplemented/None result falls through to !__eq__.
        if (PyRef r = try_dunder(*this, a, "__ne__", b, pos))
            return py_bool(truthy(r));
        if (PyRef r = try_dunder(*this, b, "__ne__", a, pos))
            return py_bool(truthy(r));
        return py_bool(!py_eq(*this, a, b));
    }
    case tok_kind::lt:
    case tok_kind::le:
    case tok_kind::gt:
    case tok_kind::ge: {
        const char* sym = encoded_op == tok_kind::lt ? "<"
                          : encoded_op == tok_kind::le ? "<="
                          : encoded_op == tok_kind::gt ? ">" : ">=";
        const bool a_inst = a && a->kind == py_kind::instance;
        const bool b_inst = b && b->kind == py_kind::instance;
        if (a_inst || b_inst) {
            // rich comparison: forward dunder on a, reflected (swapped) dunder
            // on b; when type(b) is a proper subclass of type(a) the reflected
            // arm runs first (CPython order).
            const char* fwd = encoded_op == tok_kind::lt ? "__lt__"
                              : encoded_op == tok_kind::le ? "__le__"
                              : encoded_op == tok_kind::gt ? "__gt__"
                                                           : "__ge__";
            const char* rev = encoded_op == tok_kind::lt ? "__gt__"
                              : encoded_op == tok_kind::le ? "__ge__"
                              : encoded_op == tok_kind::gt ? "__lt__"
                                                           : "__le__";
            if (proper_subclass(b, a)) {
                if (PyRef r = try_dunder(*this, b, rev, a, pos))
                    return py_bool(truthy(r));
                if (PyRef r = try_dunder(*this, a, fwd, b, pos))
                    return py_bool(truthy(r));
            } else {
                if (PyRef r = try_dunder(*this, a, fwd, b, pos))
                    return py_bool(truthy(r));
                if (PyRef r = try_dunder(*this, b, rev, a, pos))
                    return py_bool(truthy(r));
            }
            raise_exc("TypeError",
                      "'" + std::string(sym) +
                          "' not supported between instances of '" +
                          py_type_name(a) + "' and '" + py_type_name(b) + "'",
                      pos);
        }
        bool ok = false;
        const int c = py_cmp(*this, a, b, &ok);
        if (!ok)
            raise_exc("TypeError",
                      "'" + std::string(sym) +
                          "' not supported between instances of '" +
                          py_type_name(a) + "' and '" + py_type_name(b) + "'",
                      pos);
        switch (encoded_op) {
        case tok_kind::lt: return py_bool(c < 0);
        case tok_kind::le: return py_bool(c <= 0);
        case tok_kind::gt: return py_bool(c > 0);
        default:           return py_bool(c >= 0);
        }
    }
    default:
        break;
    }
    raise_exc("RuntimeError", "unhandled comparison op", pos);
}

// ── subscript ─────────────────────────────────────────────────────────────
PyRef interpreter::subscript_get(const PyRef& obj, const PyRef& index,
                                 src_pos pos) {
    gil_guard g(*this);
    const auto* sl = obj && index && index->kind == py_kind::slice_
                         ? static_cast<PySliceObj*>(index.get())
                         : nullptr;

    switch (obj ? obj->kind : py_kind::none_) {
    case py_kind::list: {
        const auto* l = as_list(obj);
        if (sl) {
            return slice_seq(*this, l->v, sl,
                             [](std::vector<PyRef> v) {
                                 return py_list(std::move(v));
                             },
                             pos);
        }
        return l->v[static_cast<std::size_t>(
            norm_index(*this, index, static_cast<int64_t>(l->v.size()), pos))];
    }
    case py_kind::tuple_: {
        const auto* t = as_tuple(obj);
        if (sl) {
            return slice_seq(*this, t->v, sl,
                             [](std::vector<PyRef> v) {
                                 return py_tuple(std::move(v));
                             },
                             pos);
        }
        return t->v[static_cast<std::size_t>(
            norm_index(*this, index, static_cast<int64_t>(t->v.size()), pos))];
    }
    case py_kind::string: {
        const auto* s = as_str(obj);
        if (sl) {
            // unicode-naive: operate on utf8 bytes (acceptable subset)
            return slice_seq(*this, s->v, sl,
                             [](std::string v) { return py_str(v); }, pos);
        }
        const int64_t n =
            norm_index(*this, index, static_cast<int64_t>(s->v.size()), pos);
        unsigned char c = static_cast<unsigned char>(s->v[static_cast<std::size_t>(n)]);
        // emit the full utf8 sequence for non-ascii leads
        std::size_t w = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (n + static_cast<int64_t>(w) > static_cast<int64_t>(s->v.size()))
            w = 1;
        return py_str(s->v.substr(static_cast<std::size_t>(n), w));
    }
    case py_kind::bytes_: {
        const auto* b = as_bytes(obj);
        if (sl) {
            return slice_seq(*this, b->v, sl,
                             [](std::string v) { return py_bytes(v); }, pos);
        }
        const int64_t n =
            norm_index(*this, index, static_cast<int64_t>(b->v.size()), pos);
        return py_int(static_cast<unsigned char>(b->v[static_cast<std::size_t>(n)]));
    }
    case py_kind::dict: {
        const auto* d = as_dict(obj);
        if (PyRef v = dict_get(*this, d, index))
            return v;
        if (PyRef miss = getattr(obj, "__missing__")) {
            return call1(miss, index, pos);
        }
        raise_exc("KeyError", py_repr(*this, index), pos);
    }
    case py_kind::range_: {
        const auto* r = static_cast<PyRangeObj*>(obj.get());
        const int64_t len =
            r->step > 0 ? (r->stop - r->start + r->step - 1) / r->step
                        : (r->start - r->stop - r->step - 1) / (-r->step);
        const int64_t clamped = len < 0 ? 0 : len;
        const int64_t n = norm_index(*this, index, clamped, pos);
        return py_int(r->start + n * r->step);
    }
    case py_kind::instance: {
        if (PyRef fn = getattr(obj, "__getitem__"))
            return call(fn, py_args{{index}}, pos);
        break;
    }
    case py_kind::class_: {
        // GenericAlias-ish: list[int] → return subscript spec tuple-ish
        // model as tuple (origin,args) wrapped
        if (!sl)
            return py_tuple({obj, index});
        break;
    }
    default:
        break;
    }
    raise_exc("TypeError",
              "'" + std::string(obj ? py_type_name(obj) : "NoneType") +
                  "' object is not subscriptable",
              pos);
}

void interpreter::subscript_set(const PyRef& obj, const PyRef& index,
                                PyRef value, src_pos pos) {
    gil_guard g(*this);
    const auto* sl = index && index->kind == py_kind::slice_
                         ? static_cast<PySliceObj*>(index.get())
                         : nullptr;
    switch (obj ? obj->kind : py_kind::none_) {
    case py_kind::list: {
        auto* l = as_list(obj);
        if (sl) {
            int64_t lo, hi, st;
            normalize_slice(*this, sl, static_cast<int64_t>(l->v.size()), &lo,
                            &hi, &st, pos);
            std::vector<PyRef> repl;
            for_each(value, [&](PyRef v) {
                repl.push_back(std::move(v));
                return true;
            });
            if (st == 1) {
                // contiguous replace — lo>hi is a well-defined empty slice
                // (`l[3:1]=x` inserts at lo); erasing begin+lo..begin+hi would
                // be UB there.
                auto& vec = l->v;
                if (lo < hi)
                    vec.erase(vec.begin() + lo, vec.begin() + hi);
                vec.insert(vec.begin() + lo, repl.begin(), repl.end());
            } else {
                // extended slice: count must match
                int64_t count = 0;
                for (int64_t k = lo; st > 0 ? k < hi : k > hi; k += st)
                    ++count;
                if (count != static_cast<int64_t>(repl.size()))
                    raise_exc("ValueError",
                              "attempt to assign sequence of size " +
                                  std::to_string(repl.size()) +
                                  " to extended slice of size " +
                                  std::to_string(count),
                              pos);
                std::size_t ri = 0;
                for (int64_t k = lo; st > 0 ? k < hi : k > hi; k += st)
                    l->v[static_cast<std::size_t>(k)] = repl[ri++];
            }
            return;
        }
        const int64_t n =
            norm_index(*this, index, static_cast<int64_t>(l->v.size()), pos);
        l->v[static_cast<std::size_t>(n)] = std::move(value);
        return;
    }
    case py_kind::dict: {
        dict_set(*this, as_dict(obj), index, std::move(value));
        return;
    }
    case py_kind::instance: {
        if (PyRef fn = getattr(obj, "__setitem__")) {
            call(fn, py_args{{index, std::move(value)}}, pos);
            return;
        }
        break;
    }
    default:
        break;
    }
    raise_exc("TypeError",
              "'" + std::string(obj ? py_type_name(obj) : "NoneType") +
                  "' object does not support item assignment",
              pos);
}

void interpreter::subscript_del(const PyRef& obj, const PyRef& index,
                                src_pos pos) {
    gil_guard g(*this);
    switch (obj ? obj->kind : py_kind::none_) {
    case py_kind::list: {
        auto* l = as_list(obj);
        const auto* sl = index && index->kind == py_kind::slice_
                             ? static_cast<PySliceObj*>(index.get())
                             : nullptr;
        if (sl) {
            int64_t lo, hi, st;
            normalize_slice(*this, sl, static_cast<int64_t>(l->v.size()), &lo,
                            &hi, &st, pos);
            if (st == 1) {
                // lo>hi (e.g. `del l[3:1]`) is an empty slice — no-op, not UB
                if (lo < hi)
                    l->v.erase(l->v.begin() + lo, l->v.begin() + hi);
            } else {
                // collect indices, erase descending
                std::vector<int64_t> idxs;
                for (int64_t k = lo; st > 0 ? k < hi : k > hi; k += st)
                    idxs.push_back(k);
                std::sort(idxs.rbegin(), idxs.rend());
                for (const int64_t k : idxs)
                    l->v.erase(l->v.begin() + k);
            }
            return;
        }
        const int64_t n =
            norm_index(*this, index, static_cast<int64_t>(l->v.size()), pos);
        l->v.erase(l->v.begin() + n);
        return;
    }
    case py_kind::dict: {
        if (!dict_del(*this, as_dict(obj), index))
            raise_exc("KeyError", py_repr(*this, index), pos);
        return;
    }
    case py_kind::instance: {
        if (PyRef fn = getattr(obj, "__delitem__")) {
            call(fn, py_args{{index}}, pos);
            return;
        }
        break;
    }
    default:
        break;
    }
    raise_exc("TypeError",
              "'" + std::string(obj ? py_type_name(obj) : "NoneType") +
                  "' object does not support item deletion",
              pos);
}

} // namespace sao::plugins::pymini
