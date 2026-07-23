#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/compositor.h"
#include "sao/ui/panel_sdk.h"

namespace sao::launcher::plugin_manager_panel {

inline constexpr std::string_view kPanelId = "sao.launcher.plugin_manager";

enum class PluginState {
    unknown,
    discovered,
    validating,
    resolving_dependencies,
    bootstrapping,
    loading,
    loaded_active,
    loaded_disabled,
    unloading,
    unloaded,
    failed,
    enabling,
    disabling,
};

enum class PluginSource {
    unknown,
    built_in,
    user,
};

struct PluginSnapshot {
    std::string plugin_id;
    std::string name;
    std::string version;
    std::string language;
    std::string description;
    std::string source_path;
    PluginState state{PluginState::unknown};
    PluginSource source{PluginSource::unknown};
    bool manifest_enabled{};
};

struct Snapshot {
    bool loader_available{};
    bool reload_all_available{};
    std::vector<PluginSnapshot> plugins;
    std::string error_message;
};

struct Operations {
    std::function<Snapshot()> snapshot;
    std::function<sao_status_t(std::string_view)> enable;
    std::function<sao_status_t(std::string_view)> disable;
    std::function<sao_status_t(std::string_view)> reload;
    std::function<sao_status_t()> reload_all;
};

using ReloadAllHandler = std::function<sao_status_t()>;

[[nodiscard]] std::string_view plugin_state_label(PluginState state) noexcept;
[[nodiscard]] bool plugin_state_is_transitioning(PluginState state) noexcept;
[[nodiscard]] bool plugin_state_is_enabled(PluginState state) noexcept;
[[nodiscard]] bool plugin_state_allows_enable(PluginState state) noexcept;
[[nodiscard]] bool plugin_state_allows_reload(PluginState state) noexcept;

// Pure internal seams used by focused tests and by the production owner.
[[nodiscard]] SaoPanelDescriptor descriptor_for_testing() noexcept;
[[nodiscard]] std::string build_spec_for_testing(const Snapshot& snapshot);

struct Owner final {
    explicit Owner(sao_ui_compositor_handle_t borrowed_compositor) noexcept;
    Owner(sao_ui_compositor_handle_t borrowed_compositor,
          ReloadAllHandler reload_all_handler) noexcept;
    Owner(sao_ui_compositor_handle_t borrowed_compositor, Operations operations) noexcept;
    ~Owner();

    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    Owner(Owner&& other) noexcept;
    Owner& operator=(Owner&& other) noexcept;

    [[nodiscard]] sao_status_t set_operations(Operations operations) noexcept;

    // Registers once, refreshes, shows, and raises the existing instance on
    // repeated calls. The compositor is borrowed and must outlive this owner.
    [[nodiscard]] sao_status_t open() noexcept;
    [[nodiscard]] sao_status_t close() noexcept;
    [[nodiscard]] sao_status_t refresh() noexcept;

    // Detaches callbacks and unregisters the compositor panel. BUSY keeps the
    // instance owned and retryable when a callback is in flight.
    [[nodiscard]] sao_status_t take_offline() noexcept;

    [[nodiscard]] bool is_registered() const noexcept;
    [[nodiscard]] bool is_visible() const noexcept;
    [[nodiscard]] sao_ui_panel_handle_t panel_handle() const noexcept;

    // Routes the same action path used by the compositor callback. A plugin
    // action payload is a JSON string whose value is exactly the plugin id.
    [[nodiscard]] sao_status_t
    dispatch_action_for_testing(std::string_view action_id,
                                std::string_view payload_json = {}) noexcept;
    [[nodiscard]] sao_status_t dispatch_event_for_testing(std::int32_t event_kind) noexcept;

  private: // Pimpl state.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sao::launcher::plugin_manager_panel
