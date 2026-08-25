#include "entity_builtin_action_internal.h"

#include "sao/ui/entity_shell.h"

namespace sao::launcher::entity_builtin_action {
namespace {

sao_status_t first_failure(sao_status_t current, sao_status_t candidate) noexcept {
    return current == SAO_STATUS_OK && candidate != SAO_STATUS_OK ? candidate : current;
}

sao_status_t rollback_topmost(const Operations& operations, bool previous) noexcept {
    sao_status_t status = SAO_STATUS_OK;
    if (operations.persist_topmost_mode != nullptr) {
        status =
            first_failure(status, operations.persist_topmost_mode(previous, operations.user_data));
    }
    if (operations.apply_topmost_mode != nullptr) {
        status =
            first_failure(status, operations.apply_topmost_mode(previous, operations.user_data));
    }
    return status;
}

sao_status_t rollback_streaming(const Operations& operations, bool previous) noexcept {
    sao_status_t status = SAO_STATUS_OK;
    if (operations.persist_streaming_mode != nullptr) {
        status = first_failure(status,
                               operations.persist_streaming_mode(previous, operations.user_data));
    }
    if (operations.apply_streaming_mode != nullptr) {
        status =
            first_failure(status, operations.apply_streaming_mode(previous, operations.user_data));
    }
    return status;
}

sao_status_t fail(State& state, sao_status_t status,
                  sao_status_t compensation_status = SAO_STATUS_OK) noexcept {
    if (status == SAO_STATUS_ERR_UNKNOWN || compensation_status != SAO_STATUS_OK) {
        state.controls_degraded = true;
        state.last_status = SAO_STATUS_ERR_UNKNOWN;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    state.last_status = status;
    return status;
}

sao_status_t toggle_topmost(State& state, const Operations& operations) noexcept {
    if (state.controls_degraded) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    if (operations.apply_topmost_mode == nullptr || operations.persist_topmost_mode == nullptr ||
        operations.refresh_entity == nullptr) {
        return fail(state, SAO_STATUS_ERR_NOT_INITIALIZED);
    }

    const bool previous = state.topmost;
    const bool target = !previous;
    sao_status_t status = operations.apply_topmost_mode(target, operations.user_data);
    if (status != SAO_STATUS_OK) {
        return fail(state, status);
    }
    status = operations.persist_topmost_mode(target, operations.user_data);
    if (status != SAO_STATUS_OK) {
        const sao_status_t compensation_status =
            operations.apply_topmost_mode(previous, operations.user_data);
        return fail(state, status, compensation_status);
    }

    state.topmost = target;
    status = operations.refresh_entity(operations.user_data);
    if (status != SAO_STATUS_OK) {
        state.topmost = previous;
        sao_status_t compensation_status = rollback_topmost(operations, previous);
        compensation_status =
            first_failure(compensation_status, operations.refresh_entity(operations.user_data));
        return fail(state, status, compensation_status);
    }
    state.last_status = SAO_STATUS_OK;
    return SAO_STATUS_OK;
}

sao_status_t toggle_streaming(State& state, const Operations& operations) noexcept {
    if (state.controls_degraded) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    if (!state.streaming_entitled) {
        return fail(state, SAO_STATUS_ERR_ACCESS_DENIED);
    }
    if (operations.apply_streaming_mode == nullptr ||
        operations.persist_streaming_mode == nullptr || operations.refresh_entity == nullptr) {
        return fail(state, SAO_STATUS_ERR_NOT_INITIALIZED);
    }

    const bool previous = state.streaming_mode;
    const bool target = !previous;
    sao_status_t status = operations.apply_streaming_mode(target, operations.user_data);
    if (status != SAO_STATUS_OK) {
        return fail(state, status);
    }
    status = operations.persist_streaming_mode(target, operations.user_data);
    if (status != SAO_STATUS_OK) {
        const sao_status_t compensation_status =
            operations.apply_streaming_mode(previous, operations.user_data);
        return fail(state, status, compensation_status);
    }

    state.streaming_mode = target;
    status = operations.refresh_entity(operations.user_data);
    if (status != SAO_STATUS_OK) {
        state.streaming_mode = previous;
        sao_status_t compensation_status = rollback_streaming(operations, previous);
        compensation_status =
            first_failure(compensation_status, operations.refresh_entity(operations.user_data));
        return fail(state, status, compensation_status);
    }
    state.last_status = SAO_STATUS_OK;
    return SAO_STATUS_OK;
}

sao_status_t reload_plugins(State& state, const Operations& operations) noexcept {
    if (operations.reload_plugins == nullptr || operations.refresh_entity == nullptr) {
        return fail(state, SAO_STATUS_ERR_NOT_INITIALIZED);
    }
    const sao_status_t status = operations.reload_plugins(operations.user_data);
    const sao_status_t refresh_status = operations.refresh_entity(operations.user_data);
    if (refresh_status != SAO_STATUS_OK) {
        return fail(state, status == SAO_STATUS_OK ? refresh_status : status, refresh_status);
    }
    return fail(state, status);
}

sao_status_t run_owned_action(State& state, RunOwnedActionFn action,
                              const Operations& operations) noexcept {
    if (action == nullptr) {
        return fail(state, SAO_STATUS_ERR_NOT_INITIALIZED);
    }
    return fail(state, action(operations.user_data));
}

} // namespace

bool should_show_shared_fisheye(const SharedFisheyeVisibility& visibility) noexcept {
    return visibility.workshop || visibility.plugin_manager || visibility.process_selector ||
           visibility.entity_menu || visibility.native_panel;
}

sao_status_t authorization_status(std::int32_t action, const State& state) noexcept {
    const bool controls_ready = state.authority.controls;
    bool control_action = false;
    bool available = false;
    switch (action) {
    case SAO_UI_ENTITY_ACTION_OPEN_ABOUT:
        available = state.authority.about;
        break;
    case SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST:
        control_action = true;
        available = controls_ready && state.authority.topmost;
        break;
    case SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR:
        control_action = true;
        available = controls_ready && state.authority.nervgear;
        break;
    case SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE:
        control_action = true;
        available = controls_ready && state.authority.streaming;
        break;
    case SAO_UI_ENTITY_ACTION_SAVE_SETTINGS:
        control_action = true;
        available = controls_ready && state.authority.save_settings;
        break;
    case SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL:
        control_action = true;
        available = controls_ready && state.authority.fisheye_procedural;
        break;
    case SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE:
        control_action = true;
        available = controls_ready && state.authority.fisheye_live;
        break;
    case SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR:
        available = state.authority.ai_editor;
        break;
    case SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP:
        available = state.authority.workshop;
        break;
    case SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR:
        available = state.authority.process_selector;
        break;
    case SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER:
        available = state.authority.plugin_runtime && state.authority.plugin_manager;
        break;
    case SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS:
        available = state.authority.plugin_runtime && state.authority.reload_plugins;
        break;
    case SAO_UI_ENTITY_ACTION_PLUGIN_STATUS:
        available = state.authority.plugin_runtime && state.authority.plugin_status;
        break;
    case SAO_UI_ENTITY_ACTION_OPEN_LICENSE_ACTIVATION:
        available = state.authority.license_activation;
        break;
    case SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT:
    case SAO_UI_ENTITY_ACTION_SET_ALL_DARK:
        available = state.authority.theme;
        break;
    default:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!state.authority.publication_available) {
        return kKnownUnavailableStatus;
    }
    if (control_action && state.controls_degraded) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return available ? SAO_STATUS_OK : kKnownUnavailableStatus;
}

sao_status_t dispatch(std::int32_t action, State& state, const Operations& operations) noexcept {
    const sao_status_t authority_status = authorization_status(action, state);
    if (authority_status != SAO_STATUS_OK) {
        state.last_status = authority_status;
        return authority_status;
    }
    switch (action) {
    case SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST:
        return toggle_topmost(state, operations);
    case SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE:
        return toggle_streaming(state, operations);
    case SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS:
        return reload_plugins(state, operations);
    case SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL:
        return run_owned_action(state, operations.set_fisheye_procedural, operations);
    case SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE:
        return run_owned_action(state, operations.set_fisheye_live, operations);
    case SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP:
        return run_owned_action(state, operations.open_workshop, operations);
    case SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR:
        return run_owned_action(state, operations.open_process_selector, operations);
    case SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER:
        return run_owned_action(state, operations.open_plugin_manager, operations);
    case SAO_UI_ENTITY_ACTION_PLUGIN_STATUS:
        return run_owned_action(state, operations.open_plugin_status, operations);
    case SAO_UI_ENTITY_ACTION_OPEN_LICENSE_ACTIVATION:
        return run_owned_action(state, operations.open_license_panel, operations);
    case SAO_UI_ENTITY_ACTION_OPEN_ABOUT:
    case SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR:
    case SAO_UI_ENTITY_ACTION_SAVE_SETTINGS:
    case SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR:
    case SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT:
    case SAO_UI_ENTITY_ACTION_SET_ALL_DARK:
        return fail(state, kKnownUnavailableStatus);
    default:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

} // namespace sao::launcher::entity_builtin_action
