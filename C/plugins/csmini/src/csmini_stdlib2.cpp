// csmini_stdlib2.cpp — JsonSerializer + remaining builtin installers
// (mirrors pymini_stdlib2.cpp role).
//
// JsonSerializer mirrors System.Text.Json minimal: Serialize(v) / Deserialize
// / Deserialize<T>(s).  cs→json: null/bool/int/double/string/char/array→arr,
// dict→obj (string keys), instance→attrs dict, else ToString.  json→cs:
// obj→dict(str keys), arr→array, primitives direct.
#include "csmini_interp.h"

#include <nlohmann/json.hpp>

namespace sao::plugins::csmini {
namespace {

using njson = nlohmann::json;

njson cs_to_json(interpreter& i, const CsRef& v) {
    if (!v)
        return njson();
    switch (v->kind) {
    case cs_kind::null_:
        return njson();
    case cs_kind::boolean:
        return njson(as_bool(v)->v);
    case cs_kind::integer:
    case cs_kind::char_:
        return njson(as_int(v) ? as_int(v)->v : as_char(v)->v);
    case cs_kind::number:
        return njson(as_float(v)->v);
    case cs_kind::string:
        return njson(as_str(v)->v);
    case cs_kind::array: {
        njson out = njson::array();
        for (const CsRef& x : as_array(v)->v)
            out.push_back(cs_to_json(i, x));
        return out;
    }
    case cs_kind::dict: {
        njson out = njson::object();
        for (const auto& [k, x] : as_dict(v)->items)
            out[cs_to_str(i, k)] = cs_to_json(i, x);
        return out;
    }
    case cs_kind::instance: {
        // public-instance → { "field": value, ... }
        njson out = njson::object();
        if (auto* d = as_dict(as_inst(v)->attrs))
            for (const auto& [k, x] : d->items)
                out[cs_to_str(i, k)] = cs_to_json(i, x);
        return out;
    }
    case cs_kind::exception_: {
        auto* e = as_exc(v);
        njson out = njson::object();
        out["$type"] = e->type_name;
        out["Message"] = e->message;
        return out;
    }
    default:
        return njson(cs_to_str(i, v));
    }
}

// shared with csmini_host payload decode (declared in csmini_interp.h)
CsRef json_to_cs(const njson& j) {
    if (j.is_null())
        return cs_null();
    if (j.is_boolean())
        return cs_bool(j.get<bool>());
    if (j.is_number_integer())
        return cs_int(j.get<int64_t>());
    if (j.is_number_unsigned())
        return cs_int(static_cast<int64_t>(j.get<uint64_t>()));
    if (j.is_number_float())
        return cs_float(j.get<double>());
    if (j.is_string())
        return cs_str(j.get<std::string>());
    if (j.is_array()) {
        auto out = cs_array();
        for (const auto& x : j)
            as_array(out)->v.push_back(json_to_cs(x));
        return out;
    }
    if (j.is_object()) {
        auto out = cs_dict();
        for (auto it = j.begin(); it != j.end(); ++it)
            dict_set(out, cs_str(it.key()), json_to_cs(it.value()));
        return out;
    }
    return cs_null();
}

CsRef take(const cs_args& a, std::size_t k) {
    return k < a.size() ? a.pos[k] : cs_null();
}

} // namespace

// public json → CsRef entry (csmini_host payload decode).
CsRef csmini_json_to_cs(const nlohmann::json& j) { return json_to_cs(j); }

// `new Exception(msg)` — exception construction path shared by new_expr eval
// and builtins raising typed exceptions.
CsRef csmini_new_exception_type(interpreter& i, const std::string& type_text,
                                const cs_args& args, src_pos pos) {
    cs_args a = args;
    return i.new_instance_eval(type_text, a, pos, *i.cur_frame);
}

void csmini_install_stdlib2(interpreter& i) {
    auto* g = as_dict(i.globals);
    // JsonSerializer — attach impls to the facade planted by builtins under
    // System.Text.Json.JsonSerializer (global name registered directly too).
    CsRef ser = cs_native("System.Text.Json.JsonSerializer", false);
    auto* sm = as_dict(as_native(ser)->members);
    dict_set(sm, cs_str("Serialize"),
             cs_builtin("JsonSerializer.Serialize",
                        [](interpreter& interp, const cs_args& a) {
                            const njson j = cs_to_json(interp, take(a, 0));
                            return cs_str(j.dump());
                        }));
    dict_set(sm, cs_str("Deserialize"),
             cs_builtin("JsonSerializer.Deserialize",
                        [](interpreter& interp, const cs_args& a) {
                            CsRef src = take(a, 0);
                            auto* s = as_str(src);
                            try {
                                const njson j =
                                    njson::parse(s ? s->v : "null");
                                return json_to_cs(j);
                            } catch (const std::exception& e) {
                                interp.raise_exc(
                                    "FormatException",
                                    std::string("json parse: ") + e.what());
                            }
                        }));
    // Deserialize<T> generic call loses the <T> in the subset parser (named-
    // arg/type-arg lexing) — but `JsonSerializer.Deserialize` resolves by
    // name either way.
    dict_set(g, cs_str("JsonSerializer"), ser);
    if (CsRef sys = dict_get(g, cs_str("System"))) {
        if (auto* sysn = as_native(sys)) {
            if (CsRef text = dict_get(as_dict(sysn->members), cs_str("Text"))) {
                if (CsRef json = dict_get(as_dict(as_native(text)->members),
                                          cs_str("Json"))) {
                    dict_set(as_dict(as_native(json)->members),
                             cs_str("JsonSerializer"), ser);
                }
            }
        }
    }
}

} // namespace sao::plugins::csmini
