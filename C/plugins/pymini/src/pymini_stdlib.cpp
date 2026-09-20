// pymini_stdlib.cpp — stdlib-lite module factories (part 1):
//   json math time re base64 hashlib random struct string textwrap
//   urllib.parse platform shutil errno
//
// Each factory creates a PyModuleObj whose dict carries PyBuiltinObj
// functions.  `import x` resolves via interpreter::stdlib_factories.
#include "pymini_interp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <regex>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

namespace sao::plugins::pymini {
namespace {

PyRef mk_mod(const std::string& name) {
    auto m = std::make_shared<PyModuleObj>();
    m->name = name;
    m->dict = py_dict();
    m->package = name;
    return m;
}

void put_fn(PyRef m, const char* name, py_native_fn fn) {
    dict_set(as_dict(as_module(m)->dict), py_str(name),
             py_builtin(name, std::move(fn)));
}

void put_c(PyRef m, const char* name, PyRef v) {
    dict_set(as_dict(as_module(m)->dict), py_str(name), std::move(v));
}

PyRef arg_at(interpreter& i, const py_args& a, std::size_t n,
             const char* fn) {
    if (n < a.pos.size())
        return a.pos[n];
    i.raise_exc("TypeError",
                std::string(fn) + " missing positional argument", {});
}

// ═══ json ═══
namespace json_impl {

void write_str(std::string& out, const std::string& s, bool ascii) {
    out += '"';
    for (const unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20 || (ascii && c >= 0x80)) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void dump_val(interpreter& i, std::string& out, const PyRef& v,
              bool ascii, const std::string& indent, int depth,
              std::unordered_set<const PyObj*>& seen) {
    if (v == nullptr || v->kind == py_kind::none_) {
        out += "null";
        return;
    }
    switch (v->kind) {
    case py_kind::boolean:
        out += as_bool(v)->v ? "true" : "false";
        return;
    case py_kind::integer:
        out += std::to_string(as_int(v)->v);
        return;
    case py_kind::number: {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", as_float(v)->v);
        out += buf;
        return;
    }
    case py_kind::string:
        write_str(out, as_str(v)->v, ascii);
        return;
    case py_kind::list:
    case py_kind::tuple_: {
        const auto& items =
            v->kind == py_kind::list ? as_list(v)->v : as_tuple(v)->v;
        if (seen.count(v.get())) {
            out += "null";
            return;
        }
        if (!indent.empty())
            seen.insert(v.get());
        out += v->kind == py_kind::list ? '[' : '[';   // tuples serialize as arrays
        for (std::size_t k = 0; k < items.size(); ++k) {
            if (k)
                out += ", ";
            if (!indent.empty()) {
                out += "\n";
                for (int d = 0; d <= depth; ++d)
                    out += indent;
            }
            dump_val(i, out, items[k], ascii, indent, depth + 1, seen);
        }
        if (!indent.empty() && !items.empty()) {
            out += "\n";
            for (int d = 0; d < depth; ++d)
                out += indent;
        }
        out += ']';
        seen.erase(v.get());
        return;
    }
    case py_kind::dict: {
        const auto* d = as_dict(v);
        out += '{';
        bool first = true;
        for (const auto& [k, val] : d->items) {
            if (!first)
                out += ", ";
            first = false;
            if (!indent.empty()) {
                out += "\n";
                for (int x = 0; x <= depth; ++x)
                    out += indent;
            }
            PyRef ks = py_str(py_to_str(i, k));
            write_str(out, as_str(ks)->v, ascii);
            out += ": ";
            dump_val(i, out, val, ascii, indent, depth + 1, seen);
        }
        if (!indent.empty() && !d->items.empty()) {
            out += "\n";
            for (int x = 0; x < depth; ++x)
                out += indent;
        }
        out += '}';
        return;
    }
    default:
        i.raise_exc("TypeError",
                    "object of type '" + std::string(py_type_name(v)) +
                        "' is not JSON serializable",
                    {});
    }
}

struct parser {
    interpreter* i = nullptr;
    const std::string* s = nullptr;
    std::size_t pos = 0;

    [[noreturn]] void fail(const std::string& m) {
        i->raise_exc("ValueError",
                     "Expecting value: line 1 column " +
                         std::to_string(pos + 1) + " (" + m + ")",
                     {});
    }
    void ws() {
        while (pos < s->size() && ((*s)[pos] == ' ' || (*s)[pos] == '\t' ||
                                   (*s)[pos] == '\n' || (*s)[pos] == '\r'))
            ++pos;
    }
    char peek() { return pos < s->size() ? (*s)[pos] : '\0'; }
    char get() {
        if (pos >= s->size())
            fail("unexpected end");
        return (*s)[pos++];
    }
    void expect(char c) {
        if (get() != c)
            fail(std::string("expected '") + c + "'");
    }
    PyRef parse() {
        ws();
        PyRef v = val();
        ws();
        if (pos != s->size())
            fail("extra data");
        return v;
    }
    PyRef val() {
        ws();
        const char c = peek();
        if (c == 'n') {
            if (s->substr(pos, 4) == "null") { pos += 4; return py_none(); }
            fail("invalid literal");
        }
        if (c == 't') {
            if (s->substr(pos, 4) == "true") { pos += 4; return py_true(); }
            fail("invalid literal");
        }
        if (c == 'f') {
            if (s->substr(pos, 5) == "false") { pos += 5; return py_false(); }
            fail("invalid literal");
        }
        if (c == '"')
            return str();
        if (c == '[')
            return arr();
        if (c == '{')
            return obj();
        if (c == '-' || (c >= '0' && c <= '9'))
            return num();
        fail("unexpected character '" + std::string(1, c) + "'");
    }
    PyRef str() {
        expect('"');
        std::string out;
        while (true) {
            const char c = get();
            if (c == '"')
                return py_str(out);
            if (c == '\\') {
                const char e = get();
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    // \uXXXX (+ surrogate pair)
                    auto hex4 = [&]() -> uint32_t {
                        uint32_t v = 0;
                        for (int k = 0; k < 4; ++k) {
                            const char h = get();
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= h - '0';
                            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                            else fail("bad \\u escape");
                        }
                        return v;
                    };
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos + 1 < s->size() && (*s)[pos] == '\\' &&
                            (*s)[pos + 1] == 'u') {
                            pos += 2;
                            const uint32_t lo = hex4();
                            cp = 0x10000 + ((cp - 0xD800) << 10) +
                                 (lo - 0xDC00);
                        }
                    }
                    // encode cp as utf8
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
                    break;
                }
                default:
                    fail("invalid escape");
                }
            } else {
                out += c;
            }
        }
    }
    PyRef arr() {
        expect('[');
        std::vector<PyRef> items;
        ws();
        if (peek() == ']') {
            get();
            return py_list();
        }
        while (true) {
            items.push_back(val());
            ws();
            const char c = get();
            if (c == ']')
                return py_list(std::move(items));
            if (c != ',')
                fail("expected ',' or ']'");
        }
    }
    PyRef obj() {
        expect('{');
        auto d = py_dict();
        ws();
        if (peek() == '}') {
            get();
            return d;
        }
        while (true) {
            ws();
            if (peek() != '"')
                fail("expected string key");
            PyRef k = str();
            ws();
            expect(':');
            PyRef v = val();
            dict_set(as_dict(d), k, v);
            ws();
            const char c = get();
            if (c == '}')
                return d;
            if (c != ',')
                fail("expected ',' or '}'");
        }
    }
    PyRef num() {
        const std::size_t start = pos;
        if (peek() == '-' || peek() == '+')
            get();
        bool flt = false;
        while (pos < s->size()) {
            const char c = (*s)[pos];
            if (c >= '0' && c <= '9') {
                ++pos;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' ||
                       c == '-') {
                if (c == '.' || c == 'e' || c == 'E')
                    flt = true;
                ++pos;
            } else {
                break;
            }
        }
        const std::string t = s->substr(start, pos - start);
        try {
            return flt ? py_float(std::stod(t)) : py_int(std::stoll(t));
        } catch (...) {
            fail("bad number '" + t + "'");
        }
    }
};

