#include "sao/core/process.h"

#include "sao/core/error.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <cwchar>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

struct sao_core_process_s {
    HANDLE native_handle;
    uint32_t pid;
    uint32_t access_flags;
};

void sao_core_set_last_os_error_internal(uint32_t os_error);

namespace {

const wchar_t* image_base_name(const wchar_t* value) {
    const wchar_t* base_name = value;
    for (const wchar_t* cursor = value; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') {
            base_name = cursor + 1;
        }
    }
    return base_name;
}

bool is_equal_case_insensitive(const wchar_t* left, const wchar_t* right) {
    return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

sao_status_t process_status_from_os_error(DWORD os_error) {
    if (os_error == ERROR_ACCESS_DENIED) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    if (os_error == ERROR_INVALID_HANDLE) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (os_error == ERROR_INVALID_PARAMETER || os_error == ERROR_NOT_FOUND ||
        os_error == ERROR_PARTIAL_COPY) {
        return SAO_STATUS_ERR_PROCESS_GONE;
    }
    return SAO_STATUS_ERR_OS_CALL_FAILED;
}

sao_status_t fail_process(sao_status_t status, DWORD os_error, const char* message) {
    SetLastError(os_error);
    sao_core_set_last_os_error_internal(os_error);
    sao_core_error_set(status, "core.process", message, "platform/core/src/process.cpp", __LINE__);
    return status;
}

sao_status_t fail_process_os(DWORD os_error, const char* message) {
    return fail_process(process_status_from_os_error(os_error), os_error, message);
}

bool wide_to_utf8(const wchar_t* source, std::string& output, DWORD* out_error) {
    const int source_length = static_cast<int>(wcslen(source));
    if (source_length == 0) {
        output.clear();
        return true;
    }

    const int bytes_needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, source, source_length, nullptr, 0, nullptr, nullptr);
    if (bytes_needed <= 0) {
        *out_error = GetLastError();
        return false;
    }

    output.resize(static_cast<size_t>(bytes_needed));
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            source,
                            source_length,
                            output.data(),
                            bytes_needed,
                            nullptr,
                            nullptr) != bytes_needed) {
        *out_error = GetLastError();
        return false;
    }
    return true;
}

bool find_parent_pid(uint32_t pid, uint32_t* out_parent_pid, DWORD* out_error) {
    *out_parent_pid = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        *out_error = GetLastError();
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    BOOL has_entry = Process32FirstW(snapshot, &entry);
    if (has_entry == FALSE) {
        *out_error = GetLastError();
        CloseHandle(snapshot);
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

    CloseHandle(snapshot);
    *out_error = ERROR_NOT_FOUND;
    return false;
}

bool query_image_path_utf8(HANDLE process, std::string& image_path, DWORD* out_error) {
    std::vector<wchar_t> wide_path(32768);
    DWORD length = static_cast<DWORD>(wide_path.size());
    if (QueryFullProcessImageNameW(process, 0, wide_path.data(), &length) == FALSE) {
        *out_error = GetLastError();
        return false;
    }
    wide_path[static_cast<size_t>(length)] = L'\0';
    return wide_to_utf8(wide_path.data(), image_path, out_error);
}

bool create_module_snapshot(uint32_t pid, HANDLE* out_snapshot, DWORD* out_error) {
    for (int attempt = 0; attempt != 3; ++attempt) {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                          pid);
        if (snapshot != INVALID_HANDLE_VALUE) {
            *out_snapshot = snapshot;
            return true;
        }
        *out_error = GetLastError();
        if (*out_error != ERROR_BAD_LENGTH) {
            return false;
        }
    }
    return false;
}

struct ModuleData {
    uint64_t base_address;
    uint64_t module_size;
    std::string name;
};

bool collect_modules(uint32_t pid, std::vector<ModuleData>& modules, DWORD* out_error) {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    if (!create_module_snapshot(pid, &snapshot, out_error)) {
        return false;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    BOOL has_entry = Module32FirstW(snapshot, &entry);
    if (has_entry == FALSE) {
        *out_error = GetLastError();
        CloseHandle(snapshot);
        return false;
    }

    do {
        std::string name;
        if (!wide_to_utf8(entry.szModule, name, out_error)) {
            CloseHandle(snapshot);
            return false;
        }
        modules.push_back(ModuleData{
            reinterpret_cast<uint64_t>(entry.modBaseAddr),
            static_cast<uint64_t>(entry.modBaseSize),
            std::move(name),
        });
        entry.dwSize = sizeof(entry);
    } while (Module32NextW(snapshot, &entry) != FALSE);

    const DWORD last_error = GetLastError();
    CloseHandle(snapshot);
    if (last_error != ERROR_NO_MORE_FILES) {
        *out_error = last_error;
        return false;
    }
    return true;
}

}  // namespace

