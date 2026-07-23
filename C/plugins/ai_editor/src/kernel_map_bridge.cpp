// SAO AI Editor - kernel-map bridge implementation.

#include "sao/ai_editor/kernel_map_bridge.h"

#include <mutex>
#include <utility>

#include "sao/core/status.h"
#include "sao/rt_io/kernel_map_wire/proxy.h"
#include "sao/rt_io/kernel_map_wire/wire.h"
#include "sao/rt_io/status.h"

namespace sao::ai_editor::kernel_map {

namespace {

// The kernel_map_wire layer forces NO_INVOKE_ENTRY on-wire regardless
// of what the caller passes (see platform/rt_io/src/kernel_map_wire/
// proxy.cpp::sao_rt_io_kernel_map_proxy_map: it ORs the flag onto the
// caller's value); the bridge repeats the assertion so an operator-
// facing tool can never surface a way to flip it off.  Value mirrors
// SAO_MMD_FLAG_NO_INVOKE_ENTRY (0x2) from sao_security/driver_loader/
// manual_map_driver.h - we mirror it locally rather than including the
// security header since that is a peer-owned tree we do not depend on
// from the AI editor plugin.  If the security-side constant ever
// changes, the wire tests in this plugin will keep passing (the proxy
// still ORs its authoritative value on), but this bridge would then
// pass a stale value to the wire; a compile-time assert would be
// stronger but requires the security header.
constexpr uint32_t kFlagNoInvokeEntry = 0x00000002u;

// Translate an rt_io sao_status_t into the AI editor's status enum.
// Mirrors gpu_hunt_sdk_adapter.cpp::map_rt_io_status but returns the
// AI editor codes directly so consumers of the bridge do not need to
// pull sao_sdk_status_t into their translation units.
int32_t translate_rt_io_status(sao_status_t status) noexcept {
    switch (status) {
    case SAO_STATUS_OK:
        return SAO_AI_EDITOR_OK;
    case SAO_STATUS_ERR_INVALID_ARGUMENT:
    case SAO_RT_IO_ERR_TARGET_PID_INVALID:
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    case SAO_STATUS_ERR_NOT_INITIALIZED:
    case SAO_RT_IO_ERR_HELPER_NOT_LAUNCHED:
    case SAO_RT_IO_ERR_HELPER_HANDSHAKE_FAIL:
    case SAO_RT_IO_ERR_HELPER_EXITED:
    case SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH:
    case SAO_RT_IO_ERR_HELPER_PROTOCOL_MISMATCH:
    case SAO_RT_IO_ERR_HELPER_BOOTSTRAP_FAIL:
    case SAO_RT_IO_ERR_HELPER_NOT_FOUND:
    case SAO_RT_IO_ERR_HELPER_START_FAILED:
    case SAO_RT_IO_ERR_HELPER_READY_TIMEOUT:
    case SAO_RT_IO_ERR_HELPER_CRASHED:
    case SAO_RT_IO_ERR_DRIVER_NOT_LOADED:
    case SAO_RT_IO_ERR_SESSION_NOT_RUNNING:
    case SAO_RT_IO_ERR_SESSION_QUIESCING:
    case SAO_RT_IO_ERR_SESSION_CLOSED:
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    case SAO_STATUS_ERR_HANDLE_INVALID:
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    case SAO_STATUS_ERR_BUFFER_TOO_SMALL:
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    case SAO_STATUS_ERR_NOT_IMPLEMENTED:
    case SAO_STATUS_ERR_CAPABILITY_MISSING:
        return SAO_AI_EDITOR_ERR_NOT_IMPLEMENTED;
    case SAO_STATUS_ERR_TIMEOUT:
        return SAO_AI_EDITOR_ERR_TIMEOUT;
    case SAO_STATUS_ERR_ACCESS_DENIED:
    case SAO_RT_IO_ERR_INSUFFICIENT_PRIVILEGE:
        return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
    case SAO_STATUS_ERR_NOT_FOUND:
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    case SAO_STATUS_ERR_CANCELLED:
        return SAO_AI_EDITOR_ERR_CANCELLED;
    default:
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

}  // namespace

int32_t Bridge::activate(uint64_t invoke_result_slot_va,
                          uint32_t idle_timeout_ms,
                          uint64_t pool_tag_seed) {
    std::lock_guard<std::mutex> guard(mutex_);
    // Idempotent short-circuit: same params + already-recorded
    // activation => no-op OK.  A parameter mismatch falls through to
    // a full re-activate; the wire layer takes care of rejecting a
    // re-activate on a still-live adapter with a different config.
    if (active_snapshot_.has_value() &&
        active_snapshot_->invoke_result_slot_va == invoke_result_slot_va &&
        active_snapshot_->idle_timeout_ms == idle_timeout_ms &&
        active_snapshot_->pool_tag_seed == pool_tag_seed) {
        return SAO_AI_EDITOR_OK;
    }
    const sao_status_t status = sao_rt_io_kernel_map_proxy_activate(
        invoke_result_slot_va, idle_timeout_ms, pool_tag_seed);
    const int32_t translated = translate_rt_io_status(status);
    if (translated == SAO_AI_EDITOR_OK) {
        ActivationSnapshot snap;
        snap.invoke_result_slot_va = invoke_result_slot_va;
        snap.idle_timeout_ms = idle_timeout_ms;
        snap.pool_tag_seed = pool_tag_seed;
        active_snapshot_ = snap;
    }
    return translated;
}

int32_t Bridge::deactivate() {
    std::lock_guard<std::mutex> guard(mutex_);
    const sao_status_t status = sao_rt_io_kernel_map_proxy_deactivate();
    const int32_t translated = translate_rt_io_status(status);
    // Clear the activation snapshot on OK *and* on
    // NOT_INITIALIZED - the latter means the wire layer has already
    // torn down the adapter, so treating it as "still active" would
    // leak a stale snapshot across the next activate() call.
    if (translated == SAO_AI_EDITOR_OK ||
        translated == SAO_AI_EDITOR_ERR_NOT_INITIALIZED) {
        active_snapshot_.reset();
    }
    return translated;
}

int32_t Bridge::status(BridgeStatus& out) {
    out = BridgeStatus{};
    std::lock_guard<std::mutex> guard(mutex_);
    SaoRtIoKmStatusReply reply{};
    const sao_status_t status = sao_rt_io_kernel_map_proxy_status(&reply);
    const int32_t translated = translate_rt_io_status(status);
    if (translated != SAO_AI_EDITOR_OK) {
        return translated;
    }
    out.active = reply.active != 0;
    out.map_count = reply.map_count;
    return SAO_AI_EDITOR_OK;
}

int32_t Bridge::map(const uint8_t* driver_bytes,
                     uint32_t driver_len,
                     MapResult& out) {
    out = MapResult{};
    if (driver_bytes == nullptr || driver_len == 0) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (driver_len > SAO_RT_IO_KMOP_MAX_DRIVER_BYTES) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    // Strict-mode: pass NO_INVOKE_ENTRY on-wire.  The wire layer
    // already forces the bit on, so the value we pass is really a
    // diagnostic aid; test cases assert that this exact value shows
    // up in the shim payload.
    uint64_t target_base = 0;
    int32_t entry_status = 0;
    const sao_status_t status = sao_rt_io_kernel_map_proxy_map(
        kFlagNoInvokeEntry, driver_bytes, driver_len,
        &target_base, &entry_status);
    const int32_t translated = translate_rt_io_status(status);
    if (translated != SAO_AI_EDITOR_OK) {
        return translated;
    }
    out.target_base = target_base;
    out.entry_status = entry_status;
    return SAO_AI_EDITOR_OK;
}

int32_t Bridge::unmap(uint64_t target_base) {
    if (target_base == 0) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    return translate_rt_io_status(
        sao_rt_io_kernel_map_proxy_unmap(target_base));
}

int32_t Bridge::enumerate(std::vector<uint64_t>& out_bases) {
    out_bases.clear();
    std::lock_guard<std::mutex> guard(mutex_);
    // First pass: ask for the count only.
    uint32_t required = 0;
    sao_status_t status = sao_rt_io_kernel_map_proxy_enumerate(
        nullptr, 0, &required);
    // On success with no images the proxy either reports required=0
    // via the count-only pass or replies BUFFER_TOO_SMALL depending on
    // the wire layer's implementation.  Treat both as "collect count,
    // then reissue with a real buffer".
    if (status != SAO_STATUS_OK &&
        status != SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
        return translate_rt_io_status(status);
    }
    if (required == 0) {
        return SAO_AI_EDITOR_OK;
    }
    std::vector<uint64_t> buffer(required);
    uint32_t received = 0;
    status = sao_rt_io_kernel_map_proxy_enumerate(
        buffer.data(), required, &received);
    const int32_t translated = translate_rt_io_status(status);
    if (translated != SAO_AI_EDITOR_OK) {
        return translated;
    }
    if (received > buffer.size()) {
        // Would indicate the wire layer changed its mind about the
        // count between calls; refuse to hand back a truncated slice.
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    buffer.resize(received);
    out_bases = std::move(buffer);
    return SAO_AI_EDITOR_OK;
}

Bridge& shared_bridge() {
    static Bridge instance;
    return instance;
}

}  // namespace sao::ai_editor::kernel_map
