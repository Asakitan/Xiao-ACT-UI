#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"

#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/loader/plugin_scanner.h"
#include "sao_plugins/sao_status.h"

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
};

namespace {

namespace loader = sao::plugins::loader;

bool configuredPathsExist(
    const sao::launcher::PluginsProviderConfiguration& configuration) {
    std::error_code error;
    for (const auto& root : configuration.roots) {
        if (!std::filesystem::is_directory(root, error) || error) return false;
    }
    for (const auto& root : configuration.user_roots) {
        error.clear();
        if (!std::filesystem::is_directory(root, error) || error) return false;
    }
    for (const auto& manifest : configuration.manifests) {
        error.clear();
        if (!std::filesystem::is_regular_file(manifest, error) || error) {
            return false;
        }
    }
    return true;
}

bool loadConfiguredManifest(const std::wstring& manifest_path,
                            loader::plugin_manifest& manifest) {
    if (loader::sao_plugins_manifest_load_from_file(manifest_path.c_str(),
                                                    &manifest) != SAO_OK ||
        loader::validate_manifest(manifest) != SAO_OK) {
        return false;
    }
    const auto directory = std::filesystem::path(manifest_path).parent_path();
    std::error_code error;
    if (!std::filesystem::is_regular_file(
            directory / std::filesystem::u8path(manifest.entry), error) ||
        error) {
        return false;
    }
    if (!manifest.native_entry.empty()) {
        error.clear();
        if (!std::filesystem::is_regular_file(
                directory / std::filesystem::u8path(manifest.native_entry),
                error) ||
            error) {
            return false;
        }
    }
    return true;
}

bool addManifest(sao_plugins_registry& owned,
                 const loader::plugin_manifest& manifest,
                 std::unordered_set<std::string>& ids) {
    if (!ids.insert(manifest.plugin_id).second) return false;
    loader::plugin_handle_t handle = nullptr;
    if (loader::sao_plugins_registry_add_plugin(
            owned.registry, &manifest, &handle) != SAO_OK || handle == nullptr) {
        return false;
    }
    owned.handles.push_back(handle);
    owned.autostart.push_back(manifest.enabled);
    return true;
}

void rollbackRegistry(sao_plugins_registry& owned) noexcept {
    for (auto iterator = owned.handles.rbegin();
         iterator != owned.handles.rend(); ++iterator) {
        (void)loader::sao_plugins_lifecycle_unload(*iterator);
        (void)loader::sao_plugins_registry_remove(owned.registry, *iterator);
    }
    owned.handles.clear();
    owned.autostart.clear();
}

} // namespace

extern "C" sao_status_t sao_plugins_discover(
    sao_platform_ctx*, sao_plugins_registry** out) {
    if (out == nullptr) return SAO_STATUS_INVALID_ARGUMENT;
    *out = nullptr;
    try {
    const auto configuration =
        sao::launcher::launcherProviderConfigurationSnapshot().plugins;
    if (!configuration.enabled ||
        (configuration.roots.empty() && configuration.user_roots.empty() &&
         configuration.manifests.empty())) {
        return SAO_STATUS_PLUGIN_LOAD_FAIL;
    }
    if (!configuredPathsExist(configuration)) {
        return SAO_STATUS_PLUGIN_LOAD_FAIL;
    }
    auto owned = std::make_unique<sao_plugins_registry>();
    owned->registry = loader::sao_plugins_registry_instance();
    std::unordered_set<std::string> ids;

    loader::scan_config scan;
    scan.builtin_roots = configuration.roots;
    scan.user_roots = configuration.user_roots;
    scan.enable_workspace_walkup = configuration.workspace_walkup;
    scan.max_depth = configuration.max_depth;
    loader::scanned_plugin* discovered = nullptr;
    size_t count = 0;
    if (loader::sao_plugins_scanner_discover(&scan, &discovered, &count) != SAO_OK) {
        return SAO_STATUS_PLUGIN_LOAD_FAIL;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!addManifest(*owned, discovered[index].manifest, ids)) {
            loader::sao_plugins_scanner_free(discovered, count);
            rollbackRegistry(*owned);
            return SAO_STATUS_PLUGIN_LOAD_FAIL;
        }
    }
    loader::sao_plugins_scanner_free(discovered, count);

    for (const auto& manifest_path : configuration.manifests) {
        loader::plugin_manifest manifest;
        if (!loadConfiguredManifest(manifest_path, manifest) ||
            !addManifest(*owned, manifest, ids)) {
            rollbackRegistry(*owned);
            return SAO_STATUS_PLUGIN_LOAD_FAIL;
        }
    }
    *out = owned.release();
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_PLUGIN_LOAD_FAIL;
    }
}

extern "C" sao_status_t sao_plugins_activate_autostart(
    sao_plugins_registry* registry) {
    if (registry == nullptr) return SAO_STATUS_INVALID_ARGUMENT;
    try {
    if (registry->handles.empty()) return SAO_STATUS_OK;
    std::vector<loader::plugin_handle_t> sorted(registry->handles.size());
    if (loader::sao_plugins_lifecycle_topo_sort(
            registry->handles.data(), registry->handles.size(),
            sorted.data()) != SAO_OK) {
        return SAO_STATUS_PLUGIN_LOAD_FAIL;
    }
    for (const auto handle : sorted) {
        const auto found = std::find(
            registry->handles.begin(), registry->handles.end(), handle);
        const auto index = static_cast<size_t>(
            std::distance(registry->handles.begin(), found));
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
    if (registry == nullptr) return SAO_STATUS_INVALID_ARGUMENT;
    try {
    while (!registry->handles.empty()) {
        const size_t index = registry->handles.size() - 1;
        const auto handle = registry->handles[index];
        const auto state = loader::sao_plugins_lifecycle_state(handle);
        const bool needs_unload =
            state != loader::lifecycle_state::discovered &&
            state != loader::lifecycle_state::unloaded &&
            state != loader::lifecycle_state::failed;
        if ((needs_unload &&
             loader::sao_plugins_lifecycle_unload(handle) != SAO_OK) ||
            loader::sao_plugins_registry_remove(registry->registry, handle) != SAO_OK) {
            return SAO_STATUS_INTERNAL;
        }
        registry->handles.erase(registry->handles.begin() + index);
        registry->autostart.erase(registry->autostart.begin() + index);
    }
    delete registry;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_INTERNAL;
    }
}