PyRef m_loads(interpreter& i, const py_args& a) {
    const auto* s = as_str(arg_at(i, a, 0, "loads"));
    if (!s)
        i.raise_exc("TypeError", "loads() argument must be str", {});
    parser p{&i, &s->v, 0};
    return p.parse();
}
PyRef m_load(interpreter& i, const py_args& a) {
    // accept file-like (has read()) or str
    if (auto* s = as_str(arg_at(i, a, 0, "load")))
        return m_loads(i, a);
    PyRef read = i.getattr(a.pos[0], "read");
    if (read) {
        PyRef txt = i.call0(read, {});
        py_args a2;
        a2.pos.push_back(txt);
        return m_loads(i, a2);
    }
    i.raise_exc("TypeError", "load() expects str or file-like", {});
}
PyRef m_dumps(interpreter& i, const py_args& a) {
    PyRef v = arg_at(i, a, 0, "dumps");
    bool ascii = true;
    std::string indent;
    for (const auto& [k, kw] : a.kw) {
        if (k == "ensure_ascii")
            ascii = i.truthy(kw);
        if (k == "indent")
            if (auto* s = as_str(kw))
                indent = s->v;
            else {
                bool ok = false;
                const int64_t n = py_to_int(kw, &ok);
                if (ok && n > 0)
                    indent.assign(static_cast<std::size_t>(n), ' ');
            }
        if (k == "separators")
            (void)kw;   // fixed ", " / ": " separators in subset
        if (k == "sort_keys")
            (void)kw;
        if (k == "default")
            (void)kw;
    }
    std::string out;
    std::unordered_set<const PyObj*> seen;
    dump_val(i, out, v, ascii, indent, 0, seen);
    return py_str(out);
}
PyRef m_dump(interpreter& i, const py_args& a) {
    py_args a2;
    a2.pos.push_back(a.pos[0]);
    a2.kw = a.kw;
    PyRef txt = m_dumps(i, a2);
    if (a.pos.size() > 1) {
        if (PyRef w = i.getattr(a.pos[1], "write")) {
            i.call1(w, txt, {});
            return py_none();
        }
    }
    return txt;
}

} // namespace json_impl

PyRef mod_json(interpreter& i) {
    PyRef m = mk_mod("json");
    put_fn(m, "loads", json_impl::m_loads);
    put_fn(m, "load", json_impl::m_load);
    put_fn(m, "dumps", json_impl::m_dumps);
    put_fn(m, "dump", json_impl::m_dump);
    return m;
}

// ═══ math ═══
namespace math_impl {
double f1(interpreter& i, const py_args& a, const char* fn) {
    bool ok = false;
    const double v = py_to_float(arg_at(i, a, 0, fn), &ok);
    if (!ok)
        i.raise_exc("TypeError", std::string(fn) + "() expects number", {});
    return v;
}
PyRef unary1(const char* n, double (*f)(double), interpreter& i,
             const py_args& a) {
    return py_float(f(f1(i, a, n)));
}
PyRef m_sin(interpreter& i, const py_args& a) { return unary1("sin", ::sin, i, a); }
PyRef m_cos(interpreter& i, const py_args& a) { return unary1("cos", ::cos, i, a); }
PyRef m_tan(interpreter& i, const py_args& a) { return unary1("tan", ::tan, i, a); }
PyRef m_asin(interpreter& i, const py_args& a) { return unary1("asin", ::asin, i, a); }
PyRef m_acos(interpreter& i, const py_args& a) { return unary1("acos", ::acos, i, a); }
PyRef m_atan(interpreter& i, const py_args& a) { return unary1("atan", ::atan, i, a); }
PyRef m_sinh(interpreter& i, const py_args& a) { return unary1("sinh", ::sinh, i, a); }
PyRef m_cosh(interpreter& i, const py_args& a) { return unary1("cosh", ::cosh, i, a); }
PyRef m_tanh(interpreter& i, const py_args& a) { return unary1("tanh", ::tanh, i, a); }
PyRef m_exp(interpreter& i, const py_args& a) { return unary1("exp", ::exp, i, a); }
PyRef m_sqrt(interpreter& i, const py_args& a) { return unary1("sqrt", ::sqrt, i, a); }
PyRef m_fabs(interpreter& i, const py_args& a) { return unary1("fabs", ::fabs, i, a); }
PyRef m_floor(interpreter& i, const py_args& a) {
    return py_int(static_cast<int64_t>(std::floor(f1(i, a, "floor"))));
}
PyRef m_ceil(interpreter& i, const py_args& a) {
    return py_int(static_cast<int64_t>(std::ceil(f1(i, a, "ceil"))));
}
PyRef m_trunc(interpreter& i, const py_args& a) {
    return py_int(static_cast<int64_t>(std::trunc(f1(i, a, "trunc"))));
}
PyRef m_log(interpreter& i, const py_args& a) {
    const double v = f1(i, a, "log");
    if (a.pos.size() > 1) {
        const double b = f1(i, a, "log");
        return py_float(std::log(v) / std::log(b));
    }
    return py_float(std::log(v));
}
PyRef m_log2(interpreter& i, const py_args& a) {
    return py_float(std::log2(f1(i, a, "log2")));
}
PyRef m_log10(interpreter& i, const py_args& a) {
    return py_float(std::log10(f1(i, a, "log10")));
}
PyRef m_pow(interpreter& i, const py_args& a) {
    return py_float(std::pow(f1(i, a, "pow"), f1(i, a, "pow")));
}
PyRef m_degrees(interpreter& i, const py_args& a) {
    return py_float(f1(i, a, "degrees") * 180.0 / 3.14159265358979323846);
}
PyRef m_radians(interpreter& i, const py_args& a) {
    return py_float(f1(i, a, "radians") * 3.14159265358979323846 / 180.0);
}
PyRef m_fmod(interpreter& i, const py_args& a) {
    return py_float(std::fmod(f1(i, a, "fmod"), f1(i, a, "fmod")));
}
PyRef m_isnan(interpreter& i, const py_args& a) {
    return py_bool(std::isnan(f1(i, a, "isnan")));
}
PyRef m_isinf(interpreter& i, const py_args& a) {
    return py_bool(std::isinf(f1(i, a, "isinf")));
}
PyRef m_isfinite(interpreter& i, const py_args& a) {
    return py_bool(std::isfinite(f1(i, a, "isfinite")));
}
PyRef m_hypot(interpreter& i, const py_args& a) {
    double acc = 0.0;
    for (const auto& p : a.pos) {
        bool ok = false;
        const double v = py_to_float(p, &ok);
        acc += v * v;
    }
    return py_float(std::sqrt(acc));
}
PyRef m_gcd(interpreter& i, const py_args& a) {
    int64_t x = 0;
    for (const auto& p : a.pos) {
        bool ok = false;
        int64_t v = py_to_int(p, &ok);
        if (v == INT64_MIN)
            i.raise_exc("OverflowError",
                        "gcd() argument too large to negate", {});
        if (v < 0)
            v = -v;
        int64_t a2 = x, b2 = v;
        while (b2) {
            const int64_t t = b2;
            b2 = a2 % b2;
            a2 = t;
        }
        x = a2;
    }
    return py_int(x);
}
} // namespace math_impl

PyRef mod_math(interpreter& i) {
    PyRef m = mk_mod("math");
    put_fn(m, "sin", math_impl::m_sin);
    put_fn(m, "cos", math_impl::m_cos);
    put_fn(m, "tan", math_impl::m_tan);
    put_fn(m, "asin", math_impl::m_asin);
    put_fn(m, "acos", math_impl::m_acos);
    put_fn(m, "atan", math_impl::m_atan);
    put_fn(m, "sinh", math_impl::m_sinh);
    put_fn(m, "cosh", math_impl::m_cosh);
    put_fn(m, "tanh", math_impl::m_tanh);
    put_fn(m, "exp", math_impl::m_exp);
    put_fn(m, "sqrt", math_impl::m_sqrt);
    put_fn(m, "fabs", math_impl::m_fabs);
    put_fn(m, "floor", math_impl::m_floor);
    put_fn(m, "ceil", math_impl::m_ceil);
    put_fn(m, "trunc", math_impl::m_trunc);
    put_fn(m, "log", math_impl::m_log);
    put_fn(m, "log2", math_impl::m_log2);
    put_fn(m, "log10", math_impl::m_log10);
    put_fn(m, "pow", math_impl::m_pow);
    put_fn(m, "degrees", math_impl::m_degrees);
    put_fn(m, "radians", math_impl::m_radians);
    put_fn(m, "fmod", math_impl::m_fmod);
    put_fn(m, "isnan", math_impl::m_isnan);
    put_fn(m, "isinf", math_impl::m_isinf);
    put_fn(m, "isfinite", math_impl::m_isfinite);
    put_fn(m, "hypot", math_impl::m_hypot);
    put_fn(m, "gcd", math_impl::m_gcd);
    put_c(m, "pi", py_float(3.14159265358979323846));
    put_c(m, "e", py_float(2.71828182845904523536));
    put_c(m, "tau", py_float(6.28318530717958647692));
    put_c(m, "inf", py_float(INFINITY));
    put_c(m, "nan", py_float(NAN));
    return m;
}

