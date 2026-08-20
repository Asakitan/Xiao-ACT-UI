#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/panel_sdk.h"

namespace sao::launcher::workshop_panel {

inline constexpr char kPanelId[] = "sao.launcher.plugin_workshop";
inline constexpr char kPanelTitle[] = "Plugin Workshop";
inline constexpr std::uint32_t kPageSize = 12;

struct PluginSummary {
    std::string id;
    std::string name;
    std::string version;
    std::string tag;
    std::string author;
    std::uint64_t updated_ms{};
    std::uint32_t rating{};
    std::uint32_t downloads{};
};

struct PluginDetail {
    PluginSummary summary;
    std::string description;
    std::string sha256_hex;
    std::string signature_algorithm;
    std::uint64_t size_bytes{};
    std::uint32_t min_client_version_major{};
    std::uint32_t min_client_version_minor{};
    std::uint32_t min_client_version_patch{};
};

struct PluginPage {
    std::vector<PluginSummary> items;
    std::uint32_t total{};
    std::uint32_t page{1};
    std::uint32_t size{kPageSize};
};

struct Operations {
    using List =
        std::function<sao_status_t(std::stop_token, std::uint32_t, std::uint32_t, PluginPage&)>;
    using Detail = std::function<sao_status_t(std::stop_token, std::string_view, PluginDetail&)>;
    using Download = std::function<sao_status_t(
        std::stop_token, std::string_view, const std::filesystem::path&, std::filesystem::path&)>;
    using Verify = std::function<sao_status_t(std::stop_token, const std::filesystem::path&,
                                              std::string_view)>;
    using Install = std::function<sao_status_t(std::stop_token, const std::filesystem::path&,
                                               const std::filesystem::path&)>;
    using Uninstall = std::function<sao_status_t(std::stop_token, std::string_view,
                                                 const std::filesystem::path&, bool)>;

    List list;
    Detail detail;
    Download download;
    Verify verify;
    Install install;
    Uninstall uninstall;

    [[nodiscard]] bool complete() const noexcept;
};

struct Snapshot {
    sao_ui_panel_handle_t panel{};
    bool online{};
    bool visible{};
    bool busy{};
    std::uint32_t page{1};
    std::uint32_t page_size{kPageSize};
    std::uint32_t total{};
    std::uint64_t queued_operations{};
    std::uint64_t completed_operations{};
    sao_status_t last_status{SAO_STATUS_OK};
    std::string status_text;
    std::string progress_text;
    std::string error_text;
    std::string last_spec;
    std::vector<PluginSummary> items;
    std::optional<PluginDetail> detail;
};

using VisibilityChangedCallback = std::function<void(bool visible)>;

// Wraps the synchronized freetier workshop client. The launcher target that
// adopts this file must link sao::server::freetier::workshop_client.
Operations make_production_operations();

[[nodiscard]] bool item_count_within_capacity_for_testing(
    std::uint32_t item_count, std::uint32_t item_cap,
    std::uint32_t requested_size) noexcept;

// Stable descriptor/theme helpers are exposed for launcher composition and
// focused contract tests. The mainline may use the visibility hook below to
// show or hide its shared fisheye backdrop; this component never creates one.
const char* theme_override_json() noexcept;
SaoPanelDescriptor panel_descriptor() noexcept;

class Owner final {
  public:
    Owner(sao_ui_compositor_handle_t compositor, std::filesystem::path base_dir);
    Owner(sao_ui_compositor_handle_t compositor, std::filesystem::path base_dir,
          Operations operations);
    ~Owner();

    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    Owner(Owner&&) = delete;
    Owner& operator=(Owner&&) = delete;

    // Owner-thread operations. open() creates at most one native panel, reuses
    // it on later calls, and queues one list request when transitioning visible.
    sao_status_t open();
    sao_status_t hide();
    sao_status_t service_ui();
    sao_status_t tick();

    // Owner-thread teardown. The owner-thread preflight runs before acceptance
    // or worker state changes. UI retirement failures preserve the panel handle
    // and callback bindings for a later retry; successful retirement is
    // idempotent.
    sao_status_t try_take_offline();

    // Replacing the callback publishes the current known visibility once, then
    // reports later transitions exactly once. Before the panel has established
    // a visibility state, registration is silent.
    void set_visibility_changed_callback(VisibilityChangedCallback callback);

    // Internal action seam used by focused tests and launcher adapters. It has
    // the same bounded JSON validation and enqueue-only behavior as panel clicks.
    sao_status_t dispatch_action_for_testing(std::string_view action,
                                             std::string_view payload_json = {});

    // Focused lifecycle seams. Panel close events remain enqueue-only and are
    // serviced by the compositor owner thread.
    sao_status_t dispatch_panel_event_for_testing(std::int32_t event_kind);
    void fail_next_unregister_for_testing(sao_status_t status);
    static void drain_deferred_cleanup_for_owner() noexcept;
    static void drain_deferred_cleanup_for_testing() noexcept;

    [[nodiscard]] Snapshot snapshot() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sao::launcher::workshop_panel
