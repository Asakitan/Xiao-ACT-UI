// as_call.cpp — persistent module and separated AngelScript hook calls.

#include "sao/plugins/angel_host/as_call.h"

#include "as_generic_bindings_internal.h"
#include "as_plugin_internal.h"

#include "sao/plugins/angel_host/as_error.h"
#include "sao/plugins/angel_host/as_module_bridge.h"
#include "sao/plugins/angel_host/as_stdlib.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/sdk_binding/binding_angel.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {

namespace {

std::mutex g_plugin_registry_mutex;
std::unordered_map<as_plugin_handle_t, shared_plugin_state> g_plugin_registry;
std::atomic_uint64_t g_next_module_generation{1};

std::string next_module_name(const std::string& plugin_id) {
    uint64_t generation = g_next_module_generation.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0)
        generation = g_next_module_generation.fetch_add(1, std::memory_order_relaxed);
    return "sao_plugin_" + plugin_id + "_g" + std::to_string(generation);
}

std::string format_retained_error(const retained_script_error& error) {
    std::ostringstream stream;
    stream << error.phase << ": " << error.message;
    if (!error.function.empty())
        stream << "\nfunction: " << error.function;
    if (!error.section.empty() || error.line != 0 || error.column != 0) {
        stream << "\nlocation: " << (error.section.empty() ? "<unknown>" : error.section) << ':'
               << error.line << ':' << error.column;
    }
    return stream.str();
}

} // namespace

int32_t register_plugin_state(const shared_plugin_state& plugin) {
    if (!plugin)
        return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_plugin_registry_mutex);
    return g_plugin_registry.emplace(plugin.get(), plugin).second
               ? SAO_OK
               : sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
}

shared_plugin_state acquire_plugin_state(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return {};
    std::lock_guard lock(g_plugin_registry_mutex);
    const auto found = g_plugin_registry.find(plugin);
    return found == g_plugin_registry.end() ? shared_plugin_state{} : found->second;
}

shared_plugin_state retire_plugin_state(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return {};
    std::lock_guard lock(g_plugin_registry_mutex);
    const auto found = g_plugin_registry.find(plugin);
    if (found == g_plugin_registry.end())
        return {};
    shared_plugin_state state = found->second;
    g_plugin_registry.erase(found);
    return state;
}

int32_t restore_plugin_state(const shared_plugin_state& plugin) {
    if (!plugin)
        return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_plugin_registry_mutex);
    return g_plugin_registry.emplace(plugin.get(), plugin).second ? SAO_OK : SAO_ERR_HANDLE_INVALID;
}

void retain_plugin_error(as_plugin_s& plugin, const char* phase, int32_t status,
                         const char* message, asIScriptContext* context) {
    retained_script_error error;
    error.status = status;
    error.phase = phase == nullptr ? "AngelScript" : phase;
    error.message = message == nullptr ? "operation failed" : message;
#if defined(SAO_HAS_ANGELSCRIPT)
    if (context != nullptr && context->GetState() == asEXECUTION_EXCEPTION) {
        int column = 0;
        const char* section = nullptr;
        error.line = context->GetExceptionLineNumber(&column, &section);
        error.column = column;
        if (section != nullptr)
            error.section = section;
        if (const char* exception = context->GetExceptionString(); exception != nullptr)
            error.message = std::string("AngelScript exception: ") + exception;
        if (const asIScriptFunction* function = context->GetExceptionFunction();
            function != nullptr) {
            if (const char* declaration = function->GetDeclaration(true, true, true);
                declaration != nullptr) {
                error.function = declaration;
            }
        }
    }
#else
    (void)context;
#endif
    plugin.retained_errors[error.phase] = std::move(error);
}

void clear_plugin_error(as_plugin_s& plugin, const char* phase) {
    if (phase != nullptr)
        plugin.retained_errors.erase(phase);
}

std::string plugin_error_text(as_plugin_handle_t plugin, const char* phase, const char* fallback) {
    const shared_plugin_state state = acquire_plugin_state(plugin);
    if (!state)
        return fallback == nullptr ? std::string{} : std::string(fallback);
    std::lock_guard lock(state->call_mutex);
    const auto found = state->retained_errors.find(phase == nullptr ? "" : phase);
    return found == state->retained_errors.end()
               ? (fallback == nullptr ? std::string{} : std::string(fallback))
               : format_retained_error(found->second);
}

