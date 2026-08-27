// Protobuf varint and zstd compress/decompress primitives.
//
// The zstd path routes through vcpkg's `zstd` package.  When the port is
// unavailable the build system defines `SAO_NET_ZSTD_MISSING` and every
// zstd entry point stubs to SAO_STATUS_ERR_CAPABILITY_MISSING,
// reclassified from NOT_IMPLEMENTED so callers can tell "zstd not linked
// in this build" from "feature never written").  This keeps the ABI stable
// regardless of dependency provenance.
//
// The protobuf varint decoder is entirely self-contained: no external
// dependency, no schema requirement.

#include "sao/net/proto_zstd.h"

#include "sao/core/error.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef SAO_NET_ZSTD_MISSING
#  include <zstd.h>
#endif

namespace {

sao_status_t fail_zstd(sao_status_t status,
                       const char* message,
                       const char* file,
                       uint32_t line) {
    sao_core_error_set(status, "net.zstd", message, file, line);
    return status;
}

sao_status_t fail_proto(sao_status_t status,
                        const char* message,
                        const char* file,
                        uint32_t line) {
    sao_core_error_set(status, "net.protobuf", message, file, line);
    return status;
}

}  // namespace

extern "C" sao_status_t SAO_NET_CALL sao_net_zstd_available(bool* available_out) {
    if (available_out == nullptr) {
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         "available_out is null", __FILE__, __LINE__);
    }
#ifdef SAO_NET_ZSTD_MISSING
    *available_out = false;
#else
    *available_out = true;
#endif
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_zstd_decompress(
    const uint8_t* input,
    size_t input_size,
    uint8_t* output_out,
    size_t out_capacity,
    size_t* out_size_out) {
    if (out_size_out == nullptr) {
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         "out_size_out is null", __FILE__, __LINE__);
    }
    *out_size_out = 0;
    if (input == nullptr || input_size == 0) {
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         "input is empty", __FILE__, __LINE__);
    }
    if (output_out == nullptr && out_capacity != 0) {
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         "output_out mismatch", __FILE__, __LINE__);
    }

#ifdef SAO_NET_ZSTD_MISSING
    (void)output_out;
    (void)out_capacity;
    // Build-time capability gate — the vcpkg zstd port wasn't
    // available at configure time.  Advertised via sao_net_zstd_available.
    return fail_zstd(SAO_STATUS_ERR_CAPABILITY_MISSING,
                     "zstd not compiled in", __FILE__, __LINE__);
#else
    // 1) Probe the frame size when known so callers can rely on
    //    BUFFER_TOO_SMALL to sound the alarm early.
    const unsigned long long frame_size =
        ZSTD_getFrameContentSize(input, input_size);
    if (frame_size == ZSTD_CONTENTSIZE_ERROR) {
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         "not a valid zstd frame", __FILE__, __LINE__);
    }
    if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (frame_size > static_cast<unsigned long long>(out_capacity)) {
            *out_size_out = static_cast<size_t>(frame_size);
            return fail_zstd(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                             "decompressed size exceeds buffer",
                             __FILE__, __LINE__);
        }
    }

    const size_t rc =
        ZSTD_decompress(output_out, out_capacity, input, input_size);
    if (ZSTD_isError(rc)) {
        const ZSTD_ErrorCode code = ZSTD_getErrorCode(rc);
        if (code == ZSTD_error_dstSize_tooSmall) {
            // Content size unknown case - propagate BUFFER_TOO_SMALL.
            return fail_zstd(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                             "output buffer too small", __FILE__, __LINE__);
        }
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         ZSTD_getErrorName(rc), __FILE__, __LINE__);
    }
    *out_size_out = rc;
    return SAO_STATUS_OK;
#endif
}

extern "C" sao_status_t SAO_NET_CALL sao_net_zstd_compress(
    const uint8_t* input,
    size_t input_size,
    uint8_t* output_out,
    size_t out_capacity,
    size_t* out_size_out,
    int32_t level) {
    if (out_size_out == nullptr) {
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         "out_size_out is null", __FILE__, __LINE__);
    }
    *out_size_out = 0;
    if (input == nullptr && input_size != 0) {
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         "input is null with nonzero size",
                         __FILE__, __LINE__);
    }
    if (output_out == nullptr && out_capacity != 0) {
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         "output_out mismatch", __FILE__, __LINE__);
    }

#ifdef SAO_NET_ZSTD_MISSING
    (void)level;
    // Build-time capability gate — see sao_net_zstd_decompress
    // for the taxonomy note.
    return fail_zstd(SAO_STATUS_ERR_CAPABILITY_MISSING,
                     "zstd not compiled in", __FILE__, __LINE__);
#else
    const int actual_level = (level == 0) ? ZSTD_CLEVEL_DEFAULT : level;
    // Bound check the level against the library's min/max.  0 preserved
    // above so callers can request "library default" without knowing the
    // range.
    const int min_level = ZSTD_minCLevel();
    const int max_level = ZSTD_maxCLevel();
    if (actual_level < min_level || actual_level > max_level) {
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         "compression level out of range",
                         __FILE__, __LINE__);
    }

    const size_t bound = ZSTD_compressBound(input_size);
    if (bound > out_capacity) {
        *out_size_out = bound;
        return fail_zstd(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                         "output buffer smaller than compressBound",
                         __FILE__, __LINE__);
    }

    const size_t rc = ZSTD_compress(output_out, out_capacity,
                                     input, input_size, actual_level);
    if (ZSTD_isError(rc)) {
        return fail_zstd(SAO_STATUS_ERR_INVALID_ARGUMENT,
                         ZSTD_getErrorName(rc), __FILE__, __LINE__);
    }
    *out_size_out = rc;
    return SAO_STATUS_OK;
#endif
}

extern "C" sao_status_t SAO_NET_CALL sao_net_protobuf_decode_varint(
    const uint8_t* buf,
    size_t size,
    uint64_t* value_out,
    size_t* consumed_out) {
    if (value_out == nullptr || consumed_out == nullptr) {
        return fail_proto(SAO_STATUS_ERR_INVALID_ARGUMENT,
                          "output pointers must be non-null",
                          __FILE__, __LINE__);
    }
    *value_out = 0;
    *consumed_out = 0;
    if (buf == nullptr || size == 0) {
        return fail_proto(SAO_STATUS_ERR_INVALID_ARGUMENT,
                          "buffer is empty", __FILE__, __LINE__);
    }

    // The tenth byte contributes exactly one bit and must terminate the
    // uint64 varint.
    uint64_t result = 0;
    for (size_t index = 0; index < size && index < 10; ++index) {
        const uint8_t byte = buf[index];
        if (index == 9) {
            if ((byte & 0x80u) != 0 || (byte & 0x7eu) != 0) {
                return fail_proto(SAO_STATUS_ERR_INVALID_ARGUMENT,
                                  "varint overflows uint64", __FILE__, __LINE__);
            }
            result |= static_cast<uint64_t>(byte & 0x01u) << 63u;
            *value_out = result;
            *consumed_out = 10;
            return SAO_STATUS_OK;
        }
        const uint64_t chunk = static_cast<uint64_t>(byte & 0x7F);
        result |= chunk << (7u * index);
        if ((byte & 0x80) == 0) {
            *value_out = result;
            *consumed_out = index + 1;
            return SAO_STATUS_OK;
        }
    }
    return fail_proto(SAO_STATUS_ERR_INVALID_ARGUMENT,
                      "varint truncated or too long",
                      __FILE__, __LINE__);
}
