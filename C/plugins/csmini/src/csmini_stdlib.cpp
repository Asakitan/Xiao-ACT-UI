// csmini_stdlib.cpp — instance member tables for value kinds: string.*,
// array/List<T>.*, Dictionary<K,V>.*, primitives (ToString etc.),
// KeyValuePair member access, exception members.
//
// Members resolve lazily — each `obj.Name` / `obj.Method` hit looks up a
// per-kind table and returns either a live value (Count/Length) or a
// cs_builtin bound to the receiver.
#include "csmini_interp.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace sao::plugins::csmini {
namespace {

CsRef take(const cs_args& a, std::size_t k) {
    return k < a.size() ? a.pos[k] : cs_null();
}
[[noreturn]] void arg_err(interpreter& i, const char* what) {
    i.raise_exc("ArgumentException", what);
}
int64_t to_i(interpreter& i, const CsRef& v) {
    bool ok = false;
    const int64_t n = cs_to_int(v, &ok);
    if (!ok)
        arg_err(i, "expected int");
    return n;
}
std::string to_s(interpreter& i, const CsRef& v) { return cs_to_str(i, v); }

// string repeat helpers for PadLeft/Right
std::string repeat(std::string s, int64_t n) {
    if (n <= 0)
        return "";
    std::string out;
    out.reserve(s.size() * static_cast<std::size_t>(n));
    for (int64_t k = 0; k < n; ++k)
        out += s;
    return out;
}

// ── string instance methods ────────────────────────────────────────────────
using member_fn = std::function<CsRef(interpreter&, CsRef self,
                                      const cs_args&)>;

CsRef str_member(const std::string& name, CsRef self) {
    auto* s = as_str(self);
    if (!s)
        return nullptr;
    const std::string& v = s->v;
    if (name == "Length")
        return cs_int((int64_t)v.size());
    if (name == "Chars") {
        // indexer-ish native accessor `str.Chars(i)`
        return cs_builtin("String.Chars", [self](interpreter& i,
                                                 const cs_args& a) {
            auto* sv = as_str(self);
            const int64_t k = to_i(i, take(a, 0));
            if (k < 0 || static_cast<std::size_t>(k) >= sv->v.size())
                i.raise_exc("IndexOutOfRangeException", "Chars");
            return cs_char((unsigned char)sv->v[static_cast<std::size_t>(k)]);
        });
    }
    static const std::unordered_map<std::string, member_fn> fns = {
        {"Substring",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             const std::string& v = as_str(self)->v;
             const int64_t start = to_i(i, take(a, 0));
             if (a.size() > 1) {
                 const int64_t len = to_i(i, take(a, 1));
                 if (start < 0 || len < 0 ||
                     static_cast<std::size_t>(start + len) > v.size())
                     i.raise_exc("ArgumentOutOfRangeException",
                                 "Substring");
                 return cs_str(v.substr((std::size_t)start,
                                        (std::size_t)len));
             }
             if (start < 0 || static_cast<std::size_t>(start) > v.size())
                 i.raise_exc("ArgumentOutOfRangeException", "Substring");
             return cs_str(v.substr((std::size_t)start));
         }},
        {"IndexOf",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             const std::string& v = as_str(self)->v;
             const std::string needle = to_s(i, take(a, 0));
             std::size_t start = a.size() > 1 ? (std::size_t)to_i(i, take(a, 1))
                                            : 0;
             const auto k = v.find(needle, start);
             return cs_int(k == std::string::npos ? -1 : (int64_t)k);
         }},
        {"LastIndexOf",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             const std::string& v = as_str(self)->v;
             const auto k = v.rfind(to_s(i, take(a, 0)));
             return cs_int(k == std::string::npos ? -1 : (int64_t)k);
         }},
        {"StartsWith",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             const std::string& v = as_str(self)->v;
             const std::string p = to_s(i, take(a, 0));
             return cs_bool(v.rfind(p, 0) == 0);
         }},
        {"EndsWith",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             const std::string& v = as_str(self)->v;
             const std::string p = to_s(i, take(a, 0));
             return cs_bool(v.size() >= p.size() &&
                            v.compare(v.size() - p.size(), p.size(), p) == 0);
         }},
        {"Contains",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             return cs_bool(as_str(self)->v.find(to_s(i, take(a, 0))) !=
                            std::string::npos);
         }},
        {"Replace",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             std::string v = as_str(self)->v;
             const std::string from = to_s(i, take(a, 0));
             const std::string to = to_s(i, take(a, 1));
             if (from.empty())
                 return cs_str(v);
             std::size_t p = 0;
             while ((p = v.find(from, p)) != std::string::npos) {
                 v.replace(p, from.size(), to);
                 p += to.size();
             }
             return cs_str(v);
         }},
        {"Split",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             const std::string& v = as_str(self)->v;
             // no-arg Split() defaults to whitespace separators only
             // (C# behavior: char[] whitespace, not punctuation).
             std::string seps = " \t\n\r\v\f";
             CsRef arg = take(a, 0);
             if (!cs_is_null(arg))
                 seps = to_s(i, arg);
             if (seps.empty())
                 seps = " ";
             auto out = cs_array();
             std::size_t pos = 0;
             while (pos <= v.size()) {
                 std::size_t k = v.find_first_of(seps, pos);
                 if (k == std::string::npos) {
                     as_array(out)->v.push_back(cs_str(v.substr(pos)));
                     break;
                 }
                 as_array(out)->v.push_back(cs_str(v.substr(pos, k - pos)));
                 pos = k + 1;
             }
             return out;
         }},
        {"ToUpper",
         [](interpreter& i, CsRef self, const cs_args&) -> CsRef {
             std::string v = as_str(self)->v;
             for (auto& c : v)
                 c = (char)std::toupper((unsigned char)c);
             return cs_str(v);
         }},
        {"ToLower",
         [](interpreter& i, CsRef self, const cs_args&) -> CsRef {
             std::string v = as_str(self)->v;
             for (auto& c : v)
                 c = (char)std::tolower((unsigned char)c);
             return cs_str(v);
         }},
        {"Trim",
         [](interpreter& i, CsRef self, const cs_args&) -> CsRef {
             std::string v = as_str(self)->v;
             auto not_ws = [](char c) {
                 return !std::isspace((unsigned char)c);
             };
             const auto b = std::find_if(v.begin(), v.end(), not_ws);
             const auto e = std::find_if(v.rbegin(), v.rend(), not_ws).base();
             return cs_str(b < e ? std::string(b, e) : "");
         }},
        {"TrimStart",
         [](interpreter& i, CsRef self, const cs_args&) -> CsRef {
             std::string v = as_str(self)->v;
             const auto b = std::find_if(v.begin(), v.end(), [](char c) {
                 return !std::isspace((unsigned char)c);
             });
             return cs_str(std::string(b, v.end()));
         }},
        {"TrimEnd",
         [](interpreter& i, CsRef self, const cs_args&) -> CsRef {
             std::string v = as_str(self)->v;
             const auto e = std::find_if(v.rbegin(), v.rend(), [](char c) {
                                return !std::isspace((unsigned char)c);
                            }).base();
             return cs_str(std::string(v.begin(), e));
         }},
        {"PadLeft",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             std::string v = as_str(self)->v;
             const int64_t w = to_i(i, take(a, 0));
             std::string pad = a.size() > 1 ? to_s(i, take(a, 1)) : " ";
             if ((int64_t)v.size() >= w)
                 return cs_str(v);
             return cs_str(repeat(pad, w - (int64_t)v.size()) + v);
         }},
        {"PadRight",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             std::string v = as_str(self)->v;
             const int64_t w = to_i(i, take(a, 0));
             std::string pad = a.size() > 1 ? to_s(i, take(a, 1)) : " ";
             if ((int64_t)v.size() >= w)
                 return cs_str(v);
             return cs_str(v + repeat(pad, w - (int64_t)v.size()));
         }},
        {"Insert",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             std::string v = as_str(self)->v;
             const int64_t k = to_i(i, take(a, 0));
             if (k < 0 || (std::size_t)k > v.size())
                 i.raise_exc("ArgumentOutOfRangeException", "Insert");
             v.insert((std::size_t)k, to_s(i, take(a, 1)));
             return cs_str(v);
         }},
        {"Remove",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             std::string v = as_str(self)->v;
             const int64_t k = to_i(i, take(a, 0));
             if (k < 0 || (std::size_t)k > v.size())
                 i.raise_exc("ArgumentOutOfRangeException", "Remove");
             if (a.size() > 1) {
                 const int64_t len = to_i(i, take(a, 1));
                 if (k + len > (int64_t)v.size())
                     i.raise_exc("ArgumentOutOfRangeException", "Remove");
                 v.erase((std::size_t)k, (std::size_t)len);
             } else {
                 v.erase((std::size_t)k);
             }
             return cs_str(v);
         }},
        {"ToCharArray",
         [](interpreter& i, CsRef self, const cs_args&) -> CsRef {
             auto out = cs_array();
             for (char c : as_str(self)->v)
                 as_array(out)->v.push_back(cs_char((unsigned char)c));
             return out;
         }},
        {"GetHashCode",
         [](interpreter& i, CsRef self, const cs_args&) -> CsRef {
             return cs_int((int64_t)std::hash<std::string>{}(as_str(self)->v));
         }},
        {"Equals",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             return cs_bool(cs_eq(self, take(a, 0)));
         }},
        {"CompareTo",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             auto* o = as_str(take(a, 0));
             return cs_int(o ? (int64_t)as_str(self)->v.compare(o->v) : 1);
         }},
    };
    if (auto it = fns.find(name); it != fns.end())
        return cs_builtin("String." + name,
                          [self, fn = it->second](interpreter& i,
                                                  const cs_args& a) {
                              return fn(i, self, a);
                          });
    // case-insensitive alias sets: ToUpperInvariant → ToUpper…
    if (name == "ToUpperInvariant")
        return str_member("ToUpper", self);
    if (name == "ToLowerInvariant")
        return str_member("ToLower", self);
    return nullptr;
}

