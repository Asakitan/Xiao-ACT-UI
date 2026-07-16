#include "sao/plugins/loader/plugin_install.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "plugin_internal.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace sao::plugins::loader {
namespace {

namespace fs = std::filesystem;

struct zip_entry {
    std::string name;
    uint32_t local_offset = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint16_t method = 0;
};

uint16_t read_u16(const unsigned char* data) noexcept {
    return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
}

uint32_t read_u32(const unsigned char* data) noexcept {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

bool read_archive(const fs::path& path, std::vector<unsigned char>& bytes) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto length = input.tellg();
    if (length <= 0 || length > 512 * 1024 * 1024) return false;
    bytes.resize(static_cast<size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    return static_cast<bool>(input);
}

bool parse_stored_zip(const std::vector<unsigned char>& bytes,
                      std::vector<zip_entry>& entries, bool& compressed) {
    compressed = false;
    if (bytes.size() < 22) return false;
    size_t eocd = std::string::npos;
    const auto minimum = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    for (size_t offset = bytes.size() - 22;; --offset) {
        if (read_u32(bytes.data() + offset) == 0x06054b50U) { eocd = offset; break; }
        if (offset == minimum) break;
    }
    if (eocd == std::string::npos) return false;
    const auto count = read_u16(bytes.data() + eocd + 10);
    const auto central_size = read_u32(bytes.data() + eocd + 12);
    const auto central_offset = read_u32(bytes.data() + eocd + 16);
    if (static_cast<uint64_t>(central_offset) + central_size > bytes.size()) return false;
    size_t cursor = central_offset;
    for (uint16_t index = 0; index < count; ++index) {
        if (cursor + 46 > bytes.size() || read_u32(bytes.data() + cursor) != 0x02014b50U) return false;
        zip_entry entry;
        entry.method = read_u16(bytes.data() + cursor + 10);
        entry.compressed_size = read_u32(bytes.data() + cursor + 20);
        entry.uncompressed_size = read_u32(bytes.data() + cursor + 24);
        const auto name_length = read_u16(bytes.data() + cursor + 28);
        const auto extra_length = read_u16(bytes.data() + cursor + 30);
        const auto comment_length = read_u16(bytes.data() + cursor + 32);
        entry.local_offset = read_u32(bytes.data() + cursor + 42);
        if (cursor + 46ULL + name_length + extra_length + comment_length > bytes.size()) return false;
        entry.name.assign(reinterpret_cast<const char*>(bytes.data() + cursor + 46), name_length);
        if (entry.method != 0 || entry.compressed_size != entry.uncompressed_size) compressed = true;
        entries.push_back(std::move(entry));
        cursor += 46ULL + name_length + extra_length + comment_length;
    }
    return !entries.empty();
}

int32_t extract_stored_zip(const std::vector<unsigned char>& bytes,
                           const std::vector<zip_entry>& entries,
                           const fs::path& destination, std::string& message) {
    std::error_code error;
    fs::create_directories(destination, error);
    if (error) return SAO_ERR_OS_CALL_FAILED;
    for (const auto& entry : entries) {
        const auto relative = fs::u8path(entry.name);
        const auto target = destination / relative;
        if (!path_is_within_base(destination.native(), target.native())) {
            message = "archive contains an unsafe path";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (!entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\')) {
            fs::create_directories(target, error);
            if (error) return SAO_ERR_OS_CALL_FAILED;
            continue;
        }
        if (static_cast<uint64_t>(entry.local_offset) + 30 > bytes.size() ||
            read_u32(bytes.data() + entry.local_offset) != 0x04034b50U) return SAO_ERR_INVALID_ARGUMENT;
        const auto name_length = read_u16(bytes.data() + entry.local_offset + 26);
        const auto extra_length = read_u16(bytes.data() + entry.local_offset + 28);
        const uint64_t data_offset = static_cast<uint64_t>(entry.local_offset) + 30 + name_length + extra_length;
        if (data_offset + entry.uncompressed_size > bytes.size()) return SAO_ERR_INVALID_ARGUMENT;
        fs::create_directories(target.parent_path(), error);
        if (error) return SAO_ERR_OS_CALL_FAILED;
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        if (!output) return SAO_ERR_OS_CALL_FAILED;
        output.write(reinterpret_cast<const char*>(bytes.data() + data_offset), entry.uncompressed_size);
        if (!output) return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
}

fs::path find_plugin_root(const fs::path& staging) {
    std::error_code error;
    if (fs::is_regular_file(staging / L"plugin.json", error)) return staging;
    fs::path candidate;
    for (const auto& item : fs::directory_iterator(staging, error)) {
        if (error || !item.is_directory(error)) continue;
        if (fs::is_regular_file(item.path() / L"plugin.json", error)) {
            if (!candidate.empty()) return {};
            candidate = item.path();
        }
    }
    return candidate;
}

std::string utf8_from_wide(const std::wstring& value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_install_archive(const wchar_t* archive_path,
                            const wchar_t* user_plugins_dir,
                            bool allow_replace,
                            install_result* result) {
    if (result == nullptr || archive_path == nullptr || user_plugins_dir == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *result = install_result{};
    try {
        const fs::path archive(archive_path);
        const fs::path user_root(user_plugins_dir);
        std::error_code error;
        if (!fs::is_regular_file(archive, error)) return SAO_ERR_HANDLE_INVALID;
        fs::create_directories(user_root, error);
        if (error) return SAO_ERR_OS_CALL_FAILED;

        std::vector<unsigned char> bytes;
        std::vector<zip_entry> entries;
        bool compressed = false;
        if (!read_archive(archive, bytes) || !parse_stored_zip(bytes, entries, compressed)) {
            result->message = "invalid ZIP archive";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (compressed) {
            result->message = "compressed ZIP entries are unsupported";
            return SAO_PLUGINS_ERR_UNSUPPORTED;
        }

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto staging = user_root / (L".sao-install-" + std::to_wstring(stamp));
        struct cleanup_guard {
            fs::path path;
            ~cleanup_guard() { std::error_code ignored; fs::remove_all(path, ignored); }
        } cleanup{staging};
        auto status = extract_stored_zip(bytes, entries, staging, result->message);
        if (status != SAO_OK) return status;
        const auto plugin_root = find_plugin_root(staging);
        if (plugin_root.empty()) {
            result->message = "archive must contain exactly one plugin.json";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        plugin_manifest manifest;
        status = sao_plugins_manifest_load_from_file((plugin_root / L"plugin.json").c_str(), &manifest);
        if (status != SAO_OK || validate_manifest(manifest) != SAO_OK) {
            result->message = "plugin manifest is invalid";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (!fs::is_regular_file(plugin_root / fs::u8path(manifest.entry), error)) {
            result->message = "plugin entry is missing";
            return SAO_ERR_HANDLE_INVALID;
        }
        if (!manifest.native_entry.empty() &&
            !fs::is_regular_file(plugin_root / fs::u8path(manifest.native_entry), error)) {
            result->message = "native plugin entry is missing";
            return SAO_ERR_HANDLE_INVALID;
        }
        const auto destination = user_root / fs::u8path(manifest.plugin_id);
        if (!path_is_within_base(user_root.native(), destination.native())) return SAO_ERR_INVALID_ARGUMENT;
        fs::path backup;
        if (fs::exists(destination, error)) {
            plugin_manifest previous;
            if (sao_plugins_manifest_load_from_file((destination / L"plugin.json").c_str(), &previous) == SAO_OK) {
                result->previous_version = previous.version;
            }
            if (!allow_replace) {
                result->message = "plugin already installed";
                return SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            const auto existing = sao_plugins_registry_find(sao_plugins_registry_instance(), manifest.plugin_id.c_str());
            if (existing != nullptr && sao_plugins_lifecycle_state(existing) != lifecycle_state::unloaded &&
                sao_plugins_lifecycle_state(existing) != lifecycle_state::discovered &&
                sao_plugins_lifecycle_state(existing) != lifecycle_state::failed) return SAO_PLUGINS_ERR_BUSY;
            backup = user_root / (L".sao-backup-" + std::to_wstring(stamp));
            fs::rename(destination, backup, error);
            if (error) return SAO_ERR_OS_CALL_FAILED;
            result->replaced = true;
        }
        fs::rename(plugin_root, destination, error);
        if (error) {
            if (!backup.empty()) {
                std::error_code rollback_error;
                fs::rename(backup, destination, rollback_error);
            }
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (!backup.empty()) fs::remove_all(backup, error);
        result->ok = true;
        result->plugin_id = manifest.plugin_id;
        result->name = manifest.name;
        result->version = manifest.version;
        result->installed_path = destination.native();
        result->message = "plugin installed";
        return SAO_OK;
    } catch (...) {
        result->message = "plugin installation failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_uninstall_plugin(const char* plugin_id, bool to_recycle_bin) {
    if (plugin_id == nullptr || plugin_id[0] == '\0') return SAO_ERR_INVALID_ARGUMENT;
    auto* registry = sao_plugins_registry_instance();
    auto* plugin = sao_plugins_registry_find(registry, plugin_id);
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    if (!plugin_is_user_owned(plugin)) return SAO_PLUGINS_ERR_NOT_OWNER;
    const auto state = sao_plugins_lifecycle_state(plugin);
    if (state != lifecycle_state::discovered && state != lifecycle_state::unloaded &&
        state != lifecycle_state::failed) return SAO_PLUGINS_ERR_BUSY;
    const auto manifest = manifest_snapshot(plugin);
    try {
        const auto directory = fs::u8path(manifest.source_path);
        if (directory.empty() || !fs::exists(directory)) return SAO_ERR_HANDLE_INVALID;
        if (to_recycle_bin) {
            auto path = directory.native();
            path.push_back(L'\0');
            path.push_back(L'\0');
            SHFILEOPSTRUCTW operation{};
            operation.wFunc = FO_DELETE;
            operation.pFrom = path.c_str();
            operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
            if (SHFileOperationW(&operation) != 0 || operation.fAnyOperationsAborted) return SAO_ERR_OS_CALL_FAILED;
        } else {
            std::error_code error;
            fs::remove_all(directory, error);
            if (error) return SAO_ERR_OS_CALL_FAILED;
        }
        return sao_plugins_registry_remove(registry, plugin);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

bool path_is_within_base(const std::wstring& base, const std::wstring& target) {
    if (base.empty() || target.empty()) return false;
    try {
        const auto canonical_base = fs::weakly_canonical(fs::path(base));
        const auto canonical_target = fs::weakly_canonical(fs::path(target));
        const auto relative = canonical_target.lexically_relative(canonical_base);
        if (relative.empty() && canonical_target != canonical_base) return false;
        if (relative.is_absolute()) return false;
        for (const auto& component : relative) if (component == L"..") return false;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sao::plugins::loader
