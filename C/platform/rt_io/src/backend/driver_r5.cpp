// SAO Auto — platform/rt_io — R5 backend impl.
//
// Ports the Python R5 driver family (`_R5*` / `_R5P_*` / `_r5_*` /
// `_r5p_*` / `_hid_nt_path`) into helper-only C++.  See
// `include/sao/rt_io/backend/driver_r5.h` for the surface contract.
//
// Backend contract:
//   * `SAO_RT_IO_HELPER_BUILD=1` must be defined on the target — the
//     header enforces it via `#error`.
//   * Sensitive strings ride through `SAO_ENC_STR()`.
//   * Anti-debug latch is consulted on every non-lifecycle entry.
//   * DeviceIoControl / NtLoadDriver are hookable so tests never
//     touch a real driver.

#ifndef SAO_RT_IO_HELPER_BUILD
#  error "driver_r5.cpp requires SAO_RT_IO_HELPER_BUILD=1 on the target"
#endif

#include "sao/rt_io/backend/driver_r5.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "sao/rt_io/anti_debug_gate.h"
#include "sao/rt_io/helper/native_syscall.h"
#include "sao/rt_io/io_dispatch.h"
#include "sao/rt_io/util/ioctl_pacer.h"
#include "sao_security/obfuscation/enc_str.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
using HANDLE = void*;
static constexpr HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(-1);
#endif

// ─────────────────────────── Constants ────────────────────────────

// _R5P_CMD_R = 0x10 / _R5P_CMD_W = 0x14.  Kept as `extern` so tests
// can prove the decrypted values match Python.
extern "C" const uint32_t kSaoRtIoR5PhysReadIoctl  = 0x10u;
extern "C" const uint32_t kSaoRtIoR5PhysWriteIoctl = 0x14u;

// R5P read output buffer floor — Python `_r5p_read` allocates at
// least a page even when the request is smaller.
static constexpr size_t kR5PMinReadBuf = 0x1000;

// Page size + mask.  Same 4 KiB assumption as Python's page walker.
static constexpr size_t   kPageSize = 0x1000;
static constexpr uint64_t kPageMask = 0xFFFULL;

// ─────────────────────────── Utilities ────────────────────────────

