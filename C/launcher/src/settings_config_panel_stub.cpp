#include "settings_config_panel.h"

namespace sao::launcher::settings {

void open_config_panel() {}

sao_status_t rebind_owner_for_testing(void* owner_opaque) noexcept {
    (void)owner_opaque;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t settings_panel_set_owner(void* owner_opaque) noexcept {
    (void)owner_opaque;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t settings_panel_bind_owner(void* owner_opaque) noexcept {
    (void)owner_opaque;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t settings_panel_unbind_owner(void* owner_opaque) noexcept {
    (void)owner_opaque;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t sao_launcher_settings_panel_set_owner(void* owner_opaque) noexcept {
    (void)owner_opaque;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t set_compositor_for_testing(sao_ui_compositor_handle_t compositor) noexcept {
    (void)compositor;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t dispatch_action_for_testing(std::string_view action_id,
                                         std::string_view payload_json) noexcept {
    (void)action_id;
    (void)payload_json;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t close_for_testing() noexcept {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t take_offline_for_testing() noexcept {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

void drain_deferred_cleanup_for_owner() noexcept {}

void drain_deferred_cleanup_for_testing() noexcept {
    drain_deferred_cleanup_for_owner();
}

void fail_next_unregister_for_testing(sao_status_t status) noexcept {
    (void)status;
}

void fail_next_handler_restore_for_testing(sao_status_t action_status,
                                           sao_status_t event_status) noexcept {
    (void)action_status;
    (void)event_status;
}

sao_status_t snapshot_for_testing(std::string& out_json) noexcept {
    out_json.clear();
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

std::string build_spec_for_testing(std::string_view snapshot_json_utf8) {
    (void)snapshot_json_utf8;
    return {};
}

} // namespace sao::launcher::settings