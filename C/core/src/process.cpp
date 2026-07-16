#include "sao_core/process.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <limits>
#include <new>

struct sao_core_process_s {
    HANDLE handle;
    uint32_t pid;
};

namespace {

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

}  // namespace

extern "C" int32_t SAO_CORE_CALL sao_core_process_open(
    uint32_t pid, sao_core_process_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (pid == 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    constexpr DWORD kReadOnlyAccess = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ;
    HANDLE process_handle = OpenProcess(kReadOnlyAccess, FALSE, static_cast<DWORD>(pid));
    if (process_handle == nullptr) {
        return status_from_last_error(GetLastError());
    }

    auto* process = new (std::nothrow) sao_core_process_s{process_handle, pid};
    if (process == nullptr) {
        CloseHandle(process_handle);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return SAO_ERR_UNKNOWN;
    }
    *out_handle = process;
    return SAO_OK;
}

extern "C" void SAO_CORE_CALL sao_core_process_close(sao_core_process_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
    if (handle->handle != nullptr) {
        CloseHandle(handle->handle);
    }
    delete handle;
}

extern "C" int32_t SAO_CORE_CALL sao_core_process_get_info(
    sao_core_process_handle_t handle,
    SaoCoreProcessInfo* out_info,
    char* out_image_path_utf8,
    size_t image_path_capacity) {
    if (out_info != nullptr) {
        *out_info = {};
    }
    if (out_image_path_utf8 != nullptr && image_path_capacity != 0) {
        out_image_path_utf8[0] = '\0';
    }
    if (handle == nullptr || handle->handle == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (out_info == nullptr || out_image_path_utf8 == nullptr || image_path_capacity == 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (GetProcessTimes(
            handle->handle, &creation_time, &exit_time, &kernel_time, &user_time) == FALSE) {
        return status_from_last_error(GetLastError());
    }

    DWORD session_id = 0;
    if (ProcessIdToSessionId(handle->pid, &session_id) == FALSE) {
        return status_from_last_error(GetLastError());
    }

    uint32_t parent_pid = 0;
    if (!find_parent_pid(handle->pid, &parent_pid)) {
        return status_from_last_error(GetLastError());
    }

    wchar_t image_path[32768]{};
    DWORD image_path_length = static_cast<DWORD>(_countof(image_path));
    if (QueryFullProcessImageNameW(
            handle->handle, 0, image_path, &image_path_length) == FALSE) {
        return status_from_last_error(GetLastError());
    }

    const int utf8_size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        image_path,
        static_cast<int>(image_path_length),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (utf8_size <= 0) {
        return status_from_last_error(GetLastError());
    }
    if (static_cast<size_t>(utf8_size) + 1 > image_path_capacity) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            image_path,
            static_cast<int>(image_path_length),
            out_image_path_utf8,
            utf8_size,
            nullptr,
            nullptr) != utf8_size) {
        out_image_path_utf8[0] = '\0';
        return status_from_last_error(GetLastError());
    }
    out_image_path_utf8[utf8_size] = '\0';

    ULARGE_INTEGER creation{};
    creation.LowPart = creation_time.dwLowDateTime;
    creation.HighPart = creation_time.dwHighDateTime;
    *out_info = SaoCoreProcessInfo{
        handle->pid,
        parent_pid,
        creation.QuadPart,
        session_id,
        0,
    };
    return SAO_OK;
}

extern "C" int32_t SAO_CORE_CALL sao_core_read_bytes(
    sao_core_process_handle_t handle,
    uint64_t address,
    uint8_t* out_buffer,
    size_t buffer_len,
    size_t* out_bytes_read) {
    if (out_bytes_read != nullptr) {
        *out_bytes_read = 0;
    }
    if (out_buffer != nullptr && buffer_len != 0) {
        std::memset(out_buffer, 0, buffer_len);
    }
    if (out_bytes_read == nullptr || (buffer_len != 0 && out_buffer == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (handle == nullptr || handle->handle == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (buffer_len == 0) {
        return SAO_OK;
    }
    if (address > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max())) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    SIZE_T bytes_read = 0;
    const BOOL read_ok = ReadProcessMemory(
        handle->handle,
        reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)),
        out_buffer,
        buffer_len,
        &bytes_read);
    if (read_ok == FALSE || bytes_read != buffer_len) {
        const DWORD error = read_ok == FALSE ? GetLastError() : ERROR_PARTIAL_COPY;
        std::memset(out_buffer, 0, buffer_len);
        *out_bytes_read = 0;
        SetLastError(error);
        return error == ERROR_ACCESS_DENIED ? SAO_ERR_ACCESS_DENIED : SAO_ERR_READ_FAULT;
    }

    *out_bytes_read = static_cast<size_t>(bytes_read);
    return SAO_OK;
}
