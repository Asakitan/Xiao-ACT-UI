#include "sao_plugins/loader.h"

#include "sao_plugins/sao_status.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/loader/plugin_scanner.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace sao::plugins::loader;

enum class facade_state : uint8_t {
    loading,
    loaded,
    unloading,
    unloaded,
};

struct facade_record {
    std::wstring canonical_plugin_path;
    plugin_handle_t ownership = nullptr;
    uint32_t lifecycle_token = 0;
    uint32_t native_abi_version = 0;
    bool owns_registry_record = false;
    std::atomic<facade_state> state{facade_state::loading};
    std::atomic<lifecycle_event> last_event{lifecycle_event::discovered};
};

std::mutex g_facade_mutex;
std::unordered_map<std::wstring, facade_record*> g_loaded_paths;
std::unordered_map<sao_plugins_native_handle_t, facade_record*> g_live_handles;
std::vector<std::unique_ptr<facade_record>> g_handle_tokens;

int32_t map_loader_status(int32_t status) noexcept {
    switch (status) {
        case SAO_OK:
        case SAO_ERR_INVALID_ARGUMENT:
        case SAO_ERR_NOT_INITIALIZED:
        case SAO_ERR_HANDLE_INVALID:
        case SAO_ERR_BUFFER_TOO_SMALL:
        case SAO_ERR_OS_CALL_FAILED:
        case SAO_ERR_NOT_IMPLEMENTED:
            return status;
        case SAO_PLUGINS_ERR_UNSUPPORTED:
            return SAO_ERR_NOT_IMPLEMENTED;
        case SAO_PLUGINS_ERR_DEPENDENCY_MISSING:
            return SAO_ERR_NOT_INITIALIZED;
        case SAO_PLUGINS_ERR_NOT_OWNER:
            return SAO_ERR_HANDLE_INVALID;
        case SAO_PLUGINS_ERR_ALREADY_EXISTS:
        case SAO_PLUGINS_ERR_DEPENDENCY_CYCLE:
        case SAO_PLUGINS_ERR_ABI_MISMATCH:
        case SAO_PLUGINS_ERR_CAPABILITY_MISMATCH:
        case SAO_PLUGINS_ERR_VERSION_MISMATCH:
            return SAO_ERR_INVALID_ARGUMENT;
        case SAO_PLUGINS_ERR_BUSY:
            return SAO_ERR_OS_CALL_FAILED;
        default:
            return SAO_ERR_OS_CALL_FAILED;
    }
}

uint32_t normalize_legacy_abi(uint32_t version) noexcept {
    if (version == 0) return 0;
    if (version <= 0xffffU) return version;
    if ((version & 0xffffU) != 0) return 0;
    return version >> 16U;
}

uint32_t manifest_abi(const plugin_manifest& manifest) noexcept {
    if (manifest.abi_version != 0) return manifest.abi_version;
    if (manifest.native_abi == "sao_plugin_v2") return 2;
    return 1;
}

bool canonicalize_existing(const fs::path& input, fs::path& output) noexcept {
    std::error_code error;
    if (!fs::exists(input, error) || error) return false;
    output = fs::weakly_canonical(input, error);
    return !error;
}

bool equivalent_paths(const fs::path& left, const fs::path& right) noexcept {
    std::error_code error;
    const auto equivalent = fs::equivalent(left, right, error);
    return equivalent && !error;
}

std::wstring canonical_path_key(const fs::path& path) {
    auto key = path.native();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return key;
}