// ═══ time ═══
namespace time_impl {
using clk = std::chrono::steady_clock;
using wall = std::chrono::system_clock;

PyRef m_time(interpreter& i, const py_args& a) {
    (void)i;
    (void)a;
    return py_float(static_cast<double>(
                        wall::now().time_since_epoch().count()) /
                    static_cast<double>(wall::period::den) *
                    static_cast<double>(wall::period::num));
}
PyRef m_monotonic(interpreter& i, const py_args& a) {
    (void)i;
    (void)a;
    return py_float(static_cast<double>(
                        clk::now().time_since_epoch().count()) /
                    static_cast<double>(clk::period::den) *
                    static_cast<double>(clk::period::num));
}
PyRef m_perf_counter(interpreter& i, const py_args& a) {
    return m_monotonic(i, a);
}
PyRef m_sleep(interpreter& i, const py_args& a) {
    bool ok = false;
    const double s = py_to_float(arg_at(i, a, 0, "sleep"), &ok);
    if (s > 0.0) {
        i.gil.unlock();
        struct rg {
            std::recursive_mutex* m;
            ~rg() { m->lock(); }
        } g{&i.gil};
        ::Sleep(static_cast<DWORD>(s * 1000.0));
    }
    return py_none();
}
PyRef m_time_ms(interpreter& i, const py_args& a) {
    (void)a;
    return py_int(static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            wall::now().time_since_epoch())
            .count()));
}
// struct_time-esque: year/mon/mday/hour/min/sec via dict (subset — plugins
// read strftime or fields; a real tm-like object unnecessary).
PyRef m_localtime(interpreter& i, const py_args& a) {
    int64_t t;
    if (a.pos.empty()) {
        t = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                wall::now().time_since_epoch())
                .count());
    } else {
        bool ok = false;
        t = static_cast<int64_t>(py_to_float(a.pos[0], &ok));
    }
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tmv{};
    localtime_s(&tmv, &tt);
    auto d = py_dict();
    auto put = [&](const char* k, int64_t v) {
        dict_set(as_dict(d), py_str(k), py_int(v));
    };
    put("tm_year", tmv.tm_year + 1900);
    put("tm_mon", tmv.tm_mon + 1);
    put("tm_mday", tmv.tm_mday);
    put("tm_hour", tmv.tm_hour);
    put("tm_min", tmv.tm_min);
    put("tm_sec", tmv.tm_sec);
    put("tm_wday", tmv.tm_wday);
    put("tm_yday", tmv.tm_yday + 1);
    put("tm_isdst", tmv.tm_isdst);
    return d;
}
PyRef m_gmtime(interpreter& i, const py_args& a) {
    int64_t t;
    if (a.pos.empty()) {
        t = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                wall::now().time_since_epoch())
                .count());
    } else {
        bool ok = false;
        t = static_cast<int64_t>(py_to_float(a.pos[0], &ok));
    }
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tmv{};
    gmtime_s(&tmv, &tt);
    auto d = py_dict();
    auto put = [&](const char* k, int64_t v) {
        dict_set(as_dict(d), py_str(k), py_int(v));
    };
    put("tm_year", tmv.tm_year + 1900);
    put("tm_mon", tmv.tm_mon + 1);
    put("tm_mday", tmv.tm_mday);
    put("tm_hour", tmv.tm_hour);
    put("tm_min", tmv.tm_min);
    put("tm_sec", tmv.tm_sec);
    put("tm_wday", tmv.tm_wday);
    put("tm_yday", tmv.tm_yday + 1);
    put("tm_isdst", tmv.tm_isdst);
    return d;
}
PyRef m_strftime(interpreter& i, const py_args& a) {
    const auto* fmt = as_str(arg_at(i, a, 0, "strftime"));
    std::tm tmv{};
    if (a.pos.size() > 1) {
        const auto* d = as_dict(a.pos[1]);
        if (d) {
            auto gv = [&](const char* k) -> int {
                if (PyRef v = dict_get(d, py_str(k))) {
                    bool ok = false;
                    return static_cast<int>(py_to_int(v, &ok));
                }
                return 0;
            };
            tmv.tm_year = gv("tm_year") - 1900;
            tmv.tm_mon = gv("tm_mon") - 1;
            tmv.tm_mday = gv("tm_mday");
            tmv.tm_hour = gv("tm_hour");
            tmv.tm_min = gv("tm_min");
            tmv.tm_sec = gv("tm_sec");
            tmv.tm_wday = gv("tm_wday");
            tmv.tm_yday = gv("tm_yday") - 1;
            tmv.tm_isdst = gv("tm_isdst");
        }
    }
    char buf[256];
    const std::size_t n = std::strftime(buf, sizeof(buf), fmt->v.c_str(), &tmv);
    return py_str(std::string(buf, n));
}
} // namespace time_impl

PyRef mod_time(interpreter& i) {
    PyRef m = mk_mod("time");
    put_fn(m, "time", time_impl::m_time);
    put_fn(m, "monotonic", time_impl::m_monotonic);
    put_fn(m, "perf_counter", time_impl::m_perf_counter);
    put_fn(m, "sleep", time_impl::m_sleep);
    put_fn(m, "time_ms", time_impl::m_time_ms);
    put_fn(m, "localtime", time_impl::m_localtime);
    put_fn(m, "gmtime", time_impl::m_gmtime);
    put_fn(m, "strftime", time_impl::m_strftime);
    return m;
}

