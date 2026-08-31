// SAO Auto — platform/rt_io — R5 driver backend.
//
// R5 is the single SIVX64.sys user-target writer.  Historical `r5p_*`
// symbols and filenames are compatibility spellings for that same driver;
// they do not define another role, backend, or fallback path.  The raw
// physical IOCTL helpers are low-level compatibility primitives: production
// callers first validate an attached-process user range and translate it
// through the controlled R1 page walk.
//
// The same driver also exposes the separately typed HID command ABI.  That
// command surface does not make R5 a generic kernel/physical write backend.
//
// Backend contract:
//   * Helper-only build guard (`SAO_RT_IO_HELPER_BUILD=1`).
//   * All sensitive strings ride through `SAO_ENC_STR()`.
//   * Anti-debug latch is checked on every non-lifecycle entrypoint;
//     once tripped every future write returns
//     `SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH`.
//   * Real device open / NtLoadDriver is bracketed by the test hooks
//     so unit tests never touch a live kernel driver.

#pragma once

#ifndef SAO_RT_IO_HELPER_BUILD
#  error "sao/rt_io/backend/driver_r5.h can only be included in the SaoUiHelper subprocess build (define SAO_RT_IO_HELPER_BUILD=1)."
#endif

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/rt_io/abi.h"
#include "sao/rt_io/status.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Constant surface (visible to tests) ───────────────────────────

// _R5P_CMD_R / _R5P_CMD_W — physical read / write IOCTL codes.  These
// match rt_io.py L1301-1302 after the `_dsi()` XOR decode.
extern const uint32_t kSaoRtIoR5PhysReadIoctl;   // 0x10
extern const uint32_t kSaoRtIoR5PhysWriteIoctl;  // 0x14

enum SaoRtIoR5HidRingOutcome : uint32_t {
    SAO_RT_IO_R5_HID_RING_NOT_SUBMITTED = 0,
    SAO_RT_IO_R5_HID_RING_COMMITTED = 1,
    SAO_RT_IO_R5_HID_RING_UNKNOWN = 2,
};

inline constexpr uint32_t kSaoRtIoR5HidRingAbiVersion = 1u;
inline constexpr uint32_t kSaoRtIoR5HidRingOutcomeSentinel = 0xFFFFFFFFu;
inline constexpr uint32_t kSaoRtIoR5HidRingKeyboardPacketSize = 16u;
inline constexpr uint32_t kSaoRtIoR5HidRingMousePacketSize = 24u;
inline constexpr uint32_t kSaoRtIoR5CommandHidRingQuery = 4u;
inline constexpr uint32_t kSaoRtIoR5CommandHidRingDispatch = 5u;

struct SaoRtIoR5CommandPacket {
    void* buffer;
    uint64_t size;
    uint32_t command;
    uint32_t reserved;
};

struct SaoRtIoR5HidRingQueryPacket {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t outcome;
    uint32_t consumed;
};

struct SaoRtIoR5HidRingDispatchPacket {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t class_devobj_va;
    uint64_t class_service_callback_va;
    uint64_t user_packet_va;
    uint32_t packet_size;
    uint32_t outcome;
    uint32_t consumed;
    uint32_t reserved;
};

