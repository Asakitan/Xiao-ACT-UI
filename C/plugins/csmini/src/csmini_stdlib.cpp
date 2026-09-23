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
#include <iterator>
#include <limits>
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

void require_arg_count(interpreter& i, const cs_args& args,
                       std::size_t minimum, std::size_t maximum,
                       std::string_view method) {
    if (args.size() < minimum || args.size() > maximum)
        i.raise_exc("ArgumentException",
                    std::string(method) + ": invalid argument count");
}

void require_collection_room(interpreter& i, std::size_t current,
                             std::size_t additional,
                             std::string_view operation) {
    if (current > k_max_collection_items ||
        additional > k_max_collection_items - current)
        i.raise_exc("InvalidOperationException",
                    std::string(operation) + " exceeds csmini collection limit");
}

CsArrayObj* require_sequence(interpreter& i, const CsRef& source,
                             std::string_view operation) {
    auto* sequence = as_array(source);
    if (!sequence)
        i.raise_exc("InvalidOperationException",
                    std::string(operation) + " requires an array/List/HashSet");
    if (sequence->v.size() > k_max_collection_items)
        i.raise_exc("InvalidOperationException",
                    std::string(operation) + " exceeds csmini collection limit");
    return sequence;
}

int compare_values(interpreter& i, const CsRef& left, const CsRef& right) {
    if (cs_is_null(left) || cs_is_null(right)) {
        if (cs_is_null(left) && cs_is_null(right))
            return 0;
        return cs_is_null(left) ? -1 : 1;
    }
    const bool left_number = left->kind == cs_kind::integer ||
                             left->kind == cs_kind::number ||
                             left->kind == cs_kind::char_ ||
                             left->kind == cs_kind::boolean;
    const bool right_number = right->kind == cs_kind::integer ||
                              right->kind == cs_kind::number ||
                              right->kind == cs_kind::char_ ||
                              right->kind == cs_kind::boolean;
    if (left_number && right_number) {
        const double a = cs_to_float(left, nullptr);
        const double b = cs_to_float(right, nullptr);
        return a < b ? -1 : a > b ? 1 : 0;
    }
    if (auto* a = as_str(left)) {
        if (auto* b = as_str(right)) {
            const int result = a->v.compare(b->v);
            return result < 0 ? -1 : result > 0 ? 1 : 0;
        }
    }
    const std::string a = cs_to_str(i, left);
    const std::string b = cs_to_str(i, right);
    const int result = a.compare(b);
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

CsRef sequence_default(const std::vector<CsRef>& values) {
    if (values.empty() || !values.front())
        return cs_null();
    switch (values.front()->kind) {
    case cs_kind::boolean:
        return cs_false();
    case cs_kind::integer:
        return cs_int(0);
    case cs_kind::number:
        return cs_float(0.0);
    case cs_kind::char_:
        return cs_char(0);
    case cs_kind::guid:
        return cs_guid({});
    default:
        return cs_null();
    }
}

bool is_linq_method(std::string_view name) {
    static constexpr std::string_view methods[] = {
        "Where", "Select", "Any", "All", "First", "FirstOrDefault",
        "Last", "LastOrDefault", "Count", "ToList", "ToArray", "Sum",
        "Min", "Max", "Distinct", "Take", "Skip", "OrderBy",
        "OrderByDescending",
    };
    return std::find(std::begin(methods), std::end(methods), name) !=
           std::end(methods);
}

CsRef linq_call(interpreter& i, std::string_view name, const CsRef& source,
                const cs_args& args) {
    CsArrayObj* sequence = require_sequence(i, source, name);
    const auto& values = sequence->v;
    if (name == "Where" || name == "Select") {
        require_arg_count(i, args, 1, 1, name);
        auto out = cs_array();
        as_array(out)->v.reserve(values.size());
        for (const CsRef& value : values) {
            CsRef mapped = i.call1(args.pos[0], value);
            if (name == "Select" || cs_truthy(mapped))
                as_array(out)->v.push_back(name == "Select" ? mapped : value);
        }
        return out;
    }
    if (name == "Any") {
        require_arg_count(i, args, 0, 1, name);
        if (args.pos.empty())
            return cs_bool(!values.empty());
        for (const CsRef& value : values)
            if (cs_truthy(i.call1(args.pos[0], value)))
                return cs_true();
        return cs_false();
    }
    if (name == "All") {
        require_arg_count(i, args, 1, 1, name);
        for (const CsRef& value : values)
            if (!cs_truthy(i.call1(args.pos[0], value)))
                return cs_false();
        return cs_true();
    }
    if (name == "First" || name == "FirstOrDefault" || name == "Last" ||
        name == "LastOrDefault") {
        require_arg_count(i, args, 0, 1, name);
        const bool from_back = name == "Last" || name == "LastOrDefault";
        for (std::size_t offset = 0; offset < values.size(); ++offset) {
            const std::size_t index = from_back ? values.size() - 1 - offset
                                                : offset;
            const CsRef& value = values[index];
            if (args.pos.empty() || cs_truthy(i.call1(args.pos[0], value)))
                return value;
        }
        if (name == "FirstOrDefault" || name == "LastOrDefault")
            return sequence_default(values);
        i.raise_exc("InvalidOperationException", "sequence contains no elements");
    }
    if (name == "Count") {
        require_arg_count(i, args, 0, 1, name);
        if (args.pos.empty())
            return cs_int(static_cast<int64_t>(values.size()));
        int64_t count = 0;
        for (const CsRef& value : values)
            if (cs_truthy(i.call1(args.pos[0], value)))
                ++count;
        return cs_int(count);
    }
    if (name == "ToList" || name == "ToArray") {
        require_arg_count(i, args, 0, 0, name);
        return cs_array(values);
    }
    if (name == "Distinct") {
        require_arg_count(i, args, 0, 0, name);
        auto out = cs_array();
        for (const CsRef& value : values)
            if (!cs_sequence_contains(as_array(out), value))
                as_array(out)->v.push_back(value);
        return out;
    }
    if (name == "Take" || name == "Skip") {
        require_arg_count(i, args, 1, 1, name);
        int64_t count = to_i(i, args.pos[0]);
        if (count < 0)
            count = 0;
        std::size_t begin = 0;
        std::size_t end = values.size();
        if (name == "Take")
            end = (std::min)(end, static_cast<std::size_t>(count));
        else
            begin = (std::min)(end, static_cast<std::size_t>(count));
        return cs_array(std::vector<CsRef>(values.begin() + begin,
                                           values.begin() + end));
    }
    if (name == "Sum") {
        require_arg_count(i, args, 0, 1, name);
        long double total = 0;
        bool integral = true;
        for (const CsRef& value : values) {
            CsRef number = args.pos.empty() ? value : i.call1(args.pos[0], value);
            if (!number || (number->kind != cs_kind::integer &&
                            number->kind != cs_kind::number &&
                            number->kind != cs_kind::char_))
                i.raise_exc("InvalidOperationException",
                            "Sum requires numeric values");
            integral = integral && number->kind != cs_kind::number;
            if (auto* integer = as_int(number))
                total += static_cast<long double>(integer->v);
            else if (auto* character = as_char(number))
                total += static_cast<long double>(character->v);
            else
                total += static_cast<long double>(as_float(number)->v);
        }
        if (integral && total >= static_cast<long double>((std::numeric_limits<int64_t>::min)()) &&
            total <= static_cast<long double>((std::numeric_limits<int64_t>::max)()))
            return cs_int(static_cast<int64_t>(total));
        return cs_float(static_cast<double>(total));
    }
    if (name == "Min" || name == "Max") {
        require_arg_count(i, args, 0, 1, name);
        CsRef best;
        bool has_best = false;
        for (const CsRef& value : values) {
            CsRef candidate = args.pos.empty() ? value : i.call1(args.pos[0], value);
            if (!has_best ||
                (name == "Min" ? compare_values(i, candidate, best) < 0
                                : compare_values(i, candidate, best) > 0)) {
                best = candidate;
                has_best = true;
            }
        }
        if (!has_best)
            i.raise_exc("InvalidOperationException", "sequence contains no elements");
        return best ? best : cs_null();
    }
    if (name == "OrderBy" || name == "OrderByDescending") {
        require_arg_count(i, args, 1, 1, name);
        std::vector<std::pair<CsRef, CsRef>> keyed;
        keyed.reserve(values.size());
        for (const CsRef& value : values)
            keyed.emplace_back(i.call1(args.pos[0], value), value);
        const bool descending = name == "OrderByDescending";
        std::stable_sort(keyed.begin(), keyed.end(),
                         [&i, descending](const auto& left, const auto& right) {
                             const int order = compare_values(i, left.first, right.first);
                             return descending ? order > 0 : order < 0;
                         });
        auto out = cs_array();
        as_array(out)->v.reserve(keyed.size());
        for (auto& entry : keyed)
            as_array(out)->v.push_back(std::move(entry.second));
        return out;
    }
    i.raise_exc("MissingMemberException",
                "unsupported Enumerable member '" + std::string(name) + "'");
}

CsRef linq_member(const std::string& name, CsRef self) {
    if (!is_linq_method(name) || name == "Count")
        return nullptr;
    return cs_builtin("Enumerable." + name,
                      [self, name](interpreter& i, const cs_args& args) {
                          return linq_call(i, name, self, args);
                      });
}

CsRef set_member(const std::string& name, CsRef self) {
    auto* set = as_array(self);
    if (!set || !set->is_set())
        return nullptr;
    if (name == "Count")
        return cs_int(static_cast<int64_t>(set->v.size()));
    if (CsRef member = linq_member(name, self))
        return member;
    if (name == "Add") {
        return cs_builtin("HashSet.Add", [self](interpreter& i, const cs_args& args) {
            require_arg_count(i, args, 1, 1, "HashSet.Add");
            auto* target = as_array(self);
            if (cs_sequence_contains(target, args.pos[0]))
                return cs_false();
            require_collection_room(i, target->v.size(), 1, "HashSet.Add");
            return cs_bool(cs_set_add(target, args.pos[0]));
        });
    }
    if (name == "Remove") {
        return cs_builtin("HashSet.Remove", [self](interpreter& i, const cs_args& args) {
            require_arg_count(i, args, 1, 1, "HashSet.Remove");
            auto& values = as_array(self)->v;
            for (auto it = values.begin(); it != values.end(); ++it) {
                if (cs_eq(*it, args.pos[0])) {
                    values.erase(it);
                    return cs_true();
                }
            }
            return cs_false();
        });
    }
    if (name == "Contains") {
        return cs_builtin("HashSet.Contains", [self](interpreter& i, const cs_args& args) {
            require_arg_count(i, args, 1, 1, "HashSet.Contains");
            return cs_bool(cs_sequence_contains(as_array(self), args.pos[0]));
        });
    }
    if (name == "Clear") {
        return cs_builtin("HashSet.Clear", [self](interpreter& i, const cs_args& args) {
            require_arg_count(i, args, 0, 0, "HashSet.Clear");
            as_array(self)->v.clear();
            return cs_null();
        });
    }
    if (name == "UnionWith" || name == "IntersectWith" ||
        name == "ExceptWith" || name == "SetEquals") {
        return cs_builtin("HashSet." + name,
                          [self, name](interpreter& i, const cs_args& args) {
            require_arg_count(i, args, 1, 1, "HashSet set operation");
            auto* target = as_array(self);
            auto* other = require_sequence(i, args.pos[0], name);
            const std::vector<CsRef> snapshot = other->v;
            if (name == "UnionWith") {
                for (const CsRef& value : snapshot) {
                    if (!cs_sequence_contains(target, value)) {
                        require_collection_room(i, target->v.size(), 1, name);
                        (void)cs_set_add(target, value);
                    }
                }
                return cs_null();
            }
            if (name == "IntersectWith") {
                std::erase_if(target->v, [&snapshot](const CsRef& value) {
                    for (const CsRef& candidate : snapshot)
                        if (cs_eq(value, candidate))
                            return false;
                    return true;
                });
                return cs_null();
            }
            if (name == "ExceptWith") {
                std::erase_if(target->v, [&snapshot](const CsRef& value) {
                    for (const CsRef& candidate : snapshot)
                        if (cs_eq(value, candidate))
                            return true;
                    return false;
                });
                return cs_null();
            }
            auto unique = cs_set(snapshot);
            auto* expected = as_array(unique);
            if (expected->v.size() != target->v.size())
                return cs_false();
            for (const CsRef& value : target->v)
                if (!cs_sequence_contains(expected, value))
                    return cs_false();
            return cs_true();
        });
    }
    return nullptr;
}

// ── array/List<T> members ──────────────────────────────────────────────────
CsRef arr_member(const std::string& name, CsRef self) {
    auto* arr = as_array(self);
    if (!arr)
        return nullptr;
    if (arr->is_set())
        return set_member(name, self);
    if (name == "Count" || name == "Length")
        return cs_int((int64_t)arr->v.size());
    if (name == "Capacity")
        return cs_int((int64_t)arr->v.capacity());
    if (CsRef member = linq_member(name, self))
        return member;
    static const std::unordered_map<std::string, member_fn> fns = {
        {"Add",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             require_arg_count(i, a, 1, 1, "List.Add");
             require_collection_room(i, as_array(self)->v.size(), 1,
                                     "List.Add");
             as_array(self)->v.push_back(take(a, 0));
             return cs_null();
         }},
        {"AddRange",
         [](interpreter& i, CsRef self, const cs_args& a) -> CsRef {
             auto* dst = as_array(self);
             require_arg_count(i, a, 1, 1, "List.AddRange");
             if (auto* src = as_array(take(a, 0))) {
                 const std::vector<CsRef> copy = src->v;
                 require_collection_room(i, dst->v.size(), copy.size(),
                                         "List.AddRange");
                 dst->v.insert(dst->v.end(), copy.begin(), copy.end());
             } else {
                 arg_err(i, "List.AddRange expects a collection");
             }
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
             require_arg_count(i, a, 2, 2, "List.Insert");
             require_collection_room(i, v->size(), 1, "List.Insert");
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

CsRef guid_member(const std::string& name, CsRef self) {
    auto* guid = as_guid(self);
    if (!guid)
        return nullptr;
    if (name == "ToString") {
        return cs_builtin("Guid.ToString", [self](interpreter& i,
                                                   const cs_args& args) {
            require_arg_count(i, args, 0, 1, "Guid.ToString");
            const std::string format =
                args.pos.empty() ? "D" : cs_to_str(i, args.pos[0]);
            const std::string text = cs_guid_format(as_guid(self)->bytes, format);
            if (text.empty())
                i.raise_exc("FormatException", "unsupported Guid format");
            return cs_str(text);
        });
    }
    if (name == "Equals") {
        return cs_builtin("Guid.Equals", [self](interpreter& i,
                                                 const cs_args& args) {
            require_arg_count(i, args, 1, 1, "Guid.Equals");
            return cs_bool(cs_eq(self, args.pos[0]));
        });
    }
    if (name == "GetHashCode") {
        return cs_builtin("Guid.GetHashCode", [self](interpreter& i,
                                                      const cs_args& args) {
            require_arg_count(i, args, 0, 0, "Guid.GetHashCode");
            uint64_t hash = 1469598103934665603ull;
            for (uint8_t byte : as_guid(self)->bytes) {
                hash ^= byte;
                hash *= 1099511628211ull;
            }
            return cs_int(static_cast<int64_t>(hash));
        });
    }
    return nullptr;
}

} // namespace

CsRef csmini_enumerable_call(interpreter& i, std::string_view name,
                             const CsRef& source, const cs_args& args) {
    return linq_call(i, name, source, args);
}

CsRef csmini_value_callable_member(interpreter& i, const CsRef& obj,
                                   const std::string& name, bool* found) {
    if (found)
        *found = false;
    if (!as_array(obj) || name != "Count")
        return nullptr;
    if (found)
        *found = true;
    return cs_builtin("Enumerable.Count",
                      [obj](interpreter& interp, const cs_args& args) {
                          return linq_call(interp, "Count", obj, args);
                      });
}

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
    case cs_kind::guid:
        m = guid_member(name, obj);
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
