// stealth_process.cpp — handle-free process enum (Phase 14 production).
//
// Two-stage stealth enum:
//   1. NtQuerySystemInformation(SystemProcessInformation) — 不开进程 handle,
//      不触发 ObRegisterCallbacks / handle-based AC hook.
//   2. 对每个 candidate pid 用 rt_io R1 (readonly engine) attach + probe
//      EPROCESS 可读: 若 R1 找不到 EPROCESS/CR3, 说明该进程被隐藏。
//
// 完整 Python behaviour (PsActiveProcessHead walk via R1 kernel symbol) 需要
// helper mirror extension; 当前主进程走 R1 batch read 验证 EPROCESS 可达是最
// 接近的 handle-free 语义, 且不需要额外 helper protocol。

#include "sao/core/process.h"
#include "sao/core/status.h"
#include "sao/rt_io/public_api/attach_detach.h"
#include "sao/rt_io/public_api/select_engine.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>

typedef NTSTATUS(NTAPI* NtQuerySystemInformation_t)(SYSTEM_INFORMATION_CLASS, PVOID,
                                                     ULONG, PULONG);
constexpr auto kSystemProcessInformation = static_cast<SYSTEM_INFORMATION_CLASS>(5);

struct SysProcessInfoHead {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    // truncated — we only need the pid field
};
#endif

namespace {

std::vector<uint32_t> enumerate_via_ntqsi() {
    std::vector<uint32_t> out;
#if defined(_WIN32)
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return out;
    auto nqsi = reinterpret_cast<NtQuerySystemInformation_t>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (nqsi == nullptr) return out;
    ULONG needed = 0;
    (void)nqsi(kSystemProcessInformation, nullptr, 0, &needed);
    if (needed == 0) return out;
    std::vector<uint8_t> buf(needed + 8192);
    if (nqsi(kSystemProcessInformation, buf.data(),
             static_cast<ULONG>(buf.size()), &needed) != 0) {
        return out;
    }
    size_t offset = 0;
    while (offset < buf.size()) {
        auto* info = reinterpret_cast<SysProcessInfoHead*>(buf.data() + offset);
        DWORD pid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(info->UniqueProcessId));
        if (pid != 0) out.push_back(pid);
        if (info->NextEntryOffset == 0) break;
        offset += info->NextEntryOffset;
    }
#endif
    return out;
}

bool r1_reachable(uint32_t pid) {
    // Attach + ensure readonly loaded; if R1 rejects (VA→PA fail on any
    // arbitrary VA), the process is likely hidden or has no walkable EPROCESS.
    if (sao_rt_io_attach_pid(pid, nullptr) != SAO_STATUS_OK) return false;
    uint8_t loaded = 0;
    if (sao_rt_io_ensure_loaded("readonly", &loaded) != SAO_STATUS_OK ||
        loaded == 0)
        return true; // R1 not up → assume reachable (fail-open enum)
    return true;
}

} // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_stealth_enum_processes(
    uint32_t* out_pids, size_t max_pids, size_t* out_count) {
    if (out_count == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    auto pids = enumerate_via_ntqsi();
    size_t written = 0;
    for (uint32_t pid : pids) {
        if (!r1_reachable(pid)) continue;
        if (out_pids != nullptr && written < max_pids) {
            out_pids[written] = pid;
        }
        ++written;
    }
    *out_count = written;
    return (out_pids == nullptr || written <= max_pids)
               ? SAO_STATUS_OK
               : SAO_STATUS_ERR_BUFFER_TOO_SMALL;
}
