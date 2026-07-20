// SAO Auto — Wave 6 tests for the game-agnostic TCP reassembler.
//
// These tests build synthetic pcap frames (Ethernet + IPv4 + TCP)
// entirely in-process; nothing depends on Npcap or a live NIC.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sao/net/tcp_reassembly.h"

namespace {

// ── Synthetic packet builder ──────────────────────────────────────────
//
// Layout:  [14B Ethernet][20B IPv4][20B TCP][payload...]
// Endianness: network (big-endian) for all multi-byte header fields.

struct FlowKey {
    std::array<uint8_t, 4> src_ip;
    std::array<uint8_t, 4> dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
};

std::vector<uint8_t> build_tcp_ipv4(const FlowKey& key, uint32_t seq,
                                    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> pkt(14 + 20 + 20 + payload.size(), 0);
    // ── Ethernet II ──
    // Dest MAC 00..05, Src MAC 06..0b — leave as zeros.
    pkt[12] = 0x08;
    pkt[13] = 0x00;  // ethertype IPv4

    // ── IPv4 ──
    uint8_t* ip = pkt.data() + 14;
    ip[0] = 0x45;   // version=4, ihl=5
    ip[1] = 0x00;   // DSCP/ECN
    uint16_t total_len = static_cast<uint16_t>(20 + 20 + payload.size());
    ip[2] = static_cast<uint8_t>(total_len >> 8);
    ip[3] = static_cast<uint8_t>(total_len & 0xff);
    ip[4] = 0;      // identification hi
    ip[5] = 0;      // identification lo
    ip[6] = 0x40;   // flags: DF, no more frag, offset=0
    ip[7] = 0x00;
    ip[8] = 64;     // TTL
    ip[9] = 6;      // proto TCP
    ip[10] = 0;     // checksum hi (unused by reassembler)
    ip[11] = 0;     // checksum lo
    std::memcpy(ip + 12, key.src_ip.data(), 4);
    std::memcpy(ip + 16, key.dst_ip.data(), 4);

    // ── TCP ──
    uint8_t* tcp = ip + 20;
    tcp[0] = static_cast<uint8_t>(key.src_port >> 8);
    tcp[1] = static_cast<uint8_t>(key.src_port & 0xff);
    tcp[2] = static_cast<uint8_t>(key.dst_port >> 8);
    tcp[3] = static_cast<uint8_t>(key.dst_port & 0xff);
    tcp[4] = static_cast<uint8_t>((seq >> 24) & 0xff);
    tcp[5] = static_cast<uint8_t>((seq >> 16) & 0xff);
    tcp[6] = static_cast<uint8_t>((seq >> 8) & 0xff);
    tcp[7] = static_cast<uint8_t>(seq & 0xff);
    // ACK = 0
    tcp[8] = 0;
    tcp[9] = 0;
    tcp[10] = 0;
    tcp[11] = 0;
    tcp[12] = 0x50;  // data offset = 5 (20 bytes), no reserved bits
    tcp[13] = 0x18;  // flags PSH+ACK
    tcp[14] = 0xff;  // window hi
    tcp[15] = 0xff;
    tcp[16] = 0;     // checksum
    tcp[17] = 0;
    tcp[18] = 0;     // urgent ptr
    tcp[19] = 0;

    if (!payload.empty()) {
        std::memcpy(tcp + 20, payload.data(), payload.size());
    }
    return pkt;
}

std::vector<uint8_t> make_payload(uint8_t start, size_t n) {
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(start + i);
    }
    return out;
}

struct DrainedChunk {
    sao_net_stream_id_t stream_id;
    std::vector<uint8_t> bytes;
};

std::vector<DrainedChunk> drain_all(sao_net_reassembler_handle_t r) {
    std::vector<DrainedChunk> chunks;
    while (true) {
        sao_net_stream_id_t sid = 0;
        size_t size = 0;
        // Sizing pop first.
        sao_status_t st = sao_net_reassembler_pop_stream(
            r, &sid, nullptr, 0, &size);
        if (st == SAO_STATUS_OK && size == 0) break;
        REQUIRE((st == SAO_STATUS_ERR_BUFFER_TOO_SMALL ||
                 st == SAO_STATUS_OK));
        std::vector<uint8_t> buf(size);
        st = sao_net_reassembler_pop_stream(r, &sid, buf.data(), buf.size(),
                                            &size);
        REQUIRE(st == SAO_STATUS_OK);
        buf.resize(size);
        chunks.push_back(DrainedChunk{sid, std::move(buf)});
    }
    return chunks;
}

