// csmini_value.cpp — singletons, factories, containers + coercions.
#include "csmini_value.h"

#include <cmath>
#include <cstdio>

namespace sao::plugins::csmini {

// ── singletons ────────────────────────────────────────────────────────────
CsRef cs_null() {
    static CsRef v = std::make_shared<CsNullObj>();
    return v;
}
CsRef cs_true() {
    static CsRef v = std::make_shared<CsBoolObj>(true);
    return v;
}
CsRef cs_false() {
    static CsRef v = std::make_shared<CsBoolObj>(false);
    return v;
}
CsRef cs_bool(bool v) { return v ? cs_true() : cs_false(); }
CsRef cs_int(int64_t v) { return std::make_shared<CsIntObj>(v); }
CsRef cs_float(double v) { return std::make_shared<CsFloatObj>(v); }
CsRef cs_str(std::string_view v) { return std::make_shared<CsStrObj>(std::string(v)); }
CsRef cs_str(const char* v) { return std::make_shared<CsStrObj>(std::string(v ? v : "")); }
CsRef cs_char(int64_t v) { return std::make_shared<CsCharObj>(v); }
CsRef cs_array(std::vector<CsRef> v) {
    return std::make_shared<CsArrayObj>(std::move(v),
                                        cs_array_semantics::sequence);
}
CsRef cs_set(std::vector<CsRef> v) {
    auto out = std::make_shared<CsArrayObj>(cs_array_semantics::set);
    out->v.reserve(v.size());
    for (CsRef& value : v)
        (void)cs_set_add(out.get(), std::move(value));
    return out;
}
CsRef cs_dict() { return std::make_shared<CsDictObj>(); }
CsRef cs_guid(const std::array<uint8_t, 16>& bytes) {
    return std::make_shared<CsGuidObj>(bytes);
}
CsRef cs_builtin(std::string name, cs_native_fn fn) {
    return std::make_shared<CsBuiltinObj>(std::move(name), std::move(fn));
}
CsRef cs_exc(const std::string& type, const std::string& msg) {
    return std::make_shared<CsExcObj>(type, msg);
}
CsRef cs_native(std::string name, bool ci) {
    return std::make_shared<CsNativeObj>(std::move(name), cs_dict(), ci);
}

bool cs_is_null(const CsRef& r) { return !r || r->kind == cs_kind::null_; }

bool cs_truthy(const CsRef& r) {
    if (!r || r->kind == cs_kind::null_)
        return false;
    if (auto* b = as_bool(r))
        return b->v;
    // C#-strict: truthiness is only defined for bool; the interpreter still
    // needs a verdict for `if (x)` on non-bool — null-check, containers count
    // as "present".
    return true;
}

int64_t cs_to_int(const CsRef& r, bool* ok) {
    if (ok)
        *ok = true;
    if (auto* i = as_int(r))
        return i->v;
    if (auto* f = as_float(r))
        return static_cast<int64_t>(f->v);
    if (auto* b = as_bool(r))
        return b->v ? 1 : 0;
    if (auto* c = as_char(r))
        return c->v;
    if (auto* s = as_str(r)) {
        try {
            std::size_t used = 0;
            const int64_t v = std::stoll(s->v, &used, 0);
            if (used == s->v.size())
                return v;
        } catch (...) {
        }
    }
    if (ok)
        *ok = false;
    return 0;
}

double cs_to_float(const CsRef& r, bool* ok) {
    if (ok)
        *ok = true;
    if (auto* f = as_float(r))
        return f->v;
    if (auto* i = as_int(r))
        return static_cast<double>(i->v);
    if (auto* c = as_char(r))
        return static_cast<double>(c->v);
    if (auto* b = as_bool(r))
        return b->v ? 1.0 : 0.0;
    if (auto* s = as_str(r)) {
        try {
            std::size_t used = 0;
            const double v = std::stod(s->v, &used);
            if (used == s->v.size())
                return v;
        } catch (...) {
        }
    }
    if (ok)
        *ok = false;
    return 0.0;
}

static void utf8_append(std::string& out, int64_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string cs_to_str(interpreter&, const CsRef& r) {
    if (!r || r->kind == cs_kind::null_)
        return "";
    switch (r->kind) {
    case cs_kind::boolean:
        return as_bool(r)->v ? "True" : "False";   // C# ToString casing
    case cs_kind::integer:
        return std::to_string(as_int(r)->v);
    case cs_kind::number: {
        const double v = as_float(r)->v;
        if (v == static_cast<int64_t>(v) && std::fabs(v) < 1e15) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%lld.0", static_cast<long long>(v));
            return buf;                            // double always shows .0
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.15g", v);
        return buf;
    }
    case cs_kind::char_: {
        std::string out;
        utf8_append(out, as_char(r)->v);
        return out;
    }
    case cs_kind::string:
        return as_str(r)->v;
    case cs_kind::array: {
        std::string out = "[";
        bool first = true;
        for (const auto& x : as_array(r)->v) {
            if (!first)
                out += ", ";
            first = false;
            if (auto* s = as_str(x)) {
                out += '"';
                out += s->v;
                out += '"';
            } else {
                out += x ? "..." : "null";
            }
        }
        return out + "]";
    }
    case cs_kind::dict: {
        std::string out = "{";
        bool first = true;
        for (const auto& [k, x] : as_dict(r)->items) {
            if (!first)
                out += ", ";
            first = false;
            if (auto* s = as_str(k)) {
                out += '"';
                out += s->v;
                out += '"';
            } else {
                out += "<key>";
            }
            out += ": ";
            if (auto* s = as_str(x)) {
                out += '"';
                out += s->v;
                out += '"';
            } else {
                out += x ? "..." : "null";
            }
        }
        return out + "}";
    }
    case cs_kind::guid:
        return cs_guid_format(as_guid(r)->bytes);
    case cs_kind::exception_: {
        auto* e = as_exc(r);
        return e->type_name + (e->message.empty() ? "" : ": " + e->message);
    }
    case cs_kind::func:
        return "<method " + as_func(r)->name + ">";
    case cs_kind::builtin:
        return "<native " + as_builtin(r)->name + ">";
    case cs_kind::bound_method:
        return "<bound method>";
    case cs_kind::property:
        return "<property " + as_property(r)->name + ">";
    case cs_kind::class_:
        return as_class(r)->name;
    case cs_kind::instance:
        return as_class(as_inst(r)->klass)->name;
    case cs_kind::native_obj:
        return as_native(r)->name;
    }
    return "<value>";
}

const char* cs_type_name(const CsRef& r) {
    if (!r || r->kind == cs_kind::null_)
        return "object";
    switch (r->kind) {
    case cs_kind::boolean: return "bool";
    case cs_kind::integer: return "long";
    case cs_kind::number: return "double";
    case cs_kind::char_: return "char";
    case cs_kind::string: return "string";
    case cs_kind::array: return as_array(r)->is_set() ? "HashSet" : "array";
    case cs_kind::dict: return "Dictionary";
    case cs_kind::guid: return "Guid";
    case cs_kind::exception_: return "Exception";
    case cs_kind::func:
    case cs_kind::builtin:
    case cs_kind::bound_method: return "method";
    case cs_kind::property: return "property";
    case cs_kind::class_: return "class";
    case cs_kind::instance: return "object";
    case cs_kind::native_obj: return "native";
    }
    return "object";
}

// ── dict helpers ──────────────────────────────────────────────────────────
bool cs_eq(const CsRef& a, const CsRef& b) {
    if (a.get() == b.get())
        return true;
    if (!a || !b)
        return false;
    const bool an = a->kind == cs_kind::integer || a->kind == cs_kind::number ||
                    a->kind == cs_kind::char_ || a->kind == cs_kind::boolean;
    const bool bn = b->kind == cs_kind::integer || b->kind == cs_kind::number ||
                    b->kind == cs_kind::char_ || b->kind == cs_kind::boolean;
    if (an && bn)
        return cs_to_float(a, nullptr) == cs_to_float(b, nullptr);
    if (a->kind != b->kind)
        return false;
    switch (a->kind) {
    case cs_kind::string:
        return as_str(a)->v == as_str(b)->v;
    case cs_kind::guid:
        return as_guid(a)->bytes == as_guid(b)->bytes;
    case cs_kind::dict:
    case cs_kind::array:
        return a.get() == b.get();
    default:
        return a.get() == b.get();
    }
}

bool cs_sequence_contains(const CsArrayObj* sequence, const CsRef& value) {
    if (!sequence)
        return false;
    for (const CsRef& current : sequence->v)
        if (cs_eq(current, value))
            return true;
    return false;
}

bool cs_set_add(CsArrayObj* set, CsRef value) {
    if (!set || !set->is_set() || cs_sequence_contains(set, value))
        return false;
    set->v.push_back(std::move(value));
    return true;
}

namespace {

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

} // namespace

bool cs_guid_parse(std::string_view text, std::array<uint8_t, 16>* out) {
    if (!out)
        return false;
    if ((text.size() == 38 && text.front() == '{' && text.back() == '}') ||
        (text.size() == 38 && text.front() == '(' && text.back() == ')'))
        text = text.substr(1, text.size() - 2);
    if (text.size() != 32 && text.size() != 36)
        return false;
    if (text.size() == 36) {
        for (std::size_t index = 0; index < text.size(); ++index) {
            const bool separator = index == 8 || index == 13 || index == 18 ||
                                   index == 23;
            if ((text[index] == '-') != separator)
                return false;
        }
    } else if (text.find('-') != std::string_view::npos) {
        return false;
    }
    std::array<uint8_t, 16> value{};
    std::size_t nibble = 0;
    int high = -1;
    for (char ch : text) {
        if (ch == '-')
            continue;
        const int digit = hex_digit(ch);
        if (digit < 0 || nibble >= 32)
            return false;
        if (high < 0) {
            high = digit;
        } else {
            value[nibble / 2] = static_cast<uint8_t>((high << 4) | digit);
            high = -1;
        }
        ++nibble;
    }
    if (nibble != 32 || high >= 0)
        return false;
    *out = value;
    return true;
}

std::string cs_guid_format(const std::array<uint8_t, 16>& bytes,
                           std::string_view format) {
    char kind = format.empty() ? 'D' : format.front();
    if (kind >= 'a' && kind <= 'z')
        kind = static_cast<char>(kind - ('a' - 'A'));
    const bool dashed = kind != 'N';
    const bool braced = kind == 'B';
    const bool parenthesized = kind == 'P';
    if (kind != 'D' && kind != 'N' && kind != 'B' && kind != 'P')
        return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(dashed ? 38 : 32);
    if (braced)
        out.push_back('{');
    else if (parenthesized)
        out.push_back('(');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (dashed && (index == 4 || index == 6 || index == 8 || index == 10))
            out.push_back('-');
        out.push_back(hex[(bytes[index] >> 4) & 0x0f]);
        out.push_back(hex[bytes[index] & 0x0f]);
    }
    if (braced)
        out.push_back('}');
    else if (parenthesized)
        out.push_back(')');
    return out;
}

CsRef dict_get(const CsDictObj* d, const CsRef& key) {
    if (!d)
        return nullptr;
    for (const auto& [k, v] : d->items)
        if (cs_eq(k, key))
            return v;
    return nullptr;
}

bool dict_set(CsDictObj* d, CsRef key, CsRef value) {
    if (!d)
        return false;
    for (auto& [k, v] : d->items)
        if (cs_eq(k, key)) {
            v = std::move(value);
            return true;
        }
    d->items.emplace_back(std::move(key), std::move(value));
    return true;
}

bool dict_del(CsDictObj* d, const CsRef& key) {
    if (!d)
        return false;
    for (auto it = d->items.begin(); it != d->items.end(); ++it)
        if (cs_eq(it->first, key)) {
            d->items.erase(it);
            return true;
        }
    return false;
}

static bool ascii_ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = static_cast<char>(y + 32);
        if (x != y)
            return false;
    }
    return true;
}

CsRef dict_get_ci(const CsDictObj* d, const CsRef& key) {
    if (!d)
        return nullptr;
    if (CsRef exact = dict_get(d, key))
        return exact;
    auto* ks = as_str(key);
    if (!ks)
        return nullptr;
    for (const auto& [k, v] : d->items)
        if (auto* kk = as_str(k); kk && ascii_ieq(kk->v, ks->v))
            return v;
    return nullptr;
}

// ── cs_guard impl (needs interpreter's recursive_mutex) ──────────────────
// defined here to keep csmini_common.h free of interpreter internals —
// implemented in csmini_interp.cpp where `interpreter` is complete.
} // namespace sao::plugins::csmini
