#pragma once

#ifndef SAO_RT_IO_HELPER_BUILD
#error "sao/rt_io/backend/driver_r5.h is helper-only"
#endif

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/rt_io/abi.h"
#include "sao/rt_io/status.h"

#ifdef __cplusplus
extern "C" {
#endif

enum SaoRtIoR5Command : int32_t {
    SAO_RT_IO_R5_COMMAND_WRITE = 2,
};

struct SaoRtIoR5CopyPacket {
    uint32_t process_id;
    uint32_t reserved;
    uint64_t source;
    void* destination;
    uint64_t size;
};

struct SaoRtIoR5Packet {
    void* buffer;
    uint64_t size;
    int32_t command;
    int32_t reserved;
};

#ifdef __cplusplus
static_assert(sizeof(void*) == 8);
static_assert(sizeof(SaoRtIoR5CopyPacket) == 32);
static_assert(offsetof(SaoRtIoR5CopyPacket, process_id) == 0);
static_assert(offsetof(SaoRtIoR5CopyPacket, source) == 8);
static_assert(offsetof(SaoRtIoR5CopyPacket, destination) == 16);
static_assert(offsetof(SaoRtIoR5CopyPacket, size) == 24);
static_assert(sizeof(SaoRtIoR5Packet) == 24);
static_assert(offsetof(SaoRtIoR5Packet, buffer) == 0);
static_assert(offsetof(SaoRtIoR5Packet, size) == 8);
static_assert(offsetof(SaoRtIoR5Packet, command) == 16);
#endif

SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_write_memory(
    uint32_t pid, uint64_t address, const uint8_t* data, size_t size);

SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_probe_writer(void);

SAO_RT_IO_API int32_t SAO_RT_IO_CALL sao_rt_io_r5_is_ready(void);
SAO_RT_IO_API void SAO_RT_IO_CALL sao_rt_io_r5_set_ready(int32_t ready);
SAO_RT_IO_API void SAO_RT_IO_CALL sao_rt_io_r5_reset(void);

typedef int32_t(SAO_RT_IO_CALL* sao_rt_io_r5_syscall_hook_t)(
    uint64_t* counter_frequency, struct SaoRtIoR5Packet* packet, void* user);

struct SaoRtIoR5HookBlock {
    sao_rt_io_r5_syscall_hook_t syscall_hook;
    void* user;
};

SAO_RT_IO_API void SAO_RT_IO_CALL
sao_rt_io_r5_install_hooks(const struct SaoRtIoR5HookBlock* hooks);

#ifdef __cplusplus
}
#endif
