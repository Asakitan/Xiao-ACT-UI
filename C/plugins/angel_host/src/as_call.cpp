// as_call.cpp — persistent module and separated AngelScript hook calls.

#include "sao/plugins/angel_host/as_call.h"

#include "as_generic_bindings_internal.h"
#include "as_plugin_internal.h"

#include "sao/plugins/loader/loader_status.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
#include <memory>
#include <string>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {
namespace {

#if defined(SAO_HAS_ANGELSCRIPT)

asIScriptFunction* find_hook(as_plugin_handle_t plugin, const char* name) {
    return plugin != nullptr && plugin->module != nullptr && name != nullptr
               ? plugin->module->GetFunctionByName(name)
               : nullptr;
}

int32_t read_source(const std::filesystem::path& path, std::string& output) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return SAO_ERR_HANDLE_INVALID;
    output.assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    return input.bad() ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
}

std::filesystem::path path_from_utf8(const char* value) {
#if defined(__cpp_char8_t)
    std::u8string encoded;
    while (*value != '\0') {
        encoded.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(*value++)));
    }
    return std::filesystem::path(encoded);
#else
    return std::filesystem::u8path(value);
#endif
}

int32_t invoke_with_new_context(asIScriptEngine* engine,
                                asIScriptFunction* function,
                                const char* args_json,
                                char** output) {
    asIScriptContext* raw = engine->CreateContext();
    if (raw == nullptr) return SAO_ERR_OS_CALL_FAILED;
    const std::unique_ptr<asIScriptContext, void (*)(asIScriptContext*)> context(
        raw, [](asIScriptContext* value) { value->Release(); });
    return invoke_generic_function(context.get(), function, args_json, output);
}

