// settings_profiles.h — named settings snapshots (Phase 13).
//
// Users can save the current settings as "Home Setup" / "Streaming Setup" etc.
// Profile files sit under %APPDATA%/SAOAuto/profiles/<name>.json.

#pragma once

#include <string>
#include <vector>

namespace sao::launcher::settings {

std::vector<std::string> list_profiles();
bool save_profile(const std::string& name);
bool load_profile(const std::string& name);
bool delete_profile(const std::string& name);

} // namespace sao::launcher::settings