// ── array/List<T> members ──────────────────────────────────────────────────
CsRef arr_member(const std::string& name, CsRef self) {
    auto* arr = as_array(self);
    if (!arr)
        return nullptr;
    if (name == "Count" || name == "Length")
        return cs_int((int64_t)arr->v.size());
    if (name == "Capacity")
        return cs_int((int64_t)arr->v.capacity());
    static const std::unordered_map<std::string, member_fn> fns = {
        {"Add",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             as_array(self)->v.push_back(take(a, 0));
             return cs_null();
         }},
        {"AddRange",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             auto* dst = as_array(self);
             if (auto* src = as_array(take(a, 0)))
                 dst->v.insert(dst->v.end(), src->v.begin(), src->v.end());
             return cs_null();
         }},
        {"Remove",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             auto* v = &as_array(self)->v;
             CsRef x = take(a, 0);
             for (auto it = v->begin(); it != v->end(); ++it) {
                 if (cs_eq(*it, x)) {
                     v->erase(it);
                     return cs_true();
                 }
             }
             return cs_false();
         }},
        {"RemoveAt",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             auto* v = &as_array(self)->v;
             const int64_t k = to_i(i, take(a, 0));
             if (k < 0 || (std::size_t)k >= v->size())
                 i.raise_exc("ArgumentOutOfRangeException", "RemoveAt");
             v->erase(v->begin() + k);
             return cs_null();
         }},
        {"Insert",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             auto* v = &as_array(self)->v;
             const int64_t k = to_i(i, take(a, 0));
             if (k < 0 || (std::size_t)k > v->size())
                 i.raise_exc("ArgumentOutOfRangeException", "Insert");
             v->insert(v->begin() + k, take(a, 1));
             return cs_null();
         }},
        {"Clear",
         [](interpreter&, CsRef self, const cs_args&) -> CsRef {
             as_array(self)->v.clear();
             return cs_null();
         }},
        {"Contains",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             for (const CsRef& x : as_array(self)->v)
                 if (cs_eq(x, take(a, 0)))
                     return cs_true();
             return cs_false();
         }},
        {"IndexOf",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             const auto& v = as_array(self)->v;
             for (std::size_t k = 0; k < v.size(); ++k)
                 if (cs_eq(v[k], take(a, 0)))
                     return cs_int((int64_t)k);
             return cs_int(-1);
         }},
        {"Sort",
         [](interpreter& i, CsRef self, const cs_args&) -> CsRef {
             auto* v = &as_array(self)->v;
             std::sort(v->begin(), v->end(), [&i](const CsRef& a,
                                                  const CsRef& b) {
                 // numeric first, then string compare
                 if (a && b && a->kind == cs_kind::integer &&
                     b->kind == cs_kind::integer)
                     return as_int(a)->v < as_int(b)->v;
                 if (a && b && a->kind == cs_kind::string &&
                     b->kind == cs_kind::string)
                     return as_str(a)->v < as_str(b)->v;
                 return cs_to_str(i, a) < cs_to_str(i, b);
             });
             return cs_null();
         }},
        {"Reverse",
         [](interpreter&, CsRef self, const cs_args&) -> CsRef {
             auto* v = &as_array(self)->v;
             std::reverse(v->begin(), v->end());
             return cs_null();
         }},
        {"ToArray",
         [](interpreter&, CsRef self, const cs_args&) -> CsRef { return self; }},
        {"Exists",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             // Exists(pred) — subset: takes a truthy-checking callable
             CsRef pred = take(a, 0);
             if (!pred)
                 return cs_false();
             for (const CsRef& x : as_array(self)->v) {
                 if (cs_truthy(i.call1(pred, x)))
                     return cs_true();
             }
             return cs_false();
         }},
        {"Find",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             CsRef pred = take(a, 0);
             if (!pred)
                 return cs_null();
             for (const CsRef& x : as_array(self)->v) {
                 if (cs_truthy(i.call1(pred, x)))
                     return x;
             }
             return cs_null();
         }},
        {"FindAll",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             CsRef pred = take(a, 0);
             auto out = cs_array();
             if (!pred)
                 return out;
             for (const CsRef& x : as_array(self)->v)
                 if (cs_truthy(i.call1(pred, x)))
                     as_array(out)->v.push_back(x);
             return out;
         }},
        {"ForEach",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             CsRef fn = take(a, 0);
             if (!fn)
                 return cs_null();
             for (const CsRef& x : as_array(self)->v)
                 (void)i.call1(fn, x);
             return cs_null();
         }},
        {"GetRange",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             auto* v = &as_array(self)->v;
             const int64_t start = to_i(i, take(a, 0));
             const int64_t count = to_i(i, take(a, 1));
             if (start < 0 || count < 0 ||
                 start + count > (int64_t)v->size())
                 i.raise_exc("ArgumentOutOfRangeException", "GetRange");
             auto out = cs_array();
             as_array(out)->v.assign(v->begin() + start,
                           v->begin() + start + count);
             return out;
         }},
    };
    if (auto it = fns.find(name); it != fns.end())
        return cs_builtin("List." + name,
                          [self, fn = it->second](interpreter& i,
                                                  const cs_args& a) {
                              return fn(i, self, a);
                          });
    return nullptr;
}