static_assert(sizeof(void*) == 8);
static_assert(sizeof(SaoRtIoR5CommandPacket) == 24);
static_assert(offsetof(SaoRtIoR5CommandPacket, buffer) == 0);
static_assert(offsetof(SaoRtIoR5CommandPacket, size) == 8);
static_assert(offsetof(SaoRtIoR5CommandPacket, command) == 16);
static_assert(offsetof(SaoRtIoR5CommandPacket, reserved) == 20);
static_assert(sizeof(SaoRtIoR5HidRingQueryPacket) == 16);
static_assert(offsetof(SaoRtIoR5HidRingQueryPacket, outcome) == 8);
static_assert(offsetof(SaoRtIoR5HidRingQueryPacket, consumed) == 12);
static_assert(sizeof(SaoRtIoR5HidRingDispatchPacket) == 48);
static_assert(offsetof(SaoRtIoR5HidRingDispatchPacket, class_devobj_va) == 8);
static_assert(offsetof(SaoRtIoR5HidRingDispatchPacket, class_service_callback_va) == 16);
static_assert(offsetof(SaoRtIoR5HidRingDispatchPacket, user_packet_va) == 24);
static_assert(offsetof(SaoRtIoR5HidRingDispatchPacket, packet_size) == 32);
static_assert(offsetof(SaoRtIoR5HidRingDispatchPacket, outcome) == 36);
static_assert(offsetof(SaoRtIoR5HidRingDispatchPacket, consumed) == 40);
static_assert(offsetof(SaoRtIoR5HidRingDispatchPacket, reserved) == 44);

// ── Name accessors ────────────────────────────────────────────────
//
// Every runtime string that the driver load orchestration needs is
// materialised through `SAO_ENC_STR()` at call time.  The accessors
// below expose the plaintext to callers that must format a filesystem
// or registry path.  The pointer is valid until the containing
// SaoEncStrBuf goes out of scope — callers copy the bytes if they
// need to hold on to them.  `capacity` includes the trailing NUL.

// R5 SIVX64.sys driver filename.
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_device_name(
    char*  out_utf8,
    size_t out_capacity,
    size_t* out_bytes_written);

// Legacy alias returning the same SIVX64.sys filename.
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5p_device_name(
    char*  out_utf8,
    size_t out_capacity,
    size_t* out_bytes_written);

// _R5P_DEV() — legacy-named R5 device path ("\Device\SIVDRIVER").
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5p_dev_path(
    char*  out_utf8,
    size_t out_capacity,
    size_t* out_bytes_written);

// _dev_nt_path(dos_name) — helper that produces the NT-form
// "\??\<dos_name>" path used by NtCreateFile.  Same helper the Python
// `_r5p_load()` and `_r3_o()` paths use.
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_dev_nt_path(
    const char* dos_name_utf8,
    char*       out_utf8,
    size_t      out_capacity,
    size_t*     out_bytes_written);

// _hid_nt_path(is_mouse, idx) — construct the NT device path for a
// HID mouse (is_mouse == 1) or keyboard (is_mouse == 0) at the given
// slot.  Python rt_io.py L689-691.
//
// Result shape:
//   "\Device\<PointerClass|KeyboardClass><idx>"
//
// Reject `is_mouse` values other than 0/1 with
// `SAO_STATUS_ERR_INVALID_ARGUMENT`.
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_hid_nt_path(
    int32_t   is_mouse,
    uint32_t  idx,
    char*     out_utf8,
    size_t    out_capacity,
    size_t*   out_bytes_written);

// ── R5 orchestration state ────────────────────────────────────────
//
// Tests use these to inspect what the load orchestration touched.  In
// production these entrypoints are called during helper startup only.

// Reset the R5 module state (handle cache + ready flag + latched
// hooks).  Tests call this at the start of every case; production
// calls it during helper shutdown.
SAO_RT_IO_API void SAO_RT_IO_CALL sao_rt_io_r5_reset(void);

// Return 1 iff `_r5_ready` is set — i.e. the load orchestration
// completed the health probe and SIVX64 is answering IOCTLs.
SAO_RT_IO_API int32_t SAO_RT_IO_CALL sao_rt_io_r5_is_ready(void);

// Direct manipulation of the ready flag — tests only.
SAO_RT_IO_API void SAO_RT_IO_CALL sao_rt_io_r5_set_ready(int32_t ready);

// ── Load orchestration (rt_io.py `_r5_setup`) ─────────────────────
//
// Full staging + registry install + NtLoadDriver + health probe.  Runs
// the R5 anti-debug gate first — a latched failure means immediate
// return with SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH.
//
// Tests always intercept the actual load via the hook block below.
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_load_orchestration(
    int32_t* out_used_fallback);   // compatibility output; always 0

