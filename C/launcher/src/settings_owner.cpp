#include "settings_owner_internal.h"

#include "settings_codec_internal.h"
#include "settings_json_internal.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sao::launcher::settings_owner {
namespace {

std::atomic<std::uint64_t> g_temp_counter{0};

struct PathRegistry final {
    std::mutex mutex;
    std::vector<std::wstring> paths;
};

PathRegistry& path_registry() {
    static PathRegistry registry;
    return registry;
}

class WinHandle final {
  public:
    WinHandle() noexcept = default;
    explicit WinHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~WinHandle() noexcept {
        close();
    }

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    WinHandle(WinHandle&& other) noexcept : handle_(other.release()) {}
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.release();
        }
        return *this;
    }

    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE get() const noexcept {
        return handle_;
    }
    HANDLE release() noexcept {
        return std::exchange(handle_, INVALID_HANDLE_VALUE);
    }
    bool close() noexcept {
        if (!valid()) {
            handle_ = INVALID_HANDLE_VALUE;
            return true;
        }
        const HANDLE handle = release();
        return ::CloseHandle(handle) != FALSE;
    }

  private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class TempFile final {
  public:
    TempFile() noexcept = default;
    ~TempFile() noexcept {
        cleanup();
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    WinHandle& handle() noexcept {
        return handle_;
    }
    std::wstring& path() noexcept {
        return path_;
    }
    const std::wstring& path() const noexcept {
        return path_;
    }
    void release_path() noexcept {
        path_.clear();
    }

  private:
    void cleanup() noexcept {
        handle_.close();
        if (!path_.empty()) {
            ::DeleteFileW(path_.c_str());
        }
    }

    WinHandle handle_;
    std::wstring path_;
};

bool is_missing_error(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool is_exists_error(DWORD error) noexcept {
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS;
}

sao_status_t status_from_win32(DWORD error) noexcept {
    if (is_missing_error(error)) {
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
        error == ERROR_LOCK_VIOLATION || error == ERROR_WRITE_PROTECT) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    if (is_exists_error(error)) {
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    }
    return SAO_STATUS_ERR_OS_CALL_FAILED;
}

bool paths_equal(const std::wstring& left, const std::wstring& right) noexcept {
    return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

sao_status_t normalize_path(const std::wstring& path, std::wstring& out) noexcept {
    try {
        std::error_code error;
        auto normalized = std::filesystem::absolute(std::filesystem::path(path), error);
        if (error) {
            return status_from_win32(static_cast<DWORD>(error.value()));
        }
        normalized = normalized.lexically_normal();
        std::wstring value = normalized.native();
        if (value.empty() || value.find(L'\0') != std::wstring::npos) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        out = std::move(value);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

bool registry_contains(const PathRegistry& registry, const std::wstring& path) noexcept {
    return std::any_of(registry.paths.begin(), registry.paths.end(),
                       [&path](const std::wstring& existing) {
                           return paths_equal(existing, path);
                       });
}

void unregister_path(const std::wstring& path) noexcept {
    try {
        auto& registry = path_registry();
        std::lock_guard lock(registry.mutex);
        const auto entry = std::find_if(registry.paths.begin(), registry.paths.end(),
                                        [&path](const std::wstring& existing) {
                                            return paths_equal(existing, path);
                                        });
        if (entry != registry.paths.end()) {
            registry.paths.erase(entry);
        }
    } catch (...) {
    }
}

sao_status_t validate_key(std::string_view key) noexcept {
    if (key.empty()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::string serialized;
        return settings_json::serialize_python_compatible(
            Json(key), settings_codec::kMaxPlaintextBytes, serialized);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t create_parent_directories(const std::wstring& path) noexcept {
    try {
        const auto parent = std::filesystem::path(path).parent_path();
        if (parent.empty()) {
            return SAO_STATUS_OK;
        }

        std::error_code error;
        const bool exists = std::filesystem::exists(parent, error);
        if (error) {
            return status_from_win32(static_cast<DWORD>(error.value()));
        }
        if (exists) {
            const bool directory = std::filesystem::is_directory(parent, error);
            if (error) {
                return status_from_win32(static_cast<DWORD>(error.value()));
            }
            return directory ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (!std::filesystem::create_directories(parent, error) && error) {
            return status_from_win32(static_cast<DWORD>(error.value()));
        }
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t read_file(const std::wstring& path, std::string& out, bool& found) noexcept {
    found = false;
    try {
        WinHandle file(::CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file.valid()) {
            const DWORD error = ::GetLastError();
            found = !is_missing_error(error);
            return status_from_win32(error);
        }
        found = true;

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file.get(), &size)) {
            return status_from_win32(::GetLastError());
        }
        if (size.QuadPart < 0 ||
            static_cast<unsigned long long>(size.QuadPart) > settings_codec::kMaxEnvelopeBytes) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t remaining = bytes.size() - offset;
            const DWORD chunk = static_cast<DWORD>(
                (std::min)(remaining,
                           static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD read = 0;
            if (!::ReadFile(file.get(), bytes.data() + offset, chunk, &read, nullptr)) {
                return status_from_win32(::GetLastError());
            }
            if (read == 0) {
                return SAO_STATUS_ERR_OS_CALL_FAILED;
            }
            offset += read;
        }

        char extra = 0;
        DWORD extra_read = 0;
        if (!::ReadFile(file.get(), &extra, 1, &extra_read, nullptr)) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_HANDLE_EOF) {
                return status_from_win32(error);
            }
        }
        if (extra_read != 0) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        out = std::move(bytes);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t create_unique_temp(const std::wstring& target, TempFile& temp) noexcept {
    try {
        constexpr int kMaxAttempts = 64;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            const auto counter = g_temp_counter.fetch_add(1, std::memory_order_relaxed);
            std::wstring candidate = target + L".tmp." + std::to_wstring(::GetCurrentProcessId()) +
                                     L"." + std::to_wstring(counter);
            WinHandle file(::CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.valid()) {
                temp.path() = std::move(candidate);
                temp.handle() = std::move(file);
                return SAO_STATUS_OK;
            }
            const DWORD error = ::GetLastError();
            if (!is_exists_error(error)) {
                return status_from_win32(error);
            }
        }
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t write_all(HANDLE file, std::string_view bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!::WriteFile(file, bytes.data() + offset, chunk, &written, nullptr)) {
            return status_from_win32(::GetLastError());
        }
        if (written == 0) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        offset += written;
    }
    return SAO_STATUS_OK;
}

sao_status_t backup_corrupt_bytes(const std::wstring& path, std::string_view bytes) noexcept {
    try {
        constexpr std::uint64_t kMaxAttempts = 64;
        const std::wstring pid = std::to_wstring(::GetCurrentProcessId());
        TempFile backup;
        for (std::uint64_t attempt = 0; attempt < kMaxAttempts; ++attempt) {
            std::wstring candidate = path + L".corrupt";
            if (attempt >= 1) {
                candidate.append(L".");
                candidate.append(pid);
            }
            if (attempt >= 2) {
                candidate.append(L".");
                candidate.append(std::to_wstring(attempt - 1));
            }

            WinHandle file(::CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.valid()) {
                backup.path() = std::move(candidate);
                backup.handle() = std::move(file);
                break;
            }
            const DWORD error = ::GetLastError();
            if (!is_exists_error(error)) {
                return status_from_win32(error);
            }
        }
        if (!backup.handle().valid()) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }

        auto status = write_all(backup.handle().get(), bytes);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        if (!::FlushFileBuffers(backup.handle().get())) {
            return status_from_win32(::GetLastError());
        }
        if (!backup.handle().close()) {
            return status_from_win32(::GetLastError());
        }
        backup.release_path();
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

struct PathState final {
    bool exists = false;
    bool directory = false;
};

sao_status_t query_path(const std::wstring& path, PathState& out) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        if (is_missing_error(error)) {
            out = {};
            return SAO_STATUS_OK;
        }
        return status_from_win32(error);
    }
    out.exists = true;
    out.directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return SAO_STATUS_OK;
}

sao_status_t create_unique_replace_backup_path(const std::wstring& target,
                                               std::wstring& out) noexcept {
    try {
        constexpr int kMaxAttempts = 64;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            const auto counter = g_temp_counter.fetch_add(1, std::memory_order_relaxed);
            std::wstring candidate =
                target + L".replace-backup." + std::to_wstring(::GetCurrentProcessId()) + L"." +
                std::to_wstring(counter);
            PathState state;
            const auto status = query_path(candidate, state);
            if (status != SAO_STATUS_OK) {
                return status;
            }
            if (!state.exists) {
                out = std::move(candidate);
                return SAO_STATUS_OK;
            }
        }
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

struct ReplaceResult final {
    sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
    bool preserve_temp = false;
};

ReplaceResult move_new_file_to_missing_target(const std::wstring& target,
                                              const std::wstring& temp) noexcept {
    if (::MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return {SAO_STATUS_OK, false};
    }

    const DWORD move_error = ::GetLastError();
    PathState current;
    const auto query_status = query_path(target, current);
    if (query_status != SAO_STATUS_OK) {
        return {query_status, true};
    }
    if (current.exists) {
        return {is_exists_error(move_error) ? SAO_STATUS_ERR_ALREADY_EXISTS
                                            : status_from_win32(move_error),
                false};
    }
    return {status_from_win32(move_error), true};
}

ReplaceResult recover_replace_failure(const std::wstring& target, const std::wstring& temp,
                                      const std::wstring& backup,
                                      DWORD replace_error) noexcept {
    PathState target_state;
    auto status = query_path(target, target_state);
    if (status != SAO_STATUS_OK) {
        return {status, true};
    }
    if (target_state.exists) {
        return {status_from_win32(replace_error), false};
    }

    PathState backup_state;
    status = query_path(backup, backup_state);
    if (status != SAO_STATUS_OK) {
        return {status, true};
    }
    if (backup_state.exists) {
        if (::MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return {status_from_win32(replace_error), false};
        }
        return {status_from_win32(replace_error), true};
    }

    return move_new_file_to_missing_target(target, temp);
}

ReplaceResult replace_target(const std::wstring& target, const std::wstring& temp) noexcept {
    PathState target_state;
    auto status = query_path(target, target_state);
    if (status != SAO_STATUS_OK) {
        return {status, false};
    }
    if (target_state.directory) {
        return {SAO_STATUS_ERR_INVALID_ARGUMENT, false};
    }
    if (!target_state.exists) {
        return move_new_file_to_missing_target(target, temp);
    }

    std::wstring backup;
    status = create_unique_replace_backup_path(target, backup);
    if (status != SAO_STATUS_OK) {
        return {status, false};
    }
    if (::ReplaceFileW(target.c_str(), temp.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
        ::DeleteFileW(backup.c_str());
        return {SAO_STATUS_OK, false};
    }
    const DWORD replace_error = ::GetLastError();
    return recover_replace_failure(target, temp, backup, replace_error);
}

sao_status_t write_file_atomically(const std::wstring& target, std::string_view bytes) noexcept {
    auto status = create_parent_directories(target);
    if (status != SAO_STATUS_OK) {
        return status;
    }

    TempFile temp;
    status = create_unique_temp(target, temp);
    if (status != SAO_STATUS_OK) {
        return status;
    }
    status = write_all(temp.handle().get(), bytes);
    if (status != SAO_STATUS_OK) {
        return status;
    }
    if (!::FlushFileBuffers(temp.handle().get())) {
        return status_from_win32(::GetLastError());
    }
    if (!temp.handle().close()) {
        return status_from_win32(::GetLastError());
    }

    const auto replace_result = replace_target(target, temp.path());
    if (replace_result.status == SAO_STATUS_OK || replace_result.preserve_temp) {
        temp.release_path();
    }
    return replace_result.status;
}

} // namespace

SettingsOwner::SettingsOwner(ConstructionToken, std::wstring path, std::wstring registry_path)
    : path_(std::move(path)), registry_path_(std::move(registry_path)) {}

SettingsOwner::~SettingsOwner() noexcept {
    if (registry_registered_) {
        unregister_path(registry_path_);
    }
}

sao_status_t SettingsOwner::create(std::wstring path,
                                   std::unique_ptr<SettingsOwner>& out) noexcept {
    try {
        if (path.empty() || path.find(L'\0') != std::wstring::npos) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        std::wstring normalized;
        auto status = normalize_path(path, normalized);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        auto owner = std::make_unique<SettingsOwner>(ConstructionToken{}, normalized, normalized);

        auto& registry = path_registry();
        {
            std::lock_guard lock(registry.mutex);
            if (registry_contains(registry, normalized)) {
                return SAO_STATUS_ERR_ALREADY_EXISTS;
            }
            registry.paths.push_back(normalized);
            owner->registry_registered_ = true;
        }
        out = std::move(owner);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t SettingsOwner::load(LoadInfo& out_info) noexcept {
    try {
        std::lock_guard lock(mutex_);
        LoadInfo info{};
        std::string raw;
        bool found = false;
        const auto read_status = read_file(path_, raw, found);
        info.found = found;
        if (read_status == SAO_STATUS_ERR_NOT_FOUND && !found) {
            Json empty = Json::object();
            document_.swap(empty);
            dirty_ = true;
            out_info = info;
            return SAO_STATUS_OK;
        }
        if (read_status != SAO_STATUS_OK) {
            info.source_status = read_status;
            out_info = info;
            return read_status;
        }

        settings_codec::DecodeResult decoded;
        const auto decode_status = settings_codec::decode(raw, decoded);
        if (decode_status != SAO_STATUS_OK) {
            info.source_status = decode_status;
            info.backup_status = backup_corrupt_bytes(path_, raw);
            if (info.backup_status != SAO_STATUS_OK) {
                out_info = info;
                return info.backup_status;
            }
            Json empty = Json::object();
            document_.swap(empty);
            dirty_ = true;
            info.recovered_corrupt = true;
            out_info = info;
            return SAO_STATUS_OK;
        }

        document_.swap(decoded.document);
        dirty_ = decoded.legacy_plaintext;
        if (decoded.legacy_plaintext) {
            info.migration_status = save_locked();
            if (info.migration_status == SAO_STATUS_OK) {
                info.legacy_migrated = true;
            }
        }
        out_info = info;
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t SettingsOwner::save_locked() noexcept {
    if (!dirty_) {
        return SAO_STATUS_OK;
    }
    try {
        Json snapshot = document_;
        std::string envelope;
        auto status = settings_codec::encode(snapshot, envelope);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        status = write_file_atomically(path_, envelope);
        if (status == SAO_STATUS_OK) {
            dirty_ = false;
        }
        return status;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t SettingsOwner::save() noexcept {
    try {
        std::lock_guard lock(mutex_);
        return save_locked();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t SettingsOwner::snapshot(Json& out) const noexcept {
    try {
        std::lock_guard lock(mutex_);
        Json copy = document_;
        out.swap(copy);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t SettingsOwner::get_value(std::string_view top_level_key, Json& out) const noexcept {
    const auto key_status = validate_key(top_level_key);
    if (key_status != SAO_STATUS_OK) {
        return key_status;
    }
    try {
        std::lock_guard lock(mutex_);
        const auto value = document_.find(std::string(top_level_key));
        if (value == document_.end()) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        Json copy = *value;
        out.swap(copy);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t SettingsOwner::get_truthy(std::string_view top_level_key, bool default_value,
                                       bool& out) const noexcept {
    const auto key_status = validate_key(top_level_key);
    if (key_status != SAO_STATUS_OK) {
        return key_status;
    }
    try {
        std::lock_guard lock(mutex_);
        const auto value = document_.find(std::string(top_level_key));
        bool result = default_value;
        if (value != document_.end()) {
            if (value->is_null()) {
                result = false;
            } else if (value->is_boolean()) {
                result = value->get<bool>();
            } else if (value->is_number_integer()) {
                result = value->get<std::int64_t>() != 0;
            } else if (value->is_number_unsigned()) {
                result = value->get<std::uint64_t>() != 0;
            } else if (value->is_number_float()) {
                result = value->get<double>() != 0.0;
            } else if (value->is_string()) {
                result = !value->get_ref<const Json::string_t&>().empty();
            } else {
                result = !value->empty();
            }
        }
        out = result;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t SettingsOwner::set_value_locked(std::string_view top_level_key, Json value) noexcept {
    try {
        std::string key(top_level_key);
        Json updated = document_;
        updated[std::move(key)] = std::move(value);
        std::string serialized;
        const auto validation_status = settings_json::serialize_python_compatible(
            updated, settings_codec::kMaxPlaintextBytes, serialized);
        if (validation_status != SAO_STATUS_OK) {
            return validation_status;
        }
        if (updated == document_) {
            return SAO_STATUS_OK;
        }
        document_.swap(updated);
        dirty_ = true;
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (const nlohmann::json::exception&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t SettingsOwner::set_value(std::string_view top_level_key, Json value) noexcept {
    const auto key_status = validate_key(top_level_key);
    if (key_status != SAO_STATUS_OK) {
        return key_status;
    }
    try {
        std::lock_guard lock(mutex_);
        return set_value_locked(top_level_key, std::move(value));
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t SettingsOwner::set_value_and_save(std::string_view top_level_key,
                                               Json value) noexcept {
    const auto key_status = validate_key(top_level_key);
    if (key_status != SAO_STATUS_OK) {
        return key_status;
    }
    try {
        std::lock_guard lock(mutex_);
        Json previous_document = document_;
        const bool previous_dirty = dirty_;
        sao_status_t status = set_value_locked(top_level_key, std::move(value));
        if (status == SAO_STATUS_OK) {
            status = save_locked();
        }
        if (status != SAO_STATUS_OK) {
            document_.swap(previous_document);
            dirty_ = previous_dirty;
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

bool SettingsOwner::dirty() const noexcept {
    try {
        std::lock_guard lock(mutex_);
        return dirty_;
    } catch (...) {
        return true;
    }
}

sao_status_t SettingsOwner::path(std::wstring& out) const noexcept {
    try {
        std::lock_guard lock(mutex_);
        std::wstring copy = path_;
        out.swap(copy);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

} // namespace sao::launcher::settings_owner
