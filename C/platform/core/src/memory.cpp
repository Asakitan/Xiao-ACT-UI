#include "sao/core/memory.h"

#include "sao/core/error.h"

#include <windows.h>

#include <limits>
#include <mutex>
#include <vector>

HANDLE sao_core_process_native_handle(sao_core_process_handle_t process) noexcept;
bool sao_core_process_has_access(sao_core_process_handle_t process, uint32_t access_flag) noexcept;
void sao_core_set_last_os_error_internal(uint32_t os_error);

namespace {

using NtReadVirtualMemoryFn = LONG(NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(LONG);

constexpr LONG k_status_access_denied = static_cast<LONG>(0xC0000022u);
constexpr LONG k_status_invalid_handle = static_cast<LONG>(0xC0000008u);
constexpr LONG k_status_invalid_cid = static_cast<LONG>(0xC000000Bu);
constexpr LONG k_status_partial_copy = static_cast<LONG>(0x8000000Du);
constexpr LONG k_status_process_is_terminating = static_cast<LONG>(0xC000010Au);

std::once_flag g_nt_read_resolve_once;
NtReadVirtualMemoryFn g_nt_read_virtual_memory = nullptr;
RtlNtStatusToDosErrorFn g_rtl_nt_status_to_dos_error = nullptr;
DWORD g_nt_read_resolve_error = ERROR_SUCCESS;

void resolve_nt_read_virtual_memory() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        g_nt_read_resolve_error = GetLastError();
        return;
    }