// ── Legacy-named SIVX64 read / write IOCTL primitives ─────────────
//
// The two primitives issue `DeviceIoControl` against the R5 handle.
// They are shaped exactly like the Python `_r5p_read` /
// `_r5p_write` helpers — same 8-byte input header, same page-aligned
// output buffer.  A NULL handle returns SAO_RT_IO_ERR_DRIVER_NOT_LOADED.
// A latched debug gate returns SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH.

// _r5p_read(pa, size) — physical read.  Returns exactly `size` bytes
// into `out_buf`; sets `*out_bytes` on completion.
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5p_read_phys(
    void*       driver_handle,   // opaque HANDLE returned by open_probe
    uint64_t    phys_addr,
    uint8_t*    out_buf,
    size_t      size,
    size_t*     out_bytes);

// _r5p_write(pa, data) — physical write.  Rejects zero-length writes.
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5p_write_phys(
    void*        driver_handle,
    uint64_t     phys_addr,
    const uint8_t* data,
    size_t       size);

// _r5_read(pid, addr, size) — walk VA→PA via caller-supplied
// resolver, then chunk through `_r5p_read`.  Tests exercise the
// chunking loop by installing a page-boundary resolver.
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_read_virtual(
    void*       driver_handle,
    uint32_t    pid,
    uint64_t    va,
    uint8_t*    out_buf,
    size_t      size,
    size_t*     out_bytes);

// _r5_write(pid, addr, data) — same walk + `_r5p_write` chunking.
// The installed resolver owns target identity and canonical user-range checks.
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_write_virtual(
    void*        driver_handle,
    uint32_t     pid,
    uint64_t     va,
    const uint8_t* data,
    size_t       size);

// ── Open helper ──────────────────────────────────────────────────
//
// Probe the R5 device using `_dev_nt_path(_R5P_DEV())`.  Returns
// SAO_RT_IO_ERR_DRIVER_NOT_LOADED when the CreateFileW fails.  On
// success `*out_handle` receives the raw void* HANDLE — helper code
// uses it in the legacy-named SIVX64 primitives.  Non-Windows targets
// always return DRIVER_NOT_LOADED.
SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_open_probe(
    void** out_handle);

// Close a previously opened R5 handle.  Idempotent for NULL.
SAO_RT_IO_API void SAO_RT_IO_CALL sao_rt_io_r5_close(void* handle);

// Probe the XiaoACTcontroler syscall command ABI.  Returns 1 only when
// the installed driver explicitly acknowledges the fixed HID Ring ABI; an
// absent/old driver or malformed response returns -1.
SAO_RT_IO_API int32_t SAO_RT_IO_CALL sao_rt_io_r5_hid_ring_query(void);

// Invoke the restricted direct ClassService dispatch.  Return values are
// deliberately tri-state and map the kernel result exactly:
//   1  committed (consumed == 1)
//   0  explicitly rejected / not submitted
//  -1  unknown, absent/old driver, malformed response, or call failure
SAO_RT_IO_API int32_t SAO_RT_IO_CALL sao_rt_io_r5_hid_ring_direct_dispatch(
    uint64_t    class_devobj_va,
    uint64_t    class_service_callback_va,
    const void* packet,
    uint32_t    packet_size);

// ── VA→PA resolver hook ───────────────────────────────────────────
//
// R5 read/write in production walks R1's page-table primitives (`_r1_w`).
// The helper-load stack owns that concern; here we depend on a caller-installed
// resolver so this module compiles and tests in isolation from R1's
// page-table plumbing.

typedef sao_status_t (SAO_RT_IO_CALL *sao_rt_io_va_to_pa_fn_t)(
    uint32_t   pid,
    uint64_t   va,
    uint64_t*  out_pa,
    void*      user);

#define SAO_RT_IO_R5_VA_RESOLVER_DEFAULT_RUNDOWN_TIMEOUT_MS 2000u