// ── dict members ───────────────────────────────────────────────────────────
CsRef dict_member(const std::string& name, CsRef self) {
    auto* d = as_dict(self);
    if (!d)
        return nullptr;
    if (name == "Count")
        return cs_int((int64_t)d->items.size());
    if (name == "Keys") {
        auto out = cs_array();
        for (const auto& [k, v] : d->items)
            as_array(out)->v.push_back(k);
        return out;
    }
    if (name == "Values") {
        auto out = cs_array();
        for (const auto& [k, v] : d->items)
            as_array(out)->v.push_back(v);
        return out;
    }
    static const std::unordered_map<std::string, member_fn> fns = {
        {"Add",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             dict_set(as_dict(self), take(a, 0), take(a, 1));
             return cs_null();
         }},
        {"ContainsKey",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             return cs_bool(dict_get(as_dict(self), take(a, 0)) != nullptr);
         }},
        {"ContainsValue",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             for (const auto& [k, v] : as_dict(self)->items)
                 if (cs_eq(v, take(a, 0)))
                     return cs_true();
             return cs_false();
         }},
        {"TryGetValue",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             // out-param unsupported — deviation: returns value-or-null
             CsRef v = dict_get(as_dict(self), take(a, 0));
             if (a.size() > 1)
                 i.record_feature("out_param");
             return v ? v : cs_null();
         }},
        {"Remove",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             return cs_bool(dict_del(as_dict(self), take(a, 0)));
         }},
        {"Clear",
         [](interpreter&, CsRef self, const cs_args&) -> CsRef {
             as_dict(self)->items.clear();
             return cs_null();
         }},
        {"GetValueOrDefault",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             CsRef v = dict_get(as_dict(self), take(a, 0));
             return v ? v : cs_null();
         }},
    };
    if (auto it = fns.find(name); it != fns.end())
        return cs_builtin("Dict." + name,
                          [self, fn = it->second](interpreter& i,
                                                  const cs_args& a) {
                              return fn(i, self, a);
                          });
    return nullptr;
}

