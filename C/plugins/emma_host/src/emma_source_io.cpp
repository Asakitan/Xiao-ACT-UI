#include "emma_source_io.h"

#include <windows.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace sao::plugins::emma_host {
namespace {

class unique_handle final {
  public:
    explicit unique_handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~unique_handle() {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    unique_handle(unique_handle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}

    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE)
                CloseHandle(value_);
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    HANDLE get() const noexcept {
        return value_;
    }

    explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE value_;
};

std::wstring normalized_final_path(HANDLE handle) {
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (required == 0)
        return {};
    std::wstring path(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, path.data(), required, flags);
    if (written == 0 || written >= required)
        return {};
    path.resize(written);
    constexpr std::wstring_view device_prefix = L"\\\\?\\";
    constexpr std::wstring_view unc_prefix = L"\\\\?\\UNC\\";
    if (path.compare(0, unc_prefix.size(), unc_prefix) == 0) {
        path = L"\\\\" + path.substr(unc_prefix.size());
    } else if (path.compare(0, device_prefix.size(), device_prefix) == 0) {
        path.erase(0, device_prefix.size());
    }
    return path;
}

bool handle_is_regular_non_reparse(HANDLE handle) noexcept {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes))) {
        return false;
    }
    return (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool handle_is_directory_non_reparse(HANDLE handle) noexcept {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes))) {
        return false;
    }
    return (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool final_path_is_within(std::wstring_view base, std::wstring_view target) noexcept {
    while (base.size() > 3 && (base.back() == L'\\' || base.back() == L'/'))
        base.remove_suffix(1);
    if (base.empty() || target.size() <= base.size())
        return false;
    if (CompareStringOrdinal(base.data(), static_cast<int>(base.size()), target.data(),
                             static_cast<int>(base.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    return target[base.size()] == L'\\' || target[base.size()] == L'/';
}

} // namespace

int32_t read_emma_source_file(const std::filesystem::path& plugin_root,
                              std::string_view relative_path, std::string& out_source,
                              std::filesystem::path& out_final_path,
                              std::string& out_error_message) noexcept {
    out_source.clear();
    out_final_path.clear();
    out_error_message.clear();
    try {
        if (plugin_root.empty() || relative_path.empty() ||
            relative_path.find('\0') != std::string_view::npos) {
            out_error_message = "Emma source path is invalid";
            return SAO_ERR_INVALID_ARGUMENT;
        }

#if defined(__cpp_char8_t)
        const auto* relative_first = reinterpret_cast<const char8_t*>(relative_path.data());
        const std::filesystem::path relative(relative_first, relative_first + relative_path.size());
#else
        const std::filesystem::path relative =
            std::filesystem::u8path(relative_path.begin(), relative_path.end());
#endif
        const std::filesystem::path normalized_relative = relative.lexically_normal();
        if (normalized_relative.is_absolute() || normalized_relative.has_root_name() ||
            normalized_relative.has_root_directory() ||
            std::find(normalized_relative.begin(), normalized_relative.end(),
                      std::filesystem::path("..")) != normalized_relative.end()) {
            out_error_message = "Emma source path must be relative to the plugin root";
            return SAO_ERR_INVALID_ARGUMENT;
        }

        unique_handle root_handle(CreateFileW(
            plugin_root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!root_handle || !handle_is_directory_non_reparse(root_handle.get())) {
            out_error_message = "Emma plugin root is unavailable or is a reparse point";
            return SAO_ERR_HANDLE_INVALID;
        }
        const std::wstring final_root = normalized_final_path(root_handle.get());
        if (final_root.empty()) {
            out_error_message = "Emma plugin root final path could not be resolved";
            return SAO_ERR_OS_CALL_FAILED;
        }

        std::vector<unique_handle> directory_handles;
        std::filesystem::path current_directory = plugin_root;
        for (auto component = normalized_relative.begin(); component != normalized_relative.end();
             ++component) {
            auto next = component;
            ++next;
            if (next == normalized_relative.end())
                break;
            current_directory /= *component;
            unique_handle directory_handle(CreateFileW(
                current_directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!directory_handle || !handle_is_directory_non_reparse(directory_handle.get())) {
                out_error_message = "Emma source directory is unavailable or is a reparse point";
                return SAO_ERR_HANDLE_INVALID;
            }
            directory_handles.push_back(std::move(directory_handle));
        }

        const std::filesystem::path candidate = plugin_root / normalized_relative;
        unique_handle file_handle(
            CreateFileW(candidate.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file_handle) {
            out_error_message = "Emma source file could not be opened";
            return SAO_ERR_HANDLE_INVALID;
        }
        if (!handle_is_regular_non_reparse(file_handle.get())) {
            out_error_message = "Emma source file is not regular or is a reparse point";
            return SAO_ERR_HANDLE_INVALID;
        }

        const std::wstring final_file = normalized_final_path(file_handle.get());
        if (final_file.empty()) {
            out_error_message = "Emma source final path could not be resolved";
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (!final_path_is_within(final_root, final_file)) {
            out_error_message = "Emma source path escapes the plugin root";
            return SAO_ERR_HANDLE_INVALID;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_handle.get(), &size) || size.QuadPart < 0) {
            out_error_message = "Emma source size could not be read";
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (static_cast<unsigned long long>(size.QuadPart) > kMaximumEmmaSourceBytes) {
            out_error_message = "Emma source exceeds the 8 MiB size limit";
            return SAO_ERR_INVALID_ARGUMENT;
        }

        out_source.resize(static_cast<size_t>(size.QuadPart));
        size_t offset = 0;
        while (offset < out_source.size()) {
            const size_t remaining = out_source.size() - offset;
            const DWORD chunk =
                static_cast<DWORD>(std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
            DWORD read = 0;
            if (!ReadFile(file_handle.get(), out_source.data() + offset, chunk, &read, nullptr) ||
                read == 0) {
                out_source.clear();
                out_error_message = "Emma source file could not be read";
                return SAO_ERR_OS_CALL_FAILED;
            }
            offset += read;
        }
        out_final_path = std::filesystem::path(final_file);
        return SAO_OK;
    } catch (...) {
        out_source.clear();
        out_final_path.clear();
        out_error_message = "Emma source read failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::emma_host
