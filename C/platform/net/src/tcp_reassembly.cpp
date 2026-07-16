// SAO Auto — game-agnostic TCP reassembler (Wave 6).
//
// See `include/sao/net/tcp_reassembly.h` for the API contract.
//
// This translation unit intentionally has zero knowledge of any
// application-layer protocol.  It emits raw ordered byte chunks and
// does no framing.

#include "sao/net/tcp_reassembly.h"
#include "tcp_reassembly_internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

namespace {

// ── Fixed sizes ────────────────────────────────────────────────────────
constexpr uint32_t kDefaultMaxStreams = 1024;
constexpr uint32_t kDefaultMaxBytesPerStream = 4u * 1024u * 1024u;
constexpr uint32_t kDefaultTimeoutMs = 30000;

// ── 5-tuple key ────────────────────────────────────────────────────────
struct FiveTuple {
    std::array<uint8_t, 16> src_ip;  // IPv4 = last 4 bytes, first 12 zero
    std::array<uint8_t, 16> dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t ip_version;  // 4 or 6

    bool operator==(const FiveTuple& o) const noexcept {
        return src_port == o.src_port && dst_port == o.dst_port &&
               ip_version == o.ip_version && src_ip == o.src_ip &&
               dst_ip == o.dst_ip;
    }
};

struct FiveTupleHash {
    size_t operator()(const FiveTuple& t) const noexcept {
        // FNV-1a 64 over the raw bytes of ports + ips.
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint8_t b) {
            h ^= b;
            h *= 1099511628211ull;
        };
        for (uint8_t b : t.src_ip) mix(b);
        for (uint8_t b : t.dst_ip) mix(b);
        mix(t.src_port & 0xff);
        mix((t.src_port >> 8) & 0xff);
        mix(t.dst_port & 0xff);
        mix((t.dst_port >> 8) & 0xff);
        mix(t.ip_version);
        return static_cast<size_t>(h);
    }
};

// Parsed pcap frame payload we hand to the flow bookkeeping.
struct ParsedTcp {
    FiveTuple key;
    uint32_t seq;
    const uint8_t* payload;
    uint32_t payload_len;
};

// ── Frame parser ───────────────────────────────────────────────────────
//
// Handles Ethernet II (dst[6] src[6] type[2]).  Type 0x0800 = IPv4,
// 0x86dd = IPv6.  Also handles frames that look like raw IP (first
// byte's high nibble == 4 or 6) for pcap DLTs other than EN10MB.

static bool read_ip_over_ethernet(const uint8_t* pkt, size_t size,
                                  const uint8_t** ip_out, size_t* ip_size_out,
                                  uint8_t* ip_ver_out) {
    if (size < 14) return false;
    uint16_t etype = static_cast<uint16_t>((pkt[12] << 8) | pkt[13]);
    if (etype == 0x0800) {
        *ip_ver_out = 4;
        *ip_out = pkt + 14;
        *ip_size_out = size - 14;
        return true;
    }
    if (etype == 0x86dd) {
        *ip_ver_out = 6;
        *ip_out = pkt + 14;
        *ip_size_out = size - 14;
        return true;
    }
    return false;
}

