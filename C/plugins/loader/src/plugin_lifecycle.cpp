#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_deps.h"
#include "plugin_internal.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
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
        for (const auto& [token, subscriber] : g_subscribers) subscribers.push_back(subscriber);
    }
    for (const auto& subscriber : subscribers) {
        if (subscriber.callback == nullptr) continue;
        try {
            subscriber.callback(plugin, event, message ? message : "", subscriber.user_data);
        } catch (...) {
        }
    }
}

std::string dependency_id(std::string requirement) {
    if (requirement.find(':') != std::string::npos) return {};
    const auto stop = requirement.find_first_of("<>=!~; ");
    if (stop != std::string::npos) requirement.resize(stop);
    return requirement;
}

bool adapter_for(engine_kind language, host_adapter_vtable& output) {
    const auto index = language_index(language);
    std::lock_guard lock(g_lifecycle_mutex);
    if (index >= g_adapters.size() || !g_adapters[index].present) return false;
    output = g_adapters[index].value;
    return true;
}

uint32_t expected_native_abi(const plugin_manifest& manifest) {
    if (manifest.abi_version != 0) return manifest.abi_version;
    if (manifest.native_abi == "sao_plugin_v2") return 2;
    return 1;
}

int32_t validate_native_descriptor(const plugin_manifest& manifest,
                                   const native_plugin_descriptor& descriptor) {
    if (descriptor.struct_size < sizeof(native_plugin_descriptor) ||
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
    for (const auto& required : manifest.capabilities) {
        bool found = false;
        for (uint32_t index = 0; index < descriptor.capability_count; ++index) {
            const auto* provided = descriptor.capabilities[index];
            if (provided != nullptr && required.id == provided) { found = true; break; }
        }
        if (!found) return SAO_PLUGINS_ERR_CAPABILITY_MISMATCH;
    }
    return SAO_OK;
}

void clear_native(plugin_handle_t plugin) noexcept {
    if (plugin->context != nullptr) {
        sao_plugins_ctx_destroy(plugin->context);
        plugin->context = nullptr;
    }
    if (plugin->native_module != nullptr) FreeLibrary(plugin->native_module);
    plugin->native_module = nullptr;
    plugin->native_on_load = nullptr;
    plugin->native_on_enable = nullptr;
    plugin->native_on_disable = nullptr;
    plugin->native_on_unload = nullptr;
}

int32_t load_native(plugin_handle_t plugin, const plugin_manifest& manifest) {
    const auto dll_path = std::filesystem::u8path(manifest.source_path) /
                          std::filesystem::u8path(manifest.native_entry);
    auto module = LoadLibraryExW(dll_path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) return SAO_ERR_OS_CALL_FAILED;
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
    try {
        status = query(&descriptor);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status == SAO_OK) status = validate_native_descriptor(manifest, descriptor);
    if (status != SAO_OK) {
        FreeLibrary(module);
        return status;
    }
    auto* context = sao_plugins_ctx_create(plugin);
    if (context == nullptr) {
        FreeLibrary(module);
        return SAO_ERR_OS_CALL_FAILED;
    }
    try {
        status = on_load(context);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK) {
        sao_plugins_ctx_destroy(context);
        FreeLibrary(module);
        return status;
    }
    {
        std::lock_guard lock(plugin->mutex);
        plugin->native_module = module;
        plugin->native_on_load = on_load;
        plugin->native_on_enable = on_enable;
        plugin->native_on_disable = on_disable;
        plugin->native_on_unload = on_unload;
        plugin->context = context;
    }
    return SAO_OK;
}

int32_t load_single(plugin_handle_t plugin, std::unordered_set<plugin_handle_t>& visiting) {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr) return SAO_ERR_HANDLE_INVALID;
    (void)retained;
    {
        std::lock_guard lock(plugin->mutex);
        if (plugin->state == lifecycle_state::loaded_active ||
            plugin->state == lifecycle_state::loaded_disabled) return SAO_OK;
        if (plugin->state == lifecycle_state::loading || !visiting.insert(plugin).second) {
            plugin->state = lifecycle_state::failed;
            plugin->last_error = "dependency cycle";
            return SAO_PLUGINS_ERR_DEPENDENCY_CYCLE;
        }
        plugin->state = lifecycle_state::resolving_deps;
    }
    const auto manifest = manifest_snapshot(plugin);
    for (const auto& requirement : manifest.requires_list) {
        const auto id = dependency_id(requirement);
        if (id.empty()) continue;
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
    auto status = sao_plugins_deps_ensure(std::filesystem::u8path(manifest.source_path).c_str(), false, &dependencies);
    if (status != SAO_OK) {
        std::lock_guard lock(plugin->mutex);
        plugin->state = lifecycle_state::failed;
        plugin->last_error = "dependency bootstrap failed";
        visiting.erase(plugin);
        return status;
    }
    {
        std::lock_guard lock(plugin->mutex);
        plugin->state = lifecycle_state::loading;
        plugin->stop_requested.store(false);
    }
    publish(plugin, lifecycle_event::load_started, "load started");
    host_adapter_vtable adapter{};
    bool adapter_loaded = false;
    if (!manifest.native_entry.empty()) {
        status = load_native(plugin, manifest);
    } else {
        if (!adapter_for(manifest.language, adapter) || adapter.load_plugin == nullptr ||
            adapter.call_on_load == nullptr || adapter.unload_plugin == nullptr) {
            status = SAO_PLUGINS_ERR_UNSUPPORTED;
        } else {
            try {
                status = adapter.load_plugin(plugin, &manifest, adapter.host_user_data);
                adapter_loaded = status == SAO_OK;
                if (status == SAO_OK) {
                    auto* context = sao_plugins_ctx_create(plugin);
                    if (context == nullptr) status = SAO_ERR_OS_CALL_FAILED;
                    else {
                        std::lock_guard lock(plugin->mutex);
                        plugin->context = context;
                    }
                }
                if (status == SAO_OK) status = adapter.call_on_load(plugin, adapter.host_user_data);
            } catch (...) {
                status = SAO_ERR_OS_CALL_FAILED;
            }
        }
    }
    visiting.erase(plugin);
    if (status != SAO_OK) {
        clear_native(plugin);
        if (adapter_loaded) {
            try {
                if (adapter.unload_plugin(plugin, adapter.host_user_data) != SAO_OK) {
                    sao_plugins_isolation_record_failure(plugin, "host rollback unload failed");
                }
            } catch (...) {
                sao_plugins_isolation_record_failure(plugin, "host rollback unload crossed exception boundary");
            }
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

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_register_host_adapter(engine_kind language,
                                            const host_adapter_vtable* vtable) {
    const auto index = language_index(language);
    if (vtable == nullptr || index == 0 || index >= g_adapters.size() ||
        vtable->load_plugin == nullptr || vtable->call_on_load == nullptr ||
        vtable->unload_plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_lifecycle_mutex);
    g_adapters[index] = {*vtable, true};
    return SAO_OK;
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
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr) return SAO_ERR_HANDLE_INVALID;
    (void)retained;
    const auto manifest = manifest_snapshot(plugin);
    lifecycle_state previous_state = lifecycle_state::unknown;
    {
        std::lock_guard lock(plugin->mutex);
        if (plugin->state == lifecycle_state::discovered || plugin->state == lifecycle_state::unloaded) return SAO_OK;
        if (plugin->state == lifecycle_state::unloading || plugin->state == lifecycle_state::loading) return SAO_PLUGINS_ERR_BUSY;
        previous_state = plugin->state;
        plugin->state = lifecycle_state::unloading;
        plugin->stop_requested.store(true);
        if (plugin->context != nullptr) plugin_context_request_stop(plugin->context);
    }
    publish(plugin, lifecycle_event::unload_started, "unload started");
    int32_t status = SAO_OK;
    bool allow_unload = true;
    try {
        if (plugin->native_module != nullptr) {
            if (previous_state == lifecycle_state::loaded_active &&
                plugin->native_on_disable != nullptr) status = plugin->native_on_disable();
            if (status == SAO_OK && plugin->native_on_unload != nullptr) status = plugin->native_on_unload();
        } else {
            host_adapter_vtable adapter{};
            if (!adapter_for(manifest.language, adapter)) status = SAO_PLUGINS_ERR_UNSUPPORTED;
            else {
                if (previous_state == lifecycle_state::loaded_active &&
                    adapter.call_on_disable != nullptr) {
                    status = adapter.call_on_disable(plugin, adapter.host_user_data);
                }
                if (status == SAO_OK && adapter.call_on_unload != nullptr) {
                    status = adapter.call_on_unload(plugin, &allow_unload, adapter.host_user_data);
                }
                if (status == SAO_OK && allow_unload) status = adapter.unload_plugin(plugin, adapter.host_user_data);
            }
        }
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK || !allow_unload) {
        {
            std::lock_guard lock(plugin->mutex);
            plugin->state = allow_unload ? lifecycle_state::failed : lifecycle_state::loaded_disabled;
        }
        publish(plugin, allow_unload ? lifecycle_event::load_failed : lifecycle_event::unload_blocked,
                allow_unload ? "unload failed" : "unload blocked");
        return allow_unload ? status : SAO_PLUGINS_ERR_BUSY;
    }
    clear_native(plugin);
    plugin_remove_extensions(plugin);
    {
        std::lock_guard lock(plugin->mutex);
        plugin->state = lifecycle_state::unloaded;
    }
    publish(plugin, lifecycle_event::unloaded, "unloaded");
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_enable(plugin_handle_t plugin) {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr) return SAO_ERR_HANDLE_INVALID;
    (void)retained;
    auto state = sao_plugins_lifecycle_state(plugin);
    if (state == lifecycle_state::discovered || state == lifecycle_state::unloaded || state == lifecycle_state::failed) {
        const auto status = sao_plugins_lifecycle_load(plugin);
        if (status != SAO_OK) return status;
        state = sao_plugins_lifecycle_state(plugin);
    }
    if (state == lifecycle_state::loaded_active) return SAO_OK;
    if (state != lifecycle_state::loaded_disabled) return SAO_PLUGINS_ERR_BUSY;
    const auto manifest = manifest_snapshot(plugin);
    int32_t status = SAO_OK;
    try {
        if (plugin->native_module != nullptr) {
            if (plugin->native_on_enable == nullptr) return SAO_PLUGINS_ERR_UNSUPPORTED;
            status = plugin->native_on_enable();
        } else {
            host_adapter_vtable adapter{};
            if (!adapter_for(manifest.language, adapter) || adapter.call_on_enable == nullptr) return SAO_PLUGINS_ERR_UNSUPPORTED;
            status = adapter.call_on_enable(plugin, adapter.host_user_data);
        }
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status == SAO_OK) {
        {
            std::lock_guard lock(plugin->mutex);
            plugin->manifest.enabled = true;
            plugin->state = lifecycle_state::loaded_active;
        }
        publish(plugin, lifecycle_event::enabled, "enabled");
    }
    return status;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_disable(plugin_handle_t plugin) {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr) return SAO_ERR_HANDLE_INVALID;
    (void)retained;
    if (sao_plugins_lifecycle_state(plugin) == lifecycle_state::loaded_disabled) return SAO_OK;
    if (sao_plugins_lifecycle_state(plugin) != lifecycle_state::loaded_active) return SAO_PLUGINS_ERR_BUSY;
    const auto manifest = manifest_snapshot(plugin);
    int32_t status = SAO_OK;
    try {
        if (plugin->native_module != nullptr) {
            if (plugin->native_on_disable == nullptr) return SAO_PLUGINS_ERR_UNSUPPORTED;
            status = plugin->native_on_disable();
        } else {
            host_adapter_vtable adapter{};
            if (!adapter_for(manifest.language, adapter) || adapter.call_on_disable == nullptr) return SAO_PLUGINS_ERR_UNSUPPORTED;
            status = adapter.call_on_disable(plugin, adapter.host_user_data);
        }
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status == SAO_OK) {
        {
            std::lock_guard lock(plugin->mutex);
            plugin->manifest.enabled = false;
            plugin->state = lifecycle_state::loaded_disabled;
        }
        publish(plugin, lifecycle_event::disabled, "disabled");
    }
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
    if (retained == nullptr) return lifecycle_state::unknown;
    std::lock_guard lock(retained->mutex);
    return retained->state;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_topo_sort(plugin_handle_t* handles, size_t count,
                                plugin_handle_t* out_sorted_handles) {
    if ((count > 0 && (handles == nullptr || out_sorted_handles == nullptr))) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::unordered_map<std::string, plugin_handle_t> by_id;
        std::vector<std::shared_ptr<plugin_handle_s>> retained_plugins;
        retained_plugins.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            auto retained = retain_plugin(handles[index]);
            if (retained == nullptr) return SAO_ERR_HANDLE_INVALID;
            retained_plugins.push_back(std::move(retained));
            by_id.emplace(manifest_snapshot(handles[index]).plugin_id, handles[index]);
        }
        enum class mark : uint8_t { none, visiting, done };
        std::unordered_map<plugin_handle_t, mark> marks;
        std::vector<plugin_handle_t> result;
        std::function<int32_t(plugin_handle_t)> visit = [&](plugin_handle_t plugin) -> int32_t {
            if (marks[plugin] == mark::done) return SAO_OK;
            if (marks[plugin] == mark::visiting) return SAO_PLUGINS_ERR_DEPENDENCY_CYCLE;
            marks[plugin] = mark::visiting;
            for (const auto& requirement : manifest_snapshot(plugin).requires_list) {
                const auto id = dependency_id(requirement);
                if (id.empty()) continue;
                const auto iterator = by_id.find(id);
                if (iterator == by_id.end()) return SAO_PLUGINS_ERR_DEPENDENCY_MISSING;
                const auto status = visit(iterator->second);
                if (status != SAO_OK) return status;
            }
            marks[plugin] = mark::done;
            result.push_back(plugin);
            return SAO_OK;
        };
        for (size_t index = 0; index < count; ++index) {
            const auto status = visit(handles[index]);
            if (status != SAO_OK) return status;
        }
        std::copy(result.begin(), result.end(), out_sorted_handles);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_subscribe(lifecycle_event_cb callback,
                                void* user_data,
                                uint32_t* out_token) {
    if (callback == nullptr || out_token == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    const auto token = g_next_subscriber.fetch_add(1);
    std::lock_guard lock(g_lifecycle_mutex);
    g_subscribers.emplace(token, subscriber_record{callback, user_data});
    *out_token = token;
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_unsubscribe(uint32_t token) {
    if (token == 0) return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_lifecycle_mutex);
    return g_subscribers.erase(token) == 1 ? SAO_OK : SAO_ERR_HANDLE_INVALID;
}

} // namespace sao::plugins::loader
