#pragma once

#include <cstdint>
#include <filesystem>

#include "sao/ai_editor/ai_editor_status.h"

namespace sao::ai_editor::native {

// Run the AI editor as a Model Context Protocol server on stdio.  The MCP
// spec framing (Content-Length headers over stdio) is decoded here and
// dispatched into the same NativeRuntime tools registry the JSON-RPC
// handler uses, so external MCP clients (Claude Desktop, Cursor, etc.)
// can call readFile/listFiles/searchFiles/editFile inside the SAO
// workspace with the exact same permission model.
SAO_AI_EDITOR_API int run_mcp_server_stdio(
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
SAO_AI_EDITOR_API int run_extension_host_stdio(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& node_executable_hint);

}  // namespace sao::ai_editor::native
