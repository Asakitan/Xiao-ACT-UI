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

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/dialog.h"
#endif

#include <atomic>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

namespace sao::launcher::settings {

namespace {
std::atomic<sao::launcher::settings_owner::SettingsOwner*> g_owner{nullptr};

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
sao_ui_compositor_handle_t borrow_platform_compositor() noexcept {
    void* raw = nullptr;
    if (sao_sdk_platform_get_ui_compositor(&raw) != SAO_SDK_OK)
        return nullptr;
    return static_cast<sao_ui_compositor_handle_t>(raw);
}
#endif
}

extern "C" void sao_launcher_settings_panel_set_owner(void* owner_opaque) {
    g_owner.store(reinterpret_cast<sao::launcher::settings_owner::SettingsOwner*>(
        owner_opaque));
}

void open_config_panel() {
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    sao_ui_compositor_handle_t compositor = borrow_platform_compositor();
    if (compositor == nullptr)
        return;
    auto* owner = g_owner.load();
    if (owner == nullptr) return;
    nlohmann::ordered_json snapshot;
    if (owner->snapshot(snapshot) != SAO_STATUS_OK) return;
    std::string body = "SAO Auto Settings\n\n" + snapshot.dump(2);
    (void)sao_ui_dialog_show_info(compositor, nullptr, "SAO Auto — Settings", body.c_str(),
                                  nullptr, nullptr);
#endif
}

} // namespace sao::launcher::settings