    g_nt_read_virtual_memory = reinterpret_cast<NtReadVirtualMemoryFn>(
        GetProcAddress(ntdll, "NtReadVirtualMemory"));
    g_rtl_nt_status_to_dos_error = reinterpret_cast<RtlNtStatusToDosErrorFn>(
        GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (g_nt_read_virtual_memory == nullptr) {
        g_nt_read_resolve_error = ERROR_PROC_NOT_FOUND;
    }
}

DWORD dos_error_from_ntstatus(LONG nt_status) {
    if (g_rtl_nt_status_to_dos_error != nullptr) {
        const ULONG os_error = g_rtl_nt_status_to_dos_error(nt_status);
        if (os_error != ERROR_MR_MID_NOT_FOUND) {
            return static_cast<DWORD>(os_error);
        }
    }
    if (nt_status == k_status_partial_copy) {
        return ERROR_PARTIAL_COPY;
    }
    if (nt_status == k_status_access_denied) {
        return ERROR_ACCESS_DENIED;
    }
    if (nt_status == k_status_invalid_handle) {
        return ERROR_INVALID_HANDLE;
    }
    return ERROR_GEN_FAILURE;
}

sao_status_t memory_status_from_ntstatus(LONG nt_status) {
    if (nt_status == k_status_partial_copy) {
        return SAO_STATUS_ERR_READ_FAULT;
    }
    if (nt_status == k_status_access_denied) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    if (nt_status == k_status_invalid_handle) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (nt_status == k_status_invalid_cid || nt_status == k_status_process_is_terminating) {
        return SAO_STATUS_ERR_PROCESS_GONE;
    }
    return SAO_STATUS_ERR_OS_CALL_FAILED;
}

sao_status_t memory_status_from_os_error(DWORD os_error) {
    if (os_error == ERROR_ACCESS_DENIED) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    if (os_error == ERROR_INVALID_HANDLE) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (os_error == ERROR_INVALID_PARAMETER || os_error == ERROR_NOT_FOUND) {
        return SAO_STATUS_ERR_PROCESS_GONE;
    }
    return SAO_STATUS_ERR_OS_CALL_FAILED;
}

sao_status_t fail_memory(sao_status_t status, DWORD os_error, const char* message) {
    SetLastError(os_error);
    sao_core_set_last_os_error_internal(os_error);
    sao_core_error_set(status, "core.memory", message, __FILE__, __LINE__);
    return status;
}

bool add_signed_offset(uint64_t address, int32_t offset, uint64_t* out_address) {
    const int64_t signed_offset = static_cast<int64_t>(offset);
    if (signed_offset >= 0) {
        const uint64_t delta = static_cast<uint64_t>(signed_offset);
        if (address > std::numeric_limits<uint64_t>::max() - delta) {
            return false;
        }
        *out_address = address + delta;
        return true;
    }

    const uint64_t magnitude = static_cast<uint64_t>(-(signed_offset + 1)) + 1;
    if (address < magnitude) {
        return false;
    }
    *out_address = address - magnitude;
    return true;
}

bool is_readable_protection(DWORD protect) {
    if ((protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protect & 0xFFu) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

struct RegionData {
    SaoMemRegion region;
};

}  // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_core_mem_read(
    sao_core_process_handle_t process,
    uint64_t address,
    void* out_buffer,
    size_t buffer_len,
    size_t* out_bytes_read) {
    if (out_bytes_read == nullptr) {
        return fail_memory(SAO_STATUS_ERR_INVALID_ARGUMENT,
                           ERROR_INVALID_PARAMETER,
                           "out_bytes_read is null");
    }
    *out_bytes_read = 0;
    if (process == nullptr) {
        return fail_memory(SAO_STATUS_ERR_HANDLE_INVALID, ERROR_INVALID_HANDLE, "process handle is null");
    }
    if (buffer_len != 0 && out_buffer == nullptr) {
        return fail_memory(SAO_STATUS_ERR_INVALID_ARGUMENT,
                           ERROR_INVALID_PARAMETER,
                           "memory read buffer is null");
    }
    if (!sao_core_process_has_access(process, SAO_PROCESS_ACCESS_READ)) {
        return fail_memory(SAO_STATUS_ERR_ACCESS_DENIED,
                           ERROR_ACCESS_DENIED,
                           "process handle was not opened for memory reads");
    }
    if (buffer_len == 0) {
        return SAO_STATUS_OK;
    }

    const HANDLE native_handle = sao_core_process_native_handle(process);
    if (native_handle == nullptr) {
        return fail_memory(SAO_STATUS_ERR_HANDLE_INVALID, ERROR_INVALID_HANDLE, "native process handle is invalid");
    }

    std::call_once(g_nt_read_resolve_once, resolve_nt_read_virtual_memory);
    if (g_nt_read_virtual_memory == nullptr) {
        const DWORD os_error = g_nt_read_resolve_error == ERROR_SUCCESS
            ? ERROR_PROC_NOT_FOUND
            : g_nt_read_resolve_error;
        return fail_memory(SAO_STATUS_ERR_OS_CALL_FAILED, os_error, "NtReadVirtualMemory is unavailable");
    }

    SIZE_T bytes_read = 0;
    const LONG nt_status = g_nt_read_virtual_memory(
        native_handle,
        reinterpret_cast<PVOID>(static_cast<uintptr_t>(address)),
        out_buffer,
        buffer_len,
        &bytes_read);
    *out_bytes_read = static_cast<size_t>(bytes_read);

    if (nt_status == 0 && bytes_read == buffer_len) {
        return SAO_STATUS_OK;
    }
    if (nt_status == 0 || nt_status == k_status_partial_copy) {
        return fail_memory(SAO_STATUS_ERR_READ_FAULT,
                           ERROR_PARTIAL_COPY,
                           "NtReadVirtualMemory completed a short read");
    }
    return fail_memory(memory_status_from_ntstatus(nt_status),
                       dos_error_from_ntstatus(nt_status),
                       "NtReadVirtualMemory failed");
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_mem_read_u32(
    sao_core_process_handle_t process, uint64_t address, uint32_t* out_value) {
    if (out_value == nullptr) {
        return fail_memory(SAO_STATUS_ERR_INVALID_ARGUMENT, ERROR_INVALID_PARAMETER, "u32 output is null");
    }
    *out_value = 0;
    size_t bytes_read = 0;
    const sao_status_t status = sao_core_mem_read(process, address, out_value, sizeof(*out_value), &bytes_read);
    if (status != SAO_STATUS_OK || bytes_read != sizeof(*out_value)) {
        *out_value = 0;
        return status == SAO_STATUS_OK
            ? fail_memory(SAO_STATUS_ERR_READ_FAULT, ERROR_PARTIAL_COPY, "u32 read was short")
            : status;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_mem_read_u64(
    sao_core_process_handle_t process, uint64_t address, uint64_t* out_value) {
    if (out_value == nullptr) {
        return fail_memory(SAO_STATUS_ERR_INVALID_ARGUMENT, ERROR_INVALID_PARAMETER, "u64 output is null");
    }
    *out_value = 0;
    size_t bytes_read = 0;
    const sao_status_t status = sao_core_mem_read(process, address, out_value, sizeof(*out_value), &bytes_read);
    if (status != SAO_STATUS_OK || bytes_read != sizeof(*out_value)) {
        *out_value = 0;
        return status == SAO_STATUS_OK
            ? fail_memory(SAO_STATUS_ERR_READ_FAULT, ERROR_PARTIAL_COPY, "u64 read was short")
            : status;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_mem_read_i32(
    sao_core_process_handle_t process, uint64_t address, int32_t* out_value) {
    if (out_value == nullptr) {
        return fail_memory(SAO_STATUS_ERR_INVALID_ARGUMENT, ERROR_INVALID_PARAMETER, "i32 output is null");
    }
    *out_value = 0;
    size_t bytes_read = 0;
    const sao_status_t status = sao_core_mem_read(process, address, out_value, sizeof(*out_value), &bytes_read);
    if (status != SAO_STATUS_OK || bytes_read != sizeof(*out_value)) {
        *out_value = 0;
        return status == SAO_STATUS_OK
            ? fail_memory(SAO_STATUS_ERR_READ_FAULT, ERROR_PARTIAL_COPY, "i32 read was short")
            : status;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_mem_read_ptr(
    sao_core_process_handle_t process, uint64_t address, uint64_t* out_value) {
    return sao_core_mem_read_u64(process, address, out_value);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_mem_read_pointer_chain(
    sao_core_process_handle_t process,
    uint64_t base_address,
    const int32_t* offsets,
    size_t offset_count,
    uint64_t* out_final_address) {
    if (out_final_address == nullptr || (offset_count != 0 && offsets == nullptr)) {
        return fail_memory(SAO_STATUS_ERR_INVALID_ARGUMENT,
                           ERROR_INVALID_PARAMETER,
                           "pointer chain arguments are invalid");
    }
    *out_final_address = 0;
    if (offset_count == 0) {
        *out_final_address = base_address;
        return SAO_STATUS_OK;
    }

    uint64_t current_address = base_address;
    for (size_t index = 0; index < offset_count; ++index) {
        uint64_t pointer_value = 0;
        const sao_status_t read_status = sao_core_mem_read_ptr(process, current_address, &pointer_value);
        if (read_status != SAO_STATUS_OK) {
            return read_status;
        }
        if (index + 1 == offset_count) {
            if (!add_signed_offset(pointer_value, offsets[index], out_final_address)) {
                return fail_memory(SAO_STATUS_ERR_INVALID_ARGUMENT,
                                   ERROR_ARITHMETIC_OVERFLOW,
                                   "final pointer offset overflows the address range");
            }
            return SAO_STATUS_OK;
        }
        if (!add_signed_offset(pointer_value, offsets[index], &current_address)) {
            return fail_memory(SAO_STATUS_ERR_INVALID_ARGUMENT,
                               ERROR_ARITHMETIC_OVERFLOW,
                               "pointer offset overflows the address range");
        }
    }

    return fail_memory(SAO_STATUS_ERR_UNKNOWN, ERROR_GEN_FAILURE, "pointer chain resolution failed");
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_mem_enum_regions(
    sao_core_process_handle_t process,
    SaoMemRegion* out_regions,
    size_t max_regions,
    size_t* out_region_count) {
    if (out_region_count == nullptr) {
        return fail_memory(SAO_STATUS_ERR_INVALID_ARGUMENT,
                           ERROR_INVALID_PARAMETER,
                           "out_region_count is null");
    }
    *out_region_count = 0;
    if (process == nullptr) {
        return fail_memory(SAO_STATUS_ERR_HANDLE_INVALID, ERROR_INVALID_HANDLE, "process handle is null");
    }
    const HANDLE native_handle = sao_core_process_native_handle(process);
    if (native_handle == nullptr) {
        return fail_memory(SAO_STATUS_ERR_HANDLE_INVALID, ERROR_INVALID_HANDLE, "native process handle is invalid");
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const uintptr_t maximum_address = reinterpret_cast<uintptr_t>(system_info.lpMaximumApplicationAddress);
    uintptr_t address = reinterpret_cast<uintptr_t>(system_info.lpMinimumApplicationAddress);
    std::vector<RegionData> regions;

    while (address <= maximum_address) {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T result = VirtualQueryEx(
            native_handle, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi));
        if (result == 0) {
            const DWORD os_error = GetLastError();
            return fail_memory(memory_status_from_os_error(os_error),
                               os_error,
                               "VirtualQueryEx failed");
        }
        if (mbi.RegionSize == 0) {
            return fail_memory(SAO_STATUS_ERR_OS_CALL_FAILED,
                               ERROR_INVALID_DATA,
                               "VirtualQueryEx returned a zero-sized region");
        }

        if (mbi.State == MEM_COMMIT && is_readable_protection(mbi.Protect)) {
            regions.push_back(RegionData{SaoMemRegion{
                reinterpret_cast<uint64_t>(mbi.BaseAddress),
                static_cast<uint64_t>(mbi.RegionSize),
                mbi.Protect,
                mbi.State,
                mbi.Type,
                0,
            }});
        }

        const uintptr_t region_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (mbi.RegionSize > std::numeric_limits<uintptr_t>::max() - region_base) {
            break;
        }
        const uintptr_t next_address = region_base + mbi.RegionSize;
        if (next_address <= address || next_address > maximum_address) {
            break;
        }
        address = next_address;
    }

    *out_region_count = regions.size();
    if (out_regions == nullptr) {
        return SAO_STATUS_OK;
    }
    if (max_regions < regions.size()) {
        return fail_memory(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                           ERROR_INSUFFICIENT_BUFFER,
                           "memory region buffer is too small");
    }
    for (size_t index = 0; index < regions.size(); ++index) {
        out_regions[index] = regions[index].region;
    }
    return SAO_STATUS_OK;
}
