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
#include <filesystem>
#include <memory>
#include <new>
#include <unordered_set>
#include <utility>
#include <vector>

struct sao_plugins_registry {
    sao::plugins::loader::registry_handle_t registry = nullptr;
    std::vector<sao::plugins::loader::plugin_handle_t> handles;
    std::vector<bool> autostart;
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

namespace {

namespace loader = sao::plugins::loader;

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

bool addManifest(sao_plugins_registry& owned, const loader::plugin_manifest& manifest,
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
    return true;
}

bool adapterRegistrationConflict(int32_t status) noexcept {
    return status == loader::SAO_PLUGINS_ERR_ALREADY_EXISTS ||
           status == loader::SAO_PLUGINS_ERR_BUSY;
}

int32_t
registerHostAdapters(sao_plugins_registry& owned,
                     const sao::launcher::PluginsProviderConfiguration& configuration) noexcept {
    (void)owned;
    [[maybe_unused]] int32_t status = SAO_OK;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON)
    if (!configuration.python_home.empty() &&
        sao::plugins::python_host::sao_plugins_pyhost_available(
            configuration.python_home.c_str())) {
        sao::plugins::python_host::py_host_config python_host_config{};
        python_host_config.python_home = configuration.python_home.c_str();
        status = sao::plugins::python_host::sao_plugins_pyhost_register_loader_adapter(
            &python_host_config, &owned.python_owner);
        if (adapterRegistrationConflict(status))
            return status;
    }
#else
    (void)configuration;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
    status = sao::plugins::emma_host::sao_plugins_emma_register_loader_adapter(&owned.emma_owner);
    if (adapterRegistrationConflict(status))
        return status;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_ANGEL)
    sao::plugins::angel_host::as_host_config angel_host_config{};
    status = sao::plugins::angel_host::sao_plugins_ashost_register_loader_adapter(
        &angel_host_config, &owned.angel_owner);
    if (adapterRegistrationConflict(status))
        return status;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_LUA)
    sao::plugins::lua_host::lua_host_config lua_host_config{};
    status = sao::plugins::lua_host::sao_plugins_luahost_register_loader_adapter(&lua_host_config,
                                                                                 &owned.lua_owner);
    if (adapterRegistrationConflict(status))
        return status;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
    bool available = false;
    status = sao::plugins::csharp_host::sao_plugins_cshost_is_available(&available);
    if (status == SAO_OK && available) {
        sao::plugins::csharp_host::cs_host_config csharp_host_config{};
        status = sao::plugins::csharp_host::sao_plugins_cshost_register_loader_adapter(
            &csharp_host_config, &owned.csharp_owner);
        if (adapterRegistrationConflict(status))
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

int32_t unregisterHostAdapters(sao_plugins_registry& owned) noexcept {
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

int32_t rollbackRegistry(sao_plugins_registry& owned) noexcept {
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
    }
    return unregisterHostAdapters(owned);
}

sao_status_t rollbackDiscoveryFailure(std::unique_ptr<sao_plugins_registry>& owned,
                                      sao_plugins_registry** out,
                                      sao_status_t failure_status) noexcept {
    const int32_t cleanup_status = rollbackRegistry(*owned);
    if (cleanup_status == SAO_OK)
        return failure_status;
    *out = owned.release();
    return cleanup_status;
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
        owned->registry = loader::sao_plugins_registry_instance();
        const int32_t adapter_status = registerHostAdapters(*owned, configuration);
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
            if (!addManifest(*owned, discovered[index].manifest, ids)) {
                loader::sao_plugins_scanner_free(discovered, count);
                return rollbackDiscoveryFailure(owned, out, SAO_STATUS_PLUGIN_LOAD_FAIL);
            }
        }
        loader::sao_plugins_scanner_free(discovered, count);

        for (const auto& manifest_path : configuration.manifests) {
            loader::plugin_manifest manifest;
            if (!loadConfiguredManifest(manifest_path, manifest) ||
                !addManifest(*owned, manifest, ids)) {
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
    if (registry == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    try {
        if (registry->handles.empty())
            return SAO_STATUS_OK;
        std::vector<loader::plugin_handle_t> sorted(registry->handles.size());
        if (loader::sao_plugins_lifecycle_topo_sort(
                registry->handles.data(), registry->handles.size(), sorted.data()) != SAO_OK) {
            return SAO_STATUS_PLUGIN_LOAD_FAIL;
        }
        for (const auto handle : sorted) {
            const auto found =
                std::find(registry->handles.begin(), registry->handles.end(), handle);
            const auto index = static_cast<size_t>(std::distance(registry->handles.begin(), found));
            if (found != registry->handles.end() && registry->autostart[index] &&
                loader::sao_plugins_lifecycle_load(handle) != SAO_OK) {
                return SAO_STATUS_PLUGIN_LOAD_FAIL;
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_PLUGIN_LOAD_FAIL;
    }
}

extern "C" sao_status_t sao_plugins_shutdown(sao_plugins_registry* registry) {
    if (registry == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    try {
        const int32_t status = rollbackRegistry(*registry);
        if (status != SAO_OK)
            return status;
        delete registry;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_INTERNAL;
    }
}