// ═══ re — lite via std::regex ECMAScript ═══
namespace re_impl {

int flags_of(const py_args& a, std::size_t n) {
    if (n < a.pos.size()) {
        bool ok = false;
        return static_cast<int>(py_to_int(a.pos[n], &ok));
    }
    return 0;
}

std::regex make_re(interpreter& i, const std::string& pat, int flags) {
    try {
        auto opts = std::regex::ECMAScript;
        if (flags & 2)
            opts = std::regex_constants::icase;
        return std::regex(pat, opts);
    } catch (const std::regex_error& e) {
        i.raise_exc("ValueError",
                    std::string("regex error: ") + e.what(), {});
    }
}

PyRef match_to_obj(const std::smatch& m, const std::string& src,
                   PyRef pattern_obj) {
    auto mo = py_dict();
    dict_set(as_dict(mo), py_str("__match__"), py_true());
    dict_set(as_dict(mo), py_str("__matched__"), py_str(m.str()));
    dict_set(as_dict(mo), py_str("re"), pattern_obj);
    // group methods via builtin wrappers
    PyRef self_key = py_str("m");
    // store groups as list
    std::vector<PyRef> groups;
    for (std::size_t g = 1; g < m.size(); ++g)
        groups.push_back(py_str(m[static_cast<int>(g)].str()));
    dict_set(as_dict(mo), py_str("groups_tuple"), py_list(std::move(groups)));
    dict_set(as_dict(mo), py_str("span_start"), py_int(m.position()));
    dict_set(as_dict(mo), py_str("span_end"),
             py_int(m.position() + static_cast<std::ptrdiff_t>(m.length())));
    return mo;
}

PyRef m_search(interpreter& i, const py_args& a) {
    const auto* pat = as_str(arg_at(i, a, 0, "search"));
    const auto* src = as_str(arg_at(i, a, 1, "search"));
    if (!pat || !src)
        i.raise_exc("TypeError", "search() expects str pattern and str", {});
    auto re = make_re(i, pat->v, flags_of(a, 2));
    std::smatch m;
    if (std::regex_search(src->v, m, re))
        return match_to_obj(m, src->v, a.pos[0]);
    return py_none();
}
PyRef m_match(interpreter& i, const py_args& a) {
    const auto* pat = as_str(arg_at(i, a, 0, "match"));
    const auto* src = as_str(arg_at(i, a, 1, "match"));
    if (!pat || !src)
        i.raise_exc("TypeError", "match() expects str pattern and str", {});
    auto re = make_re(i, pat->v, flags_of(a, 2));
    std::smatch m;
    if (std::regex_search(src->v, m, re) && m.position() == 0)
        return match_to_obj(m, src->v, a.pos[0]);
    return py_none();
}
PyRef m_fullmatch(interpreter& i, const py_args& a) {
    const auto* pat = as_str(arg_at(i, a, 0, "fullmatch"));
    const auto* src = as_str(arg_at(i, a, 1, "fullmatch"));
    if (!pat || !src)
        i.raise_exc("TypeError", "fullmatch() expects str pattern and str", {});
    auto re = make_re(i, pat->v, flags_of(a, 2));
    std::smatch m;
    if (std::regex_match(src->v, m, re))
        return match_to_obj(m, src->v, a.pos[0]);
    return py_none();
}
PyRef m_findall(interpreter& i, const py_args& a) {
    const auto* pat = as_str(arg_at(i, a, 0, "findall"));
    const auto* src = as_str(arg_at(i, a, 1, "findall"));
    if (!pat || !src)
        i.raise_exc("TypeError", "findall() expects str pattern and str", {});
    auto re = make_re(i, pat->v, flags_of(a, 2));
    std::vector<PyRef> out;
    for (std::sregex_iterator it(src->v.begin(), src->v.end(), re), end;
         it != end; ++it) {
        if (it->size() > 1 && (*it)[1].matched)
            out.push_back(py_str((*it)[1].str()));
        else
            out.push_back(py_str(it->str()));
    }
    return py_list(std::move(out));
}
PyRef m_finditer(interpreter& i, const py_args& a) {
    return m_findall(i, a);   // subset: list instead of iterator
}
PyRef m_sub(interpreter& i, const py_args& a) {
    const auto* pat = as_str(arg_at(i, a, 0, "sub"));
    const auto* repl = as_str(arg_at(i, a, 1, "sub"));
    const auto* src = arg_at(i, a, 2, "sub") && a.pos.size() > 2
                          ? as_str(a.pos[2])
                          : nullptr;
    if (!pat || !src)
        i.raise_exc("TypeError", "sub() expects pattern,repl,string", {});
    auto re = make_re(i, pat->v, flags_of(a, 3));
    std::string repl_s = repl ? repl->v : std::string{};
    // convert Python \1 → std $1
    {
        std::string out2;
        for (std::size_t k = 0; k < repl_s.size(); ++k) {
            if (repl_s[k] == '\\' && k + 1 < repl_s.size() &&
                repl_s[k + 1] >= '1' && repl_s[k + 1] <= '9') {
                out2 += '$';
                out2 += repl_s[++k];
            } else if (repl_s[k] == '\\' && k + 1 < repl_s.size()) {
                out2 += repl_s[++k];
            } else {
                out2 += repl_s[k];
            }
        }
        repl_s = out2;
    }
    const int64_t count =
        a.pos.size() > 3 ? [&] {
            bool ok = false;
            return py_to_int(a.pos[3], &ok);
        }()
                         : 0;
    // std::regex_replace handles all; count-limit via iterative rewrite
    if (count > 0) {
        std::string out;
        std::size_t pos = 0;
        int64_t done = 0;
        std::smatch m;
        const std::string& s = src->v;
        while (done < count) {
            std::string rest = s.substr(pos);
            if (!std::regex_search(rest, m, re))
                break;
            out += rest.substr(0, static_cast<std::size_t>(m.position()));
            out += m.format(repl_s);
            pos += static_cast<std::size_t>(m.position() + m.length());
            ++done;
        }
        out += s.substr(pos);
        return py_str(out);
    }
    return py_str(std::regex_replace(src->v, re, repl_s));
}
PyRef m_subn(interpreter& i, const py_args& a) {
    PyRef r = m_sub(i, a);
    // count approximation via findall
    py_args a2;
    a2.pos.push_back(a.pos[0]);
    if (a.pos.size() > 2)
        a2.pos.push_back(a.pos[2]);
    PyRef found = m_findall(i, a2);
    const auto* l = as_list(found);
    return py_tuple({r, py_int(l ? static_cast<int64_t>(l->v.size()) : 0)});
}
PyRef m_split(interpreter& i, const py_args& a) {
    const auto* pat = as_str(arg_at(i, a, 0, "split"));
    const auto* src = as_str(arg_at(i, a, 1, "split"));
    if (!pat || !src)
        i.raise_exc("TypeError", "split() expects str pattern and str", {});
    auto re = make_re(i, pat->v, flags_of(a, 2));
    std::vector<PyRef> out;
    std::sregex_token_iterator it(src->v.begin(), src->v.end(), re, -1), end;
    for (; it != end; ++it)
        out.push_back(py_str(it->str()));
    return py_list(std::move(out));
}
PyRef m_compile(interpreter& i, const py_args& a) {
    const auto* pat = as_str(arg_at(i, a, 0, "compile"));
    if (!pat)
        i.raise_exc("TypeError", "compile() expects str", {});
    const int fl = flags_of(a, 1);
    // compile now to surface errors eagerly
    (void)make_re(i, pat->v, fl);
    auto d = py_dict();
    dict_set(as_dict(d), py_str("__re_pattern__"), a.pos[0]);
    dict_set(as_dict(d), py_str("__re_flags__"), py_int(fl));
    // bound methods that re-dispatch to module fns with pattern prefilled
    auto wrap = [&](const char* n, py_native_fn fn) {
        dict_set(as_dict(d), py_str(n),
                 py_builtin(n, [pat_s = pat->v, fl, fn](interpreter& i2,
                                                        const py_args& a2) {
                     py_args a3;
                     a3.pos.push_back(py_str(pat_s));
                     for (const auto& p : a2.pos)
                         a3.pos.push_back(p);
                     a3.pos.push_back(py_int(fl));
                     a3.kw = a2.kw;
                     return fn(i2, a3);
                 }));
    };
    wrap("search", m_search);
    wrap("match", m_match);
    wrap("fullmatch", m_fullmatch);
    wrap("findall", m_findall);
    wrap("finditer", m_finditer);
    wrap("sub", m_sub);
    wrap("split", m_split);
    dict_set(as_dict(d), py_str("pattern"), a.pos[0]);
    dict_set(as_dict(d), py_str("flags"), py_int(fl));
    return d;
}
PyRef m_escape(interpreter& i, const py_args& a) {
    const auto* s = as_str(arg_at(i, a, 0, "escape"));
    std::string out;
    for (const char c : s->v) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            out += c;
        else {
            out += '\\';
            out += c;
        }
    }
    return py_str(out);
}
} // namespace re_impl

PyRef mod_re(interpreter& i) {
    PyRef m = mk_mod("re");
    put_fn(m, "search", re_impl::m_search);
    put_fn(m, "match", re_impl::m_match);
    put_fn(m, "fullmatch", re_impl::m_fullmatch);
    put_fn(m, "findall", re_impl::m_findall);
    put_fn(m, "finditer", re_impl::m_finditer);
    put_fn(m, "sub", re_impl::m_sub);
    put_fn(m, "subn", re_impl::m_subn);
    put_fn(m, "split", re_impl::m_split);
    put_fn(m, "compile", re_impl::m_compile);
    put_fn(m, "escape", re_impl::m_escape);
    put_c(m, "IGNORECASE", py_int(2));
    put_c(m, "I", py_int(2));
    put_c(m, "MULTILINE", py_int(8));
    put_c(m, "M", py_int(8));
    put_c(m, "DOTALL", py_int(16));
    put_c(m, "S", py_int(16));
    put_c(m, "VERBOSE", py_int(64));
    put_c(m, "X", py_int(64));
    put_c(m, "ASCII", py_int(256));
    put_c(m, "A", py_int(256));
    return m;
}

// ═══ base64 ═══
namespace b64_impl {
const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode(const std::string& in) {
    std::string out;
    for (std::size_t k = 0; k < in.size(); k += 3) {
        const uint32_t v =
            (static_cast<unsigned char>(in[k]) << 16) |
            (k + 1 < in.size() ? static_cast<unsigned char>(in[k + 1]) << 8 : 0u) |
            (k + 2 < in.size() ? static_cast<unsigned char>(in[k + 2]) : 0u);
        out += b64[(v >> 18) & 63];
        out += b64[(v >> 12) & 63];
        out += k + 1 < in.size() ? b64[(v >> 6) & 63] : '=';
        out += k + 2 < in.size() ? b64[v & 63] : '=';
    }
    return out;
}
std::string decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
        const int v = val(c);
        if (v < 0)
            continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}
std::string bytes_of(interpreter& i, const PyRef& v, const char* fn) {
    if (auto* s = as_str(v))
        return s->v;
    if (auto* b = as_bytes(v))
        return b->v;
    i.raise_exc("TypeError",
                std::string(fn) + ": expected bytes-like", {});
}
PyRef m_b64encode(interpreter& i, const py_args& a) {
    return py_bytes(encode(bytes_of(i, arg_at(i, a, 0, "b64encode"),
                                   "b64encode")));
}
PyRef m_b64decode(interpreter& i, const py_args& a) {
    return py_bytes(decode(bytes_of(i, arg_at(i, a, 0, "b64decode"),
                                   "b64decode")));
}
PyRef m_urlsafe_b64encode(interpreter& i, const py_args& a) {
    std::string out = encode(bytes_of(i, arg_at(i, a, 0, "urlsafe_b64encode"),
                                      "urlsafe_b64encode"));
    for (auto& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return py_bytes(out);
}
PyRef m_urlsafe_b64decode(interpreter& i, const py_args& a) {
    std::string in = bytes_of(i, arg_at(i, a, 0, "urlsafe_b64decode"),
                              "urlsafe_b64decode");
    for (auto& c : in) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    return py_bytes(decode(in));
}
} // namespace b64_impl

PyRef mod_base64(interpreter& i) {
    PyRef m = mk_mod("base64");
    put_fn(m, "b64encode", b64_impl::m_b64encode);
    put_fn(m, "b64decode", b64_impl::m_b64decode);
    put_fn(m, "urlsafe_b64encode", b64_impl::m_urlsafe_b64encode);
    put_fn(m, "urlsafe_b64decode", b64_impl::m_urlsafe_b64decode);
    put_fn(m, "standard_b64encode", b64_impl::m_b64encode);
    put_fn(m, "standard_b64decode", b64_impl::m_b64decode);
    return m;
}

