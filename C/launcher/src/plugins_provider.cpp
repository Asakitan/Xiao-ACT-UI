#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"

#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/loader/plugin_scanner.h"
#include "sao_plugins/sao_status.h"

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON)
#include "sao/plugins/python_host/py_host.h"
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
#include "sao/plugins/emma_host/emma_loader_adapter.h"
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_ANGEL)
#include "sao/plugins/angel_host/as_call.h"
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_LUA)
#include "sao/plugins/lua_host/lua_host.h"
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
#include "sao/plugins/csharp_host/cs_loader_adapter.h"
#endif

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

struct sao_plugins_registry_body {
    sao::plugins::loader::registry_handle_t registry = nullptr;
    std::vector<sao::plugins::loader::plugin_handle_t> handles;
    std::vector<bool> autostart;
    std::vector<sao::plugins::loader::plugin_manifest> manifests;
    int32_t python_runtime_status = SAO_PLUGINS_PYTHON_RUNTIME_HOST_UNAVAILABLE;
    int32_t python_launch_strategy = SAO_PLUGINS_PYTHON_LAUNCH_DEFER_DEGRADED;
    int32_t operational_status = SAO_PLUGINS_OPERATIONAL_READY;
    sao_status_t last_operation_status = SAO_STATUS_OK;
    sao_status_t last_rollback_status = SAO_STATUS_OK;
    bool rollback_attempted = false;
    bool rollback_succeeded = false;
    uint32_t deferred_count = 0;
    std::mutex operation_mutex;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON)
    sao::plugins::python_host::py_loader_adapter_owner_t python_owner{};
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
    sao::plugins::emma_host::emma_loader_adapter_owner_t emma_owner{};
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_ANGEL)
    sao::plugins::angel_host::as_loader_adapter_owner_t angel_owner{};
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_LUA)
    sao::plugins::lua_host::lua_loader_adapter_owner_t lua_owner{};
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
    sao::plugins::csharp_host::cs_loader_adapter_owner_t csharp_owner{};
#endif
};

struct sao_plugins_registry {
    std::mutex mutex;
    std::condition_variable idle;
    std::shared_ptr<sao_plugins_registry_body> body;
    uint32_t active_calls = 0;
    bool retiring = false;
    bool retired = false;
};

namespace {

namespace loader = sao::plugins::loader;

thread_local sao_plugins_registry* g_active_operation_registry = nullptr;

class ProviderOperationGuard final {
  public:
    explicit ProviderOperationGuard(sao_plugins_registry* registry) noexcept
        : registry_(g_active_operation_registry == nullptr ? registry : nullptr) {
        if (registry_ != nullptr)
            g_active_operation_registry = registry_;
    }

    ~ProviderOperationGuard() {
        if (registry_ != nullptr)
            g_active_operation_registry = nullptr;
    }

    ProviderOperationGuard(const ProviderOperationGuard&) = delete;
    ProviderOperationGuard& operator=(const ProviderOperationGuard&) = delete;

    bool acquired() const noexcept {
        return registry_ != nullptr;
    }

  private:
    sao_plugins_registry* registry_ = nullptr;
};

class RegistryLease final {
  public:
    RegistryLease() = default;
    ~RegistryLease() {
        release();
    }

    RegistryLease(const RegistryLease&) = delete;
    RegistryLease& operator=(const RegistryLease&) = delete;

    sao_status_t acquire(sao_plugins_registry* shell) noexcept {
        if (shell == nullptr)
            return SAO_STATUS_INVALID_ARGUMENT;
        try {
            std::lock_guard lock(shell->mutex);
            if (shell->retiring || shell->retired || !shell->body) {
                return SAO_STATUS_INTERNAL;
            }
            shell_ = shell;
            body_ = shell->body;
            ++shell_->active_calls;
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_INTERNAL;
        }
    }

    sao_plugins_registry_body& body() const noexcept {
        return *body_;
    }

    uint32_t active_calls() const noexcept {
        if (shell_ == nullptr)
            return 0;
        std::lock_guard lock(shell_->mutex);
        return shell_->active_calls;
    }

