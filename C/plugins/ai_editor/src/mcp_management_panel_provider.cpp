#include "mcp_management_panel_provider.h"

#include "kernel_map_panel_provider.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace sao::ai_editor::native {

namespace {

std::string read_asset(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream output;
    output << stream.rdbuf();
    return output.str();
}

std::string fallback_html() {
    return R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><title>MCP Management</title></head><body><h1>MCP Management</h1><p>MCP panel assets are unavailable.</p></body></html>)HTML";
}

}

void McpManagementPanelProvider::install_snapshot_provider(
    SnapshotProvider provider) {
    std::lock_guard<std::mutex> guard(mutex_);
    snapshot_provider_ = std::move(provider);
}

void McpManagementPanelProvider::install_kernel_map_navigator(
    KernelMapNavigator navigator) {
    std::lock_guard<std::mutex> guard(mutex_);
    kernel_map_navigator_ = std::move(navigator);
}

bool McpManagementPanelProvider::is_registered() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return registered_;
}

std::string McpManagementPanelProvider::load_bundled_html(
    const std::string& assets_root) const {
    if (assets_root.empty()) {
        return fallback_html();
    }
    const std::filesystem::path root(assets_root);
    std::string html = read_asset(root / "index.html");
    if (html.empty()) {
        return fallback_html();
    }
    const std::string css = read_asset(root / "panel.css");
    const std::string script = read_asset(root / "panel.js");
    if (!css.empty()) {
        const std::string marker = R"(<link rel="stylesheet" href="panel.css">)";
        const auto position = html.find(marker);
        if (position != std::string::npos) {
            html.replace(position, marker.size(), "<style>\n" + css + "\n</style>");
        }
    }
    if (!script.empty()) {
        const std::string marker = R"(<script src="panel.js"></script>)";
        const auto position = html.find(marker);
        if (position != std::string::npos) {
            html.replace(position, marker.size(),
                         "<script>\n" + script + "\n</script>");
        }
    }
    return html;
}

int32_t McpManagementPanelProvider::register_with_runtime(
    WebviewPanelRegistry& registry, const std::string& assets_root) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (registered_) {
        return SAO_AI_EDITOR_OK;
    }
    WebviewPanelOptions options;
    options.enable_scripts = true;
    options.retain_context_when_hidden = true;
    options.view_column = 1;
    options.extras = Json{{"builtin", true}, {"kind", "mcp-management"}};

    WebviewPanelState state;
    bool adopted_by_provider = false;
    int32_t status = registry.create(
        std::string{kMcpManagementPanelId}, std::string{kMcpManagementViewType},
        std::string{kMcpManagementPanelTitle}, options,
        WebviewPanelOwner::native_runtime, state);
    if (status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT) {
        const auto existing = registry.snapshot(std::string{kMcpManagementPanelId});
        if (!existing.has_value() || existing->disposed || existing->owner != WebviewPanelOwner::native_runtime || existing->view_type != kMcpManagementViewType || existing->title != kMcpManagementPanelTitle) {
            return status;
        }
        state = *existing;
    } else if (status != SAO_AI_EDITOR_OK) {
        return status;
    } else {
        adopted_by_provider = true;
    }
    status = registry.set_html(std::string{kMcpManagementPanelId},
                               load_bundled_html(assets_root), state);
    if (status != SAO_AI_EDITOR_OK) {
        if (adopted_by_provider) {
            WebviewPanelState rollback;
            (void)registry.dispose(std::string{kMcpManagementPanelId}, rollback);
        }
        return status;
    }
    registered_ = true;
    return SAO_AI_EDITOR_OK;
}

int32_t McpManagementPanelProvider::unregister_from_runtime(
    WebviewPanelRegistry& registry) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!registered_) {
        return SAO_AI_EDITOR_OK;
    }
    WebviewPanelState state;
    const int32_t status = registry.dispose(std::string{kMcpManagementPanelId}, state);
    if (status != SAO_AI_EDITOR_OK && status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return status;
    }
    registered_ = false;
    return SAO_AI_EDITOR_OK;
}

int32_t McpManagementPanelProvider::handle_message(const Json& message,
                                                    Json& out_reply) {
    if (!message.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::string command = message.value("cmd", std::string{});
    if (command.empty()) {
        command = message.value("command", std::string{});
    }
    if (command.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json request_id = message.value("requestId", Json());
    SnapshotProvider snapshot;
    KernelMapNavigator navigator;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        snapshot = snapshot_provider_;
        navigator = kernel_map_navigator_;
    }

    out_reply = Json{{"cmd", command}};
    if (!request_id.is_null()) {
        out_reply["requestId"] = request_id;
    }
    if (command == "snapshot" || command == "refresh") {
        if (!snapshot) {
            out_reply["status"] = "error";
            out_reply["reason"] = "MCP registry is unavailable";
            return SAO_AI_EDITOR_OK;
        }
        out_reply["status"] = "ok";
        out_reply["payload"] = snapshot();
        return SAO_AI_EDITOR_OK;
    }
    if (command == "open_kernel_map") {
        const bool opened = navigator && navigator();
        out_reply["status"] = opened ? "ok" : "error";
        out_reply["payload"] = Json{{"panelId", kKernelMapPanelId},
                                    {"opened", opened}};
        if (!opened) {
            out_reply["reason"] = "Kernel Map panel is unavailable";
        }
        return SAO_AI_EDITOR_OK;
    }
    out_reply["status"] = "error";
    out_reply["reason"] = "unknown command";
    return SAO_AI_EDITOR_OK;
}

}
