// hotkey_config_panel.cpp — Phase 12 (Python parity closure production).
//
// Formats the current hotkey table into a compact UTF-8 listing and displays
// it via MessageBox (read-only view). Rebinding uses the CLI-style
// sao_launcher_hotkey_rebind API — a fully interactive entity_shell panel
// depends on widget_input capture-mode which the widget kit doesn't yet
// expose; this view + rebind API is the shipped surface for users.

#include "hotkey_config_panel.h"

#include "hotkey_manager.h"

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sao::launcher::hotkey {

std::string format_combo_utf8(uint32_t vk, uint32_t modifiers) {
    std::string out;
#if defined(_WIN32)
    if (modifiers & MOD_CONTROL) out += "Ctrl+";
    if (modifiers & MOD_ALT)     out += "Alt+";
    if (modifiers & MOD_SHIFT)   out += "Shift+";
    if (modifiers & MOD_WIN)     out += "Win+";
#endif
    if (vk >= 'A' && vk <= 'Z') {
        out += static_cast<char>(vk);
    } else if (vk >= '0' && vk <= '9') {
        out += static_cast<char>(vk);
    } else if (vk == VK_HOME)   out += "Home";
    else if (vk == VK_INSERT)   out += "Insert";
    else if (vk == VK_DELETE)   out += "Delete";
    else if (vk == VK_ESCAPE)   out += "Esc";
    else if (vk == VK_SPACE)    out += "Space";
    else if (vk == VK_RETURN)   out += "Enter";
    else if (vk >= VK_F1 && vk <= VK_F24) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "F%u", vk - VK_F1 + 1);
        out += buf;
    } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "VK(0x%X)", vk);
        out += buf;
    }
    return out;
}

void open_config_panel() {
    auto bindings = snapshot();
    std::string body = "Current Hotkeys\n\n";
    for (const auto& b : bindings) {
        body += b.id + " (" + b.description + "): " +
                format_combo_utf8(b.vk, b.modifiers) + "\n";
    }
    body += "\nTo rebind, call sao_launcher_hotkey_rebind(id, vk, mods) from "
            "the settings API and restart.";
#if defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, body.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, body.c_str(), -1, w.data(), wlen);
    MessageBoxW(nullptr, w.c_str(), L"SAO Auto — Hotkeys",
                 MB_OK | MB_ICONINFORMATION);
#endif
}

} // namespace sao::launcher::hotkey
