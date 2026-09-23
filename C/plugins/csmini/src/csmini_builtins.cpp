// csmini_builtins.cpp — globals/builtins facades: Console, Math, Convert,
// DateTime, JsonSerializer, System namespace, collections type objects,
// exception types, primitive-type facades (int.Parse / string.Format …).
//
// Facades are CsNativeObj member bags; methods are cs_builtin entries.
// `new List<T>()` / `new Dictionary<K,V>()` resolve through the type-name
// fast paths in interpreter::new_instance_eval before facade lookup.
#include "csmini_interp.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <random>
#endif

namespace sao::plugins::csmini {
namespace {

using nn = cs_native_fn;

CsDictObj* D(const CsRef& bag) { return as_dict(as_native(bag)->members); }

CsRef take(const cs_args& a, std::size_t k) {
    return k < a.size() ? a.pos[k] : cs_null();
}

CsRef need_bag(std::string name) {
    return cs_native(std::move(name), false);
}
CsDictObj* members_of(const CsRef& bag) { return as_dict(as_native(bag)->members); }

void put(const CsRef& bag, const char* name, CsRef v) {
    dict_set(members_of(bag), cs_str(name), std::move(v));
}
void putfn(const CsRef& bag, const char* name, nn fn) {
    dict_set(members_of(bag), cs_str(name),
             cs_builtin(std::string("net.") + name, std::move(fn)));
}

void log_line(interpreter& i, const std::string& s) {
    if (i.on_log)
        i.on_log(s);
}

// ── numbers ────────────────────────────────────────────────────────────────
double num_arg(interpreter& i, const CsRef& v, const char* what = "number") {
    bool ok = false;
    const double d = cs_to_float(v, &ok);
    if (!ok)
        i.raise_exc("ArgumentException",
                    std::string("expected numeric ") + what);
    return d;
}
int64_t int_arg(interpreter& i, const CsRef& v, const char* what = "int") {
    bool ok = false;
    const int64_t n = cs_to_int(v, &ok);
    if (!ok)
        i.raise_exc("ArgumentException",
                    std::string("expected integer ") + what);
    return n;
}

// ── primitive-type facades ─────────────────────────────────────────────────
// `int`/`long`/`double`/… usable as statics: `int.Parse`, `double.TryParse`.
nn parse_int_fn(bool floating_ok) {
    return [floating_ok](interpreter& i, const cs_args& a) {
        CsRef v = take(a, 0);
        if (auto* s = as_str(v)) {
            try {
                if (floating_ok) {
                    std::size_t n = 0;
                    double d = std::stod(s->v, &n);
                    if (n != s->v.size()) {
                        // reject trailing junk (allow sign/digits only)
                        bool digits = true;
                        for (char c : s->v)
                            if (!std::isdigit((unsigned char)c) &&
                                c != '.' && c != '-' && c != '+' &&
                                c != 'e' && c != 'E')
                                digits = false;
                        if (!digits)
                            i.raise_exc("FormatException",
                                        "bad number format");
                    }
                    return cs_int((int64_t)d);
                }
                // int.Parse tolerates surrounding whitespace but must
                // consume the whole string — `12x` is a FormatException.
                const auto tb = s->v.find_first_not_of(" \t\r\n");
                if (tb == std::string::npos)
                    i.raise_exc("FormatException", "bad number format");
                const auto te = s->v.find_last_not_of(" \t\r\n");
                const std::string t = s->v.substr(tb, te - tb + 1);
                std::size_t used = 0;
                const int64_t n = std::stoll(t, &used);
                if (used != t.size())
                    i.raise_exc("FormatException", "bad number format");
                return cs_int(n);
            } catch (const cs_error&) {
                throw;
            } catch (const sig_raise&) {
                throw;
            } catch (const std::exception&) {
                i.raise_exc("FormatException", "bad number format");
            }
        }
        bool ok = false;
        int64_t n = cs_to_int(v, &ok);
        if (ok)
            return cs_int(n);
        double d = cs_to_float(v, &ok);
        if (ok)
            return cs_int((int64_t)d);
        i.raise_exc("FormatException", "cannot parse value");
    };
}

} // namespace

CsRef csmini_make_task(CsRef result, bool value_task) {
    CsRef task = cs_native(value_task ? "System.Threading.Tasks.ValueTask"
                                      : "System.Threading.Tasks.Task",
                           false);
    CsRef awaiter = cs_native(value_task
                                  ? "System.Runtime.CompilerServices.ValueTaskAwaiter"
                                  : "System.Runtime.CompilerServices.TaskAwaiter",
                              false);
    put(awaiter, "IsCompleted", cs_true());
    put(awaiter, "GetResult",
        cs_builtin("TaskAwaiter.GetResult",
                   [result](interpreter&, const cs_args&) { return result; }));
    put(task, "Result", result);
    put(task, "IsCompleted", cs_true());
    put(task, "IsCompletedSuccessfully", cs_true());
    put(task, "IsFaulted", cs_false());
    put(task, "IsCanceled", cs_false());
    put(task, "GetAwaiter",
        cs_builtin("Task.GetAwaiter",
                   [awaiter](interpreter&, const cs_args&) { return awaiter; }));
    put(task, "GetResult",
        cs_builtin("Task.GetResult",
                   [result](interpreter&, const cs_args&) { return result; }));
    if (value_task) {
        put(task, "AsTask",
            cs_builtin("ValueTask.AsTask",
                       [result](interpreter&, const cs_args&) {
                           return csmini_make_task(result, false);
                       }));
    }
    put(task, "ContinueWith",
        cs_builtin("Task.ContinueWith",
                   [](interpreter& i, const cs_args&) -> CsRef {
                       i.record_feature("async_continuations");
                       i.raise_exc("NotSupportedException",
                                   "Task continuations are outside async-lite");
                   }));
    put(task, "ConfigureAwait",
        cs_builtin("Task.ConfigureAwait",
                   [](interpreter& i, const cs_args&) -> CsRef {
                       i.record_feature("async_continuations");
                       i.raise_exc("NotSupportedException",
                                   "Task continuations are outside async-lite");
                   }));
    return task;
}

CsRef csmini_await_value(interpreter& i, const CsRef& value, src_pos pos) {
    if (!value || value->kind != cs_kind::native_obj)
        return value ? value : cs_null();
    bool found = false;
    CsRef get_awaiter = i.getattr(value, "GetAwaiter", &found);
    if (!found || !get_awaiter)
        return value;
    CsRef awaiter = i.call0(get_awaiter, pos);
    CsRef get_result = i.getattr(awaiter, "GetResult", &found);
    if (!found || !get_result)
        i.raise_exc("InvalidOperationException",
                    "awaiter has no synchronous GetResult", pos);
    return i.call0(get_result, pos);
}

void csmini_install_builtins(interpreter& i) {
    auto* g = as_dict(i.globals);
    const auto inject = [&](const char* name, CsRef v) {
        dict_set(g, cs_str(name), std::move(v));
    };

    // ── Console ───────────────────────────────────────────────────────
    {
        CsRef con = need_bag("System.Console");
        putfn(con, "WriteLine", [](interpreter& i, const cs_args& a) {
            const std::string s =
                a.pos.empty() ? "" : cs_to_str(i, take(a, 0));
            log_line(i, s);
            return cs_null();
        });
        putfn(con, "Write", [](interpreter& i, const cs_args& a) {
            log_line(i, cs_to_str(i, take(a, 0)));
            return cs_null();
        });
        putfn(con, "ReadLine", [](interpreter& i, const cs_args&) {
            // no interactive stdin in plugin host — return empty string
            (void)i;
            return cs_str("");
        });
        CsRef err = need_bag("System.Console.Error");
        putfn(err, "WriteLine", [](interpreter& i, const cs_args& a) {
            log_line(i, std::string("[err] ") + cs_to_str(i, take(a, 0)));
            return cs_null();
        });
        putfn(err, "Write", [](interpreter& i, const cs_args& a) {
            log_line(i, std::string("[err] ") + cs_to_str(i, take(a, 0)));
            return cs_null();
        });
        put(con, "Error", err);
        inject("Console", con);
    }

    // ── Math ──────────────────────────────────────────────────────────
    {
        CsRef m = need_bag("System.Math");
        put(m, "PI", cs_float(3.14159265358979323846));
        put(m, "E", cs_float(2.71828182845904523536));
        auto num1 = [](double (*cfn)(double), const char* name) {
            return [cfn, name](interpreter& i, const cs_args& a) {
                return cs_float(cfn(num_arg(i, take(a, 0), name)));
            };
        };
        putfn(m, "Abs", [](interpreter& i, const cs_args& a) {
            CsRef v = take(a, 0);
            if (v && v->kind == cs_kind::integer) {
                const int64_t x = as_int(v)->v;
                if (x == INT64_MIN)
                    i.raise_exc("OverflowException",
                                "Abs overflow on Int64.MinValue");
                return cs_int(x < 0 ? -x : x);
            }
            return cs_float(std::fabs(num_arg(i, v, "Abs")));
        });
        putfn(m, "Min", [](interpreter& i, const cs_args& a) {
            if (take(a, 0) && take(a, 0)->kind == cs_kind::integer &&
                take(a, 1) && take(a, 1)->kind == cs_kind::integer)
                return cs_int((std::min)(as_int(take(a, 0))->v,
                                         as_int(take(a, 1))->v));
            return cs_float((std::min)(num_arg(i, take(a, 0)),
                                       num_arg(i, take(a, 1))));
        });
        putfn(m, "Max", [](interpreter& i, const cs_args& a) {
            if (take(a, 0) && take(a, 0)->kind == cs_kind::integer &&
                take(a, 1) && take(a, 1)->kind == cs_kind::integer)
                return cs_int((std::max)(as_int(take(a, 0))->v,
                                         as_int(take(a, 1))->v));
            return cs_float((std::max)(num_arg(i, take(a, 0)),
                                       num_arg(i, take(a, 1))));
        });
        putfn(m, "Pow", [](interpreter& i, const cs_args& a) {
            return cs_float(std::pow(num_arg(i, take(a, 0)),
                                     num_arg(i, take(a, 1))));
        });
        putfn(m, "Sqrt", num1(std::sqrt, "Sqrt"));
        putfn(m, "Floor", num1(std::floor, "Floor"));
        putfn(m, "Ceiling", num1(std::ceil, "Ceiling"));
        putfn(m, "Round", [](interpreter& i, const cs_args& a) {
            const double d = num_arg(i, take(a, 0), "Round");
            // C# default is ToEven (banker's rounding) — rint honors the
            // default FE_TONEAREST rounding mode = round-half-to-even.
            if (a.size() > 1) {
                const int64_t k = int_arg(i, take(a, 1), "digits");
                const double p = std::pow(10.0, (double)k);
                return cs_float(std::rint(d * p) / p);
            }
            return cs_float(std::rint(d));
        });
        putfn(m, "Truncate", num1(std::trunc, "Truncate"));
        putfn(m, "Sin", num1(std::sin, "Sin"));
        putfn(m, "Cos", num1(std::cos, "Cos"));
        putfn(m, "Tan", num1(std::tan, "Tan"));
        putfn(m, "Asin", num1(std::asin, "Asin"));
        putfn(m, "Acos", num1(std::acos, "Acos"));
        putfn(m, "Atan", num1(std::atan, "Atan"));
        putfn(m, "Atan2", [](interpreter& i, const cs_args& a) {
            return cs_float(std::atan2(num_arg(i, take(a, 0)),
                                       num_arg(i, take(a, 1))));
        });
        putfn(m, "Exp", num1(std::exp, "Exp"));
        putfn(m, "Log", [](interpreter& i, const cs_args& a) {
            const double d = num_arg(i, take(a, 0), "Log");
            if (a.size() > 1)
                return cs_float(std::log(d) / std::log(num_arg(i, take(a, 1))));
            return cs_float(std::log(d));
        });
        putfn(m, "Log10", num1(std::log10, "Log10"));
        putfn(m, "Sign", [](interpreter& i, const cs_args& a) {
            const double d = num_arg(i, take(a, 0), "Sign");
            return cs_int(d > 0 ? 1 : d < 0 ? -1 : 0);
        });
        putfn(m, "Clamp", [](interpreter& i, const cs_args& a) {
            CsRef v = take(a, 0), lo = take(a, 1), hi = take(a, 2);
            if (v && v->kind == cs_kind::integer && lo && hi &&
                lo->kind == cs_kind::integer && hi->kind == cs_kind::integer)
                return cs_int((std::min)((std::max)(as_int(v)->v, as_int(lo)->v),
                                         as_int(hi)->v));
            const double d = num_arg(i, v), dl = num_arg(i, lo),
                         dh = num_arg(i, hi);
            return cs_float((std::min)((std::max)(d, dl), dh));
        });
        inject("Math", m);
    }

    // ── Convert ────────────────────────────────────────────────────────
    {
        CsRef c = need_bag("System.Convert");
        putfn(c, "ToInt32", [](interpreter& i, const cs_args& a) {
            bool ok = false;
            int64_t v = cs_to_int(take(a, 0), &ok);
            if (!ok)
                i.raise_exc("InvalidCastException", "ToInt32 failed");
            return cs_int(v);
        });
        putfn(c, "ToInt64", [](interpreter& i, const cs_args& a) {
            bool ok = false;
            int64_t v = cs_to_int(take(a, 0), &ok);
            if (!ok)
                i.raise_exc("InvalidCastException", "ToInt64 failed");
            return cs_int(v);
        });
        putfn(c, "ToDouble", [](interpreter& i, const cs_args& a) {
            bool ok = false;
            double d = cs_to_float(take(a, 0), &ok);
            if (!ok)
                i.raise_exc("InvalidCastException", "ToDouble failed");
            return cs_float(d);
        });
        putfn(c, "ToSingle", [](interpreter& i, const cs_args& a) {
            bool ok = false;
            double d = cs_to_float(take(a, 0), &ok);
            if (!ok)
                i.raise_exc("InvalidCastException", "ToSingle failed");
            return cs_float(d);
        });
        putfn(c, "ToString", [](interpreter& i, const cs_args& a) {
            return cs_str(cs_to_str(i, take(a, 0)));
        });
        putfn(c, "ToBoolean", [](interpreter& i, const cs_args& a) {
            CsRef v = take(a, 0);
            if (auto* s = as_str(v)) {
                // Convert.ToBoolean(string) accepts only True/False
                // (case-insensitive, ws-trimmed) — anything else is a
                // FormatException.
                const auto tb = s->v.find_first_not_of(" \t\r\n");
                const auto te = s->v.find_last_not_of(" \t\r\n");
                std::string t = tb == std::string::npos
                                    ? ""
                                    : s->v.substr(tb, te - tb + 1);
                for (char& ch : t)
                    ch = static_cast<char>(
                        std::tolower(static_cast<unsigned char>(ch)));
                if (t == "true")
                    return cs_true();
                if (t == "false")
                    return cs_false();
                i.raise_exc("FormatException",
                            "String was not recognized as a valid Boolean");
            }
            return cs_bool(cs_truthy(v));
        });
        putfn(c, "ToChar", [](interpreter& i, const cs_args& a) {
            CsRef v = take(a, 0);
            bool ok = false;
            int64_t k = cs_to_int(v, &ok);
            if (ok)
                return cs_char(k);
            if (auto* s = as_str(v); s && !s->v.empty())
                return cs_char((unsigned char)s->v[0]);
            i.raise_exc("InvalidCastException", "ToChar failed");
        });
        putfn(c, "ChangeType", [](interpreter& i, const cs_args& a) {
            // ChangeType(v, typeof/int-name) — coerce by kind hint
            CsRef v = take(a, 0);
            CsRef t = take(a, 1);
            std::string tn = cs_to_str(i, t);
            // typeof markers carry Name/FullName
            if (auto* n = as_native(t)) {
                CsRef name = dict_get(members_of(t), cs_str("Name"));
                if (name)
                    tn = cs_to_str(i, name);
            }
            if (tn == "Int32" || tn == "Int64" || tn == "int")
                return cs_int(cs_to_int(v, nullptr));
            if (tn == "Double" || tn == "Single" || tn == "double")
                return cs_float(cs_to_float(v, nullptr));
            if (tn == "Boolean" || tn == "bool")
                return cs_bool(cs_truthy(v));
            return cs_str(cs_to_str(i, v));
        });
        inject("Convert", c);
    }

    // ── DateTime — unix-seconds double model ──────────────────────────
    {
        CsRef dt = need_bag("System.DateTime");
        auto now_secs = [] {
            return cs_float(std::chrono::duration<double>(
                                std::chrono::system_clock::now()
                                    .time_since_epoch())
                                .count());
        };
        put(dt, "Now", cs_builtin("DateTime.Now",
                                  [now_secs](interpreter& i, const cs_args&) {
                                      (void)i;
                                      return now_secs();
                                  }));
        put(dt, "UtcNow", cs_builtin("DateTime.UtcNow",
                                     [now_secs](interpreter& i,
                                                const cs_args&) {
                                         (void)i;
                                         return now_secs();
                                     }));
        put(dt, "Today", cs_builtin("DateTime.Today",
                                    [](interpreter& i, const cs_args&) {
                                        (void)i;
                                        std::time_t t = std::time(nullptr);
                                        return cs_float((double)(t -
                                                                 (t % 86400)));
                                    }));
        putfn(dt, "FromUnixTimeSeconds",
              [](interpreter& i, const cs_args& a) {
                  return cs_float((double)int_arg(i, take(a, 0)));
              });
        inject("DateTime", dt);
    }

    // ── primitive-type facades (int/double/bool/string/char/long/object) ──
    {
        auto mk_num = [](const char* name, nn parse) {
            CsRef t = cs_native(name, false);
            putfn(t, "Parse", std::move(parse));
            putfn(t, "ToString", [](interpreter& i, const cs_args& a) {
                return cs_str(cs_to_str(i, take(a, 0)));
            });
            return t;
        };
        inject("int", mk_num("System.Int32", parse_int_fn(false)));
        inject("Int32", mk_num("System.Int32", parse_int_fn(false)));
        inject("long", mk_num("System.Int64", parse_int_fn(false)));
        inject("Int64", mk_num("System.Int64", parse_int_fn(false)));
        inject("double", mk_num("System.Double",
                                [](interpreter& i, const cs_args& a) {
                                    CsRef v = take(a, 0);
                                    bool ok = false;
                                    double d = cs_to_float(v, &ok);
                                    if (ok)
                                        return cs_float(d);
                                    if (auto* s = as_str(v)) {
                                        try {
                                            return cs_float(std::stod(s->v));
                                        } catch (...) {
                                        }
                                    }
                                    i.raise_exc("FormatException",
                                                "bad double");
                                }));
        inject("Double", mk_num("System.Double",
                                [](interpreter& i, const cs_args& a) {
                                    CsRef v = take(a, 0);
                                    bool ok = false;
                                    double d = cs_to_float(v, &ok);
                                    if (ok)
                                        return cs_float(d);
                                    if (auto* s = as_str(v)) {
                                        try {
                                            return cs_float(std::stod(s->v));
                                        } catch (...) {
                                        }
                                    }
                                    i.raise_exc("FormatException",
                                                "bad double");
                                }));
        inject("float", mk_num("System.Single",
                               [](interpreter& i, const cs_args& a) {
                                   return cs_float(num_arg(i, take(a, 0)));
                               }));
        inject("decimal", mk_num("System.Decimal",
                                 [](interpreter& i, const cs_args& a) {
                                     return cs_float(num_arg(i, take(a, 0)));
                                 }));
        CsRef bfac = mk_num("System.Boolean",
                            [](interpreter& i, const cs_args& a) {
                                CsRef v = take(a, 0);
                                if (auto* s = as_str(v))
                                    return cs_bool(s->v == "true" ||
                                                   s->v == "True");
                                return cs_bool(cs_truthy(v));
                            });
        putfn(bfac, "Parse", [](interpreter& i, const cs_args& a) {
            CsRef v = take(a, 0);
            if (auto* s = as_str(v)) {
                if (s->v == "true" || s->v == "True")
                    return cs_true();
                if (s->v == "false" || s->v == "False")
                    return cs_false();
                i.raise_exc("FormatException", "bad bool");
            }
            return cs_bool(cs_truthy(v));
        });
        inject("bool", bfac);
        inject("Boolean", bfac);
        inject("char", mk_num("System.Char", parse_int_fn(false)));
        inject("Char", mk_num("System.Char", parse_int_fn(false)));
        inject("byte", mk_num("System.Byte", parse_int_fn(false)));
        inject("object", cs_native("System.Object", false));
        inject("Object", cs_native("System.Object", false));
        inject("var", cs_native("System.Object", false));
        inject("void", cs_native("System.Void", false));
    }

    // ── string facade (statics: Format/Join/Concat/IsNullOrEmpty/…) ─────
    {
        CsRef sf = cs_native("System.String", false);
        put(sf, "Empty", cs_str(""));
        putfn(sf, "Format", [](interpreter& i, const cs_args& a) {
            // {0}{1} positional; format spec after ':' stripped (deviation)
            auto* s = as_str(take(a, 0));
            if (!s) {
                i.raise_exc("ArgumentException", "Format needs a format string");
            }
            std::string out;
            const std::string& fmt = s->v;
            for (std::size_t k = 0; k < fmt.size(); ++k) {
                if (fmt[k] == '{') {
                    if (k + 1 < fmt.size() && fmt[k + 1] == '{') {
                        out.push_back('{');
                        ++k;
                        continue;
                    }
                    std::size_t e = fmt.find('}', k);
                    if (e == std::string::npos) {
                        i.raise_exc("FormatException", "unclosed '{'");
                    }
                    std::string inner = fmt.substr(k + 1, e - k - 1);
                    const auto colon = inner.find(':');
                    const auto comma = inner.find(',');
                    std::string idx = inner;
                    if (colon != std::string::npos)
                        idx = inner.substr(0, colon);
                    if (comma != std::string::npos)
                        idx = idx.substr(0, comma);
                    int n = -1;
                    try {
                        n = std::stoi(idx);
                    } catch (...) {
                    }
                    if (n < 0 || static_cast<std::size_t>(n + 1) >= a.size()) {
                        i.raise_exc("FormatException",
                                    "bad index in Format");
                    }
                    out += cs_to_str(i, a.pos[static_cast<std::size_t>(n) + 1]);
                    k = e;
                    continue;
                }
                if (fmt[k] == '}' && k + 1 < fmt.size() && fmt[k + 1] == '}') {
                    out.push_back('}');
                    ++k;
                    continue;
                }
                out.push_back(fmt[k]);
            }
            return cs_str(out);
        });
        putfn(sf, "Join", [](interpreter& i, const cs_args& a) {
            const std::string sep = cs_to_str(i, take(a, 0));
            CsRef items = take(a, 1);
            std::string out;
            std::vector<CsRef> seq;
            if (auto* arr = as_array(items))
                seq = arr->v;
            else
                seq.assign(a.pos.begin() + 1, a.pos.end());
            for (std::size_t k = 0; k < seq.size(); ++k) {
                if (k)
                    out += sep;
                out += cs_to_str(i, seq[k]);
            }
            return cs_str(out);
        });
        putfn(sf, "Concat", [](interpreter& i, const cs_args& a) {
            std::string out;
            CsRef v = take(a, 0);
            if (auto* arr = as_array(v); arr && a.size() == 1) {
                for (const CsRef& x : arr->v)
                    out += cs_to_str(i, x);
                return cs_str(out);
            }
            for (const CsRef& x : a.pos)
                out += cs_to_str(i, x);
            return cs_str(out);
        });
        putfn(sf, "IsNullOrEmpty", [](interpreter& i, const cs_args& a) {
            CsRef v = take(a, 0);
            if (cs_is_null(v))
                return cs_true();
            auto* s = as_str(v);
            return cs_bool(s && s->v.empty());
        });
        putfn(sf, "IsNullOrWhiteSpace", [](interpreter& i, const cs_args& a) {
            CsRef v = take(a, 0);
            if (cs_is_null(v))
                return cs_true();
            auto* s = as_str(v);
            if (!s)
                return cs_false();
            for (char c : s->v)
                if (!std::isspace((unsigned char)c))
                    return cs_false();
            return cs_true();
        });
        inject("string", sf);
        inject("String", sf);
    }

    // ── exception family — `new Exception(msg)` handled by
    //    new_instance_eval; expose facades so `catch (Exception e)` and
    //    `Exception.ToString` name checks can resolve statics too.
    {
        const char* exc_names[] = {
            "Exception",
            "InvalidOperationException",
            "ArgumentException",
            "ArgumentNullException",
            "ArgumentOutOfRangeException",
            "NotSupportedException",
            "NotImplementedException",
            "NullReferenceException",
            "FormatException",
            "OverflowException",
            "InvalidCastException",
            "KeyNotFoundException",
            "IndexOutOfRangeException",
            "DivideByZeroException",
            "ArithmeticException",
            "InvalidDataException",
            "ApplicationException",
            "MissingMemberException",
            "TypeLoadException",
            "IOException",
            "TimeoutException",
            "OperationCanceledException",
        };
        for (const char* xn : exc_names)
            inject(xn, cs_native(std::string("System.") + xn, false));
    }

    // ── collections type objects (for `new List<int>`-less spellings) ──
    inject("List", cs_native("System.Collections.Generic.List`1", false));
    inject("Dictionary",
           cs_native("System.Collections.Generic.Dictionary`2", false));
    inject("HashSet", cs_native("System.Collections.Generic.HashSet`1", false));

    // ── LINQ Enumerable facade ─────────────────────────────────────────
    {
        CsRef enumerable = cs_native("System.Linq.Enumerable", false);
        static constexpr const char* methods[] = {
            "Where", "Select", "Any", "All", "First", "FirstOrDefault",
            "Last", "LastOrDefault", "Count", "ToList", "ToArray", "Sum",
            "Min", "Max", "Distinct", "Take", "Skip", "OrderBy",
            "OrderByDescending",
        };
        for (const char* method : methods) {
            putfn(enumerable, method,
                  [method](interpreter& interp, const cs_args& args) {
                      if (args.pos.empty())
                          interp.raise_exc(
                              "ArgumentException",
                              std::string("Enumerable.") + method +
                                  " requires a source");
                      cs_args tail;
                      tail.pos.assign(args.pos.begin() + 1, args.pos.end());
                      return csmini_enumerable_call(interp, method, args.pos[0],
                                                    tail);
                  });
        }
        inject("Enumerable", enumerable);
    }

    // ── synchronous Task / ValueTask ───────────────────────────────────
    {
        auto make_task_facade = [](bool value_task) {
            CsRef facade = cs_native(value_task
                                         ? "System.Threading.Tasks.ValueTask"
                                         : "System.Threading.Tasks.Task",
                                     false);
            put(facade, "CompletedTask", csmini_make_task(cs_null(), value_task));
            putfn(facade, "FromResult",
                  [value_task](interpreter& interp, const cs_args& args) {
                      if (args.pos.size() != 1)
                          interp.raise_exc("ArgumentException",
                                           "FromResult expects one value");
                      return csmini_make_task(args.pos[0], value_task);
                  });
            return facade;
        };
        CsRef task = make_task_facade(false);
        putfn(task, "Delay", [](interpreter& interp, const cs_args& args) {
            if (args.pos.size() != 1)
                interp.raise_exc("ArgumentException", "Task.Delay expects milliseconds");
            const int64_t requested = int_arg(interp, args.pos[0], "milliseconds");
            if (requested < 0)
                interp.raise_exc("ArgumentOutOfRangeException",
                                 "Task.Delay milliseconds must be nonnegative");
            constexpr int64_t maximum_delay_ms = 250;
            if (requested > maximum_delay_ms) {
                interp.record_feature("async_continuations");
                interp.raise_exc("NotSupportedException",
                                 "Task.Delay exceeds synchronous async-lite limit");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(requested));
            return csmini_make_task(cs_null(), false);
        });
        putfn(task, "WhenAll", [](interpreter& interp, const cs_args& args) {
            std::vector<CsRef> tasks;
            if (args.pos.size() == 1 && as_array(args.pos[0]))
                tasks = as_array(args.pos[0])->v;
            else
                tasks = args.pos;
            if (tasks.size() > k_max_collection_items)
                interp.raise_exc("InvalidOperationException",
                                 "Task.WhenAll exceeds csmini collection limit");
            auto results = cs_array();
            as_array(results)->v.reserve(tasks.size());
            for (const CsRef& pending : tasks)
                as_array(results)->v.push_back(
                    csmini_await_value(interp, pending));
            return csmini_make_task(results, false);
        });
        putfn(task, "Run", [](interpreter& interp, const cs_args&) -> CsRef {
            interp.record_feature("async_continuations");
            interp.raise_exc("NotSupportedException",
                             "Task.Run is outside synchronous async-lite");
        });
        CsRef value_task = make_task_facade(true);
        inject("Task", task);
        inject("ValueTask", value_task);
    }

    // ── Guid ───────────────────────────────────────────────────────────
    {
        CsRef gd = cs_native("System.Guid", false);
        put(gd, "Empty", cs_guid({}));
        putfn(gd, "NewGuid", [](interpreter& interp, const cs_args& args) -> CsRef {
            if (!args.pos.empty())
                interp.raise_exc("ArgumentException", "Guid.NewGuid takes no arguments");
            std::array<uint8_t, 16> bytes{};
#if defined(_WIN32)
            if (BCryptGenRandom(nullptr, bytes.data(),
                                static_cast<ULONG>(bytes.size()),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
                interp.raise_exc("InvalidOperationException",
                                 "system random generator failed");
#else
            std::random_device random;
            for (uint8_t& byte : bytes)
                byte = static_cast<uint8_t>(random());
#endif
            bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
            bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
            return cs_guid(bytes);
        });
        putfn(gd, "Parse", [](interpreter& interp, const cs_args& args) -> CsRef {
            if (args.pos.size() != 1 || !as_str(args.pos[0]))
                interp.raise_exc("ArgumentException", "Guid.Parse expects a string");
            std::array<uint8_t, 16> bytes{};
            if (!cs_guid_parse(as_str(args.pos[0])->v, &bytes))
                interp.raise_exc("FormatException", "invalid Guid format");
            return cs_guid(bytes);
        });
        putfn(gd, "TryParse", [](interpreter& interp, const cs_args& args) {
            if (args.pos.size() != 1)
                interp.raise_exc(
                    "ArgumentException",
                    "csmini Guid.TryParse(string) expects one argument");
            std::array<uint8_t, 16> bytes{};
            return cs_bool(as_str(args.pos[0]) &&
                           cs_guid_parse(as_str(args.pos[0])->v, &bytes));
        });
        inject("Guid", gd);
    }

    // ── System namespace bag ────────────────────────────────────────────
    {
        CsRef sys = cs_native("System", false);
        auto link = [&](const char* member, const char* global) {
            if (CsRef v = dict_get(g, cs_str(global)))
                put(sys, member, v);
        };
        link("Console", "Console");
        link("Math", "Math");
        link("Convert", "Convert");
        link("DateTime", "DateTime");
        link("String", "String");
        link("Exception", "Exception");
        link("InvalidOperationException", "InvalidOperationException");
        link("NotSupportedException", "NotSupportedException");
        link("ArgumentException", "ArgumentException");
        link("Guid", "Guid");
        link("Task", "Task");
        link("ValueTask", "ValueTask");
        link("Int32", "Int32");
        link("Int64", "Int64");
        link("Double", "Double");
        link("Single", "float");
        link("Boolean", "Boolean");
        link("Char", "Char");
        link("Byte", "byte");
        link("Object", "Object");
        link("Void", "void");
        // System.Collections.Generic / System.Text.Json subtrees
        CsRef coll = cs_native("System.Collections", false);
        CsRef gen = cs_native("System.Collections.Generic", false);
        put(gen, "List", dict_get(g, cs_str("List")));
        put(gen, "Dictionary", dict_get(g, cs_str("Dictionary")));
        put(gen, "HashSet", dict_get(g, cs_str("HashSet")));
        put(gen, "KeyValuePair",
            cs_native("System.Collections.Generic.KeyValuePair`2", false));
        put(coll, "Generic", gen);
        put(sys, "Collections", coll);
        CsRef linq = cs_native("System.Linq", false);
        put(linq, "Enumerable", dict_get(g, cs_str("Enumerable")));
        put(sys, "Linq", linq);
        CsRef threading = cs_native("System.Threading", false);
        CsRef tasks = cs_native("System.Threading.Tasks", false);
        put(tasks, "Task", dict_get(g, cs_str("Task")));
        put(tasks, "ValueTask", dict_get(g, cs_str("ValueTask")));
        put(threading, "Tasks", tasks);
        put(sys, "Threading", threading);
        CsRef text = cs_native("System.Text", false);
        CsRef json = cs_native("System.Text.Json", false);
        put(text, "Json", json);
        put(sys, "Text", text);
        // JsonSerializer facade (impl in stdlib2 — placeholder member ref)
        put(json, "JsonSerializer",
            cs_native("System.Text.Json.JsonSerializer", false));
        // System.IO → hard NO marker (feature "io"): member access raises
        CsRef io = cs_native("System.IO", false);
        putfn(io, "__blocked__", [](interpreter& i, const cs_args&) -> CsRef {
            i.record_feature("io");
            i.raise_exc("NotSupportedException",
                        "System.IO is outside the csmini subset (feature 'io')");
        });
        put(sys, "IO", io);
        inject("System", sys);
    }

    // ── Environment (lite) ──────────────────────────────────────────────
    {
        CsRef env = cs_native("System.Environment", false);
        put(env, "NewLine", cs_str("\n"));
        putfn(env, "GetEnvironmentVariable",
              [](interpreter& i, const cs_args& a) {
                  auto* s = as_str(take(a, 0));
                  if (!s)
                      return cs_null();
                  const char* v = std::getenv(s->v.c_str());
                  return v ? cs_str(v) : cs_null();
              });
        inject("Environment", env);
    }
}

// stdlib/stdlib2 installers live in their own TUs.
} // namespace sao::plugins::csmini
