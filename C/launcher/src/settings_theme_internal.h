#pragma once

#include "settings_owner_internal.h"

#include <cstdint>
#include <string_view>

namespace sao::launcher::settings_theme {

enum class PanelTheme : std::uint8_t {
    dark,
    light,
};

sao_status_t parse_panel_theme(std::string_view value, PanelTheme& out) noexcept;

sao_status_t read_process_theme(const settings_owner::SettingsOwner& owner,
                                PanelTheme& out) noexcept;

sao_status_t replace_all_panel_themes(settings_owner::SettingsOwner& owner,
                                      PanelTheme theme) noexcept;

} // namespace sao::launcher::settings_theme