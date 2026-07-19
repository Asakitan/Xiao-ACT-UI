#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/loader/loader_status.h"
#include "plugin_internal.h"

#include <algorithm>
#include <iterator>
#include <new>
#include <shared_mutex>

namespace sao::plugins::loader {

plugin_registry_s& registry_storage() noexcept {
    static plugin_registry_s registry;
    return registry;
}

std::shared_ptr<plugin_handle_s> retain_plugin(plugin_handle_t plugin) noexcept {
    if (plugin == nullptr) return nullptr;
    try {
        auto& registry = registry_storage();
        std::shared_lock lock(registry.mutex);
        const auto iterator = std::find_if(registry.plugins.begin(), registry.plugins.end(),
            [plugin](const auto& entry) { return entry.second.get() == plugin; });
        return iterator == registry.plugins.end() ? nullptr : iterator->second;
    } catch (...) {
        return nullptr;
    }
}

bool registry_contains(plugin_handle_t plugin) noexcept {
    return retain_plugin(plugin) != nullptr;
}

plugin_manifest manifest_snapshot(plugin_handle_t plugin) {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr) return {};
    std::lock_guard lock(retained->mutex);
    return retained->manifest;
}

bool plugin_is_user_owned(plugin_handle_t plugin) noexcept {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr) return false;
    std::lock_guard lock(retained->mutex);
    return retained->user_owned;
}

void plugin_remove_extensions(plugin_handle_t plugin) noexcept {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr) return;
    std::string plugin_id;
    {
        std::lock_guard plugin_lock(retained->mutex);
        plugin_id = retained->manifest.plugin_id;
    }
    auto& registry = registry_storage();
    std::unique_lock lock(registry.mutex);
    registry.extensions.erase(
        std::remove_if(registry.extensions.begin(), registry.extensions.end(),
                       [&plugin_id](const extension_record& record) {
                           return record.plugin_id == plugin_id;
                       }),
        registry.extensions.end());
}

extern "C" SAO_PLUGINS_API registry_handle_t SAO_PLUGINS_CALL
sao_plugins_registry_instance(void) {
    return &registry_storage();
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_registry_add_plugin(registry_handle_t reg,
                                const plugin_manifest* manifest,
                                plugin_handle_t* out_handle) {
    if (out_handle != nullptr) *out_handle = nullptr;
    if (reg == nullptr || manifest == nullptr || out_handle == nullptr ||
        validate_manifest(*manifest) != SAO_OK) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::unique_lock lock(reg->mutex);
        if (reg->plugins.contains(manifest->plugin_id)) return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        auto plugin = std::make_shared<plugin_handle_s>();
        plugin->manifest = *manifest;
        plugin->user_owned = manifest->user_installed;
        auto* raw = plugin.get();
        reg->plugins.emplace(manifest->plugin_id, std::move(plugin));
        *out_handle = raw;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API plugin_handle_t SAO_PLUGINS_CALL
sao_plugins_registry_find(registry_handle_t reg, const char* plugin_id) {
    if (reg == nullptr || plugin_id == nullptr || plugin_id[0] == '\0') return nullptr;
    try {
        std::shared_lock lock(reg->mutex);
        const auto iterator = reg->plugins.find(plugin_id);
        return iterator == reg->plugins.end() ? nullptr : iterator->second.get();
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_registry_remove(registry_handle_t reg, plugin_handle_t handle) {
    if (reg == nullptr || handle == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::unique_lock lock(reg->mutex);
        const auto iterator = std::find_if(reg->plugins.begin(), reg->plugins.end(),
            [handle](const auto& item) { return item.second.get() == handle; });
        if (iterator == reg->plugins.end()) return SAO_ERR_HANDLE_INVALID;
        const auto retained = iterator->second;
        {
            std::lock_guard plugin_lock(retained->mutex);
            const bool failed_without_runtime =
                retained->state == lifecycle_state::failed &&
                retained->context == nullptr &&
                retained->native_module == nullptr &&
                retained->dependency_session == nullptr;
            if (retained->state != lifecycle_state::discovered &&
                retained->state != lifecycle_state::unloaded &&
                !failed_without_runtime) {
                return SAO_PLUGINS_ERR_BUSY;
            }
        }
        const auto plugin_id = iterator->first;
        reg->extensions.erase(
            std::remove_if(reg->extensions.begin(), reg->extensions.end(),
                           [&plugin_id](const extension_record& record) {
                               return record.plugin_id == plugin_id;
                           }),
            reg->extensions.end());
        reg->plugins.erase(iterator);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_registry_add_extension(registry_handle_t reg,
                                   plugin_handle_t owner,
                                   const extension_record* record) {
    if (reg == nullptr || owner == nullptr || record == nullptr || record->id.empty()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::unique_lock lock(reg->mutex);
        const auto owner_iterator = std::find_if(reg->plugins.begin(), reg->plugins.end(),
            [owner](const auto& item) { return item.second.get() == owner; });
        if (owner_iterator == reg->plugins.end()) return SAO_ERR_HANDLE_INVALID;
        const auto duplicate = std::find_if(reg->extensions.begin(), reg->extensions.end(),
            [record, owner](const extension_record& existing) {
                return existing.plugin_id == owner->manifest.plugin_id &&
                       existing.kind == record->kind && existing.id == record->id;
            });
        if (duplicate != reg->extensions.end()) return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        auto owned_record = *record;
        owned_record.plugin_id = owner->manifest.plugin_id;
        reg->extensions.push_back(std::move(owned_record));
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

std::vector<plugin_manifest> snapshot_manifests(registry_handle_t reg) {
    if (reg == nullptr) return {};
    std::shared_lock lock(reg->mutex);
    std::vector<plugin_manifest> result;
    result.reserve(reg->plugins.size());
    for (const auto& [id, plugin] : reg->plugins) result.push_back(plugin->manifest);
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.plugin_id < right.plugin_id;
    });
    return result;
}

std::vector<extension_record> snapshot_extensions(registry_handle_t reg,
                                                  extension_kind kind) {
    if (reg == nullptr) return {};
    std::shared_lock lock(reg->mutex);
    std::vector<extension_record> result;
    std::copy_if(reg->extensions.begin(), reg->extensions.end(),
                 std::back_inserter(result),
                 [kind](const extension_record& record) { return record.kind == kind; });
    return result;
}

} // namespace sao::plugins::loader
