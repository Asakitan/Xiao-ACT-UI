// SAO AI Editor — plugin manifest contributions (Python parity).
//
// The Python AI Editor reads plugins/<id>/plugin.json twice: mcp_client
// merges manifest `mcpServers` (namespaced as "<id>.<server>") into the MCP
// server list, and chat_providers registers manifest `chatProviders` (same
// namespacing, file-safe id validation).  The C++ runtime owns an mcp_client
// but has no provider registry, so both scans live here and the runtime
// decides how to surface each half.

#pragma once

#include "native_utils.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct SaoAiEditorMcpClient;

namespace sao::ai_editor::native {

inline constexpr std::size_t kMaximumPluginContributionDiagnostics = 16;

struct PluginContributions {
    // "<plugin>.<server>" -> raw manifest server config object.
    Json mcp_servers = Json::object();
    // Validated chat provider entries with id rewritten to "<plugin>.<id>".
    Json chat_providers = Json::array();
    // Best-effort diagnostics (capped); never fails the scan.
    std::vector<std::string> diagnostics;
};

// Python chat_providers.load_manifest_chat_providers parity: a
// manifest-supplied id must be a bare, file-safe identifier (no path
// segments, no ".." traversal) before it may live inside the
// "<plugin>.<id>" namespace.
bool valid_manifest_provider_id(std::string_view raw_id) noexcept;

// Scan one plugin root (a directory containing plugin.json).  Returns true
// when a parseable manifest existed; contributions are appended to `out`.
bool scan_plugin_root(const std::filesystem::path& root_dir,
                      std::string_view plugin_id,
                      PluginContributions& out);

// Scan an {id: root-path} plugin root map (the same shape
// ScopeStore::initialize accepts).  Deterministic (sorted by id).
void scan_plugin_contributions(const Json& plugin_roots,
                               PluginContributions& out);

// Python mcp_client.load_mcp_configs parity: plugin manifests are executable
// configuration and stay inert until enabled + discovery + autostart +
// workspace trust are all satisfied.
bool plugin_mcp_autostart_enabled(const Json& mcp_settings);

// Register every scanned MCP server into the client.  Per-server failures
// are collected in `diagnostics` and never fail the batch.  Returns the
// number of successful registrations.
int32_t register_plugin_mcp_servers(SaoAiEditorMcpClient* client,
                                    const Json& servers,
                                    std::vector<std::string>& diagnostics);

}  // namespace sao::ai_editor::native