// ── char / numeric / bool ToString-style members ───────────────────────────
CsRef prim_member(const std::string& name, CsRef self) {
    if (name == "ToString")
        return cs_builtin("prim.ToString",
                          [self](interpreter& i, const cs_args&) {
                              return cs_str(cs_to_str(i, self));
                          });
    if (name == "GetHashCode")
        return cs_builtin("prim.GetHashCode",
                          [self](interpreter& i, const cs_args&) {
                              return cs_int((int64_t)reinterpret_cast<
                                            uintptr_t>(self.get()) /
                                            16);
                          });
    if (name == "Equals")
        return cs_builtin("prim.Equals",
                          [self](interpreter& i, const cs_args& a) {
                              return cs_bool(cs_eq(self, take(a, 0)));
                          });
    if (name == "CompareTo")
        return cs_builtin("prim.CompareTo",
                          [self](interpreter& i, const cs_args& a) {
                              CsRef o = take(a, 0);
                              bool ok1 = false, ok2 = false;
                              const double d1 = cs_to_float(self, &ok1);
                              const double d2 = cs_to_float(o, &ok2);
                              if (!ok1 || !ok2)
                                  return cs_int(1);
                              return cs_int(d1 < d2 ? -1 : d1 > d2 ? 1 : 0);
                          });
    // char helpers
    if (self && self->kind == cs_kind::char_) {
        if (name == "IsDigit" || name == "IsLetter" || name == "IsWhiteSpace" ||
            name == "IsUpper" || name == "IsLower" || name == "ToUpper" ||
            name == "ToLower") {
            return cs_builtin(
                "Char." + name,
                [self, name](interpreter& i, const cs_args& a) {
                    int64_t c = as_char(self)->v;
                    if (a.size() > 0) {
                        bool ok = false;
                        c = cs_to_int(take(a, 0), &ok);
                    }
                    if (name == "IsDigit")
                        return cs_bool(std::isdigit((int)c) != 0);
                    if (name == "IsLetter")
                        return cs_bool(std::isalpha((int)c) != 0);
                    if (name == "IsWhiteSpace")
                        return cs_bool(std::isspace((int)c) != 0);
                    if (name == "IsUpper")
                        return cs_bool(std::isupper((int)c) != 0);
                    if (name == "IsLower")
                        return cs_bool(std::islower((int)c) != 0);
                    if (name == "ToUpper")
                        return cs_char(std::toupper((int)c));
                    return cs_char(std::tolower((int)c));
                });
        }
    }
    return nullptr;
}

} // namespace

