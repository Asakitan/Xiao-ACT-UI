// SAO Auto - Wave 6 - protobuf + zstd primitives.
//
// This header exposes the *game-agnostic* low-level building blocks for
// wire-format work:
//
//   * zstd compress / decompress that any plugin can use for its own
//     transport framing;
//   * a protobuf varint decoder that recognises the base wire type
//     without any .proto schema knowledge.
//
// The concrete .proto for star_resonance lives in the plugin, not here.
// The platform provides bytes-in-bytes-out primitives; parsers assemble
// them into typed getters.
//
// The zstd APIs route through `find_package(zstd CONFIG)` provided by
// vcpkg's `zstd` port.  When the ports are absent the CMake gate
// disables the source file and the symbols are exported as stubs that
// return SAO_STATUS_ERR_CAPABILITY_MISSING (Wave 17c — reclassified from
// NOT_IMPLEMENTED so callers can distinguish "zstd not linked in this
// build" from "feature never written").  Callers should therefore not
// assume zstd is always compiled in and can probe via a small
// roundtrip smoke test at startup.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/net/npcap_capture.h"  // for SAO_NET_API/CALL macros

#ifdef __cplusplus
extern "C" {
#endif

// zstd availability probe.  Returns SAO_STATUS_OK regardless; the caller
// inspects `*available_out`.  When false, every other zstd call in this
// header returns SAO_STATUS_ERR_CAPABILITY_MISSING (Wave 17c; see CMake gate).
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_zstd_available(
    bool* available_out);

// Decompress `input`/`input_size` into the caller-provided `output_out`
// buffer.  `*out_size_out` receives the number of bytes actually
// written on success or the required buffer size on
// SAO_STATUS_ERR_BUFFER_TOO_SMALL.  Malformed input (bad magic, missing
// frame header, corrupted body) resolves to SAO_STATUS_ERR_INVALID_ARGUMENT.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_zstd_decompress(
    const uint8_t* input,
    size_t input_size,
    uint8_t* output_out,
    size_t out_capacity,
    size_t* out_size_out);

// Compress `input` into a zstd frame.  `level` follows ZSTD's convention:
// 1 = fastest, 22 = smallest.  0 selects the library default (3).
// SAO_STATUS_ERR_BUFFER_TOO_SMALL populates `*out_size_out` with the
// worst-case required size so callers can retry with a larger buffer.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_zstd_compress(
    const uint8_t* input,
    size_t input_size,
    uint8_t* output_out,
    size_t out_capacity,
    size_t* out_size_out,
    int32_t level);

// Decode one protobuf base-128 varint from `buf`.  `*value_out` gets the
// unsigned 64-bit result; `*consumed_out` is set to the byte count that
// was walked (1..10).  Truncated / non-terminating varints yield
// SAO_STATUS_ERR_INVALID_ARGUMENT.  This is deliberately the *only*
// protobuf primitive in the platform - full message decoding requires
// the schema and therefore stays in the plugin.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_protobuf_decode_varint(
    const uint8_t* buf,
    size_t size,
    uint64_t* value_out,
    size_t* consumed_out);

#ifdef __cplusplus
}  // extern "C"
#endif
