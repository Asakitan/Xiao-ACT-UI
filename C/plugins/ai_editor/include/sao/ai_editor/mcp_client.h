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

SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL sao_ai_editor_mcp_client_destroy(
    sao_ai_editor_mcp_client_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