const FlowKey kFlowA{
    {10, 0, 0, 1}, {192, 168, 1, 100}, 8888, 40000};
const FlowKey kFlowB{
    {10, 0, 0, 2}, {192, 168, 1, 100}, 9999, 40001};

}  // namespace

TEST_CASE("reasm_single_segment_pass_through", "[net][automation][reasm]") {
    SaoReassemblerConfig cfg{};
    sao_net_reassembler_handle_t r = nullptr;
    REQUIRE(sao_net_reassembler_create(&cfg, &r) == SAO_STATUS_OK);
    REQUIRE(r != nullptr);

    auto payload = make_payload(0xAA, 32);
    auto pkt = build_tcp_ipv4(kFlowA, 1000, payload);
    REQUIRE(sao_net_reassembler_ingest(r, pkt.data(), pkt.size(), 1) ==
            SAO_STATUS_OK);

    auto chunks = drain_all(r);
    REQUIRE(chunks.size() == 1);
    REQUIRE(chunks[0].bytes == payload);
    REQUIRE(chunks[0].stream_id != 0);

    SaoReassemblerStats stats{};
    REQUIRE(sao_net_reassembler_get_stats(r, &stats) == SAO_STATUS_OK);
    REQUIRE(stats.packets_ingested == 1);
    REQUIRE(stats.bytes_delivered == payload.size());
    REQUIRE(stats.streams_created == 1);
    REQUIRE(stats.streams_active == 1);

    sao_net_reassembler_destroy(r);
}

TEST_CASE("reasm_two_segments_in_order_reassembled",
          "[net][automation][reasm]") {
    SaoReassemblerConfig cfg{};
    sao_net_reassembler_handle_t r = nullptr;
    REQUIRE(sao_net_reassembler_create(&cfg, &r) == SAO_STATUS_OK);

    auto p1 = make_payload(0x01, 16);
    auto p2 = make_payload(0x40, 16);
    auto pkt1 = build_tcp_ipv4(kFlowA, 5000, p1);
    auto pkt2 = build_tcp_ipv4(kFlowA, 5000 + 16, p2);
    REQUIRE(sao_net_reassembler_ingest(r, pkt1.data(), pkt1.size(), 1) ==
            SAO_STATUS_OK);
    REQUIRE(sao_net_reassembler_ingest(r, pkt2.data(), pkt2.size(), 2) ==
            SAO_STATUS_OK);

    auto chunks = drain_all(r);
    // The reassembler may split into 1 or 2 chunks — the important
    // invariant is: total bytes are in exact order across a single
    // stream_id.
    REQUIRE(!chunks.empty());
    sao_net_stream_id_t sid = chunks.front().stream_id;
    std::vector<uint8_t> joined;
    for (auto& c : chunks) {
        REQUIRE(c.stream_id == sid);
        joined.insert(joined.end(), c.bytes.begin(), c.bytes.end());
    }
    std::vector<uint8_t> expected;
    expected.insert(expected.end(), p1.begin(), p1.end());
    expected.insert(expected.end(), p2.begin(), p2.end());
    REQUIRE(joined == expected);

    sao_net_reassembler_destroy(r);
}

