#include "sao/plugins/loader/plugin_scanner.h"

#include <algorithm>
#include <filesystem>
#include <new>
#include <set>

namespace sao::plugins::loader {
namespace {

namespace fs = std::filesystem;

uint64_t fnv1a(uint64_t hash, const void* data, size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t file_time_ns(const fs::path& path) {
    const auto value = fs::last_write_time(path).time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
}

bool canonical_path(const fs::path& input, fs::path& output) {
    std::error_code error;
    output = fs::weakly_canonical(input, error);
    return !error;
}

int32_t refresh_path(const fs::path& directory, bool user, bool workspace,
                     scanned_plugin& output) {
    output = scanned_plugin{};
    const auto manifest_path = directory / L"plugin.json";
    std::error_code error;
    if (!fs::is_regular_file(manifest_path, error)) return SAO_ERR_HANDLE_INVALID;
    const auto status = sao_plugins_manifest_load_from_file(manifest_path.c_str(), &output.manifest);
    if (status != SAO_OK) return status;
    if (validate_manifest(output.manifest) != SAO_OK) return SAO_ERR_INVALID_ARGUMENT;
    const auto entry_path = directory / fs::u8path(output.manifest.entry);
    if (!fs::is_regular_file(entry_path, error)) return SAO_ERR_HANDLE_INVALID;
    if (!output.manifest.native_entry.empty()) {
        const auto native_path = directory / fs::u8path(output.manifest.native_entry);
        if (!fs::is_regular_file(native_path, error)) return SAO_ERR_HANDLE_INVALID;
    }
    output.is_user_installed = user;
    output.is_workspace_plugin = workspace;
    output.manifest.user_installed = user;
    output.manifest_mtime_ns = file_time_ns(manifest_path);
    output.manifest_size = fs::file_size(manifest_path, error);
    return error ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
}

void scan_root(const fs::path& root, uint32_t max_depth, bool user, bool workspace,
               std::set<std::wstring>& seen, std::vector<scanned_plugin>& output) {
    std::error_code error;
    if (!fs::is_directory(root, error)) return;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_directory(error)) continue;
        const auto depth = static_cast<uint32_t>(iterator.depth() + 1);
        if (depth > max_depth) {
            iterator.disable_recursion_pending();
            continue;
        }
        const auto directory = iterator->path();
        if (!fs::is_regular_file(directory / L"plugin.json", error)) continue;
        fs::path canonical;
        if (!canonical_path(directory, canonical) || !seen.insert(canonical.native()).second) {
            iterator.disable_recursion_pending();
            continue;
        }
        scanned_plugin plugin;
        if (refresh_path(canonical, user, workspace, plugin) == SAO_OK) {
            output.push_back(std::move(plugin));
        }
        iterator.disable_recursion_pending();
    }
}

bool has_workspace_marker(const fs::path& directory) {
    std::error_code error;
    for (const auto* marker : {L".git", L"sao_auto", L"tools", L".vscode", L".github"}) {
        if (fs::exists(directory / marker, error) && !error) return true;
        error.clear();
    }
    return false;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_scanner_discover(const scan_config* cfg,
                             scanned_plugin** out_plugins,
                             size_t* out_count) {
    if (out_plugins == nullptr || out_count == nullptr || cfg == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_plugins = nullptr;
    *out_count = 0;
    try {
        std::vector<scanned_plugin> discovered;
        std::set<std::wstring> seen;
        const auto depth = std::max<uint32_t>(1, cfg->max_depth);
        for (const auto& root : cfg->builtin_roots) scan_root(root, depth, false, false, seen, discovered);
        for (const auto& root : cfg->user_roots) scan_root(root, depth, true, false, seen, discovered);
        if (cfg->enable_workspace_walkup) {
            auto current = fs::current_path();
            while (!current.empty() && !has_workspace_marker(current)) {
                const auto parent = current.parent_path();
                if (parent == current) { current.clear(); break; }
                current = parent;
            }
            if (!current.empty()) scan_root(current / L"plugins", depth, false, true, seen, discovered);
        }
        std::sort(discovered.begin(), discovered.end(), [](const auto& left, const auto& right) {
            return left.manifest.plugin_id < right.manifest.plugin_id;
        });
        if (discovered.empty()) return SAO_OK;
        auto result = std::make_unique<scanned_plugin[]>(discovered.size());
        std::move(discovered.begin(), discovered.end(), result.get());
        *out_count = discovered.size();
        *out_plugins = result.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_scanner_free(scanned_plugin* plugins, size_t) {
    delete[] plugins;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_scanner_refresh_one(const wchar_t* plugin_dir,
                                scanned_plugin* out_plugin) {
    if (plugin_dir == nullptr || out_plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        return refresh_path(fs::path(plugin_dir), false, false, *out_plugin);
    } catch (...) {
        *out_plugin = scanned_plugin{};
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API uint64_t SAO_PLUGINS_CALL
sao_plugins_scanner_signature(const scan_config* cfg) {
    if (cfg == nullptr) return 0;
    scanned_plugin* plugins = nullptr;
    size_t count = 0;
    if (sao_plugins_scanner_discover(cfg, &plugins, &count) != SAO_OK) return 0;
    uint64_t hash = 1469598103934665603ULL;
    for (size_t index = 0; index < count; ++index) {
        const auto& plugin = plugins[index];
        hash = fnv1a(hash, plugin.manifest.plugin_id.data(), plugin.manifest.plugin_id.size());
        hash = fnv1a(hash, &plugin.manifest_mtime_ns, sizeof(plugin.manifest_mtime_ns));
        hash = fnv1a(hash, &plugin.manifest_size, sizeof(plugin.manifest_size));
    }
    sao_plugins_scanner_free(plugins, count);
    return hash;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_scanner_find_workspace_root(const wchar_t* start_dir, wchar_t** out_root_path) {
    if (start_dir == nullptr || out_root_path == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_root_path = nullptr;
    try {
        fs::path current;
        if (!canonical_path(fs::path(start_dir), current)) return SAO_ERR_HANDLE_INVALID;
        while (!current.empty()) {
            if (has_workspace_marker(current)) {
                const auto value = current.native();
                auto result = std::make_unique<wchar_t[]>(value.size() + 1);
                std::copy(value.begin(), value.end(), result.get());
                result[value.size()] = L'\0';
                *out_root_path = result.release();
                return SAO_OK;
            }
            const auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        return SAO_ERR_HANDLE_INVALID;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_scanner_free_wstring(wchar_t* str) {
    delete[] str;
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_scanner_is_plugin_dir(const wchar_t* plugin_dir) {
    if (plugin_dir == nullptr) return false;
    std::error_code error;
    return fs::is_regular_file(fs::path(plugin_dir) / L"plugin.json", error) && !error;
}

} // namespace sao::plugins::loader
