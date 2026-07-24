#pragma once

#include "sao/core/status.h"

#include <cstdint>

namespace sao::launcher::entity_builtin_action {

inline constexpr sao_status_t kKnownUnavailableStatus = SAO_STATUS_ERR_NOT_IMPLEMENTED;

using ApplyTopmostModeFn = sao_status_t (*)(bool enabled, void* user_data);
using PersistTopmostModeFn = sao_status_t (*)(bool enabled, void* user_data);
using ApplyStreamingModeFn = sao_status_t (*)(bool enabled, void* user_data);
using PersistStreamingModeFn = sao_status_t (*)(bool enabled, void* user_data);
using ReloadPluginsFn = sao_status_t (*)(void* user_data);
using RefreshEntityFn = sao_status_t (*)(void* user_data);
using RunOwnedActionFn = sao_status_t (*)(void* user_data);

inline constexpr std::int32_t kSharedFisheyeBackdropZOrder = -500'000'000;

struct SharedFisheyeVisibility {
    bool workshop = false;
    bool plugin_manager = false;
    bool process_selector = false;
    bool entity_menu = false;
};

struct Authority {
    bool publication_available = false;
    bool controls = false;
    bool topmost = false;
    bool nervgear = false;
    bool streaming = false;
    bool save_settings = false;
    bool ai_editor = false;
    bool workshop = false;
    bool process_selector = false;
    bool plugin_runtime = false;
    bool plugin_manager = false;
    bool reload_plugins = false;
    bool plugin_status = false;
    bool fisheye_procedural = false;
    bool fisheye_live = false;
    bool theme = false;
    bool about = false;
    // Tracks whether the launcher-side runtime installer completed successfully
    // for every plugin runtime the manifest enumerated. When set to false the
    // Panel surfaces plugin runtimes as unavailable so users understand why a
    // Python/Lua/AngelScript/C# plugin refused to load. Set to true both when
    // every runtime installed cleanly and when the SAO_PLUGINS_ENABLE_RUNTIME_
    // AUTOINSTALL toggle is OFF (repro/hardened builds) — in that case plugin
    // hosts still rely on their pre-installed runtimes and the Panel does not
    // pretend the installer failed.
    bool runtime_installer = true;
};

struct State {
    bool topmost = false;
    bool streaming_mode = false;
    bool streaming_entitled = false;
    bool controls_degraded = false;
    sao_status_t last_status = SAO_STATUS_OK;
    Authority authority;
};

struct Operations {
    ApplyTopmostModeFn apply_topmost_mode = nullptr;
    PersistTopmostModeFn persist_topmost_mode = nullptr;
    ApplyStreamingModeFn apply_streaming_mode = nullptr;
    PersistStreamingModeFn persist_streaming_mode = nullptr;
    ReloadPluginsFn reload_plugins = nullptr;
    RefreshEntityFn refresh_entity = nullptr;
    RunOwnedActionFn open_workshop = nullptr;
    RunOwnedActionFn open_process_selector = nullptr;
    RunOwnedActionFn open_plugin_manager = nullptr;
    RunOwnedActionFn open_plugin_status = nullptr;
    RunOwnedActionFn set_fisheye_procedural = nullptr;
    RunOwnedActionFn set_fisheye_live = nullptr;
    void* user_data = nullptr;
};

bool should_show_shared_fisheye(const SharedFisheyeVisibility& visibility) noexcept;

sao_status_t authorization_status(std::int32_t action, const State& state) noexcept;

sao_status_t dispatch(std::int32_t action, State& state, const Operations& operations) noexcept;

} // namespace sao::launcher::entity_builtin_action