TEST_CASE("reasm_out_of_order_segments_wait", "[net][automation][reasm]") {
    SaoReassemblerConfig cfg{};
    sao_net_reassembler_handle_t r = nullptr;
    REQUIRE(sao_net_reassembler_create(&cfg, &r) == SAO_STATUS_OK);

    // Two segments arrive out of order; nothing should pop until the
    // gap is closed.
    auto p1 = make_payload(0x10, 8);
    auto p2 = make_payload(0x20, 8);
    auto pkt1 = build_tcp_ipv4(kFlowA, 100, p1);
    auto pkt2 = build_tcp_ipv4(kFlowA, 108, p2);

    // Deliver pkt2 first.
    REQUIRE(sao_net_reassembler_ingest(r, pkt2.data(), pkt2.size(), 1) ==
            SAO_STATUS_OK);
    // Reassembler seeded next_seq from *this* segment as ISN.  So it
    // treats seq=108 as in-order and buffers it.  To simulate a real
    // out-of-order arrival we must seed the flow with the earlier ISN
    // ourselves.
    //
    // Rebuild: start with a marker packet at seq=100 payload len 0?
    // TCP payload=0 skips flow book-keeping.  So instead we drop pkt2
    // as a "fresh flow", then send pkt1 which the reassembler will see
    // as retransmit — which is the wrong test.
    //
    // Correct construction: send pkt1 first (seeds ISN to 100), then a
    // pkt3 that arrives out-of-order.
    sao_net_reassembler_destroy(r);
    REQUIRE(sao_net_reassembler_create(&cfg, &r) == SAO_STATUS_OK);

    // Segment A at seq=1000 (opens the flow, next_seq will move to 1008).
    auto pA = make_payload(0x01, 8);
    auto pktA = build_tcp_ipv4(kFlowA, 1000, pA);
    REQUIRE(sao_net_reassembler_ingest(r, pktA.data(), pktA.size(), 1) ==
            SAO_STATUS_OK);

    // Drain the in-order delivery of A.
    auto after_a = drain_all(r);
    REQUIRE(after_a.size() == 1);
    REQUIRE(after_a[0].bytes == pA);

    // Segment C arrives BEFORE B  (C = seq 1016, B = seq 1008)
    auto pB = make_payload(0x20, 8);
    auto pC = make_payload(0x40, 8);
    auto pktC = build_tcp_ipv4(kFlowA, 1016, pC);
    REQUIRE(sao_net_reassembler_ingest(r, pktC.data(), pktC.size(), 2) ==
            SAO_STATUS_OK);
    // Nothing should pop — C is out of order.
    auto after_c = drain_all(r);
    REQUIRE(after_c.empty());

    // Segment B arrives — the gap closes, both should now flow.
    auto pktB = build_tcp_ipv4(kFlowA, 1008, pB);
    REQUIRE(sao_net_reassembler_ingest(r, pktB.data(), pktB.size(), 3) ==
            SAO_STATUS_OK);
    auto after_b = drain_all(r);
    std::vector<uint8_t> joined;
    for (auto& c : after_b) {
        joined.insert(joined.end(), c.bytes.begin(), c.bytes.end());
    }
    std::vector<uint8_t> expected;
    expected.insert(expected.end(), pB.begin(), pB.end());
    expected.insert(expected.end(), pC.begin(), pC.end());
    REQUIRE(joined == expected);

    sao_net_reassembler_destroy(r);
}

TEST_CASE("reasm_retransmit_dedup", "[net][automation][reasm]") {
    SaoReassemblerConfig cfg{};
    sao_net_reassembler_handle_t r = nullptr;
    REQUIRE(sao_net_reassembler_create(&cfg, &r) == SAO_STATUS_OK);

    auto payload = make_payload(0x55, 24);
    auto pkt = build_tcp_ipv4(kFlowA, 7000, payload);
    // First delivery.
    REQUIRE(sao_net_reassembler_ingest(r, pkt.data(), pkt.size(), 1) ==
            SAO_STATUS_OK);
    auto first = drain_all(r);
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].bytes == payload);

    // Retransmit — same seq, same payload.  Must be dropped, no new
    // chunk queued.
    REQUIRE(sao_net_reassembler_ingest(r, pkt.data(), pkt.size(), 2) ==
            SAO_STATUS_OK);
    auto second = drain_all(r);
    REQUIRE(second.empty());

    SaoReassemblerStats stats{};
    REQUIRE(sao_net_reassembler_get_stats(r, &stats) == SAO_STATUS_OK);
    REQUIRE(stats.packets_dropped_retransmit >= 1);
    REQUIRE(stats.bytes_delivered == payload.size());

    sao_net_reassembler_destroy(r);
}

TEST_CASE("reasm_timeout_flushes_stale", "[net][automation][reasm]") {
    SaoReassemblerConfig cfg{};
    cfg.timeout_ms = 100;  // 100 ms — small so the test is fast
    sao_net_reassembler_handle_t r = nullptr;
    REQUIRE(sao_net_reassembler_create(&cfg, &r) == SAO_STATUS_OK);

    // Set up an OOO gap that would otherwise never drain.
    // Segment A opens the flow.
    auto pA = make_payload(0xA0, 4);
    auto pktA = build_tcp_ipv4(kFlowA, 500, pA);
    REQUIRE(sao_net_reassembler_ingest(r, pktA.data(), pktA.size(), 0) ==
            SAO_STATUS_OK);
    (void)drain_all(r);  // clear A

    // Segment C sits out of order — B never arrives.
    auto pC = make_payload(0xC0, 4);
    auto pktC = build_tcp_ipv4(kFlowA, 508, pC);
    REQUIRE(sao_net_reassembler_ingest(r, pktC.data(), pktC.size(), 10) ==
            SAO_STATUS_OK);
    REQUIRE(drain_all(r).empty());

    // Drive time forward past the timeout by ingesting an UNRELATED
    // packet on a different flow — gc_timeouts runs on every ingest.
    auto pOther = make_payload(0x77, 1);
    auto pktOther = build_tcp_ipv4(kFlowB, 9000, pOther);
    REQUIRE(sao_net_reassembler_ingest(r, pktOther.data(), pktOther.size(),
                                        10 + 200) == SAO_STATUS_OK);

    // FlowA should have been evicted; its buffered OOO chunk C is
    // silently dropped (no pending in-order bytes to flush).  We only
    // observe the effect via stats.
    SaoReassemblerStats stats{};
    REQUIRE(sao_net_reassembler_get_stats(r, &stats) == SAO_STATUS_OK);
    REQUIRE(stats.streams_flushed_timeout >= 1);

    sao_net_reassembler_destroy(r);
}

