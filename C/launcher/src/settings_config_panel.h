#pragma once
#include "sao/core/status.h"
#include <string>
#include <string_view>
struct sao_ui_compositor_s;
typedef struct sao_ui_compositor_s* sao_ui_compositor_handle_t;
namespace sao::launcher::settings {
inline constexpr char kSettingsPanelId[] = "sao.launcher.settings";
inline constexpr char kSettingsPanelTitle[] = "SAO Auto — Settings";
inline constexpr char kSettingsActionToggle[] = "settings.toggle";
inline constexpr char kSettingsActionNumericAdjust[] = "settings.numeric.adjust";
inline constexpr char kSettingsActionTheme[] = "settings.theme";
inline constexpr char kSettingsActionProfileQuickBackup[] = "settings.profile.quick_backup";
inline constexpr char kSettingsActionProfileLoad[] = "settings.profile.load";
inline constexpr char kSettingsActionProfileDelete[] = "settings.profile.delete";
inline constexpr char kSettingsActionRefresh[] = "settings.refresh";
inline constexpr char kSettingsActionClose[] = "settings.close";
inline constexpr std::string_view kQuickBackupProfileName = "quick-backup";
void open_config_panel();
sao_status_t rebind_owner_for_testing(void* owner_opaque) noexcept;
sao_status_t settings_panel_set_owner(void* owner_opaque) noexcept;
sao_status_t settings_panel_bind_owner(void* owner_opaque) noexcept;
sao_status_t settings_panel_unbind_owner(void* owner_opaque) noexcept;
extern "C" sao_status_t sao_launcher_settings_panel_set_owner(void* owner_opaque) noexcept;
// Thread-safe entry for owner-side change notifications (init_pipeline dirty
// notifications, owner subscriber). Repaints only on the owner/compositor
// thread; foreign calls latch a pending flag for the next owner-thread pass.
// Matches init_pipeline's forward declaration (no noexcept).
extern "C" sao_status_t sao_launcher_settings_config_external_refresh(void);

// Boot-time VT hypervisor switch state, surfaced on the Advanced section.
// The owning process binds a query fn during platform bring-up; the panel
// never touches launcher internals directly (the ui-off stub build keeps a
// no-op so the link survives).
struct VtHypervisorState {
    int32_t enabled;          // persisted switch read at this boot
    int32_t state;            // kVtHypervisorState* result of the driver stage
    sao_status_t last_status; // non-zero failure detail when state == failed
};
inline constexpr int32_t kVtHypervisorStateOff = 0;
inline constexpr int32_t kVtHypervisorStateActive = 1;
inline constexpr int32_t kVtHypervisorStateFailed = 2;
using VtStateQueryFn = sao_status_t (*)(void* user, VtHypervisorState* out) noexcept;
sao_status_t settings_panel_bind_vt_state_query(VtStateQueryFn fn, void* user) noexcept;
sao_status_t set_compositor_for_testing(sao_ui_compositor_handle_t compositor) noexcept;
sao_status_t dispatch_action_for_testing(std::string_view action_id,
                                         std::string_view payload_json = {}) noexcept;
sao_status_t close_for_testing() noexcept;
sao_status_t take_offline_for_testing() noexcept;
void drain_deferred_cleanup_for_owner() noexcept;
void drain_deferred_cleanup_for_testing() noexcept;
void fail_next_unregister_for_testing(sao_status_t status) noexcept;
void fail_next_handler_restore_for_testing(sao_status_t action_status,
                                           sao_status_t event_status) noexcept;
sao_status_t snapshot_for_testing(std::string& out_json) noexcept;
std::string build_spec_for_testing(std::string_view snapshot_json_utf8);
} // namespace sao::launcher::settings