static bool parse_pcap_frame(const uint8_t* pkt, size_t size, ParsedTcp& out) {
    if (pkt == nullptr || size < 20) return false;

    const uint8_t* ip = nullptr;
    size_t ip_size = 0;
    uint8_t ip_ver = 0;

    // First try Ethernet II.
    if (!read_ip_over_ethernet(pkt, size, &ip, &ip_size, &ip_ver)) {
        // Fall back: raw IP (no link-layer header).
        uint8_t nibble = static_cast<uint8_t>(pkt[0] >> 4);
        if (nibble == 4 || nibble == 6) {
            ip = pkt;
            ip_size = size;
            ip_ver = nibble;
        } else {
            return false;
        }
    }

    if (ip_ver == 4) {
        if (ip_size < 20) return false;
        uint8_t ihl = static_cast<uint8_t>(ip[0] & 0x0f);
        size_t ip_header_len = static_cast<size_t>(ihl) * 4u;
        if (ip_header_len < 20 || ip_size < ip_header_len) return false;
        uint8_t proto = ip[9];
        if (proto != 6) return false;  // not TCP

        // Skip IP fragments (offset != 0 or MF set) — reassembler is
        // TCP-level; layered IP reassembly is out of scope for Wave 6.
        uint16_t frag = static_cast<uint16_t>((ip[6] << 8) | ip[7]);
        uint16_t frag_off = static_cast<uint16_t>(frag & 0x1fff);
        bool more_frag = (frag & 0x2000) != 0;
        if (frag_off != 0 || more_frag) return false;

        out.key.ip_version = 4;
        out.key.src_ip.fill(0);
        out.key.dst_ip.fill(0);
        std::memcpy(&out.key.src_ip[12], ip + 12, 4);
        std::memcpy(&out.key.dst_ip[12], ip + 16, 4);

        const uint8_t* tcp = ip + ip_header_len;
        size_t tcp_size = ip_size - ip_header_len;
        if (tcp_size < 20) return false;

        out.key.src_port = static_cast<uint16_t>((tcp[0] << 8) | tcp[1]);
        out.key.dst_port = static_cast<uint16_t>((tcp[2] << 8) | tcp[3]);
        uint32_t seq = static_cast<uint32_t>(tcp[4]) << 24 |
                       static_cast<uint32_t>(tcp[5]) << 16 |
                       static_cast<uint32_t>(tcp[6]) << 8 |
                       static_cast<uint32_t>(tcp[7]);
        uint8_t data_off = static_cast<uint8_t>((tcp[12] >> 4) & 0x0f);
        size_t tcp_header_len = static_cast<size_t>(data_off) * 4u;
        if (tcp_header_len < 20 || tcp_size < tcp_header_len) return false;

        out.seq = seq;
        out.payload = tcp + tcp_header_len;
        out.payload_len = static_cast<uint32_t>(tcp_size - tcp_header_len);
        return true;
    }

    if (ip_ver == 6) {
        if (ip_size < 40) return false;
        uint8_t next_hdr = ip[6];
        // Only handle the base case: IPv6 -> TCP with no extension chain.
        if (next_hdr != 6) return false;

        out.key.ip_version = 6;
        std::memcpy(out.key.src_ip.data(), ip + 8, 16);
        std::memcpy(out.key.dst_ip.data(), ip + 24, 16);

        const uint8_t* tcp = ip + 40;
        size_t tcp_size = ip_size - 40;
        if (tcp_size < 20) return false;

        out.key.src_port = static_cast<uint16_t>((tcp[0] << 8) | tcp[1]);
        out.key.dst_port = static_cast<uint16_t>((tcp[2] << 8) | tcp[3]);
        uint32_t seq = static_cast<uint32_t>(tcp[4]) << 24 |
                       static_cast<uint32_t>(tcp[5]) << 16 |
                       static_cast<uint32_t>(tcp[6]) << 8 |
                       static_cast<uint32_t>(tcp[7]);
        uint8_t data_off = static_cast<uint8_t>((tcp[12] >> 4) & 0x0f);
        size_t tcp_header_len = static_cast<size_t>(data_off) * 4u;
        if (tcp_header_len < 20 || tcp_size < tcp_header_len) return false;

        out.seq = seq;
        out.payload = tcp + tcp_header_len;
        out.payload_len = static_cast<uint32_t>(tcp_size - tcp_header_len);
        return true;
    }

    return false;
}

// ── Per-flow state ─────────────────────────────────────────────────────
struct TcpFlow {
    sao_net_stream_id_t stream_id = 0;
    // ISN + next_seq are 32-bit; comparisons use wrapping arithmetic.
    uint32_t next_seq = 0;
    bool initialized = false;
    uint64_t last_seen_ms = 0;
    uint64_t buffered_bytes = 0;  // in-order bytes still sitting in `pending`
    std::vector<uint8_t> pending;  // in-order but not yet popped
    std::map<uint32_t, std::vector<uint8_t>> out_of_order;
};

// Compare seq numbers with wrap-around: returns true if a is "less than" b
// on the 32-bit sequence circle.
static bool seq_lt(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}

// Returns b - a on the 32-bit sequence circle (unsigned).
static uint32_t seq_delta(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>(b - a);
}

}  // namespace

namespace sao::net::internal {

bool tcp_packet_remote_ipv4(const uint8_t* packet, size_t size,
                            uint8_t remote_ipv4_out[4],
                            uint16_t* remote_port_out) noexcept {
    if (remote_ipv4_out == nullptr || remote_port_out == nullptr) return false;
    std::memset(remote_ipv4_out, 0, 4);
    *remote_port_out = 0;
    ParsedTcp parsed{};
    if (!parse_pcap_frame(packet, size, parsed) || parsed.key.ip_version != 4) {
        return false;
    }
    std::memcpy(remote_ipv4_out, parsed.key.src_ip.data() + 12, 4);
    *remote_port_out = parsed.key.src_port;
    return true;
}

}  // namespace sao::net::internal

