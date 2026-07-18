// MCP (Model Context Protocol) client — spawns a stdio-based MCP server
// subprocess, performs the JSON-RPC initialize handshake, and exposes the
// standard MCP endpoints (tools/prompts/resources) to the AI editor runtime.
//
// The client owns one subprocess per registered server.  Requests are
// serialized with correlation ids and returned to the caller synchronously
// (with a per-request timeout).  Notifications and log records are queued
// for the runtime's event loop.
//
// Follows the AI editor NativeRuntime pattern — public entrypoints below
// stay behind opaque handles + JSON-in/JSON-out payloads.

#pragma once

#include <cstdint>

#include "sao/ai_editor/ai_editor_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SaoAiEditorMcpClient* sao_ai_editor_mcp_client_t;

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_mcp_client_create(
    sao_ai_editor_mcp_client_t* out_handle);

// Register (and start) a server with the given configuration.
//
// config_json fields (UTF-8 JSON object):
//   name       (string, required)  — display id for this server
//   transport  (string)            — "stdio" (default) or "http"
//
//   Stdio transport fields (used when transport == "stdio"):
//   command    (string, required)  — executable path
//   args       (array<string>)     — command-line arguments
//   env        (object<string,string>) — extra environment variables
//   cwd        (string)            — working directory (optional)
//
//   HTTP transport fields (used when transport == "http"):
//   url        (string, required)  — Streamable HTTP endpoint (http/https)
//   headers    (object<string,string>) — extra request headers, e.g.
//                                        {"Authorization": "Bearer …"}
//
//   startupMs  (integer)           — initialize handshake budget, default 15000
//
// The HTTP transport speaks the MCP Streamable HTTP protocol (2024-11-05+):
// every JSON-RPC turn is POSTed to the endpoint, and the server may reply
// with a single application/json body or a text/event-stream with one
// SSE event containing the response.  Long-lived GET /events subscriptions
// and resumability are not implemented here.
//
// Returns SAO_AI_EDITOR_OK on successful handshake.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_register(sao_ai_editor_mcp_client_t handle,
                                  const char* config_json,
                                  uint32_t config_len);

// Return the list of registered servers as a JSON array (NUL-terminated).
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_list_servers(sao_ai_editor_mcp_client_t handle,
                                      char* json_out,
                                      uint32_t json_cap,
                                      uint32_t* out_len);

// Return the aggregated tools/list result across all servers.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_list_tools(sao_ai_editor_mcp_client_t handle,
                                    char* json_out,
                                    uint32_t json_cap,
                                    uint32_t* out_len);

// Call one MCP tool.
//
// request_json fields:
//   server      (string, required)  — server name registered above
//   name        (string, required)  — tool identifier
//   arguments   (object)            — MCP tool arguments
//   timeoutMs   (integer)           — request budget, default 30000
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_call_tool(sao_ai_editor_mcp_client_t handle,
                                   const char* request_json,
                                   uint32_t request_len,
                                   char* response_out,
                                   uint32_t response_cap,
                                   uint32_t* out_len);

// Aggregated prompts/list across servers.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_list_prompts(sao_ai_editor_mcp_client_t handle,
                                      char* json_out,
                                      uint32_t json_cap,
                                      uint32_t* out_len);

// Render one MCP prompt (server-side prompts/get).
//
// request_json fields:
//   server      (string, required)  — server name registered above
//   name        (string, required)  — prompt identifier
//   arguments   (object)            — MCP prompt arguments (optional)
//   timeoutMs   (integer)           — request budget, default 30000
//
// On success the response echoes the MCP prompts/get result verbatim —
// typically {"description": <string>, "messages": [...]} — with the source
// "server" field mixed in so callers can trace which registry entry produced
// the reply.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_get_prompt(sao_ai_editor_mcp_client_t handle,
                                    const char* request_json,
                                    uint32_t request_len,
                                    char* response_out,
                                    uint32_t response_cap,
                                    uint32_t* out_len);

// Aggregated resources/list across servers.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_list_resources(sao_ai_editor_mcp_client_t handle,
                                        char* json_out,
                                        uint32_t json_cap,
                                        uint32_t* out_len);

// Read one MCP resource.
// request_json fields: server, uri (required), timeoutMs (optional).
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_read_resource(sao_ai_editor_mcp_client_t handle,
                                       const char* request_json,
                                       uint32_t request_len,
                                       char* response_out,
                                       uint32_t response_cap,
                                       uint32_t* out_len);

// Shut down one server by name (empty name → all servers).
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_close(sao_ai_editor_mcp_client_t handle,
                               const char* server_name);

// ---- Notifications -------------------------------------------------------
//
// MCP servers may emit JSON-RPC notifications (messages with a `method` but
// no `id`) to announce list changes (`notifications/tools/list_changed`,
// `notifications/prompts/list_changed`, `notifications/resources/list_changed`,
// `notifications/resources/updated`) or log records (`notifications/message`).
// This client offers two ways for the runtime to observe them:
//
//   1. A push-style forwarder installed via
//      sao_ai_editor_mcp_client_set_notification_forwarder — every notification
//      is JSON-serialized (as a single-line UTF-8 string) and handed to the
//      caller's function pointer immediately from the reader thread.  The
//      envelope shape is:
//        {"server":"<name>","notification":{...raw JSON-RPC notification...}}
//      Passing a null `fn` clears the forwarder (and drops any queued items).
//      Callback runs on the client's stdio reader thread — keep it fast.
//
//   2. A pull-style queue drained via
//      sao_ai_editor_mcp_client_next_notification (same envelope shape).
//      Notifications received while no forwarder is installed accumulate in a
//      bounded queue (older entries are dropped once the cap is reached).
//
// The two mechanisms coexist:  installing a forwarder short-circuits queue
// growth (the forwarder is invoked and nothing lands in the queue), so the
// pull API is safe to leave idle when the runtime consumes the push feed.
//
// The HTTP transport is strictly request/response, so notifications never fire
// on servers registered with transport="http".

typedef void (SAO_AI_EDITOR_CALL* sao_ai_editor_mcp_notification_fn)(
    void* user, const char* json_utf8, uint32_t json_len);

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_set_notification_forwarder(
    sao_ai_editor_mcp_client_t handle,
    void* user,
    sao_ai_editor_mcp_notification_fn fn);

// Drain one pending notification into json_out (NUL-terminated).  Returns
// SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL when the caller-provided buffer is too
// small (writing the required size into *out_len), SAO_AI_EDITOR_ERR_NOT_FOUND
// when no notification is queued, and SAO_AI_EDITOR_OK on successful drain.
// Uses the two-call size-then-drain pattern shared by the rest of this API.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_next_notification(
    sao_ai_editor_mcp_client_t handle,
    char* json_out,
    uint32_t json_cap,
    uint32_t* out_len);

SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL sao_ai_editor_mcp_client_destroy(
    sao_ai_editor_mcp_client_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
