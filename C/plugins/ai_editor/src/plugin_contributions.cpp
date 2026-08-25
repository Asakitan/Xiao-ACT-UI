// SAO AI Editor — plugin manifest contributions (Python parity).

#include "plugin_contributions.h"

#include "sao/ai_editor/mcp_client.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace sao::ai_editor::native {

namespace {

// plugin.json must stay small; anything beyond this is rejected outright
// (fail-closed, mirrors loader manifest budget posture).
constexpr std::size_t kMaximumManifestBytes = 1U * 1024U * 1024U;

void push_diagnostic(std::vector<std::string>& diagnostics,
                     std::string message) {
    if (diagnostics.size() >= kMaximumPluginContributionDiagnostics) {
        return;
    }
    diagnostics.push_back(std::move(message));
}

}  // namespace

bool valid_manifest_provider_id(std::string_view raw_id) noexcept {
    if (raw_id.empty()) {
        return false;
    }
    if (raw_id.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char character : raw_id) {
        if (character == '/' || character == '\\') {
            return false;
        }
    }
    return true;
}

bool scan_plugin_root(const std::filesystem::path& root_dir,
                      std::string_view plugin_id,
                      PluginContributions& out) {
    if (plugin_id.empty() || !valid_simple_id(plugin_id)) {
        push_diagnostic(out.diagnostics,
                        "skipped plugin root with invalid id \"" +
                            std::string(plugin_id) + "\"");
        return false;
    }
    std::error_code error;
    const std::filesystem::path manifest = root_dir / L"plugin.json";
    if (!std::filesystem::is_regular_file(manifest, error) || error) {
        return false;
    }
    std::ifstream input(manifest, std::ios::binary);
    if (!input) {
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    if (text.size() > kMaximumManifestBytes) {
        push_diagnostic(out.diagnostics,
                        "skipped oversized manifest for plugin \"" +
                            std::string(plugin_id) + "\"");
        return false;
    }
    Json data;
    try {
        data = Json::parse(text, nullptr, false);
    } catch (...) {
        push_diagnostic(out.diagnostics,
                        "skipped unparseable manifest for plugin \"" +
                            std::string(plugin_id) + "\"");
        return false;
    }
    if (!data.is_object()) {
        push_diagnostic(out.diagnostics,
                        "skipped non-object manifest for plugin \"" +
                            std::string(plugin_id) + "\"");
        return false;
    }
    if (data.contains("mcpServers") && data["mcpServers"].is_object()) {
        for (const auto& [server_id, server_config] :
             data["mcpServers"].items()) {
            std::string name(plugin_id);
            name.push_back('.');
            name.append(server_id);
            if (!server_config.is_object() || out.mcp_servers.contains(name)) {
                continue;  // first plugin with a given id wins (Python parity)
            }
            out.mcp_servers[name] = server_config;
        }
    }
    if (data.contains("chatProviders") && data["chatProviders"].is_array()) {
        for (const auto& entry : data["chatProviders"]) {
            if (!entry.is_object()) {
                continue;
            }
            const std::string raw_id = entry.value("id", std::string{});
            if (!valid_manifest_provider_id(raw_id)) {
                push_diagnostic(out.diagnostics,
                                "skipped unsafe chat provider id in plugin \"" +
                                    std::string(plugin_id) + "\"");
                continue;
            }
            Json declared = entry;
            declared["id"] = std::string(plugin_id) + "." + raw_id;
            out.chat_providers.push_back(std::move(declared));
        }
    }
    return true;
}

void scan_plugin_contributions(const Json& plugin_roots,
                               PluginContributions& out) {
    if (!plugin_roots.is_object()) {
        return;
    }
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    for (const auto& [id, path_value] : plugin_roots.items()) {
        if (!valid_simple_id(id) || !path_value.is_string()) {
            continue;
        }
        std::filesystem::path root(path_value.get<std::string>());
        std::error_code error;
        root = std::filesystem::weakly_canonical(root, error);
        if (error) {
            continue;
        }
        roots.emplace_back(id, std::move(root));
    }
    std::sort(roots.begin(), roots.end(),
              [](const auto& left, const auto& right) {
                  if (left.first != right.first) {
                      return left.first < right.first;
                  }
                  return left.second < right.second;
              });
    for (const auto& [id, root] : roots) {
        scan_plugin_root(root, id, out);
    }
}

bool plugin_mcp_autostart_enabled(const Json& mcp_settings) {
    if (!mcp_settings.is_object()) {
        return false;
    }
    if (!mcp_settings.value("enabled", true)) {
        return false;
    }
    if (!mcp_settings.value("discovery_enabled", true)) {
        return false;
    }
    if (!mcp_settings.value("autostart", false)) {
        return false;
    }
    if (!mcp_settings.value("workspace_trusted", false)) {
        return false;
    }
    return true;
}

int32_t register_plugin_mcp_servers(SaoAiEditorMcpClient* client,
                                    const Json& servers,
                                    std::vector<std::string>& diagnostics) {
    if (client == nullptr || !servers.is_object()) {
        return 0;
    }
    int32_t registered = 0;
    for (const auto& [name, raw_config] : servers.items()) {
        if (!raw_config.is_object()) {
            continue;
        }
        Json config = raw_config;
        // The client registry key is the namespaced id; the manifest cannot
        // override it (any manifest "name" becomes a display-only leftover,
        // which matches how Python treats the id/config split).
        config["name"] = name;
        const std::string serialized = dump_json(config);
        const int32_t status = sao_ai_editor_mcp_client_register(
            client, serialized.data(),
            static_cast<uint32_t>(serialized.size()));
        if (status == SAO_AI_EDITOR_OK) {
            ++registered;
        } else {
            push_diagnostic(diagnostics,
                            "plugin MCP server \"" + name +
                                "\" failed to register (status " +
                                std::to_string(status) + ")");
        }
    }
    return registered;
}

}  // namespace sao::ai_editor::native