namespace {

// Copy an already-decrypted SAO_ENC_STR into a caller-provided buffer,
// producing the standard (out_utf8, out_capacity, out_bytes_written)
// tuple return.
sao_status_t emit_string_out(
    const char* src, size_t src_len,
    char* out_utf8, size_t out_capacity, size_t* out_bytes_written) {
    if (out_utf8 == nullptr || out_capacity == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_capacity < src_len + 1) {
        if (out_bytes_written != nullptr) *out_bytes_written = src_len;
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(out_utf8, src, src_len);
    out_utf8[src_len] = '\0';
    if (out_bytes_written != nullptr) *out_bytes_written = src_len;
    return SAO_STATUS_OK;
}

// ── Global state ──────────────────────────────────────────────────

struct R5VaResolverSlot {
    std::mutex mutation_mutex;
    std::mutex state_mutex;
    std::condition_variable cv;
    sao_rt_io_va_to_pa_fn_t fn = nullptr;
    void* user = nullptr;
    const void* owner_identity = nullptr;
    uint64_t owner_generation = 0u;
    uint64_t generation_high_water = 0u;
    uint64_t revision = 0u;
    uint64_t revision_high_water = 0u;
    uint32_t mode = SAO_RT_IO_R5_VA_RESOLVER_MODE_NONE;
    bool admission_open = false;
    uint32_t in_flight = 0u;
    bool installed = false;
};

struct R5State {
    std::atomic<int32_t> ready{0};
    sao_rt_io_r5_ioctl_hook_t ioctl_hook = nullptr;
    sao_rt_io_r5_load_hook_t  load_hook  = nullptr;
    sao_rt_io_r5_syscall_hook_t syscall_hook = nullptr;
    void*                     hook_user  = nullptr;
    R5VaResolverSlot          va_resolver;
    std::mutex                hook_mutex;
};

R5State& state() {
    static R5State s;
    return s;
}

struct HookSnapshot {
    sao_rt_io_r5_ioctl_hook_t ioctl_hook;
    sao_rt_io_r5_load_hook_t  load_hook;
    sao_rt_io_r5_syscall_hook_t syscall_hook;
    void*                     user;
};

HookSnapshot snapshot_hooks() {
    HookSnapshot h{};
    R5State& s = state();
    std::lock_guard<std::mutex> lk(s.hook_mutex);
    h.ioctl_hook = s.ioctl_hook;
    h.load_hook  = s.load_hook;
    h.syscall_hook = s.syscall_hook;
    h.user       = s.hook_user;
    return h;
}

thread_local R5VaResolverSlot* g_active_r5_va_resolver_slot = nullptr;
thread_local uint32_t g_active_r5_va_resolver_depth = 0u;

struct R5VaResolverStateSnapshot {
    sao_rt_io_va_to_pa_fn_t fn = nullptr;
    void* user = nullptr;
    const void* owner_identity = nullptr;
    uint64_t owner_generation = 0u;
    uint64_t generation_high_water = 0u;
    uint64_t revision = 0u;
    uint64_t revision_high_water = 0u;
    uint32_t mode = SAO_RT_IO_R5_VA_RESOLVER_MODE_NONE;
    bool admission_open = false;
    bool installed = false;
};

R5VaResolverStateSnapshot snapshot_va_resolver_state(
    const R5VaResolverSlot& slot) noexcept {
    return {slot.fn,
            slot.user,
            slot.owner_identity,
            slot.owner_generation,
            slot.generation_high_water,
            slot.revision,
            slot.revision_high_water,
            slot.mode,
            slot.admission_open,
            slot.installed};
}

void restore_va_resolver_state(
    R5VaResolverSlot& slot,
    const R5VaResolverStateSnapshot& snapshot) noexcept {
    slot.fn = snapshot.fn;
    slot.user = snapshot.user;
    slot.owner_identity = snapshot.owner_identity;
    slot.owner_generation = snapshot.owner_generation;
    slot.generation_high_water = snapshot.generation_high_water;
    slot.revision = snapshot.revision;
    slot.revision_high_water = snapshot.revision_high_water;
    slot.mode = snapshot.mode;
    slot.admission_open = snapshot.admission_open;
    slot.installed = snapshot.installed;
}

bool va_resolver_tuple_matches(
    const R5VaResolverSlot& slot,
    const R5VaResolverStateSnapshot& snapshot) noexcept {
    return slot.fn == snapshot.fn &&
           slot.user == snapshot.user &&
           slot.owner_identity == snapshot.owner_identity &&
           slot.owner_generation == snapshot.owner_generation &&
           slot.generation_high_water == snapshot.generation_high_water &&
           slot.revision == snapshot.revision &&
           slot.revision_high_water == snapshot.revision_high_water &&
           slot.mode == snapshot.mode &&
           slot.installed == snapshot.installed;
}

bool next_counter(uint64_t high_water, uint64_t* out_value) noexcept {
    if (out_value == nullptr || high_water == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    const uint64_t candidate = high_water + 1u;
    if (candidate == 0u) {
        return false;
    }
    *out_value = candidate;
    return true;
}

uint64_t function_bits(sao_rt_io_va_to_pa_fn_t fn) noexcept {
    uint64_t bits = 0u;
    static_assert(sizeof(fn) <= sizeof(bits));
    std::memcpy(&bits, &fn, sizeof(fn));
    return bits;
}

class R5VaResolverLease {
  public:
    R5VaResolverLease() noexcept {
        try {
            R5VaResolverSlot& slot = state().va_resolver;
            std::lock_guard<std::mutex> lock(slot.state_mutex);
            if (!slot.installed || !slot.admission_open || slot.fn == nullptr) {
                return;
            }
            fn_ = slot.fn;
            user_ = slot.user;
            owner_identity_ = slot.owner_identity;
            owner_generation_ = slot.owner_generation;
            slot_ = &slot;
            ++slot.in_flight;
            if (g_active_r5_va_resolver_slot == slot_) {
                ++g_active_r5_va_resolver_depth;
            } else {
                g_active_r5_va_resolver_slot = slot_;
                g_active_r5_va_resolver_depth = 1u;
            }
            acquired_ = true;
        } catch (...) {
        }
    }

    R5VaResolverLease(const R5VaResolverLease&) = delete;
    R5VaResolverLease& operator=(const R5VaResolverLease&) = delete;

    ~R5VaResolverLease() noexcept {
        release();
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

    sao_rt_io_va_to_pa_fn_t fn() const noexcept {
        return fn_;
    }

    void* user() const noexcept {
        return user_;
    }

    const void* owner_identity() const noexcept {
        return owner_identity_;
    }

    uint64_t owner_generation() const noexcept {
        return owner_generation_;
    }

  private:
    void release() noexcept {
        if (!acquired_ || slot_ == nullptr) {
            return;
        }
        if (g_active_r5_va_resolver_slot == slot_ &&
            g_active_r5_va_resolver_depth != 0u) {
            --g_active_r5_va_resolver_depth;
            if (g_active_r5_va_resolver_depth == 0u) {
                g_active_r5_va_resolver_slot = nullptr;
            }
        }
        try {
            std::lock_guard<std::mutex> lock(slot_->state_mutex);
            if (slot_->in_flight != 0u) {
                --slot_->in_flight;
            }
            if (slot_->in_flight == 0u) {
                slot_->cv.notify_all();
            }
        } catch (...) {
        }
        acquired_ = false;
    }

    R5VaResolverSlot* slot_ = nullptr;
    sao_rt_io_va_to_pa_fn_t fn_ = nullptr;
    void* user_ = nullptr;
    const void* owner_identity_ = nullptr;
    uint64_t owner_generation_ = 0u;
    bool acquired_ = false;
};

sao_status_t invoke_va_resolver(
    uint32_t pid, uint64_t va, uint64_t* out_pa) noexcept {
    if (out_pa == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_pa = 0u;
    R5VaResolverLease lease;
    if (!lease) {
        return SAO_RT_IO_ERR_INTERNAL_ERROR;
    }
    try {
        return lease.fn()(pid, va, out_pa, lease.user());
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t r5_rebind_va_resolver(
    sao_rt_io_va_to_pa_fn_t fn,
    void* user,
    const void* owner_identity,
    uint64_t owner_generation,
    bool legacy) noexcept {
    R5VaResolverSlot* slot_ptr = nullptr;
    std::unique_lock<std::mutex> mutation_lock;
    std::unique_lock<std::mutex> state_lock;
    R5VaResolverStateSnapshot prior{};
    bool admission_changed = false;
    try {
        slot_ptr = &state().va_resolver;
        R5VaResolverSlot& slot = *slot_ptr;
        if (g_active_r5_va_resolver_slot == slot_ptr &&
            g_active_r5_va_resolver_depth != 0u) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }

        mutation_lock = std::unique_lock<std::mutex>(slot.mutation_mutex);
        state_lock = std::unique_lock<std::mutex>(slot.state_mutex);

        if (legacy) {
            if (slot.installed && slot.mode == SAO_RT_IO_R5_VA_RESOLVER_MODE_OWNED) {
                return SAO_STATUS_ERR_ALREADY_EXISTS;
            }
            if (fn != nullptr && slot.installed &&
                slot.mode == SAO_RT_IO_R5_VA_RESOLVER_MODE_LEGACY &&
                slot.admission_open && slot.fn == fn && slot.user == user) {
                return SAO_STATUS_OK;
            }
            if (fn == nullptr && (!slot.installed ||
                                  slot.mode != SAO_RT_IO_R5_VA_RESOLVER_MODE_LEGACY)) {
                return SAO_STATUS_OK;
            }
        } else {
            if (fn == nullptr || owner_identity == nullptr || owner_generation == 0u) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            if (slot.installed) {
                if (slot.mode != SAO_RT_IO_R5_VA_RESOLVER_MODE_OWNED ||
                    slot.owner_identity != owner_identity ||
                    slot.owner_generation != owner_generation) {
                    return SAO_STATUS_ERR_ALREADY_EXISTS;
                }
                if (slot.fn == fn && slot.user == user && slot.admission_open) {
                    return SAO_STATUS_OK;
                }
                return SAO_STATUS_ERR_ALREADY_EXISTS;
            }
            if (owner_generation <= slot.generation_high_water) {
                return SAO_STATUS_ERR_ALREADY_EXISTS;
            }
        }

        uint64_t candidate_generation = 0u;
        if (fn != nullptr) {
            if (legacy) {
                if (!next_counter(slot.generation_high_water, &candidate_generation)) {
                    return SAO_RT_IO_ERR_INTERNAL_ERROR;
                }
            } else {
                candidate_generation = owner_generation;
            }
        }
        uint64_t candidate_revision = 0u;
        if (!next_counter(slot.revision_high_water, &candidate_revision)) {
            return SAO_RT_IO_ERR_INTERNAL_ERROR;
        }

        prior = snapshot_va_resolver_state(slot);
        slot.admission_open = false;
        admission_changed = true;
        if (!slot.cv.wait_for(
                state_lock,
                std::chrono::milliseconds(kR5VaResolverRundownTimeoutMs),
                [&slot] { return slot.in_flight == 0u; })) {
            restore_va_resolver_state(slot, prior);
            admission_changed = false;
            return SAO_STATUS_ERR_TIMEOUT;
        }
        if (!va_resolver_tuple_matches(slot, prior)) {
            restore_va_resolver_state(slot, prior);
            admission_changed = false;
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }

        slot.revision = candidate_revision;
        slot.revision_high_water = candidate_revision;
        if (fn == nullptr) {
            slot.fn = nullptr;
            slot.user = nullptr;
            slot.owner_identity = nullptr;
            slot.owner_generation = 0u;
            slot.mode = SAO_RT_IO_R5_VA_RESOLVER_MODE_NONE;
            slot.installed = false;
            slot.admission_open = false;
        } else {
            slot.fn = fn;
            slot.user = user;
            slot.owner_identity = legacy ? nullptr : owner_identity;
            slot.owner_generation = candidate_generation;
            slot.generation_high_water = candidate_generation;
            slot.mode = legacy ? SAO_RT_IO_R5_VA_RESOLVER_MODE_LEGACY
                               : SAO_RT_IO_R5_VA_RESOLVER_MODE_OWNED;
            slot.installed = true;
            slot.admission_open = true;
        }
        admission_changed = false;
        return SAO_STATUS_OK;
    } catch (...) {
        if (slot_ptr != nullptr && state_lock.owns_lock() && admission_changed) {
            restore_va_resolver_state(*slot_ptr, prior);
        }
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t r5_clear_va_resolver_if_owner(
    const void* owner_identity,
    uint64_t owner_generation,
    uint32_t timeout_ms,
    int32_t* out_cleared) noexcept {
    if (out_cleared != nullptr) {
        *out_cleared = 0;
    }
    if (owner_identity == nullptr || owner_generation == 0u ||
        timeout_ms > kR5VaResolverRundownTimeoutMs) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    R5VaResolverSlot* slot_ptr = nullptr;
    std::unique_lock<std::mutex> mutation_lock;
    std::unique_lock<std::mutex> state_lock;
    R5VaResolverStateSnapshot prior{};
    bool admission_changed = false;
    try {
        slot_ptr = &state().va_resolver;
        R5VaResolverSlot& slot = *slot_ptr;
        if (g_active_r5_va_resolver_slot == slot_ptr &&
            g_active_r5_va_resolver_depth != 0u) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }

        mutation_lock = std::unique_lock<std::mutex>(slot.mutation_mutex);
        state_lock = std::unique_lock<std::mutex>(slot.state_mutex);
        if (!slot.installed || slot.mode != SAO_RT_IO_R5_VA_RESOLVER_MODE_OWNED ||
            slot.owner_identity != owner_identity ||
            slot.owner_generation != owner_generation) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        uint64_t candidate_revision = 0u;
        if (!next_counter(slot.revision_high_water, &candidate_revision)) {
            return SAO_RT_IO_ERR_INTERNAL_ERROR;
        }

        prior = snapshot_va_resolver_state(slot);
        slot.admission_open = false;
        admission_changed = true;
        if (!slot.cv.wait_for(
                state_lock,
                std::chrono::milliseconds(timeout_ms),
                [&slot] { return slot.in_flight == 0u; })) {
            restore_va_resolver_state(slot, prior);
            admission_changed = false;
            return SAO_STATUS_ERR_TIMEOUT;
        }
        if (!va_resolver_tuple_matches(slot, prior)) {
            restore_va_resolver_state(slot, prior);
            admission_changed = false;
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }

        slot.fn = nullptr;
        slot.user = nullptr;
        slot.owner_identity = nullptr;
        slot.owner_generation = 0u;
        slot.mode = SAO_RT_IO_R5_VA_RESOLVER_MODE_NONE;
        slot.installed = false;
        slot.admission_open = false;
        slot.revision = candidate_revision;
        slot.revision_high_water = candidate_revision;
        admission_changed = false;
        if (out_cleared != nullptr) {
            *out_cleared = 1;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        if (slot_ptr != nullptr && state_lock.owns_lock() && admission_changed) {
            restore_va_resolver_state(*slot_ptr, prior);
        }
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

// Gate helper — Python's `_r5p_write` guard.  A latched debug gate
// means every future write is denied.  Reads are also denied so a
// compromised process cannot exfiltrate memory via a poked handle.
bool gate_permits() {
    return sao_rt_io_gate_is_latched() == 0;
}

// Real DeviceIoControl (production path).  Extracted so the hook
// dispatch stays symmetric.
bool real_ioctl(void* driver_handle,
                uint32_t ioctl_code,
                const void* in_buf, uint32_t in_size,
                void* out_buf, uint32_t out_size,
                uint32_t* out_returned) {
#if defined(_WIN32)
    if (driver_handle == nullptr) return false;
    // 2026-07 hardening: shape R5 (SIVX64) IOCTL burst.  SIV UI drives
    // both reads and writes so classify as OTHER (op-weight table in
    // the SIV_LIKE profile is uniform).
    sao_rt_io_ioctl_pacer_before_default(SAO_RT_IO_IOCTL_PACER_ROLE_R5,
                                         SAO_RT_IO_IOCTL_OP_OTHER);
    uint32_t returned = 0u;
    const int32_t ok = sao_rt_io_dispatch_d2(
        driver_handle,
        ioctl_code,
        const_cast<void*>(in_buf),
        in_size,
        out_buf,
        out_size,
        &returned);
    sao_rt_io_ioctl_pacer_after_default(SAO_RT_IO_IOCTL_PACER_ROLE_R5,
                                        SAO_RT_IO_IOCTL_OP_OTHER);
    if (out_returned != nullptr) *out_returned = returned;
    return ok != 0;
#else
    (void)driver_handle; (void)ioctl_code; (void)in_buf; (void)in_size;
    (void)out_buf; (void)out_size; (void)out_returned;
    return false;
#endif
}

// Dispatch — hook first, real syscall second.
bool dispatch_ioctl(void* driver_handle,
                    uint32_t ioctl_code,
                    const void* in_buf, uint32_t in_size,
                    void* out_buf, uint32_t out_size,
                    uint32_t* out_returned) {
    const auto h = snapshot_hooks();
    if (h.ioctl_hook != nullptr) {
        const int32_t rc = h.ioctl_hook(
            driver_handle, ioctl_code, in_buf, in_size,
            out_buf, out_size, out_returned, h.user);
        return rc != 0;
    }
    return real_ioctl(driver_handle, ioctl_code,
                      in_buf, in_size, out_buf, out_size, out_returned);
}

int32_t real_r5_syscall(uint64_t* counter_frequency,
                       SaoRtIoR5CommandPacket* packet) {
#if defined(_WIN32)
    if (counter_frequency == nullptr || packet == nullptr) {
        return static_cast<int32_t>(0xC000007Au);
    }
    return sao_rt_io_native_nt_query_auxiliary_counter_frequency(
        counter_frequency, packet);
#else
    (void)counter_frequency;
    (void)packet;
    return -1;
#endif
}

int32_t dispatch_r5_syscall(SaoRtIoR5CommandPacket* packet) {
    uint64_t counter_frequency = 0;
    const auto hooks = snapshot_hooks();
    if (hooks.syscall_hook != nullptr) {
        return hooks.syscall_hook(
            &counter_frequency, packet, hooks.user);
    }
    return real_r5_syscall(&counter_frequency, packet);
}

// Compose the R5P write payload buffer.  Mirrors `_r5p_write`'s
// packing (Python rt_io.py L3373-3384).
//   [0x00]  QWORD  page_pa
//   [0x08]  DWORD  0x1000              (page size)
//   [0x0E]  WORD   0x02                (protection/tag byte, offset 0x0E)
//   [0x14]  WORD   n_entries
//   [0x30 + i*24]  DWORD  offset_within_page
//   [0x34 + i*24]  DWORD  mask
//   [0x38 + i*24]  DWORD  value
//   (16 bytes trailer — 3 entries+6) = layout matches Python
struct R5PWriteEntry {
    uint32_t offset;
    uint32_t mask;
    uint32_t value;
};

std::vector<uint8_t> pack_write_buffer(
    uint64_t page_pa,
    const std::vector<R5PWriteEntry>& entries) {
    const size_t n = entries.size();
    const size_t buf_sz = (n * 3 + 6) * 8;
    std::vector<uint8_t> buf(buf_sz, 0u);
    std::memcpy(buf.data() + 0x00, &page_pa, sizeof(uint64_t));
    const uint32_t page_size_dw = 0x1000u;
    std::memcpy(buf.data() + 0x08, &page_size_dw, sizeof(uint32_t));
    const uint16_t tag_word = 0x0002u;
    std::memcpy(buf.data() + 0x0E, &tag_word, sizeof(uint16_t));
    const uint16_t n_ent = static_cast<uint16_t>(n);
    std::memcpy(buf.data() + 0x14, &n_ent, sizeof(uint16_t));
    for (size_t i = 0; i < n; ++i) {
        const size_t base = 0x30 + i * 24;
        std::memcpy(buf.data() + base + 0, &entries[i].offset, sizeof(uint32_t));
        std::memcpy(buf.data() + base + 4, &entries[i].mask,   sizeof(uint32_t));
        std::memcpy(buf.data() + base + 8, &entries[i].value,  sizeof(uint32_t));
    }
    return buf;
}

}  // namespace

// ─────────────────────── Public entry points ──────────────────────

// ── Name accessors ────────────────────────────────────────────────

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_device_name(
    char*  out_utf8,
    size_t out_capacity,
    size_t* out_bytes_written) {
    // Active R5 user-mode writer filename.
    const auto s = SAO_ENC_STR("SIVX64.sys");
    const char* src = s.decrypt();
    return emit_string_out(src, s.size(), out_utf8, out_capacity, out_bytes_written);
}

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5p_device_name(
    char*  out_utf8,
    size_t out_capacity,
    size_t* out_bytes_written) {
    // Python `_R5P_F() = "SIVX64.sys"`.
    const auto s = SAO_ENC_STR("SIVX64.sys");
    const char* src = s.decrypt();
    return emit_string_out(src, s.size(), out_utf8, out_capacity, out_bytes_written);
}

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5p_dev_path(
    char*  out_utf8,
    size_t out_capacity,
    size_t* out_bytes_written) {
    // Python `_R5P_DEV() = "SIVDRIVER"`.
    const auto s = SAO_ENC_STR("SIVDRIVER");
    const char* src = s.decrypt();
    return emit_string_out(src, s.size(), out_utf8, out_capacity, out_bytes_written);
}

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_dev_nt_path(
    const char* dos_name_utf8,
    char*       out_utf8,
    size_t      out_capacity,
    size_t*     out_bytes_written) {
    if (dos_name_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // Python `_dev_nt_path(dos_name) = "\??\" + dos_name`.
    const auto prefix = SAO_ENC_STR("\\??\\");
    const char* prefix_ptr = prefix.decrypt();
    const size_t prefix_len = prefix.size();
    const size_t dos_len    = std::strlen(dos_name_utf8);
    const size_t total_len  = prefix_len + dos_len;

    if (out_utf8 == nullptr || out_capacity == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_capacity < total_len + 1) {
        if (out_bytes_written != nullptr) *out_bytes_written = total_len;
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(out_utf8, prefix_ptr, prefix_len);
    std::memcpy(out_utf8 + prefix_len, dos_name_utf8, dos_len);
    out_utf8[total_len] = '\0';
    if (out_bytes_written != nullptr) *out_bytes_written = total_len;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_hid_nt_path(
    int32_t   is_mouse,
    uint32_t  idx,
    char*     out_utf8,
    size_t    out_capacity,
    size_t*   out_bytes_written) {
    if (is_mouse != 0 && is_mouse != 1) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_utf8 == nullptr || out_capacity == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    // Python `_hid_nt_path`:
    //   prefix = "\\Device\\"
    //   cls    = "PointerClass" or "KeyboardClass"
    //   suffix = str(idx)
    const auto prefix = SAO_ENC_STR("\\Device\\");
    const char* prefix_ptr = prefix.decrypt();
    const size_t prefix_len = prefix.size();

    // Materialise the class token via SAO_ENC_STR so it never appears
    // as a plaintext ".rdata" literal.
    const char*  cls_ptr  = nullptr;
    size_t       cls_len  = 0;
    // The two SaoEncStrBuf instances must both stay alive past the
    // subsequent copy, so we capture pointers up-front.
    const auto ptr_cls = SAO_ENC_STR("PointerClass");
    const auto kbd_cls = SAO_ENC_STR("KeyboardClass");
    if (is_mouse == 1) {
        cls_ptr = ptr_cls.decrypt();
        cls_len = ptr_cls.size();
    } else {
        cls_ptr = kbd_cls.decrypt();
        cls_len = kbd_cls.size();
    }

    // idx → decimal.  A 32-bit value never needs more than 10 chars.
    char idx_buf[16];
    const int idx_written = std::snprintf(idx_buf, sizeof(idx_buf), "%u",
                                          static_cast<unsigned int>(idx));
    if (idx_written <= 0) return SAO_STATUS_ERR_UNKNOWN;
    const size_t idx_len = static_cast<size_t>(idx_written);

    const size_t total_len = prefix_len + cls_len + idx_len;
    if (out_capacity < total_len + 1) {
        if (out_bytes_written != nullptr) *out_bytes_written = total_len;
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    size_t off = 0;
    std::memcpy(out_utf8 + off, prefix_ptr, prefix_len); off += prefix_len;
    std::memcpy(out_utf8 + off, cls_ptr,    cls_len);    off += cls_len;
    std::memcpy(out_utf8 + off, idx_buf,    idx_len);    off += idx_len;
    out_utf8[off] = '\0';
    if (out_bytes_written != nullptr) *out_bytes_written = total_len;
    return SAO_STATUS_OK;
}

// ── R5 ready flag ─────────────────────────────────────────────────

extern "C" void SAO_RT_IO_CALL sao_rt_io_r5_reset(void) {
    try {
        R5State& s = state();
        (void)r5_rebind_va_resolver(nullptr, nullptr, nullptr, 0u, true);
        s.ready.store(0, std::memory_order_release);
        std::lock_guard<std::mutex> lk(s.hook_mutex);
        s.ioctl_hook = nullptr;
        s.load_hook  = nullptr;
        s.syscall_hook = nullptr;
        s.hook_user  = nullptr;
    } catch (...) {
    }
}

extern "C" int32_t SAO_RT_IO_CALL sao_rt_io_r5_is_ready(void) {
    return state().ready.load(std::memory_order_acquire);
}

extern "C" void SAO_RT_IO_CALL sao_rt_io_r5_set_ready(int32_t ready) {
    state().ready.store(ready ? 1 : 0, std::memory_order_release);
}

// ── Load orchestration ────────────────────────────────────────────

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_load_orchestration(
    int32_t* out_used_fallback) {
    if (out_used_fallback != nullptr) *out_used_fallback = 0;

    if (!sao_rt_io_gate_run_all()) {
        return SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH;
    }

    const auto h = snapshot_hooks();
    if (h.load_hook == nullptr) {
        return SAO_RT_IO_ERR_HELPER_NOT_LAUNCHED;
    }

    void* handle = nullptr;
    const sao_status_t status = h.load_hook(/*legacy_is_fallback=*/0, &handle, h.user);
    if (status == SAO_STATUS_OK && handle != nullptr) {
        state().ready.store(1, std::memory_order_release);
        return SAO_STATUS_OK;
    }
    return SAO_RT_IO_ERR_DRIVER_NOT_LOADED;
}

// ── R5P read / write ──────────────────────────────────────────────

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5p_read_phys(
    void*       driver_handle,
    uint64_t    phys_addr,
    uint8_t*    out_buf,
    size_t      size,
    size_t*     out_bytes) {
    if (out_bytes != nullptr) *out_bytes = 0;
    if (out_buf == nullptr || size == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (driver_handle == nullptr) return SAO_RT_IO_ERR_DRIVER_NOT_LOADED;
    if (!gate_permits()) return SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH;

    // Python `_r5p_read` allocates `max(size, 0x1000)` for the output.
    const size_t out_sz = (size < kR5PMinReadBuf) ? kR5PMinReadBuf : size;
    std::vector<uint8_t> scratch(out_sz, 0u);

    // Input payload = 8-byte little-endian phys address.
    uint8_t in_buf[8];
    std::memcpy(in_buf, &phys_addr, sizeof(uint64_t));

    uint32_t returned = 0;
    const bool ok = dispatch_ioctl(driver_handle,
                                   kSaoRtIoR5PhysReadIoctl,
                                   in_buf, 8,
                                   scratch.data(),
                                   static_cast<uint32_t>(out_sz),
                                   &returned);
    if (!ok) return SAO_RT_IO_ERR_READ_FAILED;
    if (returned < size) return SAO_RT_IO_ERR_READ_FAILED;

    std::memcpy(out_buf, scratch.data(), size);
    if (out_bytes != nullptr) *out_bytes = size;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5p_write_phys(
    void*        driver_handle,
    uint64_t     phys_addr,
    const uint8_t* data,
    size_t       size) {
    if (data == nullptr || size == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (driver_handle == nullptr) return SAO_RT_IO_ERR_DRIVER_NOT_LOADED;
    if (!gate_permits()) return SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH;

    size_t off = 0;
    while (off < size) {
        const uint64_t page_pa  = (phys_addr + off) & ~kPageMask;
        const uint64_t page_off = (phys_addr + off) & kPageMask;
        const size_t chunk_hi   = kPageSize - static_cast<size_t>(page_off);
        const size_t remaining  = size - off;
        const size_t chunk      = (remaining < chunk_hi) ? remaining : chunk_hi;

        // Build the entry list.  Python packs one entry per 4 / 2 / 1
        // byte slice.  For the fully-aligned fast path we only emit
        // DWORD entries; sub-DWORD tails read the current DWORD via
        // `_r5p_read`, patch, and write back.
        std::vector<R5PWriteEntry> entries;
        size_t pos = 0;
        while (pos < chunk) {
            const size_t remain = chunk - pos;
            R5PWriteEntry ent{};
            ent.offset = static_cast<uint32_t>(page_off + pos);
            ent.mask   = 0u;
            if (remain >= 4) {
                uint32_t v = 0;
                std::memcpy(&v, data + off + pos, sizeof(uint32_t));
                ent.value = v;
                entries.push_back(ent);
                pos += 4;
            } else if (remain >= 2) {
                uint16_t v = 0;
                std::memcpy(&v, data + off + pos, sizeof(uint16_t));
                uint8_t cur[4] = {0};
                size_t got = 0;
                const sao_status_t rs = sao_rt_io_r5p_read_phys(
                    driver_handle,
                    page_pa + page_off + pos,
                    cur, 4, &got);
                if (rs != SAO_STATUS_OK || got != 4) {
                    return SAO_RT_IO_ERR_READ_FAILED;
                }
                uint32_t cur_v = 0;
                std::memcpy(&cur_v, cur, sizeof(uint32_t));
                const uint32_t new_v = (cur_v & 0xFFFF0000u) | v;
                ent.value = new_v;
                entries.push_back(ent);
                pos += 2;
            } else {
                uint8_t cur[4] = {0};
                size_t got = 0;
                const sao_status_t rs = sao_rt_io_r5p_read_phys(
                    driver_handle,
                    page_pa + page_off + pos,
                    cur, 4, &got);
                if (rs != SAO_STATUS_OK || got != 4) {
                    return SAO_RT_IO_ERR_READ_FAILED;
                }
                uint32_t cur_v = 0;
                std::memcpy(&cur_v, cur, sizeof(uint32_t));
                const uint32_t new_v = (cur_v & 0xFFFFFF00u)
                                     | static_cast<uint32_t>(data[off + pos]);
                ent.value = new_v;
                entries.push_back(ent);
                pos += 1;
            }
        }

        const std::vector<uint8_t> buf = pack_write_buffer(page_pa, entries);
        std::vector<uint8_t> reply(buf.size(), 0u);
        uint32_t returned = 0;
        const bool ok = dispatch_ioctl(
            driver_handle,
            kSaoRtIoR5PhysWriteIoctl,
            buf.data(),
            static_cast<uint32_t>(buf.size()),
            reply.data(),
            static_cast<uint32_t>(reply.size()),
            &returned);
        if (!ok) return SAO_RT_IO_ERR_WRITE_FAILED;

        off += chunk;
    }
    return SAO_STATUS_OK;
}

// ── R5 read / write via VA resolver ───────────────────────────────

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_read_virtual(
    void*       driver_handle,
    uint32_t    pid,
    uint64_t    va,
    uint8_t*    out_buf,
    size_t      size,
    size_t*     out_bytes) {
    if (out_bytes != nullptr) *out_bytes = 0;
    if (out_buf == nullptr || size == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (driver_handle == nullptr) return SAO_RT_IO_ERR_DRIVER_NOT_LOADED;
    if (!gate_permits()) return SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH;
    try {
        size_t remaining = size;
        size_t written   = 0;
        uint64_t cur     = va;
        while (remaining > 0) {
            uint64_t pa = 0;
            const sao_status_t st = invoke_va_resolver(pid, cur, &pa);
            if (st != SAO_STATUS_OK) return st;
            const uint64_t page_off = cur & kPageMask;
            const size_t chunk_hi   = kPageSize - static_cast<size_t>(page_off);
            const size_t chunk      = (remaining < chunk_hi) ? remaining : chunk_hi;
            size_t got = 0;
            const sao_status_t rs = sao_rt_io_r5p_read_phys(
                driver_handle, pa, out_buf + written, chunk, &got);
            if (rs != SAO_STATUS_OK || got != chunk) return rs;
            written   += chunk;
            remaining -= chunk;
            cur       += chunk;
        }
        if (out_bytes != nullptr) *out_bytes = written;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_write_virtual(
    void*        driver_handle,
    uint32_t     pid,
    uint64_t     va,
    const uint8_t* data,
    size_t       size) {
    if (data == nullptr || size == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (driver_handle == nullptr) return SAO_RT_IO_ERR_DRIVER_NOT_LOADED;
    if (!gate_permits()) return SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH;
    try {
        size_t remaining = size;
        size_t off       = 0;
        uint64_t cur     = va;
        while (remaining > 0) {
            uint64_t pa = 0;
            const sao_status_t st = invoke_va_resolver(pid, cur, &pa);
            if (st != SAO_STATUS_OK) return st;
            const uint64_t page_off = cur & kPageMask;
            const size_t chunk_hi   = kPageSize - static_cast<size_t>(page_off);
            const size_t chunk      = (remaining < chunk_hi) ? remaining : chunk_hi;
            const sao_status_t rs = sao_rt_io_r5p_write_phys(
                driver_handle, pa, data + off, chunk);
            if (rs != SAO_STATUS_OK) return rs;
            off       += chunk;
            remaining -= chunk;
            cur       += chunk;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

// ── Open helper ──────────────────────────────────────────────────

extern "C" sao_status_t SAO_RT_IO_CALL sao_rt_io_r5_open_probe(
    void** out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;

    // Compose the NT path via the shared helper — same call the
    // Python `_r5p_load` uses.
    char dev_name[64] = {0};
    size_t written = 0;
    sao_status_t st = sao_rt_io_r5p_dev_path(dev_name, sizeof(dev_name), &written);
    if (st != SAO_STATUS_OK) return st;

    char nt_path[128] = {0};
    st = sao_rt_io_dev_nt_path(dev_name, nt_path, sizeof(nt_path), &written);
    if (st != SAO_STATUS_OK) return st;

#if defined(_WIN32)
    // Convert the NT path to the DOS form `\\.\<name>` for CreateFileW.
    // Python uses NtCreateFile with the raw "\\??\\<name>" path — the
    // `\\.\<name>` DOS equivalent goes through the same object
    // manager symlink so we use it here for CreateFileW compatibility.
    std::wstring w = L"\\\\.\\";
    for (const char* p = dev_name; *p != '\0'; ++p) {
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }
    HANDLE h = reinterpret_cast<HANDLE>(sao_rt_io_native_route_create_file_w(
        w.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL));
    if (h == INVALID_HANDLE_VALUE) {
        return SAO_RT_IO_ERR_DRIVER_NOT_LOADED;
    }
    *out_handle = static_cast<void*>(h);
    return SAO_STATUS_OK;
#else
    return SAO_RT_IO_ERR_DRIVER_NOT_LOADED;
#endif
}

extern "C" void SAO_RT_IO_CALL sao_rt_io_r5_close(void* handle) {
#if defined(_WIN32)
    if (handle == nullptr) return;
    (void)sao_rt_io_native_route_close(handle);
#else
    (void)handle;
#endif
}

extern "C" int32_t SAO_RT_IO_CALL sao_rt_io_r5_hid_ring_query(void) {
    if (!gate_permits()) return -1;

    SaoRtIoR5HidRingQueryPacket request{};
    request.abi_version = kSaoRtIoR5HidRingAbiVersion;
    request.struct_size = sizeof(request);
    request.outcome = kSaoRtIoR5HidRingOutcomeSentinel;
    request.consumed = kSaoRtIoR5HidRingOutcomeSentinel;
    SaoRtIoR5CommandPacket packet{
        &request,
        sizeof(request),
        kSaoRtIoR5CommandHidRingQuery,
        0u,
    };
        const int32_t status = dispatch_r5_syscall(&packet);
        return status >= 0 &&
            request.outcome == SAO_RT_IO_R5_HID_RING_NOT_SUBMITTED &&
           request.consumed == 0u ? 1 : -1;
}

extern "C" int32_t SAO_RT_IO_CALL
sao_rt_io_r5_hid_ring_direct_dispatch(
    uint64_t class_devobj_va,
    uint64_t class_service_callback_va,
    const void* packet_bytes,
    uint32_t packet_size) {
    if (!gate_permits() || class_devobj_va == 0u ||
        class_service_callback_va == 0u || packet_bytes == nullptr ||
        (packet_size != kSaoRtIoR5HidRingKeyboardPacketSize &&
         packet_size != kSaoRtIoR5HidRingMousePacketSize)) {
        return -1;
    }

    SaoRtIoR5HidRingDispatchPacket request{};
    request.abi_version = kSaoRtIoR5HidRingAbiVersion;
    request.struct_size = sizeof(request);
    request.class_devobj_va = class_devobj_va;
    request.class_service_callback_va = class_service_callback_va;
    request.user_packet_va = reinterpret_cast<uint64_t>(packet_bytes);
    request.packet_size = packet_size;
    request.outcome = kSaoRtIoR5HidRingOutcomeSentinel;
    request.consumed = kSaoRtIoR5HidRingOutcomeSentinel;
    request.reserved = 0u;
    SaoRtIoR5CommandPacket syscall_packet{
        &request,
        sizeof(request),
        kSaoRtIoR5CommandHidRingDispatch,
        0u,
    };
    const int32_t status = dispatch_r5_syscall(&syscall_packet);

    if (request.outcome == kSaoRtIoR5HidRingOutcomeSentinel) return -1;
    if (request.outcome == SAO_RT_IO_R5_HID_RING_NOT_SUBMITTED) {
        return status >= 0 && request.consumed == 0u ? 0 : -1;
    }
    if (request.outcome == SAO_RT_IO_R5_HID_RING_COMMITTED) {
        return status >= 0 && request.consumed == 1u ? 1 : -1;
    }
    return -1;
}

// ── VA→PA resolver hook ───────────────────────────────────────────

extern "C" void SAO_RT_IO_CALL sao_rt_io_r5_set_va_resolver(
    sao_rt_io_va_to_pa_fn_t fn,
    void*                   user) {
    try {
        (void)r5_rebind_va_resolver(fn, user, nullptr, 0u, true);
    } catch (...) {
    }
}

extern "C" sao_status_t SAO_RT_IO_CALL
sao_rt_io_r5_install_va_resolver_owned_v2(
    sao_rt_io_va_to_pa_fn_t fn,
    void*                   user,
    const void*             owner_identity,
    uint64_t                owner_generation) {
    try {
        return r5_rebind_va_resolver(
            fn, user, owner_identity, owner_generation, false);
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

extern "C" sao_status_t SAO_RT_IO_CALL
sao_rt_io_r5_clear_va_resolver_if_owner_v2(
    const void* owner_identity,
    uint64_t    owner_generation,
    uint32_t    timeout_ms,
    int32_t*    out_cleared) {
    try {
        return r5_clear_va_resolver_if_owner(
            owner_identity, owner_generation, timeout_ms, out_cleared);
    } catch (...) {
        if (out_cleared != nullptr) *out_cleared = 0;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

extern "C" void SAO_RT_IO_CALL sao_rt_io_r5_va_resolver_snapshot(
    SaoRtIoR5VaResolverSnapshot* out_snapshot) {
    if (out_snapshot == nullptr) return;
    std::memset(out_snapshot, 0, sizeof(*out_snapshot));
    try {
        R5VaResolverSlot& slot = state().va_resolver;
        std::lock_guard<std::mutex> lock(slot.state_mutex);
        out_snapshot->installed = slot.installed ? 1u : 0u;
        out_snapshot->mode = slot.mode;
        out_snapshot->function = function_bits(slot.fn);
        out_snapshot->user = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(slot.user));
        out_snapshot->owner_identity = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(slot.owner_identity));
        out_snapshot->owner_generation = slot.owner_generation;
        out_snapshot->revision = slot.revision;
    } catch (...) {
    }
}

// ── Hook block installer ─────────────────────────────────────────

extern "C" void SAO_RT_IO_CALL sao_rt_io_r5_install_hooks(
    const struct SaoRtIoR5HookBlock* hooks) {
    R5State& s = state();
    std::lock_guard<std::mutex> lk(s.hook_mutex);
    if (hooks == nullptr) {
        s.ioctl_hook = nullptr;
        s.load_hook  = nullptr;
        s.syscall_hook = nullptr;
        s.hook_user  = nullptr;
        return;
    }
    s.ioctl_hook = hooks->ioctl_hook;
    s.load_hook  = hooks->load_hook;
    s.syscall_hook = hooks->syscall_hook;
    s.hook_user  = hooks->user;
}
