#include "as_generic_bindings_internal.h"

#include "as_plugin_internal.h"

#include "sao_plugins/sao_status.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {

#if defined(SAO_HAS_ANGELSCRIPT)

namespace {

using ordered_json = nlohmann::ordered_json;
using as_string = as_string_value;

class as_string_factory final : public asIStringFactory {
public:
    const void* GetStringConstant(const char* data, asUINT length) override {
        try {
            auto value = std::make_unique<as_string>();
            if (data != nullptr && length > 0) value->data.assign(data, length);
            return value.release();
        } catch (...) {
            return nullptr;
        }
    }

    int ReleaseStringConstant(const void* value) override {
        try {
            delete static_cast<const as_string*>(value);
            return 0;
        } catch (...) {
            return asERROR;
        }
    }

    int GetRawStringData(const void* value, char* data,
                         asUINT* length) const override {
        try {
            const auto* string = static_cast<const as_string*>(value);
            if (string == nullptr) return asINVALID_ARG;
            if (length != nullptr) {
                *length = static_cast<asUINT>(string->data.size());
            }
            if (data != nullptr && !string->data.empty()) {
                std::memcpy(data, string->data.data(), string->data.size());
            }
            return asSUCCESS;
        } catch (...) {
            return asERROR;
        }
    }
};

as_string_factory g_string_factory;

bool registration_ok(int result) {
    return result >= 0 || result == asALREADY_REGISTERED ||
           result == asNAME_TAKEN;
}

void set_active_exception(const char* message) noexcept {
    if (auto* context = asGetActiveContext(); context != nullptr) {
        (void)context->SetException(message);
    }
}

void string_construct(asIScriptGeneric* generic) {
    try {
        new (generic->GetObject()) as_string();
    } catch (...) {
        set_active_exception("string construction failed");
    }
}

void string_copy_construct(asIScriptGeneric* generic) {
    try {
        const auto* source =
            static_cast<const as_string*>(generic->GetArgAddress(0));
        new (generic->GetObject()) as_string(
            source == nullptr ? as_string{} : *source);
    } catch (...) {
        set_active_exception("string copy construction failed");
    }
}

void string_destruct(asIScriptGeneric* generic) {
    try {
        static_cast<as_string*>(generic->GetObject())->~as_string();
    } catch (...) {
    }
}

void string_assign(asIScriptGeneric* generic) {
    try {
        auto* target = static_cast<as_string*>(generic->GetObject());
        const auto* source =
            static_cast<const as_string*>(generic->GetArgAddress(0));
        if (target != nullptr && source != nullptr) {
            target->data = source->data;
        }
        generic->SetReturnAddress(target);
    } catch (...) {
        set_active_exception("string assignment failed");
    }
}

void string_add_assign(asIScriptGeneric* generic) {
    try {
        auto* target = static_cast<as_string*>(generic->GetObject());
        const auto* source =
            static_cast<const as_string*>(generic->GetArgAddress(0));
        if (target != nullptr && source != nullptr) {
            target->data += source->data;
        }
        generic->SetReturnAddress(target);
    } catch (...) {
        set_active_exception("string append assignment failed");
    }
}

void string_equals(asIScriptGeneric* generic) {
    try {
        const auto* left = static_cast<const as_string*>(generic->GetObject());
        const auto* right =
            static_cast<const as_string*>(generic->GetArgAddress(0));
        generic->SetReturnByte(left != nullptr && right != nullptr &&
                                       left->data == right->data
                                   ? 1
                                   : 0);
    } catch (...) {
        generic->SetReturnByte(0);
    }
}

void string_add(asIScriptGeneric* generic) {
    try {
        const auto* left = static_cast<const as_string*>(generic->GetObject());
        const auto* right =
            static_cast<const as_string*>(generic->GetArgAddress(0));
        as_string result;
        if (left != nullptr) result.data = left->data;
        if (right != nullptr) result.data += right->data;
        new (generic->GetAddressOfReturnLocation()) as_string(std::move(result));
    } catch (...) {
        set_active_exception("string return construction failed");
    }
}

as_plugin_s* active_plugin() {
    auto* context = asGetActiveContext();
    return context == nullptr
               ? nullptr
               : static_cast<as_plugin_s*>(context->GetUserData(
                     static_cast<asPWORD>(kPluginContextUserDataSlot)));
}

void copy_counter_text(char* output, size_t capacity,
                       const std::string& value) {
    if (output == nullptr || capacity == 0) return;
    const size_t length = std::min(capacity - 1, value.size());
    if (length > 0) std::memcpy(output, value.data(), length);
    output[length] = '\0';
}

void global_log_info(asIScriptGeneric* generic) {
    try {
        auto* plugin = active_plugin();
        const auto* message =
            static_cast<const as_string*>(generic->GetArgAddress(0));
        if (plugin == nullptr) return;
        ++plugin->counters.log_info_calls;
        copy_counter_text(plugin->counters.last_log_utf8,
                          sizeof(plugin->counters.last_log_utf8),
                          message == nullptr ? std::string{} : message->data);
    } catch (...) {
    }
}

void global_register_ui_panel(asIScriptGeneric* generic) {
    try {
        auto* plugin = active_plugin();
        const auto* panel =
            static_cast<const as_string*>(generic->GetArgAddress(0));
        if (plugin == nullptr) return;
        ++plugin->counters.register_ui_panel_calls;
        copy_counter_text(plugin->counters.last_panel_id_utf8,
                          sizeof(plugin->counters.last_panel_id_utf8),
                          panel == nullptr ? std::string{} : panel->data);
    } catch (...) {
    }
}

void global_register_hotkey(asIScriptGeneric* generic) {
    try {
        auto* plugin = active_plugin();
        const auto* hotkey =
            static_cast<const as_string*>(generic->GetArgAddress(0));
        const auto* key =
            static_cast<const as_string*>(generic->GetArgAddress(1));
        if (plugin == nullptr) return;
        ++plugin->counters.register_hotkey_calls;
        copy_counter_text(plugin->counters.last_hotkey_id_utf8,
                          sizeof(plugin->counters.last_hotkey_id_utf8),
                          hotkey == nullptr ? std::string{} : hotkey->data);
        copy_counter_text(plugin->counters.last_hotkey_key_utf8,
                          sizeof(plugin->counters.last_hotkey_key_utf8),
                          key == nullptr ? std::string{} : key->data);
    } catch (...) {
    }
}

void context_log(asIScriptGeneric* generic) {
    try {
        auto* context =
            static_cast<loader::plugin_context_t*>(generic->GetObject());
        const auto* message =
            static_cast<const as_string*>(generic->GetArgAddress(0));
        if (context != nullptr && message != nullptr) {
            loader::sao_plugins_ctx_log(context, message->data.c_str());
        }
    } catch (...) {
    }
}

void context_plugin_id(asIScriptGeneric* generic) {
    try {
        auto* context =
            static_cast<loader::plugin_context_t*>(generic->GetObject());
        as_string result;
        if (context != nullptr) {
            result.data = loader::sao_plugins_ctx_plugin_id(context);
        }
        new (generic->GetAddressOfReturnLocation()) as_string(std::move(result));
    } catch (...) {
        set_active_exception("PluginContext::plugin_id return failed");
    }
}

void context_should_stop(asIScriptGeneric* generic) {
    try {
        auto* context =
            static_cast<loader::plugin_context_t*>(generic->GetObject());
        generic->SetReturnByte(
            loader::sao_plugins_ctx_should_stop(context) ? 1 : 0);
    } catch (...) {
        generic->SetReturnByte(1);
    }
}

bool is_string_type(asIScriptEngine* engine, int type_id) {
    auto* type = engine->GetTypeInfoById(type_id);
    return type != nullptr && std::strcmp(type->GetName(), "string") == 0;
}

template <typename Integer>
bool read_integer(const ordered_json& value, Integer& output) {
    static_assert(std::is_integral_v<Integer>);
    if constexpr (std::is_signed_v<Integer>) {
        if (value.is_number_unsigned()) {
            const auto raw = value.get<uint64_t>();
            if (raw > static_cast<uint64_t>(
                          std::numeric_limits<Integer>::max())) {
                return false;
            }
            output = static_cast<Integer>(raw);
            return true;
        }
        if (!value.is_number_integer()) return false;
        const auto raw = value.get<int64_t>();
        if (raw < static_cast<int64_t>(std::numeric_limits<Integer>::min()) ||
            raw > static_cast<int64_t>(std::numeric_limits<Integer>::max())) {
            return false;
        }
        output = static_cast<Integer>(raw);
        return true;
    } else {
        if (!value.is_number_unsigned()) return false;
        const auto raw = value.get<uint64_t>();
        if (raw > static_cast<uint64_t>(
                      std::numeric_limits<Integer>::max())) {
            return false;
        }
        output = static_cast<Integer>(raw);
        return true;
    }
}

template <typename Floating>
bool read_floating(const ordered_json& value, Floating& output) {
    static_assert(std::is_floating_point_v<Floating>);
    if (!value.is_number()) return false;
    const double raw = value.get<double>();
    if (!std::isfinite(raw) ||
        raw < static_cast<double>(std::numeric_limits<Floating>::lowest()) ||
        raw > static_cast<double>(std::numeric_limits<Floating>::max())) {
        return false;
    }
    output = static_cast<Floating>(raw);
    return true;
}

int32_t set_arguments(asIScriptContext* context, asIScriptFunction* function,
                      const char* args_json_utf8,
                      std::vector<as_string>& string_arguments) {
    ordered_json arguments = ordered_json::array();
    if (args_json_utf8 != nullptr && args_json_utf8[0] != '\0') {
        arguments = ordered_json::parse(args_json_utf8, nullptr, false);
        if (arguments.is_discarded() || !arguments.is_array()) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }
    if (arguments.size() != function->GetParamCount()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    string_arguments.reserve(arguments.size());
    for (asUINT index = 0; index < function->GetParamCount(); ++index) {
        int type_id = 0;
        if (function->GetParam(index, &type_id) < 0) {
            return SAO_ERR_OS_CALL_FAILED;
        }
        const auto& value = arguments[index];
        int result = asINVALID_ARG;
        switch (type_id) {
        case asTYPEID_BOOL:
            if (!value.is_boolean()) return SAO_ERR_INVALID_ARGUMENT;
            result = context->SetArgByte(index, value.get<bool>() ? 1 : 0);
            break;
        case asTYPEID_INT8: {
            int8_t argument = 0;
            if (!read_integer(value, argument)) return SAO_ERR_INVALID_ARGUMENT;
            result = context->SetArgByte(index, static_cast<asBYTE>(argument));
            break;
        }
        case asTYPEID_UINT8: {
            uint8_t argument = 0;
            if (!read_integer(value, argument)) return SAO_ERR_INVALID_ARGUMENT;
            result = context->SetArgByte(index, argument);
            break;
        }
        case asTYPEID_INT16: {
            int16_t argument = 0;
            if (!read_integer(value, argument)) return SAO_ERR_INVALID_ARGUMENT;
            result = context->SetArgWord(index, static_cast<asWORD>(argument));
            break;
        }
        case asTYPEID_UINT16: {
            uint16_t argument = 0;
            if (!read_integer(value, argument)) return SAO_ERR_INVALID_ARGUMENT;
            result = context->SetArgWord(index, argument);
            break;
        }
        case asTYPEID_INT32: {
            int32_t argument = 0;
            if (!read_integer(value, argument)) return SAO_ERR_INVALID_ARGUMENT;
            result = context->SetArgDWord(index, static_cast<asDWORD>(argument));
            break;
        }
        case asTYPEID_UINT32: {
            uint32_t argument = 0;
            if (!read_integer(value, argument)) return SAO_ERR_INVALID_ARGUMENT;
            result = context->SetArgDWord(index, argument);
            break;
        }
        case asTYPEID_INT64: {
            int64_t argument = 0;
            if (!read_integer(value, argument)) return SAO_ERR_INVALID_ARGUMENT;
            result = context->SetArgQWord(index, static_cast<asQWORD>(argument));
            break;
        }
        case asTYPEID_UINT64: {
            uint64_t argument = 0;
            if (!read_integer(value, argument)) return SAO_ERR_INVALID_ARGUMENT;
            result = context->SetArgQWord(index, argument);
            break;
        }
        case asTYPEID_FLOAT: {
            float argument = 0.0F;
            if (!read_floating(value, argument)) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            result = context->SetArgFloat(index, argument);
            break;
        }
        case asTYPEID_DOUBLE: {
            double argument = 0.0;
            if (!read_floating(value, argument)) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            result = context->SetArgDouble(index, argument);
            break;
        }
        default:
            if (!is_string_type(context->GetEngine(), type_id) ||
                !value.is_string()) {
                return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
            }
            string_arguments.push_back({value.get<std::string>()});
            result = context->SetArgObject(index, &string_arguments.back());
            break;
        }
        if (result < 0) return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
}

int32_t copy_json_result(const ordered_json& value, char** output) {
    if (output == nullptr) return SAO_OK;
    const std::string text = value.dump();
    auto buffer = std::unique_ptr<char, decltype(&std::free)>(
        static_cast<char*>(std::malloc(text.size() + 1)), &std::free);
    if (!buffer) return SAO_ERR_OS_CALL_FAILED;
    if (!text.empty()) std::memcpy(buffer.get(), text.data(), text.size());
    buffer.get()[text.size()] = '\0';
    *output = buffer.release();
    return SAO_OK;
}

int32_t serialize_return(asIScriptContext* context,
                         asIScriptFunction* function, char** output) {
    const int type_id = function->GetReturnTypeId();
    switch (type_id) {
    case asTYPEID_VOID:
        return copy_json_result(nullptr, output);
    case asTYPEID_BOOL:
        return copy_json_result(context->GetReturnByte() != 0, output);
    case asTYPEID_INT8:
        return copy_json_result(
            static_cast<int8_t>(context->GetReturnByte()), output);
    case asTYPEID_INT16:
        return copy_json_result(
            static_cast<int16_t>(context->GetReturnWord()), output);
    case asTYPEID_INT32:
        return copy_json_result(static_cast<int32_t>(context->GetReturnDWord()),
                                output);
    case asTYPEID_UINT8:
        return copy_json_result(context->GetReturnByte(), output);
    case asTYPEID_UINT16:
        return copy_json_result(context->GetReturnWord(), output);
    case asTYPEID_UINT32:
        return copy_json_result(context->GetReturnDWord(), output);
    case asTYPEID_INT64:
        return copy_json_result(static_cast<int64_t>(context->GetReturnQWord()),
                                output);
    case asTYPEID_UINT64:
        return copy_json_result(context->GetReturnQWord(), output);
    case asTYPEID_FLOAT: {
        const float value = context->GetReturnFloat();
        return copy_json_result(std::isfinite(value) ? ordered_json(value)
                                                     : ordered_json(nullptr),
                                output);
    }
    case asTYPEID_DOUBLE: {
        const double value = context->GetReturnDouble();
        return copy_json_result(std::isfinite(value) ? ordered_json(value)
                                                     : ordered_json(nullptr),
                                output);
    }
    default:
        if (!is_string_type(context->GetEngine(), type_id)) {
            return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        }
        const auto* value =
            static_cast<const as_string*>(context->GetReturnObject());
        return copy_json_result(value == nullptr ? ordered_json(nullptr)
                                                : ordered_json(value->data),
                                output);
    }
}

} // namespace

int32_t register_generic_core_bindings(asIScriptEngine* engine) {
    if (engine == nullptr) return SAO_ERR_INVALID_ARGUMENT;

    if (engine->GetTypeInfoByName("string") == nullptr) {
        int result = engine->RegisterObjectType(
            "string", sizeof(as_string),
            asOBJ_VALUE | asOBJ_APP_CLASS_CDAK);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectBehaviour(
            "string", asBEHAVE_CONSTRUCT, "void f()",
            asFUNCTION(string_construct), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectBehaviour(
            "string", asBEHAVE_CONSTRUCT, "void f(const string &in)",
            asFUNCTION(string_copy_construct), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectBehaviour(
            "string", asBEHAVE_DESTRUCT, "void f()",
            asFUNCTION(string_destruct), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterStringFactory("string", &g_string_factory);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectMethod(
            "string", "string &opAssign(const string &in)",
            asFUNCTION(string_assign), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectMethod(
            "string", "string &opAddAssign(const string &in)",
            asFUNCTION(string_add_assign), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectMethod(
            "string", "bool opEquals(const string &in) const",
            asFUNCTION(string_equals), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectMethod(
            "string", "string opAdd(const string &in) const",
            asFUNCTION(string_add), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
    }

    if (engine->GetTypeInfoByName("PluginContext") == nullptr) {
        int result = engine->RegisterObjectType(
            "PluginContext", 0, asOBJ_REF | asOBJ_NOCOUNT);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectMethod(
            "PluginContext", "void log(const string &in)",
            asFUNCTION(context_log), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectMethod(
            "PluginContext", "void log_info(const string &in)",
            asFUNCTION(context_log), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectMethod(
            "PluginContext", "string get_plugin_id() const property",
            asFUNCTION(context_plugin_id), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
        result = engine->RegisterObjectMethod(
            "PluginContext", "bool get_should_stop() const property",
            asFUNCTION(context_should_stop), asCALL_GENERIC);
        if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
    }

    int result = engine->RegisterGlobalFunction(
        "void log_info(const string &in)", asFUNCTION(global_log_info),
        asCALL_GENERIC);
    if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterGlobalFunction(
        "void register_ui_panel(const string &in)",
        asFUNCTION(global_register_ui_panel), asCALL_GENERIC);
    if (!registration_ok(result)) return SAO_ERR_OS_CALL_FAILED;
    result = engine->RegisterGlobalFunction(
        "void register_hotkey(const string &in, const string &in)",
        asFUNCTION(global_register_hotkey), asCALL_GENERIC);
    return registration_ok(result) ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
}

int32_t invoke_generic_function(asIScriptContext* context,
                                asIScriptFunction* function,
                                const char* args_json_utf8,
                                char** out_result_json_utf8,
                                void* context_user_data) {
    if (out_result_json_utf8 != nullptr) *out_result_json_utf8 = nullptr;
    if (context == nullptr || function == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        if (context->Prepare(function) < 0) return SAO_ERR_OS_CALL_FAILED;
        context->SetUserData(context_user_data,
                             kPluginContextUserDataSlot);
        std::vector<as_string> string_arguments;
        const int32_t argument_status =
            set_arguments(context, function, args_json_utf8, string_arguments);
        if (argument_status != SAO_OK) return argument_status;
        if (context->Execute() != asEXECUTION_FINISHED) {
            return SAO_ERR_OS_CALL_FAILED;
        }
        return serialize_return(context, function, out_result_json_utf8);
    } catch (const nlohmann::json::exception&) {
        return SAO_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

#else

int32_t register_generic_core_bindings(asIScriptEngine*) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

int32_t invoke_generic_function(asIScriptContext*, asIScriptFunction*,
                                const char*, char**, void*) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

#endif

} // namespace sao::plugins::angel_host
