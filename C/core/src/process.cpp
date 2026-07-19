#include "sao_core/process.h"

#include "logging.h"

#include <windows.h>

#include <tlhelp32.h>

#include <cstring>
#include <limits>
#include <new>

struct sao_legacy_core_process_s {
    HANDLE handle;
    uint32_t pid;
};

namespace {

constexpr char kComponent[] = "core.process";

int32_t fail(int32_t status, const char* message) noexcept {
    sao::legacy_core::emit_log(sao::legacy_core::kLogLevelError, kComponent, status, message);
    return status;
}

void lifecycle(int32_t status, const char* message) noexcept {
    sao::legacy_core::emit_log(status == SAO_OK ? sao::legacy_core::kLogLevelInfo
                                                : sao::legacy_core::kLogLevelError,
                               kComponent, status, message);
}

int32_t status_from_last_error(DWORD error) noexcept {
    if (error == ERROR_ACCESS_DENIED) {
        return SAO_ERR_ACCESS_DENIED;
    }
    if (error == ERROR_INVALID_HANDLE) {
        return SAO_ERR_HANDLE_INVALID;
    }
    return SAO_ERR_OS_CALL_FAILED;
}

bool find_parent_pid(uint32_t pid, uint32_t* out_parent_pid) noexcept {
    *out_parent_pid = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) == FALSE) {
        const DWORD error = GetLastError();
        CloseHandle(snapshot);
        SetLastError(error);
        return false;
    }

    do {
        if (entry.th32ProcessID == pid) {
            *out_parent_pid = entry.th32ParentProcessID;
            CloseHandle(snapshot);
            return true;
        }
        entry.dwSize = sizeof(entry);
    } while (Process32NextW(snapshot, &entry) != FALSE);

    const DWORD error = GetLastError() == ERROR_NO_MORE_FILES ? ERROR_NOT_FOUND : GetLastError();
    CloseHandle(snapshot);
    SetLastError(error);
    return false;
}

} // namespace

extern "C" int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_process_open(uint32_t pid, sao_legacy_core_process_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "process_open requires an output handle");
    }
    *out_handle = nullptr;
    if (pid == 0) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "process_open received an invalid process id");
    }

    constexpr DWORD kReadOnlyAccess = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ;
    HANDLE process_handle = OpenProcess(kReadOnlyAccess, FALSE, static_cast<DWORD>(pid));
    if (process_handle == nullptr) {
        return fail(status_from_last_error(GetLastError()),
                    "process_open failed to open the process");
    }

    auto* process = new (std::nothrow) sao_legacy_core_process_s{process_handle, pid};
    if (process == nullptr) {
        CloseHandle(process_handle);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return fail(SAO_ERR_UNKNOWN, "process_open failed to allocate its handle");
    }
    *out_handle = process;
    lifecycle(SAO_OK, "process_open completed");
    return SAO_OK;
}

