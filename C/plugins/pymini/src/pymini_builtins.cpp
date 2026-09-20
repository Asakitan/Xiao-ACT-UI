// pymini_builtins.cpp — builtin functions, types and member-method tables.
//
// Install order: interpreter ctor leaves builtins_dict empty; the HOST
// (pymini_host.cpp) calls pymini_install_builtins(*this) once per new
// interpreter before user source runs.
//
// Builtin member methods (str.upper, list.append, dict.items, …) resolve
// lazily via getattr_builtin_member() which returns a PyBuiltinObj bound
// closure wrapping the receiver — zero per-object allocation beyond the
// wrapper.
#include "pymini_interp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace sao::plugins::pymini {
namespace {

// utf8 helpers for case ops
std::string utf8_lower(std::string s) {
    for (auto& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z')
            c = static_cast<char>(u + 32);
    }
    return s;
}
std::string utf8_upper(std::string s) {
    for (auto& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'a' && u <= 'z')
            c = static_cast<char>(u - 32);
    }
    return s;
}

int64_t str_find_sub(const std::string& hay, const std::string& needle,
                     int64_t start, int64_t end_) {
    const int64_t n = static_cast<int64_t>(hay.size());
    if (end_ < 0)
        end_ += n;
    if (end_ > n)
        end_ = n;
    if (start < 0)
        start += n;
    if (start < 0)
        start = 0;
    if (start >= end_)
        return -1;
    const auto pos = hay.find(needle, static_cast<std::size_t>(start));
    if (pos == std::string::npos || pos + needle.size() > static_cast<std::size_t>(end_))
        return -1;
    return static_cast<int64_t>(pos);
}

std::vector<std::string> str_split_ws(const std::string& s, int64_t maxsplit) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() &&
               std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= s.size())
            break;
        const std::size_t beg = i;
        while (i < s.size() &&
               !std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        out.push_back(s.substr(beg, i - beg));
        if (maxsplit >= 0 && static_cast<int64_t>(out.size()) >= maxsplit)
            break;
    }
    if (maxsplit >= 0 && static_cast<int64_t>(out.size()) == maxsplit &&
        i < s.size()) {
        while (i < s.size() &&
               std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i < s.size())
            out.push_back(s.substr(i));
    }
    return out;
}

std::vector<std::string> str_split_sep(const std::string& s,
                                       const std::string& sep,
                                       int64_t maxsplit) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t hit = s.find(sep, pos);
        if (hit == std::string::npos ||
            (maxsplit >= 0 &&
             static_cast<int64_t>(out.size()) >= maxsplit)) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, hit - pos));
        pos = hit + sep.size();
    }
    return out;
}

// helper — args guards
int64_t arg_int(interpreter& i, const py_args& a, std::size_t n,
                int64_t fallback = 0) {
    if (n >= a.pos.size())
        return fallback;
    const PyRef& v = a.pos[n];
    // CPython 'i' conversion: int/bool pass through, instances via __index__(),
    // everything else (float, str, list…) is a TypeError — no silent coerce.
    if (v && (v->kind == py_kind::integer || v->kind == py_kind::boolean))
        return py_to_int(v, nullptr);
    if (v && v->kind == py_kind::instance) {
        if (PyRef fn = i.getattr(v, "__index__")) {
            const PyRef r = i.call0(fn, {});
            if (r && (r->kind == py_kind::integer ||
                      r->kind == py_kind::boolean))
                return py_to_int(r, nullptr);
            i.raise_exc("TypeError", "__index__ returned non-int", {});
        }
    }
    i.raise_exc("TypeError",
                "expected integer argument, got '" +
                    std::string(py_type_name(v)) + "'",
                {});
}

PyRef need_arg(interpreter& i, const py_args& a, std::size_t n,
               const char* fname) {
    if (n < a.pos.size())
        return a.pos[n];
    i.raise_exc("TypeError",
                std::string(fname) + " missing required argument", {});
}

// produce bound member for receiver `obj` named `name` with fn
PyRef member_fn(const std::string& name, const PyRef& recv,
                py_native_fn fn) {
    return py_builtin(name, [fn, recv](interpreter& i,
                                       const py_args& a) -> PyRef {
        py_args a2;
        a2.pos.push_back(recv);
        for (const auto& p : a.pos)
            a2.pos.push_back(p);
        a2.kw = a.kw;
        return fn(i, a2);
    });
}

