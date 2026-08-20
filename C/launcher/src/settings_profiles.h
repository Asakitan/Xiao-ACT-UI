#pragma once
#include "sao/core/status.h"
#include <mutex>
#include <string>
#include <vector>
namespace sao::launcher::settings {
inline constexpr char kQuickBackupProfile[] = "quick-backup";
std::vector<std::string> list_profiles();
bool save_profile(const std::string& name);
bool load_profile(const std::string& name);
bool delete_profile(const std::string& name);
sao_status_t profile_path(const std::string& name, std::wstring& out) noexcept;
sao_status_t settings_profiles_set_owner(void* owner_opaque) noexcept;
std::recursive_mutex& settings_action_mutex() noexcept;
void set_profiles_directory_for_testing(std::wstring path) noexcept;
} // namespace sao::launcher::settings
extern "C" sao_status_t sao_launcher_settings_profiles_set_owner(void* owner_opaque) noexcept;