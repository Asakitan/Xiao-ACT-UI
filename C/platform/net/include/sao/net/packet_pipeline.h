// SAO Auto — packet pipeline: capture -> endpoint lock -> reassembly -> dispatch.
//
// Mirrors the responsibilities Python splits across:
//   * `packet_capture.py`  -> npcap_capture.h (this file only glues onto it)
//   * `packet_bridge.py`   -> endpoint identification + inbound filtering
//   * `parser_adapter.py`  -> hands framed application payloads to a parser
//
// The pipeline is deliberately protocol-agnostic.  Concrete parsers live
// under `../plugins/star_resonance_plugin/` and register frame callbacks
// via `sao_net_pipeline_set_frame_callback`.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/net/npcap_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_net_pipeline_s* sao_net_pipeline_handle_t;

struct SaoTcpEndpoint {
    uint8_t  ipv4[4];
    uint16_t port;
    uint16_t _pad;
};

// Application-frame delivery — bytes are already stripped of Ethernet /
// IP / TCP headers and reassembled to a single ordered stream chunk.  A
// single downlink frame may straddle multiple TCP segments; the pipeline
// coalesces before firing the callback.
typedef void (SAO_NET_CALL* sao_net_frame_callback_t)(
    const uint8_t* frame_bytes,
    size_t frame_length,
    uint64_t ts_unix_ns,
    void* user_data);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_pipeline_create(
    sao_net_capture_handle_t capture,
    sao_net_pipeline_handle_t* out_handle);

SAO_NET_API void SAO_NET_CALL sao_net_pipeline_destroy(
    sao_net_pipeline_handle_t handle);

// Completion-aware destroy. BUSY from the pipeline's own frame callback
// means owner release is recorded and finalization is deferred until that
// callback exits; other BUSY returns require a serialized retry. Final owner
// destruction must not race a brand-new raw-handle API entry.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_pipeline_try_destroy(
    sao_net_pipeline_handle_t handle);

// Optional endpoint hint — if set, the pipeline only accepts inbound
// segments from this remote.  Zeroed endpoint means "auto-discover from
// heuristics" (see Python `packet_bridge._lock_endpoint`).
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_pipeline_lock_endpoint(
    sao_net_pipeline_handle_t handle,
    const SaoTcpEndpoint* remote);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_pipeline_set_frame_callback(
    sao_net_pipeline_handle_t handle,
    sao_net_frame_callback_t callback,
    void* user_data);

// Diagnostics for the UI status bar.
struct SaoPipelineStats {
    uint64_t frames_delivered;
    uint64_t bytes_delivered;
    uint64_t reorder_events;    // OOO TCP segments patched in
    uint64_t drop_events;       // segments dropped for policy reasons
    SaoTcpEndpoint locked_remote;
    bool     endpoint_locked;
    uint8_t  _pad[3];
};

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_pipeline_snapshot_stats(
    sao_net_pipeline_handle_t handle, SaoPipelineStats* out_stats);

#ifdef __cplusplus
}  // extern "C"
#endif