HANDLE sao_core_process_native_handle(sao_core_process_handle_t process) noexcept {
    return process != nullptr ? process->native_handle : nullptr;
}

bool sao_core_process_has_access(sao_core_process_handle_t process, uint32_t access_flag) noexcept {
    return process != nullptr && (process->access_flags & access_flag) == access_flag;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_process_open(
    uint32_t pid, uint32_t access_flags,
    sao_core_process_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return fail_process(SAO_STATUS_ERR_INVALID_ARGUMENT, ERROR_INVALID_PARAMETER, "out_handle is null");
    }
    *out_handle = nullptr;
    if (pid == 0 || access_flags == 0 ||
        (access_flags & ~(SAO_PROCESS_ACCESS_INFO | SAO_PROCESS_ACCESS_READ)) != 0) {
        return fail_process(SAO_STATUS_ERR_INVALID_ARGUMENT,
                            ERROR_INVALID_PARAMETER,
                            "invalid process id or access flags");
    }

    DWORD desired_access = PROCESS_QUERY_LIMITED_INFORMATION;
    if ((access_flags & SAO_PROCESS_ACCESS_READ) != 0) {
        desired_access |= PROCESS_VM_READ;
    }

    const HANDLE native_handle = OpenProcess(desired_access, FALSE, pid);
    if (native_handle == nullptr) {
        return fail_process_os(GetLastError(), "OpenProcess failed");
    }

    sao_core_process_s* process = new (std::nothrow) sao_core_process_s{
        native_handle,
        pid,
        access_flags,
    };
    if (process == nullptr) {
        CloseHandle(native_handle);
        return fail_process(SAO_STATUS_ERR_UNKNOWN, ERROR_NOT_ENOUGH_MEMORY, "process handle allocation failed");
    }

    *out_handle = process;
    return SAO_STATUS_OK;
}