// ═══ str member methods ═══
// each takes (interp, args) where args.pos[0] = the string receiver
PyRef m_str_upper(interpreter& i, const py_args& a) {
    return py_str(utf8_upper(as_str(a.pos[0])->v));
}
PyRef m_str_lower(interpreter& i, const py_args& a) {
    return py_str(utf8_lower(as_str(a.pos[0])->v));
}
PyRef m_str_title(interpreter& i, const py_args& a) {
    std::string s = utf8_lower(as_str(a.pos[0])->v);
    bool word_start = true;
    for (auto& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(u >= 'a' && u <= 'z') && !(u >= '0' && u <= '9') &&
            !(u >= 'A' && u <= 'Z')) {
            word_start = true;
        } else if (word_start) {
            c = static_cast<char>(std::toupper(u));
            word_start = false;
        }
    }
    return py_str(s);
}
PyRef m_str_capitalize(interpreter& i, const py_args& a) {
    std::string s = utf8_lower(as_str(a.pos[0])->v);
    if (!s.empty())
        s[0] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(s[0])));
    return py_str(s);
}
PyRef m_str_casefold(interpreter& i, const py_args& a) {
    return py_str(utf8_lower(as_str(a.pos[0])->v));
}
PyRef m_str_swapcase(interpreter& i, const py_args& a) {
    std::string s = as_str(a.pos[0])->v;
    for (auto& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'a' && u <= 'z')
            c = static_cast<char>(u - 32);
        else if (u >= 'A' && u <= 'Z')
            c = static_cast<char>(u + 32);
    }
    return py_str(s);
}
PyRef m_str_strip(interpreter& i, const py_args& a) {
    std::string s = as_str(a.pos[0])->v;
    std::string chars;
    if (a.pos.size() > 1) {
        if (auto* c = as_str(a.pos[1]))
            chars = c->v;
        else if (!py_is_none(a.pos[1]))
            i.raise_exc("TypeError", "strip arg must be str or None", {});
    }
    auto is_sep = [&](char c) {
        return chars.empty()
                   ? std::isspace(static_cast<unsigned char>(c))
                   : chars.find(c) != std::string::npos;
    };
    std::size_t b = 0, e = s.size();
    while (b < e && is_sep(s[b]))
        ++b;
    while (e > b && is_sep(s[e - 1]))
        --e;
    return py_str(s.substr(b, e - b));
}
PyRef m_str_lstrip(interpreter& i, const py_args& a) {
    std::string s = as_str(a.pos[0])->v;
    std::string chars;
    if (a.pos.size() > 1 && !py_is_none(a.pos[1])) {
        if (auto* c = as_str(a.pos[1]))
            chars = c->v;
    }
    auto is_sep = [&](char c) {
        return chars.empty()
                   ? std::isspace(static_cast<unsigned char>(c))
                   : chars.find(c) != std::string::npos;
    };
    std::size_t b = 0;
    while (b < s.size() && is_sep(s[b]))
        ++b;
    return py_str(s.substr(b));
}
PyRef m_str_rstrip(interpreter& i, const py_args& a) {
    std::string s = as_str(a.pos[0])->v;
    std::string chars;
    if (a.pos.size() > 1 && !py_is_none(a.pos[1])) {
        if (auto* c = as_str(a.pos[1]))
            chars = c->v;
    }
    auto is_sep = [&](char c) {
        return chars.empty()
                   ? std::isspace(static_cast<unsigned char>(c))
                   : chars.find(c) != std::string::npos;
    };
    std::size_t e = s.size();
    while (e > 0 && is_sep(s[e - 1]))
        --e;
    return py_str(s.substr(0, e));
}
PyRef m_str_startswith(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    PyRef arg = need_arg(i, a, 1, "startswith");
    int64_t start = arg_int(i, a, 2, 0);
    auto check = [&](const std::string& p) {
        if (start < 0)
            start = 0;
        return start + static_cast<int64_t>(p.size()) <=
                       static_cast<int64_t>(s.size()) &&
               s.compare(static_cast<std::size_t>(start), p.size(), p) == 0;
    };
    if (auto* t = as_tuple(arg)) {
        for (const auto& p : t->v)
            if (auto* ps = as_str(p); ps && check(ps->v))
                return py_true();
        return py_false();
    }
    if (auto* ps = as_str(arg))
        return py_bool(check(ps->v));
    i.raise_exc("TypeError", "startswith first arg must be str or tuple", {});
}
PyRef m_str_endswith(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    PyRef arg = need_arg(i, a, 1, "endswith");
    auto check = [&](const std::string& suf) {
        return s.size() >= suf.size() &&
               s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (auto* t = as_tuple(arg)) {
        for (const auto& p : t->v)
            if (auto* ps = as_str(p); ps && check(ps->v))
                return py_true();
        return py_false();
    }
    if (auto* ps = as_str(arg))
        return py_bool(check(ps->v));
    i.raise_exc("TypeError", "endswith first arg must be str or tuple", {});
}
PyRef m_str_find(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    PyRef arg = need_arg(i, a, 1, "find");
    const auto* sub = as_str(arg);
    if (!sub)
        i.raise_exc("TypeError", "must be str", {});
    return py_int(str_find_sub(s, sub->v, arg_int(i, a, 2, 0),
                               arg_int(i, a, 3,
                                       static_cast<int64_t>(s.size()))));
}
PyRef m_str_rfind(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    const auto* sub = as_str(need_arg(i, a, 1, "rfind"));
    const auto pos = s.rfind(sub->v);
    return py_int(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
}
PyRef m_str_index(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    const auto* sub = as_str(need_arg(i, a, 1, "index"));
    const auto hit =
        str_find_sub(s, sub->v, arg_int(i, a, 2, 0),
                     arg_int(i, a, 3, static_cast<int64_t>(s.size())));
    if (hit < 0)
        i.raise_exc("ValueError", "substring not found", {});
    return py_int(hit);
}
PyRef m_str_count(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    const auto* sub = as_str(need_arg(i, a, 1, "count"));
    int64_t n = 0;
    std::size_t pos = 0;
    while ((pos = s.find(sub->v, pos)) != std::string::npos) {
        ++n;
        pos += sub->v.empty() ? 1 : sub->v.size();
    }
    return py_int(n);
}
PyRef m_str_replace(interpreter& i, const py_args& a) {
    std::string s = as_str(a.pos[0])->v;
    const auto* from = as_str(need_arg(i, a, 1, "replace"));
    const auto* to = as_str(need_arg(i, a, 2, "replace"));
    const int64_t maxn = arg_int(i, a, 3, -1);
    std::string out;
    std::size_t pos = 0;
    int64_t done = 0;
    if (from->v.empty()) {
        // insert `to` between every char
        for (const char c : s) {
            if (maxn < 0 || done < maxn) {
                out += to->v;
                ++done;
            }
            out += c;
        }
        if (maxn < 0 || done < maxn)
            out += to->v;
        return py_str(out);
    }
    while (true) {
        const std::size_t hit = s.find(from->v, pos);
        if (hit == std::string::npos || (maxn >= 0 && done >= maxn)) {
            out += s.substr(pos);
            break;
        }
        out += s.substr(pos, hit - pos);
        out += to->v;
        pos = hit + from->v.size();
        ++done;
    }
    return py_str(out);
}
PyRef m_str_split(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    PyRef sep_arg = a.pos.size() > 1 ? a.pos[1] : PyRef{};
    int64_t maxsplit = arg_int(i, a, 2, -1);
    std::vector<std::string> parts;
    if (!sep_arg || py_is_none(sep_arg)) {
        parts = str_split_ws(s, maxsplit);
    } else {
        const auto* sep = as_str(sep_arg);
        if (!sep || sep->v.empty())
            i.raise_exc("ValueError", "empty separator", {});
        parts = str_split_sep(s, sep->v, maxsplit);
    }
    std::vector<PyRef> out;
    for (auto& p : parts)
        out.push_back(py_str(p));
    return py_list(std::move(out));
}
PyRef m_str_rsplit(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    PyRef sep_arg = a.pos.size() > 1 ? a.pos[1] : PyRef{};
    const int64_t maxsplit = arg_int(i, a, 2, -1);
    std::vector<std::string> parts;
    if (!sep_arg || py_is_none(sep_arg)) {
        parts = str_split_ws(s, maxsplit);
        // Python applies maxsplit from the right on ws too — approximate:
        if (maxsplit >= 0 && static_cast<int64_t>(parts.size()) > maxsplit + 1) {
            std::vector<std::string> tail(parts.end() - (maxsplit + 1),
                                          parts.end());
            std::string head;
            for (std::size_t k = 0; k + (maxsplit + 1) < parts.size(); ++k) {
                head += parts[k];
                head += ' ';
            }
            std::vector<std::string> out2;
            if (!head.empty())
                out2.push_back(head.substr(0, head.size() - 1));
            out2.insert(out2.end(), tail.begin(), tail.end());
            parts = std::move(out2);
        }
    } else {
        const auto* sep = as_str(sep_arg);
        auto all = str_split_sep(s, sep->v, -1);
        if (maxsplit >= 0 &&
            static_cast<int64_t>(all.size()) > maxsplit + 1) {
            const std::size_t head_n = all.size() - (maxsplit + 1);
            std::string head = all[0];
            for (std::size_t k = 1; k < head_n; ++k) {
                head += sep->v;
                head += all[k];
            }
            std::vector<std::string> out2{head};
            out2.insert(out2.end(), all.begin() + head_n, all.end());
            parts = std::move(out2);
        } else {
            parts = std::move(all);
        }
    }
    std::vector<PyRef> out;
    for (auto& p : parts)
        out.push_back(py_str(p));
    return py_list(std::move(out));
}
PyRef m_str_join(interpreter& i, const py_args& a) {
    const auto& sep = as_str(a.pos[0])->v;
    PyRef it = need_arg(i, a, 1, "join");
    std::string out;
    bool first = true;
    i.for_each(it, [&](PyRef v) {
        auto* s = as_str(v);
        if (!s)
            i.raise_exc("TypeError", "sequence item must be str", {});
        if (!first)
            out += sep;
        out += s->v;
        first = false;
        return true;
    });
    return py_str(out);
}
PyRef m_str_format(interpreter& i, const py_args& a) {
    // minimal {} {0} {name} {.2f}-format support
    std::string s = as_str(a.pos[0])->v;
    std::string out;
    std::size_t auto_i = 0;
    for (std::size_t p = 0; p < s.size(); ++p) {
        if (s[p] == '{' && p + 1 < s.size() && s[p + 1] == '{') {
            out += '{';
            ++p;
            continue;
        }
        if (s[p] == '}' && p + 1 < s.size() && s[p + 1] == '}') {
            out += '}';
            ++p;
            continue;
        }
        if (s[p] != '{') {
            out += s[p];
            continue;
        }
        const auto close = s.find('}', p);
        if (close == std::string::npos) {
            out += s.substr(p);
            break;
        }
        std::string body = s.substr(p + 1, close - p - 1);
        p = close;
        // conv + spec split
        std::string spec, conv;
        if (auto c = body.find(':'); c != std::string::npos) {
            spec = body.substr(c + 1);
            body = body.substr(0, c);
        }
        PyRef val;
        if (body.empty()) {
            if (auto_i < a.pos.size() - 1)
                val = a.pos[1 + auto_i++];
        } else if (std::all_of(body.begin(), body.end(), ::isdigit)) {
            const std::size_t idx = static_cast<std::size_t>(std::stoi(body));
            if (idx + 1 < a.pos.size())
                val = a.pos[idx + 1];
        } else {
            for (const auto& [k, v] : a.kw)
                if (k == body)
                    val = v;
        }
        if (!val)
            i.raise_exc("KeyError", "format key '" + body + "'", {});
        if (!spec.empty() &&
            (spec.back() == 'f' || spec.back() == 'F' || spec.back() == 'e' ||
             spec.back() == 'g' || spec.back() == 'd' || spec.back() == 'x' ||
             spec.back() == 'X')) {
            std::string fmt = "%" + spec;
            char buf[128];
            if (spec.back() == 'd' || spec.back() == 'x' ||
                spec.back() == 'X') {
                bool ok = false;
                std::snprintf(buf, sizeof(buf), fmt.c_str(),
                              py_to_int(val, &ok));
            } else {
                bool ok = false;
                std::snprintf(buf, sizeof(buf), fmt.c_str(),
                              py_to_float(val, &ok));
            }
            out += buf;
        } else {
            out += py_to_str(i, val);
        }
    }
    return py_str(out);
}
PyRef m_str_partition(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    const auto* sep = as_str(need_arg(i, a, 1, "partition"));
    const auto pos = s.find(sep->v);
    if (pos == std::string::npos)
        return py_tuple({a.pos[0], py_str(""), py_str("")});
    return py_tuple({py_str(s.substr(0, pos)), a.pos[1],
                     py_str(s.substr(pos + sep->v.size()))});
}
PyRef m_str_rpartition(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    const auto* sep = as_str(need_arg(i, a, 1, "rpartition"));
    const auto pos = s.rfind(sep->v);
    if (pos == std::string::npos)
        return py_tuple({py_str(""), py_str(""), a.pos[0]});
    return py_tuple({py_str(s.substr(0, pos)), a.pos[1],
                     py_str(s.substr(pos + sep->v.size()))});
}
PyRef m_str_zfill(interpreter& i, const py_args& a) {
    std::string s = as_str(a.pos[0])->v;
    const int64_t w = arg_int(i, a, 1, 0);
    std::string sign;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        sign = s[0];
        s = s.substr(1);
    }
    if (static_cast<int64_t>(s.size()) < w - static_cast<int64_t>(sign.size()))
        s.insert(0, w - sign.size() - s.size(), '0');
    return py_str(sign + s);
}
PyRef m_str_center(interpreter& i, const py_args& a) {
    std::string s = as_str(a.pos[0])->v;
    const int64_t w = arg_int(i, a, 1, 0);
    const int64_t pad = w - static_cast<int64_t>(s.size());
    if (pad <= 0)
        return a.pos[0];
    const int64_t l = pad / 2;
    const int64_t r = pad - l;
    return py_str(std::string(l, ' ') + s + std::string(r, ' '));
}
PyRef m_str_ljust(interpreter& i, const py_args& a) {
    std::string s = as_str(a.pos[0])->v;
    const int64_t w = arg_int(i, a, 1, 0);
    char fill = ' ';
    if (a.pos.size() > 2)
        if (auto* fc = as_str(a.pos[2]); fc && !fc->v.empty())
            fill = fc->v[0];
    if (static_cast<int64_t>(s.size()) < w)
        s.append(w - s.size(), fill);
    return py_str(s);
}
PyRef m_str_rjust(interpreter& i, const py_args& a) {
    std::string s = as_str(a.pos[0])->v;
    const int64_t w = arg_int(i, a, 1, 0);
    char fill = ' ';
    if (a.pos.size() > 2)
        if (auto* fc = as_str(a.pos[2]); fc && !fc->v.empty())
            fill = fc->v[0];
    if (static_cast<int64_t>(s.size()) < w)
        s.insert(0, w - s.size(), fill);
    return py_str(s);
}
PyRef m_str_expandtabs(interpreter& i, const py_args& a) {
    const std::string s = as_str(a.pos[0])->v;
    const int64_t tab = arg_int(i, a, 1, 8);
    std::string out;
    int64_t col = 0;
    for (const char c : s) {
        if (c == '\t') {
            // tabsize<=0 → the tab expands to zero columns (CPython drops it)
            const int64_t n = tab > 0 ? tab - (col % tab) : 0;
            out.append(static_cast<std::size_t>(n), ' ');
            col += n;
        } else {
            out += c;
            col = (c == '\n' || c == '\r') ? 0 : col + 1;
        }
    }
    return py_str(out);
}
PyRef m_str_splitlines(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    const bool keep = a.pos.size() > 1 && i.truthy(a.pos[1]);
    std::vector<PyRef> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t nl = s.find('\n', pos);
        const bool crlf = nl != std::string::npos && nl > pos && s[nl - 1] == '\r';
        if (nl == std::string::npos) {
            out.push_back(py_str(s.substr(pos)));
            break;
        }
        out.push_back(py_str(s.substr(pos, crlf ? nl - pos - 1 : nl - pos)));
        if (keep) {
            auto* last = as_str(out.back());
            last->v += crlf ? "\r\n" : "\n";
        }
        pos = nl + 1;
    }
    return py_list(std::move(out));
}
PyRef m_str_isalpha(interpreter& i, const py_args& a) {
    (void)i;
    const auto& s = as_str(a.pos[0])->v;
    if (s.empty())
        return py_false();
    for (const char c : s)
        if (!std::isalpha(static_cast<unsigned char>(c)))
            return py_false();
    return py_true();
}
PyRef m_str_isdigit(interpreter& i, const py_args& a) {
    (void)i;
    const auto& s = as_str(a.pos[0])->v;
    if (s.empty())
        return py_false();
    for (const char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return py_false();
    return py_true();
}
PyRef m_str_isalnum(interpreter& i, const py_args& a) {
    (void)i;
    const auto& s = as_str(a.pos[0])->v;
    if (s.empty())
        return py_false();
    for (const char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)))
            return py_false();
    return py_true();
}
PyRef m_str_isspace(interpreter& i, const py_args& a) {
    (void)i;
    const auto& s = as_str(a.pos[0])->v;
    if (s.empty())
        return py_false();
    for (const char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)))
            return py_false();
    return py_true();
}
PyRef m_str_islower(interpreter& i, const py_args& a) {
    (void)i;
    const auto& s = as_str(a.pos[0])->v;
    bool any = false;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z')
            return py_false();
        if (u >= 'a' && u <= 'z')
            any = true;
    }
    return py_bool(any);
}
PyRef m_str_isupper(interpreter& i, const py_args& a) {
    (void)i;
    const auto& s = as_str(a.pos[0])->v;
    bool any = false;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'a' && u <= 'z')
            return py_false();
        if (u >= 'A' && u <= 'Z')
            any = true;
    }
    return py_bool(any);
}
PyRef m_str_istitle(interpreter& i, const py_args& a) {
    (void)i;
    const auto& s = as_str(a.pos[0])->v;
    bool in_word = false, any = false;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool alpha = std::isalpha(u);
        if (alpha && !in_word) {
            if (!std::isupper(u))
                return py_false();
            in_word = true;
            any = true;
        } else if (alpha && in_word && !std::islower(u)) {
            return py_false();
        } else if (!alpha) {
            in_word = false;
        }
    }
    return py_bool(any);
}
PyRef m_str_removeprefix(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    const auto* p = as_str(need_arg(i, a, 1, "removeprefix"));
    if (s.rfind(p->v, 0) == 0)
        return py_str(s.substr(p->v.size()));
    return a.pos[0];
}
PyRef m_str_removesuffix(interpreter& i, const py_args& a) {
    const auto& s = as_str(a.pos[0])->v;
    const auto* p = as_str(need_arg(i, a, 1, "removesuffix"));
    if (!p->v.empty() && s.size() >= p->v.size() &&
        s.compare(s.size() - p->v.size(), p->v.size(), p->v) == 0)
        return py_str(s.substr(0, s.size() - p->v.size()));
    return a.pos[0];
}
PyRef m_str_encode(interpreter& i, const py_args& a) {
    return py_bytes(as_str(a.pos[0])->v);
}

