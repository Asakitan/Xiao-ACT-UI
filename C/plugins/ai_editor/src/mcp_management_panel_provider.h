#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "webview_panel_registry.h"

namespace sao::ai_editor::native {

inline constexpr const char kMcpManagementPanelId[] = "mcp-management-builtin";
inline constexpr const char kMcpManagementViewType[] = "sao.mcp_management";
inline constexpr const char kMcpManagementPanelTitle[] = "MCP Management";

class McpManagementPanelProvider final : public NativePanelProvider {
public:
    using SnapshotProvider = std::function<Json()>;
    using KernelMapNavigator = std::function<bool()>;

    McpManagementPanelProvider() = default;
    ~McpManagementPanelProvider() override = default;

    McpManagementPanelProvider(const McpManagementPanelProvider&) = delete;
    McpManagementPanelProvider& operator=(const McpManagementPanelProvider&) = delete;

    int32_t register_with_runtime(WebviewPanelRegistry& registry,
                                  const std::string& assets_root) override;
    int32_t unregister_from_runtime(WebviewPanelRegistry& registry) override;
    int32_t handle_message(const Json& message, Json& out_reply) override;
    std::string_view provider_panel_id() const noexcept override {
        return panel_id();
    }

    void install_snapshot_provider(SnapshotProvider provider);
    void install_kernel_map_navigator(KernelMapNavigator navigator);
    bool is_registered() const noexcept;

    static std::string_view panel_id() noexcept { return kMcpManagementPanelId; }
    static std::string_view view_type() noexcept { return kMcpManagementViewType; }

private:
    std::string load_bundled_html(const std::string& assets_root) const;

    mutable std::mutex mutex_;
    SnapshotProvider snapshot_provider_;
    KernelMapNavigator kernel_map_navigator_;
    bool registered_ = false;
};

}
