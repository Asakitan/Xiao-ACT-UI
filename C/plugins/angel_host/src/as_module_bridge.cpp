#include "sao/plugins/angel_host/as_module_bridge.h"

#include "as_generic_bindings_internal.h"

#include "sao/plugins/angel_host/as_call.h"
#include "as_host_internal.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/sdk_binding/binding_angel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {

namespace {
std::recursive_mutex g_engine_execution_mutex;
#if defined(SAO_HAS_ANGELSCRIPT)
thread_local size_t g_engine_execution_depth = 0;
thread_local shared_host_state g_deferred_host_finalize;
#endif
}

std::recursive_mutex& engine_execution_mutex() noexcept {
    return g_engine_execution_mutex;
}

bool engine_execution_active() noexcept {
#if defined(SAO_HAS_ANGELSCRIPT)
    return g_engine_execution_depth != 0;
#else
    return false;
#endif
}

#if defined(SAO_HAS_ANGELSCRIPT)
engine_execution_guard::engine_execution_guard() : lock_(engine_execution_mutex()) {
    ++g_engine_execution_depth;
}

engine_execution_guard::~engine_execution_guard() {
    if (g_engine_execution_depth > 0)
        --g_engine_execution_depth;
    const bool outermost = g_engine_execution_depth == 0;
    lock_.unlock();
    if (outermost && g_deferred_host_finalize) {
        auto host = std::move(g_deferred_host_finalize);
        maybe_finalize_host(host);
    }
}

void defer_host_finalize(const shared_host_state& host) noexcept {
    if (host)
        g_deferred_host_finalize = host;
}
#endif