// ═══ list member methods ═══
PyRef m_list_append(interpreter& i, const py_args& a) {
    (void)i;
    as_list(a.pos[0])->v.push_back(need_arg(i, a, 1, "append"));
    return py_none();
}
PyRef m_list_extend(interpreter& i, const py_args& a) {
    auto* l = as_list(a.pos[0]);
    // materialize the source first — `l.extend(l)` (or any iterator
    // aliasing l) would otherwise grow the vector mid-iteration forever.
    std::vector<PyRef> items;
    i.for_each(need_arg(i, a, 1, "extend"), [&](PyRef v) {
        items.push_back(std::move(v));
        return true;
    });
    l->v.insert(l->v.end(), items.begin(), items.end());
    return py_none();
}
PyRef m_list_insert(interpreter& i, const py_args& a) {
    auto* l = as_list(a.pos[0]);
    int64_t n = arg_int(i, a, 1, 0);
    PyRef v = need_arg(i, a, 2, "insert");
    if (n < 0)
        n += static_cast<int64_t>(l->v.size());
    if (n < 0)
        n = 0;
    if (n > static_cast<int64_t>(l->v.size()))
        n = static_cast<int64_t>(l->v.size());
    l->v.insert(l->v.begin() + n, std::move(v));
    return py_none();
}
PyRef m_list_remove(interpreter& i, const py_args& a) {
    auto* l = as_list(a.pos[0]);
    PyRef v = need_arg(i, a, 1, "remove");
    for (std::size_t k = 0; k < l->v.size(); ++k)
        if (py_eq(i, l->v[k], v)) {
            l->v.erase(l->v.begin() + k);
            return py_none();
        }
    i.raise_exc("ValueError", "list.remove(x): x not in list", {});
}
PyRef m_list_pop(interpreter& i, const py_args& a) {
    auto* l = as_list(a.pos[0]);
    if (l->v.empty())
        i.raise_exc("IndexError", "pop from empty list", {});
    int64_t n = arg_int(i, a, 1, -1);
    if (n < 0)
        n += static_cast<int64_t>(l->v.size());
    if (n < 0 || n >= static_cast<int64_t>(l->v.size()))
        i.raise_exc("IndexError", "pop index out of range", {});
    PyRef v = l->v[static_cast<std::size_t>(n)];
    l->v.erase(l->v.begin() + n);
    return v;
}
PyRef m_list_clear(interpreter& i, const py_args& a) {
    (void)i;
    as_list(a.pos[0])->v.clear();
    return py_none();
}
PyRef m_list_index(interpreter& i, const py_args& a) {
    auto* l = as_list(a.pos[0]);
    PyRef v = need_arg(i, a, 1, "index");
    for (std::size_t k = 0; k < l->v.size(); ++k)
        if (py_eq(i, l->v[k], v))
            return py_int(static_cast<int64_t>(k));
    i.raise_exc("ValueError", "x not in list", {});
}
PyRef m_list_count(interpreter& i, const py_args& a) {
    auto* l = as_list(a.pos[0]);
    PyRef v = need_arg(i, a, 1, "count");
    int64_t n = 0;
    for (const auto& el : l->v)
        if (py_eq(i, el, v))
            ++n;
    return py_int(n);
}
PyRef m_list_sort(interpreter& i, const py_args& a) {
    auto* l = as_list(a.pos[0]);
    PyRef key_fn;
    bool rev = false;
    for (const auto& [k, v] : a.kw) {
        if (k == "key")
            key_fn = v;
        if (k == "reverse")
            rev = i.truthy(v);
    }
    if (a.pos.size() > 1)
        key_fn = a.pos[1];
    std::stable_sort(l->v.begin(), l->v.end(), [&](const PyRef& x,
                                                   const PyRef& y) {
        PyRef kx = key_fn ? i.call1(key_fn, x, {}) : x;
        PyRef ky = key_fn ? i.call1(key_fn, y, {}) : y;
        bool ok = false;
        int c = py_cmp(i, kx, ky, &ok);
        if (!ok)
            i.raise_exc("TypeError", "unorderable types", {});
        return rev ? c > 0 : c < 0;
    });
    return py_none();
}
PyRef m_list_reverse(interpreter& i, const py_args& a) {
    (void)i;
    auto* l = as_list(a.pos[0]);
    std::reverse(l->v.begin(), l->v.end());
    return py_none();
}
PyRef m_list_copy(interpreter& i, const py_args& a) {
    (void)i;
    return py_list(as_list(a.pos[0])->v);
}

// ═══ tuple member methods (tuple.count/index mirror list semantics) ═══
PyRef m_tuple_count(interpreter& i, const py_args& a) {
    auto* t = as_tuple(a.pos[0]);
    PyRef v = need_arg(i, a, 1, "count");
    int64_t n = 0;
    for (const auto& el : t->v)
        if (py_eq(i, el, v))
            ++n;
    return py_int(n);
}
PyRef m_tuple_index(interpreter& i, const py_args& a) {
    auto* t = as_tuple(a.pos[0]);
    PyRef v = need_arg(i, a, 1, "index");
    for (std::size_t k = 0; k < t->v.size(); ++k)
        if (py_eq(i, t->v[k], v))
            return py_int(static_cast<int64_t>(k));
    i.raise_exc("ValueError", "x not in tuple", {});
}

// ═══ dict member methods ═══
PyRef m_dict_get(interpreter& i, const py_args& a) {
    auto* d = as_dict(a.pos[0]);
    PyRef v = dict_get(i, d, need_arg(i, a, 1, "get"));
    if (!v && a.pos.size() > 2)
        return a.pos[2];
    return v ? v : py_none();
}
PyRef m_dict_keys(interpreter& i, const py_args& a) {
    (void)i;
    std::vector<PyRef> out;
    for (const auto& [k, v] : as_dict(a.pos[0])->items)
        out.push_back(k);
    return py_list(std::move(out));
}
PyRef m_dict_values(interpreter& i, const py_args& a) {
    (void)i;
    std::vector<PyRef> out;
    for (const auto& [k, v] : as_dict(a.pos[0])->items)
        out.push_back(v);
    return py_list(std::move(out));
}
PyRef m_dict_items(interpreter& i, const py_args& a) {
    (void)i;
    std::vector<PyRef> out;
    for (const auto& [k, v] : as_dict(a.pos[0])->items)
        out.push_back(py_tuple({k, v}));
    return py_list(std::move(out));
}
PyRef m_dict_setdefault(interpreter& i, const py_args& a) {
    auto* d = as_dict(a.pos[0]);
    PyRef key = need_arg(i, a, 1, "setdefault");
    if (PyRef v = dict_get(i, d, key))
        return v;
    PyRef dflt = a.pos.size() > 2 ? a.pos[2] : py_none();
    dict_set(i, d, key, dflt);
    return dflt;
}
PyRef m_dict_update(interpreter& i, const py_args& a) {
    auto* d = as_dict(a.pos[0]);
    for (std::size_t n = 1; n < a.pos.size(); ++n) {
        PyRef src = a.pos[n];
        if (auto* sd = as_dict(src)) {
            for (auto& [k, v] : sd->items)
                dict_set(i, d, k, v);
        } else {
            i.for_each(src, [&](PyRef pair) {
                if (auto* t = as_tuple(pair); t && t->v.size() == 2)
                    dict_set(i, d, t->v[0], t->v[1]);
                else if (auto* l = as_list(pair); l && l->v.size() == 2)
                    dict_set(i, d, l->v[0], l->v[1]);
                return true;
            });
        }
    }
    for (const auto& [k, v] : a.kw)
        dict_set(i, d, py_str(k), v);
    return py_none();
}
PyRef m_dict_pop(interpreter& i, const py_args& a) {
    auto* d = as_dict(a.pos[0]);
    PyRef key = need_arg(i, a, 1, "pop");
    if (PyRef v = dict_get(i, d, key)) {
        dict_del(i, d, key);
        return v;
    }
    if (a.pos.size() > 2)
        return a.pos[2];
    i.raise_exc("KeyError", py_repr(i, key), {});
}
PyRef m_dict_popitem(interpreter& i, const py_args& a) {
    auto* d = as_dict(a.pos[0]);
    if (d->items.empty())
        i.raise_exc("KeyError", "popitem(): dictionary is empty", {});
    auto pr = d->items.back();
    d->items.pop_back();
    return py_tuple({pr.first, pr.second});
}
PyRef m_dict_clear(interpreter& i, const py_args& a) {
    (void)i;
    as_dict(a.pos[0])->items.clear();
    return py_none();
}
PyRef m_dict_copy(interpreter& i, const py_args& a) {
    (void)i;
    auto d = py_dict();
    for (const auto& [k, v] : as_dict(a.pos[0])->items)
        dict_set(i, as_dict(d), k, v);
    return d;
}
PyRef m_dict_fromkeys(interpreter& i, const py_args& a) {
    // called as dict.fromkeys(seq[, v]) — receiver slot carries none here
    PyRef seq = need_arg(i, a, 0, "fromkeys");
    PyRef dflt = a.pos.size() > 1 ? a.pos[1] : py_none();
    auto d = py_dict();
    i.for_each(seq, [&](PyRef k) {
        dict_set(i, as_dict(d), k, dflt);
        return true;
    });
    return d;
}

