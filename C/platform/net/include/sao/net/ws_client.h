// SAO Auto — WebSocket client (WSS supported).
//
// Used by the launcher for the optional cloud sync/telemetry channel.
// Zero external deps in the ABI — the runtime implementation sits on
// top of WinHTTP's WebSocket layer.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/net/npcap_capture.h"  // SAO_NET_API macro

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_net_ws_client_s* sao_net_ws_client_handle_t;

enum sao_net_ws_message_type_e : int32_t {
    SAO_NET_WS_MSG_TEXT   = 0,
    SAO_NET_WS_MSG_BINARY = 1,
    SAO_NET_WS_MSG_CLOSE  = 2,
};

typedef void (SAO_NET_CALL* sao_net_ws_message_callback_t)(
    int32_t message_type,
    const uint8_t* payload,
    size_t payload_len,
    void* user_data);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_ws_connect(
    const char* url_utf8,               // wss://…
    const char* headers_utf8,           // optional extra HTTP handshake headers
    uint32_t timeout_ms,
    sao_net_ws_client_handle_t* out_handle);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_ws_connect_scoped_v2(
    const char* url_utf8,               // wss://…
    const char* scope_utf8,              // exact TLS scope
    const char* headers_utf8,           // optional extra HTTP handshake headers
    uint32_t timeout_ms,
    sao_net_ws_client_handle_t* out_handle);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_ws_start_receive(
    sao_net_ws_client_handle_t handle,
    sao_net_ws_message_callback_t callback,
    void* user_data);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_ws_send(
    sao_net_ws_client_handle_t handle,
    int32_t message_type,
    const uint8_t* payload,
    size_t payload_len);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_ws_close(
    sao_net_ws_client_handle_t handle, uint16_t close_code);

SAO_NET_API void SAO_NET_CALL sao_net_ws_destroy(
    sao_net_ws_client_handle_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