  private:
    void release() noexcept {
        if (shell_ == nullptr)
            return;
        {
            std::lock_guard lock(shell_->mutex);
            if (shell_->active_calls > 0)
                --shell_->active_calls;
        }
        shell_->idle.notify_all();
        body_.reset();
        shell_ = nullptr;
    }

    sao_plugins_registry* shell_ = nullptr;
    std::shared_ptr<sao_plugins_registry_body> body_;
};

bool configuredPathsExist(const sao::launcher::PluginsProviderConfiguration& configuration) {
    std::error_code error;
    for (const auto& root : configuration.roots) {
        if (!std::filesystem::is_directory(root, error) || error)
            return false;
    }
    for (const auto& root : configuration.user_roots) {
        error.clear();
        if (!std::filesystem::is_directory(root, error) || error)
            return false;
    }
    for (const auto& manifest : configuration.manifests) {
        error.clear();
        if (!std::filesystem::is_regular_file(manifest, error) || error) {
            return false;
        }
    }
    return true;
}

bool loadConfiguredManifest(const std::wstring& manifest_path, loader::plugin_manifest& manifest) {
    if (loader::sao_plugins_manifest_load_from_file(manifest_path.c_str(), &manifest) != SAO_OK ||
        loader::validate_manifest(manifest) != SAO_OK) {
        return false;
    }
    const auto directory = std::filesystem::path(manifest_path).parent_path();
    std::error_code error;
    if (manifest.native_entry.empty() &&
        (!std::filesystem::is_regular_file(directory / std::filesystem::u8path(manifest.entry),
                                           error) ||
         error)) {
        return false;
    }
    if (!manifest.native_entry.empty()) {
        error.clear();
        if (!std::filesystem::is_regular_file(
                directory / std::filesystem::u8path(manifest.native_entry), error) ||
            error) {
            return false;
        }
    }
    return true;
}

bool addManifest(sao_plugins_registry_body& owned, const loader::plugin_manifest& manifest,
                 std::unordered_set<std::string>& ids) {
    if (!ids.insert(manifest.plugin_id).second)
        return false;
    loader::plugin_handle_t handle = nullptr;
    if (loader::sao_plugins_registry_add_plugin(owned.registry, &manifest, &handle) != SAO_OK ||
        handle == nullptr) {
        return false;
    }
    owned.handles.push_back(handle);
    owned.autostart.push_back(manifest.enabled);
    owned.manifests.push_back(manifest);
    return true;
}

int32_t
registerHostAdapters(sao_plugins_registry_body& owned,
                     const sao::launcher::PluginsProviderConfiguration& configuration) noexcept {
    (void)owned;
    [[maybe_unused]] int32_t status = SAO_OK;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON)
    if (configuration.python_home.empty()) {
        owned.python_runtime_status = SAO_PLUGINS_PYTHON_RUNTIME_UNCONFIGURED;
        owned.python_launch_strategy = SAO_PLUGINS_PYTHON_LAUNCH_DEFER_DEGRADED;
    } else if (!sao::plugins::python_host::sao_plugins_pyhost_available(
                   configuration.python_home.c_str())) {
        owned.python_runtime_status = SAO_PLUGINS_PYTHON_RUNTIME_UNAVAILABLE;
        owned.python_launch_strategy = SAO_PLUGINS_PYTHON_LAUNCH_DEFER_DEGRADED;
    } else {
        sao::plugins::python_host::py_host_config python_host_config{};
        python_host_config.python_home = configuration.python_home.c_str();
        status = sao::plugins::python_host::sao_plugins_pyhost_register_loader_adapter(
            &python_host_config, &owned.python_owner);
        if (status != SAO_OK)
            return status;
        owned.python_runtime_status = SAO_PLUGINS_PYTHON_RUNTIME_READY;
        owned.python_launch_strategy = SAO_PLUGINS_PYTHON_LAUNCH_IN_PROCESS;
    }
#else
    (void)configuration;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
    status = sao::plugins::emma_host::sao_plugins_emma_register_loader_adapter(&owned.emma_owner);
    if (status != SAO_OK)
        return status;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_ANGEL)
    sao::plugins::angel_host::as_host_config angel_host_config{};
    status = sao::plugins::angel_host::sao_plugins_ashost_register_loader_adapter(
        &angel_host_config, &owned.angel_owner);
    if (status != SAO_OK)
        return status;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_LUA)
    sao::plugins::lua_host::lua_host_config lua_host_config{};
    status = sao::plugins::lua_host::sao_plugins_luahost_register_loader_adapter(&lua_host_config,
                                                                                 &owned.lua_owner);
    if (status != SAO_OK)
        return status;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
    bool available = false;
    status = sao::plugins::csharp_host::sao_plugins_cshost_is_available(&available);
    if (status != SAO_OK)
        return status;
    if (available) {
        sao::plugins::csharp_host::cs_host_config csharp_host_config{};
        status = sao::plugins::csharp_host::sao_plugins_cshost_register_loader_adapter(
            &csharp_host_config, &owned.csharp_owner);
        if (status != SAO_OK)
            return status;
    }
