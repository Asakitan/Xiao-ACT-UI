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
extern "C" sao_status_t sao_launcher_settings_panel_set_owner(void* owner_opaque) noexcept;
sao_status_t set_compositor_for_testing(sao_ui_compositor_handle_t compositor) noexcept;
sao_status_t dispatch_action_for_testing(std::string_view action_id, std::string_view payload_json = {}) noexcept;
sao_status_t close_for_testing() noexcept;
sao_status_t take_offline_for_testing() noexcept;
sao_status_t snapshot_for_testing(std::string& out_json) noexcept;
std::string build_spec_for_testing(std::string_view snapshot_json_utf8);
} // namespace sao::launcher::settings