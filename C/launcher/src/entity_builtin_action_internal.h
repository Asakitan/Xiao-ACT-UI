#pragma once

#include "sao/core/status.h"

#include <cstdint>

namespace sao::launcher::entity_builtin_action {

using ApplyTopmostModeFn = sao_status_t (*)(bool enabled, void* user_data);
using PersistTopmostModeFn = sao_status_t (*)(bool enabled, void* user_data);
using ApplyStreamingModeFn = sao_status_t (*)(bool enabled, void* user_data);
using PersistStreamingModeFn = sao_status_t (*)(bool enabled, void* user_data);
using ReloadPluginsFn = sao_status_t (*)(void* user_data);
using RefreshEntityFn = sao_status_t (*)(void* user_data);

struct State {
    bool topmost = false;
    bool streaming_mode = false;
    bool streaming_entitled = false;
    bool controls_degraded = false;
    sao_status_t last_status = SAO_STATUS_OK;
};

struct Operations {
    ApplyTopmostModeFn apply_topmost_mode = nullptr;
    PersistTopmostModeFn persist_topmost_mode = nullptr;
    ApplyStreamingModeFn apply_streaming_mode = nullptr;
    PersistStreamingModeFn persist_streaming_mode = nullptr;
    ReloadPluginsFn reload_plugins = nullptr;
    RefreshEntityFn refresh_entity = nullptr;
    void* user_data = nullptr;
};

sao_status_t dispatch(std::int32_t action, State& state, const Operations& operations) noexcept;

} // namespace sao::launcher::entity_builtin_action