int32_t resolve_descriptor(const wchar_t* legacy_path,
                           fs::path& plugin_directory,
                           fs::path& requested_native_path,
                           scanned_plugin& scanned) {
    fs::path canonical;
    if (!canonicalize_existing(fs::path(legacy_path), canonical)) {
        return SAO_ERR_HANDLE_INVALID;
    }

    std::error_code error;
    if (fs::is_directory(canonical, error) && !error) {
        plugin_directory = canonical;
    } else if (fs::is_regular_file(canonical, error) && !error) {
        if (canonical.filename() == L"plugin.json") {
            plugin_directory = canonical.parent_path();
        } else if (canonical.extension() == L".dll") {
            plugin_directory = canonical.parent_path();
            requested_native_path = canonical;
        } else {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    } else {
        return SAO_ERR_HANDLE_INVALID;
    }

    const auto status = sao_plugins_scanner_refresh_one(plugin_directory.c_str(), &scanned);
    if (status != SAO_OK) return map_loader_status(status);
    if (!requested_native_path.empty()) {
        if (scanned.manifest.native_entry.empty()) return SAO_ERR_INVALID_ARGUMENT;
        const auto declared_native = plugin_directory / fs::u8path(scanned.manifest.native_entry);
        if (!equivalent_paths(requested_native_path, declared_native)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }
    return SAO_OK;
}

void SAO_PLUGINS_CALL observe_lifecycle(plugin_handle_t plugin,
                                        lifecycle_event event,
                                        const char*,
                                        void* user_data) noexcept {
    auto* record = static_cast<facade_record*>(user_data);
    if (record != nullptr && record->ownership == plugin) record->last_event.store(event);
}

void erase_live_record(facade_record* record, sao_plugins_native_handle_t handle) {
    std::lock_guard lock(g_facade_mutex);
    const auto path = g_loaded_paths.find(record->canonical_plugin_path);
    if (path != g_loaded_paths.end() && path->second == record) g_loaded_paths.erase(path);
    const auto live = g_live_handles.find(handle);
    if (live != g_live_handles.end() && live->second == record) g_live_handles.erase(live);
}

} // namespace

struct sao_plugins_native_s {};

extern "C" int32_t SAO_PLUGINS_CALL sao_plugins_native_load(
    const wchar_t* dll_path,
    uint32_t manifest_abi_version,
    sao_plugins_native_handle_t* out_handle) {
    if (out_handle != nullptr) *out_handle = nullptr;
    if (dll_path == nullptr || dll_path[0] == L'\0' || out_handle == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    try {
        const auto requested_abi = normalize_legacy_abi(manifest_abi_version);
        if (requested_abi == 0) return SAO_ERR_INVALID_ARGUMENT;

        fs::path plugin_directory;
        fs::path requested_native_path;
        scanned_plugin scanned;
        auto status = resolve_descriptor(
            dll_path, plugin_directory, requested_native_path, scanned);
        if (status != SAO_OK) return status;
        if (manifest_abi(scanned.manifest) != requested_abi) {
            return map_loader_status(SAO_PLUGINS_ERR_ABI_MISMATCH);
        }

        const auto path_key = canonical_path_key(plugin_directory);
        {
            std::lock_guard lock(g_facade_mutex);
            const auto existing = g_loaded_paths.find(path_key);
            if (existing != g_loaded_paths.end()) {
                auto* record = existing->second;
                if (record->native_abi_version != requested_abi) {
                    return map_loader_status(SAO_PLUGINS_ERR_ABI_MISMATCH);
                }
                if (record->state.load() != facade_state::loaded) {
                    return map_loader_status(SAO_PLUGINS_ERR_BUSY);
                }
                *out_handle = reinterpret_cast<sao_plugins_native_handle_t>(record);
                return SAO_OK;
            }
        }

        auto record = std::make_unique<facade_record>();
        record->canonical_plugin_path = path_key;
        record->native_abi_version = requested_abi;
        auto* record_ptr = record.get();
        const auto facade_handle = reinterpret_cast<sao_plugins_native_handle_t>(record_ptr);
        {
            std::lock_guard lock(g_facade_mutex);
            g_handle_tokens.push_back(std::move(record));
            const auto [existing, inserted] = g_loaded_paths.emplace(path_key, record_ptr);
            if (!inserted) {
                record_ptr->state.store(facade_state::unloaded);
                return map_loader_status(SAO_PLUGINS_ERR_BUSY);
            }
            try {
                g_live_handles.emplace(facade_handle, record_ptr);
            } catch (...) {
                g_loaded_paths.erase(existing);
                record_ptr->state.store(facade_state::unloaded);
                throw;
            }
        }

        status = sao_plugins_registry_add_plugin(
            sao_plugins_registry_instance(), &scanned.manifest, &record_ptr->ownership);
        if (status == SAO_OK) record_ptr->owns_registry_record = true;
        if (status == SAO_OK) {
            status = sao_plugins_lifecycle_subscribe(
                observe_lifecycle, record_ptr, &record_ptr->lifecycle_token);
        }
        if (status == SAO_OK) status = sao_plugins_lifecycle_load(record_ptr->ownership);
        if (status != SAO_OK) {
            if (record_ptr->lifecycle_token != 0) {
                (void)sao_plugins_lifecycle_unsubscribe(record_ptr->lifecycle_token);
                record_ptr->lifecycle_token = 0;
            }
            if (record_ptr->owns_registry_record) {
                (void)sao_plugins_registry_remove(
                    sao_plugins_registry_instance(), record_ptr->ownership);
                record_ptr->owns_registry_record = false;
            }
            record_ptr->ownership = nullptr;
            record_ptr->state.store(facade_state::unloaded);
            erase_live_record(record_ptr, facade_handle);
            return map_loader_status(status);
        }

        record_ptr->state.store(facade_state::loaded);
        *out_handle = facade_handle;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" void SAO_PLUGINS_CALL sao_plugins_native_unload(sao_plugins_native_handle_t handle) {
    if (handle == nullptr) return;

    try {
        facade_record* record = nullptr;
        {
            std::lock_guard lock(g_facade_mutex);
            const auto iterator = g_live_handles.find(handle);
            if (iterator == g_live_handles.end()) return;
            record = iterator->second;
        }

        auto expected = facade_state::loaded;
        if (!record->state.compare_exchange_strong(expected, facade_state::unloading)) return;

        int32_t status = SAO_ERR_OS_CALL_FAILED;
        try {
            status = sao_plugins_lifecycle_unload(record->ownership);
        } catch (...) {
            status = SAO_ERR_OS_CALL_FAILED;
        }
        if (status != SAO_OK) {
            record->state.store(facade_state::loaded);
            return;
        }

        if (record->lifecycle_token != 0) {
            (void)sao_plugins_lifecycle_unsubscribe(record->lifecycle_token);
            record->lifecycle_token = 0;
        }
        if (record->owns_registry_record) {
            const auto remove_status = sao_plugins_registry_remove(
                sao_plugins_registry_instance(), record->ownership);
            if (remove_status != SAO_OK) {
                record->state.store(facade_state::loaded);
                return;
            }
            record->owns_registry_record = false;
        }
        record->ownership = nullptr;
        record->state.store(facade_state::unloaded);
        erase_live_record(record, handle);
    } catch (...) {
    }
}
