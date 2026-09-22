#include "sao/plugins/angel_host/as_stdlib.h"

#include "as_generic_bindings_internal.h"
#include "sao/plugins/sdk_binding/binding_common.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

#if defined(SAO_HAS_ANGELSCRIPT_ADDONS)
#include <datetime/datetime.h>
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>
#include <scriptmath/scriptmath.h>
#endif

namespace sao::plugins::angel_host {

#if defined(SAO_HAS_ANGELSCRIPT)
namespace {

using ordered_json = nlohmann::ordered_json;

bool shared_json_text_valid(std::string_view text) noexcept {
    return !text.empty() && sdk_binding::sao_plugins_binding_validate_json_text(
                                reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool shared_json_value_valid(const ordered_json& value, std::string& serialized) noexcept {
    try { serialized = value.dump(); return shared_json_text_valid(serialized); }
    catch (...) { serialized.clear(); return false; }
}

struct json_value {
    std::atomic_uint32_t references{1};
    ordered_json value = nullptr;
};

enum class stdlib_install_phase {
    installing,
    installed,
    failed,
};

struct stdlib_install_state {
    stdlib_install_phase phase = stdlib_install_phase::installing;
    int32_t status = SAO_ERR_NOT_INITIALIZED;
};

std::mutex g_stdlib_install_mutex;
constexpr asPWORD kStdlibInstallStateSlot = static_cast<asPWORD>(0x53414F4153535444ULL);

bool registration_ok(int result) {
    return result >= 0;
}

void set_active_exception(const char* message) noexcept {
    if (auto* context = asGetActiveContext(); context != nullptr) {
        (void)context->SetException(message);
    }
}

template <typename Callback>
void json_callback_barrier(const char* message, Callback&& callback) noexcept {
    try {
        callback();
    } catch (...) {
        set_active_exception(message);
    }
}

void stdlib_state_cleanup(asIScriptEngine* engine) {
    if (engine == nullptr)
        return;
    auto* state = static_cast<stdlib_install_state*>(engine->GetUserData(kStdlibInstallStateSlot));
    (void)engine->SetUserData(nullptr, kStdlibInstallStateSlot);
    delete state;
}

void json_factory(asIScriptGeneric* generic) {
    json_callback_barrier("json allocation failed", [generic] {
        *static_cast<json_value**>(generic->GetAddressOfReturnLocation()) = new json_value();
    });
}

void json_add_ref(asIScriptGeneric* generic) {
    json_callback_barrier("json add-ref failed", [generic] {
        auto* value = static_cast<json_value*>(generic->GetObject());
        if (value != nullptr)
            value->references.fetch_add(1, std::memory_order_relaxed);
    });
}

void json_release(asIScriptGeneric* generic) {
    json_callback_barrier("json release failed", [generic] {
        auto* value = static_cast<json_value*>(generic->GetObject());
        if (value != nullptr && value->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete value;
    });
}

void json_parse(asIScriptGeneric* generic) {
    json_callback_barrier("json_parse failed", [generic] {
        const auto* text = static_cast<const std::string*>(generic->GetArgAddress(0));
        if (text == nullptr) {
            set_active_exception("json_parse received a null string");
            return;
        }
        if (!shared_json_text_valid(*text)) { set_active_exception("JSON exceeds the shared complexity or UTF-8 budget"); return; }
        ordered_json parsed = ordered_json::parse(*text, nullptr, false);
        if (parsed.is_discarded()) {
            set_active_exception("invalid JSON");
            return;
        }
        auto* value = new json_value();
        value->value = std::move(parsed);
        *static_cast<json_value**>(generic->GetAddressOfReturnLocation()) = value;
    });
}

void json_stringify(asIScriptGeneric* generic) {
    json_callback_barrier("json serialization failed", [generic] {
        const auto* value = static_cast<const json_value*>(generic->GetObject());
        std::string serialized;
        if (value != nullptr && !shared_json_value_valid(value->value, serialized)) { set_active_exception("JSON exceeds the shared complexity or UTF-8 budget"); return; }
        new (generic->GetAddressOfReturnLocation())
            std::string(value == nullptr ? "null" : std::move(serialized));
    });
}

const ordered_json* json_member(const json_value* object, const std::string* key) {
    if (object == nullptr || key == nullptr || !object->value.is_object()) {
        return nullptr;
    }
    const auto found = object->value.find(*key);
    return found == object->value.end() ? nullptr : &*found;
}

void json_contains(asIScriptGeneric* generic) {
    json_callback_barrier("json contains failed", [generic] {
        const auto* object = static_cast<const json_value*>(generic->GetObject());
        const auto* key = static_cast<const std::string*>(generic->GetArgAddress(0));
        generic->SetReturnByte(json_member(object, key) == nullptr ? 0 : 1);
    });
}

void json_size(asIScriptGeneric* generic) {
    json_callback_barrier("json size failed", [generic] {
        const auto* object = static_cast<const json_value*>(generic->GetObject());
        generic->SetReturnDWord(object == nullptr ? 0 : static_cast<asDWORD>(object->value.size()));
    });
}

void json_get_string(asIScriptGeneric* generic) {
    json_callback_barrier("json string access failed", [generic] {
        const auto* object = static_cast<const json_value*>(generic->GetObject());
        const auto* key = static_cast<const std::string*>(generic->GetArgAddress(0));
        const auto* fallback = static_cast<const std::string*>(generic->GetArgAddress(1));
        const ordered_json* member = json_member(object, key);
        new (generic->GetAddressOfReturnLocation())
            std::string(member != nullptr && member->is_string()
                            ? member->get<std::string>()
                            : (fallback == nullptr ? std::string{} : *fallback));
    });
}

void json_get_int(asIScriptGeneric* generic) {
    json_callback_barrier("json integer access failed", [generic] {
        const auto* object = static_cast<const json_value*>(generic->GetObject());
        const auto* key = static_cast<const std::string*>(generic->GetArgAddress(0));
        const ordered_json* member = json_member(object, key);
        generic->SetReturnQWord(member != nullptr && member->is_number_integer()
                                    ? static_cast<asQWORD>(member->get<int64_t>())
                                    : generic->GetArgQWord(1));
    });
}

void json_get_number(asIScriptGeneric* generic) {
    json_callback_barrier("json number access failed", [generic] {
        const auto* object = static_cast<const json_value*>(generic->GetObject());
        const auto* key = static_cast<const std::string*>(generic->GetArgAddress(0));
        const ordered_json* member = json_member(object, key);
        generic->SetReturnDouble(member != nullptr && member->is_number()
                                     ? member->get<double>()
                                     : generic->GetArgDouble(1));
    });
}

void json_get_bool(asIScriptGeneric* generic) {
    json_callback_barrier("json boolean access failed", [generic] {
        const auto* object = static_cast<const json_value*>(generic->GetObject());
        const auto* key = static_cast<const std::string*>(generic->GetArgAddress(0));
        const ordered_json* member = json_member(object, key);
        generic->SetReturnByte(member != nullptr && member->is_boolean()
                                   ? static_cast<asBYTE>(member->get<bool>() ? 1 : 0)
                                   : generic->GetArgByte(1));
    });
}

// Legacy implicit scalar conversions — old scripts assign json@ straight into
// bool/int/double/string locals, so the json type exposes opImplConv overloads.
void json_op_conv_bool(asIScriptGeneric* generic) {
    json_callback_barrier("json bool conversion failed", [generic] {
        const auto* object = static_cast<const json_value*>(generic->GetObject());
        bool out = false;
        if (object != nullptr) {
            const ordered_json& value = object->value;
            if (value.is_boolean())
                out = value.get<bool>();
            else if (value.is_number())
                out = value.get<double>() != 0.0;
            else if (value.is_string()) {
                const std::string& text = value.get_ref<const std::string&>();
                out = !(text.empty() || text == "0" || text == "false" || text == "null");
            } else if (!value.is_null()) {
                out = true;
            }
        }
        generic->SetReturnByte(static_cast<asBYTE>(out ? 1 : 0));
    });
}

void json_op_conv_int(asIScriptGeneric* generic) {
    json_callback_barrier("json integer conversion failed", [generic] {
        const auto* object = static_cast<const json_value*>(generic->GetObject());
        int64_t out = 0;
        if (object != nullptr) {
            const ordered_json& value = object->value;
            if (value.is_number_integer() || value.is_number_unsigned())
                out = value.get<int64_t>();
            else if (value.is_number_float())
                out = static_cast<int64_t>(value.get<double>());
            else if (value.is_boolean())
                out = value.get<bool>() ? 1 : 0;
            else if (value.is_string())
                out = static_cast<int64_t>(std::strtoll(
                    value.get_ref<const std::string&>().c_str(), nullptr, 10));
        }
        generic->SetReturnQWord(static_cast<asQWORD>(out));
    });
}

void json_op_conv_number(asIScriptGeneric* generic) {
    json_callback_barrier("json number conversion failed", [generic] {
        const auto* object = static_cast<const json_value*>(generic->GetObject());
        double out = 0.0;
        if (object != nullptr) {
            const ordered_json& value = object->value;
            if (value.is_number())
                out = value.get<double>();
            else if (value.is_boolean())
                out = value.get<bool>() ? 1.0 : 0.0;
            else if (value.is_string())
                out = std::strtod(value.get_ref<const std::string&>().c_str(), nullptr);
        }
        generic->SetReturnDouble(out);
    });
}

void json_op_conv_string(asIScriptGeneric* generic) {
    json_callback_barrier("json string conversion failed", [generic] {
        const auto* object = static_cast<const json_value*>(generic->GetObject());
        std::string out;
        if (object != nullptr) {
            const ordered_json& value = object->value;
            if (value.is_string())
                out = value.get<std::string>();
            else if (value.is_number_float()) {
                const double number = value.get<double>();
                out = number == std::trunc(number)
                          ? std::to_string(static_cast<int64_t>(number))
                          : value.dump();
            } else if (!value.is_null()) {
                out = value.dump();
            }
        }
        new (generic->GetAddressOfReturnLocation()) std::string(std::move(out));
    });
}

void json_set_string(asIScriptGeneric* generic) {
    json_callback_barrier("json string update failed", [generic] {
        auto* object = static_cast<json_value*>(generic->GetObject());
        const auto* key = static_cast<const std::string*>(generic->GetArgAddress(0));
        const auto* value = static_cast<const std::string*>(generic->GetArgAddress(1));
        if (object != nullptr && key != nullptr && value != nullptr) {
            if (!object->value.is_object())
                object->value = ordered_json::object();
            object->value[*key] = *value;
        }
    });
}

void json_set_int(asIScriptGeneric* generic) {
    json_callback_barrier("json integer update failed", [generic] {
        auto* object = static_cast<json_value*>(generic->GetObject());
        const auto* key = static_cast<const std::string*>(generic->GetArgAddress(0));
        if (object != nullptr && key != nullptr) {
            if (!object->value.is_object())
                object->value = ordered_json::object();
            object->value[*key] = static_cast<int64_t>(generic->GetArgQWord(1));
        }
    });
}

void json_set_number(asIScriptGeneric* generic) {
    json_callback_barrier("json number update failed", [generic] {
        auto* object = static_cast<json_value*>(generic->GetObject());
        const auto* key = static_cast<const std::string*>(generic->GetArgAddress(0));
        if (object != nullptr && key != nullptr) {
            if (!object->value.is_object())
                object->value = ordered_json::object();
            object->value[*key] = generic->GetArgDouble(1);
        }
    });
}

void json_set_bool(asIScriptGeneric* generic) {
    json_callback_barrier("json boolean update failed", [generic] {
        auto* object = static_cast<json_value*>(generic->GetObject());
        const auto* key = static_cast<const std::string*>(generic->GetArgAddress(0));
        if (object != nullptr && key != nullptr) {
            if (!object->value.is_object())
                object->value = ordered_json::object();
            object->value[*key] = generic->GetArgByte(1) != 0;
        }
    });
}

// set_json(key, raw): inserts raw JSON text as a *parsed* value (objects,
// arrays, numbers, ...), so generated engine wrappers can embed spec/descriptor
// payloads without flattening them to strings.  Returns false when the raw
// text fails the bounded shared parse — the key is left untouched (fail
// closed) so callers can route through ctx.engine_call(..., raw_json) instead.
void json_set_json(asIScriptGeneric* generic) {
    json_callback_barrier("json raw update failed", [generic] {
        auto* object = static_cast<json_value*>(generic->GetObject());
        const auto* key = static_cast<const std::string*>(generic->GetArgAddress(0));
        const auto* raw = static_cast<const std::string*>(generic->GetArgAddress(1));
        bool ok = false;
        if (object != nullptr && key != nullptr && raw != nullptr &&
            shared_json_text_valid(*raw)) {
            ordered_json parsed = ordered_json::parse(*raw, nullptr, false);
            if (!parsed.is_discarded()) {
                if (!object->value.is_object())
                    object->value = ordered_json::object();
                object->value[*key] = std::move(parsed);
                ok = true;
            }
        }
        generic->SetReturnByte(ok ? 1 : 0);
    });
}

int32_t register_json(asIScriptEngine* engine) {
    if (engine->GetTypeInfoByName("json") != nullptr)
        return SAO_OK;
    int result = engine->RegisterObjectType("json", 0, asOBJ_REF);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectBehaviour("json", asBEHAVE_FACTORY, "json@ f()",
                                             asFUNCTION(json_factory), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectBehaviour("json", asBEHAVE_ADDREF, "void f()",
                                             asFUNCTION(json_add_ref), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectBehaviour("json", asBEHAVE_RELEASE, "void f()",
                                             asFUNCTION(json_release), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "string stringify() const",
                                          asFUNCTION(json_stringify), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "bool contains(const string &in) const",
                                          asFUNCTION(json_contains), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "uint size() const", asFUNCTION(json_size),
                                          asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod(
        "json", "string get_string(const string &in, const string &in) const",
        asFUNCTION(json_get_string), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "int64 get_int(const string &in, int64) const",
                                          asFUNCTION(json_get_int), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result =
        engine->RegisterObjectMethod("json", "double get_number(const string &in, double) const",
                                     asFUNCTION(json_get_number), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "bool get_bool(const string &in, bool) const",
                                          asFUNCTION(json_get_bool), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "bool opImplConv() const",
                                          asFUNCTION(json_op_conv_bool), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "int64 opImplConv() const",
                                          asFUNCTION(json_op_conv_int), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "double opImplConv() const",
                                          asFUNCTION(json_op_conv_number), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "string opImplConv() const",
                                          asFUNCTION(json_op_conv_string), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "void set(const string &in, const string &in)",
                                          asFUNCTION(json_set_string), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "void set(const string &in, int64)",
                                          asFUNCTION(json_set_int), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "void set(const string &in, double)",
                                          asFUNCTION(json_set_number), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json", "void set(const string &in, bool)",
                                          asFUNCTION(json_set_bool), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterObjectMethod("json",
                                          "bool set_json(const string &in, const string &in)",
                                          asFUNCTION(json_set_json), asCALL_GENERIC);
    if (!registration_ok(result))
        return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterGlobalFunction("json@ json_parse(const string &in)",
                                            asFUNCTION(json_parse), asCALL_GENERIC);
    return registration_ok(result) ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
}

// Legacy comparison support for `dict["key"]` results: old scripts compare a
// dictionaryValue directly against `null`, `false` and other scalars. The addon
// only ships opConv/opAssign, so we add a `?` opEquals that coerces both sides.
int64_t dictvalue_other_i64(int type_id, const void* address) noexcept {
    if (address == nullptr)
        return 0;
    switch (type_id) {
    case asTYPEID_BOOL:
    case asTYPEID_INT8:
        return static_cast<int64_t>(*static_cast<const int8_t*>(address));
    case asTYPEID_UINT8:
        return static_cast<int64_t>(*static_cast<const uint8_t*>(address));
    case asTYPEID_INT16:
        return static_cast<int64_t>(*static_cast<const int16_t*>(address));
    case asTYPEID_UINT16:
        return static_cast<int64_t>(*static_cast<const uint16_t*>(address));
    case asTYPEID_INT32:
        return static_cast<int64_t>(*static_cast<const int32_t*>(address));
    case asTYPEID_UINT32:
        return static_cast<int64_t>(*static_cast<const uint32_t*>(address));
    case asTYPEID_UINT64:
        return static_cast<int64_t>(*static_cast<const uint64_t*>(address));
    default:
        return static_cast<int64_t>(*static_cast<const int64_t*>(address));
    }
}

void dictvalue_op_equals(asIScriptGeneric* generic) {
    json_callback_barrier("dictionaryValue compare failed", [generic] {
        const auto* self = static_cast<const CScriptDictValue*>(generic->GetObject());
        asIScriptEngine* engine = generic->GetEngine();
        const int other_id = generic->GetArgTypeId(0);
        // `?&in other` arg slots hold a pointer to the caller's storage —
        // dereference once to reach the actual data.
        void* other_slot = generic->GetAddressOfArg(0);
        void* other_addr =
            other_slot != nullptr ? *static_cast<void**>(other_slot) : nullptr;
        bool equal = false;
        if (self != nullptr) {
            const int self_id = self->GetTypeId();
            if (other_id == asTYPEID_VOID || other_addr == nullptr) {
                equal = self_id == asTYPEID_VOID;
            } else if ((other_id & asTYPEID_MASK_OBJECT) == 0) {
                if (other_id == asTYPEID_DOUBLE || other_id == asTYPEID_FLOAT) {
                    double value = 0.0;
                    const double other =
                        other_id == asTYPEID_DOUBLE
                            ? *static_cast<const double*>(other_addr)
                            : static_cast<double>(*static_cast<const float*>(other_addr));
                    equal = self->Get(engine, value) && value == other;
                } else {
                    asINT64 value = 0;
                    equal = self->Get(engine, value) &&
                            value == static_cast<asINT64>(
                                         dictvalue_other_i64(other_id, other_addr));
                }
            } else {
                const asITypeInfo* info = engine->GetTypeInfoById(other_id);
                const char* type_name = info != nullptr ? info->GetName() : "";
                if (std::strcmp(type_name, "string") == 0) {
                    std::string value;
                    equal = self->Get(engine, &value, other_id) &&
                            value == *static_cast<const std::string*>(other_addr);
                } else if (std::strcmp(type_name, "dictionaryValue") == 0) {
                    const auto* other =
                        static_cast<const CScriptDictValue*>(other_addr);
                    if (other != nullptr && self_id == other->GetTypeId()) {
                        if (self_id == asTYPEID_VOID) {
                            equal = true;
                        } else if ((self_id & asTYPEID_OBJHANDLE) != 0) {
                            equal = *static_cast<void* const*>(
                                        self->GetAddressOfValue()) ==
                                    *static_cast<void* const*>(
                                        other->GetAddressOfValue());
                        } else if ((self_id & asTYPEID_MASK_OBJECT) != 0) {
                            // non-handle objects: only string gets a content
                            // compare — other value types have no portable
                            // storage in dictValue to fetch generically.
                            const int base_self =
                                self_id &
                                ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
                            const asITypeInfo* self_info =
                                engine->GetTypeInfoById(base_self);
                            if (self_info != nullptr &&
                                std::strcmp(self_info->GetName(), "string") == 0) {
                                std::string a;
                                std::string b;
                                equal = self->Get(engine, &a, base_self) &&
                                        other->Get(engine, &b, base_self) && a == b;
                            }
                        } else {
                            asINT64 ai = 0, bi = 0;
                            double ad = 0.0, bd = 0.0;
                            if (self->Get(engine, ai) && other->Get(engine, bi))
                                equal = ai == bi;
                            else if (self->Get(engine, ad) && other->Get(engine, bd))
                                equal = ad == bd;
                        }
                    }
                } else if ((other_id & asTYPEID_OBJHANDLE) != 0) {
                    // other_addr points at the caller's handle variable.
                    const void* other_obj = *static_cast<void* const*>(other_addr);
                    equal = (self_id & asTYPEID_OBJHANDLE) != 0 &&
                            *static_cast<void* const*>(self->GetAddressOfValue()) ==
                                other_obj;
                }
            }
        }
        generic->SetReturnByte(static_cast<asBYTE>(equal ? 1 : 0));
    });
}

int32_t perform_stdlib_install(asIScriptEngine* engine) {
#if !defined(SAO_HAS_ANGELSCRIPT_ADDONS)
    (void)engine;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (engine->GetTypeInfoByName("json") != nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    const int32_t core_status = register_generic_core_bindings(engine);
    if (core_status != SAO_OK)
        return core_status;
    const int32_t json_status = register_json(engine);
    if (json_status != SAO_OK)
        return json_status;

    if (engine->GetTypeInfoByDecl("array<int>") == nullptr)
        RegisterScriptArray(engine, true);
    if (engine->GetTypeInfoByName("dictionary") == nullptr)
        RegisterScriptDictionary(engine);
    // Legacy `dict["key"] == scalar|null` comparisons.
    if (engine->GetTypeInfoByName("dictionaryValue") != nullptr) {
        const int dict_eq = engine->RegisterObjectMethod(
            "dictionaryValue", "bool opEquals(?&in other) const",
            asFUNCTION(dictvalue_op_equals), asCALL_GENERIC);
        if (dict_eq != asALREADY_REGISTERED && !registration_ok(dict_eq))
            return SAO_ERR_OS_CALL_FAILED;
    }
    if (engine->GetGlobalFunctionByDecl("float cos(float)") == nullptr)
        RegisterScriptMath(engine);
    if (engine->GetTypeInfoByName("datetime") == nullptr)
        RegisterScriptDateTime(engine);

    // Canonical ctx.* surface — registered after dictionary/array/json exist.
    const int32_t ctx_status = register_ctx_surface_bindings(engine);
    if (ctx_status != SAO_OK)
        return ctx_status;

    return engine->GetTypeInfoByName("string") != nullptr &&
                   engine->GetTypeInfoByName("PluginContext") != nullptr &&
                   engine->GetTypeInfoByName("json") != nullptr &&
                   engine->GetTypeInfoByDecl("array<int>") != nullptr &&
                   engine->GetTypeInfoByName("dictionary") != nullptr &&
                   engine->GetGlobalFunctionByDecl("float cos(float)") != nullptr &&
                   engine->GetTypeInfoByName("datetime") != nullptr
               ? SAO_OK
               : SAO_ERR_OS_CALL_FAILED;
#endif
}

} // namespace
#endif

#if defined(SAO_HAS_ANGELSCRIPT)

// json@ bridging helpers for the ctx surface (json_value lives in the
// anonymous namespace above — these are the only cross-TU entry points).
void* json_ref_from_ordered(asIScriptEngine* engine,
                            const nlohmann::ordered_json& value) noexcept {
    (void)engine;
    try {
        std::string serialized;
        if (!shared_json_value_valid(value, serialized))
            return nullptr;
        auto* object = new json_value();
        object->value = value;
        return object;
    } catch (...) {
        return nullptr;
    }
}

const nlohmann::ordered_json* json_ref_value(const void* object) noexcept {
    const auto* value = static_cast<const json_value*>(object);
    return value == nullptr ? nullptr : &value->value;
}

#endif

extern "C" SAO_PLUGINS_API
    int32_t SAO_PLUGINS_CALL sao_plugins_ashost_install_stdlib(asIScriptEngine* engine) {
#if defined(SAO_HAS_ANGELSCRIPT)
    if (engine == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    engine_execution_guard engine_lock;
    try {
        std::lock_guard lock(g_stdlib_install_mutex);
        auto* state =
            static_cast<stdlib_install_state*>(engine->GetUserData(kStdlibInstallStateSlot));
        if (state != nullptr)
            return state->status;

        auto pending = std::make_unique<stdlib_install_state>();
        state = pending.get();
        void* previous = engine->SetUserData(state, kStdlibInstallStateSlot);
        if (previous != nullptr) {
            (void)engine->SetUserData(previous, kStdlibInstallStateSlot);
            return SAO_ERR_OS_CALL_FAILED;
        }
        engine->SetEngineUserDataCleanupCallback(stdlib_state_cleanup, kStdlibInstallStateSlot);
        pending.release();

        state->status = perform_stdlib_install(engine);
        state->phase = state->status == SAO_OK ? stdlib_install_phase::installed
                                               : stdlib_install_phase::failed;
        return state->status;
    } catch (...) {
        std::lock_guard lock(g_stdlib_install_mutex);
        auto* state =
            static_cast<stdlib_install_state*>(engine->GetUserData(kStdlibInstallStateSlot));
        if (state != nullptr) {
            state->phase = stdlib_install_phase::failed;
            state->status = SAO_ERR_OS_CALL_FAILED;
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    (void)engine;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

} // namespace sao::plugins::angel_host