typedef enum SaoRtIoR5VaResolverMode {
    SAO_RT_IO_R5_VA_RESOLVER_MODE_NONE = 0u,
    SAO_RT_IO_R5_VA_RESOLVER_MODE_LEGACY = 1u,
    SAO_RT_IO_R5_VA_RESOLVER_MODE_OWNED = 2u,
} SaoRtIoR5VaResolverMode;

typedef struct SaoRtIoR5VaResolverSnapshot {
    uint32_t installed;
    uint32_t mode;
    uint64_t function;
    uint64_t user;
    uint64_t owner_identity;
    uint64_t owner_generation;
    uint64_t revision;
} SaoRtIoR5VaResolverSnapshot;

static_assert(sizeof(SaoRtIoR5VaResolverSnapshot) == 48);
static_assert(offsetof(SaoRtIoR5VaResolverSnapshot, installed) == 0);
static_assert(offsetof(SaoRtIoR5VaResolverSnapshot, mode) == 4);
static_assert(offsetof(SaoRtIoR5VaResolverSnapshot, function) == 8);
static_assert(offsetof(SaoRtIoR5VaResolverSnapshot, user) == 16);
static_assert(offsetof(SaoRtIoR5VaResolverSnapshot, owner_identity) == 24);
static_assert(offsetof(SaoRtIoR5VaResolverSnapshot, owner_generation) == 32);
static_assert(offsetof(SaoRtIoR5VaResolverSnapshot, revision) == 40);

SAO_RT_IO_API void SAO_RT_IO_CALL sao_rt_io_r5_set_va_resolver(
    sao_rt_io_va_to_pa_fn_t fn,
    void*                   user);

SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL
sao_rt_io_r5_install_va_resolver_owned_v2(
    sao_rt_io_va_to_pa_fn_t fn,
    void*                   user,
    const void*             owner_identity,
    uint64_t                owner_generation);

SAO_RT_IO_API sao_status_t SAO_RT_IO_CALL
sao_rt_io_r5_clear_va_resolver_if_owner_v2(
    const void* owner_identity,
    uint64_t    owner_generation,
    uint32_t    timeout_ms,
    int32_t*    out_cleared);

SAO_RT_IO_API void SAO_RT_IO_CALL sao_rt_io_r5_va_resolver_snapshot(
    SaoRtIoR5VaResolverSnapshot* out_snapshot);

// ── DeviceIoControl / NtLoadDriver hooks ─────────────────────────
//
// Tests install these to route the IOCTL and load calls into
// in-memory shims.  When a hook is nullptr the module falls back to
// the real Windows syscall (production path).

typedef int32_t (SAO_RT_IO_CALL *sao_rt_io_r5_ioctl_hook_t)(
    void*    driver_handle,
    uint32_t ioctl_code,
    const void* in_buf,
    uint32_t    in_size,
    void*       out_buf,
    uint32_t    out_size,
    uint32_t*   out_returned,
    void*       user);

typedef sao_status_t (SAO_RT_IO_CALL *sao_rt_io_r5_load_hook_t)(
    int32_t  is_fallback,      // legacy parameter; production always passes 0
    void**   out_driver_handle,
    void*    user);

typedef int32_t (SAO_RT_IO_CALL *sao_rt_io_r5_syscall_hook_t)(
    uint64_t* counter_frequency,
    struct SaoRtIoR5CommandPacket* packet,
    void* user);

struct SaoRtIoR5HookBlock {
    sao_rt_io_r5_ioctl_hook_t ioctl_hook;
    sao_rt_io_r5_load_hook_t  load_hook;
    sao_rt_io_r5_syscall_hook_t syscall_hook;
    void*                     user;
};

SAO_RT_IO_API void SAO_RT_IO_CALL sao_rt_io_r5_install_hooks(
    const struct SaoRtIoR5HookBlock* hooks);   // NULL clears all

#ifdef __cplusplus
}  // extern "C"
#endif