// ═══ hashlib ═══
namespace hashlib_impl {
// md5/sha1/sha256 via bcrypt? Windows has BCrypt — use it.
struct hash_guard {
    BCRYPT_ALG_HANDLE alg = nullptr;
    ~hash_guard() { if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
};

std::string bcrypt_hash(interpreter& i, const wchar_t* alg_id,
                        const std::string& data) {
    hash_guard g;
    if (BCryptOpenAlgorithmProvider(&g.alg, alg_id, nullptr, 0) != 0)
        i.raise_exc("RuntimeError", "BCryptOpenAlgorithmProvider failed", {});
    DWORD obj_len = 0, res = 0;
    BCryptGetProperty(g.alg, BCRYPT_OBJECT_LENGTH,
                      reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len),
                      &res, 0);
    DWORD hash_len = 0;
    BCryptGetProperty(g.alg, BCRYPT_HASH_LENGTH,
                      reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len),
                      &res, 0);
    std::vector<BYTE> obj(obj_len), out(hash_len);
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(g.alg, &h, obj.data(), obj_len, nullptr, 0, 0) != 0)
        i.raise_exc("RuntimeError", "BCryptCreateHash failed", {});
    struct hg {
        BCRYPT_HASH_HANDLE h;
        ~hg() { BCryptDestroyHash(h); }
    } hg{h};
    BCryptHashData(h, reinterpret_cast<PUCHAR>(
                          const_cast<char*>(data.data())),
                   static_cast<ULONG>(data.size()), 0);
    BCryptFinishHash(h, out.data(), hash_len, 0);
    return std::string(reinterpret_cast<char*>(out.data()), out.size());
}

std::string bytes_of(interpreter& i, const PyRef& v, const char* fn);

PyRef make_hasher(interpreter& i, const std::string& name,
                  const std::wstring& alg, const std::string& data) {
    auto d = py_dict();
    dict_set(as_dict(d), py_str("name"), py_str(name));
    // accumulating buffer shared by update()/digest()/hexdigest() — the
    // previous snapshot froze `digest` at construction and update() was a
    // stateless no-op.
    auto acc = std::make_shared<std::string>(data);
    dict_set(as_dict(d), py_str("digest"),
             py_builtin("digest", [alg, acc](interpreter& i2,
                                           const py_args&) {
                 return py_bytes(bcrypt_hash(i2, alg.c_str(), *acc));
             }));
    dict_set(as_dict(d), py_str("hexdigest"),
             py_builtin("hexdigest", [alg, acc](interpreter& i2,
                                              const py_args&) {
                 const std::string dg = bcrypt_hash(i2, alg.c_str(), *acc);
                 static const char* hx = "0123456789abcdef";
                 std::string out;
                 for (const unsigned char c : dg) {
                     out += hx[c >> 4];
                     out += hx[c & 15];
                 }
                 return py_str(out);
             }));
    dict_set(as_dict(d), py_str("update"),
             py_builtin("update", [acc](interpreter& i2, const py_args& a) {
                 if (!a.pos.empty())
                     *acc += bytes_of(i2, a.pos[0], "update");
                 return py_none();
             }));
    dict_set(as_dict(d), py_str("digest_size"),
             py_int(name == "sha256" ? 32 : name == "sha512" ? 64
                    : name == "sha1" ? 20 : 16));
    return d;
}

std::string bytes_of(interpreter& i, const PyRef& v, const char* fn) {
    if (auto* s = as_str(v))
        return s->v;
    if (auto* b = as_bytes(v))
        return b->v;
    i.raise_exc("TypeError",
                std::string(fn) + ": expected bytes-like", {});
}

PyRef md5(interpreter& i, const py_args& a) {
    return make_hasher(i, "md5", L"MD5",
                       a.pos.empty() ? "" : bytes_of(i, a.pos[0], "md5"));
}
PyRef sha1(interpreter& i, const py_args& a) {
    return make_hasher(i, "sha1", L"SHA1",
                       a.pos.empty() ? "" : bytes_of(i, a.pos[0], "sha1"));
}
PyRef sha256(interpreter& i, const py_args& a) {
    return make_hasher(i, "sha256", L"SHA256",
                       a.pos.empty() ? "" : bytes_of(i, a.pos[0], "sha256"));
}
PyRef sha512(interpreter& i, const py_args& a) {
    return make_hasher(i, "sha512", L"SHA512",
                       a.pos.empty() ? "" : bytes_of(i, a.pos[0], "sha512"));
}
} // namespace hashlib_impl

PyRef mod_hashlib(interpreter& i) {
    PyRef m = mk_mod("hashlib");
    put_fn(m, "md5", hashlib_impl::md5);
    put_fn(m, "sha1", hashlib_impl::sha1);
    put_fn(m, "sha256", hashlib_impl::sha256);
    put_fn(m, "sha512", hashlib_impl::sha512);
    return m;
}

// ═══ random ═══
namespace random_impl {
std::mt19937_64& rng() {
    static std::mt19937_64 g(std::random_device{}());
    return g;
}
PyRef m_random(interpreter& i, const py_args& a) {
    (void)i;
    (void)a;
    return py_float(std::uniform_real_distribution<double>(0.0, 1.0)(rng()));
}
PyRef m_randint(interpreter& i, const py_args& a) {
    bool o1 = false, o2 = false;
    const int64_t lo = py_to_int(arg_at(i, a, 0, "randint"), &o1);
    const int64_t hi = py_to_int(arg_at(i, a, 1, "randint"), &o2);
    return py_int(std::uniform_int_distribution<int64_t>(lo, hi)(rng()));
}
PyRef m_randrange(interpreter& i, const py_args& a) {
    int64_t lo = 0, hi = 0, step = 1;
    if (a.pos.size() == 1) {
        bool ok = false;
        hi = py_to_int(a.pos[0], &ok);
    } else {
        bool ok = false;
        lo = py_to_int(a.pos[0], &ok);
        hi = py_to_int(a.pos.size() > 1 ? a.pos[1] : a.pos[0], &ok);
        if (a.pos.size() > 2)
            step = py_to_int(a.pos[2], &ok);
    }
    const int64_t span = (hi - lo + step - 1) / step;
    if (span <= 0)
        i.raise_exc("ValueError", "empty range for randrange()", {});
    return py_int(lo + step * std::uniform_int_distribution<int64_t>(
                                    0, span - 1)(rng()));
}
PyRef m_uniform(interpreter& i, const py_args& a) {
    bool o1 = false, o2 = false;
    const double lo = py_to_float(arg_at(i, a, 0, "uniform"), &o1);
    const double hi = py_to_float(arg_at(i, a, 1, "uniform"), &o2);
    return py_float(std::uniform_real_distribution<double>(lo, hi)(rng()));
}
PyRef m_choice(interpreter& i, const py_args& a) {
    PyRef seq = arg_at(i, a, 0, "choice");
    std::vector<PyRef> items;
    i.for_each(seq, [&](PyRef v) {
        items.push_back(v);
        return true;
    });
    if (items.empty())
        i.raise_exc("IndexError", "Cannot choose from an empty sequence", {});
    return items[static_cast<std::size_t>(
        std::uniform_int_distribution<int64_t>(
            0, static_cast<int64_t>(items.size()) - 1)(rng()))];
}
PyRef m_choices(interpreter& i, const py_args& a) {
    PyRef seq = arg_at(i, a, 0, "choices");
    int64_t k = 1;
    for (const auto& [key, v] : a.kw)
        if (key == "k") {
            bool ok = false;
            k = py_to_int(v, &ok);
        }
    std::vector<PyRef> items;
    i.for_each(seq, [&](PyRef v) {
        items.push_back(v);
        return true;
    });
    std::vector<PyRef> out;
    for (int64_t n = 0; n < k; ++n)
        out.push_back(items[static_cast<std::size_t>(
            std::uniform_int_distribution<int64_t>(
                0, static_cast<int64_t>(items.size()) - 1)(rng()))]);
    return py_list(std::move(out));
}
PyRef m_shuffle(interpreter& i, const py_args& a) {
    auto* l = as_list(arg_at(i, a, 0, "shuffle"));
    if (!l)
        i.raise_exc("TypeError", "shuffle() expects a list", {});
    std::shuffle(l->v.begin(), l->v.end(), rng());
    return py_none();
}
PyRef m_sample(interpreter& i, const py_args& a) {
    std::vector<PyRef> items;
    i.for_each(arg_at(i, a, 0, "sample"), [&](PyRef v) {
        items.push_back(v);
        return true;
    });
    bool ok = false;
    const int64_t k = py_to_int(arg_at(i, a, 1, "sample"), &ok);
    if (k < 0 || k > static_cast<int64_t>(items.size()))
        i.raise_exc("ValueError",
                    "Sample larger than population or is negative", {});
    std::shuffle(items.begin(), items.end(), rng());
    if (k < static_cast<int64_t>(items.size()))
        items.resize(static_cast<std::size_t>(k));
    return py_list(std::move(items));
}
PyRef m_seed(interpreter& i, const py_args& a) {
    if (a.pos.empty()) {
        rng().seed(std::random_device{}());
        return py_none();
    }
    bool ok = false;
    const int64_t s = py_to_int(a.pos[0], &ok);
    rng().seed(static_cast<uint64_t>(s));
    return py_none();
}
PyRef m_getrandbits(interpreter& i, const py_args& a) {
    bool ok = false;
    const int64_t bits = py_to_int(arg_at(i, a, 0, "getrandbits"), &ok);
    if (bits <= 32)
        return py_int(static_cast<int64_t>(rng()() & ((1ULL << bits) - 1)));
    return py_int(static_cast<int64_t>(rng()()));
}
} // namespace random_impl