// ═══ set member methods ═══
PyRef m_set_add(interpreter& i, const py_args& a) {
    auto* s = as_set(a.pos[0]);
    set_add(i, s->items, need_arg(i, a, 1, "add"));
    return py_none();
}
PyRef m_set_discard(interpreter& i, const py_args& a) {
    auto* s = as_set(a.pos[0]);
    PyRef v = need_arg(i, a, 1, "discard");
    for (auto it = s->items.begin(); it != s->items.end(); ++it)
        if (py_eq(i, *it, v)) {
            s->items.erase(it);
            break;
        }
    return py_none();
}
PyRef m_set_remove(interpreter& i, const py_args& a) {
    auto* s = as_set(a.pos[0]);
    PyRef v = need_arg(i, a, 1, "remove");
    for (auto it = s->items.begin(); it != s->items.end(); ++it)
        if (py_eq(i, *it, v)) {
            s->items.erase(it);
            return py_none();
        }
    i.raise_exc("KeyError", py_repr(i, v), {});
}
PyRef m_set_pop(interpreter& i, const py_args& a) {
    auto* s = as_set(a.pos[0]);
    if (s->items.empty())
        i.raise_exc("KeyError", "pop from an empty set", {});
    PyRef v = s->items.front();
    s->items.erase(s->items.begin());
    return v;
}
PyRef m_set_clear(interpreter& i, const py_args& a) {
    (void)i;
    as_set(a.pos[0])->items.clear();
    return py_none();
}
PyRef m_set_update(interpreter& i, const py_args& a) {
    auto* s = as_set(a.pos[0]);
    for (std::size_t n = 1; n < a.pos.size(); ++n)
        i.for_each(a.pos[n], [&](PyRef v) {
            set_add(i, s->items, v);
            return true;
        });
    return py_none();
}
PyRef m_set_copy(interpreter& i, const py_args& a) {
    (void)i;
    auto out = std::make_shared<PySetObj>();
    out->items = as_set(a.pos[0])->items;
    return out;
}
PyRef m_set_union(interpreter& i, const py_args& a) {
    auto out = std::make_shared<PySetObj>();
    out->items = as_set(a.pos[0])->items;
    for (std::size_t n = 1; n < a.pos.size(); ++n)
        i.for_each(a.pos[n], [&](PyRef v) {
            set_add(i, out->items, v);
            return true;
        });
    return out;
}
PyRef m_set_intersection(interpreter& i, const py_args& a) {
    auto out = std::make_shared<PySetObj>();
    auto* s = as_set(a.pos[0]);
    for (const auto& v : s->items) {
        bool all_in = true;
        for (std::size_t n = 1; n < a.pos.size() && all_in; ++n)
            if (!i.contains(v, a.pos[n]))
                all_in = false;
        if (all_in)
            set_add(i, out->items, v);
    }
    return out;
}
PyRef m_set_difference(interpreter& i, const py_args& a) {
    auto out = std::make_shared<PySetObj>();
    for (const auto& v : as_set(a.pos[0])->items) {
        bool any_in = false;
        for (std::size_t n = 1; n < a.pos.size() && !any_in; ++n)
            if (i.contains(v, a.pos[n]))
                any_in = true;
        if (!any_in)
            set_add(i, out->items, v);
    }
    return out;
}
PyRef m_set_issubset(interpreter& i, const py_args& a) {
    for (const auto& v : as_set(a.pos[0])->items)
        if (!i.contains(v, need_arg(i, a, 1, "issubset")))
            return py_false();
    return py_true();
}
PyRef m_set_issuperset(interpreter& i, const py_args& a) {
    // args: this, other → every item of other ⊆ this
    PyRef other = need_arg(i, a, 1, "issuperset");
    bool all = true;
    i.for_each(other, [&](PyRef v) {
        if (!i.contains(v, a.pos[0]))
            all = false;
        return all;
    });
    return py_bool(all);
}
PyRef m_set_isdisjoint(interpreter& i, const py_args& a) {
    PyRef other = need_arg(i, a, 1, "isdisjoint");
    bool dis = true;
    i.for_each(other, [&](PyRef v) {
        if (i.contains(v, a.pos[0]))
            dis = false;
        return dis;
    });
    return py_bool(dis);
}

// ═══ bytes member methods ═══
PyRef m_bytes_decode(interpreter& i, const py_args& a) {
    (void)i;
    return py_str(as_bytes(a.pos[0])->v);   // utf8 passthrough
}
PyRef m_bytes_hex(interpreter& i, const py_args& a) {
    (void)i;
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : as_bytes(a.pos[0])->v) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return py_str(out);
}
PyRef m_bytes_fromhex(interpreter& i, const py_args& a) {
    (void)i;
    const auto* s = as_str(need_arg(i, a, 0, "fromhex"));
    std::string out;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t k = 0; k + 1 < s->v.size();) {
        if (s->v[k] == ' ') {
            ++k;
            continue;
        }
        const int h = nib(s->v[k]), l = nib(s->v[k + 1]);
        if (h < 0 || l < 0)
            i.raise_exc("ValueError", "non-hexadecimal number found in fromhex()", {});
        out += static_cast<char>((h << 4) | l);
        k += 2;
    }
    return py_bytes(out);
}
PyRef m_bytes_join(interpreter& i, const py_args& a) {
    const auto& sep = as_bytes(a.pos[0])->v;
    std::string out;
    bool first = true;
    i.for_each(need_arg(i, a, 1, "join"), [&](PyRef v) {
        if (auto* b = as_bytes(v)) {
            if (!first)
                out += sep;
            out += b->v;
            first = false;
        }
        return true;
    });
    return py_bytes(out);
}
PyRef m_bytes_startswith(interpreter& i, const py_args& a) {
    const auto& s = as_bytes(a.pos[0])->v;
    const auto* p = as_bytes(need_arg(i, a, 1, "startswith"));
    if (!p)
        i.raise_exc("TypeError", "startswith arg must be bytes", {});
    return py_bool(s.rfind(p->v, 0) == 0);
}
PyRef m_bytes_split(interpreter& i, const py_args& a) {
    const auto& s = as_bytes(a.pos[0])->v;
    const auto* sep = a.pos.size() > 1 ? as_bytes(a.pos[1]) : nullptr;
    std::vector<PyRef> out;
    if (!sep || sep->v.empty()) {
        for (auto& p : str_split_ws(s, a.pos.size() > 2 ? arg_int(i, a, 2, -1)
                                                     : -1))
            out.push_back(py_bytes(p));
        return py_list(std::move(out));
    }
    for (auto& p : str_split_sep(s, sep->v, arg_int(i, a, 2, -1)))
        out.push_back(py_bytes(p));
    return py_list(std::move(out));
}
PyRef m_bytes_find(interpreter& i, const py_args& a) {
    const auto& s = as_bytes(a.pos[0])->v;
    const auto* sub = as_bytes(need_arg(i, a, 1, "find"));
    if (!sub)
        i.raise_exc("TypeError", "must be bytes", {});
    return py_int(str_find_sub(s, sub->v, arg_int(i, a, 2, 0),
                               arg_int(i, a, 3,
                                       static_cast<int64_t>(s.size()))));
}

// ═══ int/float misc ═══
PyRef m_num_bit_length(interpreter& i, const py_args& a) {
    bool ok = false;
    int64_t v = py_to_int(a.pos[0], &ok);
    (void)i;
    if (v < 0)
        v = -v;
    return py_int(v == 0 ? 0 : 64 - __lzcnt64(static_cast<uint64_t>(v)));
}
PyRef m_num_is_integer(interpreter& i, const py_args& a) {
    (void)i;
    const auto* f = as_float(a.pos[0]);
    return py_bool(f && std::floor(f->v) == f->v);
}
PyRef m_num_hex(interpreter& i, const py_args& a) {
    (void)i;
    const auto* f = as_float(a.pos[0]);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%a", f ? f->v : 0.0);
    return py_str(buf);
}

// ═══ member tables ═══
using member_entry = std::pair<const char*, py_native_fn>;

const member_entry str_members[] = {
    {"upper", m_str_upper},         {"lower", m_str_lower},
    {"title", m_str_title},         {"capitalize", m_str_capitalize},
    {"casefold", m_str_casefold},   {"swapcase", m_str_swapcase},
    {"strip", m_str_strip},         {"lstrip", m_str_lstrip},
    {"rstrip", m_str_rstrip},       {"startswith", m_str_startswith},
    {"endswith", m_str_endswith},   {"find", m_str_find},
    {"rfind", m_str_rfind},         {"index", m_str_index},
    {"count", m_str_count},         {"replace", m_str_replace},
    {"split", m_str_split},         {"rsplit", m_str_rsplit},
    {"join", m_str_join},           {"format", m_str_format},
    {"partition", m_str_partition}, {"rpartition", m_str_rpartition},
    {"zfill", m_str_zfill},         {"center", m_str_center},
    {"ljust", m_str_ljust},         {"rjust", m_str_rjust},
    {"expandtabs", m_str_expandtabs},
    {"splitlines", m_str_splitlines},
    {"isalpha", m_str_isalpha},     {"isdigit", m_str_isdigit},
    {"isalnum", m_str_isalnum},     {"isspace", m_str_isspace},
    {"islower", m_str_islower},     {"isupper", m_str_isupper},
    {"istitle", m_str_istitle},     {"removeprefix", m_str_removeprefix},
    {"removesuffix", m_str_removesuffix},
    {"encode", m_str_encode},
};