namespace {

void retain_unexpected_lifecycle_error(as_plugin_handle_t plugin, const char* phase) noexcept {
    try {
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return;
        std::lock_guard lock(state->call_mutex);
        retain_plugin_error(*state, phase, SAO_ERR_OS_CALL_FAILED,
                            "AngelScript lifecycle raised a C++ exception");
    } catch (...) {
    }
}

#if defined(SAO_HAS_ANGELSCRIPT)

asIScriptFunction* find_hook(as_plugin_handle_t plugin, const char* name) {
    engine_execution_guard engine_lock;
    return plugin != nullptr && plugin->module != nullptr && name != nullptr
               ? plugin->module->GetFunctionByName(name)
               : nullptr;
}

int32_t capture_execution_error(as_plugin_s& plugin, asIScriptContext* context, const char* phase,
                                const char* fallback) {
    retain_plugin_error(plugin, phase, SAO_ERR_OS_CALL_FAILED, fallback, context);
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t read_source(const std::filesystem::path& path, std::string& output) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return SAO_ERR_HANDLE_INVALID;
    output.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.bad() ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
}

std::filesystem::path path_from_utf8(const char* value) {
#if defined(__cpp_char8_t)
    std::u8string encoded;
    while (*value != '\0') {
        encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(*value++)));
    }
    return std::filesystem::path(encoded);
#else
    return std::filesystem::u8path(value);
#endif
}

