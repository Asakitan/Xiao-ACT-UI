// SAO Auto — game-agnostic TCP reassembler (Wave 6).
//
// The reassembler ingests raw pcap frames (Ethernet + IPv4/IPv6 + TCP)
// and yields byte streams in sequence order per TCP flow.  It knows
// nothing about the application protocol on top; the output is a byte
// buffer plus a 5-tuple stream_id and callers layer their own framer
// on top (see `packet_pipeline.h` for the application-layer surface).
//
// Design summary:
//   * A flow is keyed by (protocol=6, src_ip, src_port, dst_ip, dst_port).
//     IPv6 is normalized to a 16-byte address; IPv4 is left-padded with
//     ::ffff:0.0.0.0/96 to share the key layout.
//   * Each flow owns an ISN, a next-expected seq number, and an
//     `std::map<seq, segment>` of out-of-order material.  In-order
//     segments append directly to a per-flow byte deque; OOO segments
//     wait until the gap closes or the timeout expires.
//   * Retransmits are dropped (dedup by seq range vs already-consumed
//     bytes).  A stale flow (no ingest within `timeout_ms`) is evicted
//     and its buffered bytes flushed to the pop queue.
//   * `sao_net_reassembler_pop_stream` yields one (stream_id, bytes)
//     tuple per call; callers drain until it returns OK with a zero
//     size (queue empty).

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/net/npcap_capture.h"  // pulls SAO_NET_API + calling convention

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_net_reassembler_s* sao_net_reassembler_handle_t;

// A stable stream_id derived from the 5-tuple.  Callers compare by
// value; the id is stable across a flow's lifetime.  Not portable
// across reassemblers (each has its own id space).
typedef uint64_t sao_net_stream_id_t;

// Configuration knobs.  A zero field falls back to a sane default so
// callers can `SaoReassemblerConfig cfg{};` and go.
struct SaoReassemblerConfig {
    // Maximum concurrent flows tracked.  Extra flows past this cap are
    // dropped (never crash).  Default: 1024.
    uint32_t max_streams;

    // Per-flow byte cap.  Segments pushing the buffered/ordered byte
    // count past this cap cause the OLDEST buffered bytes to be
    // popped as a chunk.  Default: 4 MiB.
    uint32_t max_bytes_per_stream;

    // Flow eviction timeout in milliseconds.  When a flow has not seen
    // ingest for this many ms, it is flushed on the next call.
    // Default: 30_000 ms.
    uint32_t timeout_ms;

    uint32_t _reserved;
};

struct SaoReassemblerStats {
    uint64_t packets_ingested;
    uint64_t packets_dropped_non_tcp;   // not IPv4/IPv6+TCP or malformed
    uint64_t packets_dropped_retransmit;
    uint64_t packets_dropped_no_capacity;  // hit max_streams
    uint64_t bytes_delivered;
    uint64_t streams_active;
    uint64_t streams_created;
    uint64_t streams_flushed_timeout;
};

// Wave 6 API — 5 calls.  All calls are thread-hostile (single-threaded
// use only).  Wrap externally if concurrent ingest is required.

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_reassembler_create(
    const SaoReassemblerConfig* config,
    sao_net_reassembler_handle_t* reasm_out);

// Ingest a single pcap frame.  `packet` must include the link-layer
// header (Ethernet by default; raw IP is auto-detected via version
// nibble).  `size` is the wire size; `ts_ms` is a monotonic timestamp
// in milliseconds (any epoch — used only for timeout math).  Returns
// SAO_STATUS_OK on any non-fatal outcome (drops are silent, counted in
// stats).
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_reassembler_ingest(
    sao_net_reassembler_handle_t reasm,
    const uint8_t* packet,
    size_t size,
    uint64_t ts_ms);

// Pop one completed byte chunk from a flow.  Sets `*stream_id_out`
// and `*size_out`; a `*size_out == 0` return with OK means the queue
// is empty.  If `capacity` is smaller than the pending chunk, sets
// `*size_out` to the required capacity and returns
// SAO_STATUS_ERR_BUFFER_TOO_SMALL without consuming the chunk.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_reassembler_pop_stream(
    sao_net_reassembler_handle_t reasm,
    sao_net_stream_id_t* stream_id_out,
    uint8_t* buf_out,
    size_t capacity,
    size_t* size_out);

// Force a flush of a stream: any buffered in-order bytes are queued
// for pop, any OOO gap is skipped past.  Idempotent; unknown stream
// ids return SAO_STATUS_ERR_NOT_FOUND.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_reassembler_flush_stream(
    sao_net_reassembler_handle_t reasm,
    sao_net_stream_id_t stream_id);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_reassembler_get_stats(
    sao_net_reassembler_handle_t reasm,
    SaoReassemblerStats* stats_out);

// Destroy the reassembler.  Safe on NULL.
SAO_NET_API void SAO_NET_CALL sao_net_reassembler_destroy(
    sao_net_reassembler_handle_t reasm);

#ifdef __cplusplus
}  // extern "C"
#endif
