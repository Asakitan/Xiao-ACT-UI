// SAO Auto — URL parse/build helpers.
//
// Small, allocation-free façade so callers can pull the scheme / host /
// port / path out of a URL without dragging a heavier dependency in.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/net/npcap_capture.h"  // SAO_NET_API macro

#ifdef __cplusplus
extern "C" {
#endif

struct SaoUrlParts {
    // All offsets/lengths are into the input string; the caller must
    // keep the input alive while it uses these views.
    uint16_t scheme_off, scheme_len;
    uint16_t host_off,   host_len;
    uint16_t path_off,   path_len;
    uint16_t query_off,  query_len;
    uint16_t fragment_off, fragment_len;
    uint16_t port;                  // 0 when absent
    bool     port_present;
    uint8_t  _pad[3];
};

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_url_parse(
    const char* url_utf8, SaoUrlParts* out_parts);

// URL-encode a plain ASCII segment.  Percent-encodes anything outside
// the RFC 3986 "unreserved" set.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_url_encode_component(
    const char* input_utf8,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_needed);

// URL-decode.  Invalid percent sequences return SAO_STATUS_ERR_INVALID_ARGUMENT.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_url_decode_component(
    const char* input_utf8,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_needed);

#ifdef __cplusplus
}  // extern "C"
#endif
