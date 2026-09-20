// as_ctx_surface.cpp — canonical `ctx.*` binding surface for AngelScript.
//
// Binds the full Python-compatible ctx object (~70 members) on PluginContext:
// properties, event/settings registration, timers/hotkeys, render hooks,
// extension/metadata registration, v3 entity providers for menus + action
// handlers, notify/io dialogs, compositor layers, engine registry, mem
// accessor (SaoSdkContext memory provider), ui spec builder (script_ctx) and
// ctx.load_local routed through the script_ctx runtime bridge (this file also
// registers the "angelscript" engine provider that execs .as helper modules
// on the caller's own engine).
//
// Threading mirrors as_module_bridge.cpp: every script call / callback runs
// under engine_execution_guard; callback records quiesce on release; all
// AS-visible native records outlive callback teardown via a graveyard so
// loader-side user_data can never dangle before context teardown completes.

#include "as_generic_bindings_internal.h"

#include "as_plugin_internal.h"

#include "sao/plugins/angel_host/as_call.h"
#include "sao/plugins/angel_host/as_gpu_hunt_bind.h"
#include "sao/plugins/angel_host/as_module_bridge.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/script_ctx/ctx_surface.h"
#include "sao/plugins/script_ctx/runtime_bridge.h"
#include "sao/plugins/script_ctx/script_ui.h"
#include "sao/sdk/sao_sdk_context.h"
#include "sao/sdk/sao_sdk_mem.h"
#include "sao_plugins/sao_status.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#ifdef GetObject
#undef GetObject
#endif
#endif

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

#if defined(SAO_HAS_ANGELSCRIPT_ADDONS)
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>
#endif