// ── Handle body ────────────────────────────────────────────────────────
struct sao_net_reassembler_s {
    SaoReassemblerConfig cfg{};
    SaoReassemblerStats stats{};
    std::unordered_map<FiveTuple, TcpFlow, FiveTupleHash> flows;
    sao_net_stream_id_t next_stream_id = 1;
    uint64_t clock_ms = 0;

    // FIFO of ready-to-pop chunks.
    struct Ready {
        sao_net_stream_id_t stream_id;
        std::vector<uint8_t> bytes;
    };
    std::deque<Ready> ready_queue;

    void note_deliver(std::vector<uint8_t>&& bytes,
                      sao_net_stream_id_t stream_id) {
        stats.bytes_delivered += bytes.size();
        ready_queue.push_back(Ready{stream_id, std::move(bytes)});
    }
};

// ───────────────────────────────────────────────────────────────────────
// Wave 6 API
// ───────────────────────────────────────────────────────────────────────

extern "C" sao_status_t SAO_NET_CALL sao_net_reassembler_create(
    const SaoReassemblerConfig* config,
    sao_net_reassembler_handle_t* reasm_out) {
    if (reasm_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *reasm_out = nullptr;

    auto* r = new sao_net_reassembler_s();
    if (config != nullptr) {
        r->cfg = *config;
    }
    if (r->cfg.max_streams == 0) r->cfg.max_streams = kDefaultMaxStreams;
    if (r->cfg.max_bytes_per_stream == 0) {
        r->cfg.max_bytes_per_stream = kDefaultMaxBytesPerStream;
    }
    if (r->cfg.timeout_ms == 0) r->cfg.timeout_ms = kDefaultTimeoutMs;
    *reasm_out = r;
    return SAO_STATUS_OK;
}

// Slice off the front N bytes of a pending buffer as a ready chunk.
static void deliver_front(sao_net_reassembler_s* r, TcpFlow& flow,
                          size_t n) {
    if (n == 0) return;
    if (n >= flow.pending.size()) {
        std::vector<uint8_t> chunk;
        chunk.swap(flow.pending);
        flow.buffered_bytes = 0;
        r->note_deliver(std::move(chunk), flow.stream_id);
    } else {
        std::vector<uint8_t> chunk(flow.pending.begin(),
                                   flow.pending.begin() +
                                       static_cast<ptrdiff_t>(n));
        flow.pending.erase(flow.pending.begin(),
                           flow.pending.begin() +
                               static_cast<ptrdiff_t>(n));
        flow.buffered_bytes = flow.pending.size();
        r->note_deliver(std::move(chunk), flow.stream_id);
    }
}

// After a flow's in-order buffer grew, deliver it as one chunk if
// non-empty, then check the byte cap.
static void publish_pending(sao_net_reassembler_s* r, TcpFlow& flow) {
    if (!flow.pending.empty()) {
        deliver_front(r, flow, flow.pending.size());
    }
    // The byte cap normally cannot be exceeded because we deliver after
    // each segment; keep this as a defensive guard for OOO drains.
    if (flow.buffered_bytes > r->cfg.max_bytes_per_stream) {
        deliver_front(r, flow, flow.buffered_bytes);
    }
}

// Try to consume OOO segments now that next_seq has advanced.
static void drain_ooo(sao_net_reassembler_s* r, TcpFlow& flow) {
    while (!flow.out_of_order.empty()) {
        auto it = flow.out_of_order.begin();
        uint32_t seq = it->first;
        if (seq == flow.next_seq) {
            auto data = std::move(it->second);
            flow.out_of_order.erase(it);
            flow.pending.insert(flow.pending.end(), data.begin(), data.end());
            flow.buffered_bytes = flow.pending.size();
            flow.next_seq = static_cast<uint32_t>(flow.next_seq + data.size());
            continue;
        }
        // If the front seq is behind next_seq entirely, it's a fully
        // covered retransmit — drop it silently.
        uint32_t delta = seq_delta(seq, flow.next_seq);
        if (delta > 0 && delta <= it->second.size()) {
            // partial overlap: trim and consume
            auto data = std::move(it->second);
            flow.out_of_order.erase(it);
            uint32_t skip = delta;
            if (skip < data.size()) {
                flow.pending.insert(flow.pending.end(),
                                    data.begin() +
                                        static_cast<ptrdiff_t>(skip),
                                    data.end());
                flow.buffered_bytes = flow.pending.size();
                flow.next_seq = static_cast<uint32_t>(flow.next_seq +
                                                    data.size() - skip);
            }
            continue;
        }
        if (seq_lt(seq, flow.next_seq)) {
            // fully behind → drop retransmit
            flow.out_of_order.erase(it);
            continue;
        }
        break;  // still ahead of next_seq
    }
    publish_pending(r, flow);
}

// Evict flows past the timeout.  Their remaining bytes are flushed.
static void gc_timeouts(sao_net_reassembler_s* r, uint64_t now_ms) {
    if (r->flows.empty()) return;
    uint64_t timeout = r->cfg.timeout_ms;
    for (auto it = r->flows.begin(); it != r->flows.end();) {
        auto& flow = it->second;
        if (now_ms >= flow.last_seen_ms &&
            (now_ms - flow.last_seen_ms) > timeout) {
            if (!flow.pending.empty()) {
                std::vector<uint8_t> chunk;
                chunk.swap(flow.pending);
                r->note_deliver(std::move(chunk), flow.stream_id);
            }
            r->stats.streams_flushed_timeout += 1;
            it = r->flows.erase(it);
            r->stats.streams_active =
                static_cast<uint64_t>(r->flows.size());
        } else {
            ++it;
        }
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_reassembler_ingest(
    sao_net_reassembler_handle_t reasm, const uint8_t* packet, size_t size,
    uint64_t ts_ms) {
    if (reasm == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (packet == nullptr || size == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    reasm->clock_ms = ts_ms;
    reasm->stats.packets_ingested += 1;
    gc_timeouts(reasm, ts_ms);

    ParsedTcp p{};
    if (!parse_pcap_frame(packet, size, p)) {
        reasm->stats.packets_dropped_non_tcp += 1;
        return SAO_STATUS_OK;
    }

    if (p.payload_len == 0) {
        // Pure ACK / control segment — recognised but nothing to deliver.
        // Still touch last_seen so control-only flows stay alive.
        auto it = reasm->flows.find(p.key);
        if (it != reasm->flows.end()) {
            it->second.last_seen_ms = ts_ms;
        }
        return SAO_STATUS_OK;
    }

    // Locate or create the flow.
    auto it = reasm->flows.find(p.key);
    if (it == reasm->flows.end()) {
        if (reasm->flows.size() >= reasm->cfg.max_streams) {
            reasm->stats.packets_dropped_no_capacity += 1;
            return SAO_STATUS_OK;
        }
        TcpFlow flow{};
        flow.stream_id = reasm->next_stream_id++;
        flow.next_seq = p.seq;
        flow.initialized = true;
        flow.last_seen_ms = ts_ms;
        auto ins = reasm->flows.emplace(p.key, std::move(flow));
        it = ins.first;
        reasm->stats.streams_created += 1;
        reasm->stats.streams_active =
            static_cast<uint64_t>(reasm->flows.size());
    } else {
        it->second.last_seen_ms = ts_ms;
    }

    TcpFlow& flow = it->second;

    // Detect wildly divergent seq (new ISN on same 5-tuple): treat as
    // stream reset — flush pending, reset bookkeeping.
    {
        uint32_t fwd = seq_delta(flow.next_seq, p.seq);
        uint32_t bwd = seq_delta(p.seq, flow.next_seq);
        // >1MB in both directions ≡ new ISN, cannot be legit reorder.
        if (fwd > 1000000u && bwd > 1000000u) {
            if (!flow.pending.empty()) {
                std::vector<uint8_t> chunk;
                chunk.swap(flow.pending);
                reasm->note_deliver(std::move(chunk), flow.stream_id);
            }
            flow.out_of_order.clear();
            flow.buffered_bytes = 0;
            flow.next_seq = p.seq;
        }
    }

    if (p.seq == flow.next_seq) {
        // In-order — append and advance.
        flow.pending.insert(flow.pending.end(), p.payload,
                            p.payload + p.payload_len);
        flow.buffered_bytes = flow.pending.size();
        flow.next_seq = static_cast<uint32_t>(flow.next_seq + p.payload_len);
        drain_ooo(reasm, flow);
        return SAO_STATUS_OK;
    }

    // Retransmit fully covered by consumed bytes.
    if (seq_lt(p.seq, flow.next_seq)) {
        uint32_t behind = seq_delta(p.seq, flow.next_seq);
        if (behind >= p.payload_len) {
            reasm->stats.packets_dropped_retransmit += 1;
            return SAO_STATUS_OK;
        }
        // Partial retransmit: trim leading and treat as in-order tail.
        uint32_t skip = behind;
        uint32_t tail_len = p.payload_len - skip;
        flow.pending.insert(flow.pending.end(),
                            p.payload + skip,
                            p.payload + skip + tail_len);
        flow.buffered_bytes = flow.pending.size();
        flow.next_seq = static_cast<uint32_t>(flow.next_seq + tail_len);
        drain_ooo(reasm, flow);
        return SAO_STATUS_OK;
    }

    // Out-of-order (future) segment.  Dedup on identical seq.
    auto ooo_it = flow.out_of_order.find(p.seq);
    if (ooo_it != flow.out_of_order.end()) {
        if (ooo_it->second.size() >= p.payload_len) {
            reasm->stats.packets_dropped_retransmit += 1;
            return SAO_STATUS_OK;
        }
        // Larger version — replace.
        ooo_it->second.assign(p.payload, p.payload + p.payload_len);
        return SAO_STATUS_OK;
    }
    flow.out_of_order.emplace(
        p.seq, std::vector<uint8_t>(p.payload, p.payload + p.payload_len));

    // Cap runaway OOO storage by evicting the oldest entry that is
    // strictly ahead by more than max_bytes_per_stream.
    while (!flow.out_of_order.empty()) {
        size_t total = 0;
        for (auto& kv : flow.out_of_order) total += kv.second.size();
        if (total <= reasm->cfg.max_bytes_per_stream) break;
        flow.out_of_order.erase(flow.out_of_order.rbegin()->first);
        reasm->stats.packets_dropped_no_capacity += 1;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_reassembler_pop_stream(
    sao_net_reassembler_handle_t reasm, sao_net_stream_id_t* stream_id_out,
    uint8_t* buf_out, size_t capacity, size_t* size_out) {
    if (reasm == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (size_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *size_out = 0;
    if (stream_id_out != nullptr) *stream_id_out = 0;

    if (reasm->ready_queue.empty()) return SAO_STATUS_OK;
    const auto& front = reasm->ready_queue.front();
    if (buf_out == nullptr || capacity < front.bytes.size()) {
        *size_out = front.bytes.size();
        if (stream_id_out != nullptr) *stream_id_out = front.stream_id;
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(buf_out, front.bytes.data(), front.bytes.size());
    *size_out = front.bytes.size();
    if (stream_id_out != nullptr) *stream_id_out = front.stream_id;
    reasm->ready_queue.pop_front();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_reassembler_flush_stream(
    sao_net_reassembler_handle_t reasm, sao_net_stream_id_t stream_id) {
    if (reasm == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;

    // Locate the flow by stream_id (rare op — linear scan is fine).
    for (auto& kv : reasm->flows) {
        if (kv.second.stream_id != stream_id) continue;
        TcpFlow& flow = kv.second;
        // Fold OOO tail into pending in whatever order it sits — after
        // a flush we don't care about gaps.
        for (auto& ooo : flow.out_of_order) {
            flow.pending.insert(flow.pending.end(), ooo.second.begin(),
                                ooo.second.end());
        }
        flow.out_of_order.clear();
        flow.buffered_bytes = flow.pending.size();
        if (!flow.pending.empty()) {
            std::vector<uint8_t> chunk;
            chunk.swap(flow.pending);
            reasm->note_deliver(std::move(chunk), stream_id);
        }
        return SAO_STATUS_OK;
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_reassembler_get_stats(
    sao_net_reassembler_handle_t reasm, SaoReassemblerStats* stats_out) {
    if (reasm == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (stats_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    reasm->stats.streams_active =
        static_cast<uint64_t>(reasm->flows.size());
    *stats_out = reasm->stats;
    return SAO_STATUS_OK;
}

extern "C" void SAO_NET_CALL sao_net_reassembler_destroy(
    sao_net_reassembler_handle_t reasm) {
    delete reasm;
}
