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

}  // namespace sao::ai_editor::native
