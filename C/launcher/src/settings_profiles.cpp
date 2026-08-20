#include "settings_profiles.h"
#include "settings_owner_internal.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>
#if defined(_WIN32)
#include <shlobj.h>
#include <windows.h>
#endif
namespace fs = std::filesystem;
namespace sao::launcher::settings {
namespace {
std::mutex g_owner_mutex;
std::mutex g_profiles_mutex;
std::mutex g_profile_io_mutex;
std::recursive_mutex g_settings_action_mutex;
settings_owner::SettingsOwner* g_owner = nullptr;
fs::path g_profiles_override;
constexpr std::size_t kMaximumProfileNameBytes = 64U;
constexpr std::uintmax_t kMaximumProfileBytes = 16U * 1024U * 1024U;
bool valid_profile_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaximumProfileNameBytes || name == "." || name == "..") return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char value) { return std::isalnum(value) != 0 || value == '_' || value == '-' || value == '.'; });
}
settings_owner::SettingsOwner::Lease owner_snapshot() noexcept {
    std::lock_guard lock(g_owner_mutex);
    return g_owner == nullptr ? settings_owner::SettingsOwner::Lease{} : g_owner->acquire_lease();
}
fs::path profiles_dir() {
    std::lock_guard lock(g_profiles_mutex);
    if (!g_profiles_override.empty()) return g_profiles_override;
#if defined(_WIN32)
    wchar_t* appdata = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata) == S_OK && appdata != nullptr) {
        fs::path path(appdata); CoTaskMemFree(appdata); return path / "SAOAuto" / "profiles";
    }
#endif
    const char* env = std::getenv("APPDATA");
    return fs::path(env == nullptr ? "." : env) / "SAOAuto" / "profiles";
}
fs::path profile_file(const std::string& name) { return profiles_dir() / (name + ".json"); }
bool replace_file(const fs::path& temporary, const fs::path& target) noexcept {
#if defined(_WIN32)
    return ::MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    std::error_code error; fs::remove(target, error); error.clear(); fs::rename(temporary, target, error); return !error;
#endif
}
bool write_profile_file(const fs::path& target, std::string_view bytes) noexcept {
    try {
        std::error_code error; fs::create_directories(target.parent_path(), error); if (error) return false;
        fs::path temporary = target; temporary += ".tmp";
        { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); if (!out) return false; out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); out.flush(); if (!out) return false; }
        if (replace_file(temporary, target)) return true; fs::remove(temporary, error); return false;
    } catch (...) { return false; }
}
}
extern "C" sao_status_t sao_launcher_settings_profiles_set_owner(void* owner_opaque) noexcept {
    try {
        settings_owner::SettingsOwner* previous = nullptr;
        { std::lock_guard lock(g_owner_mutex); previous = g_owner; if (previous == owner_opaque) return SAO_STATUS_OK; g_owner = nullptr; }
        if (previous != nullptr) previous->retire_and_wait();
        auto* next = reinterpret_cast<settings_owner::SettingsOwner*>(owner_opaque);
        if (next != nullptr) next->resume_after_retire();
        { std::lock_guard lock(g_owner_mutex); g_owner = next; }
        return SAO_STATUS_OK;
    } catch (...) { return SAO_STATUS_ERR_UNKNOWN; }
}
sao_status_t settings_profiles_set_owner(void* owner_opaque) noexcept { return sao_launcher_settings_profiles_set_owner(owner_opaque); }
sao_status_t settings_profiles_bind_owner(void* owner_opaque) noexcept { return sao_launcher_settings_profiles_set_owner(owner_opaque); }
sao_status_t settings_profiles_unbind_owner(void* owner_opaque) noexcept {
    {
        std::lock_guard lock(g_owner_mutex);
        if (owner_opaque != nullptr && g_owner != owner_opaque)
            return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    return sao_launcher_settings_profiles_set_owner(nullptr);
}
std::recursive_mutex& settings_action_mutex() noexcept { return g_settings_action_mutex; }
void set_profiles_directory_for_testing(std::wstring path) noexcept { try { std::lock_guard lock(g_profiles_mutex); g_profiles_override = fs::path(std::move(path)); } catch (...) {} }
std::vector<std::string> list_profiles() {
    try {
        std::lock_guard action_lock(g_settings_action_mutex); std::lock_guard io_lock(g_profile_io_mutex); std::vector<std::string> out;
        const fs::path directory = profiles_dir(); std::error_code error;
        if (!fs::exists(directory, error) || error) return out;
        if (!fs::is_directory(directory, error) || error) return out;
        fs::directory_iterator it(directory, error), end;
        while (it != end && !error) { std::error_code entry_error; if (it->is_regular_file(entry_error) && !entry_error) { const auto path = it->path(); const std::string name = path.stem().string(); if (path.extension() == ".json" && valid_profile_name(name)) out.push_back(name); } it.increment(error); }
        std::sort(out.begin(), out.end()); return out;
    } catch (...) { return {}; }
}
bool save_profile(const std::string& name) {
    try { std::lock_guard action_lock(g_settings_action_mutex); std::lock_guard io_lock(g_profile_io_mutex); if (!valid_profile_name(name)) return false; auto owner = owner_snapshot(); if (!owner) return false; nlohmann::ordered_json snapshot; if (owner->snapshot(snapshot) != SAO_STATUS_OK) return false; return write_profile_file(profile_file(name), snapshot.dump(2)); } catch (...) { return false; }
}
bool load_profile(const std::string& name) {
    try {
        std::lock_guard action_lock(g_settings_action_mutex); std::lock_guard io_lock(g_profile_io_mutex); if (!valid_profile_name(name)) return false;
        const fs::path file = profile_file(name); std::error_code error; const auto size = fs::file_size(file, error); if (error || size > kMaximumProfileBytes) return false;
        auto owner = owner_snapshot(); if (!owner) return false; std::ifstream in(file, std::ios::binary); if (!in) return false; nlohmann::ordered_json profile; in >> profile; if (!profile.is_object()) return false;
        nlohmann::ordered_json before; if (owner->snapshot(before) != SAO_STATUS_OK) return false; const bool was_dirty = owner->dirty();
        for (auto it = profile.begin(); it != profile.end(); ++it) if (owner->set_value(it.key(), it.value()) != SAO_STATUS_OK) { (void)owner->restore_snapshot(std::move(before), was_dirty); return false; }
        if (owner->save() == SAO_STATUS_OK) return true; (void)owner->restore_snapshot(std::move(before), was_dirty); return false;
    } catch (...) { return false; }
}
bool delete_profile(const std::string& name) {
    try { std::lock_guard action_lock(g_settings_action_mutex); std::lock_guard io_lock(g_profile_io_mutex); if (!valid_profile_name(name)) return false; std::error_code error; return fs::remove(profile_file(name), error) && !error; } catch (...) { return false; }
}
sao_status_t profile_path(const std::string& name, std::wstring& out) noexcept {
    if (!valid_profile_name(name)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try { out = profile_file(name).wstring(); return SAO_STATUS_OK; } catch (const std::bad_alloc&) { return SAO_STATUS_ERR_UNKNOWN; } catch (...) { return SAO_STATUS_ERR_OS_CALL_FAILED; }
}
} // namespace sao::launcher::settings
