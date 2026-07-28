// settings_profiles.cpp — Phase 13 named settings snapshots (production).

#include "settings_profiles.h"

#include "settings_owner_internal.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <shlobj.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace sao::launcher::settings {

namespace {
std::atomic<sao::launcher::settings_owner::SettingsOwner*> g_owner{nullptr};

fs::path profiles_dir() {
#if defined(_WIN32)
    wchar_t* appdata = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata) == S_OK &&
        appdata != nullptr) {
        fs::path p(appdata);
        CoTaskMemFree(appdata);
        return p / "SAOAuto" / "profiles";
    }
#endif
    const char* env = std::getenv("APPDATA");
    if (env == nullptr) env = ".";
    return fs::path(env) / "SAOAuto" / "profiles";
}
} // namespace

extern "C" void sao_launcher_settings_profiles_set_owner(void* owner_opaque) {
    g_owner.store(reinterpret_cast<sao::launcher::settings_owner::SettingsOwner*>(
        owner_opaque));
}

std::vector<std::string> list_profiles() {
    std::vector<std::string> out;
    std::error_code ec;
    fs::create_directories(profiles_dir(), ec);
    for (const auto& entry : fs::directory_iterator(profiles_dir(), ec)) {
        if (!entry.is_regular_file()) continue;
        auto p = entry.path();
        if (p.extension() == ".json") out.push_back(p.stem().string());
    }
    return out;
}

bool save_profile(const std::string& name) {
    if (name.empty()) return false;
    auto* owner = g_owner.load();
    if (owner == nullptr) return false;
    nlohmann::ordered_json snapshot;
    if (owner->snapshot(snapshot) != SAO_STATUS_OK) return false;
    std::error_code ec;
    fs::create_directories(profiles_dir(), ec);
    fs::path f = profiles_dir() / (name + ".json");
    fs::path tmp = f;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << snapshot.dump(2);
    }
    fs::rename(tmp, f, ec);
    return !ec;
}

bool load_profile(const std::string& name) {
    fs::path f = profiles_dir() / (name + ".json");
    if (!fs::exists(f)) return false;
    auto* owner = g_owner.load();
    if (owner == nullptr) return false;
    std::ifstream in(f);
    if (!in) return false;
    nlohmann::ordered_json j;
    try {
        in >> j;
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;
    for (auto it = j.begin(); it != j.end(); ++it) {
        (void)owner->set_value(it.key(), it.value());
    }
    return owner->save() == SAO_STATUS_OK;
}

bool delete_profile(const std::string& name) {
    fs::path f = profiles_dir() / (name + ".json");
    std::error_code ec;
    return fs::remove(f, ec);
}

} // namespace sao::launcher::settings
