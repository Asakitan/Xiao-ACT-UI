// SAO Auto — render-hook + overlay registry.
//
// 1:1 port of `sao_auto/python/act_platform/render_hooks.py`.
//
// A *surface* is any addressable UI region.  Plugins gain three powers
// over a surface:
//   * `register_hook`  — mutate / replace the render payload before the
//     host draws it (priority-ordered).
//   * `set_overlay`    — declare a ui_spec drawn on top of the native
//     content in a plugin layer.
//   * (UI panels live in the plugin manager surface and are handled
//     separately by the plugin manager itself.)

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/engine/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_engine_render_hook_registry_s* sao_engine_render_hook_registry_handle_t;
typedef uint64_t sao_engine_hook_token_t;

// Wildcard surface — hooks/overlays registered here apply to every surface.
#define SAO_ENGINE_ALL_SURFACES "*"

// Hook callback.  Both input and output payloads are UTF-8 JSON.
// Returning NULL for out_payload means "leave payload unchanged".
// Setting payload["__plugin_override__"] to a ui_spec fully replaces
// the native render.
typedef sao_status_t (SAO_ENGINE_CALL* sao_engine_hook_callback_t)(
    const char* surface_id_utf8,
    const uint8_t* payload_json_utf8,
    size_t payload_len,
    uint8_t* out_payload_json_utf8,
    size_t out_payload_capacity,
    size_t* out_payload_written,
    void* user_data);

enum sao_engine_render_clock_point_e : int32_t {
    SAO_ENGINE_RENDER_BEFORE_COMPOSITOR = 0,
    SAO_ENGINE_RENDER_AFTER_COMPOSITOR = 1,
    SAO_ENGINE_RENDER_BEFORE_PRESENT = 2,
    SAO_ENGINE_RENDER_AFTER_PRESENT = 3,
};

enum sao_engine_render_dispatch_flag_e : uint32_t {
    SAO_ENGINE_RENDER_DISPATCH_LOGICAL_TICK = 1u << 0u,
    SAO_ENGINE_RENDER_DISPATCH_COMPOSITOR_PRESENT = 1u << 1u,
    SAO_ENGINE_RENDER_DISPATCH_GPU_PRESENT = 1u << 2u,
    SAO_ENGINE_RENDER_DISPATCH_REDRAW_REQUESTED = 1u << 3u,
};

struct SaoEngineRenderClockPayload {
    int64_t frame_time_us;
    uint32_t frame_index;
    uint32_t frame_delta_us;
    int32_t viewport_x_px;
    int32_t viewport_y_px;
    int32_t viewport_width_px;
    int32_t viewport_height_px;
    uint32_t flags;
    uint32_t reserved;
};

typedef sao_status_t (SAO_ENGINE_CALL* sao_engine_render_clock_callback_t)(
    int32_t hook_point,
    const struct SaoEngineRenderClockPayload* payload,
    void* user_data);

typedef void (SAO_ENGINE_CALL* sao_engine_render_clock_user_data_release_t)(
    void* user_data);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_render_hook_registry_create(
    sao_engine_render_hook_registry_handle_t* out_handle);

SAO_ENGINE_API void SAO_ENGINE_CALL sao_engine_render_hook_registry_destroy(
    sao_engine_render_hook_registry_handle_t handle);

// Reports whether a native rendering-API provider is bound. The standalone
// engine registry is host-dispatched and returns SAO_STATUS_ERR_NOT_IMPLEMENTED;
// callers must not interpret logical hook registration as GPU/API interception.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL
sao_engine_render_hook_provider_status(
    sao_engine_render_hook_registry_handle_t handle);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_render_hook_register(
    sao_engine_render_hook_registry_handle_t handle,
    const char* plugin_id_utf8,
    const char* surface_id_utf8,
    float priority,
    sao_engine_hook_callback_t callback,
    void* user_data,
    sao_engine_hook_token_t* out_token);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_render_hook_unregister(
    sao_engine_render_hook_registry_handle_t handle,
    sao_engine_hook_token_t token);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL
sao_engine_render_clock_register(
    sao_engine_render_hook_registry_handle_t handle,
    const char* plugin_id_utf8,
    const char* surface_id_utf8,
    int32_t hook_point,
    float priority,
    sao_engine_render_clock_callback_t callback,
    void* user_data,
    sao_engine_render_clock_user_data_release_t release_user_data,
    sao_engine_hook_token_t* out_token);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL
sao_engine_render_clock_unregister(
    sao_engine_render_hook_registry_handle_t handle,
    sao_engine_hook_token_t token);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL
sao_engine_render_clock_request_redraw(
    sao_engine_render_hook_registry_handle_t handle,
    const char* surface_id_utf8);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL
sao_engine_render_clock_dispatch(
    sao_engine_render_hook_registry_handle_t handle,
    const char* surface_id_utf8,
    int32_t hook_point,
    uint64_t monotonic_time_ns,
    int32_t viewport_x_px,
    int32_t viewport_y_px,
    int32_t viewport_width_px,
    int32_t viewport_height_px,
    uint32_t dispatch_flags);

// Set (or replace) the plugin's overlay spec for the given surface.
// spec_json_utf8 must be a normalized ui_spec (see ui_spec.h).
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_render_hook_set_overlay(
    sao_engine_render_hook_registry_handle_t handle,
    const char* plugin_id_utf8,
    const char* surface_id_utf8,
    const uint8_t* spec_json_utf8,
    size_t spec_len);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_render_hook_clear_overlay(
    sao_engine_render_hook_registry_handle_t handle,
    const char* plugin_id_utf8,
    const char* surface_id_utf8);      // null clears all surfaces for the plugin

// Host-side dispatch — the compositor calls this to run the hook chain
// for a surface and get the final payload to render.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_render_hook_dispatch(
    sao_engine_render_hook_registry_handle_t handle,
    const char* surface_id_utf8,
    const uint8_t* input_json_utf8,
    size_t input_len,
    uint8_t* out_json_utf8,
    size_t out_capacity,
    size_t* out_bytes_written);

#ifdef __cplusplus
}  // extern "C"
#endif
