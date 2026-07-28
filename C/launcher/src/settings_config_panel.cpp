// settings_config_panel.cpp — Phase 13 (production).
//
// Formats the current settings snapshot as UTF-8 and shows a native
// message box for read-only viewing. Edits happen via the settings
// programmatic API + save_profile / load_profile in settings_profiles.
//
// A fully interactive entity_shell panel builder requires widget_input
// per-field bind — that widget path is a future extension. Meanwhile
// this surface is fully callable + guaranteed non-crashing.

#include "settings_config_panel.h"
#include "settings_owner_internal.h"

#include <atomic>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sao::launcher::settings {

namespace {
std::atomic<sao::launcher::settings_owner::SettingsOwner*> g_owner{nullptr};
}

extern "C" void sao_launcher_settings_panel_set_owner(void* owner_opaque) {
    g_owner.store(reinterpret_cast<sao::launcher::settings_owner::SettingsOwner*>(
        owner_opaque));
}

void open_config_panel() {
    auto* owner = g_owner.load();
    if (owner == nullptr) return;
    nlohmann::ordered_json snapshot;
    if (owner->snapshot(snapshot) != SAO_STATUS_OK) return;
    std::string body = "SAO Auto Settings\n\n" + snapshot.dump(2);
#if defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, body.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, body.c_str(), -1, w.data(), wlen);
    MessageBoxW(nullptr, w.c_str(), L"SAO Auto — Settings",
                 MB_OK | MB_ICONINFORMATION);
#endif
}

} // namespace sao::launcher::settings
