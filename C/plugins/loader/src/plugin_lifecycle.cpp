#include "sao/plugins/loader/plugin_lifecycle.h"
#include "plugin_internal.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_deps.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sao::plugins::loader {
namespace {

struct adapter_record {
    host_adapter_vtable value{};
    bool present = false;
    bool retiring = false;
    size_t active_calls = 0;
};

struct subscriber_record {
    lifecycle_event_cb callback = nullptr;
    void* user_data = nullptr;
};

std::mutex g_lifecycle_mutex;
std::array<adapter_record, 6> g_adapters;
std::unordered_map<uint32_t, subscriber_record> g_subscribers;
std::atomic_uint32_t g_next_subscriber{1};

size_t language_index(engine_kind language) {
    return static_cast<size_t>(language);
}

void publish(plugin_handle_t plugin, lifecycle_event event, const char* message) noexcept {
    std::vector<subscriber_record> subscribers;
    {
        std::lock_guard lock(g_lifecycle_mutex);
        subscribers.reserve(g_subscribers.size());
        for (const auto& [token, subscriber] : g_subscribers)
            subscribers.push_back(subscriber);
    }
    for (const auto& subscriber : subscribers) {
        if (subscriber.callback == nullptr)
            continue;
        try {
            subscriber.callback(plugin, event, message ? message : "", subscriber.user_data);
        } catch (...) {
        }
    }
}

std::string dependency_id(std::string requirement) {
    if (requirement.find(':') != std::string::npos)
        return {};
    const auto stop = requirement.find_first_of("<>=!~; ");
    if (stop != std::string::npos)
        requirement.resize(stop);
    return requirement;
}

class adapter_lease {
  public:
    adapter_lease() = default;
    ~adapter_lease() {
        release();
    }

    adapter_lease(const adapter_lease&) = delete;
    adapter_lease& operator=(const adapter_lease&) = delete;

    bool acquire(engine_kind language) noexcept {
        try {
            const auto index = language_index(language);
            std::lock_guard lock(g_lifecycle_mutex);
            if (index >= g_adapters.size() || !g_adapters[index].present ||
                g_adapters[index].retiring) {
                return false;
            }
            index_ = index;
            value_ = g_adapters[index].value;
            ++g_adapters[index].active_calls;
            return true;
        } catch (...) {
            return false;
        }
    }

    const host_adapter_vtable& value() const noexcept {
        return value_;
    }

  private:
    void release() noexcept {
        if (index_ >= g_adapters.size())
            return;
        try {
            std::lock_guard lock(g_lifecycle_mutex);
            auto& record = g_adapters[index_];
            if (record.active_calls > 0)
                --record.active_calls;
        } catch (...) {
        }
        index_ = g_adapters.size();
    }

