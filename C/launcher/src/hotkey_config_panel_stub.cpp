#include "hotkey_config_panel.h"

namespace sao::launcher::hotkey {

Owner::Owner(sao_ui_compositor_handle_t compositor) noexcept {
    (void)compositor;
}

Owner::~Owner() = default;

sao_status_t Owner::open() noexcept {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t Owner::close() noexcept {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t Owner::take_offline() noexcept {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t Owner::set_owner_wake_window(void* window) noexcept {
    (void)window;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t Owner::drain_capture_for_owner() noexcept {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t Owner::set_capture_hooks_for_testing(CaptureHooks hooks) noexcept {
    (void)hooks;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t Owner::dispatch_action_for_testing(std::string_view action,
                                                 std::string_view payload_json) noexcept {
    (void)action;
    (void)payload_json;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t Owner::dispatch_event_for_testing(std::int32_t event_kind) noexcept {
    (void)event_kind;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

void Owner::fail_next_unregister_for_testing(sao_status_t status) noexcept {
    (void)status;
}

void Owner::fail_next_handler_restore_for_testing(sao_status_t action_status,
                                                  sao_status_t event_status) noexcept {
    (void)action_status;
    (void)event_status;
}

void Owner::drain_deferred_cleanup_for_owner() noexcept {}

void Owner::drain_deferred_cleanup_for_testing() noexcept {
    drain_deferred_cleanup_for_owner();
}

sao_ui_panel_handle_t Owner::panel_handle() const noexcept {
    return nullptr;
}

bool Owner::is_capturing() const noexcept {
    return false;
}

SaoPanelDescriptor panel_descriptor_for_testing() noexcept {
    return {};
}

std::string build_panel_spec_for_testing() {
    return {};
}

void open_config_panel() {}

void close_config_panel() {}

void open_config_panel(Owner* owner) {
    (void)owner;
}

void close_config_panel(Owner* owner) {
    (void)owner;
}

std::string format_combo_utf8(std::uint32_t vk, std::uint32_t modifiers) {
    (void)vk;
    (void)modifiers;
    return {};
}

} // namespace sao::launcher::hotkey