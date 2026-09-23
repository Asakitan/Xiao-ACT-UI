// csmini_ops.cpp — operator semantics: binary/unary, casts, subscripts,
// assignment + ++/-- lvalue handling (mirrors pymini_ops.cpp role).
//
// Numeric model: int64 + double only.  int∘int → int (integer division
// truncates like C#), any float → double.  `+` on strings concatenates
// via C# ToString semantics.  Compound-op `op_eq` tokens arrive as their
// plain binary forms (parser maps `+=`→`+` inside binop when it wraps an
// assignment) — see interpreter::eval et::binop for assign folding.
#include "csmini_interp.h"

#include <cmath>

namespace sao::plugins::csmini {
namespace {

bool as_num(const CsRef& v, double* out) {
    bool ok = false;
    *out = cs_to_float(v, &ok);
    return ok;
}
bool as_i64(const CsRef& v, int64_t* out) {
    bool ok = false;
    *out = cs_to_int(v, &ok);
    return ok;
}

// + concat rule: either side string/stringable → C# `+` concat
bool is_concat(tok_kind op, const CsRef& a, const CsRef& b) {
    if (op != tok_kind::plus)
        return false;
    return (a && a->kind == cs_kind::string) ||
           (b && b->kind == cs_kind::string);
}

} // namespace

CsRef interpreter::binary(tok_kind op, const CsRef& a, const CsRef& b,
                          src_pos pos) {
    // parser packs compound assigns as their op kind + assign wrapper — the
    // assign wrapper lives at stmt level; here only true binary ops reach.
    switch (op) {
    case tok_kind::eq:
        return cs_bool(cs_eq(a, b));
    case tok_kind::ne:
        return cs_bool(!cs_eq(a, b));
    case tok_kind::plus: {
        if (is_concat(op, a, b))
            return cs_str(cs_to_str(*this, a) + cs_to_str(*this, b));
        if (a && a->kind == cs_kind::integer && b && b->kind == cs_kind::integer)
            return cs_int(as_int(a)->v + as_int(b)->v);
        double da, db;
        if (as_num(a, &da) && as_num(b, &db))
            return cs_float(da + db);
        break;
    }
    case tok_kind::minus: {
        if (a && a->kind == cs_kind::integer && b && b->kind == cs_kind::integer)
            return cs_int(as_int(a)->v - as_int(b)->v);
        double da, db;
        if (as_num(a, &da) && as_num(b, &db))
            return cs_float(da - db);
        break;
    }
    case tok_kind::star: {
        if (a && a->kind == cs_kind::integer && b && b->kind == cs_kind::integer)
            return cs_int(as_int(a)->v * as_int(b)->v);
        double da, db;
        if (as_num(a, &da) && as_num(b, &db))
            return cs_float(da * db);
        break;
    }
    case tok_kind::slash: {
        // C# integer division truncates toward zero
        if (a && a->kind == cs_kind::integer && b && b->kind == cs_kind::integer) {
            if (as_int(b)->v == 0)
                raise_exc("DivideByZeroException", "division by zero", pos);
            return cs_int(as_int(a)->v / as_int(b)->v);
        }
        double da, db;
        if (as_num(a, &da) && as_num(b, &db)) {
            if (db == 0.0)
                raise_exc("DivideByZeroException", "division by zero", pos);
            return cs_float(da / db);
        }
        break;
    }
    case tok_kind::percent: {
        if (a && a->kind == cs_kind::integer && b && b->kind == cs_kind::integer) {
            if (as_int(b)->v == 0)
                raise_exc("DivideByZeroException", "remainder by zero", pos);
            return cs_int(as_int(a)->v % as_int(b)->v);
        }
        double da, db;
        if (as_num(a, &da) && as_num(b, &db)) {
            if (db == 0.0)
                raise_exc("DivideByZeroException", "remainder by zero", pos);
            return cs_float(std::fmod(da, db));
        }
        break;
    }
    case tok_kind::lt:
    case tok_kind::le:
    case tok_kind::gt:
    case tok_kind::ge: {
        // string compare is ordinal
        if (a && b && a->kind == cs_kind::string && b->kind == cs_kind::string) {
            const int c = as_str(a)->v.compare(as_str(b)->v);
            if (op == tok_kind::lt)
                return cs_bool(c < 0);
            if (op == tok_kind::le)
                return cs_bool(c <= 0);
            if (op == tok_kind::gt)
                return cs_bool(c > 0);
            return cs_bool(c >= 0);
        }
        int64_t ia, ib;
        if (a && b && as_i64(a, &ia) && as_i64(b, &ib) &&
            a->kind == cs_kind::integer && b->kind == cs_kind::integer) {
            if (op == tok_kind::lt)
                return cs_bool(ia < ib);
            if (op == tok_kind::le)
                return cs_bool(ia <= ib);
            if (op == tok_kind::gt)
                return cs_bool(ia > ib);
            return cs_bool(ia >= ib);
        }
        double da, db;
        if (as_num(a, &da) && as_num(b, &db)) {
            if (op == tok_kind::lt)
                return cs_bool(da < db);
            if (op == tok_kind::le)
                return cs_bool(da <= db);
            if (op == tok_kind::gt)
                return cs_bool(da > db);
            return cs_bool(da >= db);
        }
        break;
    }
    case tok_kind::amp:
    case tok_kind::pipe:
    case tok_kind::caret:
    case tok_kind::lshift:
    case tok_kind::rshift: {
        // bool & | ^ are C# logical ops too
        if ((op == tok_kind::amp || op == tok_kind::pipe || op == tok_kind::caret) &&
            a && b && a->kind == cs_kind::boolean && b->kind == cs_kind::boolean) {
            const bool x = as_bool(a)->v, y = as_bool(b)->v;
            if (op == tok_kind::amp)
                return cs_bool(x && y);
            if (op == tok_kind::pipe)
                return cs_bool(x || y);
            return cs_bool(x != y);
        }
        int64_t ia, ib;
        if (as_i64(a, &ia) && as_i64(b, &ib)) {
            switch (op) {
            case tok_kind::amp: return cs_int(ia & ib);
            case tok_kind::pipe: return cs_int(ia | ib);
            case tok_kind::caret: return cs_int(ia ^ ib);
            case tok_kind::lshift: return cs_int(ia << (ib & 63));
            case tok_kind::rshift: return cs_int(ia >> (ib & 63));
            default: break;
            }
        }
        break;
    }
    // compound-assign ops reach binary() with the underlying op already
    // mapped by eval (statement assign path).  If a raw *_eq leaks in,
    // decode it here for safety.
    case tok_kind::plus_eq: return binary(tok_kind::plus, a, b, pos);
    case tok_kind::minus_eq: return binary(tok_kind::minus, a, b, pos);
    case tok_kind::star_eq: return binary(tok_kind::star, a, b, pos);
    case tok_kind::slash_eq: return binary(tok_kind::slash, a, b, pos);
    case tok_kind::percent_eq: return binary(tok_kind::percent, a, b, pos);
    case tok_kind::amp_eq: return binary(tok_kind::amp, a, b, pos);
    case tok_kind::pipe_eq: return binary(tok_kind::pipe, a, b, pos);
    case tok_kind::caret_eq: return binary(tok_kind::caret, a, b, pos);
    case tok_kind::lshift_eq: return binary(tok_kind::lshift, a, b, pos);
    case tok_kind::rshift_eq: return binary(tok_kind::rshift, a, b, pos);
    default:
        break;
    }
    raise_exc("InvalidOperationException",
              std::string("bad operands for binary op ") +
                  std::to_string(static_cast<int>(op)),
              pos);
}

CsRef interpreter::unary(tok_kind op, const CsRef& a, src_pos pos) {
    switch (op) {
    case tok_kind::bang:
        return cs_bool(!cs_truthy(a));
    case tok_kind::minus: {
        if (a && a->kind == cs_kind::integer)
            return cs_int(-as_int(a)->v);
        bool ok = false;
        const double d = cs_to_float(a, &ok);
        if (ok)
            return cs_float(-d);
        break;
    }
    case tok_kind::plus: {
        if (a && (a->kind == cs_kind::integer || a->kind == cs_kind::number))
            return a;
        break;
    }
    case tok_kind::tilde: {
        bool ok = false;
        const int64_t i = cs_to_int(a, &ok);
        if (ok)
            return cs_int(~i);
        break;
    }
    default:
        break;
    }
    raise_exc("InvalidOperationException",
              "bad operand for unary op", pos);
}

// ── subscripts ────────────────────────────────────────────────────────────
CsRef interpreter::subscript_get(const CsRef& obj, const CsRef& index,
                                 src_pos pos) {
    if (!obj) {
        raise_exc("NullReferenceException", "index on null", pos);
    }
    if (auto* arr = as_array(obj)) {
        if (arr->is_set())
            raise_exc("InvalidOperationException",
                      "HashSet does not support indexing", pos);
        bool ok = false;
        const int64_t i = cs_to_int(index, &ok);
        if (!ok || i < 0 || static_cast<std::size_t>(i) >= arr->v.size())
            raise_exc("IndexOutOfRangeException", "array index out of range",
                      pos);
        return arr->v[static_cast<std::size_t>(i)];
    }
    if (auto* d = as_dict(obj)) {
        CsRef v = dict_get(d, index);
        if (!v)
            raise_exc("KeyNotFoundException",
                      "key not found: " + cs_to_str(*this, index), pos);
        return v;
    }
    if (auto* s = as_str(obj)) {
        bool ok = false;
        const int64_t i = cs_to_int(index, &ok);
        if (!ok || i < 0 || static_cast<std::size_t>(i) >= s->v.size())
            raise_exc("IndexOutOfRangeException", "string index out of range",
                      pos);
        return cs_char(
            static_cast<unsigned char>(s->v[static_cast<std::size_t>(i)]));
    }
    if (auto* n = as_native(obj)) {
        // native indexer support — only dict-like natives (members dict)
        if (auto* md = as_dict(n->members)) {
            CsRef v = dict_get_ci(md, index);
            if (v)
                return v;
        }
    }
    raise_exc("InvalidOperationException",
              std::string("type '") + cs_type_name(obj) +
                  "' is not indexable",
              pos);
}

void interpreter::subscript_set(const CsRef& obj, const CsRef& index,
                                CsRef value, src_pos pos) {
    if (!obj)
        raise_exc("NullReferenceException", "index-set on null", pos);
    if (auto* arr = as_array(obj)) {
        if (arr->is_set())
            raise_exc("InvalidOperationException",
                      "HashSet does not support indexed assignment", pos);
        bool ok = false;
        const int64_t i = cs_to_int(index, &ok);
        if (!ok || i < 0 || static_cast<std::size_t>(i) >= arr->v.size())
            raise_exc("IndexOutOfRangeException", "array index out of range",
                      pos);
        arr->v[static_cast<std::size_t>(i)] = std::move(value);
        return;
    }
    if (auto* d = as_dict(obj)) {
        dict_set(d, index, std::move(value));
        return;
    }
    raise_exc("InvalidOperationException",
              std::string("type '") + cs_type_name(obj) +
                  "' is not indexable",
              pos);
}

// ── assignment / ++-- targets ─────────────────────────────────────────────
CsRef interpreter::assign_target(ast_expr* target, CsRef value, frame& f) {
    if (!target)
        return value;
    switch (target->tag) {
    case et::name: {
        if (!scope_assign(f, target->name, value)) {
            // unresolved name → write instance field when this exists,
            // else declare a local in the current scope (C# would reject;
            // the subset promotes to a writable slot for usability).
            if (f.this_ref) {
                if (setattr(f.this_ref, target->name, value))
                    return value;
            }
            if (auto* co = f.class_ref ? as_class(f.class_ref) : nullptr) {
                if (dict_get(as_dict(co->attrs), cs_str(target->name))) {
                    if (setattr(f.class_ref, target->name, value))
                        return value;
                }
            }
            if (dict_get(as_dict(globals), cs_str(target->name))) {
                dict_set(as_dict(globals), cs_str(target->name), value);
                return value;
            }
            scope_set(f, target->name, value);
        }
        return value;
    }
    case et::member: {
        CsRef base = eval(target->base.get(), f);
        if (!setattr(base, target->name, value))
            raise_exc("MissingMemberException",
                      "cannot assign member '" + target->name + "'",
                      target->pos);
        return value;
    }
    case et::index: {
        CsRef base = eval(target->base.get(), f);
        CsRef ix = eval(target->index.get(), f);
        subscript_set(base, ix, value, target->pos);
        return value;
    }
    default:
        raise_exc("InvalidOperationException", "invalid assignment target",
                  target->pos);
    }
}

CsRef interpreter::incdec_target(ast_expr* target, int64_t delta, bool post,
                                 frame& f) {
    CsRef cur_v = eval(target, f);
    bool ok = false;
    const int64_t i = cs_to_int(cur_v, &ok);
    if (!ok)
        raise_exc("InvalidOperationException", "++/-- on non-numeric value",
                  target->pos);
    CsRef next = cs_int(i + delta);
    (void)assign_target(target, next, f);
    return post ? cur_v : next;
}

// ── casts ─────────────────────────────────────────────────────────────────
CsRef interpreter::cast_value(const std::string& type_text, const CsRef& v,
                              src_pos pos) {
    const std::string& t = type_text;
    if (t == "int" || t == "long" || t == "short" || t == "byte" ||
        t == "sbyte" || t == "ushort" || t == "uint" || t == "ulong" ||
        t == "nint" || t == "nuint" || t == "Int32" || t == "Int64") {
        bool ok = false;
        const int64_t i = cs_to_int(v, &ok);
        if (!ok)
            raise_exc("InvalidCastException",
                      "cannot cast to " + t, pos);
        return cs_int(i);
    }
    if (t == "double" || t == "float" || t == "decimal" || t == "Double" ||
        t == "Single") {
        bool ok = false;
        const double d = cs_to_float(v, &ok);
        if (!ok)
            raise_exc("InvalidCastException",
                      "cannot cast to " + t, pos);
        return cs_float(d);
    }
    if (t == "bool" || t == "Boolean")
        return cs_bool(cs_truthy(v));
    if (t == "string" || t == "String")
        return cs_str(cs_to_str(*this, v));
    if (t == "char" || t == "Char") {
        bool ok = false;
        const int64_t i = cs_to_int(v, &ok);
        if (ok)
            return cs_char(i);
        if (auto* s = as_str(v); s && !s->v.empty())
            return cs_char(static_cast<unsigned char>(s->v[0]));
        raise_exc("InvalidCastException", "cannot cast to char", pos);
    }
    if (t == "object" || t == "var" || t == "dynamic")
        return v;
    // class-instance cast — strict-type check is skipped (subset): return
    // the value unchanged; runtime member access resolves dynamically.
    return v;
}

} // namespace sao::plugins::csmini