namespace sao::plugins::angel_host {

#if defined(SAO_HAS_ANGELSCRIPT) && defined(SAO_HAS_ANGELSCRIPT_ADDONS)

namespace {

using ordered_json = nlohmann::ordered_json;
using as_string = as_string_value;
namespace loader_ns = sao::plugins::loader;
namespace sc = sao::plugins::script_ctx;

// ──────────────────────────────────────────────────────────────────────────
// shared helpers
// ──────────────────────────────────────────────────────────────────────────

bool registration_ok(int result) noexcept {
    return result >= 0;
}

void set_active_exception(const char* message) noexcept {
    if (auto* context = asGetActiveContext(); context != nullptr) {
        (void)context->SetException(message);
    }
}

std::string utf8_from_wide(const wchar_t* value) {
    if (value == nullptr)
        return {};
#if defined(_WIN32)
    const int required =
        ::WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string output(static_cast<size_t>(required - 1), '\0');
    if (required > 1) {
        (void)::WideCharToMultiByte(CP_UTF8, 0, value, -1, output.data(), required, nullptr,
                                    nullptr);
    }
    return output;
#else
    std::string output;
    for (const wchar_t* p = value; *p != L'\0'; ++p) {
        const uint32_t cp = static_cast<uint32_t>(*p);
        if (cp < 0x80) {
            output.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            output.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return output;
#endif
}

std::wstring wide_from_utf8(const char* text) {
    if (text == nullptr)
        return {};
#if defined(_WIN32)
    const int required = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (required <= 0)
        return {};
    std::wstring output(static_cast<size_t>(required - 1), L'\0');
    if (required > 1)
        (void)::MultiByteToWideChar(CP_UTF8, 0, text, -1, output.data(), required);
    return output;
#else
    std::wstring output;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0;) {
        uint32_t cp = 0;
        size_t extra = 0;
        if (*p < 0x80) {
            cp = *p;
        } else if ((*p & 0xE0) == 0xC0 && p[1] != 0) {
            cp = ((uint32_t)(*p & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
            extra = 1;
        } else if ((*p & 0xF0) == 0xE0 && p[1] != 0 && p[2] != 0) {
            cp = ((uint32_t)(*p & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                 (uint32_t)(p[2] & 0x3F);
            extra = 2;
        } else {
            cp = *p;
        }
        output.push_back(static_cast<wchar_t>(cp));
        p += extra + 1;
    }
    return output;
#endif
}

void return_string(asIScriptGeneric* generic, const std::string& value) noexcept {
    try {
        new (generic->GetAddressOfReturnLocation()) as_string(value);
    } catch (...) {
        set_active_exception("ctx string return failed");
    }
}

// ──────────────────────────────────────────────────────────────────────────
// value marshalling: AS object → ordered_json
// ──────────────────────────────────────────────────────────────────────────

const asITypeInfo* type_info_for(asIScriptEngine* engine, int type_id) {
    const int base =
        type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
    return base != 0 ? engine->GetTypeInfoById(base) : nullptr;
}

bool type_is_named(asIScriptEngine* engine, int type_id, const char* name) {
    const asITypeInfo* type = type_info_for(engine, type_id);
    return type != nullptr && std::strcmp(type->GetName(), name) == 0;
}

bool type_is_funcdef(asIScriptEngine* engine, int type_id) {
    const asITypeInfo* type = type_info_for(engine, type_id);
    return type != nullptr && type->GetFuncdefSignature() != nullptr;
}

bool type_is_array(asIScriptEngine* engine, int type_id) {
    const asITypeInfo* type = type_info_for(engine, type_id);
    return type != nullptr && (type->GetFlags() & asOBJ_TEMPLATE) != 0 &&
           std::strcmp(type->GetName(), "array") == 0;
}

ordered_json dictionary_to_json(asIScriptEngine* engine, const CScriptDictionary* dict) noexcept;
ordered_json array_to_json(asIScriptEngine* engine, const CScriptArray* array) noexcept;
ordered_json object_to_json(asIScriptEngine* engine, void* object, int type_id) noexcept;

ordered_json primitive_to_json(int type_id, const void* address) {
    switch (type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST)) {
    case asTYPEID_BOOL:
        return ordered_json(*static_cast<const asBYTE*>(address) != 0);
    case asTYPEID_INT8:
        return ordered_json(static_cast<int64_t>(*static_cast<const int8_t*>(address)));
    case asTYPEID_INT16:
        return ordered_json(static_cast<int64_t>(*static_cast<const int16_t*>(address)));
    case asTYPEID_INT32:
        return ordered_json(static_cast<int64_t>(*static_cast<const int32_t*>(address)));
    case asTYPEID_INT64:
        return ordered_json(*static_cast<const int64_t*>(address));
    case asTYPEID_UINT8:
        return ordered_json(static_cast<int64_t>(*static_cast<const uint8_t*>(address)));
    case asTYPEID_UINT16:
        return ordered_json(static_cast<int64_t>(*static_cast<const uint16_t*>(address)));
    case asTYPEID_UINT32:
        return ordered_json(static_cast<int64_t>(*static_cast<const uint32_t*>(address)));
    case asTYPEID_UINT64: {
        const uint64_t raw = *static_cast<const uint64_t*>(address);
        if (raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return ordered_json(static_cast<double>(raw));
        return ordered_json(static_cast<int64_t>(raw));
    }
    case asTYPEID_FLOAT: {
        const float raw = *static_cast<const float*>(address);
        return std::isfinite(raw) ? ordered_json(raw) : ordered_json(nullptr);
    }
    case asTYPEID_DOUBLE: {
        const double raw = *static_cast<const double*>(address);
        return std::isfinite(raw) ? ordered_json(raw) : ordered_json(nullptr);
    }
    default:
        return ordered_json(nullptr);
    }
}

ordered_json dictionary_to_json(asIScriptEngine* engine, const CScriptDictionary* dict) noexcept {
    ordered_json output = ordered_json::object();
    try {
        for (auto it = dict->begin(); it != dict->end(); ++it) {
            const int type_id = it.GetTypeId();
            const void* address = it.GetAddressOfValue();
            ordered_json value = nullptr;
            if (type_id & asTYPEID_OBJHANDLE) {
                void* object = *static_cast<void* const*>(address);
                value = object != nullptr ? object_to_json(engine, object, type_id)
                                          : ordered_json(nullptr);
            } else if (type_id & asTYPEID_MASK_OBJECT) {
                value = object_to_json(engine, const_cast<void*>(address), type_id);
            } else {
                value = primitive_to_json(type_id, address);
            }
            output[it.GetKey()] = std::move(value);
        }
    } catch (...) {
        return ordered_json::object();
    }
    return output;
}

ordered_json array_to_json(asIScriptEngine* engine, const CScriptArray* array) noexcept {
    ordered_json output = ordered_json::array();
    try {
        const int element_type = array->GetElementTypeId();
        const asUINT size = array->GetSize();
        for (asUINT index = 0; index < size; ++index) {
            const void* element = array->At(index);
            if (element == nullptr)
                continue;
            ordered_json value = nullptr;
            if (element_type & asTYPEID_OBJHANDLE) {
                void* object = *static_cast<void* const*>(element);
                value = object != nullptr ? object_to_json(engine, object, element_type)
                                          : ordered_json(nullptr);
            } else if (element_type & asTYPEID_MASK_OBJECT) {
                // non-handle object elements: At() already returns the object.
                value = object_to_json(engine, const_cast<void*>(element), element_type);
            } else {
                value = primitive_to_json(element_type, element);
            }
            output.push_back(std::move(value));
        }
    } catch (...) {
        return ordered_json::array();
    }
    return output;
}

ordered_json object_to_json(asIScriptEngine* engine, void* object, int type_id) noexcept {
    if (object == nullptr)
        return ordered_json(nullptr);
    try {
        const asITypeInfo* type = type_info_for(engine, type_id);
        if (type == nullptr || type->GetFuncdefSignature() != nullptr)
            return ordered_json(nullptr);
        const char* name = type->GetName();
        if (std::strcmp(name, "string") == 0)
            return ordered_json(*static_cast<const as_string*>(object));
        if (std::strcmp(name, "json") == 0) {
            const ordered_json* inner = json_ref_value(object);
            return inner != nullptr ? ordered_json(*inner) : ordered_json(nullptr);
        }
        if (std::strcmp(name, "dictionary") == 0)
            return dictionary_to_json(engine, static_cast<const CScriptDictionary*>(object));
        if ((type->GetFlags() & asOBJ_TEMPLATE) != 0 && std::strcmp(name, "array") == 0)
            return array_to_json(engine, static_cast<const CScriptArray*>(object));
        return ordered_json(nullptr);
    } catch (...) {
        return ordered_json(nullptr);
    }
}

ordered_json value_to_json(asIScriptEngine* engine, const void* address, int type_id) noexcept {
    if (address == nullptr || type_id == 0)
        return ordered_json(nullptr);
    if (type_id & asTYPEID_OBJHANDLE) {
        void* object = *static_cast<void* const*>(address);
        return object != nullptr ? object_to_json(engine, object, type_id)
                                 : ordered_json(nullptr);
    }
    if (type_id & asTYPEID_MASK_OBJECT)
        return object_to_json(engine, const_cast<void*>(address), type_id);
    return primitive_to_json(type_id, address);
}

ordered_json generic_arg_to_json(asIScriptEngine* engine, asIScriptGeneric* generic,
                                 asUINT index) noexcept {
    if (generic == nullptr || static_cast<int>(index) >= generic->GetArgCount())
        return ordered_json(nullptr);
    return value_to_json(engine, generic->GetAddressOfArg(index),
                         generic->GetArgTypeId(index));
}

asIScriptFunction* arg_funcdef(asIScriptEngine* engine, asIScriptGeneric* generic,
                               asUINT index) noexcept {
    if (generic == nullptr || static_cast<int>(index) >= generic->GetArgCount())
        return nullptr;
    const int type_id = generic->GetArgTypeId(index);
    if ((type_id & asTYPEID_OBJHANDLE) == 0 || !type_is_funcdef(engine, type_id))
        return nullptr;
    void* address = generic->GetAddressOfArg(index);
    return address == nullptr ? nullptr
                              : static_cast<asIScriptFunction*>(*static_cast<void**>(address));
}

std::string arg_string(asIScriptGeneric* generic, asUINT index) {
    const auto* value = static_cast<const as_string*>(generic->GetArgAddress(index));
    return value == nullptr ? std::string{} : *value;
}

// ──────────────────────────────────────────────────────────────────────────
// value marshalling: ordered_json → AS objects
// ──────────────────────────────────────────────────────────────────────────

CScriptDictionary* dictionary_from_json(asIScriptEngine* engine,
                                        const ordered_json& value) noexcept;
CScriptArray* array_from_json(asIScriptEngine* engine, const ordered_json& value) noexcept;

enum class array_kind { unknown, dictionary, integer, number, string, boolean, mixed };

array_kind classify_array(const ordered_json& array_value) noexcept {
    array_kind kind = array_kind::unknown;
    for (const auto& item : array_value) {
        array_kind item_kind = array_kind::mixed;
        if (item.is_object())
            item_kind = array_kind::dictionary;
        else if (item.is_boolean())
            item_kind = array_kind::boolean;
        else if (item.is_number_integer() || item.is_number_unsigned())
            item_kind = array_kind::integer;
        else if (item.is_number_float())
            item_kind = array_kind::number;
        else if (item.is_string())
            item_kind = array_kind::string;
        else
            item_kind = array_kind::mixed;
        if (kind == array_kind::unknown) {
            kind = item_kind;
        } else if (kind != item_kind) {
            if ((kind == array_kind::integer || kind == array_kind::number) &&
                (item_kind == array_kind::integer || item_kind == array_kind::number)) {
                kind = array_kind::number;
            } else {
                return array_kind::mixed;
            }
        }
    }
    return kind;
}

CScriptArray* typed_array(asIScriptEngine* engine, const char* declaration,
                          const ordered_json& value) noexcept {
    asITypeInfo* type = engine->GetTypeInfoByDecl(declaration);
    if (type == nullptr)
        return nullptr;
    return CScriptArray::Create(type, static_cast<asUINT>(value.size()));
}

void dictionary_set_json(asIScriptEngine* engine, CScriptDictionary* dict,
                         const std::string& key, const ordered_json& value) noexcept {
    if (dict == nullptr)
        return;
    try {
        if (value.is_object()) {
            CScriptDictionary* child = dictionary_from_json(engine, value);
            if (child == nullptr)
                return;
            void* handle = child;
            dict->Set(key, &handle, engine->GetTypeInfoByDecl("dictionary")->GetTypeId() |
                                        asTYPEID_OBJHANDLE);
            engine->ReleaseScriptObject(child, engine->GetTypeInfoByDecl("dictionary"));
        } else if (value.is_array()) {
            CScriptArray* child = array_from_json(engine, value);
            if (child == nullptr) {
                // fall back to per-index object keys is not canonical: store null.
                void* handle = nullptr;
                dict->Set(key, &handle,
                          engine->GetTypeInfoByDecl("dictionary")->GetTypeId() |
                              asTYPEID_OBJHANDLE);
                return;
            }
            asITypeInfo* array_type = child->GetArrayObjectType();
            void* handle = child;
            dict->Set(key, &handle, array_type->GetTypeId() | asTYPEID_OBJHANDLE);
            engine->ReleaseScriptObject(child, array_type);
        } else if (value.is_string()) {
            as_string text = value.get<as_string>();
            asITypeInfo* type = engine->GetTypeInfoByDecl("string");
            if (type != nullptr)
                dict->Set(key, &text, type->GetTypeId());
        } else if (value.is_boolean() || value.is_number_integer() ||
                   value.is_number_unsigned()) {
            const asINT64 number = value.is_boolean() ? (value.get<bool>() ? 1 : 0)
                                                      : value.get<asINT64>();
            dict->Set(key, number);
        } else if (value.is_number_float()) {
            dict->Set(key, value.get<double>());
        } else {
            void* handle = nullptr;
            dict->Set(key, &handle,
                      engine->GetTypeInfoByDecl("dictionary")->GetTypeId() |
                          asTYPEID_OBJHANDLE);
        }
    } catch (...) {
    }
}

CScriptDictionary* dictionary_from_json(asIScriptEngine* engine,
                                        const ordered_json& value) noexcept {
    if (engine == nullptr || !value.is_object())
        return nullptr;
    try {
        CScriptDictionary* dict = CScriptDictionary::Create(engine);
        if (dict == nullptr)
            return nullptr;
        for (auto it = value.begin(); it != value.end(); ++it) {
            dictionary_set_json(engine, dict, it.key(), it.value());
        }
        return dict;
    } catch (...) {
        return nullptr;
    }
}

CScriptArray* array_from_json(asIScriptEngine* engine, const ordered_json& value) noexcept {
    if (engine == nullptr || !value.is_array())
        return nullptr;
    try {
        const array_kind kind = classify_array(value);
        const char* declaration = nullptr;
        switch (kind) {
        case array_kind::dictionary:
            declaration = "array<dictionary>";
            break;
        case array_kind::integer:
        case array_kind::boolean:
            declaration = "array<int64>";
            break;
        case array_kind::number:
            declaration = "array<double>";
            break;
        case array_kind::string:
            declaration = "array<string>";
            break;
        default:
            // empty arrays default to dictionary payloads.
            declaration = value.empty() ? "array<dictionary>" : nullptr;
            break;
        }
        if (declaration == nullptr)
            return nullptr;
        CScriptArray* array = typed_array(engine, declaration, value);
        if (array == nullptr)
            return nullptr;
        const int element_type = array->GetElementTypeId();
        asITypeInfo* dict_type = engine->GetTypeInfoByDecl("dictionary");
        asITypeInfo* string_type = engine->GetTypeInfoByDecl("string");
        asUINT index = 0;
        for (const auto& item : value) {
            void* element = array->At(index++);
            if (element == nullptr)
                continue;
            if (element_type & asTYPEID_OBJHANDLE) {
                void** slot = static_cast<void**>(element);
                if (kind == array_kind::dictionary && item.is_object() && dict_type != nullptr) {
                    CScriptDictionary* child = dictionary_from_json(engine, item);
                    *slot = child;  // array holds the handle; no extra ref needed.
                } else {
                    *slot = nullptr;
                }
            } else if (element_type & asTYPEID_MASK_OBJECT) {
                if (string_type != nullptr && item.is_string()) {
                    as_string text = item.get<as_string>();
                    engine->AssignScriptObject(element, &text, string_type);
                }
            } else {
                switch (element_type) {
                case asTYPEID_BOOL:
                    *static_cast<asBYTE*>(element) = item.is_boolean() && item.get<bool>();
                    break;
                case asTYPEID_INT64:
                    *static_cast<int64_t*>(element) =
                        item.is_number() ? item.get<int64_t>() : (item.is_boolean() ? 1 : 0);
                    break;
                case asTYPEID_DOUBLE:
                    *static_cast<double*>(element) = item.is_number() ? item.get<double>() : 0.0;
                    break;
                default:
                    break;
                }
            }
        }
        return array;
    } catch (...) {
        return nullptr;
    }
}

// ──────────────────────────────────────────────────────────────────────────
// callback records (mirrors angel_callback lifetime rules)
// ──────────────────────────────────────────────────────────────────────────

struct as_ctx_callback {
    asIScriptEngine* engine = nullptr;
    asIScriptFunction* function = nullptr;
    loader_ns::plugin_context_t* ctx = nullptr;
    shared_plugin_state plugin;
    std::mutex mutex;
    std::condition_variable idle;
    std::uint32_t active_calls = 0;
    bool releasing = false;
    bool invoked = false;
    bool released = false;
    bool one_shot = false;
    std::string timer_token;
};

struct cb_result {
    ordered_json value = nullptr;
    int return_type_id = 0;
    void* return_object = nullptr;  // caller releases via release_return_object
    int32_t status = SAO_OK;
};

void release_return_object(asIScriptEngine* engine, cb_result& result) noexcept {
    if (result.return_object == nullptr || engine == nullptr)
        return;
    engine_execution_guard engine_lock;
    const int base =
        result.return_type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
    if (asITypeInfo* type = engine->GetTypeInfoById(base))
        engine->ReleaseScriptObject(result.return_object, type);
    result.return_object = nullptr;
}

int32_t set_callback_arguments(asIScriptContext* context, asIScriptFunction* function,
                               asIScriptEngine* engine, const ordered_json& args,
                               std::vector<as_string>& string_args,
                               std::vector<std::pair<void*, const asITypeInfo*>>& release_after) {
    const asUINT param_count = function->GetParamCount();
    for (asUINT index = 0; index < param_count; ++index) {
        int type_id = 0;
        if (function->GetParam(index, &type_id) < 0)
            return SAO_ERR_OS_CALL_FAILED;
        const ordered_json& value =
            args.is_array() && index < args.size() ? args[index] : ordered_json(nullptr);
        int result = asINVALID_ARG;
        const int base_tid = type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
        switch (base_tid) {
        case asTYPEID_BOOL:
            result = context->SetArgByte(index, value.is_boolean() && value.get<bool>());
            break;
        case asTYPEID_INT8:
        case asTYPEID_INT16:
        case asTYPEID_INT32:
        case asTYPEID_UINT8:
        case asTYPEID_UINT16:
        case asTYPEID_UINT32:
            result = context->SetArgDWord(index,
                                          value.is_number() ? value.get<int64_t>() & 0xFFFFFFFF
                                                            : 0);
            break;
        case asTYPEID_INT64:
        case asTYPEID_UINT64:
            result = context->SetArgQWord(index, value.is_number()
                                                     ? static_cast<asQWORD>(value.get<int64_t>())
                                                     : 0);
            break;
        case asTYPEID_FLOAT:
            result = context->SetArgFloat(index, value.is_number() ? value.get<float>() : 0.0F);
            break;
        case asTYPEID_DOUBLE:
            result =
                context->SetArgDouble(index, value.is_number() ? value.get<double>() : 0.0);
            break;
        default:
            break;
        }
        if (result != asINVALID_ARG) {
            if (result < 0)
                return SAO_ERR_OS_CALL_FAILED;
            continue;
        }
        if (type_id & asTYPEID_OBJHANDLE) {
            const asITypeInfo* type = type_info_for(engine, type_id);
            const char* name = type != nullptr ? type->GetName() : "";
            void* argument = nullptr;
            if (type != nullptr && type->GetFuncdefSignature() != nullptr) {
                result = context->SetArgObject(index, nullptr);
            } else if (std::strcmp(name, "PluginContext") == 0) {
                // bound context object wired via SetUserData path — pass null handle.
                result = context->SetArgObject(index, nullptr);
            } else if (std::strcmp(name, "json") == 0) {
                argument = json_ref_from_ordered(engine, value);
                result = context->SetArgObject(index, argument);
                if (argument != nullptr)
                    release_after.emplace_back(argument, type);
            } else if (std::strcmp(name, "dictionary") == 0) {
                argument = value.is_object() ? dictionary_from_json(engine, value) : nullptr;
                result = context->SetArgObject(index, argument);
                if (argument != nullptr)
                    release_after.emplace_back(argument, type);
            } else if (type_is_array(engine, type_id)) {
                argument = value.is_array() ? array_from_json(engine, value) : nullptr;
                result = context->SetArgObject(index, argument);
                if (argument != nullptr)
                    release_after.emplace_back(argument, type);
            } else {
                result = context->SetArgObject(index, nullptr);
            }
        } else if (type_id & asTYPEID_MASK_OBJECT) {
            if (type_is_named(engine, type_id, "string")) {
                string_args.push_back(value.is_string() ? value.get<as_string>() : as_string{});
                result = context->SetArgObject(index, &string_args.back());
            } else {
                result = context->SetArgObject(index, nullptr);
            }
        } else {
            result = context->SetArgObject(index, nullptr);
        }
        if (result < 0)
            return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
}

ordered_json return_value_to_json(asIScriptContext* context, asIScriptFunction* function,
                                  asIScriptEngine* engine, cb_result* out) {
    const int type_id = function->GetReturnTypeId();
    if (out != nullptr) {
        out->return_type_id = type_id;
    }
    switch (type_id) {
    case asTYPEID_VOID:
        return ordered_json(nullptr);
    case asTYPEID_BOOL:
        return ordered_json(context->GetReturnByte() != 0);
    case asTYPEID_INT8:
        return ordered_json(static_cast<int64_t>(static_cast<int8_t>(context->GetReturnByte())));
    case asTYPEID_INT16:
        return ordered_json(
            static_cast<int64_t>(static_cast<int16_t>(context->GetReturnWord())));
    case asTYPEID_INT32:
        return ordered_json(static_cast<int64_t>(context->GetReturnDWord()));
    case asTYPEID_UINT8:
        return ordered_json(static_cast<int64_t>(context->GetReturnByte()));
    case asTYPEID_UINT16:
        return ordered_json(static_cast<int64_t>(context->GetReturnWord()));
    case asTYPEID_UINT32:
        return ordered_json(static_cast<int64_t>(context->GetReturnDWord()));
    case asTYPEID_INT64:
        return ordered_json(context->GetReturnQWord());
    case asTYPEID_UINT64: {
        const uint64_t raw = context->GetReturnQWord();
        if (raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return ordered_json(static_cast<double>(raw));
        return ordered_json(static_cast<int64_t>(raw));
    }
    case asTYPEID_FLOAT: {
        const float raw = context->GetReturnFloat();
        return std::isfinite(raw) ? ordered_json(raw) : ordered_json(nullptr);
    }
    case asTYPEID_DOUBLE: {
        const double raw = context->GetReturnDouble();
        return std::isfinite(raw) ? ordered_json(raw) : ordered_json(nullptr);
    }
    default:
        break;
    }
    if (type_is_named(engine, type_id, "string")) {
        const auto* text = static_cast<const as_string*>(context->GetReturnObject());
        return text == nullptr ? ordered_json(nullptr) : ordered_json(*text);
    }
    void* object = context->GetReturnObject();
    if (object == nullptr)
        return ordered_json(nullptr);
    // expose the handle object to callers that need raw access (menu rows).
    if (out != nullptr && (type_id & (asTYPEID_OBJHANDLE | asTYPEID_MASK_OBJECT)) != 0) {
        const int base = type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
        if (asITypeInfo* type = engine->GetTypeInfoById(base)) {
            engine->AddRefScriptObject(object, type);
            out->return_object = object;
        }
    }
    return object_to_json(engine, object, type_id);
}

int32_t invoke_ctx_callback(as_ctx_callback* callback, const ordered_json& args,
                            cb_result* out_result) noexcept {
    if (callback == nullptr || callback->engine == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    {
        std::lock_guard lock(callback->mutex);
        if (callback->releasing || callback->function == nullptr)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        ++callback->active_calls;
    }
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        engine_execution_guard engine_lock;
        if (asIScriptContext* raw = callback->engine->CreateContext()) {
            const std::unique_ptr<asIScriptContext, void (*)(asIScriptContext*)> context(
                raw, [](asIScriptContext* value) { value->Release(); });
            if (context->Prepare(callback->function) < 0) {
                status = SAO_ERR_OS_CALL_FAILED;
            } else {
                context->SetUserData(callback->plugin.get(),
                                     kPluginContextUserDataSlot);
                std::vector<as_string> string_args;
                std::vector<std::pair<void*, const asITypeInfo*>> release_after;
                status = set_callback_arguments(context.get(), callback->function,
                                                callback->engine, args, string_args,
                                                release_after);
                if (status == SAO_OK) {
                    if (context->Execute() != asEXECUTION_FINISHED) {
                        status = SAO_ERR_OS_CALL_FAILED;
                    } else if (out_result != nullptr) {
                        out_result->value =
                            return_value_to_json(context.get(), callback->function,
                                                 callback->engine, out_result);
                    }
                }
                for (const auto& [object, type] : release_after) {
                    if (type != nullptr && object != nullptr)
                        callback->engine->ReleaseScriptObject(object, type);
                }
                release_after.clear();
            }
        }
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    {
        std::lock_guard lock(callback->mutex);
        callback->invoked = true;
        if (--callback->active_calls == 0)
            callback->idle.notify_all();
    }
    if (callback->one_shot && callback->ctx != nullptr && !callback->timer_token.empty() &&
        status == SAO_OK) {
        (void)loader_ns::sao_plugins_ctx_complete_timer(callback->ctx,
                                                      callback->timer_token.c_str());
    }
    return status;
}

void quiesce_ctx_callback(as_ctx_callback* callback) noexcept {
    if (callback == nullptr)
        return;
    {
        std::lock_guard lock(callback->mutex);
        callback->releasing = true;
    }
    std::unique_lock lock(callback->mutex);
    callback->idle.wait(lock, [callback] { return callback->active_calls == 0; });
    // function release happens in release_callback_functions under the
    // engine lock while the module is still alive.
}

// ──────────────────────────────────────────────────────────────────────────
// per-plugin ctx surface state
// ──────────────────────────────────────────────────────────────────────────

struct menu_row {
    std::string label;
    std::string icon;
    std::string action_id;
    std::string payload;
    bool can_activate = false;
    bool keep_menu_open = false;
    bool close_menu_before = false;
};

struct action_record {
    as_ctx_callback* callback = nullptr;
    std::string payload_json;
};

struct menu_provider {
    std::string provider_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    std::mutex mutex;
    bool closing = false;
    as_ctx_callback* builder = nullptr;
    as_ctx_callback* panel_handler = nullptr;  // ACTION_ONLY: (action_id, payload) -> dict
    std::unordered_set<std::string> claimed_ids;  // empty => handles every action
    std::vector<menu_row> rows;
    std::unordered_map<std::string, action_record> actions;
    std::unordered_map<std::string, std::size_t> identity_occurrences;
    std::uint64_t revision = 0;
    shared_plugin_state plugin;
    loader_ns::plugin_context_t* ctx = nullptr;
};

struct compositor_input_record {
    as_ctx_callback* cursor_pos = nullptr;
    as_ctx_callback* mouse_button = nullptr;
    as_ctx_callback* cursor_leave = nullptr;
    as_ctx_callback* scroll = nullptr;
};

struct data_source_record {
    as_ctx_callback* start = nullptr;
    as_ctx_callback* stop = nullptr;
};

// one user_data shared across a panel's render + action callbacks.
struct panel_record {
    as_ctx_callback* render = nullptr;
    as_ctx_callback* action = nullptr;
};

struct ctx_surface_state {
    loader_ns::plugin_context_t* context = nullptr;
    shared_plugin_state plugin;
    CScriptDictionary* owner = nullptr;
    std::vector<std::unique_ptr<panel_record>> panel_records;
    std::vector<std::unique_ptr<data_source_record>> data_source_records;
    std::vector<std::unique_ptr<as_ctx_callback>> callbacks;
    std::unordered_map<std::uint32_t, as_ctx_callback*> subscription_tokens;
    std::unordered_map<std::string, as_ctx_callback*> hotkey_records;
    std::unordered_map<std::string, as_ctx_callback*> timer_records;
    std::unordered_map<std::uint32_t, as_ctx_callback*> render_hooks;
    std::vector<std::shared_ptr<menu_provider>> menus;
    std::vector<std::shared_ptr<menu_provider>> action_providers;
    as_ctx_callback* global_action_handler = nullptr;
    struct engine_record {
        void* object = nullptr;
        int type_id = 0;
    };
    std::unordered_map<std::string, engine_record> engines;
    std::vector<std::unique_ptr<compositor_input_record>> input_records;
    // Reflective-engine callback channels (ctx.engine_on / engine_off).  The
    // map is keyed by channel name ("net.frame", "ui.render_hook", ...) and is
    // guarded by its own mutex because the sdk_context_call_request
    // engine_callback trampoline fires on provider threads while registration
    // mutates it under the engine lock.
    std::mutex engine_channel_mutex;
    std::unordered_map<std::string, as_ctx_callback*> engine_channels;
};

std::mutex& ctx_state_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<loader_ns::plugin_context_t*, std::shared_ptr<ctx_surface_state>>&
ctx_states() {
    static std::unordered_map<loader_ns::plugin_context_t*,
                              std::shared_ptr<ctx_surface_state>>
        m;
    return m;
}

// user_data-visible records are never freed after teardown — a loader-side
// callback that slips past quiesce touches an inert record instead of freed
// memory.
std::mutex& graveyard_mutex() {
    static std::mutex m;
    return m;
}

std::vector<std::unique_ptr<as_ctx_callback>>& callback_graveyard() {
    static std::vector<std::unique_ptr<as_ctx_callback>> v;
    return v;
}

std::vector<std::unique_ptr<compositor_input_record>>& input_graveyard() {
    static std::vector<std::unique_ptr<compositor_input_record>> v;
    return v;
}

std::vector<std::unique_ptr<data_source_record>>& data_source_graveyard() {
    static std::vector<std::unique_ptr<data_source_record>> v;
    return v;
}

std::shared_ptr<ctx_surface_state> ctx_state_for(loader_ns::plugin_context_t* ctx) {
    std::lock_guard lock(ctx_state_mutex());
    const auto found = ctx_states().find(ctx);
    return found == ctx_states().end() ? nullptr : found->second;
}

std::shared_ptr<ctx_surface_state> ctx_state_or_create(loader_ns::plugin_context_t* ctx,
                                                     asIScriptEngine* engine) {
    std::lock_guard lock(ctx_state_mutex());
    const auto found = ctx_states().find(ctx);
    if (found != ctx_states().end())
        return found->second;
    auto state = std::make_shared<ctx_surface_state>();
    state->context = ctx;
    state->plugin = acquire_plugin_state_by_bound_context(ctx);
    ctx_states().emplace(ctx, state);
    (void)engine;
    return state;
}

loader_ns::plugin_context_t* context_of(asIScriptGeneric* generic) {
    return static_cast<loader_ns::plugin_context_t*>(generic->GetObject());
}

as_ctx_callback* make_callback(const std::shared_ptr<ctx_surface_state>& state,
                             asIScriptFunction* function, asIScriptEngine* engine) {
    if (function == nullptr || !state)
        return nullptr;
    auto record = std::make_unique<as_ctx_callback>();
    record->engine = engine != nullptr
                         ? engine
                         : (state->plugin != nullptr ? state->plugin->engine : nullptr);
    record->function = function;
    function->AddRef();
    record->ctx = state->context;
    record->plugin = state->plugin;
    as_ctx_callback* raw = record.get();
    state->callbacks.push_back(std::move(record));
    return raw;
}

void retire_callback(const std::shared_ptr<ctx_surface_state>& state,
                     as_ctx_callback* callback) {
    if (!state || callback == nullptr)
        return;
    quiesce_ctx_callback(callback);
    if (callback->function != nullptr && callback->engine != nullptr) {
        engine_execution_guard engine_lock;
        callback->function->Release();
        callback->function = nullptr;
    }
    for (auto it = state->callbacks.begin(); it != state->callbacks.end(); ++it) {
        if (it->get() == callback) {
            std::lock_guard grave(graveyard_mutex());
            callback_graveyard().push_back(std::move(*it));
            state->callbacks.erase(it);
            return;
        }
    }
}

void release_callback_functions(const std::shared_ptr<ctx_surface_state>& state,
                                asIScriptEngine* engine) noexcept {
    if (!state || engine == nullptr)
        return;
    engine_execution_guard engine_lock;
    for (const auto& callback : state->callbacks) {
        if (callback->function != nullptr) {
            callback->function->Release();
            callback->function = nullptr;
        }
        callback->invoked = true;
    }
}

// ──────────────────────────────────────────────────────────────────────────
// native bridges → invoke_ctx_callback
// ──────────────────────────────────────────────────────────────────────────

void SAO_PLUGINS_CALL event_dispatch(const char* topic_utf8, const char* event_json_utf8,
                                     void* user_data) {
    auto* callback = static_cast<as_ctx_callback*>(user_data);
    if (callback == nullptr)
        return;
    ordered_json args = ordered_json::array();
    ordered_json event = nullptr;
    if (event_json_utf8 != nullptr) {
        ordered_json parsed = ordered_json::parse(event_json_utf8, nullptr, false);
        event = parsed.is_discarded() ? ordered_json(nullptr) : std::move(parsed);
    }
    args.push_back(std::move(event));
    (void)topic_utf8;
    (void)invoke_ctx_callback(callback, args, nullptr);
}

void SAO_PLUGINS_CALL hotkey_dispatch(void* user_data) {
    auto* callback = static_cast<as_ctx_callback*>(user_data);
    (void)invoke_ctx_callback(callback, ordered_json::array(), nullptr);
}

void SAO_PLUGINS_CALL timer_dispatch(void* user_data) {
    auto* callback = static_cast<as_ctx_callback*>(user_data);
    (void)invoke_ctx_callback(callback, ordered_json::array(), nullptr);
}

// ──────────────────────────────────────────────────────────────────────────
// reflective engine surface (sdk_binding method_engine_call / method_engine_list)
// ──────────────────────────────────────────────────────────────────────────
//
// `engine_on` is implemented via request->engine_callback instead of an
// `engine_poll` drain queue — the honest path: the as_ctx_callback machinery
// already quiesces funcdef refs at teardown, invoke_ctx_callback re-enters
// under engine_execution_guard from provider threads, and the trampoline
// resolves channel→function through ctx_state_for so a catalog slot that kept
// our trampoline past unload lands on a missing map entry and fails closed.

void SAO_PLUGINS_CALL engine_channel_trampoline(const char* channel_utf8,
                                                const uint8_t* payload_json_utf8,
                                                size_t payload_size, void* user_data) {
    auto* ctx = static_cast<loader_ns::plugin_context_t*>(user_data);
    if (ctx == nullptr || channel_utf8 == nullptr || channel_utf8[0] == '\0')
        return;
    const std::shared_ptr<ctx_surface_state> state = ctx_state_for(ctx);
    if (!state)
        return;
    as_ctx_callback* callback = nullptr;
    {
        std::lock_guard lock(state->engine_channel_mutex);
        const auto found = state->engine_channels.find(channel_utf8);
        if (found != state->engine_channels.end())
            callback = found->second;
    }
    if (callback == nullptr)
        return;
    ordered_json payload = nullptr;
    if (payload_json_utf8 != nullptr && payload_size != 0) {
        const char* begin = reinterpret_cast<const char*>(payload_json_utf8);
        ordered_json parsed = ordered_json::parse(begin, begin + payload_size, nullptr, false);
        payload = parsed.is_discarded() ? ordered_json(nullptr) : std::move(parsed);
    }
    // cb(dictionary@ payload, string channel): payload first so a one-arg
    // `void cb(dictionary@)` overload shape still works.
    ordered_json args = ordered_json::array();
    args.push_back(std::move(payload));
    args.push_back(channel_utf8);
    (void)invoke_ctx_callback(callback, args, nullptr);
}

// Shared dispatch: sends `body` (full {"name","args"} envelope for
// engine_call; empty for engine_list) through sao_plugins_sdk_context_dispatch
// and returns the exact {"status","result"} JSON the engine surface writes
// into out_result_json_utf8.  Buffer semantics follow engine_result — first
// pass on a stack buffer, retry once heap-sized on BUFFER_TOO_SMALL.
int32_t engine_dispatch_json(asIScriptGeneric* generic,
                             sdk_binding::sdk_method_id method,
                             const std::string& body, as_string* out_text) {
    auto* loader_ctx = context_of(generic);
    SaoSdkContext* sdk = gpu_hunt_binding_context(generic->GetEngine());
    sdk_binding::sdk_context_call_request request{};
    if (!body.empty()) {
        request.args_json_utf8 = body.c_str();
        request.args_size = body.size();
    }
    request.engine_callback = &engine_channel_trampoline;
    request.callback_user_data = loader_ctx;
    int32_t status = sdk != nullptr && loader_ctx != nullptr
                         ? SAO_ERR_OS_CALL_FAILED
                         : SAO_ERR_NOT_IMPLEMENTED;
    char stack_buffer[16384];
    size_t required = 0;
    if (status != SAO_ERR_NOT_IMPLEMENTED) {
        request.out_result_json_utf8 = stack_buffer;
        request.out_capacity = sizeof(stack_buffer);
        request.out_required = &required;
        status = sdk_binding::sao_plugins_sdk_context_dispatch(sdk, method, &request);
        if (status == SAO_ERR_BUFFER_TOO_SMALL && required > sizeof(stack_buffer) &&
            required <= sdk_binding::kMaximumBindingJsonBytes + 1) {
            std::string heap(required, '\0');
            request.out_result_json_utf8 = heap.data();
            request.out_capacity = heap.size();
            status = sdk_binding::sao_plugins_sdk_context_dispatch(sdk, method, &request);
            if (status == SAO_OK)
                out_text->assign(heap.data(), required > 0 ? required - 1 : 0);
        } else if (status == SAO_OK && required != 0) {
            out_text->assign(stack_buffer, required - 1);
        }
    }
    if (out_text->empty()) {
        // Dispatch wrote no result envelope (validation failure, missing
        // SaoSdkContext, ...) — still honor the {"status","result"} shape.
        try {
            ordered_json envelope = ordered_json::object();
            envelope["status"] = status;
            envelope["result"] = nullptr;
            out_text->assign(envelope.dump());
        } catch (...) {
            out_text->assign("{\"status\":-1,\"result\":null}");
        }
    }
    return status;
}

void ctx_engine_call(asIScriptGeneric* generic) {
    const std::string name = arg_string(generic, 0);
    const std::string args_text = arg_string(generic, 1);
    std::string body;
    bool args_ok = !name.empty();
    if (args_ok) {
        ordered_json args = ordered_json::object();
        if (!args_text.empty()) {
            ordered_json parsed = ordered_json::parse(args_text, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                args_ok = false;
            } else {
                args = std::move(parsed);
            }
        }
        if (args_ok) {
            try {
                ordered_json envelope = ordered_json::object();
                envelope["name"] = name;
                envelope["args"] = std::move(args);
                body = envelope.dump();
            } catch (...) {
                args_ok = false;
            }
        }
    }
    // bad args → empty body → dispatch returns INVALID_ARGUMENT envelope.
    as_string result;
    (void)engine_dispatch_json(generic, sdk_binding::sdk_method_id::method_engine_call,
                               args_ok ? body : std::string{}, &result);
    return_string(generic, result);
}

void ctx_engine_list(asIScriptGeneric* generic) {
    as_string result;
    (void)engine_dispatch_json(generic, sdk_binding::sdk_method_id::method_engine_list,
                               std::string{}, &result);
    return_string(generic, result);
}

void ctx_engine_on(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string channel = arg_string(generic, 0);
    asIScriptFunction* fn = arg_funcdef(generic->GetEngine(), generic, 1);
    bool ok = false;
    if (ctx != nullptr && !channel.empty() && fn != nullptr) {
        if (const auto state = ctx_state_or_create(ctx, generic->GetEngine())) {
            if (as_ctx_callback* record = make_callback(state, fn, generic->GetEngine())) {
                as_ctx_callback* previous = nullptr;
                {
                    std::lock_guard lock(state->engine_channel_mutex);
                    const auto found = state->engine_channels.find(channel);
                    if (found != state->engine_channels.end()) {
                        previous = found->second;
                        found->second = record;
                    } else {
                        state->engine_channels.emplace(channel, record);
                    }
                }
                if (previous != nullptr)
                    retire_callback(state, previous);
                ok = true;
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_engine_off(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string channel = arg_string(generic, 0);
    bool ok = false;
    if (ctx != nullptr && !channel.empty()) {
        if (const auto state = ctx_state_for(ctx)) {
            as_ctx_callback* previous = nullptr;
            {
                std::lock_guard lock(state->engine_channel_mutex);
                const auto found = state->engine_channels.find(channel);
                if (found != state->engine_channels.end()) {
                    previous = found->second;
                    state->engine_channels.erase(found);
                }
            }
            if (previous != nullptr) {
                retire_callback(state, previous);
                ok = true;
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

std::string parse_result_json(const cb_result& result) {
    if (result.value.is_null())
        return {};
    return result.value.is_string() ? result.value.get<std::string>() : result.value.dump();
}

int32_t SAO_PLUGINS_CALL render_hook_dispatch(const char* surface_utf8,
                                              const char* payload_json_utf8,
                                              char** out_spec_json_utf8, void* user_data) {
    if (out_spec_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_spec_json_utf8 = nullptr;
    auto* callback = static_cast<as_ctx_callback*>(user_data);
    if (callback == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    ordered_json args = ordered_json::array();
    args.push_back(surface_utf8 == nullptr ? "" : surface_utf8);
    ordered_json payload = nullptr;
    if (payload_json_utf8 != nullptr) {
        ordered_json parsed = ordered_json::parse(payload_json_utf8, nullptr, false);
        payload = parsed.is_discarded() || parsed.is_null() ? ordered_json::object() : std::move(parsed);
    }
    args.push_back(std::move(payload));
    cb_result result;
    const int32_t status = invoke_ctx_callback(callback, args, &result);
    if (status != SAO_OK)
        return status;
    const std::string text = parse_result_json(result);
    if (text.empty())
        return SAO_OK;
    auto* buffer = static_cast<char*>(std::malloc(text.size() + 1));
    if (buffer == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    std::memcpy(buffer, text.data(), text.size() + 1);
    *out_spec_json_utf8 = buffer;
    return SAO_OK;
}



int32_t invoke_data_source(as_ctx_callback* callback) {
    cb_result result;
    const int32_t status =
        callback != nullptr ? invoke_ctx_callback(callback, ordered_json::array(), &result)
                            : SAO_ERR_HANDLE_INVALID;
    if (status != SAO_OK)
        return status;
    return result.value.is_number() ? static_cast<int32_t>(result.value.get<int64_t>())
                                    : SAO_OK;
}

int32_t SAO_PLUGINS_CALL data_source_start_dispatch(void* user_data) {
    auto* record = static_cast<data_source_record*>(user_data);
    return record != nullptr ? invoke_data_source(record->start) : SAO_ERR_HANDLE_INVALID;
}

int32_t SAO_PLUGINS_CALL data_source_stop_dispatch(void* user_data) {
    auto* record = static_cast<data_source_record*>(user_data);
    return record != nullptr ? invoke_data_source(record->stop) : SAO_ERR_HANDLE_INVALID;
}

// canonical panel render gets only the payload dict (not the surface id).
int32_t SAO_PLUGINS_CALL panel_render_dispatch(const char* payload_json_utf8,
                                               char** out_spec_json_utf8, void* user_data) {
    if (out_spec_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_spec_json_utf8 = nullptr;
    auto* record = static_cast<panel_record*>(user_data);
    as_ctx_callback* callback = record != nullptr ? record->render : nullptr;
    if (callback == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    ordered_json args = ordered_json::array();
    ordered_json payload = nullptr;
    if (payload_json_utf8 != nullptr) {
        ordered_json parsed = ordered_json::parse(payload_json_utf8, nullptr, false);
        payload = parsed.is_discarded() || parsed.is_null() ? ordered_json::object() : std::move(parsed);
    }
    args.push_back(std::move(payload));
    cb_result result;
    const int32_t status = invoke_ctx_callback(callback, args, &result);
    if (status != SAO_OK)
        return status;
    const std::string text = parse_result_json(result);
    if (text.empty())
        return SAO_OK;
    auto* buffer = static_cast<char*>(std::malloc(text.size() + 1));
    if (buffer == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    std::memcpy(buffer, text.data(), text.size() + 1);
    *out_spec_json_utf8 = buffer;
    return SAO_OK;
}

std::vector<std::unique_ptr<panel_record>>& panel_graveyard() {
    static std::vector<std::unique_ptr<panel_record>> records;
    return records;
}

int32_t SAO_PLUGINS_CALL panel_action_dispatch(const char* action_id_utf8,
                                               const char* payload_json_utf8,
                                               char** out_result_json_utf8,
                                               void* user_data) {
    if (out_result_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_result_json_utf8 = nullptr;
    auto* record = static_cast<panel_record*>(user_data);
    if (record == nullptr || record->action == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    ordered_json args = ordered_json::array();
    args.push_back(action_id_utf8 == nullptr ? "" : action_id_utf8);
    ordered_json payload = nullptr;
    if (payload_json_utf8 != nullptr) {
        ordered_json parsed = ordered_json::parse(payload_json_utf8, nullptr, false);
        payload = parsed.is_discarded() || parsed.is_null() ? ordered_json::object()
                                                            : std::move(parsed);
    }
    args.push_back(std::move(payload));
    cb_result result;
    const int32_t status = invoke_ctx_callback(record->action, args, &result);
    if (status != SAO_OK)
        return status;
    const std::string text = parse_result_json(result);
    if (text.empty())
        return SAO_OK;
    auto* buffer = static_cast<char*>(std::malloc(text.size() + 1));
    if (buffer == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    std::memcpy(buffer, text.data(), text.size() + 1);
    *out_result_json_utf8 = buffer;
    return SAO_OK;
}

// the ctx compositor-input spec shares one user_data across its four slots —
// each dispatcher picks its own callback field out of the record.
void SAO_PLUGINS_CALL compositor_pos_dispatch(float x, float y, void* user_data) {
    auto* record = static_cast<compositor_input_record*>(user_data);
    if (record == nullptr || record->cursor_pos == nullptr)
        return;
    ordered_json args = ordered_json::array();
    args.push_back(static_cast<double>(x));
    args.push_back(static_cast<double>(y));
    (void)invoke_ctx_callback(record->cursor_pos, args, nullptr);
}

void SAO_PLUGINS_CALL compositor_button_dispatch(std::uint32_t button, bool pressed,
                                                 void* user_data) {
    auto* record = static_cast<compositor_input_record*>(user_data);
    if (record == nullptr || record->mouse_button == nullptr)
        return;
    ordered_json args = ordered_json::array();
    args.push_back(static_cast<int64_t>(button));
    args.push_back(pressed);
    (void)invoke_ctx_callback(record->mouse_button, args, nullptr);
}

void SAO_PLUGINS_CALL compositor_leave_dispatch(void* user_data) {
    auto* record = static_cast<compositor_input_record*>(user_data);
    if (record == nullptr || record->cursor_leave == nullptr)
        return;
    (void)invoke_ctx_callback(record->cursor_leave, ordered_json::array(), nullptr);
}

void SAO_PLUGINS_CALL compositor_scroll_dispatch(float dx, float dy, void* user_data) {
    auto* record = static_cast<compositor_input_record*>(user_data);
    if (record == nullptr || record->scroll == nullptr)
        return;
    ordered_json args = ordered_json::array();
    args.push_back(static_cast<double>(dx));
    args.push_back(static_cast<double>(dy));
    (void)invoke_ctx_callback(record->scroll, args, nullptr);
}

// ──────────────────────────────────────────────────────────────────────────
// menu snapshot provider (v2 producer protocol, mirrors lua bridge)
// ──────────────────────────────────────────────────────────────────────────

std::string hash_menu_identity(const std::string& value) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : value) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(hash));
    return buffer;
}

std::string dict_get_string(const CScriptDictionary* dict, const char* key) {
    if (dict == nullptr || key == nullptr)
        return {};
    const int type_id = dict->GetTypeId(key);
    if (type_id <= 0)
        return {};
    void* stored = nullptr;
    if ((type_id & asTYPEID_OBJHANDLE) != 0) {
        if (!dict->Get(key, &stored, type_id) || stored == nullptr)
            return {};
        return {};
    }
    if (!dict->Get(key, &stored, type_id) || stored == nullptr)
        return {};
    return *static_cast<const std::string*>(stored);
}

asIScriptFunction* dict_get_funcdef(CScriptDictionary* dict, const char* key,
                                    asIScriptEngine* engine) {
    if (dict == nullptr || key == nullptr || engine == nullptr)
        return nullptr;
    const int type_id = dict->GetTypeId(key);
    if ((type_id & asTYPEID_OBJHANDLE) == 0 || !type_is_funcdef(engine, type_id))
        return nullptr;
    void* stored = nullptr;
    if (!dict->Get(key, &stored, type_id))
        return nullptr;
    return static_cast<asIScriptFunction*>(stored);
}

bool dict_get_bool(const CScriptDictionary* dict, const char* key, bool fallback,
                   bool* present = nullptr) {
    if (dict == nullptr || key == nullptr)
        return fallback;
    const int type_id = dict->GetTypeId(key);
    if (type_id <= 0) {
        if (present != nullptr)
            *present = false;
        return fallback;
    }
    if (present != nullptr)
        *present = true;
    asINT64 integer = 0;
    if (dict->Get(key, integer))
        return integer != 0;
    double number = 0.0;
    if (dict->Get(key, number))
        return number != 0.0;
    return fallback;
}

std::string dict_get_payload(const CScriptDictionary* dict, const char* key,
                             asIScriptEngine* engine) {
    if (dict == nullptr || key == nullptr)
        return {};
    const int type_id = dict->GetTypeId(key);
    if (type_id <= 0)
        return {};
    void* stored = nullptr;
    if ((type_id & asTYPEID_OBJHANDLE) != 0 || (type_id & asTYPEID_MASK_OBJECT) != 0) {
        if (!dict->Get(key, &stored, type_id) || stored == nullptr)
            return {};
        const ordered_json value = object_to_json(engine, stored, type_id);
        return value.is_string() ? value.get<std::string>()
                                 : (value.is_null() ? std::string{} : value.dump());
    }
    asINT64 integer = 0;
    if (dict->Get(key, integer))
        return std::to_string(integer);
    double number = 0.0;
    if (dict->Get(key, number))
        return std::to_string(number);
    return {};
}

bool build_menu_snapshot(menu_provider* provider, asIScriptEngine* engine,
                         const std::shared_ptr<ctx_surface_state>& state) noexcept {
    if (provider == nullptr || provider->builder == nullptr || engine == nullptr)
        return false;
    std::lock_guard lock(provider->mutex);
    if (provider->closing)
        return false;
    cb_result result;
    const int32_t status =
        invoke_ctx_callback(provider->builder, ordered_json::array(), &result);
    if (status != SAO_OK)
        return false;

    std::vector<menu_row> rows;
    std::unordered_map<std::string, action_record> actions;
    try {
        const int base_tid =
            result.return_type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
        CScriptArray* raw_rows = nullptr;
        if ((result.return_type_id & asTYPEID_OBJHANDLE) != 0 &&
            type_is_array(engine, base_tid) && result.return_object != nullptr) {
            raw_rows = static_cast<CScriptArray*>(result.return_object);
        }
        if (raw_rows != nullptr) {
            const int element_type = raw_rows->GetElementTypeId();
            for (asUINT index = 0; index < raw_rows->GetSize(); ++index) {
                CScriptDictionary* row_dict = nullptr;
                if (element_type & asTYPEID_OBJHANDLE) {
                    row_dict =
                        static_cast<CScriptDictionary*>(*static_cast<void**>(raw_rows->At(index)));
                } else if (element_type & asTYPEID_MASK_OBJECT) {
                    row_dict = static_cast<CScriptDictionary*>(raw_rows->At(index));
                }
                if (row_dict == nullptr)
                    continue;
                menu_row row;
                row.label = dict_get_string(row_dict, "label");
                if (row.label.empty())
                    continue;
                row.icon = dict_get_string(row_dict, "icon");
                row.payload = dict_get_payload(row_dict, "payload", engine);
                row.keep_menu_open = dict_get_bool(row_dict, "keep_menu_open", false);
                row.close_menu_before = dict_get_bool(row_dict, "close_menu_before", false);
                bool requested = true;
                const bool explicit_flag =
                    dict_get_bool(row_dict, "can_activate", true, &requested);
                asIScriptFunction* command =
                    dict_get_funcdef(row_dict, "command", engine);
                row.can_activate = command != nullptr && (!explicit_flag || requested);

                std::string identity = dict_get_string(row_dict, "action_id");
                if (identity.empty())
                    identity = dict_get_string(row_dict, "id");
                if (identity.empty()) {
                    identity.append(row.label);
                    identity.push_back('\n');
                    identity.append(row.icon);
                    identity.push_back('\n');
                    identity.append(row.payload);
                    identity.push_back(row.can_activate ? '1' : '0');
                    identity.push_back(row.keep_menu_open ? '1' : '0');
                    identity.push_back(row.close_menu_before ? '1' : '0');
                    const std::size_t occurrence =
                        provider->identity_occurrences[identity]++;
                    identity.push_back('#');
                    identity.append(std::to_string(occurrence));
                }
                row.action_id = "menu-action-" +
                                hash_menu_identity(provider->contribution_id + "\n" + identity);
                if (command != nullptr) {
                    as_ctx_callback* cb = make_callback(state, command, engine);
                    if (cb != nullptr)
                        actions.emplace(row.action_id, action_record{cb, row.payload});
                }
                rows.push_back(std::move(row));
            }
        }
    } catch (...) {
        release_return_object(engine, result);
        return false;
    }

    const bool changed = provider->revision == 0 || provider->rows.size() != rows.size() ||
                         [&] {
                             for (std::size_t i = 0; i < rows.size(); ++i) {
                                 if (!(rows[i].label == provider->rows[i].label &&
                                       rows[i].icon == provider->rows[i].icon &&
                                       rows[i].action_id == provider->rows[i].action_id &&
                                       rows[i].payload == provider->rows[i].payload &&
                                       rows[i].can_activate ==
                                           provider->rows[i].can_activate &&
                                       rows[i].keep_menu_open ==
                                           provider->rows[i].keep_menu_open &&
                                       rows[i].close_menu_before ==
                                           provider->rows[i].close_menu_before))
                                     return true;
                             }
                             return false;
                         }();
    // retire callbacks of stale actions, install the fresh action table.
    std::vector<as_ctx_callback*> stale;
    for (const auto& [action_id, record] : provider->actions) {
        if (actions.find(action_id) == actions.end())
            stale.push_back(record.callback);
    }
    provider->actions = std::move(actions);
    provider->rows = std::move(rows);
    if (changed)
        ++provider->revision;
    release_return_object(engine, result);
    for (as_ctx_callback* stale_cb : stale)
        retire_callback(state, stale_cb);
    return true;
}

int32_t SAO_PLUGINS_CALL menu_snapshot_v2_dispatch(
    void* rows, std::uint32_t capacity, std::uint32_t row_stride_bytes, std::uint32_t* out_count,
    std::uint64_t* out_revision, loader_ns::entity_snapshot_content_token_t* out_content_token,
    std::uint32_t* out_row_stride_bytes, void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || out_content_token == nullptr ||
        out_row_stride_bytes == nullptr || user_data == nullptr ||
        (capacity > 0 && rows == nullptr) || (rows == nullptr && row_stride_bytes != 0)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* provider = static_cast<menu_provider*>(user_data);
    if (provider->closing)
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    asIScriptEngine* engine =
        provider->builder != nullptr ? provider->builder->engine : nullptr;
    if (rows == nullptr &&
        !build_menu_snapshot(provider, engine, ctx_state_for(provider->ctx))) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    std::lock_guard lock(provider->mutex);
    *out_count = static_cast<std::uint32_t>(provider->rows.size());
    *out_revision = provider->revision;
    *out_content_token =
        provider->revision == loader_ns::kInvalidEntitySnapshotContentToken
            ? 1
            : provider->revision;
    *out_row_stride_bytes =
        provider->rows.empty()
            ? 0
            : static_cast<std::uint32_t>(sizeof(loader_ns::entity_menu_row_v2));
    if (capacity < provider->rows.size())
        return SAO_ERR_BUFFER_TOO_SMALL;
    if (!provider->rows.empty() &&
        row_stride_bytes < sizeof(loader_ns::entity_menu_row_v2)) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_ABI_MISMATCH;
    }
    if (!provider->rows.empty() &&
        row_stride_bytes % alignof(loader_ns::entity_menu_row_v2) != 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    for (std::size_t index = 0; index < provider->rows.size(); ++index) {
        const auto& source = provider->rows[index];
        const loader_ns::entity_menu_row_v2 row{
            sizeof(loader_ns::entity_menu_row_v2),
            provider->contribution_id.c_str(),
            provider->name.c_str(),
            provider->icon.c_str(),
            provider->priority,
            source.label.c_str(),
            source.icon.c_str(),
            source.action_id.c_str(),
            source.payload.c_str(),
            static_cast<std::uint8_t>(source.can_activate),
            static_cast<std::uint8_t>(source.keep_menu_open),
            static_cast<std::uint8_t>(source.close_menu_before),
            {},
        };
        std::memcpy(static_cast<std::byte*>(rows) + index * row_stride_bytes, &row,
                    sizeof(row));
    }
    return SAO_OK;
}

int32_t submit_action_result(loader_ns::entity_action_result_sink_v2_fn sink, void* sink_data,
                             bool handled, const char* json) noexcept {
    if (sink == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    const loader_ns::entity_action_result_v2 result{
        sizeof(loader_ns::entity_action_result_v2),
        loader_ns::kEntityActionAbiVersion2,
        static_cast<std::uint8_t>(handled ? 1 : 0),
        {},
        json,
    };
    return sink(&result, sink_data);
}

int32_t run_menu_action(as_ctx_callback* callback, const std::string& payload_json,
                        loader_ns::entity_action_result_sink_v2_fn sink,
                        void* sink_data) noexcept {
    ordered_json args = ordered_json::array();
    ordered_json payload = ordered_json::parse(payload_json, nullptr, false);
    args.push_back(payload.is_discarded() || payload.is_null() ? ordered_json::object()
                                                             : payload);
    cb_result result;
    const int32_t status = invoke_ctx_callback(callback, args, &result);
    if (status != SAO_OK)
        return submit_action_result(sink, sink_data, false, nullptr);
    const std::string json = parse_result_json(result);
    return submit_action_result(sink, sink_data, true, json.empty() ? nullptr : json.c_str());
}

int32_t SAO_PLUGINS_CALL provider_action_v2_dispatch(
    const char* action_id_utf8, const char* payload_json_utf8,
    loader_ns::entity_action_result_sink_v2_fn result_sink, void* result_sink_user_data,
    void* user_data) {
    if (action_id_utf8 == nullptr || payload_json_utf8 == nullptr || result_sink == nullptr ||
        user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* provider = static_cast<menu_provider*>(user_data);
    if (provider->closing)
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    std::lock_guard lock(provider->mutex);
    // ACTION_ONLY providers dispatch straight to their panel/global handler,
    // scoped to the action ids claimed by their panel meta when known.
    if (provider->panel_handler != nullptr) {
        if (!provider->claimed_ids.empty() &&
            provider->claimed_ids.find(action_id_utf8) == provider->claimed_ids.end()) {
            return submit_action_result(result_sink, result_sink_user_data, false, nullptr);
        }
        ordered_json args = ordered_json::array();
        args.push_back(action_id_utf8);
        ordered_json payload = ordered_json::parse(payload_json_utf8, nullptr, false);
        args.push_back(payload.is_discarded() || payload.is_null()
                           ? ordered_json::object()
                           : payload);
        cb_result result;
        const int32_t status =
            invoke_ctx_callback(provider->panel_handler, args, &result);
        if (status != SAO_OK)
            return submit_action_result(result_sink, result_sink_user_data, false, nullptr);
        const std::string json = parse_result_json(result);
        return submit_action_result(result_sink, result_sink_user_data, true,
                                    json.empty() ? nullptr : json.c_str());
    }
    const auto found = provider->actions.find(action_id_utf8);
    if (found == provider->actions.end()) {
        // fall back to the global action handler on the same ctx, if present.
        if (provider->ctx != nullptr) {
            if (const auto state = ctx_state_for(provider->ctx)) {
                if (state->global_action_handler != nullptr &&
                    state->global_action_handler->function != nullptr) {
                    ordered_json args = ordered_json::array();
                    args.push_back(action_id_utf8);
                    ordered_json payload =
                        ordered_json::parse(payload_json_utf8, nullptr, false);
                    args.push_back(payload.is_discarded() || payload.is_null()
                                       ? ordered_json::object()
                                       : payload);
                    cb_result result;
                    const int32_t status = invoke_ctx_callback(
                        state->global_action_handler, args, &result);
                    if (status != SAO_OK)
                        return submit_action_result(result_sink, result_sink_user_data, false,
                                                    nullptr);
                    const std::string json = parse_result_json(result);
                    return submit_action_result(result_sink, result_sink_user_data, true,
                                                json.empty() ? nullptr : json.c_str());
                }
            }
        }
        return submit_action_result(result_sink, result_sink_user_data, false, nullptr);
    }
    return run_menu_action(found->second.callback, payload_json_utf8, result_sink,
                           result_sink_user_data);
}

// ──────────────────────────────────────────────────────────────────────────
// entity provider registration helpers
// ──────────────────────────────────────────────────────────────────────────

std::string slugify(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9');
        out.push_back(alnum ? c : '-');
    }
    return out.empty() ? "item" : out;
}

std::shared_ptr<menu_provider> register_menu_provider(
    loader_ns::plugin_context_t* ctx, const std::shared_ptr<ctx_surface_state>& state,
    const std::string& provider_id, const std::string& contribution_id,
    const std::string& root_id, const std::string& name, const std::string& icon,
    double priority, as_ctx_callback* builder) {
    auto provider = std::make_shared<menu_provider>();
    provider->provider_id = provider_id;
    provider->contribution_id = contribution_id;
    provider->root_id = root_id;
    provider->name = name;
    provider->icon = icon;
    provider->priority = priority;
    provider->builder = builder;
    provider->ctx = ctx;
    provider->plugin = state != nullptr ? state->plugin : nullptr;

    loader_ns::entity_root_contribution_descriptor root{
        sizeof(loader_ns::entity_root_contribution_descriptor),
        provider->contribution_id.c_str(),
        provider->root_id.c_str(),
        provider->name.c_str(),
        provider->icon.c_str(),
        provider->priority,
    };
    loader_ns::context_entity_provider_descriptor_v3 descriptor{
        sizeof(loader_ns::context_entity_provider_descriptor_v3),
        provider->provider_id.c_str(),
        &menu_snapshot_v2_dispatch,
        nullptr,
        provider.get(),
        root_id.empty() ? nullptr : &root,
        &provider_action_v2_dispatch,
        provider.get(),
        0,
        0,
    };
    if (loader_ns::sao_plugins_ctx_register_entity_provider_v3(ctx, &descriptor) != SAO_OK)
        return nullptr;
    return provider;
}

bool register_action_only_provider(loader_ns::plugin_context_t* ctx,
                                   menu_provider* provider) {
    loader_ns::context_entity_provider_descriptor_v3 descriptor{
        sizeof(loader_ns::context_entity_provider_descriptor_v3),
        provider->provider_id.c_str(),
        nullptr,
        nullptr,
        provider,
        nullptr,
        &provider_action_v2_dispatch,
        provider,
        loader_ns::kContextEntityProviderV3ActionOnly,
        0,
    };
    return loader_ns::sao_plugins_ctx_register_entity_provider_v3(ctx, &descriptor) == SAO_OK;
}


// ──────────────────────────────────────────────────────────────────────────
// UiBuilder / MemAccess / LocalModule object types
// ──────────────────────────────────────────────────────────────────────────

struct ui_builder_object {};

ui_builder_object g_ui_builder;

struct mem_access_object {};

mem_access_object g_mem_access;

struct local_module_handle {
    std::atomic_uint32_t references{1};
    std::shared_ptr<sc::script_module> module;
};

void ui_builder_method(asIScriptGeneric* generic) {
    try {
        asIScriptFunction* function = generic->GetFunction();
        const char* method = function != nullptr ? function->GetName() : "";
        asIScriptEngine* engine = generic->GetEngine();
        ordered_json args = ordered_json::array();
        for (asUINT index = 0; static_cast<int>(index) < generic->GetArgCount(); ++index)
            args.push_back(generic_arg_to_json(engine, generic, index));
        nlohmann::json ordered_args = args;
        nlohmann::json node;
        std::string error;
        if (!sc::script_ui_build(method, ordered_args, node, error)) {
            set_active_exception(error.empty() ? "ui build failed" : error.c_str());
            return;
        }
        CScriptDictionary* dict = dictionary_from_json(engine, ordered_json(node));
        *static_cast<CScriptDictionary**>(generic->GetAddressOfReturnLocation()) = dict;
    } catch (...) {
        set_active_exception("ui build failed");
    }
}

SaoSdkContext* mem_sdk_context(asIScriptGeneric* generic) {
    return gpu_hunt_binding_context(generic->GetEngine());
}

void mem_attach(asIScriptGeneric* generic) {
    SaoSdkContext* sdk = mem_sdk_context(generic);
    bool ok = false;
    if (sdk != nullptr) {
        SaoSdkMemoryTargetIdentity identity{};
        identity.struct_size = SAO_SDK_MEMORY_TARGET_IDENTITY_REQUIRED_SIZE;
        identity.abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
        identity.process_id = generic->GetArgDWord(0);
        const std::string image = arg_string(generic, 1);
        if (!image.empty()) {
            const size_t length = (std::min)(image.size(), size_t(SAO_SDK_MEMORY_NAME_CAPACITY - 1));
            std::memcpy(identity.image_name_utf8, image.data(), length);
        }
        ok = sao_sdk_mem_attach(sdk, &identity) == SAO_SDK_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void mem_detach(asIScriptGeneric* generic) {
    if (SaoSdkContext* sdk = mem_sdk_context(generic))
        (void)sao_sdk_mem_detach(sdk);
}

void mem_module_base(asIScriptGeneric* generic) {
    SaoSdkContext* sdk = mem_sdk_context(generic);
    const std::string name = arg_string(generic, 0);
    uint64_t base = 0;
    if (sdk != nullptr && !name.empty())
        (void)sao_sdk_mem_module_base(sdk, name.c_str(), &base);
    generic->SetReturnQWord(base);
}

void mem_read_u32(asIScriptGeneric* generic) {
    SaoSdkContext* sdk = mem_sdk_context(generic);
    uint32_t value = 0;
    if (sdk != nullptr)
        (void)sao_sdk_mem_read_u32(sdk, generic->GetArgQWord(0), &value);
    generic->SetReturnDWord(value);
}

void mem_read_u64(asIScriptGeneric* generic) {
    SaoSdkContext* sdk = mem_sdk_context(generic);
    uint64_t value = 0;
    if (sdk != nullptr)
        (void)sao_sdk_mem_read_u64(sdk, generic->GetArgQWord(0), &value);
    generic->SetReturnQWord(value);
}

void mem_read(asIScriptGeneric* generic) {
    SaoSdkContext* sdk = mem_sdk_context(generic);
    const uint64_t address = generic->GetArgQWord(0);
    const uint32_t length = generic->GetArgDWord(1);
    CScriptArray* output = nullptr;
    if (sdk != nullptr && length != 0 && length <= (64u * 1024u * 1024u)) {
        std::vector<uint8_t> buffer(length);
        size_t read = 0;
        if (sao_sdk_mem_read(sdk, address, buffer.data(), buffer.size(), &read) == SAO_SDK_OK &&
            read > 0) {
            if (asITypeInfo* type = generic->GetEngine()->GetTypeInfoByDecl("array<uint8>")) {
                output = CScriptArray::Create(type, static_cast<asUINT>(read));
                if (output != nullptr)
                    std::memcpy(output->At(0), buffer.data(), read);
            }
        }
    }
    *static_cast<CScriptArray**>(generic->GetAddressOfReturnLocation()) = output;
}

void mem_read_ptr_chain(asIScriptGeneric* generic) {
    SaoSdkContext* sdk = mem_sdk_context(generic);
    const uint64_t base = generic->GetArgQWord(0);
    uint64_t final_address = 0;
    if (sdk != nullptr) {
        if (const CScriptArray* offsets =
                *static_cast<const CScriptArray* const*>(generic->GetAddressOfArg(1));
            offsets != nullptr) {
            const int element_type = offsets->GetElementTypeId();
            std::vector<int32_t> chain;
            chain.reserve(offsets->GetSize());
            for (asUINT index = 0; index < offsets->GetSize(); ++index) {
                const void* element = offsets->At(index);
                int64_t value = 0;
                switch (element_type) {
                case asTYPEID_INT8:
                    value = *static_cast<const int8_t*>(element);
                    break;
                case asTYPEID_INT16:
                    value = *static_cast<const int16_t*>(element);
                    break;
                case asTYPEID_INT32:
                    value = *static_cast<const int32_t*>(element);
                    break;
                case asTYPEID_INT64:
                    value = *static_cast<const int64_t*>(element);
                    break;
                case asTYPEID_UINT8:
                    value = *static_cast<const uint8_t*>(element);
                    break;
                case asTYPEID_UINT16:
                    value = *static_cast<const uint16_t*>(element);
                    break;
                case asTYPEID_UINT32:
                    value = *static_cast<const uint32_t*>(element);
                    break;
                case asTYPEID_UINT64:
                    value = static_cast<int64_t>(*static_cast<const uint64_t*>(element));
                    break;
                default:
                    break;
                }
                chain.push_back(static_cast<int32_t>(value));
            }
            if (!chain.empty())
                (void)sao_sdk_mem_read_ptr_chain(sdk, base, chain.data(), chain.size(),
                                                 &final_address);
        }
    }
    generic->SetReturnQWord(final_address);
}

void mem_enumerate_modules(asIScriptGeneric* generic) {
    SaoSdkContext* sdk = mem_sdk_context(generic);
    CScriptDictionary* output = nullptr;
    if (sdk != nullptr && sao_sdk_mem_v1_5_status(sdk) == SAO_SDK_OK) {
        std::vector<SaoSdkMemoryModule> modules(256);
        size_t count = 0;
        sao_sdk_status_t status = sao_sdk_mem_enumerate_modules(
            sdk, modules.data(), modules.size(), sizeof(SaoSdkMemoryModule), &count);
        if (status == SAO_SDK_ERR_BUFFER_TOO_SMALL &&
            count > modules.size()) {
            modules.resize((std::min)(count, size_t(SAO_SDK_MEMORY_MAX_MODULE_COUNT)));
            status = sao_sdk_mem_enumerate_modules(sdk, modules.data(), modules.size(),
                                                   sizeof(SaoSdkMemoryModule), &count);
        }
        if (status == SAO_SDK_OK) {
            modules.resize((std::min)(count, modules.size()));
            output = CScriptDictionary::Create(generic->GetEngine());
            if (output != nullptr) {
                for (const auto& module : modules) {
                    CScriptDictionary* entry =
                        CScriptDictionary::Create(generic->GetEngine());
                    if (entry == nullptr)
                        continue;
                    const asINT64 base = static_cast<asINT64>(module.base_address);
                    const asINT64 size = static_cast<asINT64>(module.image_size);
                    entry->Set("base", base);
                    entry->Set("size", size);
                    void* handle = entry;
                    asITypeInfo* dict_type =
                        generic->GetEngine()->GetTypeInfoByDecl("dictionary");
                    output->Set(module.name_utf8, &handle,
                                dict_type->GetTypeId() | asTYPEID_OBJHANDLE);
                    generic->GetEngine()->ReleaseScriptObject(entry, dict_type);
                }
            }
        }
    }
    *static_cast<CScriptDictionary**>(generic->GetAddressOfReturnLocation()) = output;
}

// ── LocalModule ──

ordered_json script_value_to_json(const sc::script_value& value) {
    using kind = sc::script_value::kind;
    switch (value.k) {
    case kind::boolean:
        return ordered_json(value.boolean);
    case kind::integer:
        return ordered_json(value.integer);
    case kind::number:
        return std::isfinite(value.number) ? ordered_json(value.number)
                                           : ordered_json(nullptr);
    case kind::string:
    case kind::bytes:
        return ordered_json(value.text);
    case kind::list: {
        ordered_json array = ordered_json::array();
        for (const auto& item : value.items)
            array.push_back(item ? script_value_to_json(*item) : ordered_json(nullptr));
        return array;
    }
    case kind::map: {
        ordered_json object = ordered_json::object();
        for (const auto& [key, item] : value.object)
            object[key] = item ? script_value_to_json(*item) : ordered_json(nullptr);
        return object;
    }
    default:
        return ordered_json(nullptr);
    }
}

sc::script_value_ptr json_to_script_value(const ordered_json& value) {
    if (value.is_null())
        return sc::script_value::null_value();
    if (value.is_boolean())
        return sc::script_value::make_boolean(value.get<bool>());
    if (value.is_number_integer())
        return sc::script_value::make_integer(value.get<int64_t>());
    if (value.is_number_unsigned()) {
        const uint64_t raw = value.get<uint64_t>();
        if (raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return sc::script_value::make_number(static_cast<double>(raw));
        return sc::script_value::make_integer(static_cast<int64_t>(raw));
    }
    if (value.is_number_float())
        return sc::script_value::make_number(value.get<double>());
    if (value.is_string())
        return sc::script_value::make_string(value.get<std::string>());
    if (value.is_array()) {
        std::vector<sc::script_value_ptr> items;
        items.reserve(value.size());
        for (const auto& item : value)
            items.push_back(json_to_script_value(item));
        return sc::script_value::make_list(std::move(items));
    }
    std::vector<std::pair<std::string, sc::script_value_ptr>> object;
    object.reserve(value.size());
    for (auto it = value.begin(); it != value.end(); ++it)
        object.emplace_back(it.key(), json_to_script_value(it.value()));
    return sc::script_value::make_map(std::move(object));
}

void local_module_add_ref(asIScriptGeneric* generic) {
    static_cast<local_module_handle*>(generic->GetObject())
        ->references.fetch_add(1, std::memory_order_relaxed);
}

void local_module_release(asIScriptGeneric* generic) {
    auto* handle = static_cast<local_module_handle*>(generic->GetObject());
    if (handle != nullptr &&
        handle->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete handle;
}

local_module_handle* local_module_of(asIScriptGeneric* generic) {
    return static_cast<local_module_handle*>(generic->GetObject());
}

void local_module_id(asIScriptGeneric* generic) {
    const auto* handle = local_module_of(generic);
    return_string(generic, handle != nullptr && handle->module != nullptr
                                 ? handle->module->module_id()
                                 : std::string{});
}

void local_module_names(asIScriptGeneric* generic) {
    const auto* handle = local_module_of(generic);
    CScriptArray* names = nullptr;
    if (handle != nullptr && handle->module != nullptr) {
        try {
            const std::vector<std::string> members = handle->module->member_names();
            if (asITypeInfo* type =
                    generic->GetEngine()->GetTypeInfoByDecl("array<string>")) {
                names = CScriptArray::Create(type, static_cast<asUINT>(members.size()));
                if (names != nullptr) {
                    asITypeInfo* string_type =
                        generic->GetEngine()->GetTypeInfoByDecl("string");
                    asUINT index = 0;
                    for (const auto& member : members) {
                        as_string text = member;
                        generic->GetEngine()->AssignScriptObject(names->At(index++), &text,
                                                                 string_type);
                    }
                }
            }
        } catch (...) {
        }
    }
    *static_cast<CScriptArray**>(generic->GetAddressOfReturnLocation()) = names;
}

void local_module_get(asIScriptGeneric* generic) {
    const auto* handle = local_module_of(generic);
    void* result = nullptr;
    if (handle != nullptr && handle->module != nullptr) {
        const std::string name = arg_string(generic, 0);
        sc::script_value_ptr value;
        std::string error;
        if (handle->module->get(name, &value, &error) == SAO_OK && value)
            result = json_ref_from_ordered(generic->GetEngine(),
                                           script_value_to_json(*value));
    }
    *static_cast<void**>(generic->GetAddressOfReturnLocation()) = result;
}

void local_module_call(asIScriptGeneric* generic) {
    const auto* handle = local_module_of(generic);
    void* result = nullptr;
    if (handle != nullptr && handle->module != nullptr) {
        const std::string name = arg_string(generic, 0);
        const ordered_json args = generic_arg_to_json(generic->GetEngine(), generic, 1);
        std::vector<sc::script_value_ptr> positional;
        if (args.is_array()) {
            positional.reserve(args.size());
            for (const auto& item : args)
                positional.push_back(json_to_script_value(item));
        } else if (!args.is_null()) {
            positional.push_back(json_to_script_value(args));
        }
        sc::script_value_ptr value;
        std::string error;
        if (handle->module->call(name, positional, &value, &error) == SAO_OK && value)
            result = json_ref_from_ordered(generic->GetEngine(),
                                           script_value_to_json(*value));
    }
    *static_cast<void**>(generic->GetAddressOfReturnLocation()) = result;
}

// ──────────────────────────────────────────────────────────────────────────
// .as engine provider for runtime_bridge
// ──────────────────────────────────────────────────────────────────────────

shared_plugin_state plugin_for_context(loader_ns::plugin_context_t* ctx) {
    return acquire_plugin_state_by_bound_context(ctx);
}

struct as_helper_module final : sc::script_module {
    shared_plugin_state plugin;
    std::string module_name;
    std::string identity;

    ~as_helper_module() override {
        if (plugin != nullptr && plugin->engine != nullptr && !module_name.empty()) {
            engine_execution_guard engine_lock;
            plugin->engine->DiscardModule(module_name.c_str());
        }
    }

    const std::string& module_id() const noexcept override { return identity; }

    std::vector<std::string> member_names() const override {
        std::vector<std::string> names;
        if (plugin == nullptr || plugin->engine == nullptr)
            return names;
        engine_execution_guard engine_lock;
        asIScriptModule* module = plugin->engine->GetModule(module_name.c_str(),
                                                          asGM_ONLY_IF_EXISTS);
        if (module == nullptr)
            return names;
        for (asUINT index = 0; index < module->GetGlobalVarCount(); ++index) {
            const char* name = nullptr;
            if (module->GetGlobalVar(index, &name) >= 0 && name != nullptr)
                names.emplace_back(name);
        }
        for (asUINT index = 0; index < module->GetFunctionCount(); ++index) {
            if (asIScriptFunction* fn = module->GetFunctionByIndex(index)) {
                if (const char* name = fn->GetName())
                    names.emplace_back(name);
            }
        }
        return names;
    }

    int32_t get(const std::string& name, sc::script_value_ptr* out_value,
                std::string* out_error) override {
        if (out_value == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        *out_value = sc::script_value::null_value();
        if (plugin == nullptr || plugin->engine == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        engine_execution_guard engine_lock;
        asIScriptModule* module = plugin->engine->GetModule(module_name.c_str(),
                                                          asGM_ONLY_IF_EXISTS);
        if (module == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        const int index = module->GetGlobalVarIndexByName(name.c_str());
        if (index < 0) {
            if (module->GetFunctionByName(name.c_str()) != nullptr) {
                const shared_plugin_state keep = plugin;
                const std::string function_name = name;
                const std::string owning_module = module_name;
                *out_value = sc::script_value::make_function(
                    [keep, owning_module, function_name](
                        const std::vector<sc::script_value_ptr>& args,
                        sc::script_value_ptr* result, std::string* error) -> int32_t {
                        if (result == nullptr)
                            return SAO_ERR_INVALID_ARGUMENT;
                        *result = sc::script_value::null_value();
                        if (keep == nullptr || keep->engine == nullptr)
                            return SAO_ERR_HANDLE_INVALID;
                        engine_execution_guard engine_lock;
                        asIScriptModule* module = keep->engine->GetModule(
                            owning_module.c_str(), asGM_ONLY_IF_EXISTS);
                        if (module == nullptr)
                            return SAO_ERR_HANDLE_INVALID;
                        asIScriptFunction* fn =
                            module->GetFunctionByName(function_name.c_str());
                        if (fn == nullptr)
                            return SAO_ERR_HANDLE_INVALID;
                        ordered_json positional = ordered_json::array();
                        for (const auto& arg : args)
                            positional.push_back(
                                arg ? script_value_to_json(*arg) : ordered_json(nullptr));
                        asIScriptContext* raw = keep->engine->CreateContext();
                        if (raw == nullptr)
                            return SAO_ERR_OS_CALL_FAILED;
                        const std::unique_ptr<asIScriptContext,
                                            void (*)(asIScriptContext*)>
                            context(raw, [](asIScriptContext* v) { v->Release(); });
                        if (context->Prepare(fn) < 0)
                            return SAO_ERR_OS_CALL_FAILED;
                        std::vector<as_string> string_args;
                        std::vector<std::pair<void*, const asITypeInfo*>> release_after;
                        const int32_t arg_status = set_callback_arguments(
                            context.get(), fn, keep->engine, positional, string_args,
                            release_after);
                        if (arg_status != SAO_OK) {
                            for (const auto& [object, type] : release_after)
                                keep->engine->ReleaseScriptObject(object, type);
                            return arg_status;
                        }
                        if (context->Execute() != asEXECUTION_FINISHED) {
                            for (const auto& [object, type] : release_after)
                                keep->engine->ReleaseScriptObject(object, type);
                            if (error != nullptr)
                                *error = "helper function did not finish";
                            return SAO_ERR_OS_CALL_FAILED;
                        }
                        for (const auto& [object, type] : release_after)
                            keep->engine->ReleaseScriptObject(object, type);
                        cb_result result_holder;
                        const ordered_json value = return_value_to_json(
                            context.get(), fn, keep->engine, &result_holder);
                        release_return_object(keep->engine, result_holder);
                        *result = json_to_script_value(value);
                        return SAO_OK;
                    });
                return SAO_OK;
            }
            if (out_error != nullptr)
                *out_error = "member not found";
            return SAO_ERR_HANDLE_INVALID;
        }
        int type_id = 0;
        module->GetGlobalVar(static_cast<asUINT>(index), nullptr, nullptr, &type_id,
                             nullptr);
        void* address = module->GetAddressOfGlobalVar(static_cast<asUINT>(index));
        if (address == nullptr)
            return SAO_ERR_OS_CALL_FAILED;
        *out_value =
            json_to_script_value(value_to_json(plugin->engine, address, type_id));
        return SAO_OK;
    }

    int32_t call(const std::string& name, const std::vector<sc::script_value_ptr>& args,
                 sc::script_value_ptr* out_value, std::string* out_error) override {
        if (out_value == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        *out_value = sc::script_value::null_value();
        if (plugin == nullptr || plugin->engine == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        engine_execution_guard engine_lock;
        asIScriptModule* module =
            plugin->engine->GetModule(module_name.c_str(), asGM_ONLY_IF_EXISTS);
        if (module == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        asIScriptFunction* fn = module->GetFunctionByName(name.c_str());
        if (fn == nullptr) {
            if (out_error != nullptr)
                *out_error = "function not found";
            return SAO_ERR_HANDLE_INVALID;
        }
        ordered_json positional = ordered_json::array();
        for (const auto& arg : args)
            positional.push_back(arg ? script_value_to_json(*arg) : ordered_json(nullptr));
        asIScriptContext* raw = plugin->engine->CreateContext();
        if (raw == nullptr)
            return SAO_ERR_OS_CALL_FAILED;
        const std::unique_ptr<asIScriptContext, void (*)(asIScriptContext*)> context(
            raw, [](asIScriptContext* value) { value->Release(); });
        if (context->Prepare(fn) < 0)
            return SAO_ERR_OS_CALL_FAILED;
        context->SetUserData(plugin.get(), kPluginContextUserDataSlot);
        std::vector<as_string> string_args;
        std::vector<std::pair<void*, const asITypeInfo*>> release_after;
        const int32_t arg_status = set_callback_arguments(
            context.get(), fn, plugin->engine, positional, string_args, release_after);
        if (arg_status != SAO_OK) {
            for (const auto& [object, type] : release_after)
                plugin->engine->ReleaseScriptObject(object, type);
            return arg_status;
        }
        if (context->Execute() != asEXECUTION_FINISHED) {
            for (const auto& [object, type] : release_after)
                plugin->engine->ReleaseScriptObject(object, type);
            if (out_error != nullptr)
                *out_error = "helper function did not finish";
            return SAO_ERR_OS_CALL_FAILED;
        }
        for (const auto& [object, type] : release_after)
            plugin->engine->ReleaseScriptObject(object, type);
        cb_result result;
        const ordered_json value =
            return_value_to_json(context.get(), fn, plugin->engine, &result);
        release_return_object(plugin->engine, result);
        *out_value = json_to_script_value(value);
        return SAO_OK;
    }
};

bool SAO_PLUGINS_CALL as_provider_probe(loader_ns::plugin_context_t* ctx,
                                        const wchar_t* abs_path, std::string& note,
                                        void*) noexcept {
    try {
        const shared_plugin_state plugin = plugin_for_context(ctx);
        if (plugin == nullptr || plugin->engine == nullptr ||
            plugin->lifecycle != plugin_runtime_state::ready) {
            note = "angelscript engine unavailable for ctx";
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::exists(abs_path, ec)) {
            note = "file missing";
            return false;
        }
        return true;
    } catch (...) {
        note = "probe failed";
        return false;
    }
}

int32_t SAO_PLUGINS_CALL as_provider_load(loader_ns::plugin_context_t* ctx,
                                          const wchar_t* abs_path,
                                          const std::string& logical_name,
                                          std::shared_ptr<sc::script_module>* out_module,
                                          std::string* out_error, void*) noexcept {
    if (out_module == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    out_module->reset();
    try {
        const shared_plugin_state plugin = plugin_for_context(ctx);
        if (plugin == nullptr || plugin->engine == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        std::ifstream input(abs_path, std::ios::binary);
        if (!input) {
            if (out_error != nullptr)
                *out_error = "cannot read module file";
            return SAO_ERR_HANDLE_INVALID;
        }
        const std::string source{std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()};
        engine_execution_guard engine_lock;
        asIScriptModule* module =
            plugin->engine->GetModule(logical_name.c_str(), asGM_ALWAYS_CREATE);
        if (module == nullptr) {
            if (out_error != nullptr)
                *out_error = "module creation failed";
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (module->AddScriptSection(logical_name.c_str(), source.c_str(), source.size()) <
                0 ||
            module->Build() < 0) {
            if (out_error != nullptr)
                *out_error = "script build failed";
            plugin->engine->DiscardModule(logical_name.c_str());
            return SAO_ERR_INVALID_ARGUMENT;
        }
        (void)sao_plugins_ashost_bind_ctx(plugin->engine, ctx, logical_name.c_str());
        auto helper = std::make_shared<as_helper_module>();
        helper->plugin = plugin;
        helper->module_name = logical_name;
        helper->identity = logical_name;
        *out_module = std::move(helper);
        return SAO_OK;
    } catch (...) {
        if (out_error != nullptr)
            *out_error = "module load raised a C++ exception";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

const char* const kAsExtensions[] = {"as", nullptr};
const script_ctx::script_engine_ops kAsProvider{
    "angelscript",
    50u,
    kAsExtensions,
    &as_provider_probe,
    &as_provider_load,
    nullptr,
};

void register_as_provider_once() {
    static std::once_flag once;
    std::call_once(once, [] { (void)sc::runtime_bridge_register(&kAsProvider); });
}

// ──────────────────────────────────────────────────────────────────────────
// PluginContext property accessors
// ──────────────────────────────────────────────────────────────────────────

void ctx_get_path(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    return_string(generic,
                  ctx != nullptr ? utf8_from_wide(loader_ns::sao_plugins_ctx_path(ctx))
                                 : std::string{});
}

void ctx_get_assets_path(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    std::wstring value;
    if (ctx != nullptr) {
        if (const wchar_t* path = loader_ns::sao_plugins_ctx_path(ctx)) {
            value.assign(path);
            if (!value.empty() && value.back() != L'/' && value.back() != L'\\')
                value.push_back(L'\\');
            value.append(L"assets");
        }
    }
    return_string(generic, utf8_from_wide(value.c_str()));
}

void ctx_get_web_path(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    std::wstring value;
    if (ctx != nullptr) {
        if (const wchar_t* path = loader_ns::sao_plugins_ctx_path(ctx)) {
            value.assign(path);
            if (!value.empty() && value.back() != L'/' && value.back() != L'\\')
                value.push_back(L'\\');
            value.append(L"web");
        }
    }
    return_string(generic, utf8_from_wide(value.c_str()));
}

void ctx_get_engine_property(asIScriptGeneric* generic) {
    *static_cast<void**>(generic->GetAddressOfReturnLocation()) = generic->GetObject();
}

void ctx_get_event_bus(asIScriptGeneric* generic) {
    *static_cast<void**>(generic->GetAddressOfReturnLocation()) = generic->GetObject();
}

void ctx_get_ui(asIScriptGeneric* generic) {
    *static_cast<void**>(generic->GetAddressOfReturnLocation()) = &g_ui_builder;
}

void ctx_get_mem(asIScriptGeneric* generic) {
    *static_cast<void**>(generic->GetAddressOfReturnLocation()) = &g_mem_access;
}

void ctx_get_owner(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    CScriptDictionary* owner = nullptr;
    if (ctx != nullptr) {
        if (const auto state = ctx_state_or_create(ctx, generic->GetEngine())) {
            if (state->owner == nullptr)
                state->owner = CScriptDictionary::Create(generic->GetEngine());
            owner = state->owner;
            // the state keeps its ref; the handle slot consumes a fresh one.
            if (owner != nullptr) {
                if (asITypeInfo* dict_type =
                        generic->GetEngine()->GetTypeInfoByDecl("dictionary")) {
                    generic->GetEngine()->AddRefScriptObject(owner, dict_type);
                }
            }
        }
    }
    *static_cast<CScriptDictionary**>(generic->GetAddressOfReturnLocation()) = owner;
}

// ──────────────────────────────────────────────────────────────────────────
// event bindings
// ──────────────────────────────────────────────────────────────────────────

int64_t subscribe_impl(asIScriptGeneric* generic, bool once, const char* fixed_topic) {
    auto* ctx = context_of(generic);
    if (ctx == nullptr)
        return -1;
    const std::string topic =
        fixed_topic != nullptr ? std::string(fixed_topic) : arg_string(generic, 0);
    const asUINT cb_index = fixed_topic != nullptr ? 0 : 1;
    asIScriptFunction* fn = arg_funcdef(generic->GetEngine(), generic, cb_index);
    if (topic.empty() || fn == nullptr)
        return -1;
    const auto state = ctx_state_or_create(ctx, generic->GetEngine());
    if (!state)
        return -1;
    as_ctx_callback* record = make_callback(state, fn, generic->GetEngine());
    if (record == nullptr)
        return -1;
    std::uint32_t token = 0;
    const int32_t status =
        once ? loader_ns::sao_plugins_ctx_subscribe_once(ctx, topic.c_str(), &event_dispatch,
                                                       record, &token)
             : loader_ns::sao_plugins_ctx_subscribe(ctx, topic.c_str(), &event_dispatch,
                                                    record, &token);
    if (status != SAO_OK || token == 0) {
        retire_callback(state, record);
        return -1;
    }
    state->subscription_tokens.emplace(token, record);
    return static_cast<int64_t>(token);
}

void ctx_subscribe(asIScriptGeneric* generic) {
    generic->SetReturnQWord(subscribe_impl(generic, false, nullptr));
}

void ctx_subscribe_once(asIScriptGeneric* generic) {
    generic->SetReturnQWord(subscribe_impl(generic, true, nullptr));
}

void ctx_on(asIScriptGeneric* generic) {
    generic->SetReturnQWord(subscribe_impl(generic, false, nullptr));
}

void ctx_on_damage(asIScriptGeneric* generic) {
    generic->SetReturnQWord(subscribe_impl(generic, false, "damage"));
}

void ctx_on_heal(asIScriptGeneric* generic) {
    generic->SetReturnQWord(subscribe_impl(generic, false, "heal"));
}

void ctx_on_skill(asIScriptGeneric* generic) {
    generic->SetReturnQWord(subscribe_impl(generic, false, "skill"));
}

void ctx_on_boss(asIScriptGeneric* generic) {
    generic->SetReturnQWord(subscribe_impl(generic, false, "boss"));
}

void ctx_on_snapshot(asIScriptGeneric* generic) {
    generic->SetReturnQWord(subscribe_impl(generic, false, "act_snapshot"));
}

void ctx_on_encounter_finalized(asIScriptGeneric* generic) {
    generic->SetReturnQWord(subscribe_impl(generic, false, "encounter_finalized"));
}

void ctx_unsubscribe(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const auto token = static_cast<std::uint32_t>(generic->GetArgQWord(0));
    bool ok = ctx != nullptr &&
              loader_ns::sao_plugins_ctx_unsubscribe(ctx, token) == SAO_OK;
    if (ok) {
        if (const auto state = ctx_state_for(ctx)) {
            const auto found = state->subscription_tokens.find(token);
            if (found != state->subscription_tokens.end()) {
                as_ctx_callback* record = found->second;
                state->subscription_tokens.erase(found);
                retire_callback(state, record);
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_emit(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string topic = arg_string(generic, 0);
    const ordered_json payload = generic_arg_to_json(generic->GetEngine(), generic, 1);
    bool ok = false;
    if (ctx != nullptr && !topic.empty()) {
        const std::string text = payload.is_null() ? "{}" : payload.dump();
        ok = loader_ns::sao_plugins_ctx_emit(ctx, topic.c_str(), text.c_str()) == SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_get_snapshot(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    CScriptDictionary* output = nullptr;
    if (ctx != nullptr) {
        char* raw = nullptr;
        if (loader_ns::sao_plugins_ctx_get_snapshot(ctx, &raw) == SAO_OK && raw != nullptr) {
            const ordered_json parsed = ordered_json::parse(raw, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object())
                output = dictionary_from_json(generic->GetEngine(), parsed);
            loader_ns::sao_plugins_ctx_free_string(raw);
        }
    }
    *static_cast<CScriptDictionary**>(generic->GetAddressOfReturnLocation()) = output;
}

void ctx_snapshot_value(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string path = arg_string(generic, 0);
    void* output = nullptr;
    if (ctx != nullptr && !path.empty()) {
        char* raw = nullptr;
        if (loader_ns::sao_plugins_ctx_snapshot_value(ctx, path.c_str(), &raw) == SAO_OK &&
            raw != nullptr) {
            ordered_json parsed = ordered_json::parse(raw, nullptr, false);
            if (parsed.is_discarded())
                parsed = ordered_json(raw);
            if (parsed.is_null())
                parsed = generic_arg_to_json(generic->GetEngine(), generic, 1);
            output = json_ref_from_ordered(generic->GetEngine(), parsed);
            loader_ns::sao_plugins_ctx_free_string(raw);
        } else {
            const ordered_json fallback =
                generic_arg_to_json(generic->GetEngine(), generic, 1);
            output = json_ref_from_ordered(generic->GetEngine(), fallback);
        }
    }
    *static_cast<void**>(generic->GetAddressOfReturnLocation()) = output;
}

void ctx_recent_events(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const auto limit = static_cast<std::uint32_t>(generic->GetArgDWord(0));
    const std::string topic = arg_string(generic, 1);
    CScriptArray* output = nullptr;
    if (ctx != nullptr) {
        char* raw = nullptr;
        if (loader_ns::sao_plugins_ctx_recent_events(
                ctx, limit, topic.empty() ? nullptr : topic.c_str(), &raw) == SAO_OK &&
            raw != nullptr) {
            const ordered_json parsed = ordered_json::parse(raw, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_array())
                output = array_from_json(generic->GetEngine(), parsed);
            loader_ns::sao_plugins_ctx_free_string(raw);
        }
    }
    *static_cast<CScriptArray**>(generic->GetAddressOfReturnLocation()) = output;
}

// ──────────────────────────────────────────────────────────────────────────
// settings bindings
// ──────────────────────────────────────────────────────────────────────────

void ctx_get_setting(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string key = arg_string(generic, 0);
    void* output = nullptr;
    if (ctx != nullptr && !key.empty()) {
        char* raw = nullptr;
        if (loader_ns::sao_plugins_ctx_get_setting(ctx, key.c_str(), &raw) == SAO_OK &&
            raw != nullptr) {
            ordered_json parsed = ordered_json::parse(raw, nullptr, false);
            if (parsed.is_discarded())
                parsed = ordered_json(raw);
            output = json_ref_from_ordered(generic->GetEngine(), parsed);
            loader_ns::sao_plugins_ctx_free_string(raw);
        } else {
            output = json_ref_from_ordered(generic->GetEngine(),
                                           generic_arg_to_json(generic->GetEngine(), generic,
                                                               1));
        }
    }
    *static_cast<void**>(generic->GetAddressOfReturnLocation()) = output;
}

void ctx_setting(asIScriptGeneric* generic) {
    ctx_get_setting(generic);
}

void ctx_set_setting(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string key = arg_string(generic, 0);
    const ordered_json value = generic_arg_to_json(generic->GetEngine(), generic, 1);
    bool ok = false;
    if (ctx != nullptr && !key.empty()) {
        const std::string text = value.is_null() ? "null" : value.dump();
        ok = loader_ns::sao_plugins_ctx_set_setting(ctx, key.c_str(), text.c_str()) ==
             SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_set_defaults(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const ordered_json defaults = generic_arg_to_json(generic->GetEngine(), generic, 0);
    bool ok = false;
    if (ctx != nullptr) {
        const std::string text = defaults.is_null() ? "{}" : defaults.dump();
        ok = loader_ns::sao_plugins_ctx_set_defaults(ctx, text.c_str()) == SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

// ──────────────────────────────────────────────────────────────────────────
// ui panel / render hook / overlay / extension registration
// ──────────────────────────────────────────────────────────────────────────

void ctx_register_ui_panel(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string panel_id = arg_string(generic, 0);
    const ordered_json meta = generic_arg_to_json(generic->GetEngine(), generic, 1);
    asIScriptFunction* render_fn = arg_funcdef(generic->GetEngine(), generic, 2);
    asIScriptFunction* action_fn = arg_funcdef(generic->GetEngine(), generic, 3);
    bool ok = false;
    if (ctx != nullptr && !panel_id.empty()) {
        const std::string meta_text = meta.is_null() ? "{}" : meta.dump();
        const auto state = ctx_state_or_create(ctx, generic->GetEngine());
        as_ctx_callback* render_record = nullptr;
        as_ctx_callback* action_record = nullptr;
        if (state) {
            render_record = render_fn != nullptr ? make_callback(state, render_fn, generic->GetEngine()) : nullptr;
            action_record = action_fn != nullptr ? make_callback(state, action_fn, generic->GetEngine()) : nullptr;
        }
        auto record = std::make_unique<panel_record>();
        record->render = render_record;
        record->action = action_record;
        ok = loader_ns::sao_plugins_ctx_register_ui_panel(
                 ctx, panel_id.c_str(), meta_text.c_str(),
                 render_record != nullptr ? &panel_render_dispatch : nullptr,
                 action_record != nullptr ? &panel_action_dispatch : nullptr,
                 record.get()) == SAO_OK;
        if (ok && state)
            state->panel_records.push_back(std::move(record));
        if (!ok && state) {
            if (render_record != nullptr)
                retire_callback(state, render_record);
            if (action_record != nullptr)
                retire_callback(state, action_record);
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_register_render_hook(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string surface = arg_string(generic, 0);
    asIScriptFunction* fn = arg_funcdef(generic->GetEngine(), generic, 1);
    const float priority = static_cast<float>(generic->GetArgDouble(2));
    int64_t token = -1;
    if (ctx != nullptr && !surface.empty() && fn != nullptr) {
        if (const auto state = ctx_state_or_create(ctx, generic->GetEngine())) {
            as_ctx_callback* record = make_callback(state, fn, generic->GetEngine());
            if (record != nullptr) {
                std::uint32_t raw_token = 0;
                if (loader_ns::sao_plugins_ctx_register_render_hook(
                        ctx, surface.c_str(), priority, &render_hook_dispatch, record,
                        &raw_token) == SAO_OK) {
                    state->render_hooks.emplace(raw_token, record);
                    token = static_cast<int64_t>(raw_token);
                } else {
                    retire_callback(state, record);
                }
            }
        }
    }
    generic->SetReturnQWord(token);
}

void ctx_unregister_render_hook(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const auto token = static_cast<std::uint32_t>(generic->GetArgQWord(0));
    bool ok = ctx != nullptr &&
              loader_ns::sao_plugins_ctx_unregister_render_hook(ctx, token) == SAO_OK;
    if (ok) {
        if (const auto state = ctx_state_for(ctx)) {
            const auto found = state->render_hooks.find(token);
            if (found != state->render_hooks.end()) {
                as_ctx_callback* record = found->second;
                state->render_hooks.erase(found);
                retire_callback(state, record);
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_set_overlay(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string surface = arg_string(generic, 0);
    const ordered_json spec = generic_arg_to_json(generic->GetEngine(), generic, 1);
    bool ok = false;
    if (ctx != nullptr) {
        const std::string text = spec.is_null() ? "{}" : spec.dump();
        ok = loader_ns::sao_plugins_ctx_set_overlay(ctx, surface.c_str(), text.c_str()) ==
             SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_clear_overlay(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string surface = arg_string(generic, 0);
    bool ok = false;
    if (ctx != nullptr && !surface.empty()) {
        ok = loader_ns::sao_plugins_ctx_clear_overlay(ctx, surface.c_str()) == SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_request_redraw(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string surface = arg_string(generic, 0);
    const std::string reason = arg_string(generic, 1);
    bool ok = false;
    if (ctx != nullptr) {
        ok = loader_ns::sao_plugins_ctx_request_redraw(
                 ctx, surface.empty() ? nullptr : surface.c_str(),
                 reason.empty() ? nullptr : reason.c_str()) == SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

int register_extension_impl(asIScriptGeneric* generic, const char* kind) {
    auto* ctx = context_of(generic);
    const std::string id = arg_string(generic, 0);
    const ordered_json meta = generic_arg_to_json(generic->GetEngine(), generic, 1);
    if (ctx == nullptr || id.empty())
        return 0;
    const std::string text = meta.is_null() ? "{}" : meta.dump();
    return loader_ns::sao_plugins_ctx_register_extension(ctx, kind, id.c_str(),
                                                         text.c_str(), nullptr,
                                                         nullptr) == SAO_OK
               ? 1
               : 0;
}

void ctx_register_parser_adapter(asIScriptGeneric* g) {
    g->SetReturnByte(static_cast<asBYTE>(register_extension_impl(g, "parser_adapter")));
}
void ctx_register_exporter(asIScriptGeneric* g) {
    g->SetReturnByte(static_cast<asBYTE>(register_extension_impl(g, "exporter")));
}
void ctx_register_formatter(asIScriptGeneric* g) {
    g->SetReturnByte(static_cast<asBYTE>(register_extension_impl(g, "formatter")));
}
void ctx_register_trigger_type(asIScriptGeneric* g) {
    g->SetReturnByte(static_cast<asBYTE>(register_extension_impl(g, "trigger_type")));
}
void ctx_register_report_view(asIScriptGeneric* g) {
    g->SetReturnByte(static_cast<asBYTE>(register_extension_impl(g, "report_view")));
}
void ctx_register_timer(asIScriptGeneric* g) {
    g->SetReturnByte(static_cast<asBYTE>(register_extension_impl(g, "timer")));
}

void ctx_register_menu_category(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string name = arg_string(generic, 0);
    const std::string icon = arg_string(generic, 1);
    asIScriptFunction* builder = arg_funcdef(generic->GetEngine(), generic, 2);
    const double priority = generic->GetArgDouble(3);
    bool ok = false;
    if (ctx != nullptr && !name.empty() && builder != nullptr) {
        if (const auto state = ctx_state_or_create(ctx, generic->GetEngine())) {
            as_ctx_callback* record = make_callback(state, builder, generic->GetEngine());
            if (record != nullptr) {
                const std::string slug = slugify(name);
                auto provider = register_menu_provider(
                    ctx, state, "angel-menu-" + slug, "menu-category-" + slug,
                    "plugin-menus-" +
                        (state->plugin != nullptr ? state->plugin->plugin_id : "as"),
                    name, icon, priority, record);
                if (provider) {
                    state->menus.push_back(std::move(provider));
                    ok = true;
                } else {
                    retire_callback(state, record);
                }
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_register_menu_surface(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string surface_id = arg_string(generic, 0);
    const ordered_json descriptor = generic_arg_to_json(generic->GetEngine(), generic, 1);
    const float priority = static_cast<float>(generic->GetArgDouble(2));
    bool ok = false;
    if (ctx != nullptr && !surface_id.empty()) {
        const std::string text = descriptor.is_null() ? "{}" : descriptor.dump();
        ok = loader_ns::sao_plugins_ctx_register_menu_surface(ctx, surface_id.c_str(),
                                                              text.c_str(), priority) ==
             SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_register_action_handler(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    asIScriptFunction* fn = arg_funcdef(generic->GetEngine(), generic, 0);
    bool ok = false;
    if (ctx != nullptr && fn != nullptr) {
        if (const auto state = ctx_state_or_create(ctx, generic->GetEngine())) {
            as_ctx_callback* record = make_callback(state, fn, generic->GetEngine());
            if (record != nullptr) {
                auto provider = std::make_shared<menu_provider>();
                provider->provider_id = "angel-actions";
                provider->ctx = ctx;
                provider->plugin = state->plugin;
                provider->panel_handler = record;
                if (register_action_only_provider(ctx, provider.get())) {
                    state->global_action_handler = record;
                    state->action_providers.push_back(std::move(provider));
                    ok = true;
                } else {
                    retire_callback(state, record);
                }
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_register_engine(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string name = arg_string(generic, 0);
    const int type_id = generic->GetArgTypeId(1);
    void* object = nullptr;
    if (type_id & asTYPEID_OBJHANDLE) {
        object = *static_cast<void**>(generic->GetAddressOfArg(1));
    } else if (type_id & asTYPEID_MASK_OBJECT) {
        object = generic->GetAddressOfArg(1);
    }
    bool ok = false;
    if (ctx != nullptr && !name.empty() && object != nullptr) {
        // keep a counted reference for registered objects that support it.
        const int base = type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
        if (asITypeInfo* type = generic->GetEngine()->GetTypeInfoById(base)) {
            if ((type->GetFlags() & asOBJ_REF) != 0 && !(type->GetFlags() & asOBJ_NOCOUNT))
                generic->GetEngine()->AddRefScriptObject(object, type);
        }
        if (loader_ns::sao_plugins_ctx_register_engine(ctx, name.c_str(), object) ==
            SAO_OK) {
            if (const auto state = ctx_state_or_create(ctx, generic->GetEngine())) {
                state->engines[name] = ctx_surface_state::engine_record{object, type_id};
            }
            ok = true;
        } else {
            if (asITypeInfo* type = generic->GetEngine()->GetTypeInfoById(base)) {
                if ((type->GetFlags() & asOBJ_REF) != 0 && !(type->GetFlags() & asOBJ_NOCOUNT))
                    generic->GetEngine()->ReleaseScriptObject(object, type);
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

bool get_engine_impl(asIScriptGeneric* generic, bool required) {
    auto* ctx = context_of(generic);
    const std::string name = arg_string(generic, 0);
    const int out_type = generic->GetArgTypeId(1);
    void* out_address = generic->GetAddressOfArg(1);
    if (ctx == nullptr || name.empty() || out_address == nullptr ||
        !(out_type & asTYPEID_OBJHANDLE))
        return false;
    if (loader_ns::sao_plugins_ctx_get_engine(ctx, name.c_str()) == nullptr) {
        if (required)
            set_active_exception("engine not registered");
        return false;
    }
    // only objects registered through this plugin's ctx resolve safely (we can
    // re-addref them for the caller's declared handle type).
    if (const auto state = ctx_state_for(ctx)) {
        const auto found = state->engines.find(name);
        if (found != state->engines.end()) {
            void** slot = static_cast<void**>(out_address);
            if (*slot != nullptr) {
                const int old_base =
                    out_type & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
                if (asITypeInfo* old_type =
                        generic->GetEngine()->GetTypeInfoById(old_base)) {
                    if ((old_type->GetFlags() & asOBJ_REF) != 0 &&
                        !(old_type->GetFlags() & asOBJ_NOCOUNT))
                        generic->GetEngine()->ReleaseScriptObject(*slot, old_type);
                }
            }
            *slot = found->second.object;
            const int base =
                found->second.type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
            if (asITypeInfo* type = generic->GetEngine()->GetTypeInfoById(base)) {
                if ((type->GetFlags() & asOBJ_REF) != 0 &&
                    !(type->GetFlags() & asOBJ_NOCOUNT))
                    generic->GetEngine()->AddRefScriptObject(found->second.object, type);
            }
            return true;
        }
    }
    if (required)
        set_active_exception("engine not registered");
    return false;
}

void ctx_get_engine_by_name(asIScriptGeneric* generic) {
    generic->SetReturnByte(get_engine_impl(generic, false) ? 1 : 0);
}

void ctx_require_engine(asIScriptGeneric* generic) {
    generic->SetReturnByte(get_engine_impl(generic, true) ? 1 : 0);
}

void ctx_register_data_source(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string source_id = arg_string(generic, 0);
    const ordered_json meta = generic_arg_to_json(generic->GetEngine(), generic, 1);
    asIScriptFunction* start_fn = arg_funcdef(generic->GetEngine(), generic, 2);
    asIScriptFunction* stop_fn = arg_funcdef(generic->GetEngine(), generic, 3);
    bool ok = false;
    if (ctx != nullptr && !source_id.empty()) {
        const std::string text = meta.is_null() ? "{}" : meta.dump();
        const auto state = ctx_state_or_create(ctx, generic->GetEngine());
        as_ctx_callback* start_record =
            state && start_fn != nullptr ? make_callback(state, start_fn, generic->GetEngine())
                                         : nullptr;
        as_ctx_callback* stop_record =
            state && stop_fn != nullptr ? make_callback(state, stop_fn, generic->GetEngine())
                                        : nullptr;
        if (state != nullptr && (start_record != nullptr || stop_record != nullptr)) {
            auto pair = std::make_unique<data_source_record>();
            pair->start = start_record;
            pair->stop = stop_record;
            ok = loader_ns::sao_plugins_ctx_register_data_source(
                     ctx, source_id.c_str(), text.c_str(),
                     start_record != nullptr ? &data_source_start_dispatch : nullptr,
                     stop_record != nullptr ? &data_source_stop_dispatch : nullptr,
                     pair.get()) == SAO_OK;
            if (ok) {
                state->data_source_records.push_back(std::move(pair));
            } else {
                if (start_record != nullptr)
                    retire_callback(state, start_record);
                if (stop_record != nullptr)
                    retire_callback(state, stop_record);
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

// ──────────────────────────────────────────────────────────────────────────
// hotkey + timer bindings
// ──────────────────────────────────────────────────────────────────────────

void ctx_register_hotkey(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string hotkey_id = arg_string(generic, 0);
    asIScriptFunction* fn = arg_funcdef(generic->GetEngine(), generic, 1);
    const std::string default_key = arg_string(generic, 2);
    const std::string label = arg_string(generic, 3);
    bool ok = false;
    if (ctx != nullptr && !hotkey_id.empty() && fn != nullptr) {
        if (const auto state = ctx_state_or_create(ctx, generic->GetEngine())) {
            as_ctx_callback* record = make_callback(state, fn, generic->GetEngine());
            if (record != nullptr) {
                if (loader_ns::sao_plugins_ctx_register_hotkey(
                        ctx, hotkey_id.c_str(),
                        default_key.empty() ? nullptr : default_key.c_str(),
                        label.empty() ? nullptr : label.c_str(), &hotkey_dispatch,
                        record) == SAO_OK) {
                    state->hotkey_records.emplace(hotkey_id, record);
                    ok = true;
                } else {
                    retire_callback(state, record);
                }
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_unregister_hotkey(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string hotkey_id = arg_string(generic, 0);
    bool ok = false;
    if (ctx != nullptr && !hotkey_id.empty()) {
        ok = loader_ns::sao_plugins_ctx_unregister_hotkey(ctx, hotkey_id.c_str()) ==
             SAO_OK;
        if (ok) {
            if (const auto state = ctx_state_for(ctx)) {
                const auto found = state->hotkey_records.find(hotkey_id);
                if (found != state->hotkey_records.end()) {
                    as_ctx_callback* record = found->second;
                    state->hotkey_records.erase(found);
                    retire_callback(state, record);
                }
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void set_timer_impl(asIScriptGeneric* generic, bool one_shot) {
    auto* ctx = context_of(generic);
    asIScriptFunction* fn = arg_funcdef(generic->GetEngine(), generic, 0);
    const double seconds = generic->GetArgDouble(1);
    as_string token;
    if (ctx != nullptr && fn != nullptr && seconds > 0.0) {
        if (const auto state = ctx_state_or_create(ctx, generic->GetEngine())) {
            as_ctx_callback* record = make_callback(state, fn, generic->GetEngine());
            if (record != nullptr) {
                char* raw_token = nullptr;
                const int32_t status =
                    one_shot
                        ? loader_ns::sao_plugins_ctx_set_timeout(ctx, &timer_dispatch,
                                                                 seconds, record, &raw_token)
                        : loader_ns::sao_plugins_ctx_set_interval(ctx, &timer_dispatch,
                                                                  seconds, record,
                                                                  &raw_token);
                if (status == SAO_OK && raw_token != nullptr) {
                    token = raw_token;
                    record->one_shot = one_shot;
                    record->timer_token = token;
                    state->timer_records.emplace(token, record);
                    loader_ns::sao_plugins_ctx_free_string(raw_token);
                } else {
                    if (raw_token != nullptr)
                        loader_ns::sao_plugins_ctx_free_string(raw_token);
                    retire_callback(state, record);
                }
            }
        }
    }
    new (generic->GetAddressOfReturnLocation()) as_string(std::move(token));
}

void ctx_set_interval(asIScriptGeneric* generic) {
    set_timer_impl(generic, false);
}

void ctx_set_timeout(asIScriptGeneric* generic) {
    set_timer_impl(generic, true);
}

void ctx_clear_timer(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string token = arg_string(generic, 0);
    bool ok = false;
    if (ctx != nullptr && !token.empty()) {
        ok = loader_ns::sao_plugins_ctx_clear_timer(ctx, token.c_str()) == SAO_OK;
        if (ok) {
            if (const auto state = ctx_state_for(ctx)) {
                const auto found = state->timer_records.find(token);
                if (found != state->timer_records.end()) {
                    as_ctx_callback* record = found->second;
                    state->timer_records.erase(found);
                    retire_callback(state, record);
                }
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

// ──────────────────────────────────────────────────────────────────────────
// notify / io dialogs / compositor / deps
// ──────────────────────────────────────────────────────────────────────────

void ctx_notify(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string title = arg_string(generic, 0);
    const std::string message = arg_string(generic, 1);
    const double duration = generic->GetArgDouble(2);
    const std::string kind = arg_string(generic, 3);
    bool ok = false;
    if (ctx != nullptr) {
        ok = loader_ns::sao_plugins_ctx_notify(
                 ctx, title.c_str(), message.c_str(), duration,
                 kind.empty() ? "plugin" : kind.c_str()) == SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_dismiss_notify(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    generic->SetReturnByte(ctx != nullptr &&
                                   loader_ns::sao_plugins_ctx_dismiss_notify(ctx) == SAO_OK
                               ? 1
                               : 0);
}

void ctx_toast(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string message = arg_string(generic, 0);
    generic->SetReturnByte(ctx != nullptr &&
                                   loader_ns::sao_plugins_ctx_toast(ctx, message.c_str()) ==
                                       SAO_OK
                               ? 1
                               : 0);
}

void ctx_open_file(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    asIScriptEngine* engine = generic->GetEngine();
    std::string filters;
    const int filter_type = generic->GetArgTypeId(0);
    if (type_is_named(engine, filter_type, "string")) {
        filters = *static_cast<const as_string*>(generic->GetAddressOfArg(0));
    } else {
        const ordered_json value = generic_arg_to_json(engine, generic, 0);
        if (!value.is_null())
            filters = value.is_string() ? value.get<std::string>() : value.dump();
    }
    const std::string title = arg_string(generic, 1);
    const std::string initial_dir = arg_string(generic, 2);
    const auto hwnd = static_cast<intptr_t>(generic->GetArgQWord(3));
    as_string result;
    if (ctx != nullptr) {
        const std::wstring dir_wide = wide_from_utf8(initial_dir.c_str());
        wchar_t* selected = nullptr;
        if (loader_ns::sao_plugins_ctx_open_file(
                ctx, filters.empty() ? nullptr : filters.c_str(),
                title.empty() ? nullptr : title.c_str(),
                dir_wide.empty() ? nullptr : dir_wide.c_str(), hwnd, &selected) == SAO_OK &&
            selected != nullptr) {
            result = utf8_from_wide(selected);
            loader_ns::sao_plugins_ctx_free_wstring(selected);
        }
    }
    new (generic->GetAddressOfReturnLocation()) as_string(std::move(result));
}

void ctx_open_window(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string panel_id = arg_string(generic, 0);
    const auto width = generic->GetArgDWord(1);
    const auto height = generic->GetArgDWord(2);
    bool ok = false;
    if (ctx != nullptr && !panel_id.empty()) {
        ok = loader_ns::sao_plugins_ctx_open_window(ctx, panel_id.c_str(), width,
                                                    height) == SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_create_compositor_layer(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string name = arg_string(generic, 0);
    bool ok = false;
    if (ctx != nullptr && !name.empty()) {
        ok = loader_ns::sao_plugins_ctx_create_compositor_layer(
                 ctx, name.c_str(), generic->GetArgDWord(1), generic->GetArgDWord(2),
                 generic->GetArgDWord(3), generic->GetArgDWord(4),
                 generic->GetArgDWord(5), generic->GetArgByte(6) != 0,
                 generic->GetArgByte(7) != 0, generic->GetArgDWord(8)) == SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_upload_compositor_frame(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string name = arg_string(generic, 0);
    const auto width = generic->GetArgDWord(2);
    const auto height = generic->GetArgDWord(3);
    bool ok = false;
    if (ctx != nullptr && !name.empty()) {
        if (const CScriptArray* bytes =
                *static_cast<const CScriptArray* const*>(generic->GetAddressOfArg(1));
            bytes != nullptr && bytes->GetElementTypeId() == asTYPEID_UINT8 &&
            bytes->GetSize() > 0) {
            ok = loader_ns::sao_plugins_ctx_upload_compositor_frame(
                     ctx, name.c_str(), static_cast<const uint8_t*>(bytes->At(0)),
                     bytes->GetSize(), width, height) == SAO_OK;
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_set_compositor_layer_position(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string name = arg_string(generic, 0);
    bool ok = false;
    if (ctx != nullptr && !name.empty()) {
        ok = loader_ns::sao_plugins_ctx_set_compositor_layer_position(
                 ctx, name.c_str(), generic->GetArgDWord(1), generic->GetArgDWord(2)) ==
             SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_set_compositor_layer_visible(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string name = arg_string(generic, 0);
    bool ok = false;
    if (ctx != nullptr && !name.empty()) {
        ok = loader_ns::sao_plugins_ctx_set_compositor_layer_visible(
                 ctx, name.c_str(), generic->GetArgByte(1) != 0) == SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_destroy_compositor_layer(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string name = arg_string(generic, 0);
    bool ok = false;
    if (ctx != nullptr && !name.empty()) {
        ok = loader_ns::sao_plugins_ctx_destroy_compositor_layer(ctx, name.c_str()) ==
             SAO_OK;
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_set_compositor_layer_input(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string name = arg_string(generic, 0);
    bool ok = false;
    if (ctx != nullptr && !name.empty()) {
        if (const auto state = ctx_state_or_create(ctx, generic->GetEngine())) {
            asIScriptEngine* engine = generic->GetEngine();
            auto record = std::make_unique<compositor_input_record>();
            record->cursor_pos = make_callback(
                state, arg_funcdef(engine, generic, 1), engine);
            record->mouse_button = make_callback(
                state, arg_funcdef(engine, generic, 2), engine);
            record->cursor_leave = make_callback(
                state, arg_funcdef(engine, generic, 3), engine);
            record->scroll = make_callback(state, arg_funcdef(engine, generic, 4), engine);
            ok = loader_ns::sao_plugins_ctx_set_compositor_layer_input(
                     ctx, name.c_str(),
                     record->cursor_pos != nullptr ? &compositor_pos_dispatch : nullptr,
                     record->mouse_button != nullptr ? &compositor_button_dispatch
                                                     : nullptr,
                     record->cursor_leave != nullptr ? &compositor_leave_dispatch
                                                     : nullptr,
                     record->scroll != nullptr ? &compositor_scroll_dispatch : nullptr,
                     record.get()) == SAO_OK;
            if (ok) {
                state->input_records.push_back(std::move(record));
            } else {
                if (record->cursor_pos != nullptr)
                    retire_callback(state, record->cursor_pos);
                if (record->mouse_button != nullptr)
                    retire_callback(state, record->mouse_button);
                if (record->cursor_leave != nullptr)
                    retire_callback(state, record->cursor_leave);
                if (record->scroll != nullptr)
                    retire_callback(state, record->scroll);
            }
        }
    }
    generic->SetReturnByte(ok ? 1 : 0);
}

void ctx_ensure_requirements(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const bool install = generic->GetArgByte(0) != 0;
    void* output = nullptr;
    if (ctx != nullptr) {
        char* raw = nullptr;
        if (loader_ns::sao_plugins_ctx_ensure_requirements(ctx, install, &raw) ==
                SAO_OK &&
            raw != nullptr) {
            const ordered_json parsed = ordered_json::parse(raw, nullptr, false);
            output = json_ref_from_ordered(
                generic->GetEngine(),
                parsed.is_discarded() ? ordered_json(nullptr) : parsed);
            loader_ns::sao_plugins_ctx_free_string(raw);
        } else {
            output = json_ref_from_ordered(generic->GetEngine(), ordered_json(nullptr));
        }
    }
    *static_cast<void**>(generic->GetAddressOfReturnLocation()) = output;
}

void ctx_load_local(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string rel = arg_string(generic, 0);
    local_module_handle* output = nullptr;
    if (ctx != nullptr && !rel.empty()) {
        const wchar_t* root_wide = loader_ns::sao_plugins_ctx_path(ctx);
        const std::string plugin_id = loader_ns::sao_plugins_ctx_plugin_id(ctx) != nullptr
                                          ? loader_ns::sao_plugins_ctx_plugin_id(ctx)
                                          : "";
        sc::load_local_result kind = sc::load_local_result::missing;
        std::shared_ptr<sc::script_module> module;
        std::wstring abs_path;
        std::string diag;
        if (root_wide != nullptr &&
            sc::runtime_bridge_load_local(ctx, plugin_id.c_str(), root_wide, rel.c_str(),
                                          &kind, &module, &abs_path, &diag) == SAO_OK) {
            if (kind == sc::load_local_result::module && module != nullptr) {
                output = new local_module_handle();
                output->module = std::move(module);
            } else if (kind == sc::load_local_result::path_only) {
                // path form belongs to load_local_path; module stays null.
            }
            if (kind != sc::load_local_result::module && !diag.empty())
                loader_ns::sao_plugins_ctx_log(ctx, diag.c_str());
        } else if (!diag.empty()) {
            loader_ns::sao_plugins_ctx_log(ctx, diag.c_str());
        }
    }
    *static_cast<local_module_handle**>(generic->GetAddressOfReturnLocation()) = output;
}

void ctx_load_local_path(asIScriptGeneric* generic) {
    auto* ctx = context_of(generic);
    const std::string rel = arg_string(generic, 0);
    as_string result;
    if (ctx != nullptr && !rel.empty()) {
        const wchar_t* root_wide = loader_ns::sao_plugins_ctx_path(ctx);
        const std::string plugin_id = loader_ns::sao_plugins_ctx_plugin_id(ctx) != nullptr
                                          ? loader_ns::sao_plugins_ctx_plugin_id(ctx)
                                          : "";
        sc::load_local_result kind = sc::load_local_result::missing;
        std::shared_ptr<sc::script_module> module;
        std::wstring abs_path;
        std::string diag;
        if (root_wide != nullptr &&
            sc::runtime_bridge_load_local(ctx, plugin_id.c_str(), root_wide, rel.c_str(),
                                          &kind, &module, &abs_path, &diag) == SAO_OK) {
            if (kind == sc::load_local_result::path_only)
                result = utf8_from_wide(abs_path.c_str());
            else if (!diag.empty())
                loader_ns::sao_plugins_ctx_log(ctx, diag.c_str());
        }
    }
    new (generic->GetAddressOfReturnLocation()) as_string(std::move(result));
}

// ──────────────────────────────────────────────────────────────────────────
// registration
// ──────────────────────────────────────────────────────────────────────────

int register_object_method(asIScriptEngine* engine, const char* type, const char* decl,
                           asSFuncPtr fn, bool* failed) {
    const int result = engine->RegisterObjectMethod(type, decl, fn, asCALL_GENERIC);
    if (!registration_ok(result) && failed != nullptr)
        *failed = true;
    return result;
}

// Single generic trampoline for every bound method — the registered name
// selects the handler; the outer try/catch keeps C++ exceptions out of the
// engine's generic dispatch (matching the per-fn guards elsewhere).
using ctx_handler_fn = void (*)(asIScriptGeneric*);

const std::pair<const char*, ctx_handler_fn> kCtxHandlers[] = {
    // PluginContext properties
    {"get_path", &ctx_get_path},
    {"get_assets_path", &ctx_get_assets_path},
    {"get_web_path", &ctx_get_web_path},
    {"get_event_bus", &ctx_get_event_bus},
    {"get_ui", &ctx_get_ui},
    {"get_mem", &ctx_get_mem},
    {"get_owner", &ctx_get_owner},
    // events
    {"subscribe", &ctx_subscribe},
    {"subscribe_once", &ctx_subscribe_once},
    {"on", &ctx_on},
    {"on_damage", &ctx_on_damage},
    {"on_heal", &ctx_on_heal},
    {"on_skill", &ctx_on_skill},
    {"on_boss", &ctx_on_boss},
    {"on_snapshot", &ctx_on_snapshot},
    {"on_encounter_finalized", &ctx_on_encounter_finalized},
    {"unsubscribe", &ctx_unsubscribe},
    {"emit", &ctx_emit},
    {"get_snapshot", &ctx_get_snapshot},
    {"snapshot_value", &ctx_snapshot_value},
    {"recent_events", &ctx_recent_events},
    // settings
    {"get_setting", &ctx_get_setting},
    {"setting", &ctx_setting},
    {"set_setting", &ctx_set_setting},
    {"set_defaults", &ctx_set_defaults},
    // registration surface
    {"register_ui_panel", &ctx_register_ui_panel},
    {"register_render_hook", &ctx_register_render_hook},
    {"unregister_render_hook", &ctx_unregister_render_hook},
    {"set_overlay", &ctx_set_overlay},
    {"clear_overlay", &ctx_clear_overlay},
    {"request_redraw", &ctx_request_redraw},
    {"register_parser_adapter", &ctx_register_parser_adapter},
    {"register_exporter", &ctx_register_exporter},
    {"register_formatter", &ctx_register_formatter},
    {"register_trigger_type", &ctx_register_trigger_type},
    {"register_report_view", &ctx_register_report_view},
    {"register_timer", &ctx_register_timer},
    {"register_menu_category", &ctx_register_menu_category},
    {"register_menu_surface", &ctx_register_menu_surface},
    {"register_action_handler", &ctx_register_action_handler},
    {"register_engine", &ctx_register_engine},
    {"require_engine", &ctx_require_engine},
    {"register_data_source", &ctx_register_data_source},
    {"register_hotkey", &ctx_register_hotkey},
    {"unregister_hotkey", &ctx_unregister_hotkey},
    // timers
    {"set_interval", &ctx_set_interval},
    {"set_timeout", &ctx_set_timeout},
    {"clear_timer", &ctx_clear_timer},
    // notify / io / compositor / deps
    {"notify", &ctx_notify},
    {"dismiss_notify", &ctx_dismiss_notify},
    {"toast", &ctx_toast},
    {"open_file", &ctx_open_file},
    {"open_window", &ctx_open_window},
    {"create_compositor_layer", &ctx_create_compositor_layer},
    {"upload_compositor_frame", &ctx_upload_compositor_frame},
    {"set_compositor_layer_position", &ctx_set_compositor_layer_position},
    {"set_compositor_layer_visible", &ctx_set_compositor_layer_visible},
    {"set_compositor_layer_input", &ctx_set_compositor_layer_input},
    {"destroy_compositor_layer", &ctx_destroy_compositor_layer},
    {"ensure_requirements", &ctx_ensure_requirements},
    {"load_local", &ctx_load_local},
    {"load_local_path", &ctx_load_local_path},
    // MemAccess (same trampoline — names are unique across types)
    {"attach", &mem_attach},
    {"detach", &mem_detach},
    {"module_base", &mem_module_base},
    {"read_u32", &mem_read_u32},
    {"read_u64", &mem_read_u64},
    {"read", &mem_read},
    {"read_ptr_chain", &mem_read_ptr_chain},
    {"enumerate_modules", &mem_enumerate_modules},
    // LocalModule
    {"module_id", &local_module_id},
    {"member_names", &local_module_names},
    {"get", &local_module_get},
    {"call", &local_module_call},
};

void ctx_dispatch(asIScriptGeneric* generic) {
    try {
        asIScriptFunction* function = generic->GetFunction();
        const char* name = function != nullptr ? function->GetName() : nullptr;
        if (name == nullptr)
            return;
        // "get_engine" is both the engine property (0 args -> self handle) and
        // the registry lookup (2 args -> ?&out).
        if (std::strcmp(name, "get_engine") == 0) {
            if (generic->GetArgCount() == 0) {
                ctx_get_engine_property(generic);
            } else {
                ctx_get_engine_by_name(generic);
            }
            return;
        }
        for (const auto& entry : kCtxHandlers) {
            if (std::strcmp(name, entry.first) == 0) {
                entry.second(generic);
                return;
            }
        }
    } catch (...) {
        set_active_exception("ctx call failed");
    }
}

const char* const kCtxSurfaceNames[] = {
    "plugin_id",           "path",
    "web_path",            "assets_path",
    "should_stop",         "engine",
    "event_bus",           "owner",
    "ui",                  "mem",
    "log",                 "log_info",
    "time",                "on",
    "subscribe",           "subscribe_once",
    "unsubscribe",         "on_damage",
    "on_heal",             "on_skill",
    "on_boss",             "on_snapshot",
    "on_encounter_finalized",
    "emit",                "get_snapshot",
    "snapshot_value",      "recent_events",
    "get_setting",         "setting",
    "set_setting",         "set_defaults",
    "register_parser_adapter",
    "register_exporter",   "register_formatter",
    "register_trigger_type",
    "register_report_view",
    "register_timer",      "register_ui_panel",
    "register_render_hook",
    "unregister_render_hook",
    "set_overlay",         "clear_overlay",
    "request_redraw",      "register_hotkey",
    "unregister_hotkey",   "register_menu_category",
    "register_menu_surface",
    "register_action_handler",
    "register_engine",     "register_data_source",
    "get_engine",          "require_engine",
    "set_interval",        "set_timeout",
    "clear_timer",         "notify",
    "dismiss_notify",      "toast",
    "open_file",           "open_window",
    "create_compositor_layer",
    "upload_compositor_frame",
    "set_compositor_layer_position",
    "set_compositor_layer_visible",
    "set_compositor_layer_input",
    "destroy_compositor_layer",
    "ensure_requirements", "load_local",
    "load_local_path",
    // ctx.ui.*
    "ui.panel",            "ui.section",
    "ui.card",             "ui.row",
    "ui.group",            "ui.text",
    "ui.title",            "ui.kv",
    "ui.bar",              "ui.slider",
    "ui.badge",            "ui.divider",
    "ui.spacer",           "ui.button",
    "ui.input",            "ui.table",
    "ui.canvas",           "ui.rgba_frame",
    "ui.rect",             "ui.oval",
    "ui.line",             "ui.ctext",
    // ctx.mem.*
    "mem.attach",          "mem.detach",
    "mem.module_base",     "mem.read_u32",
    "mem.read_u64",        "mem.read",
    "mem.read_ptr_chain",  "mem.enumerate_modules",
    nullptr,
};

} // namespace

// ── ctx surface teardown / registration entry points ──

void ctx_surface_teardown(void* bound_context) noexcept {
    auto* ctx = static_cast<loader_ns::plugin_context_t*>(bound_context);
    if (ctx == nullptr)
        return;
    std::shared_ptr<ctx_surface_state> state;
    {
        std::lock_guard lock(ctx_state_mutex());
        const auto found = ctx_states().find(ctx);
        if (found == ctx_states().end())
            return;
        state = found->second;
        ctx_states().erase(found);
    }
    try {
        for (auto& menu : state->menus)
            menu->closing = true;
        for (auto& provider : state->action_providers)
            provider->closing = true;
        for (auto& callback : state->callbacks)
            quiesce_ctx_callback(callback.get());
        {
            // engine channel records are owned by state->callbacks (quiesced
            // above); drop the channel index so late trampoline hits resolve
            // to nothing even if a provider thread raced past ctx_state_for.
            std::lock_guard lock(state->engine_channel_mutex);
            state->engine_channels.clear();
        }

        std::vector<const char*> provider_ids;
        for (auto& menu : state->menus)
            provider_ids.push_back(menu->provider_id.c_str());
        for (auto& provider : state->action_providers)
            provider_ids.push_back(provider->provider_id.c_str());
        if (!provider_ids.empty()) {
            (void)loader_ns::plugin_context_unregister_entity_providers(
                ctx, provider_ids.data(), provider_ids.size());
        }
        if (state->plugin != nullptr && state->plugin->engine != nullptr) {
            release_callback_functions(state, state->plugin->engine);
            if (state->owner != nullptr) {
                engine_execution_guard engine_lock;
                if (asITypeInfo* dict_type =
                        state->plugin->engine->GetTypeInfoByDecl("dictionary")) {
                    state->plugin->engine->ReleaseScriptObject(state->owner, dict_type);
                }
                state->owner = nullptr;
            }
            // release engine-registry AddRefs owned by this ctx.
            for (auto& [name, record] : state->engines) {
                const int base = record.type_id &
                                 ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
                if (asITypeInfo* type =
                        state->plugin->engine->GetTypeInfoById(base)) {
                    if ((type->GetFlags() & asOBJ_REF) != 0 &&
                        !(type->GetFlags() & asOBJ_NOCOUNT) && record.object != nullptr) {
                        state->plugin->engine->ReleaseScriptObject(record.object, type);
                    }
                }
            }
        }
        std::lock_guard grave(graveyard_mutex());
        for (auto& callback : state->callbacks)
            callback_graveyard().push_back(std::move(callback));
        for (auto& record : state->input_records)
            input_graveyard().push_back(std::move(record));
        for (auto& record : state->data_source_records)
            data_source_graveyard().push_back(std::move(record));
        for (auto& record : state->panel_records)
            panel_graveyard().push_back(std::move(record));
    } catch (...) {
    }
}

int32_t register_ctx_surface_bindings(asIScriptEngine* engine) noexcept {
    if (engine == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    engine_execution_guard engine_lock;
    try {
        register_as_provider_once();

        const bool has_dictionary = engine->GetTypeInfoByName("dictionary") != nullptr;
        const bool has_json = engine->GetTypeInfoByName("json") != nullptr;
        const bool has_array = engine->GetTypeInfoByDecl("array<int>") != nullptr;
        bool failed = false;

        // Channel callback funcdef for ctx.engine_on — signature consumed by
        // engine_channel_trampoline as cb(payload, channel).
        if (has_dictionary && engine->GetTypeInfoByName("engine_channel_cb") == nullptr) {
            failed = engine->RegisterFuncdef(
                         "void engine_channel_cb(dictionary@ payload, string channel)") < 0 ||
                     failed;
        }

        if (engine->GetTypeInfoByName("UiBuilder") == nullptr) {
            (void)engine->RegisterObjectType("UiBuilder", 0, asOBJ_REF | asOBJ_NOCOUNT);
            struct ui_method_decl {
                const char* name;
                int args;
            };
            static const ui_method_decl kUiMethods[] = {
                {"panel", 2},       {"section", 3},   {"card", 2},  {"row", 2},
                {"group", 1},       {"text", 3},      {"title", 1}, {"kv", 3},
                {"bar", 4},         {"slider", 7},    {"badge", 2}, {"divider", 0},
                {"spacer", 1},      {"button", 5},    {"input", 5}, {"table", 4},
                {"canvas", 9},      {"rgba_frame", 11},
                {"rect", 7},        {"oval", 7},      {"line", 6},  {"ctext", 7},
            };
            if (has_dictionary) {
                for (const auto& method : kUiMethods) {
                    std::string decl = "dictionary@ " + std::string(method.name) + "(";
                    for (int index = 0; index < method.args; ++index) {
                        decl += index == 0 ? "?&in a0 = null" : "";
                        if (index > 0) {
                            decl += ", ?&in a";
                            decl += std::to_string(index);
                            decl += " = null";
                        }
                    }
                    decl += ") const";
                    if (method.args == 0)
                        decl = "dictionary@ " + std::string(method.name) + "() const";
                    register_object_method(engine, "UiBuilder", decl.c_str(),
                                           asFUNCTION(ui_builder_method), &failed);
                }
            }
        }

        if (engine->GetTypeInfoByName("MemAccess") == nullptr) {
            (void)engine->RegisterObjectType("MemAccess", 0, asOBJ_REF | asOBJ_NOCOUNT);
            register_object_method(engine, "MemAccess",
                                   "bool attach(uint32 pid, const string &in image_name = \"\")",
                                   asFUNCTION(ctx_dispatch), &failed);
            register_object_method(engine, "MemAccess", "void detach()",
                                   asFUNCTION(ctx_dispatch), &failed);
            register_object_method(engine, "MemAccess",
                                   "uint64 module_base(const string &in name)",
                                   asFUNCTION(ctx_dispatch), &failed);
            register_object_method(engine, "MemAccess", "uint read_u32(uint64 address)",
                                   asFUNCTION(ctx_dispatch), &failed);
            register_object_method(engine, "MemAccess", "uint64 read_u64(uint64 address)",
                                   asFUNCTION(ctx_dispatch), &failed);
            if (has_array) {
                register_object_method(
                    engine, "MemAccess", "array<uint8>@ read(uint64 address, uint len)",
                    asFUNCTION(ctx_dispatch), &failed);
                register_object_method(
                    engine, "MemAccess",
                    "uint64 read_ptr_chain(uint64 base, array<int>@ offsets)",
                    asFUNCTION(ctx_dispatch), &failed);
            }
            if (has_dictionary) {
                register_object_method(engine, "MemAccess",
                                       "dictionary@ enumerate_modules()",
                                       asFUNCTION(ctx_dispatch), &failed);
            }
        }

        if (engine->GetTypeInfoByName("LocalModule") == nullptr) {
            (void)engine->RegisterObjectType("LocalModule", 0, asOBJ_REF);
            (void)engine->RegisterObjectBehaviour("LocalModule", asBEHAVE_ADDREF,
                                                  "void f()",
                                                  asFUNCTION(local_module_add_ref),
                                                  asCALL_GENERIC);
            (void)engine->RegisterObjectBehaviour("LocalModule", asBEHAVE_RELEASE,
                                                  "void f()",
                                                  asFUNCTION(local_module_release),
                                                  asCALL_GENERIC);
            register_object_method(engine, "LocalModule", "string module_id() const",
                                   asFUNCTION(ctx_dispatch), &failed);
            if (has_array) {
                register_object_method(engine, "LocalModule",
                                       "array<string>@ member_names() const",
                                       asFUNCTION(ctx_dispatch), &failed);
            }
            if (has_json) {
                register_object_method(engine, "LocalModule",
                                       "json@ get(const string &in name) const",
                                       asFUNCTION(ctx_dispatch), &failed);
                register_object_method(engine, "LocalModule",
                                       "json@ call(const string &in name, ?&in args = null) const",
                                       asFUNCTION(ctx_dispatch), &failed);
            }
        }

        static const struct {
            const char* decl;
            asSFuncPtr fn;
            bool needs_dictionary;
            bool needs_json;
            bool needs_array;
        } kMethods[] = {
            {"string get_path() const property", asFUNCTION(ctx_dispatch), false, false,
             false},
            {"string get_assets_path() const property", asFUNCTION(ctx_dispatch),
             false, false, false},
            {"string get_web_path() const property", asFUNCTION(ctx_dispatch), false,
             false, false},
            {"PluginContext@ get_engine() const property",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"PluginContext@ get_event_bus() const property",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"UiBuilder@ get_ui() const property", asFUNCTION(ctx_dispatch), false, false,
             false},
            {"MemAccess@ get_mem() const property", asFUNCTION(ctx_dispatch), false, false,
             false},
            {"dictionary@ get_owner() const property", asFUNCTION(ctx_dispatch), true,
             false, false},
            {"int64 subscribe(const string &in topic, ?&in callback)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"int64 subscribe_once(const string &in topic, ?&in callback)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"int64 on(const string &in topic, ?&in callback)", asFUNCTION(ctx_dispatch),
             false, false, false},
            {"int64 on_damage(?&in callback)", asFUNCTION(ctx_dispatch), false, false,
             false},
            {"int64 on_heal(?&in callback)", asFUNCTION(ctx_dispatch), false, false,
             false},
            {"int64 on_skill(?&in callback)", asFUNCTION(ctx_dispatch), false, false,
             false},
            {"int64 on_boss(?&in callback)", asFUNCTION(ctx_dispatch), false, false,
             false},
            {"int64 on_snapshot(?&in callback)", asFUNCTION(ctx_dispatch), false,
             false, false},
            {"int64 on_encounter_finalized(?&in callback)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool unsubscribe(int64 token)", asFUNCTION(ctx_dispatch), false, false,
             false},
            {"bool emit(const string &in topic, ?&in payload = null)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"dictionary@ get_snapshot()", asFUNCTION(ctx_dispatch), true, false,
             false},
            {"json@ snapshot_value(const string &in path, ?&in default_value = null)",
             asFUNCTION(ctx_dispatch), false, true, false},
            {"array<dictionary>@ recent_events(uint limit = 20, const string &in topic = \"\")",
             asFUNCTION(ctx_dispatch), true, false, true},
            {"json@ get_setting(const string &in key, ?&in default_value = null)",
             asFUNCTION(ctx_dispatch), false, true, false},
            {"json@ setting(const string &in key, ?&in default_value = null)",
             asFUNCTION(ctx_dispatch), false, true, false},
            {"bool set_setting(const string &in key, ?&in value)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool set_defaults(?&in defaults)", asFUNCTION(ctx_dispatch), false, false,
             false},
            {"bool register_ui_panel(const string &in panel_id, ?&in metadata = null, "
             "?&in render = null, ?&in on_action = null)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"int64 register_render_hook(const string &in surface, ?&in callback, "
             "float priority = 0.0f)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool unregister_render_hook(int64 token)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool set_overlay(const string &in surface, ?&in spec)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool clear_overlay(const string &in surface = \"\")",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool request_redraw(const string &in surface = \"\", const string &in reason = \"\")",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_parser_adapter(const string &in id, ?&in metadata = null)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_exporter(const string &in id, ?&in metadata = null)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_formatter(const string &in id, ?&in metadata = null)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_trigger_type(const string &in id, ?&in metadata = null)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_report_view(const string &in id, ?&in metadata = null)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_timer(const string &in id, ?&in metadata = null)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_menu_category(const string &in name, const string &in icon, "
             "?&in builder, double priority = 0.0)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_menu_surface(const string &in surface_id, ?&in descriptor = null, "
             "double priority = 0.0)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_action_handler(?&in handler)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_engine(const string &in name, ?&in engine)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool get_engine(const string &in name, ?&out engine)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool require_engine(const string &in name, ?&out engine)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_data_source(const string &in source_id, ?&in metadata = null, "
             "?&in start = null, ?&in stop = null)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool register_hotkey(const string &in hotkey_id, ?&in callback, "
             "const string &in default_key = \"\", const string &in label = \"\")",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool unregister_hotkey(const string &in hotkey_id)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"string set_interval(?&in callback, double seconds)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"string set_timeout(?&in callback, double seconds)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool clear_timer(const string &in token)", asFUNCTION(ctx_dispatch),
             false, false, false},
            {"bool notify(const string &in title, const string &in message, "
             "double duration_s = 60.0, const string &in kind = \"plugin\")",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool dismiss_notify()", asFUNCTION(ctx_dispatch), false, false, false},
            {"bool toast(const string &in message)", asFUNCTION(ctx_dispatch), false, false,
             false},
            {"string open_file(?&in filters = null, const string &in title = \"\", "
             "const string &in initial_dir = \"\", int64 hwnd = 0)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool open_window(const string &in panel_id, uint width, uint height)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool create_compositor_layer(const string &in name, uint width, uint height, "
             "int x = 0, int y = 0, int z = 140, bool click_through = true, "
             "bool high_fps = false, uint target_fps = 0)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool upload_compositor_frame(const string &in name, array<uint8>@ bytes, "
             "uint width, uint height)",
             asFUNCTION(ctx_dispatch), false, false, true},
            {"bool set_compositor_layer_position(const string &in name, int x, int y)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool set_compositor_layer_visible(const string &in name, bool visible)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool set_compositor_layer_input(const string &in name, ?&in cursor_pos = null, "
             "?&in mouse_button = null, ?&in cursor_leave = null, "
             "?&in scroll = null)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"bool destroy_compositor_layer(const string &in name)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"json@ ensure_requirements(bool install = true)",
             asFUNCTION(ctx_dispatch), false, true, false},
            {"LocalModule@ load_local(const string &in relative_path)",
             asFUNCTION(ctx_dispatch), false, false, false},
            {"string load_local_path(const string &in relative_path)",
             asFUNCTION(ctx_dispatch), false, false, false},
            // reflective engine surface — full SDK catalog via
            // binding_engine dispatch (named wrappers live in the
            // sao_engine preamble section generated per module).
            {"string engine_call(const string &in name, const string &in args_json)",
             asFUNCTION(ctx_engine_call), false, false, false},
            {"string engine_list()", asFUNCTION(ctx_engine_list), false, false,
             false},
            {"bool engine_on(const string &in channel, engine_channel_cb@ callback)",
             asFUNCTION(ctx_engine_on), false, false, false},
            {"bool engine_off(const string &in channel)",
             asFUNCTION(ctx_engine_off), false, false, false},
        };

        for (const auto& method : kMethods) {
            if ((method.needs_dictionary && !has_dictionary) ||
                (method.needs_json && !has_json) || (method.needs_array && !has_array))
                continue;
            register_object_method(engine, "PluginContext", method.decl, method.fn,
                                   &failed);
        }

        sc::ctx_surface_note_all(loader_ns::engine_kind::angelscript,
                                 kCtxSurfaceNames);
        return failed ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

#else

void ctx_surface_teardown(void*) noexcept {}

int32_t register_ctx_surface_bindings(asIScriptEngine*) noexcept {
    return SAO_OK;
}

#endif

} // namespace sao::plugins::angel_host