#endif
    return SAO_OK;
}

bool needsLifecycleUnload(loader::plugin_handle_t handle) noexcept {
    const auto state = loader::sao_plugins_lifecycle_state(handle);
    if (state == loader::lifecycle_state::discovered ||
        state == loader::lifecycle_state::unloaded) {
        return false;
    }
    if (state != loader::lifecycle_state::failed)
        return true;
    loader::plugin_context_t* context = nullptr;
    return loader::sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK &&
           context != nullptr;
}

struct ReloadTarget {
    loader::plugin_handle_t handle = nullptr;
    loader::lifecycle_state original_state = loader::lifecycle_state::unknown;
};

bool isLoadedState(loader::lifecycle_state state) noexcept {
    return state == loader::lifecycle_state::loaded_active ||
           state == loader::lifecycle_state::loaded_disabled;
}

sao_status_t firstFailure(sao_status_t current, sao_status_t candidate) noexcept {
    return current == SAO_STATUS_OK && candidate != SAO_STATUS_OK ? candidate : current;
}

sao_status_t
restoreReloadTargets(const std::vector<ReloadTarget>& targets,
                     const std::unordered_set<loader::plugin_handle_t>& affected) noexcept {
    sao_status_t aggregate = SAO_STATUS_OK;
    for (const auto& target : targets) {
        if (!affected.contains(target.handle)) {
            continue;
        }
        auto current = loader::sao_plugins_lifecycle_state(target.handle);
        if (current == target.original_state) {
            continue;
        }

        if (!isLoadedState(current)) {
            if (needsLifecycleUnload(target.handle)) {
                const sao_status_t unload_status =
                    loader::sao_plugins_lifecycle_unload(target.handle);
                aggregate = firstFailure(aggregate, unload_status);
                if (unload_status != SAO_OK) {
                    continue;
                }
            }
            const sao_status_t load_status = loader::sao_plugins_lifecycle_load(target.handle);
            aggregate = firstFailure(aggregate, load_status);
            if (load_status != SAO_OK) {
                continue;
            }
            current = loader::sao_plugins_lifecycle_state(target.handle);
        }

        if (target.original_state == loader::lifecycle_state::loaded_active &&
            current != loader::lifecycle_state::loaded_active) {
            aggregate =
                firstFailure(aggregate, loader::sao_plugins_lifecycle_enable(target.handle));
        } else if (target.original_state == loader::lifecycle_state::loaded_disabled &&
                   current != loader::lifecycle_state::loaded_disabled) {
            aggregate =
                firstFailure(aggregate, loader::sao_plugins_lifecycle_disable(target.handle));
        }
    }

    for (const auto& target : targets) {
        if (affected.contains(target.handle) &&
            loader::sao_plugins_lifecycle_state(target.handle) != target.original_state) {
            aggregate = firstFailure(aggregate, SAO_STATUS_INTERNAL);
        }
    }
    return aggregate;
}

