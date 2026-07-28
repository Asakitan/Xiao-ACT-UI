// hotkey_config_panel.h — user-facing hotkey rebinding UI panel.
//
// Phase 12 (Python parity closure) — port of gui_modules/sao_hotkey_manager.py
// interactive config dialog. Presents each binding as a row with a "rebind"
// button that captures the next VK combo.

#pragma once

#include <string>

namespace sao::launcher::hotkey {

// Open the config panel. Returns after user closes it.
// Persists any changes via hotkey_manager::save().
void open_config_panel();

// Preview a "captured" combo (called from capture-mode widget).
std::string format_combo_utf8(uint32_t vk, uint32_t modifiers);

} // namespace sao::launcher::hotkey