// getattr fallback for scalars/collections — mirrors pymini value member
// tables.  returns nullptr + found=false when no member matches.
CsRef csmini_value_member(interpreter& i, const CsRef& obj,
                          const std::string& name, bool* found) {
    if (found)
        *found = false;
    if (!obj)
        return nullptr;
    CsRef m;
    switch (obj->kind) {
    case cs_kind::string:
        m = str_member(name, obj);
        break;
    case cs_kind::array:
        m = arr_member(name, obj);
        break;
    case cs_kind::dict:
        m = dict_member(name, obj);
        break;
    case cs_kind::integer:
    case cs_kind::number:
    case cs_kind::boolean:
    case cs_kind::char_:
        m = prim_member(name, obj);
        break;
    case cs_kind::exception_: {
        auto* e = as_exc(obj);
        if (name == "Message") {
            if (found)
                *found = true;
            return cs_str(e->message);
        }
        if (name == "StackTrace")
            return cs_str([&] {
                std::string out;
                for (const auto& t : e->trace) {
                    if (!out.empty())
                        out.push_back('\n');
                    out += t;
                }
                return out;
            }());
        if (name == "ToString") {
            if (found)
                *found = true;
            return cs_builtin("Exception.ToString",
                              [r = obj](interpreter& interp,
                                        const cs_args&) {
                                  return cs_str(cs_to_str(interp, r));
                              });
        }
        break;
    }
    default:
        break;
    }
    if (m && found)
        *found = true;
    return m;
}

void csmini_install_stdlib(interpreter&) {}    // extras live in stdlib2

} // namespace sao::plugins::csmini
