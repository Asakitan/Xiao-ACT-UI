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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SaoAiEditorMcpClient;

namespace sao::ai_editor::native {

inline constexpr std::size_t kMaximumPluginContributionDiagnostics = 16;
inline constexpr std::size_t kMaximumExtensionManifestBytes = 1U * 1024U * 1024U;
inline constexpr std::size_t kMaximumExtensionMainBytes = 1U * 1024U * 1024U;

enum class ManifestTextParseResult : uint8_t {
    ok,
    invalid,
    duplicate_key,
    invalid_value,
};

ManifestTextParseResult parse_manifest_text_strict(std::string_view text, Json& result);

struct ManifestContributionInventory {
    Json commands = Json::array();
    Json view_containers = Json::array();
    Json views = Json::array();
    Json menus = Json::array();
    Json configurations = Json::array();
    Json notebooks = Json::array();
    Json debuggers = Json::array();
    Json task_definitions = Json::array();
    Json custom_editors = Json::array();
    Json mcp_servers = Json::array();
    Json chat_providers = Json::array();
    bool truncated = false;
    std::size_t total_entries = 0;
    std::size_t total_bytes = 0;
    std::string owner_path;
    std::optional<std::size_t> owner_index;

    Json summary() const;
    Json to_json() const;
};

bool parse_manifest_contribution_inventory(const Json& manifest, std::string_view owner_id,
                                           ManifestContributionInventory& out,
                                           std::vector<std::string>* diagnostics = nullptr,
                                           std::string_view owner_path = {},
                                           std::optional<std::size_t> owner_index = std::nullopt);

struct PluginContributions {
    // "<plugin>.<server>" -> bounded, owner-tagged server config object.
    Json mcp_servers = Json::object();
    // Validated chat provider entries with id rewritten to "<plugin>.<id>".
    Json chat_providers = Json::array();
    // Bounded, owner-tagged inventory for every parseable plugin manifest.
    Json inventories = Json::array();
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
bool scan_plugin_root(const std::filesystem::path& root_dir, std::string_view plugin_id,
                      PluginContributions& out);

// Scan an {id: root-path} plugin root map (the same shape
// ScopeStore::initialize accepts).  Deterministic (sorted by id).
void scan_plugin_contributions(const Json& plugin_roots, PluginContributions& out);

// Python mcp_client.load_mcp_configs parity: plugin manifests are executable
// configuration and stay inert until enabled + discovery + autostart +
// workspace trust are all satisfied.
bool plugin_mcp_autostart_enabled(const Json& mcp_settings);

// Register every scanned MCP server into the client.  Per-server failures
// are collected in `diagnostics` and never fail the batch.  Returns the
// number of successful registrations.
int32_t register_plugin_mcp_servers(SaoAiEditorMcpClient* client, const Json& servers,
                                    std::vector<std::string>& diagnostics);

} // namespace sao::ai_editor::native