#if defined(SAO_HAS_ANGELSCRIPT)
namespace {

struct angel_binding_plugin {
    sdk_binding::plugin_context_ptr context = nullptr;
    asIScriptEngine* engine = nullptr;
};

struct angel_callback {
    asIScriptEngine* engine = nullptr;
    asIScriptFunction* function = nullptr;
    host_callback_lease host_callback;
    std::mutex mutex;
    std::condition_variable idle;
    size_t active_calls = 0;
    bool releasing = false;
    bool release_pending = false;
};

std::mutex g_provider_mutex;
bool g_provider_registered = false;
std::atomic_size_t g_live_provider_plugins{0};
std::atomic_size_t g_live_callbacks{0};
constexpr size_t kMaximumCallbackNesting = 64;
thread_local std::array<angel_callback*, kMaximumCallbackNesting> g_active_callbacks{};
thread_local size_t g_active_callback_depth = 0;

bool callback_active_on_current_thread(const angel_callback* callback) noexcept {
    return std::find(g_active_callbacks.begin(),
                     g_active_callbacks.begin() + g_active_callback_depth,
                     callback) != g_active_callbacks.begin() + g_active_callback_depth;
}

void finish_callback_release(angel_callback* callback) noexcept {
    if (callback == nullptr)
        return;
    engine_execution_guard engine_lock;
    callback->function->Release();
    g_live_callbacks.fetch_sub(1, std::memory_order_relaxed);
    callback->host_callback.reset();
    delete callback;
}

bool SAO_PLUGINS_CALL provider_available(void*) {
    return true;
}

int32_t SAO_PLUGINS_CALL provider_load(void* context, void* runtime, void** out_plugin, void*) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    if (context == nullptr || runtime == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto plugin = std::make_unique<angel_binding_plugin>();
    plugin->context = reinterpret_cast<sdk_binding::plugin_context_ptr>(context);
    plugin->engine = static_cast<asIScriptEngine*>(runtime);
    *out_plugin = plugin.release();
    g_live_provider_plugins.fetch_add(1, std::memory_order_relaxed);
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_unload(void* plugin, void*) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    sdk_binding::sao_plugins_binding_release_all_callbacks();
    delete static_cast<angel_binding_plugin*>(plugin);
    g_live_provider_plugins.fetch_sub(1, std::memory_order_relaxed);
    return SAO_OK;
}

int32_t copy_provider_output(const std::string& value, uint8_t* output, size_t capacity,
                             size_t* required) {
    if (required == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *required = value.size() + 1;
    if (output == nullptr || capacity < *required)
        return SAO_ERR_BUFFER_TOO_SMALL;
    std::memcpy(output, value.c_str(), *required);
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_invoke(void* opaque_plugin, const char* method_name,
                                         const uint8_t* arguments, size_t arguments_size,
                                         uint8_t* output, size_t output_capacity,
                                         size_t* out_required, char* out_error,
                                         size_t error_capacity, void*) {
    if (out_required != nullptr)
        *out_required = 0;
    if (opaque_plugin == nullptr || method_name == nullptr || method_name[0] == '\0' ||
        out_required == nullptr || (arguments_size != 0 && arguments == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* plugin = static_cast<angel_binding_plugin*>(opaque_plugin);
    engine_execution_guard engine_lock;
    asIScriptFunction* function = nullptr;
    for (asUINT index = 0; index < plugin->engine->GetModuleCount() && function == nullptr;
         ++index) {
        asIScriptModule* module = plugin->engine->GetModuleByIndex(index);
        if (module != nullptr)
            function = module->GetFunctionByName(method_name);
    }
    if (function == nullptr)
        return SAO_ERR_HANDLE_INVALID;

    const std::string args =
        arguments == nullptr
            ? std::string{}
            : std::string(reinterpret_cast<const char*>(arguments), arguments_size);
    char* raw_result = nullptr;
    const int32_t status = sao_plugins_ashost_call_function(
        plugin->engine, function, args.empty() ? nullptr : args.c_str(), &raw_result);
    const std::string result = raw_result == nullptr ? std::string("null") : raw_result;
    sao_plugins_ashost_free_string(raw_result);
    if (status != SAO_OK) {
        if (out_error != nullptr && error_capacity != 0) {
            constexpr char message[] = "AngelScript provider invoke failed";
            const size_t count = (std::min)(error_capacity - 1, sizeof(message) - 1);
            std::memcpy(out_error, message, count);
            out_error[count] = '\0';
        }
        return status;
    }
    return copy_provider_output(result, output, output_capacity, out_required);
}

void SAO_PLUGINS_CALL invoke_angel_callback(void* user_data) {
    auto* callback = static_cast<angel_callback*>(user_data);
    if (callback == nullptr)
        return;
    if (g_active_callback_depth == kMaximumCallbackNesting)
        return;
    {
        std::lock_guard lock(callback->mutex);
        if (callback->releasing)
            return;
        ++callback->active_calls;
    }
    g_active_callbacks[g_active_callback_depth++] = callback;
    try {
        engine_execution_guard engine_lock;
        asIScriptContext* context = callback->engine->CreateContext();
        if (context != nullptr) {
            if (context->Prepare(callback->function) >= 0)
                (void)context->Execute();
            context->Release();
        }
    } catch (...) {
    }
    if (g_active_callback_depth > 0 &&
        g_active_callbacks[g_active_callback_depth - 1] == callback) {
        g_active_callbacks[--g_active_callback_depth] = nullptr;
    }
    bool finish_release = false;
    {
        std::lock_guard lock(callback->mutex);
        --callback->active_calls;
        if (callback->active_calls == 0) {
            finish_release = callback->release_pending;
            callback->idle.notify_all();
        }
    }
    if (finish_release)
        finish_callback_release(callback);
}

void SAO_PLUGINS_CALL provider_release_callback(void* callback_user_data, void*) {
    auto* callback = static_cast<angel_callback*>(callback_user_data);
    if (callback == nullptr)
        return;
    {
        std::unique_lock lock(callback->mutex);
        callback->releasing = true;
        if (callback_active_on_current_thread(callback)) {
            callback->release_pending = true;
            return;
        }
        callback->idle.wait(lock, [callback] { return callback->active_calls == 0; });
    }
    finish_callback_release(callback);
}

int32_t wrap_callback(sdk_binding::language_binding_request& request) {
    if (request.runtime == nullptr || request.value == nullptr || request.out_callback == nullptr ||
        request.out_user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto callback = std::make_unique<angel_callback>();
    callback->engine = static_cast<asIScriptEngine*>(request.runtime);
    callback->function = static_cast<asIScriptFunction*>(request.value);
    const auto host = acquire_host_for_engine(callback->engine);
    const int32_t host_status = callback->host_callback.acquire(host);
    if (host_status != SAO_OK)
        return host_status;
    engine_execution_guard engine_lock;
    callback->function->AddRef();
    g_live_callbacks.fetch_add(1, std::memory_order_relaxed);
    *request.out_callback = reinterpret_cast<void*>(&invoke_angel_callback);
    *request.out_user_data = callback.release();
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_dispatch(sdk_binding::language_binding_operation operation,
                                           sdk_binding::language_binding_request* request, void*) {
    if (request == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    switch (operation) {
    case sdk_binding::language_binding_operation::runtime_register:
        return register_generic_core_bindings(static_cast<asIScriptEngine*>(request->runtime));
    case sdk_binding::language_binding_operation::context_bind:
        return request->runtime != nullptr && request->context != nullptr
                   ? SAO_OK
                   : SAO_ERR_INVALID_ARGUMENT;
    case sdk_binding::language_binding_operation::callback_wrap:
        return wrap_callback(*request);
    default:
        return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    }
}

int32_t ensure_angel_provider() {
    std::lock_guard lock(g_provider_mutex);
    if (g_provider_registered)
        return SAO_OK;
    if (sdk_binding::sao_plugins_binding_language_host_available(
            sdk_binding::language_host_kind::angel)) {
        g_provider_registered = true;
        return SAO_OK;
    }
    sdk_binding::language_host_adapter_vtable adapter{};
    adapter.language = sdk_binding::language_host_kind::angel;
    adapter.available = provider_available;
    adapter.load_plugin = provider_load;
    adapter.unload_plugin = provider_unload;
    adapter.invoke = provider_invoke;
    adapter.dispatch = provider_dispatch;
    adapter.release_callback = provider_release_callback;
    const int32_t status = sdk_binding::sao_plugins_binding_register_language_host(&adapter);
    if (status == SAO_OK)
        g_provider_registered = true;
    return status;
}

} // namespace
#endif

extern "C" SAO_PLUGINS_API
    size_t SAO_PLUGINS_CALL sao_plugins_ashost_test_live_provider_plugin_count() {
#if defined(SAO_HAS_ANGELSCRIPT)
    return g_live_provider_plugins.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL sao_plugins_ashost_test_live_callback_count() {
#if defined(SAO_HAS_ANGELSCRIPT)
    return g_live_callbacks.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_register_sdk(asIScriptEngine* engine) {
#if defined(SAO_HAS_ANGELSCRIPT)
    if (engine == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    engine_execution_guard engine_lock;
    const int32_t provider_status = ensure_angel_provider();
    return provider_status == SAO_OK ? sdk_binding::sao_plugins_binding_angel_register_sdk(engine)
                                     : provider_status;
#else
    (void)engine;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_bind_ctx(asIScriptEngine* engine, void* ctx_handle, const char* module_name) {
#if defined(SAO_HAS_ANGELSCRIPT)
    if (engine == nullptr || module_name == nullptr || module_name[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        engine_execution_guard engine_lock;
        const int32_t provider_status = ensure_angel_provider();
        if (provider_status != SAO_OK)
            return provider_status;
        if (ctx_handle != nullptr) {
            const int32_t binding_status = sdk_binding::sao_plugins_binding_angel_bind_ctx(
                engine, reinterpret_cast<sdk_binding::plugin_context_ptr>(ctx_handle));
            if (binding_status != SAO_OK)
                return binding_status;
        }
        asIScriptModule* module = engine->GetModule(module_name, asGM_ONLY_IF_EXISTS);
        if (module == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        const int index = module->GetGlobalVarIndexByName("ctx");
        if (index < 0)
            return SAO_ERR_HANDLE_INVALID;

        int type_id = 0;
        if (module->GetGlobalVar(static_cast<asUINT>(index), nullptr, nullptr, &type_id, nullptr) <
                0 ||
            (type_id & asTYPEID_OBJHANDLE) == 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const asITypeInfo* type = engine->GetTypeInfoById(type_id);
        if (type == nullptr || std::strcmp(type->GetName(), "PluginContext") != 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        auto** slot =
            static_cast<void**>(module->GetAddressOfGlobalVar(static_cast<asUINT>(index)));
        if (slot == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        if (*slot != ctx_handle)
            *slot = ctx_handle;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    (void)engine;
    (void)ctx_handle;
    (void)module_name;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

} // namespace sao::plugins::angel_host
