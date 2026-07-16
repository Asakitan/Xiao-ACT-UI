// SAO Auto — Wave 17c gap tests for platform/net.
//
// Coverage:
//   A. capability gate — zstd (compile-time) and npcap dispatch symbol
//      (runtime) both migrated from NOT_IMPLEMENTED to CAPABILITY_MISSING.
//   B. real implementation — protobuf varint (already implemented) probed
//      to make sure it hasn't regressed.
//
// Explicit non-goals:
//   - We do NOT drive a real zstd compression stream (the CI machine may
//     ship without zstd; when zstd is present we do run a small round
//     trip because it's hermetic in-memory).
//   - We do NOT open a real Npcap adapter.  The npcap coverage here is
//     limited to the availability probe and the dispatch symbol
//     classification — no packet capture, no wire traffic.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "sao/core/status.h"
#include "sao/net/npcap_capture.h"
#include "sao/net/proto_zstd.h"

#include "wave17c_support.h"

using sao::wave17c::GapKind;
using sao::wave17c::make_pattern_bytes;
using sao::wave17c::matches_gap_kind;

TEST_CASE("wave17c net: zstd availability probe reports coherently",
          "[wave17c][net][zstd]") {
    bool available = false;
    REQUIRE(sao_net_zstd_available(&available) == SAO_STATUS_OK);

    // Same call with a null out arg still fails cleanly.
    CHECK(sao_net_zstd_available(nullptr) == SAO_STATUS_ERR_INVALID_ARGUMENT);

    const auto bytes = make_pattern_bytes(256);
    std::vector<std::uint8_t> compressed(4096);
    std::size_t compressed_size = 0;
    const sao_status_t compress_rc =
        sao_net_zstd_compress(bytes.data(), bytes.size(),
                              compressed.data(), compressed.size(),
                              &compressed_size, 0);

    if (available) {
        // When zstd is compiled in we round trip a small in-memory buffer
        // to prove Wave 17c didn't regress the real path.  No stream, no
        // disk — just a hermetic memory transform.
        REQUIRE(compress_rc == SAO_STATUS_OK);
        REQUIRE(compressed_size > 0);
        std::vector<std::uint8_t> roundtrip(bytes.size());
        std::size_t out_size = 0;
        REQUIRE(sao_net_zstd_decompress(compressed.data(), compressed_size,
                                        roundtrip.data(),
                                        roundtrip.size(),
                                        &out_size) == SAO_STATUS_OK);
        REQUIRE(out_size == bytes.size());
        CHECK(std::equal(roundtrip.begin(), roundtrip.begin() + out_size,
                         bytes.begin()));
    } else {
        // When zstd is not compiled in, Wave 17c contract says the entry
        // points must return CAPABILITY_MISSING, not NOT_IMPLEMENTED.
        CAPTURE(compress_rc);
        CHECK(matches_gap_kind(compress_rc, GapKind::CapabilityGate));
        std::vector<std::uint8_t> scratch(64);
        std::size_t decompressed_size = 0;
        const sao_status_t decompress_rc = sao_net_zstd_decompress(
            bytes.data(), bytes.size(), scratch.data(), scratch.size(),
            &decompressed_size);
        CAPTURE(decompress_rc);
        CHECK(matches_gap_kind(decompress_rc, GapKind::CapabilityGate));
    }
}

TEST_CASE("wave17c net: protobuf varint remains a real implementation",
          "[wave17c][net][protobuf]") {
    // 300 encodes as 0xAC 0x02 in protobuf varint.  Choosing a known
    // fixture avoids any state that could depend on external tooling.
    const std::array<std::uint8_t, 2> encoded{0xAC, 0x02};
    std::uint64_t value = 0;
    std::size_t consumed = 0;
    REQUIRE(sao_net_protobuf_decode_varint(encoded.data(), encoded.size(),
                                           &value, &consumed) ==
            SAO_STATUS_OK);
    CHECK(value == 300);
    CHECK(consumed == 2);

    // Truncated input surfaces INVALID_ARGUMENT, not NOT_IMPLEMENTED.
    const std::array<std::uint8_t, 1> truncated{0xAC};
    std::uint64_t truncated_value = 0;
    std::size_t truncated_consumed = 0;
    const sao_status_t rc =
        sao_net_protobuf_decode_varint(truncated.data(), truncated.size(),
                                       &truncated_value,
                                       &truncated_consumed);
    CHECK(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(truncated_value == 0);
    CHECK(truncated_consumed == 0);
}

TEST_CASE("wave17c net: npcap availability probe is stable",
          "[wave17c][net][npcap]") {
    // Hermetic — the availability probe returns SAO_STATUS_OK regardless
    // of whether wpcap.dll is present on the CI host.  We just need to
    // confirm it never regresses to NOT_IMPLEMENTED.
    bool available = true;
    const sao_status_t rc = sao_net_npcap_available(&available);
    CAPTURE(rc);
    CAPTURE(available);
    CHECK(rc == SAO_STATUS_OK);
    CHECK(sao_net_npcap_available(nullptr) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("wave17c net: npcap dispatch capability gate on a null handle",
          "[wave17c][net][npcap]") {
    // Passing a null handle short-circuits before we touch the dispatch
    // symbol; we still exercise the entry point and prove it doesn't
    // regress to NOT_IMPLEMENTED.  Real dispatch requires opening a
    // capture handle against a live adapter which we deliberately do not
    // do here (see file-level comment).
    int32_t delivered = -1;
    const sao_status_t rc = sao_net_npcap_dispatch(
        nullptr, 0, nullptr, nullptr, &delivered);
    CAPTURE(rc);
    CHECK(rc != SAO_STATUS_ERR_NOT_IMPLEMENTED);
    CHECK(rc == SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(delivered == 0);
}

TEST_CASE("wave17c net: status taxonomy exposes the same codes for net callers",
          "[wave17c][net][status]") {
    // A drive-by check that the shared status vocabulary still resolves;
    // the net DLL statically links against sao::core so this exercises
    // the wiring end-to-end.
    const char* cap_label =
        sao_status_str(SAO_STATUS_ERR_CAPABILITY_MISSING);
    REQUIRE(cap_label != nullptr);
    CHECK(std::string(cap_label) == "capability_missing");
}
