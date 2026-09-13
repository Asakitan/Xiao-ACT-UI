#pragma once

#include "sao/ui/panel_sdk.h"
#include <cstring>
#include <cstdio>

namespace sao::launcher {

template<class Open, class Close>
sao_status_t toggle_menu_panel(const char* id, Open&& open, Close&& close) noexcept {
    struct Probe { const char* id; bool visible{}; } probe{id};
    const auto status = sao_ui_panel_registry_iterate(
        [](sao_ui_panel_handle_t, const SaoPanelDescriptor* descriptor, void* data) {
            auto& value = *static_cast<Probe*>(data);
            if (descriptor && descriptor->panel_id_utf8 &&
                std::strcmp(descriptor->panel_id_utf8, value.id) == 0)
                value.visible = descriptor->visible;
        }, &probe);
    if (status != SAO_STATUS_OK) return status;
        try {
        const auto result = probe.visible ? close() : open();
    #ifndef NDEBUG
        std::fprintf(stderr, "UI_PANEL_TOGGLE id=%s hiding=%d status=%d\n", id, probe.visible ? 1 : 0, result);
    #endif
        return result;
        }
    catch (...) { return SAO_STATUS_ERR_UNKNOWN; }
}

} // namespace sao::launcher
