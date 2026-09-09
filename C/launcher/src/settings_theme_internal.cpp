#include "settings_theme_internal.h"

#include <array>
#include <new>
#include <string>
#include <utility>

namespace sao::launcher::settings_theme {
namespace {

using Json = settings_owner::Json;

constexpr std::array<std::string_view, 7> kPanelKeys = {
	"dps", "hp", "bosshp", "skillfx", "alert", "act", "buffmon",
};

std::string_view trim_ascii(std::string_view value) noexcept {
	const auto is_space = [](unsigned char ch) {
		return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' ||
			   ch == '\v';
	};
	while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
		value.remove_prefix(1);
	while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
		value.remove_suffix(1);
	return value;
}

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
	left = trim_ascii(left);
	if (left.size() != right.size())
		return false;
	for (std::size_t index = 0; index < left.size(); ++index) {
		const char ch = left[index];
		const char lowered = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
		if (lowered != right[index])
			return false;
	}
	return true;
}

} // namespace

sao_status_t parse_panel_theme(std::string_view value, PanelTheme& out) noexcept {
	if (ascii_iequals(value, "dark")) {
		out = PanelTheme::dark;
		return SAO_STATUS_OK;
	}
	if (ascii_iequals(value, "light")) {
		out = PanelTheme::light;
		return SAO_STATUS_OK;
	}
	return SAO_STATUS_ERR_INVALID_ARGUMENT;
}

sao_status_t read_process_theme(const settings_owner::SettingsOwner& owner,
								PanelTheme& out) noexcept {
    out = PanelTheme::light;
    try {
        Json panel_themes;
        const sao_status_t status = owner.get_value("panel_themes", panel_themes);
        if (status == SAO_STATUS_ERR_NOT_FOUND)
            return SAO_STATUS_OK;
        if (status != SAO_STATUS_OK)
            return status;
        if (!panel_themes.is_object())
            return SAO_STATUS_OK;
        const auto active = panel_themes.find("act");
        if (active == panel_themes.end() || !active->is_string())
            return SAO_STATUS_OK;
        PanelTheme parsed = PanelTheme::dark;
        if (parse_panel_theme(active->get_ref<const std::string&>(), parsed) == SAO_STATUS_OK)
            out = parsed;
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
		Json panel_themes;
		const sao_status_t read_status = owner.get_value("panel_themes", panel_themes);
		if (read_status == SAO_STATUS_ERR_NOT_FOUND || !panel_themes.is_object())
			panel_themes = Json::object();
		else if (read_status != SAO_STATUS_OK)
			return read_status;

		const char* value = theme == PanelTheme::light ? "light" : "dark";
		for (const std::string_view key : kPanelKeys)
			panel_themes[std::string(key)] = value;
		return owner.set_value_and_save("panel_themes", std::move(panel_themes));
	} catch (const std::bad_alloc&) {
		return SAO_STATUS_ERR_UNKNOWN;
	} catch (const nlohmann::json::exception&) {
		return SAO_STATUS_ERR_INVALID_ARGUMENT;
	} catch (...) {
		return SAO_STATUS_ERR_UNKNOWN;
	}
}

} // namespace sao::launcher::settings_theme