PyRef mod_random(interpreter& i) {
    PyRef m = mk_mod("random");
    put_fn(m, "random", random_impl::m_random);
    put_fn(m, "randint", random_impl::m_randint);
    put_fn(m, "randrange", random_impl::m_randrange);
    put_fn(m, "uniform", random_impl::m_uniform);
    put_fn(m, "choice", random_impl::m_choice);
    put_fn(m, "choices", random_impl::m_choices);
    put_fn(m, "shuffle", random_impl::m_shuffle);
    put_fn(m, "sample", random_impl::m_sample);
    put_fn(m, "seed", random_impl::m_seed);
    put_fn(m, "getrandbits", random_impl::m_getrandbits);
    return m;
}

// ═══ struct ═══
namespace struct_impl {
// minimal pack/unpack for fmt chars: < > ! @ = b B h H i I l L q Q f d s p x P ?
struct fmt_item {
    char c;
    int count;
    int size;
};

std::vector<fmt_item> parse_fmt(interpreter& i, const std::string& f,
                              bool* be) {
    std::vector<fmt_item> out;
    *be = false;
    std::size_t k = 0;
    if (k < f.size() && (f[k] == '<' || f[k] == '=' || f[k] == '@'))
        ++k;
    else if (k < f.size() && (f[k] == '>' || f[k] == '!')) {
        *be = true;
        ++k;
    }
    while (k < f.size()) {
        int count = 0;
        while (k < f.size() && std::isdigit(static_cast<unsigned char>(f[k]))) {
            count = count * 10 + (f[k] - '0');
            ++k;
        }
        if (k >= f.size())
            i.raise_exc("ValueError", "bad char in struct format", {});
        const char c = f[k++];
        int size = 0;
        switch (c) {
        case 'x': case 'b': case 'B': case 'c': case '?': size = 1; break;
        case 'h': case 'H': size = 2; break;
        case 'i': case 'I': case 'l': case 'L': case 'f': size = 4; break;
        case 'q': case 'Q': case 'd': case 'P': case 'n': case 'N': size = 8;
            break;
        case 's': case 'p': size = 1; count = count == 0 ? 1 : count; break;
        default:
            i.raise_exc("ValueError",
                        std::string("bad char in struct format: ") + c, {});
        }
        out.push_back({c, count, size});
    }
    return out;
}

std::string bytes_of(interpreter& i, const PyRef& v, const char* fn) {
    if (auto* s = as_str(v))
        return s->v;
    if (auto* b = as_bytes(v))
        return b->v;
    i.raise_exc("TypeError",
                std::string(fn) + ": expected bytes-like", {});
}

void put_u64(std::string& out, uint64_t v, int size, bool be) {
    for (int k = 0; k < size; ++k) {
        const int shift = be ? (size - 1 - k) * 8 : k * 8;
        out += static_cast<char>((v >> shift) & 0xFF);
    }
}
uint64_t get_u64(const std::string& in, std::size_t off, int size, bool be) {
    uint64_t v = 0;
    for (int k = 0; k < size; ++k) {
        const int shift = be ? (size - 1 - k) * 8 : k * 8;
        v |= static_cast<uint64_t>(
                 static_cast<unsigned char>(in[off + k]))
             << shift;
    }
    return v;
}

PyRef m_pack(interpreter& i, const py_args& a) {
    const auto* fmt = as_str(arg_at(i, a, 0, "pack"));
    bool be = false;
    auto items = parse_fmt(i, fmt->v, &be);
    std::string out;
    std::size_t arg_i = 1;
    for (const auto& it : items) {
        const int total = it.c == 's' || it.c == 'p' ? it.count : it.count == 0 ? 1 : it.count;
        for (int rep = 0; rep < (it.c == 's' || it.c == 'p' ? 1 : (it.count == 0 ? 1 : it.count)); ++rep) {
            if (it.c == 'x') {
                for (int p = 0; p < total; ++p)
                    out += '\0';
                continue;
            }
            if (it.c == 's' || it.c == 'p') {
                if (arg_i >= a.pos.size())
                    i.raise_exc("ValueError", "pack: not enough args", {});
                std::string s = bytes_of(i, a.pos[arg_i++], "pack");
                const int n = it.c == 's' ? it.count : 1;
                if (it.c == 'p') {
                    out += static_cast<char>(s.size() > 255 ? 255 : s.size());
                    out += s.substr(0, 255);
                } else {
                    s.resize(static_cast<std::size_t>(n), '\0');
                    out += s;
                }
                continue;
            }
            if (arg_i >= a.pos.size())
                i.raise_exc("ValueError", "pack: not enough args", {});
            PyRef v = a.pos[arg_i++];
            if (it.c == 'f' || it.c == 'd') {
                bool ok = false;
                const double d = py_to_float(v, &ok);
                if (it.c == 'f') {
                    const float f2 = static_cast<float>(d);
                    uint64_t bits = 0;
                    std::memcpy(&bits, &f2, 4);
                    put_u64(out, bits, 4, be);
                } else {
                    uint64_t bits = 0;
                    std::memcpy(&bits, &d, 8);
                    put_u64(out, bits, 8, be);
                }
                continue;
            }
            if (it.c == '?' || it.c == 'c') {
                if (it.c == 'c') {
                    std::string s = bytes_of(i, v, "pack");
                    out += s.empty() ? '\0' : s[0];
                } else {
                    out += i.truthy(v) ? '\x01' : '\0';
                }
                continue;
            }
            bool ok = false;
            const int64_t n = py_to_int(v, &ok);
            if (!ok)
                i.raise_exc("TypeError", "pack: expected int", {});
            put_u64(out, static_cast<uint64_t>(n), it.size, be);
        }
    }
    return py_bytes(out);
}
PyRef m_unpack(interpreter& i, const py_args& a) {
    const auto* fmt = as_str(arg_at(i, a, 0, "unpack"));
    std::string data = bytes_of(i, arg_at(i, a, 1, "unpack"), "unpack");
    bool be = false;
    auto items = parse_fmt(i, fmt->v, &be);
    // the buffer must be exactly calcsize(fmt) bytes — before this the loop
    // indexed data[] unchecked and over-read any short input.
    std::size_t required = 0;
    for (const auto& it : items) {
        if (it.c == 's' || it.c == 'p')
            required += static_cast<std::size_t>(it.count);
        else
            required += static_cast<std::size_t>(it.size) *
                        static_cast<std::size_t>(it.count == 0 ? 1 : it.count);
    }
    if (data.size() != required)
        i.raise_exc("ValueError",
                    "unpack requires a buffer of " +
                        std::to_string(required) + " bytes", {});
    std::vector<PyRef> out;
    std::size_t off = 0;
    for (const auto& it : items) {
        const int reps = (it.c == 's' || it.c == 'p') ? 1 : (it.count == 0 ? 1 : it.count);
        for (int rep = 0; rep < reps; ++rep) {
            switch (it.c) {
            case 'x':
                off += static_cast<std::size_t>(it.count == 0 ? 1
                                                             : it.count);
                continue;
            case 's': {
                out.push_back(py_bytes(
                    data.substr(off, static_cast<std::size_t>(it.count))));
                off += static_cast<std::size_t>(it.count);
                continue;
            }
            case 'p': {
                // pascal string: byte0 = length clamped to count-1, the
                // field always consumes exactly `count` bytes.
                int n = static_cast<unsigned char>(data[off]);
                if (n > it.count - 1)
                    n = it.count - 1;
                ++off;
                out.push_back(py_bytes(data.substr(off, static_cast<std::size_t>(n))));
                off += static_cast<std::size_t>(it.count) - 1;
                continue;
            }
            case 'c': {
                out.push_back(py_bytes(data.substr(off, 1)));
                ++off;
                continue;
            }
            case '?': {
                out.push_back(py_bool(data[off] != 0));
                ++off;
                continue;
            }
            case 'f': {
                const uint64_t bits = get_u64(data, off, 4, be);
                float f2;
                std::memcpy(&f2, &bits, 4);
                out.push_back(py_float(f2));
                off += 4;
                continue;
            }
            case 'd': {
                const uint64_t bits = get_u64(data, off, 8, be);
                double d2;
                std::memcpy(&d2, &bits, 8);
                out.push_back(py_float(d2));
                off += 8;
                continue;
            }
            default: {
                const uint64_t u = get_u64(data, off, it.size, be);
                off += static_cast<std::size_t>(it.size);
                const bool sign = it.c == 'b' || it.c == 'h' || it.c == 'i' ||
                                  it.c == 'l' || it.c == 'q' || it.c == 'n';
                if (sign && (u >> (it.size * 8 - 1))) {
                    const int64_t sv = static_cast<int64_t>(u) -
                                       (1LL << (it.size * 8));
                    out.push_back(py_int(sv));
                } else {
                    out.push_back(py_int(static_cast<int64_t>(u)));
                }
                continue;
            }
            }
        }
    }
    return py_tuple(std::move(out));
}
PyRef m_calcsize(interpreter& i, const py_args& a) {
    const auto* fmt = as_str(arg_at(i, a, 0, "calcsize"));
    bool be = false;
    auto items = parse_fmt(i, fmt->v, &be);
    int64_t n = 0;
    for (const auto& it : items) {
        if (it.c == 's' || it.c == 'p')
            n += it.count;
        else
            n += it.size * (it.count == 0 ? 1 : it.count);
    }
    return py_int(n);
}
} // namespace struct_impl