const member_entry list_members[] = {
    {"append", m_list_append},     {"extend", m_list_extend},
    {"insert", m_list_insert},     {"remove", m_list_remove},
    {"pop", m_list_pop},           {"clear", m_list_clear},
    {"index", m_list_index},       {"count", m_list_count},
    {"sort", m_list_sort},         {"reverse", m_list_reverse},
    {"copy", m_list_copy},
};

const member_entry dict_members[] = {
    {"get", m_dict_get},           {"keys", m_dict_keys},
    {"values", m_dict_values},     {"items", m_dict_items},
    {"setdefault", m_dict_setdefault},
    {"update", m_dict_update},     {"pop", m_dict_pop},
    {"popitem", m_dict_popitem},   {"clear", m_dict_clear},
    {"copy", m_dict_copy},
};

const member_entry set_members[] = {
    {"add", m_set_add},            {"discard", m_set_discard},
    {"remove", m_set_remove},      {"pop", m_set_pop},
    {"clear", m_set_clear},        {"update", m_set_update},
    {"copy", m_set_copy},          {"union", m_set_union},
    {"intersection", m_set_intersection},
    {"difference", m_set_difference},
    {"issubset", m_set_issubset},  {"issuperset", m_set_issuperset},
    {"isdisjoint", m_set_isdisjoint},
};

const member_entry bytes_members[] = {
    {"decode", m_bytes_decode},    {"hex", m_bytes_hex},
    {"join", m_bytes_join},        {"startswith", m_bytes_startswith},
    {"split", m_bytes_split},      {"find", m_bytes_find},
};

PyRef resolve_member(const PyRef& recv, const std::string& name,
                     const member_entry* table, std::size_t n) {
    for (std::size_t k = 0; k < n; ++k)
        if (name == table[k].first)
            return member_fn(name, recv, table[k].second);
    return nullptr;
}

} // namespace

PyRef getattr_builtin_member(interpreter& i, const PyRef& obj,
                             const std::string& name) {
    if (PyRef m = pymini_extra_member(i, obj, name))
        return m;                           // counter_/deque_/queue_/file_/thread_
    (void)i;
    switch (obj ? obj->kind : py_kind::none_) {
    case py_kind::string:
        if (name == "__class__")
            return nullptr;
        return resolve_member(obj, name, str_members,
                              sizeof(str_members) / sizeof(str_members[0]));
    case py_kind::list:
        return resolve_member(obj, name, list_members,
                              sizeof(list_members) / sizeof(list_members[0]));
    case py_kind::dict:
        return resolve_member(obj, name, dict_members,
                              sizeof(dict_members) / sizeof(dict_members[0]));
    case py_kind::set:
    case py_kind::frozenset:
        return resolve_member(obj, name, set_members,
                              sizeof(set_members) / sizeof(set_members[0]));
    case py_kind::bytes_:
        return resolve_member(obj, name, bytes_members,
                              sizeof(bytes_members) / sizeof(bytes_members[0]));
    case py_kind::integer:
    case py_kind::boolean:
        if (name == "bit_length")
            return member_fn("bit_length", obj, m_num_bit_length);
        if (name == "real" || name == "numerator")
            return obj;
        if (name == "imag" || name == "denominator")
            return py_int(obj->kind == py_kind::boolean ? 1 : 0);
        return nullptr;
    case py_kind::number:
        if (name == "is_integer")
            return member_fn("is_integer", obj, m_num_is_integer);
        if (name == "hex")
            return member_fn("hex", obj, m_num_hex);
        if (name == "real" || name == "imag")
            return obj;
        return nullptr;
    case py_kind::tuple_:
        if (name == "count")
            return member_fn("count", obj, m_tuple_count);
        if (name == "index")
            return member_fn("index", obj, m_tuple_index);
        return nullptr;
    case py_kind::builtin: {
        // class-level statics on builtin type objects: dict.fromkeys(seq[,v])
        const auto* b = as_builtin(obj);
        if (b != nullptr && b->name == "dict" && name == "fromkeys")
            return py_builtin("fromkeys", m_dict_fromkeys);
        return nullptr;
    }
    default:
        return nullptr;
    }
}

// ═══ global builtins ═══
void pymini_install_builtins(interpreter& i);   // fwd