int32_t unregisterHostAdapters(sao_plugins_registry_body& owned) noexcept {
    (void)owned;
    [[maybe_unused]] int32_t status = SAO_OK;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
    if (owned.csharp_owner != nullptr) {
        status = sao::plugins::csharp_host::sao_plugins_cshost_unregister_loader_adapter(
            owned.csharp_owner);
        if (status != SAO_OK)
            return status;
        owned.csharp_owner = nullptr;
    }
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_LUA)
    if (owned.lua_owner != nullptr) {
        status =
            sao::plugins::lua_host::sao_plugins_luahost_unregister_loader_adapter(owned.lua_owner);
        if (status != SAO_OK)
            return status;
        owned.lua_owner = nullptr;
    }
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_ANGEL)
    if (owned.angel_owner != nullptr) {
        status = sao::plugins::angel_host::sao_plugins_ashost_unregister_loader_adapter(
            owned.angel_owner);
        if (status != SAO_OK)
            return status;
        owned.angel_owner = nullptr;
    }
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
    if (owned.emma_owner != nullptr) {
        status =
            sao::plugins::emma_host::sao_plugins_emma_unregister_loader_adapter(owned.emma_owner);
        if (status != SAO_OK)
            return status;
        owned.emma_owner = nullptr;
    }
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON)
    if (owned.python_owner != nullptr) {
        status = sao::plugins::python_host::sao_plugins_pyhost_unregister_loader_adapter(
            owned.python_owner);
        if (status != SAO_OK)
            return status;
        owned.python_owner = nullptr;
    }
#endif
    return SAO_OK;
}

int32_t rollbackRegistry(sao_plugins_registry_body& owned) noexcept {
    while (!owned.handles.empty()) {
        const auto handle = owned.handles.back();
        if (needsLifecycleUnload(handle)) {
            const int32_t unload_status = loader::sao_plugins_lifecycle_unload(handle);
            if (unload_status != SAO_OK)
                return unload_status;
        }
        const int32_t remove_status = loader::sao_plugins_registry_remove(owned.registry, handle);
        if (remove_status != SAO_OK)
            return remove_status;
        owned.handles.pop_back();
        owned.autostart.pop_back();
        owned.manifests.pop_back();
    }
    return unregisterHostAdapters(owned);
}

sao_status_t rollbackDiscoveryFailure(std::unique_ptr<sao_plugins_registry>& owned,
                                      sao_plugins_registry** out,
                                      sao_status_t failure_status) noexcept {
    const int32_t cleanup_status = rollbackRegistry(*owned->body);
    if (cleanup_status == SAO_OK)
        return failure_status;
    *out = owned.release();
    return cleanup_status;
}

std::string dependencyId(std::string requirement) {
    if (requirement.find(':') != std::string::npos)
        return {};
    const auto stop = requirement.find_first_of("<>=!~; ");
    if (stop != std::string::npos)
        requirement.resize(stop);
    return requirement;
}

std::vector<bool> runtimeDeferred(const sao_plugins_registry_body& body) {
    std::vector<bool> blocked(body.handles.size(), false);
    const bool python_ready = body.python_runtime_status == SAO_PLUGINS_PYTHON_RUNTIME_READY;
    for (size_t index = 0; index < body.manifests.size(); ++index) {
        const auto& manifest = body.manifests[index];
        blocked[index] = !python_ready && manifest.native_entry.empty() &&
                         manifest.language == loader::engine_kind::python;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t index = 0; index < body.manifests.size(); ++index) {
            if (blocked[index])
                continue;
            for (const auto& requirement : body.manifests[index].requires_list) {
                const auto dependency = dependencyId(requirement);
                const auto found = std::find_if(body.manifests.begin(), body.manifests.end(),
                                                [&dependency](const auto& candidate) {
                                                    return candidate.plugin_id == dependency;
                                                });
                if (found != body.manifests.end() &&
                    blocked[static_cast<size_t>(std::distance(body.manifests.begin(), found))]) {
                    blocked[index] = true;
                    changed = true;
                    break;
                }
            }
        }
    }
    std::vector<bool> deferred(body.handles.size(), false);
    for (size_t index = 0; index < deferred.size(); ++index)
        deferred[index] = body.autostart[index] && blocked[index];
    return deferred;
}

} // namespace

