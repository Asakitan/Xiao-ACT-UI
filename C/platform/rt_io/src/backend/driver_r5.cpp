#ifndef SAO_RT_IO_HELPER_BUILD
#error "driver_r5.cpp requires SAO_RT_IO_HELPER_BUILD=1"
#endif

#include "sao/rt_io/backend/driver_r5.h"

#include <atomic>
#include <mutex>

#include "sao/rt_io/anti_debug_gate.h"
#include "sao/rt_io/helper/native_syscall.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr uint64_t kCanonicalUserVaMax = 0x00007FFFFFFFFFFFull;

struct R5State {
    std::atomic<int32_t> ready{0};
    std::mutex hook_mutex;
    sao_rt_io_r5_syscall_hook_t syscall_hook = nullptr;
    void* hook_user = nullptr;
};

R5State& state() {
    static R5State value;
    return value;
}

int32_t issue_packet(SaoRtIoR5Packet* packet) {
    if (packet == nullptr)
        return static_cast<int32_t>(0xC000000Du);
    uint64_t counter_frequency = 0u;
    sao_rt_io_r5_syscall_hook_t hook = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(state().hook_mutex);
        hook = state().syscall_hook;
        user = state().hook_user;
    }
    if (hook != nullptr)
        return hook(&counter_frequency, packet, user);
#if defined(_WIN32)
    return sao_rt_io_native_nt_query_auxiliary_counter_frequency(&counter_frequency, packet);
#else
    return static_cast<int32_t>(0xC00000BBu);
#endif
}

sao_status_t issue_write(uint32_t pid, uint64_t address, const uint8_t* data, size_t size) {
    if (pid == 0u || address == 0u || data == nullptr || size == 0u)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (address > kCanonicalUserVaMax || size - 1u > kCanonicalUserVaMax - address)
        return SAO_RT_IO_ERR_TARGET_ADDR_UNREADABLE;
    SaoRtIoR5CopyPacket copy{};
    copy.process_id = pid;
    copy.source = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data));
    copy.destination = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    copy.size = static_cast<uint64_t>(size);
    SaoRtIoR5Packet packet{&copy, sizeof(copy), SAO_RT_IO_R5_COMMAND_WRITE, 0};
    return issue_packet(&packet) >= 0 ? SAO_STATUS_OK : SAO_RT_IO_ERR_WRITE_FAILED;
}

} // namespace

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_write_memory(
    uint32_t pid, uint64_t address, const uint8_t* data, size_t size) {
    try {
        if (state().ready.load(std::memory_order_acquire) == 0)
            return SAO_RT_IO_ERR_DRIVER_NOT_LOADED;
        if (sao_rt_io_gate_is_latched() != 0)
            return SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH;
        return issue_write(pid, address, data, size);
    } catch (...) {
        return SAO_RT_IO_ERR_WRITE_FAILED;
    }
}

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_probe_writer(void) {
    try {
#if defined(_WIN32)
        if (sao_rt_io_gate_is_latched() != 0)
            return SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH;
        alignas(8) volatile uint64_t target = 0xA5D39E7412C68B0Full;
        const uint64_t original = target;
        const uint64_t probe = original ^ 0x6C19F2A73B805DE1ull;
        const uint64_t address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&target));
        const sao_status_t write_status =
            issue_write(::GetCurrentProcessId(), address,
                        reinterpret_cast<const uint8_t*>(&probe), sizeof(probe));
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const bool changed = target == probe;
        const sao_status_t restore_status =
            issue_write(::GetCurrentProcessId(), address,
                        reinterpret_cast<const uint8_t*>(&original), sizeof(original));
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const bool restored = target == original;
        if (!restored)
            return SAO_RT_IO_ERR_MUTATION_UNKNOWN;
        if (write_status != SAO_STATUS_OK || !changed || restore_status != SAO_STATUS_OK)
            return SAO_RT_IO_ERR_DRIVER_NOT_LOADED;
        return SAO_STATUS_OK;
#else
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
    } catch (...) {
        return SAO_RT_IO_ERR_MUTATION_UNKNOWN;
    }
}

extern "C" int32_t SAO_RT_IO_CALL sao_rt_io_r5_is_ready(void) {
    return state().ready.load(std::memory_order_acquire);
}

extern "C" void SAO_RT_IO_CALL sao_rt_io_r5_set_ready(int32_t ready) {
    state().ready.store(ready != 0 ? 1 : 0, std::memory_order_release);
}

extern "C" void SAO_RT_IO_CALL sao_rt_io_r5_reset(void) {
    state().ready.store(0, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> lock(state().hook_mutex);
        state().syscall_hook = nullptr;
        state().hook_user = nullptr;
    } catch (...) {
    }
}

extern "C" void SAO_RT_IO_CALL
sao_rt_io_r5_install_hooks(const SaoRtIoR5HookBlock* hooks) {
    try {
        std::lock_guard<std::mutex> lock(state().hook_mutex);
        if (hooks == nullptr) {
            state().syscall_hook = nullptr;
            state().hook_user = nullptr;
            return;
        }
        state().syscall_hook = hooks->syscall_hook;
        state().hook_user = hooks->user;
    } catch (...) {
        state().ready.store(0, std::memory_order_release);
    }
}