extern "C" void SAO_CORE_CALL sao_core_process_close(sao_core_process_handle_t process) {
    if (process == nullptr) {
        return;
    }
    if (process->native_handle != nullptr) {
        CloseHandle(process->native_handle);
    }
    delete process;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_process_enumerate(
    uint32_t* out_pids, size_t max_pids, size_t* out_pid_count) {
    if (out_pid_count == nullptr) {
        return fail_process(SAO_STATUS_ERR_INVALID_ARGUMENT,
                            ERROR_INVALID_PARAMETER,
                            "out_pid_count is null");
    }
    *out_pid_count = 0;

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return fail_process_os(GetLastError(), "CreateToolhelp32Snapshot failed");
    }

    std::vector<uint32_t> pids;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) == FALSE) {
        const DWORD os_error = GetLastError();
        CloseHandle(snapshot);
        return fail_process_os(os_error, "Process32FirstW failed");
    }

    do {
        if (entry.th32ProcessID != 0) {
            pids.push_back(entry.th32ProcessID);
        }
        entry.dwSize = sizeof(entry);
    } while (Process32NextW(snapshot, &entry) != FALSE);

    const DWORD last_error = GetLastError();
    CloseHandle(snapshot);
    if (last_error != ERROR_NO_MORE_FILES) {
        return fail_process_os(last_error, "Process32NextW failed");
    }

    *out_pid_count = pids.size();
    if (out_pids == nullptr) {
        return SAO_STATUS_OK;
    }
    if (max_pids < pids.size()) {
        return fail_process(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                            ERROR_INSUFFICIENT_BUFFER,
                            "process id buffer is too small");
    }
    for (size_t index = 0; index < pids.size(); ++index) {
        out_pids[index] = pids[index];
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_process_find_by_name(
    const wchar_t* requested_image_name, uint32_t* out_pid) {
    if (out_pid == nullptr || requested_image_name == nullptr || *requested_image_name == L'\0') {
        return fail_process(SAO_STATUS_ERR_INVALID_ARGUMENT,
                            ERROR_INVALID_PARAMETER,
                            "image name or output process id is invalid");
    }
    *out_pid = 0;
    const wchar_t* expected_name = image_base_name(requested_image_name);

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return fail_process_os(GetLastError(), "CreateToolhelp32Snapshot failed");
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) == FALSE) {
        const DWORD os_error = GetLastError();
        CloseHandle(snapshot);
        return fail_process_os(os_error, "Process32FirstW failed");
    }

    do {
        if (entry.th32ProcessID != 0 && is_equal_case_insensitive(entry.szExeFile, expected_name)) {
            *out_pid = entry.th32ProcessID;
            CloseHandle(snapshot);
            return SAO_STATUS_OK;
        }
        entry.dwSize = sizeof(entry);
    } while (Process32NextW(snapshot, &entry) != FALSE);

    const DWORD last_error = GetLastError();
    CloseHandle(snapshot);
    if (last_error != ERROR_NO_MORE_FILES) {
        return fail_process_os(last_error, "Process32NextW failed");
    }
    return fail_process(SAO_STATUS_ERR_NOT_FOUND, ERROR_NOT_FOUND, "process image name was not found");
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_process_get_info(
    sao_core_process_handle_t process,
    SaoProcessInfo* out_info,
    char* out_image_path_utf8,
    size_t image_path_capacity) {
    if (process == nullptr) {
        return fail_process(SAO_STATUS_ERR_HANDLE_INVALID, ERROR_INVALID_HANDLE, "process handle is null");
    }
    if (out_info == nullptr || out_image_path_utf8 == nullptr || image_path_capacity == 0) {
        return fail_process(SAO_STATUS_ERR_INVALID_ARGUMENT,
                            ERROR_INVALID_PARAMETER,
                            "process info output buffer is invalid");
    }
    *out_info = {};
    out_image_path_utf8[0] = '\0';

    const HANDLE native_handle = process->native_handle;
    if (native_handle == nullptr) {
        return fail_process(SAO_STATUS_ERR_HANDLE_INVALID, ERROR_INVALID_HANDLE, "native process handle is invalid");
    }

    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (GetProcessTimes(native_handle, &creation_time, &exit_time, &kernel_time, &user_time) == FALSE) {
        return fail_process_os(GetLastError(), "GetProcessTimes failed");
    }

    DWORD session_id = 0;
    if (ProcessIdToSessionId(process->pid, &session_id) == FALSE) {
        return fail_process_os(GetLastError(), "ProcessIdToSessionId failed");
    }

    uint32_t parent_pid = 0;
    DWORD os_error = ERROR_SUCCESS;
    if (!find_parent_pid(process->pid, &parent_pid, &os_error)) {
        return fail_process_os(os_error, "parent process lookup failed");
    }

    std::string image_path;
    if (!query_image_path_utf8(native_handle, image_path, &os_error)) {
        return fail_process_os(os_error, "QueryFullProcessImageNameW failed");
    }
    if (image_path.size() + 1 > image_path_capacity) {
        return fail_process(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                            ERROR_INSUFFICIENT_BUFFER,
                            "image path buffer is too small");
    }

    ULARGE_INTEGER creation{};
    creation.LowPart = creation_time.dwLowDateTime;
    creation.HighPart = creation_time.dwHighDateTime;
    *out_info = SaoProcessInfo{
        process->pid,
        parent_pid,
        creation.QuadPart,
        session_id,
        0,
    };
    memcpy(out_image_path_utf8, image_path.c_str(), image_path.size() + 1);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_process_enum_modules(
    sao_core_process_handle_t process,
    SaoModuleEntry* out_entries,
    size_t max_entries,
    size_t* out_entry_count,
    char* out_names_utf8,
    size_t names_capacity,
    size_t* out_names_used) {
    if (process == nullptr) {
        return fail_process(SAO_STATUS_ERR_HANDLE_INVALID, ERROR_INVALID_HANDLE, "process handle is null");
    }
    if (out_entry_count == nullptr || out_names_used == nullptr) {
        return fail_process(SAO_STATUS_ERR_INVALID_ARGUMENT,
                            ERROR_INVALID_PARAMETER,
                            "module enumeration count output is null");
    }
    *out_entry_count = 0;
    *out_names_used = 0;
    if ((out_entries == nullptr) != (out_names_utf8 == nullptr)) {
        return fail_process(SAO_STATUS_ERR_INVALID_ARGUMENT,
                            ERROR_INVALID_PARAMETER,
                            "module entries and names buffers must be supplied together");
    }

    std::vector<ModuleData> modules;
    DWORD os_error = ERROR_SUCCESS;
    if (!collect_modules(process->pid, modules, &os_error)) {
        return fail_process_os(os_error, "module snapshot enumeration failed");
    }

    size_t names_used = 0;
    for (const ModuleData& module : modules) {
        if (module.name.size() > std::numeric_limits<size_t>::max() - names_used - 1) {
            return fail_process(SAO_STATUS_ERR_UNKNOWN, ERROR_ARITHMETIC_OVERFLOW, "module name size overflow");
        }
        names_used += module.name.size() + 1;
    }

    *out_entry_count = modules.size();
    *out_names_used = names_used;
    if (out_entries == nullptr) {
        return SAO_STATUS_OK;
    }
    if (max_entries < modules.size() || names_capacity < names_used) {
        return fail_process(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                            ERROR_INSUFFICIENT_BUFFER,
                            "module output buffer is too small");
    }

    size_t name_offset = 0;
    for (size_t index = 0; index < modules.size(); ++index) {
        const ModuleData& module = modules[index];
        out_entries[index] = SaoModuleEntry{
            module.base_address,
            module.module_size,
            static_cast<uint32_t>(name_offset),
            0,
        };
        memcpy(out_names_utf8 + name_offset, module.name.c_str(), module.name.size() + 1);
        name_offset += module.name.size() + 1;
    }
    return SAO_STATUS_OK;
}