extern "C" void SAO_LEGACY_CORE_CALL
sao_legacy_core_process_close(sao_legacy_core_process_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
    bool close_succeeded = true;
    if (handle->handle != nullptr) {
        close_succeeded = CloseHandle(handle->handle) != FALSE;
    }
    delete handle;
    lifecycle(close_succeeded ? SAO_OK : SAO_ERR_OS_CALL_FAILED,
              close_succeeded ? "process_close completed"
                              : "process_close failed to close the OS handle");
}

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_process_get_info(
    sao_legacy_core_process_handle_t handle, SaoLegacyCoreProcessInfo* out_info,
    char* out_image_path_utf8, size_t image_path_capacity) {
    if (out_info != nullptr) {
        *out_info = {};
    }
    if (out_image_path_utf8 != nullptr && image_path_capacity != 0) {
        out_image_path_utf8[0] = '\0';
    }
    if (handle == nullptr || handle->handle == nullptr) {
        return fail(SAO_ERR_HANDLE_INVALID, "process_get_info received an invalid handle");
    }
    if (out_info == nullptr || out_image_path_utf8 == nullptr || image_path_capacity == 0) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "process_get_info received invalid output arguments");
    }

    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (GetProcessTimes(handle->handle, &creation_time, &exit_time, &kernel_time, &user_time) ==
        FALSE) {
        return fail(status_from_last_error(GetLastError()),
                    "process_get_info failed to read process times");
    }

    DWORD session_id = 0;
    if (ProcessIdToSessionId(handle->pid, &session_id) == FALSE) {
        return fail(status_from_last_error(GetLastError()),
                    "process_get_info failed to read the session id");
    }

    uint32_t parent_pid = 0;
    if (!find_parent_pid(handle->pid, &parent_pid)) {
        return fail(status_from_last_error(GetLastError()),
                    "process_get_info failed to resolve the parent process");
    }

    wchar_t image_path[32768]{};
    DWORD image_path_length = static_cast<DWORD>(_countof(image_path));
    if (QueryFullProcessImageNameW(handle->handle, 0, image_path, &image_path_length) == FALSE) {
        return fail(status_from_last_error(GetLastError()),
                    "process_get_info failed to query the image path");
    }

    const int utf8_size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, image_path,
                            static_cast<int>(image_path_length), nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0) {
        return fail(status_from_last_error(GetLastError()),
                    "process_get_info failed to size the UTF-8 image path");
    }
    if (static_cast<size_t>(utf8_size) + 1 > image_path_capacity) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return fail(SAO_ERR_BUFFER_TOO_SMALL, "process_get_info image path buffer is too small");
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, image_path,
                            static_cast<int>(image_path_length), out_image_path_utf8, utf8_size,
                            nullptr, nullptr) != utf8_size) {
        out_image_path_utf8[0] = '\0';
        return fail(status_from_last_error(GetLastError()),
                    "process_get_info failed to encode the image path");
    }
    out_image_path_utf8[utf8_size] = '\0';

    ULARGE_INTEGER creation{};
    creation.LowPart = creation_time.dwLowDateTime;
    creation.HighPart = creation_time.dwHighDateTime;
    *out_info = SaoLegacyCoreProcessInfo{
        handle->pid, parent_pid, creation.QuadPart, session_id, 0,
    };
    return SAO_OK;
}

extern "C" int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_read_bytes(sao_legacy_core_process_handle_t handle, uint64_t address,
                           uint8_t* out_buffer, size_t buffer_len, size_t* out_bytes_read) {
    if (out_bytes_read != nullptr) {
        *out_bytes_read = 0;
    }
    if (out_buffer != nullptr && buffer_len != 0) {
        std::memset(out_buffer, 0, buffer_len);
    }
    if (out_bytes_read == nullptr || (buffer_len != 0 && out_buffer == nullptr)) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "read_bytes received invalid output arguments");
    }
    if (handle == nullptr || handle->handle == nullptr) {
        return fail(SAO_ERR_HANDLE_INVALID, "read_bytes received an invalid process handle");
    }
    if (buffer_len == 0) {
        return SAO_OK;
    }
    if (address > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max())) {
        return fail(SAO_ERR_INVALID_ARGUMENT,
                    "read_bytes address is outside the native address range");
    }

    SIZE_T bytes_read = 0;
    const BOOL read_ok = ReadProcessMemory(
        handle->handle, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), out_buffer,
        buffer_len, &bytes_read);
    if (read_ok == FALSE || bytes_read != buffer_len) {
        const DWORD error = read_ok == FALSE ? GetLastError() : ERROR_PARTIAL_COPY;
        std::memset(out_buffer, 0, buffer_len);
        *out_bytes_read = 0;
        SetLastError(error);
        return fail(error == ERROR_ACCESS_DENIED ? SAO_ERR_ACCESS_DENIED : SAO_ERR_READ_FAULT,
                    "read_bytes failed to read the complete range");
    }

    *out_bytes_read = static_cast<size_t>(bytes_read);
    return SAO_OK;
}
