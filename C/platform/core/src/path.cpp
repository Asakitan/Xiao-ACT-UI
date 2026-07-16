#include "sao/core/path.h"

#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>

#include <filesystem>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct BaseDirResult {
    sao_status_t status;
    std::string utf8;
};

std::string path_to_utf8(const fs::path& path) {
    const std::u8string encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

fs::path path_from_utf8(const char* path_utf8) {
    const auto* begin = reinterpret_cast<const char8_t*>(path_utf8);
    return fs::path(std::u8string(begin, begin + std::strlen(path_utf8)));
}

sao_status_t write_utf8_result(
    const std::string& value,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_needed) {
    if (out_bytes_needed == nullptr || (out_utf8 == nullptr && capacity != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_bytes_needed = value.size() + 1;
    if (out_utf8 == nullptr) {
        return SAO_STATUS_OK;
    }
    if (capacity < value.size() + 1) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(out_utf8, value.c_str(), value.size() + 1);
    return SAO_STATUS_OK;
}

fs::path executable_directory() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD copied = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            return fs::path(std::wstring(buffer.data(), copied)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

fs::path environment_base_dir() {
    const DWORD required = GetEnvironmentVariableW(L"SAO_BASE_DIR", nullptr, 0);
    if (required == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(required);
    if (GetEnvironmentVariableW(L"SAO_BASE_DIR", buffer.data(), required) == 0) {
        return {};
    }
    return fs::path(buffer.data());
}

BaseDirResult resolve_base_dir() {
    try {
        fs::path base = environment_base_dir();
        if (base.empty()) {
            const fs::path exe_dir = executable_directory();
            if (exe_dir.empty()) {
                return {SAO_STATUS_ERR_OS_CALL_FAILED, {}};
            }
            std::error_code error;
            const bool has_plugins = fs::is_directory(exe_dir / L"plugins", error);
            if (CompareStringOrdinal(
                    exe_dir.filename().c_str(), -1, L"bin", -1, TRUE) == CSTR_EQUAL) {
                base = exe_dir.parent_path();
            } else {
                base = has_plugins ? exe_dir : exe_dir.parent_path();
            }
        }
        base = fs::absolute(base).lexically_normal().make_preferred();
        std::string encoded = path_to_utf8(base);
        while (encoded.size() > 3 &&
               (encoded.back() == '\\' || encoded.back() == '/')) {
            encoded.pop_back();
        }
        return {SAO_STATUS_OK, std::move(encoded)};
    } catch (...) {
        return {SAO_STATUS_ERR_UNKNOWN, {}};
    }
}

const BaseDirResult& cached_base_dir() {
    static const BaseDirResult result = resolve_base_dir();
    return result;
}

}  // namespace
#endif

extern "C" sao_status_t SAO_CORE_CALL sao_core_path_base_dir(
    char* out_utf8, size_t capacity, size_t* out_bytes_needed) {
#ifdef _WIN32
    const BaseDirResult& result = cached_base_dir();
    if (result.status != SAO_STATUS_OK) {
        if (out_bytes_needed != nullptr) {
            *out_bytes_needed = 0;
        }
        return result.status;
    }
    return write_utf8_result(result.utf8, out_utf8, capacity, out_bytes_needed);
#else
    (void)out_utf8;
    (void)capacity;
    if (out_bytes_needed != nullptr) {
        *out_bytes_needed = 0;
    }
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_path_join(
    const char* const* segments_utf8,
    size_t segment_count,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_needed) {
#ifdef _WIN32
    if (out_bytes_needed == nullptr ||
        (segment_count != 0 && segments_utf8 == nullptr)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        fs::path joined;
        for (size_t index = 0; index < segment_count; ++index) {
            if (segments_utf8[index] == nullptr) {
                *out_bytes_needed = 0;
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            joined /= path_from_utf8(segments_utf8[index]);
        }
        joined = joined.lexically_normal().make_preferred();
        return write_utf8_result(
            path_to_utf8(joined), out_utf8, capacity, out_bytes_needed);
    } catch (...) {
        *out_bytes_needed = 0;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
#else
    (void)segments_utf8;
    (void)segment_count;
    (void)out_utf8;
    (void)capacity;
    if (out_bytes_needed != nullptr) {
        *out_bytes_needed = 0;
    }
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" bool SAO_CORE_CALL sao_core_path_exists(const char* path_utf8) {
#ifdef _WIN32
    if (path_utf8 == nullptr || *path_utf8 == '\0') {
        return false;
    }
    try {
        std::error_code error;
        return fs::exists(path_from_utf8(path_utf8), error) && !error;
    } catch (...) {
        return false;
    }
#else
    (void)path_utf8;
    return false;
#endif
}

extern "C" bool SAO_CORE_CALL sao_core_path_is_directory(const char* path_utf8) {
#ifdef _WIN32
    if (path_utf8 == nullptr || *path_utf8 == '\0') {
        return false;
    }
    try {
        std::error_code error;
        return fs::is_directory(path_from_utf8(path_utf8), error) && !error;
    } catch (...) {
        return false;
    }
#else
    (void)path_utf8;
    return false;
#endif
}

extern "C" bool SAO_CORE_CALL sao_core_path_is_file(const char* path_utf8) {
#ifdef _WIN32
    if (path_utf8 == nullptr || *path_utf8 == '\0') {
        return false;
    }
    try {
        std::error_code error;
        return fs::is_regular_file(path_from_utf8(path_utf8), error) && !error;
    } catch (...) {
        return false;
    }
#else
    (void)path_utf8;
    return false;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_path_make_dirs(
    const char* path_utf8) {
#ifdef _WIN32
    if (path_utf8 == nullptr || *path_utf8 == '\0') {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::error_code error;
        const fs::path path = path_from_utf8(path_utf8);
        if (fs::create_directories(path, error)) {
            return SAO_STATUS_OK;
        }
        if (!error && fs::is_directory(path, error) && !error) {
            return SAO_STATUS_OK;
        }
        return error == std::errc::permission_denied
            ? SAO_STATUS_ERR_ACCESS_DENIED
            : SAO_STATUS_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    (void)path_utf8;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_path_canonicalize(
    const char* path_utf8,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_needed) {
#ifdef _WIN32
    if (path_utf8 == nullptr || *path_utf8 == '\0' || out_bytes_needed == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::error_code error;
        fs::path canonical = fs::canonical(path_from_utf8(path_utf8), error);
        if (error) {
            *out_bytes_needed = 0;
            return error == std::errc::no_such_file_or_directory
                ? SAO_STATUS_ERR_NOT_FOUND
                : SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        canonical.make_preferred();
        return write_utf8_result(
            path_to_utf8(canonical), out_utf8, capacity, out_bytes_needed);
    } catch (...) {
        *out_bytes_needed = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    (void)path_utf8;
    (void)out_utf8;
    (void)capacity;
    if (out_bytes_needed != nullptr) {
        *out_bytes_needed = 0;
    }
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}
