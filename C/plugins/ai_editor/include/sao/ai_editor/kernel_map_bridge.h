// SAO AI Editor - kernel-map driver-mapping bridge.
//
// Thin wrapper over the peer rt_io kernel-map proxy
// (sao/rt_io/kernel_map_wire/proxy.h).  This translation unit is the
// only place inside the AI editor plugin that references the raw proxy
// entry points; the tool + command wiring layers talk to `Bridge` so
// they never need to know about wire status codes or the on-wire
// SAO_MMD_FLAG_* bits.
//
// Strict-mode guard: every map request implicitly carries
// SAO_MMD_FLAG_NO_INVOKE_ENTRY (0x2 in the security-side header).  The parameters JSON exposed to the
// AI has no field to change that, and the bridge itself refuses to
// emit a request that could set the flag to zero (see map()).  The
// intent is to keep the assistant panel from ever offering an "invoke
// DriverEntry" toggle to the AI; the kernel_map_wire layer already
// forces the flag on-wire (peer agent's design) but we belt-and-brace
// it here so the operator UI cannot surface it.

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "sao/ai_editor/ai_editor_status.h"

namespace sao::ai_editor::kernel_map {

// Snapshot of adapter state pulled from KMOP_STATUS.  Mirrors the
// on-wire SaoRtIoKmStatusReply payload so callers do not have to
// include the wire header.
struct BridgeStatus {
    bool active = false;
    uint32_t map_count = 0;
};

// Result of a successful kernelMap.map operation.  target_base is a
// non-paged pool VA in the kernel; entry_status is the DriverEntry
// return value AS-IF it had been called (in strict mode it is set to
// zero by the mapper because the entry point was intentionally not
// invoked).  The field is kept in the reply so the operator can see
// what the mapper reported.
struct MapResult {
    uint64_t target_base = 0;
    int32_t entry_status = 0;
};

// Bridge singleton.  Owns the "last activation config" snapshot so a
// no-op idempotent re-activate does not thrash the proxy; the mutex
// serialises every wire call so a concurrent tool invocation and
// command palette entry cannot race the proxy layer.
class Bridge final {
public:
    Bridge() noexcept = default;
    ~Bridge() = default;

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // Idempotent - re-activating with the same params is a no-op
    // success.  Returns the AI editor status code.  When
    // invoke_result_slot_va is 0 the proxy layer reports
    // NOT_INITIALIZED, which is exactly what we want in a stock
    // build (the operator has to explicitly wire a slot VA).
    int32_t activate(uint64_t invoke_result_slot_va,
                     uint32_t idle_timeout_ms,
                     uint64_t pool_tag_seed);

    // Deactivate the adapter and drop the cached activation snapshot
    // so the next activate() call goes back over the wire.
    int32_t deactivate();

    // Fetch the current KMOP_STATUS reply and translate it into
    // BridgeStatus.
    int32_t status(BridgeStatus& out);

    // Map a driver PE from a file-image byte buffer into non-paged
    // pool.  driver_len is hard-capped at 32 MiB (matches the wire
    // layer's cap).  Always strict-mode (NO_INVOKE_ENTRY on-wire);
    // there is intentionally no `flags` parameter.
    int32_t map(const uint8_t* driver_bytes,
                uint32_t driver_len,
                MapResult& out);

    // Free a mapped driver image.  target_base must be a value
    // previously returned by map().
    int32_t unmap(uint64_t target_base);

    // Enumerate the kernel virtual addresses of every currently
    // mapped image.  out_bases is cleared before use.  The proxy is
    // called twice: once to size the reply buffer, once to fill it.
    int32_t enumerate(std::vector<uint64_t>& out_bases);

private:
    struct ActivationSnapshot {
        uint64_t invoke_result_slot_va = 0;
        uint32_t idle_timeout_ms = 0;
        uint64_t pool_tag_seed = 0;
    };

    mutable std::mutex mutex_;
    std::optional<ActivationSnapshot> active_snapshot_;
};

// Access the process-wide Bridge instance.  All AI editor
// registrations (tool registry + command palette) share this instance
// so a single wire channel handles every operator request.
Bridge& shared_bridge();

}  // namespace sao::ai_editor::kernel_map