PyRef mod_struct(interpreter& i) {
    PyRef m = mk_mod("struct");
    put_fn(m, "pack", struct_impl::m_pack);
    put_fn(m, "unpack", struct_impl::m_unpack);
    put_fn(m, "calcsize", struct_impl::m_calcsize);
    return m;
}

// ═══ string consts ═══
PyRef mod_string(interpreter& i) {
    PyRef m = mk_mod("string");
    put_c(m, "ascii_lowercase", py_str("abcdefghijklmnopqrstuvwxyz"));
    put_c(m, "ascii_uppercase", py_str("ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
    put_c(m, "ascii_letters",
          py_str("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"));
    put_c(m, "digits", py_str("0123456789"));
    put_c(m, "hexdigits", py_str("0123456789abcdefABCDEF"));
    put_c(m, "octdigits", py_str("01234567"));
    put_c(m, "punctuation",
          py_str("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"));
    put_c(m, "whitespace", py_str(" \t\n\r\v\f"));
    put_c(m, "printable",
          py_str("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW"
                 "XYZ!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ \t\n\r\v\f"));
    return m;
}

// ═══ textwrap ═══
namespace textwrap_impl {
PyRef m_fill(interpreter& i, const py_args& a) {
    const auto* s = as_str(arg_at(i, a, 0, "fill"));
    int64_t w = 70;
    for (const auto& [k, v] : a.kw)
        if (k == "width") {
            bool ok = false;
            w = py_to_int(v, &ok);
        }
    if (a.pos.size() > 1) {
        bool ok = false;
        w = py_to_int(a.pos[1], &ok);
    }
    std::string out;
    int64_t col = 0;
    std::size_t pos = 0;
    while (pos < s->v.size()) {
        while (pos < s->v.size() &&
               std::isspace(static_cast<unsigned char>(s->v[pos])) &&
               s->v[pos] != '\n')
            ++pos;
        if (pos >= s->v.size())
            break;
        const std::size_t beg = pos;
        while (pos < s->v.size() &&
               !std::isspace(static_cast<unsigned char>(s->v[pos])))
            ++pos;
        const std::size_t len = pos - beg;
        if (col > 0 && col + static_cast<int64_t>(len) > w) {
            out += '\n';
            col = 0;
        } else if (col > 0) {
            out += ' ';
            ++col;
        }
        out += s->v.substr(beg, len);
        col += static_cast<int64_t>(len);
    }
    return py_str(out);
}
PyRef m_wrap(interpreter& i, const py_args& a) {
    PyRef r = m_fill(i, a);
    std::vector<PyRef> out;
    const auto* s = as_str(r);
    std::string cur;
    for (const char c : s->v) {
        if (c == '\n') {
            out.push_back(py_str(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(py_str(cur));
    return py_list(std::move(out));
}
PyRef m_shorten(interpreter& i, const py_args& a) {
    const auto* s = as_str(arg_at(i, a, 0, "shorten"));
    int64_t w = 70;
    for (const auto& [k, v] : a.kw)
        if (k == "width") {
            bool ok = false;
            w = py_to_int(v, &ok);
        }
    if (a.pos.size() > 1) {
        bool ok = false;
        w = py_to_int(a.pos[1], &ok);
    }
    std::string placeholder = " [...]";
    if (static_cast<int64_t>(s->v.size()) <= w)
        return a.pos[0];
    if (w <= static_cast<int64_t>(placeholder.size()))
        return py_str(placeholder.substr(0, static_cast<std::size_t>(w)));
    std::string out = s->v.substr(0, static_cast<std::size_t>(w - placeholder.size()));
    const auto sp = out.rfind(' ');
    if (sp != std::string::npos)
        out = out.substr(0, sp);
    return py_str(out + placeholder);
}
PyRef m_dedent(interpreter& i, const py_args& a) {
    std::string s = as_str(arg_at(i, a, 0, "dedent"))->v;
    // find common leading whitespace of non-empty lines
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const auto nl = s.find('\n', pos);
        lines.push_back(nl == std::string::npos ? s.substr(pos)
                                              : s.substr(pos, nl - pos));
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    std::string margin;
    bool first = true;
    for (const auto& l : lines) {
        if (l.find_first_not_of(" \t") == std::string::npos)
            continue;
        std::string lead;
        for (const char c : l) {
            if (c == ' ' || c == '\t')
                lead += c;
            else
                break;
        }
        if (first) {
            margin = lead;
            first = false;
        } else {
            std::size_t k = 0;
            while (k < margin.size() && k < lead.size() &&
                   margin[k] == lead[k])
                ++k;
            margin.resize(k);
        }
    }
    std::string out;
    for (std::size_t k = 0; k < lines.size(); ++k) {
        const auto& l = lines[k];
        out += l.substr(0, margin.size()) == margin ? l.substr(margin.size())
                                                    : l;
        if (k + 1 < lines.size())
            out += '\n';
    }
    return py_str(out);
}
PyRef m_indent(interpreter& i, const py_args& a) {
    const auto* s = as_str(arg_at(i, a, 0, "indent"));
    const auto* pfx = a.pos.size() > 1 ? as_str(a.pos[1]) : nullptr;
    std::string prefix = pfx ? pfx->v : std::string("    ");
    std::string out;
    bool bol = true;
    for (const char c : s->v) {
        if (bol)
            out += prefix;
        out += c;
        bol = c == '\n';
    }
    return py_str(out);
}
} // namespace textwrap_impl

PyRef mod_textwrap(interpreter& i) {
    PyRef m = mk_mod("textwrap");
    put_fn(m, "fill", textwrap_impl::m_fill);
    put_fn(m, "wrap", textwrap_impl::m_wrap);
    put_fn(m, "shorten", textwrap_impl::m_shorten);
    put_fn(m, "dedent", textwrap_impl::m_dedent);
    put_fn(m, "indent", textwrap_impl::m_indent);
    return m;
}

// ═══ urllib.parse ═══
namespace urlparse_impl {
std::string pct_enc(const std::string& s, const std::string& safe) {
    static const char* hx = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
            safe.find(static_cast<char>(c)) != std::string::npos) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hx[c >> 4];
            out += hx[c & 15];
        }
    }
    return out;
}
std::string pct_dec(const std::string& s) {
    std::string out;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '%' && k + 2 < s.size()) {
            const int h = nib(s[k + 1]), l = nib(s[k + 2]);
            if (h >= 0 && l >= 0) {
                out += static_cast<char>((h << 4) | l);
                k += 2;
                continue;
            }
        }
        if (s[k] == '+') {
            out += ' ';
            continue;
        }
        out += s[k];
    }
    return out;
}
PyRef m_quote(interpreter& i, const py_args& a) {
    const auto* s = as_str(arg_at(i, a, 0, "quote"));
    const std::string safe = a.pos.size() > 1 && as_str(a.pos[1])
                                 ? as_str(a.pos[1])->v
                                 : std::string("/");
    return py_str(pct_enc(s->v, safe));
}
PyRef m_quote_plus(interpreter& i, const py_args& a) {
    const auto* s = as_str(arg_at(i, a, 0, "quote_plus"));
    // percent-encode with space → '+'
    std::string out2;
    for (const unsigned char c : s->v) {
        if (c == ' ') {
            out2 += '+';
        } else if (std::isalnum(c) || c == '-' || c == '_' || c == '.' ||
                   c == '~') {
            out2 += static_cast<char>(c);
        } else {
            static const char* hx = "0123456789ABCDEF";
            out2 += '%';
            out2 += hx[c >> 4];
            out2 += hx[c & 15];
        }
    }
    return py_str(out2);
}
PyRef m_unquote(interpreter& i, const py_args& a) {
    const auto* s = as_str(arg_at(i, a, 0, "unquote"));
    return py_str(pct_dec(s->v));
}
PyRef m_unquote_plus(interpreter& i, const py_args& a) {
    return m_unquote(i, a);
}
PyRef m_urlencode(interpreter& i, const py_args& a) {
    std::string out;
    auto* d = as_dict(arg_at(i, a, 0, "urlencode"));
    bool first = true;
    if (d) {
        for (const auto& [k, v] : d->items) {
            if (!first)
                out += '&';
            first = false;
            out += pct_enc(py_to_str(i, k), "");
            out += '=';
            out += pct_enc(py_to_str(i, v), "");
        }
    }
    return py_str(out);
}
PyRef m_parse_qs(interpreter& i, const py_args& a) {
    const auto* s = as_str(arg_at(i, a, 0, "parse_qs"));
    auto d = py_dict();
    std::size_t pos = 0;
    while (pos < s->v.size()) {
        const auto amp = s->v.find('&', pos);
        const std::string pair =
            s->v.substr(pos, amp == std::string::npos
                                  ? std::string::npos
                                  : amp - pos);
        const auto eq = pair.find('=');
        const std::string k = pct_dec(pair.substr(0, eq));
        const std::string v =
            eq == std::string::npos ? "" : pct_dec(pair.substr(eq + 1));
        if (PyRef cur = dict_get(as_dict(d), py_str(k))) {
            if (auto* l = as_list(cur))
                l->v.push_back(py_str(v));
        } else {
            dict_set(as_dict(d), py_str(k), py_list({py_str(v)}));
        }
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return d;
}
PyRef m_urlparse(interpreter& i, const py_args& a) {
    const auto* u = as_str(arg_at(i, a, 0, "urlparse"));
    auto d = py_dict();
    std::string rest = u->v;
    const auto scheme_end = rest.find("://");
    std::string scheme;
    if (scheme_end != std::string::npos &&
        std::all_of(rest.begin(), rest.begin() + scheme_end, [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '+' ||
                   c == '-' || c == '.';
        })) {
        scheme = rest.substr(0, scheme_end);
        rest = rest.substr(scheme_end + 3);
    }
    std::string netloc, path_q;
    const auto slash = rest.find('/');
    if (slash == std::string::npos) {
        netloc = scheme.empty() ? "" : rest;
        path_q = scheme.empty() ? rest : "";
    } else {
        netloc = rest.substr(0, slash);
        path_q = rest.substr(slash);
    }
    const auto q = path_q.find('?');
    std::string path = q == std::string::npos ? path_q : path_q.substr(0, q);
    std::string query = q == std::string::npos ? "" : path_q.substr(q + 1);
    const auto frag = query.find('#');
    std::string fragment;
    if (frag != std::string::npos) {
        fragment = query.substr(frag + 1);
        query = query.substr(0, frag);
    }
    auto put = [&](const char* k, const std::string& v) {
        dict_set(as_dict(d), py_str(k), py_str(v));
    };
    put("scheme", scheme);
    put("netloc", netloc);
    put("path", path);
    put("params", "");
    put("query", query);
    put("fragment", fragment);
    return d;
}
PyRef m_urljoin(interpreter& i, const py_args& a) {
    const auto* base = as_str(arg_at(i, a, 0, "urljoin"));
    const auto* rel = as_str(arg_at(i, a, 1, "urljoin"));
    // minimal join
    if (rel->v.find("://") != std::string::npos)
        return a.pos[1];
    const auto scheme_end = base->v.find("://");
    const std::string prefix =
        scheme_end == std::string::npos ? "" : base->v.substr(0, scheme_end + 3);
    std::string rest =
        scheme_end == std::string::npos ? base->v : base->v.substr(scheme_end + 3);
    const auto slash = rest.find('/');
    std::string host = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (!rel->v.empty() && rel->v[0] == '/')
        return py_str(prefix + host + rel->v);
    std::string dir = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto last = dir.rfind('/');
    dir = dir.substr(0, last + 1);
    return py_str(prefix + host + dir + rel->v);
}
} // namespace urlparse_impl

PyRef mod_urllib_parse(interpreter& i) {
    PyRef m = mk_mod("urllib.parse");
    put_fn(m, "quote", urlparse_impl::m_quote);
    put_fn(m, "quote_plus", urlparse_impl::m_quote_plus);
    put_fn(m, "unquote", urlparse_impl::m_unquote);
    put_fn(m, "unquote_plus", urlparse_impl::m_unquote_plus);
    put_fn(m, "urlencode", urlparse_impl::m_urlencode);
    put_fn(m, "parse_qs", urlparse_impl::m_parse_qs);
    put_fn(m, "urlparse", urlparse_impl::m_urlparse);
    put_fn(m, "urljoin", urlparse_impl::m_urljoin);
    return m;
}
PyRef mod_urllib(interpreter& i) {
    PyRef m = mk_mod("urllib");
    put_c(m, "parse", mod_urllib_parse(i));
    return m;
}

// ═══ platform ═══
PyRef mod_platform(interpreter& i) {
    PyRef m = mk_mod("platform");
    put_fn(m, "system", [](interpreter&, const py_args&) {
        return py_str("Windows");
    });
    put_fn(m, "machine", [](interpreter&, const py_args&) {
        return py_str("AMD64");
    });
    put_fn(m, "processor", [](interpreter&, const py_args&) {
        return py_str("AMD64 Family");
    });
    put_fn(m, "python_version", [](interpreter&, const py_args&) {
        return py_str("3.11.0-pymini");
    });
    put_fn(m, "platform", [](interpreter&, const py_args&) {
        return py_str("Windows-10-pymini");
    });
    put_fn(m, "release", [](interpreter&, const py_args&) {
        return py_str("10");
    });
    put_fn(m, "version", [](interpreter&, const py_args&) {
        return py_str("10.0.22631");
    });
    put_fn(m, "node", [](interpreter&, const py_args&) {
        char buf[256];
        DWORD n = sizeof(buf);
        GetComputerNameA(buf, &n);
        return py_str(std::string(buf, n));
    });
    return m;
}

// ═══ shutil-lite ═══
namespace shutil_impl {
PyRef m_copyfile(interpreter& i, const py_args& a) {
    const auto* src = as_str(arg_at(i, a, 0, "copyfile"));
    const auto* dst = as_str(arg_at(i, a, 1, "copyfile"));
    if (!src || !dst)
        i.raise_exc("TypeError", "copyfile expects str paths", {});
    const auto wsrc = std::wstring(src->v.begin(), src->v.end());
    const auto wdst = std::wstring(dst->v.begin(), dst->v.end());
    if (!CopyFileW(wsrc.c_str(), wdst.c_str(), FALSE)) {
        i.raise_exc("OSError",
                    "copyfile failed: " + std::to_string(GetLastError()),
                    {});
    }
    return py_str(dst->v);
}
PyRef m_move(interpreter& i, const py_args& a) {
    const auto* src = as_str(arg_at(i, a, 0, "move"));
    const auto* dst = as_str(arg_at(i, a, 1, "move"));
    if (!src || !dst)
        i.raise_exc("TypeError", "move expects str paths", {});
    const auto wsrc = std::wstring(src->v.begin(), src->v.end());
    const auto wdst = std::wstring(dst->v.begin(), dst->v.end());
    if (!MoveFileW(wsrc.c_str(), wdst.c_str())) {
        i.raise_exc("OSError",
                    "move failed: " + std::to_string(GetLastError()), {});
    }
    return py_str(dst->v);
}
} // namespace shutil_impl

PyRef mod_shutil(interpreter& i) {
    PyRef m = mk_mod("shutil");
    put_fn(m, "copyfile", shutil_impl::m_copyfile);
    put_fn(m, "copy", shutil_impl::m_copyfile);
    put_fn(m, "move", shutil_impl::m_move);
    return m;
}

// ═══ errno ═══
PyRef mod_errno(interpreter& i) {
    PyRef m = mk_mod("errno");
    put_c(m, "ENOENT", py_int(2));
    put_c(m, "EACCES", py_int(5));
    put_c(m, "EPERM", py_int(1));
    put_c(m, "EEXIST", py_int(17));
    put_c(m, "EINVAL", py_int(22));
    put_c(m, "ENOTDIR", py_int(20));
    put_c(m, "EISDIR", py_int(21));
    put_c(m, "ENOTEMPTY", py_int(39));
    put_c(m, "ETIMEDOUT", py_int(138));
    return m;
}

} // namespace

// registration — pymini_stdlib2.cpp registers the rest.
void pymini_register_stdlib_factories_1(interpreter& i) {
    i.stdlib_factories["json"] = mod_json;
    i.stdlib_factories["math"] = mod_math;
    i.stdlib_factories["time"] = mod_time;
    i.stdlib_factories["re"] = mod_re;
    i.stdlib_factories["base64"] = mod_base64;
    i.stdlib_factories["hashlib"] = mod_hashlib;
    i.stdlib_factories["random"] = mod_random;
    i.stdlib_factories["struct"] = mod_struct;
    i.stdlib_factories["string"] = mod_string;
    i.stdlib_factories["textwrap"] = mod_textwrap;
    i.stdlib_factories["urllib"] = mod_urllib;
    i.stdlib_factories["urllib.parse"] = mod_urllib_parse;
    i.stdlib_factories["platform"] = mod_platform;
    i.stdlib_factories["shutil"] = mod_shutil;
    i.stdlib_factories["errno"] = mod_errno;
}

} // namespace sao::plugins::pymini