extern "C" sao_status_t sao_plugins_discover(sao_platform_ctx*, sao_plugins_registry** out) {
    if (out == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    *out = nullptr;
    std::unique_ptr<sao_plugins_registry> owned;
    try {
        const auto configuration = sao::launcher::launcherProviderConfigurationSnapshot().plugins;
        if (!configuration.enabled ||
            (configuration.roots.empty() && configuration.user_roots.empty() &&
             configuration.manifests.empty())) {
            return SAO_STATUS_PLUGIN_LOAD_FAIL;
        }
        if (!configuredPathsExist(configuration)) {
            return SAO_STATUS_PLUGIN_LOAD_FAIL;
        }
        owned = std::make_unique<sao_plugins_registry>();
        owned->body = std::make_shared<sao_plugins_registry_body>();
        owned->body->registry = loader::sao_plugins_registry_instance();
        const int32_t adapter_status = registerHostAdapters(*owned->body, configuration);
        if (adapter_status != SAO_OK) {
            return rollbackDiscoveryFailure(owned, out, adapter_status);
        }
        std::unordered_set<std::string> ids;

        loader::scan_config scan;
        scan.builtin_roots = configuration.roots;
        scan.user_roots = configuration.user_roots;
        scan.enable_workspace_walkup = configuration.workspace_walkup;
        scan.max_depth = configuration.max_depth;
        loader::scanned_plugin* discovered = nullptr;
        size_t count = 0;
        if (loader::sao_plugins_scanner_discover(&scan, &discovered, &count) != SAO_OK) {
            return rollbackDiscoveryFailure(owned, out, SAO_STATUS_PLUGIN_LOAD_FAIL);
        }
        for (size_t index = 0; index < count; ++index) {
            if (!addManifest(*owned->body, discovered[index].manifest, ids)) {
                loader::sao_plugins_scanner_free(discovered, count);
                return rollbackDiscoveryFailure(owned, out, SAO_STATUS_PLUGIN_LOAD_FAIL);
            }
        }
        loader::sao_plugins_scanner_free(discovered, count);

        for (const auto& manifest_path : configuration.manifests) {
            loader::plugin_manifest manifest;
            if (!loadConfiguredManifest(manifest_path, manifest) ||
                !addManifest(*owned->body, manifest, ids)) {
                return rollbackDiscoveryFailure(owned, out, SAO_STATUS_PLUGIN_LOAD_FAIL);
            }
        }
        *out = owned.release();
        return SAO_STATUS_OK;
    } catch (...) {
        if (owned != nullptr) {
            return rollbackDiscoveryFailure(owned, out, SAO_STATUS_PLUGIN_LOAD_FAIL);
        }
        return SAO_STATUS_PLUGIN_LOAD_FAIL;
    }
}

extern "C" sao_status_t sao_plugins_activate_autostart(sao_plugins_registry* registry) {
    RegistryLease lease;
    const sao_status_t lease_status = lease.acquire(registry);
    if (lease_status != SAO_STATUS_OK)
        return lease_status;
    ProviderOperationGuard operation(registry);
    if (!operation.acquired())
        return loader::SAO_PLUGINS_ERR_BUSY;
    try {
        auto& body = lease.body();
        std::lock_guard operation_lock(body.operation_mutex);
        if (body.handles.empty())
            return SAO_STATUS_OK;
        std::vector<loader::plugin_handle_t> sorted(body.handles.size());
        if (loader::sao_plugins_lifecycle_topo_sort(body.handles.data(), body.handles.size(),
                                                    sorted.data()) != SAO_OK) {
            return SAO_STATUS_PLUGIN_LOAD_FAIL;
        }
        const auto deferred = runtimeDeferred(body);
        body.deferred_count =
            static_cast<uint32_t>(std::count(deferred.begin(), deferred.end(), true));
        body.python_launch_strategy = body.python_runtime_status == SAO_PLUGINS_PYTHON_RUNTIME_READY
                                          ? SAO_PLUGINS_PYTHON_LAUNCH_IN_PROCESS
                                          : SAO_PLUGINS_PYTHON_LAUNCH_DEFER_DEGRADED;
        for (const auto handle : sorted) {
            const auto found = std::find(body.handles.begin(), body.handles.end(), handle);
            const auto index = static_cast<size_t>(std::distance(body.handles.begin(), found));
            if (found != body.handles.end() && body.autostart[index] && !deferred[index] &&
                loader::sao_plugins_lifecycle_load(handle) != SAO_OK) {
                return SAO_STATUS_PLUGIN_LOAD_FAIL;
            }
        }
        body.last_operation_status = SAO_STATUS_OK;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_PLUGIN_LOAD_FAIL;
    }
}

extern "C" sao_status_t sao_plugins_reload_all(sao_plugins_registry* registry) {
    RegistryLease lease;
    const sao_status_t lease_status = lease.acquire(registry);
    if (lease_status != SAO_STATUS_OK)
        return lease_status;
    ProviderOperationGuard operation(registry);
    if (!operation.acquired())
        return loader::SAO_PLUGINS_ERR_BUSY;
    try {
        auto& body = lease.body();
        std::lock_guard operation_lock(body.operation_mutex);
        if (body.operational_status == SAO_PLUGINS_OPERATIONAL_DEGRADED) {
            return SAO_STATUS_INTERNAL;
        }
        if (body.handles.empty()) {
            body.last_operation_status = SAO_STATUS_OK;
            body.last_rollback_status = SAO_STATUS_OK;
            body.rollback_attempted = false;
            body.rollback_succeeded = false;
            return SAO_STATUS_OK;
        }
        std::vector<loader::plugin_handle_t> sorted(body.handles.size());
        const int32_t sort_status = loader::sao_plugins_lifecycle_topo_sort(
            body.handles.data(), body.handles.size(), sorted.data());
        if (sort_status != SAO_OK) {
            body.last_operation_status = sort_status;
            body.last_rollback_status = SAO_STATUS_OK;
            body.rollback_attempted = false;
            body.rollback_succeeded = false;
            return sort_status;
        }

        std::vector<ReloadTarget> reload_targets;
        reload_targets.reserve(sorted.size());
        for (const auto handle : sorted) {
            const auto found = std::find(body.handles.begin(), body.handles.end(), handle);
            if (found == body.handles.end())
                return SAO_STATUS_PLUGIN_LOAD_FAIL;
            const auto lifecycle_state = loader::sao_plugins_lifecycle_state(handle);
            if (isLoadedState(lifecycle_state)) {
                reload_targets.push_back({handle, lifecycle_state});
            }
        }

        const auto fail_reload = [&](sao_status_t original_status,
                                     const std::unordered_set<loader::plugin_handle_t>& affected) {
            const sao_status_t restore_status = restoreReloadTargets(reload_targets, affected);
            body.rollback_attempted = true;
            body.rollback_succeeded = restore_status == SAO_STATUS_OK;
            body.last_rollback_status = restore_status;
            if (restore_status != SAO_STATUS_OK) {
                body.operational_status = SAO_PLUGINS_OPERATIONAL_DEGRADED;
                body.last_operation_status = original_status;
                return static_cast<sao_status_t>(SAO_STATUS_INTERNAL);
            }
            body.operational_status = SAO_PLUGINS_OPERATIONAL_READY;
            body.last_operation_status = original_status;
            return original_status;
        };

        std::unordered_set<loader::plugin_handle_t> affected;
        affected.reserve(reload_targets.size());
        for (auto iterator = reload_targets.rbegin(); iterator != reload_targets.rend();
             ++iterator) {
            const int32_t status = loader::sao_plugins_lifecycle_unload(iterator->handle);
            affected.insert(iterator->handle);
            if (status != SAO_OK) {
                return fail_reload(status, affected);
            }
        }
        for (const auto& target : reload_targets) {
            affected.insert(target.handle);
            int32_t status = loader::sao_plugins_lifecycle_load(target.handle);
            if (status == SAO_OK) {
                const auto current = loader::sao_plugins_lifecycle_state(target.handle);
                if (target.original_state == loader::lifecycle_state::loaded_active &&
                    current != loader::lifecycle_state::loaded_active) {
                    status = loader::sao_plugins_lifecycle_enable(target.handle);
                } else if (target.original_state == loader::lifecycle_state::loaded_disabled &&
                           current != loader::lifecycle_state::loaded_disabled) {
                    status = loader::sao_plugins_lifecycle_disable(target.handle);
                }
            }
            if (status != SAO_OK) {
                return fail_reload(status, affected);
            }
        }
        body.operational_status = SAO_PLUGINS_OPERATIONAL_READY;
        body.last_operation_status = SAO_STATUS_OK;
        body.last_rollback_status = SAO_STATUS_OK;
        body.rollback_attempted = false;
        body.rollback_succeeded = false;
        return SAO_STATUS_OK;
    } catch (...) {
        try {
            auto& body = lease.body();
            std::lock_guard operation_lock(body.operation_mutex);
            body.operational_status = SAO_PLUGINS_OPERATIONAL_DEGRADED;
            body.last_operation_status = SAO_STATUS_INTERNAL;
            body.last_rollback_status = SAO_STATUS_OK;
            body.rollback_attempted = false;
            body.rollback_succeeded = false;
        } catch (...) {
        }
        return SAO_STATUS_INTERNAL;
    }
}

extern "C" sao_status_t sao_plugins_status_snapshot(sao_plugins_registry* registry,
                                                    sao_plugins_status_snapshot_t* out_status) {
    if (out_status == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    const std::size_t caller_size = out_status->struct_size;
    if (caller_size < sizeof(out_status->struct_size))
        return SAO_STATUS_INVALID_ARGUMENT;
    RegistryLease lease;
    const sao_status_t lease_status = lease.acquire(registry);
    if (lease_status != SAO_STATUS_OK)
        return lease_status;
    ProviderOperationGuard operation(registry);
    if (!operation.acquired())
        return loader::SAO_PLUGINS_ERR_BUSY;
    try {
        auto& body = lease.body();
        std::lock_guard operation_lock(body.operation_mutex);
        sao_plugins_status_snapshot_t candidate{};
        const std::size_t copy_size = (std::min)(caller_size, sizeof(candidate));
        candidate.struct_size = static_cast<std::uint32_t>(copy_size);
        candidate.python_runtime_status = body.python_runtime_status;
        candidate.python_launch_strategy = body.python_launch_strategy;
        candidate.operational_status = body.operational_status;
        candidate.last_operation_status = body.last_operation_status;
        candidate.last_rollback_status = body.last_rollback_status;
        candidate.rollback_attempted = body.rollback_attempted ? 1U : 0U;
        candidate.rollback_succeeded = body.rollback_succeeded ? 1U : 0U;
        candidate.discovered_count = static_cast<uint32_t>(body.handles.size());
        candidate.deferred_count = body.deferred_count;
        candidate.active_call_count = lease.active_calls();
        for (const auto handle : body.handles) {
            const auto state = loader::sao_plugins_lifecycle_state(handle);
            if (state == loader::lifecycle_state::loaded_active ||
                state == loader::lifecycle_state::loaded_disabled) {
                ++candidate.loaded_count;
            }
            if (state == loader::lifecycle_state::loaded_active) {
                ++candidate.enabled_count;
            }
        }
        std::memcpy(out_status, &candidate, copy_size);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_INTERNAL;
    }
}

extern "C" sao_status_t sao_plugins_shutdown(sao_plugins_registry* registry) {
    if (registry == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    ProviderOperationGuard operation(registry);
    if (!operation.acquired())
        return loader::SAO_PLUGINS_ERR_BUSY;
    try {
        std::shared_ptr<sao_plugins_registry_body> body;
        {
            std::unique_lock lock(registry->mutex);
            if (registry->retired)
                return SAO_STATUS_OK;
            if (registry->retiring)
                return loader::SAO_PLUGINS_ERR_BUSY;
            registry->retiring = true;
            body = registry->body;
            registry->idle.wait(lock, [registry] { return registry->active_calls == 0; });
        }
        if (!body) {
            std::lock_guard lock(registry->mutex);
            registry->retiring = false;
            registry->retired = true;
            return SAO_STATUS_OK;
        }

        std::lock_guard operation_lock(body->operation_mutex);
        const int32_t status = rollbackRegistry(*body);
        if (status != SAO_OK) {
            body->last_operation_status = status;
            std::lock_guard lock(registry->mutex);
            registry->retiring = false;
            registry->idle.notify_all();
            return status;
        }
        body->operational_status = SAO_PLUGINS_OPERATIONAL_SHUTDOWN;
        body->last_operation_status = SAO_STATUS_OK;
        {
            std::lock_guard lock(registry->mutex);
            registry->body.reset();
            registry->retiring = false;
            registry->retired = true;
        }
        registry->idle.notify_all();
        return SAO_STATUS_OK;
    } catch (...) {
        try {
            std::lock_guard lock(registry->mutex);
            registry->retiring = false;
            registry->idle.notify_all();
        } catch (...) {
        }
        return SAO_STATUS_INTERNAL;
    }
}