TEST_CASE("reasm_multi_flow_isolation", "[net][automation][reasm]") {
    SaoReassemblerConfig cfg{};
    sao_net_reassembler_handle_t r = nullptr;
    REQUIRE(sao_net_reassembler_create(&cfg, &r) == SAO_STATUS_OK);

    auto pA1 = make_payload(0xA1, 4);
    auto pB1 = make_payload(0xB1, 5);
    auto pA2 = make_payload(0xA2, 4);
    auto pB2 = make_payload(0xB2, 5);

    // Interleave two independent flows.
    auto pkt_a1 = build_tcp_ipv4(kFlowA, 1000, pA1);
    auto pkt_b1 = build_tcp_ipv4(kFlowB, 5000, pB1);
    auto pkt_a2 = build_tcp_ipv4(kFlowA, 1004, pA2);
    auto pkt_b2 = build_tcp_ipv4(kFlowB, 5005, pB2);

    REQUIRE(sao_net_reassembler_ingest(r, pkt_a1.data(), pkt_a1.size(),
                                        1) == SAO_STATUS_OK);
    REQUIRE(sao_net_reassembler_ingest(r, pkt_b1.data(), pkt_b1.size(),
                                        2) == SAO_STATUS_OK);
    REQUIRE(sao_net_reassembler_ingest(r, pkt_a2.data(), pkt_a2.size(),
                                        3) == SAO_STATUS_OK);
    REQUIRE(sao_net_reassembler_ingest(r, pkt_b2.data(), pkt_b2.size(),
                                        4) == SAO_STATUS_OK);

    auto chunks = drain_all(r);
    // We must see at least one chunk for each stream_id, and each
    // stream's bytes must be exactly the concatenation of its two
    // payloads with no cross-contamination.
    std::vector<uint8_t> stream1_bytes;
    std::vector<uint8_t> stream2_bytes;
    sao_net_stream_id_t sid1 = 0, sid2 = 0;
    for (auto& c : chunks) {
        if (sid1 == 0) {
            sid1 = c.stream_id;
        } else if (c.stream_id != sid1 && sid2 == 0) {
            sid2 = c.stream_id;
        }
        if (c.stream_id == sid1) {
            stream1_bytes.insert(stream1_bytes.end(), c.bytes.begin(),
                                  c.bytes.end());
        } else if (c.stream_id == sid2) {
            stream2_bytes.insert(stream2_bytes.end(), c.bytes.begin(),
                                  c.bytes.end());
        }
    }
    REQUIRE(sid1 != 0);
    REQUIRE(sid2 != 0);
    REQUIRE(sid1 != sid2);

    // Reconstruct expected — first flow to arrive gets sid1.  A arrived
    // first (kFlowA), so stream1 == A's bytes.
    std::vector<uint8_t> expA;
    expA.insert(expA.end(), pA1.begin(), pA1.end());
    expA.insert(expA.end(), pA2.begin(), pA2.end());
    std::vector<uint8_t> expB;
    expB.insert(expB.end(), pB1.begin(), pB1.end());
    expB.insert(expB.end(), pB2.begin(), pB2.end());
    REQUIRE(stream1_bytes == expA);
    REQUIRE(stream2_bytes == expB);

    SaoReassemblerStats stats{};
    REQUIRE(sao_net_reassembler_get_stats(r, &stats) == SAO_STATUS_OK);
    REQUIRE(stats.streams_created == 2);
    REQUIRE(stats.streams_active == 2);

    sao_net_reassembler_destroy(r);
}
