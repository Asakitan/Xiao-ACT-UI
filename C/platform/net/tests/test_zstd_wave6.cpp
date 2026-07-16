// Wave 6 tests for platform/net protobuf/zstd primitives.
//
// Coverage:
//   * zstd_compress_decompress_roundtrip_small
//   * zstd_decompress_invalid_returns_error
//   * zstd_compress_level_1_and_22
//   * varint_decode_1_byte_and_multi_byte
//
// The zstd tests SKIP gracefully when the library is not compiled in
// (SAO_NET_ZSTD_MISSING route).  The varint test is dependency-free
// and always runs.

#include <catch2/catch_test_macros.hpp>

#include "sao/net/proto_zstd.h"
#include "sao/core/status.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

bool zstd_present() {
    bool available = false;
    if (sao_net_zstd_available(&available) != SAO_STATUS_OK) {
        return false;
    }
    return available;
}

}  // namespace

TEST_CASE("zstd_compress_decompress_roundtrip_small",
          "[net][zstd][wave6]") {
    if (!zstd_present()) {
        SKIP("zstd not compiled in - skipping roundtrip");
    }

    // Small payload with repeated content - zstd should shrink it.
    const std::string payload =
        "SAO-Auto Wave6 - the-quick-brown-fox-jumps-over-the-lazy-dog. "
        "SAO-Auto Wave6 - the-quick-brown-fox-jumps-over-the-lazy-dog. "
        "SAO-Auto Wave6 - the-quick-brown-fox-jumps-over-the-lazy-dog.";

    std::vector<uint8_t> compressed(payload.size() * 2 + 128);
    size_t compressed_size = 0;
    const sao_status_t comp_rc = sao_net_zstd_compress(
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size(),
        compressed.data(),
        compressed.size(),
        &compressed_size,
        0 /* library default */);
    REQUIRE(comp_rc == SAO_STATUS_OK);
    REQUIRE(compressed_size > 0u);
    REQUIRE(compressed_size < payload.size());  // repetition should shrink

    std::vector<uint8_t> restored(payload.size() + 16);
    size_t restored_size = 0;
    const sao_status_t decomp_rc = sao_net_zstd_decompress(
        compressed.data(),
        compressed_size,
        restored.data(),
        restored.size(),
        &restored_size);
    REQUIRE(decomp_rc == SAO_STATUS_OK);
    REQUIRE(restored_size == payload.size());
    REQUIRE(std::string(reinterpret_cast<const char*>(restored.data()),
                        restored_size) == payload);
}

TEST_CASE("zstd_decompress_invalid_returns_error",
          "[net][zstd][wave6]") {
    if (!zstd_present()) {
        SKIP("zstd not compiled in - skipping invalid-input check");
    }

    // Truly bogus bytes - definitely not a zstd frame.
    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    uint8_t output[64] = {0};
    size_t written = 0;
    const sao_status_t rc = sao_net_zstd_decompress(
        garbage, sizeof(garbage), output, sizeof(output), &written);
    REQUIRE(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(written == 0u);

    // Null output size pointer must be rejected.
    const sao_status_t null_rc = sao_net_zstd_decompress(
        garbage, sizeof(garbage), output, sizeof(output), nullptr);
    REQUIRE(null_rc == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Empty input must be rejected.
    const sao_status_t empty_rc = sao_net_zstd_decompress(
        nullptr, 0, output, sizeof(output), &written);
    REQUIRE(empty_rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("zstd_compress_level_1_and_22",
          "[net][zstd][wave6]") {
    if (!zstd_present()) {
        SKIP("zstd not compiled in - skipping level probe");
    }

    // Payload with enough content that levels can produce different
    // sizes.  We do not require level 22 < level 1 (small inputs often
    // yield equal frames) - only that both levels succeed and produce
    // a decompressible frame.
    const std::string payload(4096, 'A');

    for (int32_t level : {1, 22}) {
        std::vector<uint8_t> compressed(payload.size() * 2 + 128);
        size_t compressed_size = 0;
        const sao_status_t comp_rc = sao_net_zstd_compress(
            reinterpret_cast<const uint8_t*>(payload.data()),
            payload.size(),
            compressed.data(),
            compressed.size(),
            &compressed_size,
            level);
        REQUIRE(comp_rc == SAO_STATUS_OK);
        REQUIRE(compressed_size > 0u);

        std::vector<uint8_t> restored(payload.size() + 16);
        size_t restored_size = 0;
        const sao_status_t decomp_rc = sao_net_zstd_decompress(
            compressed.data(),
            compressed_size,
            restored.data(),
            restored.size(),
            &restored_size);
        REQUIRE(decomp_rc == SAO_STATUS_OK);
        REQUIRE(restored_size == payload.size());
        REQUIRE(std::memcmp(restored.data(), payload.data(),
                             restored_size) == 0);
    }

    // Out-of-range level must be rejected cleanly.
    std::vector<uint8_t> tmp(64);
    size_t tmp_used = 0;
    const sao_status_t bad_rc = sao_net_zstd_compress(
        reinterpret_cast<const uint8_t*>("x"),
        1,
        tmp.data(),
        tmp.size(),
        &tmp_used,
        9999 /* well out of ZSTD's range */);
    REQUIRE(bad_rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("varint_decode_1_byte_and_multi_byte",
          "[net][protobuf][varint][wave6]") {
    // Single-byte varint (value < 128).
    const uint8_t one[] = {0x2A};  // 42
    uint64_t value = 0;
    size_t consumed = 0;
    REQUIRE(sao_net_protobuf_decode_varint(one, sizeof(one),
                                            &value, &consumed) == SAO_STATUS_OK);
    REQUIRE(value == 42u);
    REQUIRE(consumed == 1u);

    // Two-byte varint: 300 = 0b10101100 0b00000010 = {0xAC, 0x02}
    const uint8_t two[] = {0xAC, 0x02};
    REQUIRE(sao_net_protobuf_decode_varint(two, sizeof(two),
                                            &value, &consumed) == SAO_STATUS_OK);
    REQUIRE(value == 300u);
    REQUIRE(consumed == 2u);

    // Max value that fits in 5 bytes: 2^32 - 1 = 4294967295.  Bytes:
    // 0xFF 0xFF 0xFF 0xFF 0x0F
    const uint8_t five[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
    REQUIRE(sao_net_protobuf_decode_varint(five, sizeof(five),
                                            &value, &consumed) == SAO_STATUS_OK);
    REQUIRE(value == 4294967295ull);
    REQUIRE(consumed == 5u);

    // Full 10-byte varint carrying UINT64_MAX.
    const uint8_t ten[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                            0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    REQUIRE(sao_net_protobuf_decode_varint(ten, sizeof(ten),
                                            &value, &consumed) == SAO_STATUS_OK);
    REQUIRE(value == 0xFFFFFFFFFFFFFFFFull);
    REQUIRE(consumed == 10u);

    // Truncated varint - MSB set on the final byte with no follow-up.
    const uint8_t truncated[] = {0xFF, 0xFF};
    REQUIRE(sao_net_protobuf_decode_varint(truncated, sizeof(truncated),
                                            &value, &consumed) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Empty buffer must be rejected.
    REQUIRE(sao_net_protobuf_decode_varint(nullptr, 0,
                                            &value, &consumed) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Null output must be rejected.
    REQUIRE(sao_net_protobuf_decode_varint(one, sizeof(one),
                                            nullptr, &consumed) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_net_protobuf_decode_varint(one, sizeof(one),
                                            &value, nullptr) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
}
