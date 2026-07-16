// SAO Auto — HTTPS client for License / cloud-config downloads.
//
// Backed by WinHTTP (Phase 3) so we don't drag libcurl into the exe.
// The API is deliberately blocking + string-in/string-out for now — the
// launcher does a handful of small requests at startup; nothing hot-path.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/net/npcap_capture.h"  // SAO_NET_API macro

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_net_http_response_s* sao_net_http_response_handle_t;

// Convenience GET.  headers_utf8 may be null; when present, it's a
// list of "Key: Value\r\n" pairs, terminated by an extra empty line
// (matching WinHTTP conventions).
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_http_get(
    const char* url_utf8,
    const char* headers_utf8,
    uint32_t timeout_ms,
    sao_net_http_response_handle_t* out_response);

// POST with a raw body.  content_type_utf8 defaults to "application/octet-stream".
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_http_post(
    const char* url_utf8,
    const char* headers_utf8,
    const char* content_type_utf8,
    const uint8_t* body_bytes,
    size_t body_length,
    uint32_t timeout_ms,
    sao_net_http_response_handle_t* out_response);

SAO_NET_API void SAO_NET_CALL sao_net_http_response_close(
    sao_net_http_response_handle_t response);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_http_response_status(
    sao_net_http_response_handle_t response, uint32_t* out_status_code);

// Query one response header by name.  The required byte count includes the
// trailing null.  Passing out_utf8=nullptr with capacity=0 is a sizing query.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_http_response_header(
    sao_net_http_response_handle_t response,
    const char* name_utf8,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_needed);

// Read the response body — subsequent calls continue after the last
// byte read.  On EOF returns SAO_STATUS_OK with *out_bytes_read == 0.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_http_response_read(
    sao_net_http_response_handle_t response,
    uint8_t* out_buffer,
    size_t buffer_len,
    size_t* out_bytes_read);

#ifdef __cplusplus
}  // extern "C"
#endif