    size_t index_ = g_adapters.size();
    host_adapter_vtable value_{};
};

bool language_has_owned_context(engine_kind language) noexcept {
    try {
        auto& registry = registry_storage();
        std::shared_lock registry_lock(registry.mutex);
        for (const auto& [_, plugin] : registry.plugins) {
            std::lock_guard plugin_lock(plugin->mutex);
            if (plugin->manifest.language == language && plugin->native_module == nullptr &&
                plugin->context != nullptr && !plugin->host_adapter_unloaded) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return true;
    }
}

uint32_t expected_native_abi(const plugin_manifest& manifest) {
    if (manifest.abi_version != 0)
        return manifest.abi_version;
    if (manifest.native_abi == "sao_plugin_v2")
        return 2;
    return 1;
}

int32_t validate_native_descriptor(const plugin_manifest& manifest,
                                   const native_plugin_descriptor& descriptor) {
    constexpr size_t kBaseDescriptorSize =
        offsetof(native_plugin_descriptor, entity_provider_count);
    const bool has_entity_providers = descriptor.struct_size >= sizeof(native_plugin_descriptor);
    if (descriptor.struct_size < kBaseDescriptorSize ||
        descriptor.abi_version != expected_native_abi(manifest) ||
        descriptor.abi_version > static_cast<uint32_t>(SAO_PLUGINS_ABI_VERSION)) {
        return SAO_PLUGINS_ERR_ABI_MISMATCH;
    }
    if (descriptor.plugin_version == nullptr || manifest.version != descriptor.plugin_version) {
        return SAO_PLUGINS_ERR_VERSION_MISMATCH;
    }
    if (descriptor.capability_count > 0 && descriptor.capabilities == nullptr) {
        return SAO_PLUGINS_ERR_CAPABILITY_MISMATCH;
    }
    if (has_entity_providers && descriptor.entity_provider_count > 0 &&
        descriptor.entity_providers == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const uint32_t provider_count = has_entity_providers ? descriptor.entity_provider_count : 0;
    if (provider_count > kMaximumEntityProvidersPerContext) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    uintptr_t provider_address = reinterpret_cast<uintptr_t>(descriptor.entity_providers);
    for (uint32_t index = 0; index < provider_count; ++index) {
        const auto* provider =
            reinterpret_cast<const native_entity_provider_descriptor*>(provider_address);
        if (provider->struct_size < kNativeEntityProviderDescriptorRequiredPrefixSize)
            return SAO_PLUGINS_ERR_ABI_MISMATCH;
        native_entity_provider_descriptor current{};
        std::memcpy(&current, provider,
                    (std::min)(static_cast<size_t>(provider->struct_size), sizeof(current)));
        if (current.provider_id_utf8 == nullptr || current.snapshot == nullptr ||
            current.action_handler == nullptr) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (current.struct_size > (std::numeric_limits<uintptr_t>::max)() - provider_address) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        provider_address += current.struct_size;
    }
    for (const auto& required : manifest.capabilities) {
        bool found = false;
        for (uint32_t index = 0; index < descriptor.capability_count; ++index) {
            const auto* provided = descriptor.capabilities[index];
            if (provided != nullptr && required.id == provided) {
                found = true;
                break;
            }
        }
        if (!found)
            return SAO_PLUGINS_ERR_CAPABILITY_MISMATCH;
    }
    return SAO_OK;
}

int32_t call_native_query_cpp(native_plugin_query_fn callback,
                              native_plugin_descriptor* descriptor) noexcept {
    try {
        return callback(descriptor);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_native_query(native_plugin_query_fn callback,
                          native_plugin_descriptor* descriptor) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_native_query_cpp(callback, descriptor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_native_query_cpp(callback, descriptor);
#endif
}

int32_t validate_native_descriptor_cpp(const plugin_manifest& manifest,
                                       const native_plugin_descriptor& descriptor) noexcept {
    try {
        return validate_native_descriptor(manifest, descriptor);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t validate_native_descriptor_guarded(const plugin_manifest& manifest,
                                           const native_plugin_descriptor& descriptor) noexcept {
#if defined(_MSC_VER)
    __try {
        return validate_native_descriptor_cpp(manifest, descriptor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return validate_native_descriptor_cpp(manifest, descriptor);
#endif
}

int32_t call_native_on_load_cpp(native_on_load_fn callback, plugin_context_t* context) noexcept {
    try {
        return callback(context);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_native_on_load(native_on_load_fn callback, plugin_context_t* context) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_native_on_load_cpp(callback, context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_native_on_load_cpp(callback, context);
#endif
}

int32_t call_native_simple_cpp(native_simple_fn callback) noexcept {
    try {
        return callback();
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_native_simple(native_simple_fn callback) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_native_simple_cpp(callback);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_native_simple_cpp(callback);
#endif
}

int32_t clear_owned_runtime(plugin_handle_t plugin) noexcept {
    try {
        plugin_context_t* context = nullptr;
        HMODULE module = nullptr;
        {
            std::lock_guard lock(plugin->mutex);
            context = plugin->context;
            module = plugin->native_module;
        }
        if (context != nullptr) {
            const int32_t status = plugin_context_destroy(context);
            if (status != SAO_OK)
                return status;
        }
        if (module != nullptr && FreeLibrary(module) == FALSE) {
            return SAO_ERR_OS_CALL_FAILED;
        }
        {
            std::lock_guard lock(plugin->mutex);
            plugin->native_module = nullptr;
            plugin->native_on_load = nullptr;
            plugin->native_on_enable = nullptr;
            plugin->native_on_disable = nullptr;
            plugin->native_on_unload = nullptr;
            plugin->unload_hook_completed = false;
            plugin->host_adapter_unloaded = false;
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t close_owned_dependencies(plugin_handle_t plugin) noexcept {
    try {
        deps_session_t dependency_session = nullptr;
        {
            std::lock_guard lock(plugin->mutex);
            dependency_session = plugin->dependency_session;
        }
        if (dependency_session == nullptr)
            return SAO_OK;
        const int32_t status = sao_plugins_deps_session_close(dependency_session);
        if (status != SAO_OK)
            return status;
        std::lock_guard lock(plugin->mutex);
        if (plugin->dependency_session == dependency_session)
            plugin->dependency_session = nullptr;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t load_native(plugin_handle_t plugin, const plugin_manifest& manifest) {
    {
        std::lock_guard lock(plugin->mutex);
        if (plugin->native_module != nullptr || plugin->context != nullptr) {
            return SAO_PLUGINS_ERR_BUSY;
        }
    }
    const auto plugin_root = std::filesystem::u8path(manifest.source_path);
    const auto dll_path = plugin_root / std::filesystem::u8path(manifest.native_entry);
    std::filesystem::path resolved_dll_path;
    const int32_t path_status =
        resolve_contained_existing_path(plugin_root, dll_path, resolved_dll_path);
    if (path_status != SAO_OK)
        return path_status;
    auto module =
        LoadLibraryExW(resolved_dll_path.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    const auto query = reinterpret_cast<native_plugin_query_fn>(
        GetProcAddress(module, SAO_PLUGIN_NATIVE_QUERY_SYMBOL));
    const auto on_load = reinterpret_cast<native_on_load_fn>(
        GetProcAddress(module, SAO_PLUGIN_NATIVE_ON_LOAD_SYMBOL));
    const auto on_enable = reinterpret_cast<native_simple_fn>(
        GetProcAddress(module, SAO_PLUGIN_NATIVE_ON_ENABLE_SYMBOL));
    const auto on_disable = reinterpret_cast<native_simple_fn>(
        GetProcAddress(module, SAO_PLUGIN_NATIVE_ON_DISABLE_SYMBOL));
    const auto on_unload = reinterpret_cast<native_simple_fn>(
        GetProcAddress(module, SAO_PLUGIN_NATIVE_ON_UNLOAD_SYMBOL));
    if (query == nullptr || on_load == nullptr || on_unload == nullptr ||
        (manifest.enabled && (on_enable == nullptr || on_disable == nullptr))) {
        FreeLibrary(module);
        return SAO_ERR_HANDLE_INVALID;
    }
    native_plugin_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    status = call_native_query(query, &descriptor);
    if (status == SAO_OK) {
        status = validate_native_descriptor_guarded(manifest, descriptor);
    }
    if (status != SAO_OK) {
        FreeLibrary(module);
        return status;
    }
    auto* context = sao_plugins_ctx_create(plugin);
    if (context == nullptr) {
        FreeLibrary(module);
        return SAO_ERR_OS_CALL_FAILED;
    }
    {
        std::lock_guard lock(plugin->mutex);
        plugin->native_module = module;
        plugin->native_on_load = on_load;
        plugin->native_on_enable = on_enable;
        plugin->native_on_disable = on_disable;
        plugin->native_on_unload = on_unload;
        plugin->context = context;
        plugin->unload_hook_completed = false;
        plugin->host_adapter_unloaded = false;
    }
    const bool has_entity_providers = descriptor.struct_size >= sizeof(native_plugin_descriptor);
    const auto* entity_providers = has_entity_providers ? descriptor.entity_providers : nullptr;
    const uint32_t entity_provider_count =
        has_entity_providers ? descriptor.entity_provider_count : 0;
    bool on_load_entered = false;
    try {
        status = plugin_context_register_entity_providers(context, entity_providers,
                                                          entity_provider_count);
        if (status == SAO_OK) {
            on_load_entered = true;
            status = call_native_on_load(on_load, context);
        }
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK) {
        int32_t rollback_status = SAO_OK;
        if (on_load_entered) {
            rollback_status = call_native_simple(on_unload);
            if (rollback_status == SAO_OK) {
                std::lock_guard lock(plugin->mutex);
                plugin->unload_hook_completed = true;
            }
        }
        if (rollback_status != SAO_OK)
            return rollback_status;
        const int32_t cleanup_status = plugin_context_release_resources(context);
        if (cleanup_status != SAO_OK)
            return cleanup_status;
        const int32_t clear_status = clear_owned_runtime(plugin);
        return clear_status == SAO_OK ? status : clear_status;
    }
    return SAO_OK;
}

int32_t load_single(plugin_handle_t plugin, std::unordered_set<plugin_handle_t>& visiting) {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    (void)retained;
    {
        std::lock_guard lock(plugin->mutex);
        if (plugin->state == lifecycle_state::loaded_active ||
            plugin->state == lifecycle_state::loaded_disabled)
            return SAO_OK;
        if (!visiting.insert(plugin).second) {
            plugin->state = lifecycle_state::failed;
            plugin->last_error = "dependency cycle";
            return SAO_PLUGINS_ERR_DEPENDENCY_CYCLE;
        }
        if (plugin->native_module != nullptr || plugin->context != nullptr ||
            plugin->dependency_session != nullptr ||
            (plugin->state != lifecycle_state::discovered &&
             plugin->state != lifecycle_state::unloaded &&
             plugin->state != lifecycle_state::failed)) {
            visiting.erase(plugin);
            return SAO_PLUGINS_ERR_BUSY;
        }
        plugin->state = lifecycle_state::resolving_deps;
    }
    const auto manifest = manifest_snapshot(plugin);
    for (const auto& requirement : manifest.requires_list) {
        const auto id = dependency_id(requirement);
        if (id.empty())
            continue;
        auto dependency = sao_plugins_registry_find(sao_plugins_registry_instance(), id.c_str());
        if (dependency == nullptr) {
            std::lock_guard lock(plugin->mutex);
            plugin->state = lifecycle_state::failed;
            plugin->last_error = "dependency missing: " + id;
            visiting.erase(plugin);
            return SAO_PLUGINS_ERR_DEPENDENCY_MISSING;
        }
        const auto status = load_single(dependency, visiting);
        if (status != SAO_OK) {
            std::lock_guard lock(plugin->mutex);
            plugin->state = lifecycle_state::failed;
            plugin->last_error = "dependency failed: " + id;
            visiting.erase(plugin);
            return status;
        }
    }
    if (validate_manifest(manifest) != SAO_OK) {
        {
            std::lock_guard lock(plugin->mutex);
            plugin->state = lifecycle_state::failed;
            plugin->last_error = "manifest validation failed";
        }
        visiting.erase(plugin);
        publish(plugin, lifecycle_event::validate_failed, "manifest validation failed");
        return SAO_ERR_INVALID_ARGUMENT;
    }
    deps_bootstrap_record dependencies;
    {
        std::lock_guard lock(plugin->mutex);
        plugin->state = lifecycle_state::bootstrapping;
    }
    auto status = sao_plugins_deps_ensure(std::filesystem::u8path(manifest.source_path).c_str(),
                                          false, &dependencies);
    if (status != SAO_OK) {
        std::lock_guard lock(plugin->mutex);
        plugin->state = lifecycle_state::failed;
        plugin->last_error = "dependency bootstrap failed";
        visiting.erase(plugin);
        return status;
    }
    if (!dependencies.added_paths.empty()) {
        deps_session_t dependency_session = nullptr;
        status = sao_plugins_deps_attach(manifest.plugin_id.c_str(),
                                         std::filesystem::u8path(manifest.source_path).c_str(),
                                         &dependencies, &dependency_session);
        if (dependency_session != nullptr) {
            std::lock_guard lock(plugin->mutex);
            plugin->dependency_session = dependency_session;
        }
        if (status != SAO_OK) {
            std::lock_guard lock(plugin->mutex);
            plugin->state = lifecycle_state::failed;
            plugin->last_error = "dependency attach failed";
            visiting.erase(plugin);
            return status;
        }
    }
    {
        std::lock_guard lock(plugin->mutex);
        plugin->state = lifecycle_state::loading;
        plugin->stop_requested.store(false);
    }
    publish(plugin, lifecycle_event::load_started, "load started");
    adapter_lease adapter;
    bool adapter_loaded = false;
    bool adapter_on_load_entered = false;
    bool rollback_adapter_unloaded = false;
    if (!manifest.native_entry.empty()) {
        status = load_native(plugin, manifest);
    } else {
        auto* context = sao_plugins_ctx_create(plugin);
        if (context == nullptr) {
            status = SAO_ERR_OS_CALL_FAILED;
        } else {
            std::lock_guard lock(plugin->mutex);
            plugin->context = context;
            plugin->unload_hook_completed = false;
            plugin->host_adapter_unloaded = false;
        }
        if (status == SAO_OK &&
            (!adapter.acquire(manifest.language) || adapter.value().load_plugin == nullptr ||
             adapter.value().call_on_load == nullptr || adapter.value().unload_plugin == nullptr)) {
            status = SAO_PLUGINS_ERR_UNSUPPORTED;
        } else if (status == SAO_OK) {
            try {
                status =
                    adapter.value().load_plugin(plugin, &manifest, adapter.value().host_user_data);
                adapter_loaded = status == SAO_OK;
                if (status == SAO_OK) {
                    adapter_on_load_entered = true;
                    status = adapter.value().call_on_load(plugin, adapter.value().host_user_data);
                }
            } catch (...) {
                status = SAO_ERR_OS_CALL_FAILED;
            }
        }
    }
    visiting.erase(plugin);
    if (status != SAO_OK) {
        if (manifest.native_entry.empty()) {
            plugin_context_t* context = nullptr;
            {
                std::lock_guard lock(plugin->mutex);
                context = plugin->context;
            }
            int32_t rollback_status = SAO_OK;
            bool allow_unload = true;
            if (adapter_loaded && adapter_on_load_entered &&
                adapter.value().call_on_unload != nullptr) {
                try {
                    rollback_status = adapter.value().call_on_unload(
                        plugin, &allow_unload, adapter.value().host_user_data);
                } catch (...) {
                    rollback_status = SAO_ERR_OS_CALL_FAILED;
                }
            }
            if (rollback_status == SAO_OK && allow_unload) {
                {
                    std::lock_guard lock(plugin->mutex);
                    plugin->unload_hook_completed = true;
                }
                const int32_t resource_status = plugin_context_release_resources(context);
                if (resource_status != SAO_OK) {
                    status = resource_status;
                } else if (adapter_loaded) {
                    try {
                        const int32_t unload_status =
                            adapter.value().unload_plugin(plugin, adapter.value().host_user_data);
                        if (unload_status == SAO_OK) {
                            std::lock_guard lock(plugin->mutex);
                            plugin->host_adapter_unloaded = true;
                            rollback_adapter_unloaded = true;
                        } else {
                            status = unload_status;
                            sao_plugins_isolation_record_failure(plugin,
                                                                 "host rollback unload failed");
                        }
                    } catch (...) {
                        status = SAO_ERR_OS_CALL_FAILED;
                        sao_plugins_isolation_record_failure(
                            plugin, "host rollback unload crossed exception boundary");
                    }
                }
            } else {
                status = rollback_status == SAO_OK ? SAO_PLUGINS_ERR_BUSY : rollback_status;
            }
            if (rollback_status == SAO_OK && allow_unload &&
                (!adapter_loaded || rollback_adapter_unloaded)) {
                const int32_t cleanup_status = clear_owned_runtime(plugin);
                if (cleanup_status != SAO_OK)
                    status = cleanup_status;
            }
        }
        bool owns_runtime = false;
        {
            std::lock_guard lock(plugin->mutex);
            owns_runtime = plugin->context != nullptr || plugin->native_module != nullptr;
        }
        if (!owns_runtime) {
            const int32_t dependency_status = close_owned_dependencies(plugin);
            if (dependency_status != SAO_OK)
                status = dependency_status;
        }
        {
            std::lock_guard lock(plugin->mutex);
            plugin->state = lifecycle_state::failed;
            plugin->last_error = "plugin load failed";
        }
        publish(plugin, lifecycle_event::load_failed, "plugin load failed");
        return status;
    }
    {
        std::lock_guard lock(plugin->mutex);
        plugin->state = lifecycle_state::loaded_disabled;
        plugin->last_error.clear();
    }
    publish(plugin, lifecycle_event::loaded, "loaded");
    return manifest.enabled ? sao_plugins_lifecycle_enable(plugin) : SAO_OK;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_lifecycle_register_host_adapter(
    engine_kind language, const host_adapter_vtable* vtable) {
    const auto index = language_index(language);
    if (vtable == nullptr || index == 0 || index >= g_adapters.size() ||
        vtable->load_plugin == nullptr || vtable->call_on_load == nullptr ||
        vtable->unload_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(g_lifecycle_mutex);
        auto& record = g_adapters[index];
        if (record.present)
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        if (record.retiring || record.active_calls != 0) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        record.value = *vtable;
        record.present = true;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_unregister_host_adapter(engine_kind language) {
    const auto index = language_index(language);
    if (index == 0 || index >= g_adapters.size()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        {
            std::lock_guard lock(g_lifecycle_mutex);
            auto& record = g_adapters[index];
            if (!record.present)
                return SAO_ERR_HANDLE_INVALID;
            if (record.retiring || record.active_calls != 0) {
                return SAO_PLUGINS_ERR_BUSY;
            }
            record.present = false;
            record.retiring = true;
        }
        if (language_has_owned_context(language)) {
            std::lock_guard lock(g_lifecycle_mutex);
            auto& record = g_adapters[index];
            record.present = true;
            record.retiring = false;
            return SAO_PLUGINS_ERR_BUSY;
        }
        {
            std::lock_guard lock(g_lifecycle_mutex);
            auto& record = g_adapters[index];
            record.value = {};
            record.retiring = false;
        }
        return SAO_OK;
    } catch (...) {
        try {
            std::lock_guard lock(g_lifecycle_mutex);
            auto& record = g_adapters[index];
            if (record.retiring) {
                record.present = true;
                record.retiring = false;
            }
        } catch (...) {
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_get_context(plugin_handle_t plugin, plugin_context_t** out_context) {
    if (out_context == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_context = nullptr;
    try {
        const auto retained = retain_plugin(plugin);
        if (retained == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(retained->mutex);
        if (retained->context == nullptr)
            return SAO_ERR_NOT_INITIALIZED;
        *out_context = retained->context;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_load(plugin_handle_t plugin) {
    try {
        std::unordered_set<plugin_handle_t> visiting;
        return load_single(plugin, visiting);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_unload(plugin_handle_t plugin) {
    try {
        const auto retained = retain_plugin(plugin);
        if (retained == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        (void)retained;
        const auto manifest = manifest_snapshot(plugin);
        lifecycle_state previous_state = lifecycle_state::unknown;
        plugin_context_t* context = nullptr;
        HMODULE native_module = nullptr;
        native_simple_fn native_on_disable = nullptr;
        native_simple_fn native_on_unload = nullptr;
        bool unload_hook_completed = false;
        bool host_adapter_unloaded = false;
        deps_session_t dependency_session = nullptr;
        {
            std::lock_guard lock(plugin->mutex);
            if (plugin->state == lifecycle_state::discovered ||
                plugin->state == lifecycle_state::unloaded) {
                return SAO_OK;
            }
            const bool resident_failed =
                plugin->state == lifecycle_state::failed &&
                (plugin->native_module != nullptr || plugin->context != nullptr ||
                 plugin->dependency_session != nullptr);
            if (plugin->state != lifecycle_state::loaded_active &&
                plugin->state != lifecycle_state::loaded_disabled && !resident_failed) {
                return SAO_PLUGINS_ERR_BUSY;
            }
            context = plugin->context;
            native_module = plugin->native_module;
            native_on_disable = plugin->native_on_disable;
            native_on_unload = plugin->native_on_unload;
            unload_hook_completed = plugin->unload_hook_completed;
            host_adapter_unloaded = plugin->host_adapter_unloaded;
            dependency_session = plugin->dependency_session;
            if (plugin_context_event_is_current_thread(context) ||
                plugin_context_platform_is_current_thread(context)) {
                return SAO_PLUGINS_ERR_BUSY;
            }
            previous_state = plugin->state;
            plugin->state = lifecycle_state::unloading;
        }
        if (context == nullptr && native_module == nullptr && dependency_session != nullptr) {
            publish(plugin, lifecycle_event::unload_started, "unload started");
            const int32_t dependency_status = close_owned_dependencies(plugin);
            {
                std::lock_guard lock(plugin->mutex);
                plugin->state = dependency_status == SAO_OK ? lifecycle_state::unloaded
                                                            : lifecycle_state::failed;
                plugin->last_error = dependency_status == SAO_OK
                                         ? std::string{}
                                         : std::string{"dependency cleanup failed"};
            }
            publish(plugin,
                    dependency_status == SAO_OK ? lifecycle_event::unloaded
                                                : lifecycle_event::unload_failed,
                    dependency_status == SAO_OK ? "unloaded" : "dependency cleanup failed");
            return dependency_status;
        }
        if (plugin_context_entity_provider_is_current_thread(context)) {
            std::lock_guard lock(plugin->mutex);
            plugin->state = previous_state;
            return SAO_PLUGINS_ERR_BUSY;
        }
        plugin->stop_requested.store(true);
        plugin_context_request_stop(context);
        publish(plugin, lifecycle_event::unload_started, "unload started");
        const int32_t platform_quiesce_status = plugin_context_quiesce_platform(context);
        if (platform_quiesce_status != SAO_OK) {
            {
                std::lock_guard lock(plugin->mutex);
                plugin->state = lifecycle_state::failed;
                plugin->last_error = "platform capability rundown failed";
            }
            publish(plugin, lifecycle_event::unload_failed, "platform capability rundown failed");
            return platform_quiesce_status;
        }
        const int32_t quiesce_status = plugin_context_quiesce_entity_providers(context);
        if (quiesce_status != SAO_OK) {
            {
                std::lock_guard lock(plugin->mutex);
                plugin->state = lifecycle_state::failed;
                plugin->last_error = "entity provider rundown failed";
            }
            publish(plugin, lifecycle_event::unload_failed, "entity provider rundown failed");
            return quiesce_status;
        }

        int32_t status = SAO_OK;
        bool allow_unload = true;
        bool disable_completed = false;
        adapter_lease adapter;
        try {
            if (native_module != nullptr) {
                if (previous_state == lifecycle_state::loaded_active &&
                    native_on_disable != nullptr) {
                    status = call_native_simple(native_on_disable);
                    disable_completed = status == SAO_OK;
                }
                if (status == SAO_OK && !unload_hook_completed && native_on_unload != nullptr) {
                    status = call_native_simple(native_on_unload);
                    unload_hook_completed = status == SAO_OK;
                }
            } else if (!host_adapter_unloaded) {
                if (!adapter.acquire(manifest.language)) {
                    status = SAO_PLUGINS_ERR_UNSUPPORTED;
                } else {
                    const auto& value = adapter.value();
                    if (previous_state == lifecycle_state::loaded_active &&
                        value.call_on_disable != nullptr) {
                        status = value.call_on_disable(plugin, value.host_user_data);
                        disable_completed = status == SAO_OK;
                    }
                    if (status == SAO_OK && !unload_hook_completed &&
                        value.call_on_unload != nullptr) {
                        status = value.call_on_unload(plugin, &allow_unload, value.host_user_data);
                        unload_hook_completed = status == SAO_OK && allow_unload;
                    } else if (status == SAO_OK && !unload_hook_completed) {
                        unload_hook_completed = true;
                    }
                }
            }
        } catch (...) {
            status = SAO_ERR_OS_CALL_FAILED;
        }
        if (unload_hook_completed) {
            std::lock_guard lock(plugin->mutex);
            plugin->unload_hook_completed = true;
        }
        if (status != SAO_OK || !allow_unload) {
            const bool blocked = status == SAO_OK && !allow_unload;
            if (blocked) {
                plugin->stop_requested.store(false);
                plugin_context_clear_stop(context);
            }
            {
                std::lock_guard lock(plugin->mutex);
                plugin->state = blocked && previous_state != lifecycle_state::failed
                                    ? lifecycle_state::loaded_disabled
                                    : lifecycle_state::failed;
                if (disable_completed)
                    plugin->manifest.enabled = false;
                plugin->last_error = blocked ? "unload blocked" : "unload failed";
            }
            publish(plugin,
                    blocked ? lifecycle_event::unload_blocked : lifecycle_event::unload_failed,
                    blocked ? "unload blocked" : "unload failed");
            return blocked ? SAO_PLUGINS_ERR_BUSY : status;
        }

        status = plugin_context_release_resources(context);
        if (status == SAO_OK && native_module == nullptr && !host_adapter_unloaded) {
            try {
                status = adapter.value().unload_plugin(plugin, adapter.value().host_user_data);
            } catch (...) {
                status = SAO_ERR_OS_CALL_FAILED;
            }
            if (status == SAO_OK) {
                std::lock_guard lock(plugin->mutex);
                plugin->host_adapter_unloaded = true;
            }
        }
        if (status == SAO_OK)
            status = clear_owned_runtime(plugin);
        if (status == SAO_OK)
            status = close_owned_dependencies(plugin);
        if (status != SAO_OK) {
            {
                std::lock_guard lock(plugin->mutex);
                plugin->state = lifecycle_state::failed;
                plugin->last_error = "runtime cleanup failed";
            }
            publish(plugin, lifecycle_event::unload_failed, "runtime cleanup failed");
            return status;
        }
        {
            std::lock_guard lock(plugin->mutex);
            plugin->state = lifecycle_state::unloaded;
            plugin->last_error.clear();
        }
        publish(plugin, lifecycle_event::unloaded, "unloaded");
        return SAO_OK;
    } catch (...) {
        try {
            const auto retained = retain_plugin(plugin);
            if (retained != nullptr) {
                std::lock_guard lock(retained->mutex);
                if (retained->state == lifecycle_state::unloading) {
                    retained->state = lifecycle_state::failed;
                    retained->last_error = "unload failed";
                }
            }
        } catch (...) {
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_enable(plugin_handle_t plugin) {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    (void)retained;
    auto state = sao_plugins_lifecycle_state(plugin);
    if (state == lifecycle_state::discovered || state == lifecycle_state::unloaded ||
        state == lifecycle_state::failed) {
        const auto status = sao_plugins_lifecycle_load(plugin);
        if (status != SAO_OK)
            return status;
        state = sao_plugins_lifecycle_state(plugin);
    }
    plugin_context_t* context = nullptr;
    {
        std::lock_guard lock(plugin->mutex);
        if (plugin->state == lifecycle_state::loaded_active)
            return SAO_OK;
        if (plugin->state != lifecycle_state::loaded_disabled) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        context = plugin->context;
        plugin->state = lifecycle_state::enabling;
    }
    const auto manifest = manifest_snapshot(plugin);
    int32_t status = SAO_OK;
    bool enable_entered = false;
    adapter_lease adapter;
    try {
        if (plugin->native_module != nullptr) {
            if (plugin->native_on_enable != nullptr) {
                enable_entered = true;
                status = call_native_simple(plugin->native_on_enable);
            } else {
                status = SAO_PLUGINS_ERR_UNSUPPORTED;
            }
        } else {
            if (adapter.acquire(manifest.language) && adapter.value().call_on_enable != nullptr) {
                enable_entered = true;
                status = adapter.value().call_on_enable(plugin, adapter.value().host_user_data);
            } else {
                status = SAO_PLUGINS_ERR_UNSUPPORTED;
            }
        }
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status == SAO_OK) {
        const int32_t activation_status = plugin_context_resume_entity_providers(context);
        if (activation_status != SAO_OK) {
            int32_t rollback_status = SAO_OK;
            try {
                if (plugin->native_module != nullptr) {
                    rollback_status = plugin->native_on_disable != nullptr
                                          ? call_native_simple(plugin->native_on_disable)
                                          : SAO_PLUGINS_ERR_UNSUPPORTED;
                } else {
                    rollback_status = adapter.value().call_on_disable != nullptr
                                          ? adapter.value().call_on_disable(
                                                plugin, adapter.value().host_user_data)
                                          : SAO_PLUGINS_ERR_UNSUPPORTED;
                }
            } catch (...) {
                rollback_status = SAO_ERR_OS_CALL_FAILED;
            }
            {
                std::lock_guard lock(plugin->mutex);
                plugin->state = rollback_status == SAO_OK ? lifecycle_state::loaded_disabled
                                                          : lifecycle_state::failed;
                plugin->last_error = "entity provider activation failed";
            }
            return rollback_status == SAO_OK ? activation_status : rollback_status;
        }
        {
            std::lock_guard lock(plugin->mutex);
            plugin->manifest.enabled = true;
            plugin->state = lifecycle_state::loaded_active;
            plugin->last_error.clear();
        }
        publish(plugin, lifecycle_event::enabled, "enabled");
    } else {
        int32_t rollback_status = SAO_OK;
        if (enable_entered) {
            try {
                if (plugin->native_module != nullptr) {
                    rollback_status = plugin->native_on_disable != nullptr
                                          ? call_native_simple(plugin->native_on_disable)
                                          : SAO_PLUGINS_ERR_UNSUPPORTED;
                } else {
                    rollback_status = adapter.value().call_on_disable != nullptr
                                          ? adapter.value().call_on_disable(
                                                plugin, adapter.value().host_user_data)
                                          : SAO_PLUGINS_ERR_UNSUPPORTED;
                }
            } catch (...) {
                rollback_status = SAO_ERR_OS_CALL_FAILED;
            }
        }
        {
            std::lock_guard lock(plugin->mutex);
            plugin->state = rollback_status == SAO_OK ? lifecycle_state::loaded_disabled
                                                      : lifecycle_state::failed;
            plugin->last_error = "enable failed";
        }
        if (rollback_status != SAO_OK)
            return rollback_status;
    }
    return status;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_disable(plugin_handle_t plugin) {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    (void)retained;
    plugin_context_t* context = nullptr;
    {
        std::lock_guard lock(plugin->mutex);
        if (plugin->state == lifecycle_state::loaded_disabled)
            return SAO_OK;
        if (plugin->state != lifecycle_state::loaded_active) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        context = plugin->context;
        plugin->state = lifecycle_state::disabling;
    }
    if (plugin_context_entity_provider_is_current_thread(context)) {
        std::lock_guard lock(plugin->mutex);
        plugin->state = lifecycle_state::loaded_active;
        return SAO_PLUGINS_ERR_BUSY;
    }
    const auto manifest = manifest_snapshot(plugin);
    const int32_t quiesce_status = plugin_context_quiesce_entity_providers(context);
    if (quiesce_status != SAO_OK) {
        {
            std::lock_guard lock(plugin->mutex);
            plugin->state = lifecycle_state::failed;
            plugin->last_error = "entity provider rundown failed";
        }
        publish(plugin, lifecycle_event::disable_failed, "entity provider rundown failed");
        return quiesce_status;
    }
    int32_t status = SAO_OK;
    adapter_lease adapter;
    try {
        if (plugin->native_module != nullptr) {
            status = plugin->native_on_disable != nullptr
                         ? call_native_simple(plugin->native_on_disable)
                         : SAO_PLUGINS_ERR_UNSUPPORTED;
        } else {
            status =
                adapter.acquire(manifest.language) && adapter.value().call_on_disable != nullptr
                    ? adapter.value().call_on_disable(plugin, adapter.value().host_user_data)
                    : SAO_PLUGINS_ERR_UNSUPPORTED;
        }
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status == SAO_OK) {
        {
            std::lock_guard lock(plugin->mutex);
            plugin->manifest.enabled = false;
            plugin->state = lifecycle_state::loaded_disabled;
            plugin->last_error.clear();
        }
        publish(plugin, lifecycle_event::disabled, "disabled");
        return SAO_OK;
    }
    {
        std::lock_guard lock(plugin->mutex);
        plugin->state = lifecycle_state::failed;
        plugin->last_error = "disable failed";
    }
    publish(plugin, lifecycle_event::disable_failed, "disable failed");
    return status;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_reload(plugin_handle_t plugin) {
    const auto status = sao_plugins_lifecycle_unload(plugin);
    return status == SAO_OK ? sao_plugins_lifecycle_load(plugin) : status;
}

extern "C" SAO_PLUGINS_API lifecycle_state SAO_PLUGINS_CALL
sao_plugins_lifecycle_state(plugin_handle_t plugin) {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr)
        return lifecycle_state::unknown;
    std::lock_guard lock(retained->mutex);
    return retained->state;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_lifecycle_topo_sort(
    plugin_handle_t* handles, size_t count, plugin_handle_t* out_sorted_handles) {
    if ((count > 0 && (handles == nullptr || out_sorted_handles == nullptr)))
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::unordered_map<std::string, plugin_handle_t> by_id;
        std::vector<std::shared_ptr<plugin_handle_s>> retained_plugins;
        retained_plugins.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            auto retained = retain_plugin(handles[index]);
            if (retained == nullptr)
                return SAO_ERR_HANDLE_INVALID;
            retained_plugins.push_back(std::move(retained));
            by_id.emplace(manifest_snapshot(handles[index]).plugin_id, handles[index]);
        }
        enum class mark : uint8_t { none, visiting, done };
        std::unordered_map<plugin_handle_t, mark> marks;
        std::vector<plugin_handle_t> result;
        std::function<int32_t(plugin_handle_t)> visit = [&](plugin_handle_t plugin) -> int32_t {
            if (marks[plugin] == mark::done)
                return SAO_OK;
            if (marks[plugin] == mark::visiting)
                return SAO_PLUGINS_ERR_DEPENDENCY_CYCLE;
            marks[plugin] = mark::visiting;
            for (const auto& requirement : manifest_snapshot(plugin).requires_list) {
                const auto id = dependency_id(requirement);
                if (id.empty())
                    continue;
                const auto iterator = by_id.find(id);
                if (iterator == by_id.end())
                    return SAO_PLUGINS_ERR_DEPENDENCY_MISSING;
                const auto status = visit(iterator->second);
                if (status != SAO_OK)
                    return status;
            }
            marks[plugin] = mark::done;
            result.push_back(plugin);
            return SAO_OK;
        };
        for (size_t index = 0; index < count; ++index) {
            const auto status = visit(handles[index]);
            if (status != SAO_OK)
                return status;
        }
        std::copy(result.begin(), result.end(), out_sorted_handles);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_subscribe(lifecycle_event_cb callback, void* user_data, uint32_t* out_token) {
    if (callback == nullptr || out_token == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    const auto token = g_next_subscriber.fetch_add(1);
    std::lock_guard lock(g_lifecycle_mutex);
    g_subscribers.emplace(token, subscriber_record{callback, user_data});
    *out_token = token;
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_unsubscribe(uint32_t token) {
    if (token == 0)
        return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_lifecycle_mutex);
    return g_subscribers.erase(token) == 1 ? SAO_OK : SAO_ERR_HANDLE_INVALID;
}

} // namespace sao::plugins::loader
