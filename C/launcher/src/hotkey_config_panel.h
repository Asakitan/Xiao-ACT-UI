#pragma once

#include "sao/core/status.h"
#include "sao/ui/panel_sdk.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace sao::launcher::hotkey {

inline constexpr char kPanelId[] = "sao.launcher.hotkey_config";
inline constexpr char kPanelTitle[] = "Hotkey Configuration";
inline constexpr char kCaptureAction[] = "hotkey.capture";
inline constexpr char kResetAction[] = "hotkey.reset";
inline constexpr std::uint32_t kCaptureCompletionMessage = 0x8000u + 0x4A31u;

enum class PanelStatus {
    ready,
    capturing,
    success,
    cancelled,
    conflict,
    system_error,
    save_error,
};

struct CaptureHooks {
    std::function<short(int)> get_async_key_state;
    std::function<bool()> post_owner_wake;
};

SaoPanelDescriptor panel_descriptor_for_testing() noexcept;
std::string build_panel_spec_for_testing();

class Owner final {
  public:
    explicit Owner(sao_ui_compositor_handle_t compositor) noexcept;
    ~Owner();

    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    Owner(Owner&&) = delete;
    Owner& operator=(Owner&&) = delete;

    sao_status_t open() noexcept;
    sao_status_t close() noexcept;
    sao_status_t take_offline() noexcept;
    sao_status_t set_owner_wake_window(void* window) noexcept;
    sao_status_t drain_capture_for_owner() noexcept;
    sao_status_t set_capture_hooks_for_testing(CaptureHooks hooks) noexcept;
    sao_status_t dispatch_action_for_testing(std::string_view action,
                                             std::string_view payload_json = {}) noexcept;
    sao_status_t dispatch_event_for_testing(std::int32_t event_kind) noexcept;
    [[nodiscard]] sao_ui_panel_handle_t panel_handle() const noexcept;
    [[nodiscard]] bool is_capturing() const noexcept;
    void fail_next_unregister_for_testing(sao_status_t status) noexcept;
    void fail_next_handler_restore_for_testing(sao_status_t action_status,
                                               sao_status_t event_status) noexcept;
    static void drain_deferred_cleanup_for_owner() noexcept;
    static void drain_deferred_cleanup_for_testing() noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

void open_config_panel();
void close_config_panel();
void open_config_panel(Owner* owner);
void close_config_panel(Owner* owner);
std::string format_combo_utf8(std::uint32_t vk, std::uint32_t modifiers);

} // namespace sao::launcher::hotkey