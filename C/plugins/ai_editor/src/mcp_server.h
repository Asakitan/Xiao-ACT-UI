#pragma once

#include <cstdint>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "sao/ai_editor/ai_editor_native.h"
#include "sao/ai_editor/ai_editor_status.h"

namespace sao::ai_editor::native {

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL dispatch_mcp_message(
    sao_ai_editor_runtime_t runtime,
    const nlohmann::json& request,
    nlohmann::json& response);

// Run the AI editor as a Model Context Protocol server on stdio.  The MCP
// spec framing (Content-Length headers over stdio) is decoded here and
// dispatched into the same NativeRuntime tools registry the JSON-RPC
// handler uses, so external MCP clients (Claude Desktop, Cursor, etc.)
// can call readFile/listFiles/searchFiles/editFile inside the SAO
// workspace with the exact same permission model.
SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL run_mcp_server_stdio(
    const std::filesystem::path& workspace_root);

// Run the AI editor as a long-lived extension host over stdio.  Unlike the
// MCP server, this passes JSON-RPC requests straight through to the
// NativeRuntime (extensions.*/tools.*/mcp.*/workflows.*/agents.*) so a
// parent process can drive the full runtime surface — including loading
// the Node.js extension host shim and activating real VS Code extensions
// — without owning a named pipe.  Framing follows the MCP spec
// (Content-Length headers) so the existing decoder can be reused.  The
// optional node_executable hint is applied as an implicit
// `extensions.configure_host` before the request loop starts; passing an
// empty path leaves configuration to the caller.  The loop terminates on
// stdin EOF or a `host.shutdown` JSON-RPC request (which is answered
// before exiting so the parent sees a clean handshake).
SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL run_extension_host_stdio(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& node_executable_hint);

}  // namespace sao::ai_editor::native

namespace sao::ai_editor {

// Streamable HTTP entrypoints. The legacy port-only start creates a
// generated bearer token retrievable through the query API.
extern "C" SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL
sao_ai_editor_run_mcp_server_streamable_http(
    uint16_t port);
extern "C" SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL
sao_ai_editor_run_mcp_server_streamable_http_with_token(
    uint16_t port, const char* token_utf8);
extern "C" SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL
sao_ai_editor_run_mcp_server_streamable_http_ex(
    uint16_t port, const char* workspace_utf8, const char* token_utf8);
extern "C" SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL
sao_ai_editor_stop_mcp_server_streamable_http(void);
extern "C" SAO_AI_EDITOR_API uint16_t SAO_AI_EDITOR_CALL sao_ai_editor_mcp_http_port(void);
extern "C" SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL sao_ai_editor_mcp_http_token(
    char* token_out, uint32_t token_capacity, uint32_t* token_size);

}  // namespace sao::ai_editor