int32_t execute_lifecycle(as_plugin_handle_t plugin,
                          asIScriptFunction* function,
                          bool pass_context,
                          bool* bool_result = nullptr) {
    if (function == nullptr || plugin->context == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (pass_context) {
        int type_id = 0;
        if (function->GetParamCount() != 1 ||
            function->GetParam(0, &type_id) < 0 ||
            (type_id & asTYPEID_OBJHANDLE) == 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto* type = plugin->engine->GetTypeInfoById(type_id);
        if (type == nullptr ||
            std::strcmp(type->GetName(), "PluginContext") != 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    } else if (function->GetParamCount() != 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int return_type = function->GetReturnTypeId();
    if ((bool_result == nullptr && return_type != asTYPEID_VOID) ||
        (bool_result != nullptr && return_type != asTYPEID_VOID &&
         return_type != asTYPEID_BOOL)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (plugin->context->Prepare(function) < 0) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    plugin->context->SetUserData(plugin, kPluginContextUserDataSlot);
    if (pass_context &&
        plugin->context->SetArgObject(0, plugin->bound_context) < 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (plugin->context->Execute() != asEXECUTION_FINISHED) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (bool_result != nullptr &&
        function->GetReturnTypeId() == asTYPEID_BOOL) {
        *bool_result = plugin->context->GetReturnByte() != 0;
    }
    return SAO_OK;
}

#endif

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_load_script(asIScriptEngine* engine,
                               const wchar_t* plugin_dir,
                               const char* entry_relative,
                               const char* plugin_id_utf8,
                               void* ctx_ptr,
                               as_plugin_handle_t* out_plugin) {
    if (out_plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    if (engine == nullptr || plugin_dir == nullptr ||
        entry_relative == nullptr || plugin_id_utf8 == nullptr ||
        entry_relative[0] == '\0' || plugin_id_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const std::filesystem::path path =
            std::filesystem::path(plugin_dir) /
            path_from_utf8(entry_relative);
        std::string source;
        int32_t status = read_source(path, source);
        if (status != SAO_OK) return status;
        status = register_generic_core_bindings(engine);
        if (status != SAO_OK) return status;

        auto plugin = std::make_unique<as_plugin_s>();
        plugin->engine = engine;
        plugin->bound_context = ctx_ptr;
        plugin->plugin_id = plugin_id_utf8;
        plugin->module_name = "sao_plugin_" + plugin->plugin_id;
        plugin->module =
            engine->GetModule(plugin->module_name.c_str(), asGM_ALWAYS_CREATE);
        if (plugin->module == nullptr) return SAO_ERR_OS_CALL_FAILED;
        if (plugin->module->AddScriptSection(entry_relative, source.c_str(),
                                             source.size()) < 0 ||
            plugin->module->Build() < 0) {
            engine->DiscardModule(plugin->module_name.c_str());
            plugin->module = nullptr;
            return SAO_ERR_INVALID_ARGUMENT;
        }
        plugin->context = engine->CreateContext();
        if (plugin->context == nullptr) {
            engine->DiscardModule(plugin->module_name.c_str());
            plugin->module = nullptr;
            return SAO_ERR_OS_CALL_FAILED;
        }
        plugin->context->SetUserData(plugin.get(),
                         kPluginContextUserDataSlot);
        *out_plugin = plugin.release();
        return SAO_OK;
#else
        (void)ctx_ptr;
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_load(as_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        std::lock_guard lock(plugin->call_mutex);
        asIScriptFunction* function = find_hook(plugin, "on_load");
        if (function == nullptr) return SAO_OK;
        return execute_lifecycle(plugin, function,
                     function->GetParamCount() == 1);
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_enable(as_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        std::lock_guard lock(plugin->call_mutex);
        auto* function = find_hook(plugin, "on_enable");
        return function == nullptr
                   ? SAO_OK
                   : execute_lifecycle(plugin, function, false);
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_disable(as_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        std::lock_guard lock(plugin->call_mutex);
        auto* function = find_hook(plugin, "on_disable");
        return function == nullptr
                   ? SAO_OK
                   : execute_lifecycle(plugin, function, false);
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_unload(as_plugin_handle_t plugin,
                                  bool* out_allow_unload) {
    if (out_allow_unload == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_allow_unload = true;
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        std::lock_guard lock(plugin->call_mutex);
        auto* function = find_hook(plugin, "on_unload");
        return function == nullptr
                   ? SAO_OK
                   : execute_lifecycle(plugin, function, false,
                                       out_allow_unload);
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_hook(as_plugin_handle_t plugin,
                             const char* hook_name,
                             const char* args_json_utf8,
                             char** out_result_json_utf8) {
    if (out_result_json_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_result_json_utf8 = nullptr;
    if (plugin == nullptr || hook_name == nullptr || hook_name[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        std::lock_guard lock(plugin->call_mutex);
        auto* function = find_hook(plugin, hook_name);
        if (function == nullptr) return SAO_ERR_HANDLE_INVALID;
        return invoke_generic_function(plugin->context, function,
                           args_json_utf8,
                           out_result_json_utf8, plugin);
#else
        (void)args_json_utf8;
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_has_hook(as_plugin_handle_t plugin,
                            const char* hook_name) {
    if (plugin == nullptr || hook_name == nullptr) return false;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        std::lock_guard lock(plugin->call_mutex);
        return find_hook(plugin, hook_name) != nullptr;
#else
        return false;
#endif
    } catch (...) {
        return false;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_function(asIScriptEngine* engine,
                                 asIScriptFunction* fn,
                                 const char* args_json_utf8,
                                 char** out_result_json_utf8) {
    if (out_result_json_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_result_json_utf8 = nullptr;
    if (engine == nullptr || fn == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
    return invoke_with_new_context(engine, fn, args_json_utf8,
                       out_result_json_utf8);
#else
        (void)args_json_utf8;
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unload_script(as_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        {
            std::lock_guard lock(plugin->call_mutex);
            if (plugin->context != nullptr) {
                plugin->context->SetUserData(nullptr,
                                             kPluginContextUserDataSlot);
                plugin->context->Release();
                plugin->context = nullptr;
            }
            if (plugin->module != nullptr && plugin->engine != nullptr) {
                plugin->engine->DiscardModule(plugin->module_name.c_str());
                plugin->module = nullptr;
            }
        }
        delete plugin;
        return SAO_OK;
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API asIScriptModule* SAO_PLUGINS_CALL
sao_plugins_ashost_get_module(as_plugin_handle_t plugin) {
    try {
        return plugin != nullptr ? plugin->module : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API asIScriptEngine* SAO_PLUGINS_CALL
sao_plugins_ashost_get_engine(as_plugin_handle_t plugin) {
    try {
        return plugin != nullptr ? plugin->engine : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_ashost_get_bound_context(as_plugin_handle_t plugin) {
    try {
        return plugin != nullptr ? plugin->bound_context : nullptr;
    } catch (...) {
        return nullptr;
    }
}

} // namespace sao::plugins::angel_host
