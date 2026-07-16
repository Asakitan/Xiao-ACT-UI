// SAO Auto — platform runtime context.
//
// A game-agnostic aggregate owning the five engine primitives:
//   * event bus
//   * state store
//   * render hook registry
//   * (ui_spec has no runtime object — it is a pure validator)
//
// It is a plain aggregate — creation order matters (bus first, then
// consumers).  Ownership follows RAII on the C++ side, but the ABI
// exposes it as an opaque handle so plugins can pass it around.
//
// Game-specific state (DPS rollups, boss HP, encounter clocks,
// mechanic triggers) is NOT owned here — plugins allocate and manage
// their own aggregates, and communicate with the platform via the
// event bus + state store defined below.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/engine/abi.h"
#include "sao/engine/event_bus.h"
#include "sao/engine/render_hook.h"
#include "sao/engine/state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_engine_runtime_s* sao_engine_runtime_handle_t;

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_runtime_create(
    sao_engine_runtime_handle_t* out_handle);

SAO_ENGINE_API void SAO_ENGINE_CALL sao_engine_runtime_destroy(
    sao_engine_runtime_handle_t handle);

// Accessors — return borrowed handles owned by the runtime.  Do not
// close them individually; sao_engine_runtime_destroy tears everything
// down in the reverse order.
SAO_ENGINE_API sao_engine_event_bus_handle_t SAO_ENGINE_CALL
    sao_engine_runtime_event_bus(sao_engine_runtime_handle_t handle);

SAO_ENGINE_API sao_engine_state_handle_t SAO_ENGINE_CALL
    sao_engine_runtime_state(sao_engine_runtime_handle_t handle);

SAO_ENGINE_API sao_engine_render_hook_registry_handle_t SAO_ENGINE_CALL
    sao_engine_runtime_render_hooks(sao_engine_runtime_handle_t handle);

// Runtime tick — plugins/host call this periodically (~60 Hz). It provides a
// serialized monotonic lifecycle boundary for runtime-owned primitives.
// ts_ns is the current monotonic nanosecond timestamp.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_runtime_tick(
    sao_engine_runtime_handle_t handle, uint64_t ts_ns);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL
sao_engine_runtime_render_dispatch(
    sao_engine_runtime_handle_t handle,
    const char* surface_id_utf8,
    int32_t hook_point,
    uint64_t monotonic_time_ns,
    int32_t viewport_x_px,
    int32_t viewport_y_px,
    int32_t viewport_width_px,
    int32_t viewport_height_px,
    uint32_t dispatch_flags);

#ifdef __cplusplus
}  // extern "C"
#endif
