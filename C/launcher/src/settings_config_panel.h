// settings_config_panel.h — interactive settings editor panel.
//
// Phase 13 (Python parity closure) — port of gui_modules/settings_manager.py.
// Categorized (audio/display/hotkeys/network/plugins); candidate buffer +
// live-preview via sao_sdk_sound_test_beep / theme_apply_preview; final
// apply commits via settings_owner_set_batch.

#pragma once

namespace sao::launcher::settings {

// Open the config panel (blocks until user closes / applies).
void open_config_panel();

} // namespace sao::launcher::settings
