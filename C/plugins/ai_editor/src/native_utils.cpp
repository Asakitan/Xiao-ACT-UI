#include "native_utils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

namespace sao::ai_editor::native {
namespace {

thread_local const char* g_atomic_write_last_stage = "not_started";
thread_local DWORD g_atomic_write_last_error = ERROR_SUCCESS;

void mark_atomic_write_stage(const char* stage) noexcept {
    g_atomic_write_last_stage = stage;
}

bool component_equal(const std::filesystem::path& left, const std::filesystem::path& right) {
    auto a = left.native();
    auto b = right.native();
    std::transform(a.begin(), a.end(), a.begin(),
                   [](wchar_t character) { return std::towlower(character); });
    std::transform(b.begin(), b.end(), b.begin(),
                   [](wchar_t character) { return std::towlower(character); });
    return a == b;
}

bool path_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    while (root_it != root.end()) {
        if (candidate_it == candidate.end() || !component_equal(*root_it, *candidate_it)) {
            return false;
        }
        ++root_it;
        ++candidate_it;
    }
    return true;
}

bool path_equal(const std::filesystem::path& left, const std::filesystem::path& right) {
    return path_within(left, right) && path_within(right, left);
}

class UniqueHandle final {
  public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
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

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct FileIdentity final {
    ULONGLONG volume_serial{};
    std::array<unsigned char, sizeof(FILE_ID_128)> file_id{};
};

struct FileBinding final {
    FileIdentity identity{};
    std::filesystem::path final_path;
    DWORD attributes{};
};

bool same_identity(const FileIdentity& left, const FileIdentity& right) noexcept {
    return left.volume_serial == right.volume_serial && left.file_id == right.file_id;
}

bool query_file_binding(HANDLE handle, FileBinding& result) {
    FILE_ID_INFO identity{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) {
        return false;
    }
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (required == 0) {
        return false;
    }
    std::vector<wchar_t> path_buffer(required);
    const DWORD written = GetFinalPathNameByHandleW(handle, path_buffer.data(), required, flags);
    if (written == 0 || written >= required) {
        return false;
    }
    result.identity.volume_serial = identity.VolumeSerialNumber;
    std::memcpy(result.identity.file_id.data(), identity.FileId.Identifier,
                result.identity.file_id.size());
    result.final_path =
        std::filesystem::path(std::wstring(path_buffer.data(), written)).lexically_normal();
    result.attributes = basic.FileAttributes;
    return !result.final_path.empty();
}

bool same_binding(const FileBinding& left, const FileBinding& right) {
    return same_identity(left.identity, right.identity) &&
           path_equal(left.final_path, right.final_path);
}

bool open_directory_binding(const std::filesystem::path& path, UniqueHandle& handle,
                            FileBinding& binding) {
    UniqueHandle opened(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr));
    if (!opened.valid() || !query_file_binding(opened.get(), binding) ||
        (binding.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (binding.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    handle = std::move(opened);
    return true;
}

bool directory_binding_matches(const std::filesystem::path& path, const FileBinding& expected) {
    UniqueHandle handle;
    FileBinding current;
    return open_directory_binding(path, handle, current) && same_binding(current, expected);
}

bool missing_path_error(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

int32_t open_or_create_directory_binding(const std::filesystem::path& path, bool create,
                                         UniqueHandle& handle, FileBinding& binding) {
    bool create_attempted = false;
    for (;;) {
        UniqueHandle opened(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr));
        if (opened.valid()) {
            if (!query_file_binding(opened.get(), binding)) {
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            if ((binding.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (binding.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
            }
            handle = std::move(opened);
            return SAO_AI_EDITOR_OK;
        }
        const DWORD open_error = GetLastError();
        if (!create || create_attempted || !missing_path_error(open_error)) {
            return missing_path_error(open_error) ? SAO_AI_EDITOR_ERR_OS_CALL_FAILED
                                                  : SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }
        create_attempted = true;
        if (!CreateDirectoryW(path.c_str(), nullptr)) {
            const DWORD create_error = GetLastError();
            if (create_error != ERROR_ALREADY_EXISTS && create_error != ERROR_FILE_EXISTS) {
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
        }
    }
}

struct PinnedDirectory final {
    std::filesystem::path requested_path;
    UniqueHandle handle;
    FileBinding binding;
};

class PinnedDirectoryChain final {
  public:
    int32_t open(const std::filesystem::path& root, const std::filesystem::path& parent,
                 bool create) {
        entries_.clear();
        if (!path_within(root, parent)) {
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }

        auto root_it = root.begin();
        auto parent_it = parent.begin();
        while (root_it != root.end()) {
            if (parent_it == parent.end() || !component_equal(*root_it, *parent_it)) {
                return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
            }
            ++root_it;
            ++parent_it;
        }

        PinnedDirectory root_entry;
        root_entry.requested_path = root;
        int32_t status =
            open_or_create_directory_binding(root, false, root_entry.handle, root_entry.binding);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        entries_.push_back(std::move(root_entry));

        std::filesystem::path current = root;
        for (; parent_it != parent.end(); ++parent_it) {
            if (*parent_it == L"." || *parent_it == L"..") {
                return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
            }
            current /= *parent_it;
            PinnedDirectory entry;
            entry.requested_path = current;
            status = open_or_create_directory_binding(current, create, entry.handle, entry.binding);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            if (!path_equal(entry.binding.final_path.parent_path(),
                            entries_.back().binding.final_path) ||
                !path_within(entries_.front().binding.final_path, entry.binding.final_path)) {
                return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
            }
            entries_.push_back(std::move(entry));
        }
        return matches() ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }

    bool matches() const {
        if (entries_.empty()) {
            return false;
        }
        for (size_t index = 0; index < entries_.size(); ++index) {
            FileBinding held;
            if (!query_file_binding(entries_[index].handle.get(), held) ||
                !same_binding(held, entries_[index].binding)) {
                return false;
            }
            UniqueHandle reopened;
            FileBinding current;
            if (!open_directory_binding(entries_[index].requested_path, reopened, current) ||
                !same_binding(current, entries_[index].binding)) {
                return false;
            }
            if (index != 0 && !path_equal(current.final_path.parent_path(),
                                          entries_[index - 1].binding.final_path)) {
                return false;
            }
        }
        return true;
    }

    HANDLE parent_handle() const noexcept {
        return entries_.empty() ? nullptr : entries_.back().handle.get();
    }

    const FileBinding& root_binding() const noexcept {
        return entries_.front().binding;
    }

    const FileBinding& parent_binding() const noexcept {
        return entries_.back().binding;
    }

  private:
    std::vector<PinnedDirectory> entries_;
};

struct TargetSnapshot final {
    bool exists{};
    UniqueHandle handle;
    FileBinding binding{};
};

int32_t capture_target(const std::filesystem::path& path, const FileBinding& parent,
                       const FileBinding& root, TargetSnapshot& result, bool readable = false) {
    result = {};
    const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_DELETE | (readable ? 0 : FILE_SHARE_WRITE);
    UniqueHandle handle(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES | (readable ? GENERIC_READ : 0), sharing, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) {
        const DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        return missing_path_error(error) ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    FileBinding binding;
    if (!query_file_binding(handle.get(), binding)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if ((binding.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        !path_equal(binding.final_path.parent_path(), parent.final_path) ||
        !path_within(root.final_path, binding.final_path)) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    result.exists = true;
    result.handle = std::move(handle);
    result.binding = std::move(binding);
    return SAO_AI_EDITOR_OK;
}

int32_t read_text_handle(HANDLE handle, std::size_t maximum_bytes, std::string& result) {
    result.clear();
    LARGE_INTEGER size{};
    LARGE_INTEGER origin{};
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(handle, &size) ||
        size.QuadPart < 0) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if (static_cast<uint64_t>(size.QuadPart) > maximum_bytes) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    if (!SetFilePointerEx(handle, origin, nullptr, FILE_BEGIN)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    result.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < result.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(result.size() - offset,
                       static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (!ReadFile(handle, result.data() + offset, chunk, &read, nullptr) || read == 0 ||
            read > chunk) {
            result.clear();
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        offset += read;
    }
    LARGE_INTEGER rebound_size{};
    if (!GetFileSizeEx(handle, &rebound_size) || rebound_size.QuadPart != size.QuadPart) {
        result.clear();
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    if (!valid_utf8(result)) {
        result.clear();
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    return SAO_AI_EDITOR_OK;
}

bool same_target(const TargetSnapshot& left, const TargetSnapshot& right) {
    return left.exists == right.exists &&
           (!left.exists || same_binding(left.binding, right.binding));
}

class TemporaryFile final {
  public:
    ~TemporaryFile() noexcept {
        if (handle_.valid() && !renamed_) {
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            const bool delete_on_close =
                SetFileInformationByHandle(handle_.get(), FileDispositionInfo, &disposition,
                                           sizeof(disposition)) != FALSE;
            if (delete_on_close) {
                handle_.reset();
                return;
            }
            FileBinding expected;
            bool identified = false;
            try {
                identified = query_file_binding(handle_.get(), expected);
            } catch (...) {
                identified = false;
            }
            handle_.reset();
            if (!delete_on_close && identified && !path_.empty()) {
                UniqueHandle reopened(CreateFileW(path_.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                                                  nullptr));
                FileBinding current;
                try {
                    if (reopened.valid() && query_file_binding(reopened.get(), current) &&
                        same_binding(expected, current) &&
                        (current.attributes &
                         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0) {
                        (void)SetFileInformationByHandle(reopened.get(), FileDispositionInfo,
                                                         &disposition, sizeof(disposition));
                    }
                } catch (...) {
                }
            }
        }
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    TemporaryFile() = default;

    void adopt(HANDLE handle, std::filesystem::path path) noexcept {
        handle_.reset(handle);
        path_ = std::move(path);
    }

    HANDLE handle() const noexcept {
        return handle_.get();
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    bool close_for_path_commit(FileBinding& binding) noexcept {
        try {
            if (!query_file_binding(handle_.get(), binding))
                return false;
            committed_binding_ = binding;
            has_committed_binding_ = true;
            handle_.reset();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool committed_binding(FileBinding& binding) const noexcept {
        if (!has_committed_binding_)
            return false;
        binding = committed_binding_;
        return true;
    }

    void discard_closed_path() noexcept {
        if (!path_.empty())
            (void)DeleteFileW(path_.c_str());
        path_.clear();
    }

    void mark_renamed() noexcept {
        renamed_ = true;
        path_.clear();
    }

  private:
    UniqueHandle handle_;
    std::filesystem::path path_;
    FileBinding committed_binding_{};
    bool has_committed_binding_{};
    bool renamed_{};
};

bool random_temporary_name(std::wstring& result) {
    std::array<UCHAR, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return false;
    }
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    result = L".sao-atomic-";
    result.reserve(result.size() + random.size() * 2 + 4);
    for (const UCHAR byte : random) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    result += L".tmp";
    return true;
}

int32_t create_temporary_file(const std::filesystem::path& parent_path, const FileBinding& parent,
                              TemporaryFile& result) {
    constexpr size_t maximum_attempts = 64;
    for (size_t attempt = 0; attempt < maximum_attempts; ++attempt) {
        std::wstring name;
        if (!random_temporary_name(name)) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        const std::filesystem::path candidate = parent_path / name;
        const HANDLE handle =
            CreateFileW(candidate.c_str(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_WRITE_THROUGH,
                        nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        result.adopt(handle, candidate);
        FileBinding binding;
        if (!query_file_binding(result.handle(), binding)) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        if ((binding.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
            !path_equal(binding.final_path.parent_path(), parent.final_path)) {
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
}

bool verify_temporary_file(const TemporaryFile& temporary, const FileBinding& parent) {
    FileBinding binding;
    return query_file_binding(temporary.handle(), binding) &&
           (binding.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
           path_equal(binding.final_path.parent_path(), parent.final_path);
}

bool write_all(HANDLE handle, std::string_view content) noexcept {
    size_t offset = 0;
    while (offset < content.size()) {
        const size_t remaining = content.size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(remaining, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(handle, content.data() + offset, chunk, &written, nullptr) || written == 0 ||
            written > chunk) {
            return false;
        }
        offset += written;
    }
    return true;
}

int32_t rename_temporary_file(TemporaryFile& temporary, HANDLE parent,
                              const std::filesystem::path& target, bool replace_existing) {
    const std::wstring name = target.filename().native();
    if (name.empty() || name.size() > (std::numeric_limits<DWORD>::max)() / sizeof(wchar_t)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const size_t name_bytes = name.size() * sizeof(wchar_t);
    constexpr size_t name_offset = offsetof(FILE_RENAME_INFO, FileName);
    if (name_bytes > (std::numeric_limits<size_t>::max)() - name_offset) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const size_t required = (std::max)(sizeof(FILE_RENAME_INFO), name_offset + name_bytes);
    if (required > (std::numeric_limits<DWORD>::max)()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const size_t storage_units =
        required / sizeof(std::max_align_t) + (required % sizeof(std::max_align_t) != 0 ? 1u : 0u);
    std::vector<std::max_align_t> storage(storage_units);
    std::memset(storage.data(), 0, required);
    auto* information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    information->ReplaceIfExists = replace_existing ? TRUE : FALSE;
    information->RootDirectory = parent;
    information->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(information->FileName, name.data(), name_bytes);
    if (!SetFileInformationByHandle(temporary.handle(), FileRenameInfo, information,
                                    static_cast<DWORD>(required))) {
        const DWORD error = GetLastError();
        g_atomic_write_last_error = error;
        const std::wstring& target_name = target.native();
        const bool replaced =
            replace_existing
                ? ReplaceFileW(target_name.c_str(), temporary.path().c_str(), nullptr,
                               REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE
                : MoveFileExW(temporary.path().c_str(), target_name.c_str(),
                              MOVEFILE_WRITE_THROUGH) != FALSE;
        if (replaced ||
            (!std::filesystem::exists(temporary.path()) &&
             std::filesystem::exists(target))) {
            temporary.mark_renamed();
            return SAO_AI_EDITOR_OK;
        }
        g_atomic_write_last_error = GetLastError() != ERROR_SUCCESS ? GetLastError() : error;
        if (!replace_existing && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    temporary.mark_renamed();
    return SAO_AI_EDITOR_OK;
}

int32_t rename_temporary_file_compat(TemporaryFile& temporary,
                                     const std::filesystem::path& target,
                                     bool replace_existing) {
    FileBinding source_binding;
    if (!temporary.close_for_path_commit(source_binding)) {
        g_atomic_write_last_error = GetLastError();
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    const std::filesystem::path source_path = temporary.path();
    const std::wstring& target_name = target.native();
    const bool replaced =
        replace_existing
            ? ReplaceFileW(target_name.c_str(), source_path.c_str(), nullptr,
                           REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE
            : MoveFileExW(source_path.c_str(), target_name.c_str(),
                          MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!replaced) {
        const DWORD error = GetLastError();
        g_atomic_write_last_error = error;
        temporary.discard_closed_path();
        if (!replace_existing &&
            (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if (std::filesystem::exists(source_path)) {
        g_atomic_write_last_error = ERROR_ACCESS_DENIED;
        temporary.discard_closed_path();
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    temporary.mark_renamed();
    return SAO_AI_EDITOR_OK;
}

int32_t verify_published_file(const TemporaryFile& temporary, const FileBinding& parent,
                              const FileBinding& root, const std::filesystem::path& target) {
    FileBinding source_binding;
    const bool has_handle = temporary.handle() != nullptr &&
                            temporary.handle() != INVALID_HANDLE_VALUE;
    if (has_handle) {
        if (!query_file_binding(temporary.handle(), source_binding))
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } else if (!temporary.committed_binding(source_binding)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    const std::filesystem::path expected =
        (parent.final_path / target.filename()).lexically_normal();
    if ((source_binding.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0 ||
        (has_handle && !path_equal(source_binding.final_path, expected))) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    TargetSnapshot published;
    const int32_t status = capture_target(target, parent, root, published);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    if (!published.exists ||
        !same_identity(source_binding.identity, published.binding.identity) ||
        !path_equal(published.binding.final_path, expected) ||
        !path_equal(published.binding.final_path.parent_path(), parent.final_path) ||
        !path_within(root.final_path, published.binding.final_path)) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    return SAO_AI_EDITOR_OK;
}

bool absolute_normalized(const std::filesystem::path& path, std::filesystem::path& result) {
    if (path.empty() || path.native().find(L'\0') != std::wstring::npos) {
        return false;
    }
    std::error_code error;
    result = path.is_absolute() ? path : std::filesystem::absolute(path, error);
    if (error || !result.is_absolute()) {
        return false;
    }
    result = result.lexically_normal();
    return !result.empty();
}

int32_t read_text_bounded_impl(const std::filesystem::path& root, const std::filesystem::path& path,
                               std::size_t maximum_bytes, std::string& result) {
    result.clear();
    try {
        std::filesystem::path root_path;
        if (!absolute_normalized(root, root_path)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::filesystem::path target;
        if (path.is_absolute()) {
            if (!absolute_normalized(path, target)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        } else {
            if (path.has_root_name() || path.has_root_directory() ||
                path.native().find(L'\0') != std::wstring::npos) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            target = (root_path / path).lexically_normal();
        }
        if (!path_within(root_path, target) || path_equal(root_path, target)) {
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }
        const std::filesystem::path filename = target.filename();
        const std::filesystem::path parent_path = target.parent_path();
        if (filename.empty() || filename == L"." || filename == L".." ||
            filename.native().find(L':') != std::wstring::npos || parent_path.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }

        PinnedDirectoryChain directories;
        int32_t status = directories.open(root_path, parent_path, false);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        const FileBinding& parent = directories.parent_binding();
        const FileBinding& effective_root = directories.root_binding();
        const std::filesystem::path bound_target =
            (parent.final_path / filename).lexically_normal();
        if (!path_equal(bound_target.parent_path(), parent.final_path) ||
            !path_within(effective_root.final_path, bound_target)) {
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }

        TargetSnapshot before;
        status = capture_target(bound_target, parent, effective_root, before, true);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        if (!before.exists) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        status = read_text_handle(before.handle.get(), maximum_bytes, result);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        if (!directories.matches()) {
            result.clear();
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }
        TargetSnapshot after;
        status = capture_target(bound_target, parent, effective_root, after);
        if (status != SAO_AI_EDITOR_OK || !same_target(before, after)) {
            result.clear();
            return status == SAO_AI_EDITOR_OK ? SAO_AI_EDITOR_ERR_BUSY : status;
        }
        return SAO_AI_EDITOR_OK;
    } catch (const std::bad_alloc&) {
        result.clear();
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } catch (...) {
        result.clear();
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

int32_t write_text_atomic_impl(const std::filesystem::path* bounded_root,
                               const std::filesystem::path& path, std::string_view content,
                               bool create_only,
                               const std::string_view* expected_content = nullptr) {
    mark_atomic_write_stage("validate_content");
    if (!valid_utf8(content) || (expected_content != nullptr && !valid_utf8(*expected_content))) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    try {
        const bool bounded = bounded_root != nullptr;
        std::filesystem::path root_path;
        std::filesystem::path target;
        if (bounded) {
            if (!absolute_normalized(*bounded_root, root_path)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (path.is_absolute()) {
                if (!absolute_normalized(path, target)) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
            } else {
                if (path.has_root_name() || path.has_root_directory() ||
                    path.native().find(L'\0') != std::wstring::npos) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                target = (root_path / path).lexically_normal();
            }
            if (!path_within(root_path, target) || path_equal(root_path, target)) {
                return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
            }
        } else if (!absolute_normalized(path, target)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }

        const auto filename = target.filename();
        mark_atomic_write_stage("open_parent");
        const auto parent_path = target.parent_path();
        if (filename.empty() || filename == L"." || filename == L".." ||
            filename.native().find(L':') != std::wstring::npos || parent_path.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }

        PinnedDirectoryChain directory_chain;
        UniqueHandle parent_handle;
        FileBinding parent_binding_storage;
        FileBinding root_binding_storage;
        if (bounded) {
            const int32_t status = directory_chain.open(root_path, parent_path, true);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        } else {
            std::error_code error;
            std::filesystem::create_directories(parent_path, error);
            if (error ||
                !open_directory_binding(parent_path, parent_handle, parent_binding_storage)) {
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            root_binding_storage = parent_binding_storage;
        }

        const FileBinding& parent_binding =
            bounded ? directory_chain.parent_binding() : parent_binding_storage;
        const FileBinding& effective_root =
            bounded ? directory_chain.root_binding() : root_binding_storage;
        const HANDLE parent_directory_handle =
            bounded ? directory_chain.parent_handle() : parent_handle.get();
        const auto directories_match = [&]() {
            return bounded ? directory_chain.matches()
                           : directory_binding_matches(parent_path, parent_binding);
        };

        const std::filesystem::path bound_target =
            (parent_binding.final_path / filename).lexically_normal();
        if (!path_equal(bound_target.parent_path(), parent_binding.final_path) ||
            !path_within(effective_root.final_path, bound_target)) {
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }

        TemporaryFile temporary;
        mark_atomic_write_stage("create_temporary");
        int32_t status =
            create_temporary_file(parent_binding.final_path, parent_binding, temporary);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        if (!directories_match() || !verify_temporary_file(temporary, parent_binding)) {
            mark_atomic_write_stage("verify_temporary");
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }

        TargetSnapshot target_before_write;
        mark_atomic_write_stage("capture_before_write");
        status = capture_target(bound_target, parent_binding, effective_root, target_before_write,
                                expected_content != nullptr);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        if (create_only && target_before_write.exists) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (expected_content != nullptr) {
            if (!target_before_write.exists) {
                return SAO_AI_EDITOR_ERR_BUSY;
            }
            std::string current_content;
            status = read_text_handle(target_before_write.handle.get(), expected_content->size(),
                                      current_content);
            if (status == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL ||
                (status == SAO_AI_EDITOR_OK &&
                 std::string_view(current_content) != *expected_content)) {
                return SAO_AI_EDITOR_ERR_BUSY;
            }
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        mark_atomic_write_stage("write_temporary");
        if (!write_all(temporary.handle(), content) || !FlushFileBuffers(temporary.handle())) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }

        if (!directories_match() || !verify_temporary_file(temporary, parent_binding)) {
            mark_atomic_write_stage("verify_temporary_after_write");
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }
        TargetSnapshot target_before_replace;
        mark_atomic_write_stage("capture_before_replace");
        status = capture_target(bound_target, parent_binding, effective_root,
                                target_before_replace, expected_content != nullptr);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        if (!same_target(target_before_write, target_before_replace)) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (expected_content != nullptr) {
            std::string current_content;
            status = read_text_handle(target_before_replace.handle.get(),
                                      expected_content->size(), current_content);
            if (status == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL ||
                (status == SAO_AI_EDITOR_OK &&
                 std::string_view(current_content) != *expected_content)) {
                return SAO_AI_EDITOR_ERR_BUSY;
            }
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        mark_atomic_write_stage("rename_temporary");
        status = rename_temporary_file(temporary, parent_directory_handle, bound_target,
                                       !create_only && target_before_replace.exists);
        if (status != SAO_AI_EDITOR_OK) {
            target_before_write.handle.reset();
            target_before_replace.handle.reset();
            status = rename_temporary_file_compat(
                temporary, bound_target, !create_only && target_before_replace.exists);
        }
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        if (!directories_match()) {
            mark_atomic_write_stage("verify_directories_after_rename");
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }
        mark_atomic_write_stage("verify_published");
        return verify_published_file(temporary, parent_binding, effective_root, bound_target);
    } catch (const std::bad_alloc&) {
        mark_atomic_write_stage("allocation");
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } catch (...) {
        mark_atomic_write_stage("exception");
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

} // namespace

bool valid_utf8(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    if (value.size() > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

bool valid_simple_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128 || value.find("..") != value.npos) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_' ||
               character == '.';
    });
}

std::wstring utf8_to_wide(std::string_view value) {
    if (!valid_utf8(value) || value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length) != length) {
        return {};
    }
    return result;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<size_t>(INT_MAX)) {
        return {};
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length, nullptr,
                            nullptr) != length) {
        return {};
    }
    return result;
}

bool normalize_root(std::string_view value, std::filesystem::path& result, bool create) {
    if (value.empty()) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(value);
    if (wide.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::path path(wide);
    if (!path.is_absolute()) {
        return false;
    }
    if (create) {
        std::filesystem::create_directories(path, error);
        if (error) {
            return false;
        }
    }
    if (!std::filesystem::is_directory(path, error) || error) {
        return false;
    }
    result = std::filesystem::weakly_canonical(path, error);
    return !error && result.is_absolute();
}

bool resolve_bounded_path(const std::filesystem::path& root, std::string_view value, bool for_write,
                          std::filesystem::path& result) {
    if (value.empty() || !valid_utf8(value)) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(value);
    if (wide.empty()) {
        return false;
    }
    std::filesystem::path requested(wide);
    std::filesystem::path candidate = requested.is_absolute() ? requested : root / requested;
    std::error_code error;
    if (for_write && !std::filesystem::exists(candidate, error)) {
        auto parent = candidate.parent_path();
        if (parent.empty()) {
            parent = root;
        }
        const auto canonical_parent = std::filesystem::weakly_canonical(parent, error);
        if (error || !path_within(root, canonical_parent)) {
            return false;
        }
        candidate = canonical_parent / candidate.filename();
    } else {
        candidate = std::filesystem::weakly_canonical(candidate, error);
        if (error) {
            return false;
        }
    }
    if (!path_within(root, candidate)) {
        return false;
    }
    result = candidate;
    return true;
}

int32_t read_text_file(const std::filesystem::path& path, uint32_t maximum_bytes,
                       std::string& result) {
    result.clear();
    UniqueHandle handle(CreateFileW(
        path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.valid()) {
        return GetLastError() == ERROR_SHARING_VIOLATION ? SAO_AI_EDITOR_ERR_BUSY
                                                         : SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    FileBinding binding;
    if (!query_file_binding(handle.get(), binding)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if ((binding.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    return read_text_handle(handle.get(), maximum_bytes, result);
}

int32_t read_text_file_bounded(const std::filesystem::path& root, const std::filesystem::path& path,
                               uint32_t maximum_bytes, std::string& result) {
    return read_text_bounded_impl(root, path, maximum_bytes, result);
}

int32_t write_text_atomic(const std::filesystem::path& path, std::string_view content) {
    return write_text_atomic_impl(nullptr, path, content, false);
}

const char* atomic_write_last_stage() noexcept {
    return g_atomic_write_last_stage;
}

uint32_t atomic_write_last_error() noexcept {
    return g_atomic_write_last_error;
}

int32_t write_text_atomic_bounded(const std::filesystem::path& root,
                                  const std::filesystem::path& path, std::string_view content) {
    return write_text_atomic_impl(&root, path, content, false);
}

int32_t write_text_atomic_bounded_if_unchanged(const std::filesystem::path& root,
                                               const std::filesystem::path& path,
                                               std::string_view expected_content,
                                               std::string_view content) {
    return write_text_atomic_impl(&root, path, content, false, &expected_content);
}

int32_t create_text_atomic_bounded(const std::filesystem::path& root,
                                   const std::filesystem::path& path, std::string_view content) {
    return write_text_atomic_impl(&root, path, content, true);
}

int32_t copy_text_to_caller(std::string_view value, char* output, uint32_t capacity,
                            uint32_t* out_length) {
    if (out_length == nullptr || value.size() > UINT32_MAX) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    *out_length = static_cast<uint32_t>(value.size());
    if (output == nullptr || capacity <= value.size()) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    if (!value.empty()) {
        std::memcpy(output, value.data(), value.size());
    }
    output[value.size()] = '\0';
    return SAO_AI_EDITOR_OK;
}

Json rpc_error(const Json& id, int code, std::string_view message, const Json& data) {
    Json error{{"code", code}, {"message", std::string(message)}};
    if (!data.is_null()) {
        error["data"] = data;
    }
    return Json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", std::move(error)},
                {"sao", {{"protocolVersion", 1}}}};
}

Json rpc_result(const Json& id, const Json& result) {
    return Json{
        {"jsonrpc", "2.0"}, {"id", id}, {"result", result}, {"sao", {{"protocolVersion", 1}}}};
}

std::string dump_json(const Json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
}

} // namespace sao::ai_editor::native