int32_t invoke_with_new_context(asIScriptEngine* engine, asIScriptFunction* function,
                                const char* args_json, char** output) {
    engine_execution_guard engine_lock;
    asIScriptContext* raw = engine->CreateContext();
    if (raw == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    const std::unique_ptr<asIScriptContext, void (*)(asIScriptContext*)> context(
        raw, [](asIScriptContext* value) { value->Release(); });
    return invoke_generic_function(context.get(), function, args_json, output);
}

int32_t execute_lifecycle(as_plugin_s& plugin, asIScriptFunction* function, const char* phase,
                          bool pass_context, bool* bool_result = nullptr) {
    engine_execution_guard engine_lock;
    if (function == nullptr || plugin.context == nullptr) {
        retain_plugin_error(plugin, phase, SAO_ERR_HANDLE_INVALID,
                            "AngelScript lifecycle function or context is unavailable");
        return SAO_ERR_HANDLE_INVALID;
    }
    if (pass_context) {
        int type_id = 0;
        if (function->GetParamCount() != 1 || function->GetParam(0, &type_id) < 0 ||
            (type_id & asTYPEID_OBJHANDLE) == 0) {
            retain_plugin_error(plugin, phase, SAO_ERR_INVALID_ARGUMENT,
                                "AngelScript lifecycle signature is invalid");
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto* type = plugin.engine->GetTypeInfoById(type_id);
        if (type == nullptr || std::strcmp(type->GetName(), "PluginContext") != 0) {
            retain_plugin_error(plugin, phase, SAO_ERR_INVALID_ARGUMENT,
                                "AngelScript lifecycle context type is invalid");
            return SAO_ERR_INVALID_ARGUMENT;
        }
    } else if (function->GetParamCount() != 0) {
        retain_plugin_error(plugin, phase, SAO_ERR_INVALID_ARGUMENT,
                            "AngelScript lifecycle hook must not accept arguments");
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int return_type = function->GetReturnTypeId();
    if ((bool_result == nullptr && return_type != asTYPEID_VOID) ||
        (bool_result != nullptr && return_type != asTYPEID_VOID && return_type != asTYPEID_BOOL)) {
        retain_plugin_error(plugin, phase, SAO_ERR_INVALID_ARGUMENT,
                            "AngelScript lifecycle return type is invalid");
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (plugin.context->Prepare(function) < 0) {
        retain_plugin_error(plugin, phase, SAO_ERR_OS_CALL_FAILED,
                            "AngelScript context prepare failed");
        return SAO_ERR_OS_CALL_FAILED;
    }
    clear_plugin_error(plugin, phase);
    plugin.context->SetUserData(&plugin, kPluginContextUserDataSlot);
    if (pass_context && plugin.context->SetArgObject(0, plugin.bound_context) < 0) {
        retain_plugin_error(plugin, phase, SAO_ERR_INVALID_ARGUMENT,
                            "AngelScript lifecycle context binding failed");
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (plugin.context->Execute() != asEXECUTION_FINISHED) {
        return capture_execution_error(plugin, plugin.context, phase,
                                       "AngelScript lifecycle did not finish");
    }
    if (bool_result != nullptr && function->GetReturnTypeId() == asTYPEID_BOOL) {
        *bool_result = plugin.context->GetReturnByte() != 0;
    }
    return SAO_OK;
}

int32_t teardown_plugin_locked(as_plugin_s& plugin) {
    engine_execution_guard engine_lock;
    if (plugin.binding != nullptr) {
        const int32_t status = sdk_binding::sao_plugins_binding_angel_deactivate(plugin.binding);
        if (status != SAO_OK) {
            plugin.lifecycle = plugin_runtime_state::cleanup_pending;
            retain_plugin_error(plugin, "teardown", status,
                                "AngelScript sdk_binding deactivate failed");
            return status;
        }
        plugin.binding = nullptr;
    }
    if (plugin.module != nullptr && plugin.engine != nullptr) {
        const int32_t status = sao_plugins_ashost_bind_ctx(
            plugin.engine, nullptr, plugin.module_name.c_str());
        if (status != SAO_OK) {
            plugin.lifecycle = plugin_runtime_state::cleanup_pending;
            retain_plugin_error(plugin, "teardown", status,
                                "AngelScript module context cleanup failed");
            return status;
        }
    }
    if (plugin.context != nullptr) {
        plugin.context->SetUserData(nullptr, kPluginContextUserDataSlot);
        plugin.context->Release();
        plugin.context = nullptr;
    }
    if (plugin.module != nullptr && plugin.engine != nullptr) {
        plugin.engine->DiscardModule(plugin.module_name.c_str());
        plugin.module = nullptr;
    }
    plugin.bound_context = nullptr;
    plugin.lifecycle = plugin_runtime_state::dead;
    plugin.unload_owner = {};
    plugin.unload_hook_completed = false;
    return SAO_OK;
}

int32_t rollback_failed_load(const shared_plugin_state& plugin, int32_t failure,
                             as_plugin_handle_t* out_plugin) noexcept {
    if (!plugin)
        return failure;
    int32_t cleanup_status = SAO_OK;
    try {
        std::lock_guard lock(plugin->call_mutex);
        cleanup_status = teardown_plugin_locked(*plugin);
    } catch (...) {
        cleanup_status = SAO_ERR_OS_CALL_FAILED;
        plugin->lifecycle = plugin_runtime_state::cleanup_pending;
    }
    if (cleanup_status == SAO_OK)
        return failure;
    const int32_t register_status = register_plugin_state(plugin);
    if (register_status == SAO_OK ||
        register_status == sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS) {
        if (out_plugin != nullptr)
            *out_plugin = plugin.get();
        return cleanup_status;
    }
    return register_status;
}


#endif

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ashost_load_script(
    asIScriptEngine* engine, const wchar_t* plugin_dir, const char* entry_relative,
    const char* plugin_id_utf8, void* ctx_ptr, as_plugin_handle_t* out_plugin) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    if (engine == nullptr || plugin_dir == nullptr || entry_relative == nullptr ||
        plugin_id_utf8 == nullptr || entry_relative[0] == '\0' or plugin_id_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    shared_plugin_state plugin;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_host_state host_state = acquire_host_for_engine(engine);
        if (!host_state)
            return SAO_ERR_HANDLE_INVALID;
        host_instance_lease host_instance;
        const int32_t admission_status = host_instance.acquire(host_state);
        if (admission_status != SAO_OK)
            return admission_status;
        engine_execution_guard engine_lock;
        const std::filesystem::path path =
            std::filesystem::path(plugin_dir) / path_from_utf8(entry_relative);
        std::string source;
        int32_t status = read_source(path, source);
        if (status != SAO_OK)
            return status;
        status = sao_plugins_ashost_register_sdk(engine);
        if (status != SAO_OK)
            return status;
        const int32_t stdlib_status = sao_plugins_ashost_install_stdlib(engine);
        if (stdlib_status != SAO_OK and stdlib_status != SAO_ERR_NOT_IMPLEMENTED)
            return stdlib_status;

        plugin = std::make_shared<as_plugin_s>();
        plugin->host_state = host_state;
        plugin->host_instance = std::move(host_instance);
        plugin->engine = engine;
        plugin->bound_context = ctx_ptr;
        plugin->plugin_id = plugin_id_utf8;
        plugin->module_name = next_module_name(plugin->plugin_id);
        plugin->module = engine->GetModule(plugin->module_name.c_str(), asGM_ALWAYS_CREATE);
        if (plugin->module == nullptr)
            return rollback_failed_load(plugin, SAO_ERR_OS_CALL_FAILED, out_plugin);
        constexpr char bridge_section[] = "PluginContext@ ctx;";
        if (plugin->module->AddScriptSection("sao_module_bridge", bridge_section,
                                             sizeof(bridge_section) - 1) < 0 or
            plugin->module->AddScriptSection(entry_relative, source.c_str(), source.size()) < 0 or
            plugin->module->Build() < 0) {
            return rollback_failed_load(plugin, SAO_ERR_INVALID_ARGUMENT, out_plugin);
        }
        status = sao_plugins_ashost_bind_ctx(engine, ctx_ptr, plugin->module_name.c_str());
        if (status != SAO_OK)
            return rollback_failed_load(plugin, status, out_plugin);
        status = sdk_binding::sao_plugins_binding_angel_activate(
            reinterpret_cast<sdk_binding::plugin_context_ptr>(ctx_ptr), engine, &plugin->binding);
        if (status != SAO_OK)
            return rollback_failed_load(plugin, status, out_plugin);
        plugin->context = engine->CreateContext();
        if (plugin->context == nullptr)
            return rollback_failed_load(plugin, SAO_ERR_OS_CALL_FAILED, out_plugin);
        plugin->context->SetUserData(plugin.get(), kPluginContextUserDataSlot);
        status = register_plugin_state(plugin);
        if (status != SAO_OK)
            return rollback_failed_load(plugin, status, out_plugin);
        *out_plugin = plugin.get();
        return SAO_OK;
#else
        (void)ctx_ptr;
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return rollback_failed_load(plugin, SAO_ERR_OS_CALL_FAILED, out_plugin);
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_load(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        if (state->lifecycle != plugin_runtime_state::ready)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        asIScriptFunction* function = find_hook(state.get(), "on_load");
        if (function == nullptr)
            return SAO_OK;
        return execute_lifecycle(*state, function, "on_load", function->GetParamCount() == 1);
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        retain_unexpected_lifecycle_error(plugin, "on_load");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_enable(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        if (state->lifecycle != plugin_runtime_state::ready)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        auto* function = find_hook(state.get(), "on_enable");
        return function == nullptr ? SAO_OK
                                   : execute_lifecycle(*state, function, "on_enable", false);
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        retain_unexpected_lifecycle_error(plugin, "on_enable");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_disable(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        if (state->lifecycle != plugin_runtime_state::ready)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        auto* function = find_hook(state.get(), "on_disable");
        return function == nullptr ? SAO_OK
                                   : execute_lifecycle(*state, function, "on_disable", false);
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        retain_unexpected_lifecycle_error(plugin, "on_disable");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_unload(as_plugin_handle_t plugin, bool* out_allow_unload) {
    if (out_allow_unload == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_allow_unload = true;
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    shared_plugin_state state;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        const std::thread::id owner = std::this_thread::get_id();
        if (state->lifecycle == plugin_runtime_state::ready) {
            state->lifecycle = plugin_runtime_state::unloading;
            state->unload_owner = owner;
            state->unload_hook_completed = false;
        } else if (state->lifecycle == plugin_runtime_state::unloading) {
            if (state->unload_owner != owner)
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            if (state->unload_hook_completed)
                return SAO_OK;
        } else {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        auto* function = find_hook(state.get(), "on_unload");
        if (function == nullptr) {
            state->unload_hook_completed = true;
            return SAO_OK;
        }
        const int32_t status = execute_lifecycle(*state, function, "on_unload", false,
                                                  out_allow_unload);
        if (status != SAO_OK || !*out_allow_unload) {
            state->lifecycle = plugin_runtime_state::ready;
            state->unload_owner = {};
            state->unload_hook_completed = false;
        } else {
            state->unload_hook_completed = true;
        }
        return status;
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        if (state) {
            try {
                std::lock_guard lock(state->call_mutex);
                if (state->unload_owner == std::this_thread::get_id() &&
                    state->lifecycle == plugin_runtime_state::unloading) {
                    state->lifecycle = plugin_runtime_state::ready;
                    state->unload_owner = {};
                    state->unload_hook_completed = false;
                }
            } catch (...) {
            }
        }
        retain_unexpected_lifecycle_error(plugin, "on_unload");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_hook(as_plugin_handle_t plugin, const char* hook_name,
                             const char* args_json_utf8, char** out_result_json_utf8) {
    if (out_result_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_result_json_utf8 = nullptr;
    if (plugin == nullptr || hook_name == nullptr || hook_name[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        engine_execution_guard engine_lock;
        if (state->lifecycle != plugin_runtime_state::ready)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        auto* function = find_hook(state.get(), hook_name);
        if (function == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        clear_plugin_error(*state, hook_name);
        const int32_t status = invoke_generic_function(state->context, function, args_json_utf8,
                                                       out_result_json_utf8, state.get());
        if (status != SAO_OK && state->context->GetState() == asEXECUTION_EXCEPTION) {
            return capture_execution_error(*state, state->context, hook_name,
                                           "AngelScript hook did not finish");
        }
        if (status != SAO_OK) {
            retain_plugin_error(*state, hook_name, status,
                                "AngelScript hook argument or return conversion failed");
        }
        return status;
#else
        (void)args_json_utf8;
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_has_hook(as_plugin_handle_t plugin, const char* hook_name) {
    if (plugin == nullptr || hook_name == nullptr)
        return false;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return false;
        std::lock_guard lock(state->call_mutex);
        return state->lifecycle == plugin_runtime_state::ready &&
               find_hook(state.get(), hook_name) != nullptr;
#else
        return false;
#endif
    } catch (...) {
        return false;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_function(asIScriptEngine* engine, asIScriptFunction* fn,
                                 const char* args_json_utf8, char** out_result_json_utf8) {
    if (out_result_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_result_json_utf8 = nullptr;
    if (engine == nullptr || fn == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        return invoke_with_new_context(engine, fn, args_json_utf8, out_result_json_utf8);
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
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    shared_plugin_state state;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        const std::thread::id owner = std::this_thread::get_id();
        if (state->lifecycle == plugin_runtime_state::ready ||
            state->lifecycle == plugin_runtime_state::cleanup_pending) {
            state->lifecycle = plugin_runtime_state::unloading;
            state->unload_owner = owner;
        } else if (state->lifecycle == plugin_runtime_state::unloading) {
            if (state->unload_owner != owner)
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        } else {
            return SAO_ERR_HANDLE_INVALID;
        }
        const int32_t status = teardown_plugin_locked(*state);
        if (status != SAO_OK) {
            state->lifecycle = plugin_runtime_state::cleanup_pending;
            state->unload_owner = {};
            state->unload_hook_completed = false;
            return status;
        }
        const shared_plugin_state retired = retire_plugin_state(plugin);
        if (!retired)
            return SAO_ERR_HANDLE_INVALID;
        state->host_instance.reset();
        state->host_state.reset();
        return SAO_OK;
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        if (state) {
            try {
                std::lock_guard lock(state->call_mutex);
                if (state->unload_owner == std::this_thread::get_id() &&
                    state->lifecycle == plugin_runtime_state::unloading) {
                    state->lifecycle = plugin_runtime_state::cleanup_pending;
                    state->unload_owner = {};
                    state->unload_hook_completed = false;
                }
            } catch (...) {
            }
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API asIScriptModule* SAO_PLUGINS_CALL
sao_plugins_ashost_get_module(as_plugin_handle_t plugin) {
    try {
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return nullptr;
        std::lock_guard lock(state->call_mutex);
        return state->lifecycle == plugin_runtime_state::ready ? state->module : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API asIScriptEngine* SAO_PLUGINS_CALL
sao_plugins_ashost_get_engine(as_plugin_handle_t plugin) {
    try {
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return nullptr;
        std::lock_guard lock(state->call_mutex);
        return state->lifecycle == plugin_runtime_state::ready ? state->engine : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_ashost_get_bound_context(as_plugin_handle_t plugin) {
    try {
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return nullptr;
        std::lock_guard lock(state->call_mutex);
        return state->lifecycle == plugin_runtime_state::ready ? state->bound_context : nullptr;
    } catch (...) {
        return nullptr;
    }
}

} // namespace sao::plugins::angel_host
