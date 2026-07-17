#include "settings_theme_internal.h"

#include <array>
#include <new>
#include <string_view>
#include <utility>

namespace sao::launcher::settings_theme {
namespace {

using Json = settings_owner::Json;

constexpr std::array<std::string_view, 7> kPanelKeys = {
    "dps", "hp", "bosshp", "skillfx", "alert", "act", "buffmon",
};

bool is_ascii_space(unsigned char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\f' || value == '\v';
}

char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A'))
        : value;
}

bool is_light(std::string_view value) noexcept {
    while (!value.empty() &&
           is_ascii_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           is_ascii_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    constexpr std::string_view kLight = "light";
    if (value.size() != kLight.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (ascii_lower(value[index]) != kLight[index]) {
            return false;
        }
    }
    return true;
}

bool is_act_key(std::string_view value) noexcept {
    while (!value.empty() &&
           is_ascii_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           is_ascii_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    constexpr std::string_view kAct = "act";
    if (value.size() != kAct.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (ascii_lower(value[index]) != kAct[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

sao_status_t read_process_theme(const settings_owner::SettingsOwner& owner,
                                PanelTheme& out) noexcept {
    out = PanelTheme::dark;
    try {
        Json panel_themes;
        const sao_status_t status = owner.get_value("panel_themes", panel_themes);
        if (status == SAO_STATUS_ERR_NOT_FOUND) {
            return SAO_STATUS_OK;
        }
        if (status != SAO_STATUS_OK) {
            return status;
        }
        if (!panel_themes.is_object()) {
            return SAO_STATUS_OK;
        }
        for (auto entry = panel_themes.begin(); entry != panel_themes.end(); ++entry) {
            if (!is_act_key(entry.key())) {
                continue;
            }
            out = entry->is_string() && is_light(entry->get_ref<const std::string&>())
                ? PanelTheme::light
                : PanelTheme::dark;
        }
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t replace_all_panel_themes(settings_owner::SettingsOwner& owner,
                                      PanelTheme theme) noexcept {
    try {
        Json panel_themes = Json::object();
        const char* value = theme == PanelTheme::light ? "light" : "dark";
        for (const auto key : kPanelKeys) {
            panel_themes.emplace(std::string(key), value);
        }
        return owner.set_value("panel_themes", std::move(panel_themes));
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (const nlohmann::json::exception&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

} // namespace sao::launcher::settings_theme
