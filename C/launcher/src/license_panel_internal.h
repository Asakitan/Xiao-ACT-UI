// license_panel_internal.h — native activation panel.
//
// Replaces the removed Python webview license_panel.html. Lets a user enter
// an activation key, display the HWID, copy it, and run
// sao_license_client_activate without a restart.

#pragma once

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#include <cstddef>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct sao_ui_compositor_s;
typedef struct sao_ui_compositor_s* sao_ui_compositor_handle_t;

namespace sao::launcher::license_panel {

inline constexpr char kPanelId[] = "sao.launcher.license_activation";
inline constexpr char kPanelTitle[] = "License Activation";
inline constexpr char kActivateAction[] = "license.activate";
inline constexpr char kCopyHwidAction[] = "license.copy_hwid";
inline constexpr char kRefreshAction[] = "license.refresh";
inline constexpr char kKeyInputAction[] = "license.key_input";
inline constexpr char kSkipAction[] = "license.skip";

// Injectable operations so focused tests can avoid touching the real license
// client / clipboard / refresh path.
struct Operations {
    // Blocking activation call. Returns SAO_OK on success; the status string
    // surface in the UI maps non-OK codes to human-readable text.
    using Activate = std::function<sao_status_t(std::string_view key)>;
    using CancelActivation = std::function<void()>;
    // Reads the 32-byte HWID into out_hex (must hold >=65 chars).
    using GetHwid = std::function<sao_status_t(std::string& out_hex)>;
    // Reads tier/expiry after a successful activation.
    using GetStatus = std::function<sao_status_t(std::string& out_tier, std::uint64_t& out_expiry_ms)>;
    // Copies text to the clipboard. Returns SAO_STATUS_OK on success.
    using CopyToClipboard = std::function<sao_status_t(std::string_view text)>;
    // Triggers the license refresh path so feature flags update live.
    using RefreshLicense = std::function<sao_status_t()>;

    Activate activate;
    CancelActivation cancel_activation;
    GetHwid get_hwid;
    GetStatus get_status;
    CopyToClipboard copy_to_clipboard;
    RefreshLicense refresh_license;

    [[nodiscard]] bool complete() const noexcept;
};

Operations make_default_operations();

struct Snapshot {
    bool panel_created{};
    bool visible{};
    bool busy{};
    sao_status_t last_status{SAO_STATUS_OK};
    std::string status_text;
    std::string error_text;
    std::string hwid_hex;
    std::string license_key;
    std::string tier;
    std::uint64_t expiry_ms{};
    bool activated{};
    std::string rendered_spec_json;
};

class Owner final {
  public:
    explicit Owner(sao_ui_compositor_handle_t compositor);
    Owner(sao_ui_compositor_handle_t compositor, Operations operations);
    ~Owner() noexcept;

    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    Owner(Owner&&) = delete;
    Owner& operator=(Owner&&) = delete;

    sao_status_t open() noexcept;
    sao_status_t close() noexcept;
    sao_status_t service_ui() noexcept;
    // Owner-thread, retryable panel retirement. Any failure preserves the
    // registered panel and callback ownership for a later retry.
    sao_status_t take_offline() noexcept;
    void fail_next_unregister_for_testing(sao_status_t status) noexcept;
    void fail_next_handler_restore_for_testing(sao_status_t action_status, sao_status_t event_status) noexcept;
    static void drain_deferred_cleanup_for_owner() noexcept;
    static void drain_deferred_cleanup_for_testing() noexcept;
    sao_status_t dispatch_event_for_testing(std::int32_t event_kind) noexcept;
    sao_status_t dispatch_action(std::string_view action_id,
                                 std::string_view payload_json = {}) noexcept;
    sao_status_t snapshot(Snapshot& out) const noexcept;

  private:
    struct State;
    struct AdoptStateTag {};
    explicit Owner(std::unique_ptr<State> state, AdoptStateTag) noexcept;
    struct OperationGuard;

    sao_status_t require_owner_thread() const noexcept;
    sao_status_t begin_operation() noexcept;
    void end_operation() noexcept;
    bool begin_callback() noexcept;
    void end_callback() noexcept;
    void handle_panel_event(std::int32_t event_kind) noexcept;
    sao_status_t ensure_panel() noexcept;
    sao_status_t publish() noexcept;
    sao_status_t run_activation(std::string key) noexcept;

    static void SAO_UI_CALL panel_action_callback(const char* action_id_utf8,
                                                  const std::uint8_t* payload_json_utf8,
                                                  std::size_t payload_len,
                                                  void* user_data) noexcept;
    static void SAO_UI_CALL panel_event_callback(std::int32_t event_kind,
                                                 void* user_data) noexcept;
    static void activation_thread_main(State* state, std::string key) noexcept;

    static void defer_state(std::unique_ptr<State> state) noexcept;
    static void drain_deferred_cleanup() noexcept;
    static std::mutex deferred_mutex_;
    static std::vector<std::unique_ptr<State>>* deferred_cleanup_;

    std::unique_ptr<State> state_;
};

} // namespace sao::launcher::license_panel