namespace {
PyRef b_len(interpreter& i, const py_args& a) {
    PyRef o = need_arg(i, a, 0, "len");
    switch (o->kind) {
    case py_kind::string:
        // count utf8 code points, not bytes
        {
            const auto& s = as_str(o)->v;
            int64_t n = 0;
            for (std::size_t k = 0; k < s.size();) {
                const unsigned char c = static_cast<unsigned char>(s[k]);
                k += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
                ++n;
            }
            return py_int(n);
        }
    case py_kind::bytes_:
        return py_int(static_cast<int64_t>(as_bytes(o)->v.size()));
    case py_kind::list:
        return py_int(static_cast<int64_t>(as_list(o)->v.size()));
    case py_kind::tuple_:
        return py_int(static_cast<int64_t>(as_tuple(o)->v.size()));
    case py_kind::set:
    case py_kind::frozenset:
        return py_int(static_cast<int64_t>(as_set(o)->items.size()));
    case py_kind::dict:
        return py_int(static_cast<int64_t>(as_dict(o)->items.size()));
    case py_kind::range_: {
        const auto* r = static_cast<PyRangeObj*>(o.get());
        const int64_t n =
            r->step > 0 ? (r->stop - r->start + r->step - 1) / r->step
                        : (r->start - r->stop - r->step - 1) / (-r->step);
        return py_int(n < 0 ? 0 : n);
    }
    case py_kind::instance: {
        if (PyRef fn = i.getattr(o, "__len__"))
            return i.call0(fn, {});
        break;
    }
    default:
        break;
    }
    i.raise_exc("TypeError",
                "object of type '" + std::string(py_type_name(o)) +
                    "' has no len()",
                {});
}

PyRef b_print(interpreter& i, const py_args& a) {
    std::string out;
    std::string sep = " ", end = "\n";
    for (const auto& [k, v] : a.kw) {
        if (k == "sep")
            if (auto* s = as_str(v))
                sep = s->v;
        if (k == "end")
            if (auto* s = as_str(v))
                end = s->v;
    }
    for (std::size_t n = 0; n < a.pos.size(); ++n) {
        if (n)
            out += sep;
        out += py_to_str(i, a.pos[n]);
    }
    out += end;
    if (i.on_log)
        i.on_log(out);
    return py_none();
}

PyRef b_str(interpreter& i, const py_args& a) {
    if (a.pos.empty())
        return py_str("");
    return py_str(py_to_str(i, a.pos[0]));
}
PyRef b_repr(interpreter& i, const py_args& a) {
    return py_str(py_repr(i, need_arg(i, a, 0, "repr")));
}
PyRef b_int(interpreter& i, const py_args& a) {
    PyRef v = need_arg(i, a, 0, "int");
    if (auto* s = as_str(v)) {
        std::string t = s->v;
        // trim whitespace
        const auto b = t.find_first_not_of(" \t\r\n");
        const auto e = t.find_last_not_of(" \t\r\n");
        t = b == std::string::npos ? "" : t.substr(b, e - b + 1);
        // base param?
        int64_t base = arg_int(i, a, 1, 10);
        if (t.size() > 2 && t[0] == '0' &&
            (t[1] == 'x' || t[1] == 'X' || t[1] == 'b' || t[1] == 'o')) {
            const char pfx = t[1] == 'x' || t[1] == 'X' ? 'x'
                             : t[1] == 'b' ? 'b' : 'o';
            base = pfx == 'x' ? 16 : pfx == 'b' ? 2 : 8;
            t = t.substr(2);
        }
        try {
            std::size_t used = 0;
            const long long iv = std::stoll(t, &used, static_cast<int>(base));
            if (used != t.size())   // trailing junk: int("12x") must fail
                i.raise_exc("ValueError", "invalid literal for int()", {});
            return py_int(iv);
        } catch (...) {
            i.raise_exc("ValueError", "invalid literal for int()", {});
        }
    }
    bool ok = false;
    const int64_t n = py_to_int(v, &ok);
    if (!ok)
        i.raise_exc("TypeError", "int() argument must be number or string", {});
    return py_int(n);
}
PyRef b_float(interpreter& i, const py_args& a) {
    PyRef v = need_arg(i, a, 0, "float");
    if (auto* s = as_str(v)) {
        try {
            return py_float(std::stod(s->v));
        } catch (...) {
            i.raise_exc("ValueError", "could not convert string to float", {});
        }
    }
    bool ok = false;
    const double d = py_to_float(v, &ok);
    if (!ok)
        i.raise_exc("TypeError", "float() argument must be number or string", {});
    return py_float(d);
}
PyRef b_bool(interpreter& i, const py_args& a) {
    if (a.pos.empty())
        return py_false();
    return py_bool(i.truthy(a.pos[0]));
}
PyRef b_bytes(interpreter& i, const py_args& a) {
    if (a.pos.empty())
        return py_bytes("");
    PyRef v = a.pos[0];
    if (auto* s = as_str(v))
        return py_bytes(s->v);
    if (auto* b = as_bytes(v))
        return v;
    if (py_is_int_like(v)) {
        bool ok = false;
        const int64_t n = py_to_int(v, &ok);
        return py_bytes(std::string(static_cast<std::size_t>(n), '\0'));
    }
    std::string out;
    i.for_each(v, [&](PyRef item) {
        bool ok = false;
        out += static_cast<char>(py_to_int(item, &ok) & 0xFF);
        return true;
    });
    return py_bytes(out);
}
PyRef b_bytearray(interpreter& i, const py_args& a) {
    return b_bytes(i, a);   // model bytearray as immutable bytes (subset)
}
PyRef b_list(interpreter& i, const py_args& a) {
    std::vector<PyRef> out;
    if (!a.pos.empty())
        i.for_each(a.pos[0], [&](PyRef v) {
            out.push_back(std::move(v));
            return true;
        });
    return py_list(std::move(out));
}
PyRef b_tuple(interpreter& i, const py_args& a) {
    std::vector<PyRef> out;
    if (!a.pos.empty())
        i.for_each(a.pos[0], [&](PyRef v) {
            out.push_back(std::move(v));
            return true;
        });
    return py_tuple(std::move(out));
}
PyRef b_set(interpreter& i, const py_args& a) {
    auto s = py_set();
    if (!a.pos.empty())
        i.for_each(a.pos[0], [&](PyRef v) {
            set_add(i, as_set(s)->items, v);
            return true;
        });
    return s;
}
PyRef b_frozenset(interpreter& i, const py_args& a) {
    auto s = std::make_shared<PySetObj>(py_kind::frozenset,
                                        std::vector<PyRef>{});
    if (!a.pos.empty())
        i.for_each(a.pos[0], [&](PyRef v) {
            set_add(i, s->items, v);
            return true;
        });
    return s;
}
PyRef b_dict(interpreter& i, const py_args& a) {
    auto d = py_dict();
    if (!a.pos.empty()) {
        PyRef src = a.pos[0];
        if (auto* sd = as_dict(src)) {
            for (auto& [k, v] : sd->items)
                dict_set(i, as_dict(d), k, v);
        } else {
            i.for_each(src, [&](PyRef pair) {
                std::vector<PyRef> kv;
                i.for_each(pair, [&](PyRef e) {
                    kv.push_back(e);
                    return true;
                });
                if (kv.size() == 2)
                    dict_set(i, as_dict(d), kv[0], kv[1]);
                return true;
            });
        }
    }
    for (const auto& [k, v] : a.kw)
        dict_set(i, as_dict(d), py_str(k), v);
    return d;
}
PyRef b_sorted(interpreter& i, const py_args& a) {
    std::vector<PyRef> out;
    i.for_each(need_arg(i, a, 0, "sorted"), [&](PyRef v) {
        out.push_back(std::move(v));
        return true;
    });
    PyRef key_fn;
    bool rev = false;
    for (const auto& [k, v] : a.kw) {
        if (k == "key")
            key_fn = v;
        if (k == "reverse")
            rev = i.truthy(v);
    }
    std::stable_sort(out.begin(), out.end(), [&](const PyRef& x, const PyRef& y) {
        PyRef kx = key_fn ? i.call1(key_fn, x, {}) : x;
        PyRef ky = key_fn ? i.call1(key_fn, y, {}) : y;
        bool ok = false;
        int c = py_cmp(i, kx, ky, &ok);
        if (!ok)
            i.raise_exc("TypeError", "unorderable types", {});
        return rev ? c > 0 : c < 0;
    });
    return py_list(std::move(out));
}
PyRef b_reversed(interpreter& i, const py_args& a) {
    std::vector<PyRef> out;
    i.for_each(need_arg(i, a, 0, "reversed"), [&](PyRef v) {
        out.push_back(std::move(v));
        return true;
    });
    std::reverse(out.begin(), out.end());
    auto ez = std::make_shared<PyEnumZipObj>(py_kind::reversed_,
                                             py_list(std::move(out)), 0);
    return ez;
}
PyRef b_range(interpreter& i, const py_args& a) {
    // range args accept int/bool only — range("3")/range(1.5) are TypeErrors
    auto rng_int = [&](std::size_t n) -> int64_t {
        const PyRef r = a.pos[n];
        if (r != nullptr &&
            (r->kind == py_kind::integer || r->kind == py_kind::boolean))
            return r->kind == py_kind::integer
                       ? static_cast<PyIntObj*>(r.get())->v
                       : (static_cast<PyBoolObj*>(r.get())->v ? 1 : 0);
        i.raise_exc("TypeError",
                    "range() integer argument expected, got '" +
                        std::string(py_type_name(r)) + "'",
                    {});
    };
    int64_t start = 0, stop = 0, step = 1;
    if (a.pos.size() == 1) {
        stop = rng_int(0);
    } else if (a.pos.size() == 2) {
        start = rng_int(0);
        stop = rng_int(1);
    } else if (a.pos.size() >= 3) {
        start = rng_int(0);
        stop = rng_int(1);
        step = rng_int(2);
    }
    if (step == 0)
        i.raise_exc("ValueError", "range() arg 3 must not be zero", {});
    return std::make_shared<PyRangeObj>(start, stop, step);
}
PyRef b_enumerate(interpreter& i, const py_args& a) {
    PyRef src = need_arg(i, a, 0, "enumerate");
    int64_t start = arg_int(i, a, 1, 0);
    return std::make_shared<PyEnumZipObj>(py_kind::enumerate_, src, start);
}
PyRef b_zip(interpreter& i, const py_args& a) {
    std::vector<PyRef> its;
    for (const auto& p : a.pos)
        its.push_back(i.iter(p));
    if (its.empty())
        return py_list({});   // zip() → empty iterator
    // N-way: src = list of all iterators; each step yields a flat N-tuple.
    return std::make_shared<PyEnumZipObj>(
        py_kind::zip_, py_list(std::move(its)), 0);
}
PyRef b_map(interpreter& i, const py_args& a) {
    PyRef fn = need_arg(i, a, 0, "map");
    PyRef src = need_arg(i, a, 1, "map");
    return std::make_shared<PyEnumZipObj>(
        py_kind::map_, py_list({fn, i.iter(src)}), 0);
}
PyRef b_filter(interpreter& i, const py_args& a) {
    PyRef fn = a.pos.empty() ? py_none() : a.pos[0];
    PyRef src = need_arg(i, a, 1, "filter");
    return std::make_shared<PyEnumZipObj>(
        py_kind::filter_, py_list({fn, i.iter(src)}), 0);
}
PyRef b_abs(interpreter& i, const py_args& a) {
    PyRef v = need_arg(i, a, 0, "abs");
    if (auto* f = as_float(v))
        return py_float(std::fabs(f->v));
    bool ok = false;
    const int64_t n = py_to_int(v, &ok);
    if (ok)
        return py_int(n < 0 ? -n : n);
    if (v->kind == py_kind::instance)
        if (PyRef fn = i.getattr(v, "__abs__"))
            return i.call0(fn, {});
    i.raise_exc("TypeError", "bad operand type for abs()", {});
}
PyRef b_round(interpreter& i, const py_args& a) {
    PyRef v = need_arg(i, a, 0, "round");
    const int64_t ndig = arg_int(i, a, 1, 0);
    bool has_nd = a.pos.size() > 1;
    bool ok = false;
    const double d = py_to_float(v, &ok);
    if (!ok)
        i.raise_exc("TypeError", "round() arg must be number", {});
    const double scale = std::pow(10.0, static_cast<double>(ndig));
    // banker's rounding (half-to-even) like CPython round()
    const double r = std::nearbyint(d * scale) / scale;
    if (!has_nd)
        return py_int(static_cast<int64_t>(r));
    return py_float(r);
}
PyRef b_min(interpreter& i, const py_args& a) {
    std::vector<PyRef> items;
    if (a.pos.size() == 1)
        i.for_each(a.pos[0], [&](PyRef v) {
            items.push_back(v);
            return true;
        });
    else
        items = a.pos;
    if (items.empty())
        i.raise_exc("ValueError", "min() arg is an empty sequence", {});
    PyRef best = items[0];
    for (std::size_t k = 1; k < items.size(); ++k) {
        bool ok = false;
        const int c = py_cmp(i, items[k], best, &ok);
        if (!ok)
            i.raise_exc("TypeError", "unorderable types", {});
        if (c < 0)
            best = items[k];
    }
    return best;
}
PyRef b_max(interpreter& i, const py_args& a) {
    std::vector<PyRef> items;
    if (a.pos.size() == 1)
        i.for_each(a.pos[0], [&](PyRef v) {
            items.push_back(v);
            return true;
        });
    else
        items = a.pos;
    if (items.empty())
        i.raise_exc("ValueError", "max() arg is an empty sequence", {});
    PyRef best = items[0];
    for (std::size_t k = 1; k < items.size(); ++k) {
        bool ok = false;
        const int c = py_cmp(i, items[k], best, &ok);
        if (!ok)
            i.raise_exc("TypeError", "unorderable types", {});
        if (c > 0)
            best = items[k];
    }
    return best;
}
PyRef b_sum(interpreter& i, const py_args& a) {
    PyRef acc = py_int(0);
    i.for_each(need_arg(i, a, 0, "sum"), [&](PyRef v) {
        acc = i.binary(tok_kind::plus, acc, v, {});
        return true;
    });
    if (a.pos.size() > 1)
        acc = i.binary(tok_kind::plus, a.pos[1], acc, {});
    return acc;
}
PyRef b_any(interpreter& i, const py_args& a) {
    bool hit = false;
    i.for_each(need_arg(i, a, 0, "any"), [&](PyRef v) {
        if (i.truthy(v))
            hit = true;
        return !hit;
    });
    return py_bool(hit);
}
PyRef b_all(interpreter& i, const py_args& a) {
    bool all = true;
    i.for_each(need_arg(i, a, 0, "all"), [&](PyRef v) {
        if (!i.truthy(v))
            all = false;
        return all;
    });
    return py_bool(all);
}
PyRef b_ord(interpreter& i, const py_args& a) {
    const auto* s = as_str(need_arg(i, a, 0, "ord"));
    if (s && !s->v.empty()) {
        // decode the FIRST utf8 code point — ord("中") is 20013, not 0xE4
        const unsigned char c = static_cast<unsigned char>(s->v[0]);
        const std::size_t n =
            (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (n > s->v.size() || s->v.size() != n)
            i.raise_exc("TypeError", "ord() expected a character", {});
        uint32_t cp = 0;
        if (n == 1) {
            cp = c;
        } else if (n == 2) {
            cp = (static_cast<uint32_t>(c & 0x1F) << 6) |
                 (static_cast<unsigned char>(s->v[1]) & 0x3F);
        } else if (n == 3) {
            cp = (static_cast<uint32_t>(c & 0x0F) << 12) |
                 ((static_cast<unsigned char>(s->v[1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(s->v[2]) & 0x3F);
        } else {
            cp = (static_cast<uint32_t>(c & 0x07) << 18) |
                 ((static_cast<unsigned char>(s->v[1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(s->v[2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(s->v[3]) & 0x3F);
        }
        return py_int(static_cast<int64_t>(cp));
    }
    i.raise_exc("TypeError", "ord() expected a character", {});
}
PyRef b_chr(interpreter& i, const py_args& a) {
    const int64_t cp = arg_int(i, a, 0, 0);
    if (cp < 0 || cp > 0x10FFFF)
        i.raise_exc("ValueError", "chr() arg not in range", {});
    std::string out;
    const uint32_t n = static_cast<uint32_t>(cp);
    if (n < 0x80)
        out += static_cast<char>(n);
    else if (n < 0x800) {
        out += static_cast<char>(0xC0 | (n >> 6));
        out += static_cast<char>(0x80 | (n & 0x3F));
    } else if (n < 0x10000) {
        out += static_cast<char>(0xE0 | (n >> 12));
        out += static_cast<char>(0x80 | ((n >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (n & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (n >> 18));
        out += static_cast<char>(0x80 | ((n >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((n >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (n & 0x3F));
    }
    return py_str(out);
}
PyRef b_hex(interpreter& i, const py_args& a) {
    char buf[32];
    const int64_t n = arg_int(i, a, 0, 0);
    std::snprintf(buf, sizeof(buf), n < 0 ? "-0x%llx" : "0x%llx",
                  static_cast<unsigned long long>(n < 0 ? -n : n));
    return py_str(buf);
}
PyRef b_oct(interpreter& i, const py_args& a) {
    char buf[32];
    const int64_t n = arg_int(i, a, 0, 0);
    std::snprintf(buf, sizeof(buf), "0o%llo",
                  static_cast<unsigned long long>(n));
    return py_str(buf);
}
PyRef b_bin(interpreter& i, const py_args& a) {
    int64_t n = arg_int(i, a, 0, 0);
    const bool neg = n < 0;
    if (neg)
        n = -n;
    std::string bits;
    do {
        bits.insert(bits.begin(), (n & 1) ? '1' : '0');
        n >>= 1;
    } while (n);
    return py_str((neg ? "-0b" : "0b") + bits);
}
PyRef b_divmod(interpreter& i, const py_args& a) {
    bool o1 = false, o2 = false;
    const int64_t x = py_to_int(need_arg(i, a, 0, "divmod"), &o1);
    const int64_t y = py_to_int(need_arg(i, a, 1, "divmod"), &o2);
    if (!o1 || !o2)
        i.raise_exc("TypeError", "divmod() expects ints", {});
    if (y == 0)
        i.raise_exc("ZeroDivisionError", "integer division or modulo by zero",
                    {});
    int64_t q = x / y;
    if ((x % y != 0) && ((x < 0) != (y < 0)))
        --q;
    int64_t r = x % y;
    if (r != 0 && ((r < 0) != (y < 0)))
        r += y;
    return py_tuple({py_int(q), py_int(r)});
}
PyRef b_pow(interpreter& i, const py_args& a) {
    bool o1 = false, o2 = false;
    const double x = py_to_float(need_arg(i, a, 0, "pow"), &o1);
    const double y = py_to_float(need_arg(i, a, 1, "pow"), &o2);
    if (a.pos.size() > 2) {
        bool o3 = false;
        const int64_t m = py_to_int(a.pos[2], &o3);
        int64_t b = 0, e = 0;
        if (o3) {
            if (m == 0)
                i.raise_exc("ValueError",
                            "pow() 3rd argument cannot be 0", {});
            b = static_cast<int64_t>(x);
            e = static_cast<int64_t>(y);
            int64_t acc = 1 % m;
            for (int64_t k = 0; k < e; ++k)
                acc = (acc * b) % m;
            return py_int(((acc % m) + m) % m);
        }
    }
    return py_float(std::pow(x, y));
}
PyRef b_isinstance(interpreter& i, const py_args& a) {
    PyRef obj = need_arg(i, a, 0, "isinstance");
    PyRef cls = need_arg(i, a, 1, "isinstance");
    auto kind_name = [](const PyRef& o) -> const char* {
        return py_type_name(o);
    };
    auto check_one = [&](const PyRef& c) -> bool {
        // exception objects aren't class instances — match by walking the
        // builtin/user exception type chain (isinstance(e, ValueError)).
        if (obj != nullptr && obj->kind == py_kind::exception_)
            return i.exc_matches(obj, c);
        if (auto* cc = as_class(c)) {
            if (auto* in = as_inst(obj)) {
                for (const auto* mc : as_class(in->klass)->mro)
                    if (mc == cc)
                        return true;
                return false;
            }
            // builtin types as class objects: name matching
            return kind_name(obj) == cc->name;
        }
        if (auto* b = as_builtin(c))
            return kind_name(obj) == b->name;
        return false;
    };
    if (auto* t = as_tuple(cls)) {
        for (const auto& c : t->v)
            if (check_one(c))
                return py_true();
        return py_false();
    }
    (void)i;
    return py_bool(check_one(cls));
}
PyRef b_issubclass(interpreter& i, const py_args& a) {
    PyRef obj = need_arg(i, a, 0, "issubclass");
    PyRef cls = need_arg(i, a, 1, "issubclass");
    auto* oc = as_class(obj);
    if (oc == nullptr)
        return py_false();
    auto check_one = [&](const PyRef& c) -> bool {
        if (auto* cc = as_class(c)) {
            for (const auto* mc : oc->mro)
                if (mc == cc)
                    return true;
            // name-based fallback (builtin parents)
            for (const auto* mc : oc->mro)
                if (mc && mc->name == cc->name)
                    return true;
        }
        return false;
    };
    if (auto* t = as_tuple(cls)) {
        for (const auto& c : t->v)
            if (check_one(c))
                return py_true();
        return py_false();
    }
    return py_bool(check_one(cls));
}
PyRef b_type(interpreter& i, const py_args& a) {
    PyRef o = need_arg(i, a, 0, "type");
    // return class obj for instances; for builtins produce a marker class
    if (auto* in = as_inst(o))
        return in->klass;
    auto c = std::make_shared<PyClassObj>();
    c->name = py_type_name(o);
    c->attrs = py_dict();
    return c;
}
PyRef b_getattr(interpreter& i, const py_args& a) {
    PyRef o = need_arg(i, a, 0, "getattr");
    const auto* n = as_str(need_arg(i, a, 1, "getattr"));
    bool found = false;
    PyRef v = i.getattr(o, n->v, &found);
    if (!found) {
        if (a.pos.size() > 2)
            return a.pos[2];
        i.raise_exc("AttributeError", n->v, {});
    }
    return v;
}
PyRef b_setattr(interpreter& i, const py_args& a) {
    PyRef o = need_arg(i, a, 0, "setattr");
    const auto* n = as_str(need_arg(i, a, 1, "setattr"));
    if (!i.setattr(o, n->v, need_arg(i, a, 2, "setattr")))
        i.raise_exc("AttributeError", "cannot set attribute", {});
    return py_none();
}
PyRef b_hasattr(interpreter& i, const py_args& a) {
    PyRef o = need_arg(i, a, 0, "hasattr");
    const auto* n = as_str(need_arg(i, a, 1, "hasattr"));
    return py_bool(i.hasattr(o, n->v));
}
PyRef b_delattr(interpreter& i, const py_args& a) {
    PyRef o = need_arg(i, a, 0, "delattr");
    const auto* n = as_str(need_arg(i, a, 1, "delattr"));
    i.delattr(o, n->v);
    return py_none();
}
PyRef b_callable(interpreter& i, const py_args& a) {
    (void)i;
    PyRef o = need_arg(i, a, 0, "callable");
    switch (o->kind) {
    case py_kind::func:
    case py_kind::builtin:
    case py_kind::bound_method:
    case py_kind::staticmethod_:
    case py_kind::classmethod_:
    case py_kind::class_:
        return py_true();
    case py_kind::instance:
        return py_bool(i.getattr(o, "__call__") != nullptr);
    default:
        return py_false();
    }
}
PyRef b_iter(interpreter& i, const py_args& a) {
    return i.iter(need_arg(i, a, 0, "iter"));
}
PyRef b_next(interpreter& i, const py_args& a) {
    PyRef it = need_arg(i, a, 0, "next");
    // next() requires an iterator — non-iterator iterables (list/range/str)
    // would silently restart at element 0 on every call.
    switch (it->kind) {
    case py_kind::iterator:
    case py_kind::enumerate_:
    case py_kind::map_:
    case py_kind::filter_:
    case py_kind::zip_:
    case py_kind::reversed_:
    case py_kind::file_:
        break;
    case py_kind::instance:
        if (i.getattr(it, "__next__") || i.getattr(it, "__getitem__"))
            break;
        [[fallthrough]];
    default:
        i.raise_exc("TypeError",
                    "'" + std::string(py_type_name(it)) +
                        "' object is not an iterator",
                    {});
    }
    PyRef out;
    if (i.iter_next(it, &out))
        return out;
    if (a.pos.size() > 1)
        return a.pos[1];
    i.raise_exc("StopIteration", "", {});
}
PyRef b_id(interpreter& i, const py_args& a) {
    (void)i;
    PyRef o = need_arg(i, a, 0, "id");
    return py_int(static_cast<int64_t>(
        reinterpret_cast<uintptr_t>(o.get()) & 0x7FFFFFFFFFFFFFFFLL));
}
PyRef b_vars(interpreter& i, const py_args& a) {
    if (a.pos.empty()) {
        if (i.cur_frame)
            return i.cur_frame->locals;
        return py_dict();
    }
    PyRef o = a.pos[0];
    if (auto* in = as_inst(o))
        return in->attrs;
    if (auto* m = as_module(o))
        return m->dict;
    if (auto* mp = static_cast<PyModuleProxyObj*>(
            o->kind == py_kind::module_proxy ? o.get() : nullptr))
        return mp->dict;
    i.raise_exc("TypeError", "vars() argument must have __dict__", {});
}
PyRef b_dir(interpreter& i, const py_args& a) {
    std::vector<PyRef> names;
    if (a.pos.empty()) {
        if (i.cur_frame) {
            for (const auto& [k, v] : as_dict(i.cur_frame->locals)->items)
                if (auto* ks = as_str(k))
                    names.push_back(py_str(ks->v));
        }
    } else {
        PyRef o = a.pos[0];
        PyRef d;
        if (auto* in = as_inst(o))
            d = in->attrs;
        else if (auto* m = as_module(o))
            d = m->dict;
        else if (auto* c = as_class(o))
            d = c->attrs;
        if (d)
            for (const auto& [k, v] : as_dict(d)->items)
                if (auto* ks = as_str(k))
                    names.push_back(py_str(ks->v));
    }
    std::sort(names.begin(), names.end(),
              [](const PyRef& x, const PyRef& y) {
                  return as_str(x)->v < as_str(y)->v;
              });
    return py_list(std::move(names));
}
PyRef b_globals(interpreter& i, const py_args& a) {
    (void)a;
    return i.cur_frame ? i.cur_frame->globals : i.builtins_dict;
}
PyRef b_locals(interpreter& i, const py_args& a) {
    (void)a;
    return i.cur_frame ? i.cur_frame->locals : i.builtins_dict;
}
PyRef b_format(interpreter& i, const py_args& a) {
    PyRef v = need_arg(i, a, 0, "format");
    const auto* spec = a.pos.size() > 1 ? as_str(a.pos[1]) : nullptr;
    if (spec && !spec->v.empty()) {
        // route through str.__format__-lite: "">">"+width only via str.format
        return py_str(py_to_str(i, v));
    }
    return py_str(py_to_str(i, v));
}
PyRef b_input(interpreter& i, const py_args& a) {
    (void)a;
    i.raise_exc("RuntimeError", "input() is not available in pymini", {});
}
PyRef b_open(interpreter& i, const py_args& a) {
    // sandboxed open — pymini_ctx.cpp installs the real one per-plugin;
    // placeholder raises until a plugin context binds it.
    (void)a;
    i.raise_exc("RuntimeError", "open() is not bound — host ctx required",
                {});
}
PyRef b_hash(interpreter& i, const py_args& a) {
    const PyRef v = need_arg(i, a, 0, "hash");
    if (v != nullptr && v->kind == py_kind::instance) {
        if (PyRef fn = i.getattr(v, "__hash__")) {
            const PyRef h = i.call0(fn, {});
            bool ok = false;
            const int64_t n = py_to_int(h, &ok);
            if (!ok)
                i.raise_exc("TypeError", "__hash__ should return int", {});
            return py_int(n);
        }
    }
    bool ok = false;
    const int64_t h = py_hash(v, &ok);
    if (!ok)
        i.raise_exc("TypeError", "unhashable type", {});
    return py_int(h);
}
PyRef b_exit(interpreter& i, const py_args& a) {
    const int64_t code = arg_int(i, a, 0, 0);
    (void)i;
    i.raise_exc("SystemExit", std::to_string(code), {});
}

// install_exc_class — builtin exception types as PyClassObj carrying the
// `__call_exc__` marker.  interpreter::call() sees the marker anywhere in
// the mro and produces a PyExcObj with the concrete type name.
void install_exc_class(interpreter& i, const char* name,
                       const char* base_name) {
    auto c = std::make_shared<PyClassObj>();
    c->name = name;
    c->attrs = py_dict();
    dict_set(as_dict(c->attrs), py_str("__call_exc__"), py_str(name));
    // mro: self + base classes created eagerly at install time — we wire a
    // synthetic chain: script-visible mro just carries the class itself plus
    // already-installed base (if found).
    if (base_name != nullptr) {
        if (PyRef b = dict_get(as_dict(i.builtins_dict), py_str(base_name)))
            if (auto* bc = as_class(b))
                c->bases.push_back(b);
    }
    c->mro.push_back(c.get());
    dict_set(as_dict(i.builtins_dict), py_str(name), c);
}

} // namespace

void pymini_install_builtins(interpreter& i) {
    auto put = [&](const char* name, py_native_fn fn) {
        dict_set(as_dict(i.builtins_dict), py_str(name),
                 py_builtin(name, std::move(fn)));
    };
    put("len", b_len);
    put("print", b_print);
    put("str", b_str);
    put("repr", b_repr);
    put("int", b_int);
    put("float", b_float);
    put("bool", b_bool);
    put("bytes", b_bytes);
    put("bytearray", b_bytearray);
    put("list", b_list);
    put("tuple", b_tuple);
    put("set", b_set);
    put("frozenset", b_frozenset);
    put("dict", b_dict);
    put("sorted", b_sorted);
    put("reversed", b_reversed);
    put("range", b_range);
    put("enumerate", b_enumerate);
    put("zip", b_zip);
    put("map", b_map);
    put("filter", b_filter);
    put("abs", b_abs);
    put("round", b_round);
    put("min", b_min);
    put("max", b_max);
    put("sum", b_sum);
    put("any", b_any);
    put("all", b_all);
    put("ord", b_ord);
    put("chr", b_chr);
    put("hex", b_hex);
    put("oct", b_oct);
    put("bin", b_bin);
    put("divmod", b_divmod);
    put("pow", b_pow);
    put("isinstance", b_isinstance);
    put("issubclass", b_issubclass);
    put("type", b_type);
    put("getattr", b_getattr);
    put("setattr", b_setattr);
    put("hasattr", b_hasattr);
    put("delattr", b_delattr);
    put("callable", b_callable);
    put("iter", b_iter);
    put("next", b_next);
    put("id", b_id);
    put("vars", b_vars);
    put("dir", b_dir);
    put("globals", b_globals);
    put("locals", b_locals);
    put("format", b_format);
    put("input", b_input);
    put("open", b_open);          // replaced per-plugin sandbox
    put("hash", b_hash);
    put("exit", b_exit);
    put("quit", b_exit);
    // constants
    dict_set(as_dict(i.builtins_dict), py_str("True"), py_true());
    dict_set(as_dict(i.builtins_dict), py_str("False"), py_false());
    dict_set(as_dict(i.builtins_dict), py_str("None"), py_none());
    dict_set(as_dict(i.builtins_dict), py_str("Ellipsis"), py_none());
    dict_set(as_dict(i.builtins_dict), py_str("NotImplemented"), py_none());
    // staticmethod/classmethod/property decorators as builtins
    dict_set(as_dict(i.builtins_dict), py_str("staticmethod"),
             py_builtin("staticmethod", [](interpreter&, const py_args& a) {
                 return std::make_shared<PyStaticObj>(a.pos.empty()
                                                          ? PyRef{}
                                                          : a.pos[0]);
             }));
    dict_set(as_dict(i.builtins_dict), py_str("classmethod"),
             py_builtin("classmethod", [](interpreter&, const py_args& a) {
                 return std::make_shared<PyClassMethodObj>(a.pos.empty()
                                                               ? PyRef{}
                                                               : a.pos[0]);
             }));
    dict_set(as_dict(i.builtins_dict), py_str("property"),
             py_builtin("property", [](interpreter&, const py_args& a) {
                 PyRef g = a.pos.size() > 0 ? a.pos[0] : PyRef{};
                 PyRef s = a.pos.size() > 1 ? a.pos[1] : PyRef{};
                 PyRef d = a.pos.size() > 2 ? a.pos[2] : PyRef{};
                 return std::make_shared<PyPropertyObj>(g, s, d);
             }));
    dict_set(as_dict(i.builtins_dict), py_str("super"),
             py_builtin("super", [](interpreter& i2, const py_args& a) {
                 PyRef kls = a.pos.size() > 0 ? a.pos[0] : PyRef{};
                 PyRef self = a.pos.size() > 1 ? a.pos[1] : PyRef{};
                 if (!kls || !self) {
                     // 0-arg super() inside method — bind caller class/self.
                     // `__class__` lives in the captured closure chain (the
                     // cell injected at class creation), not in locals.
                     if (i2.cur_frame) {
                         PyRef s, k;
                         auto scan = [&](const PyRef& dictref) {
                             auto* d = as_dict(dictref);
                             if (d == nullptr)
                                 return;
                             if (!s) {
                                 if (!(s = dict_get(d, py_str("self"))))
                                     s = dict_get(d, py_str("cls"));
                             }
                             if (!k)
                                 k = dict_get(d, py_str("__class__"));
                         };
                         scan(i2.cur_frame->locals);
                         for (const auto& cap : i2.cur_frame->captured)
                             scan(cap);
                         if (!k && s)
                             k = s->kind == py_kind::instance
                                     ? as_inst(s)->klass
                                     : s;
                         if (k && s)
                             return std::make_shared<PySuperObj>(
                                 k, s);
                     }
                     i2.raise_exc("RuntimeError",
                                  "super(): no arguments resolvable", {});
                 }
                 return std::make_shared<PySuperObj>(kls, self);
             }));
    // object base class sentinel
    {
        auto obj_cls = std::make_shared<PyClassObj>();
        obj_cls->name = "object";
        obj_cls->attrs = py_dict();
        obj_cls->mro.push_back(obj_cls.get());
        dict_set(as_dict(i.builtins_dict), py_str("object"), obj_cls);
    }
    // builtin exception class objects
    static const char* exc_table[][2] = {
        {"BaseException", nullptr},       {"Exception", "BaseException"},
        {"ArithmeticError", "Exception"}, {"ZeroDivisionError", "ArithmeticError"},
        {"OverflowError", "ArithmeticError"},
        {"AssertionError", "Exception"},  {"AttributeError", "Exception"},
        {"BufferError", "Exception"},     {"EOFError", "Exception"},
        {"ImportError", "Exception"},     {"ModuleNotFoundError", "ImportError"},
        {"LookupError", "Exception"},     {"IndexError", "LookupError"},
        {"KeyError", "LookupError"},
        {"KeyboardInterrupt", "BaseException"},
        {"MemoryError", "Exception"},     {"NameError", "Exception"},
        {"UnboundLocalError", "NameError"},
        {"OSError", "Exception"},         {"IOError", "OSError"},
        {"FileNotFoundError", "OSError"}, {"PermissionError", "OSError"},
        {"IsADirectoryError", "OSError"}, {"NotADirectoryError", "OSError"},
        {"TimeoutError", "OSError"},      {"FileExistsError", "OSError"},
        {"RuntimeError", "Exception"},    {"NotImplementedError", "RuntimeError"},
        {"RecursionError", "RuntimeError"},
        {"StopIteration", "Exception"},   {"StopAsyncIteration", "Exception"},
        {"SyntaxError", "Exception"},     {"IndentationError", "SyntaxError"},
        {"TabError", "IndentationError"},
        {"SystemExit", "BaseException"},  {"TypeError", "Exception"},
        {"ValueError", "Exception"},      {"UnicodeError", "ValueError"},
        {"UnicodeDecodeError", "UnicodeError"},
        {"UnicodeEncodeError", "UnicodeError"},
        {"GeneratorExit", "BaseException"},
        {"UnsupportedSyntax", "Exception"},
        {"Warning", "Exception"},         {"UserWarning", "Warning"},
        {"DeprecationWarning", "Warning"},{"PendingDeprecationWarning", "Warning"},
        {"RuntimeWarning", "Warning"},    {"SyntaxWarning", "Warning"},
        {"ResourceWarning", "Warning"},   {"FutureWarning", "Warning"},
        {"ImportWarning", "Warning"},     {"UnicodeWarning", "Warning"},
        {"BytesWarning", "Warning"},
    };
    for (const auto& e : exc_table)
        install_exc_class(i, e[0], e[1]);
}

} // namespace sao::plugins::pymini
