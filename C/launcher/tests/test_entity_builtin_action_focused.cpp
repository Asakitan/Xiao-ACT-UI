#include "entity_builtin_action_internal.h"
#include "sao/ui/entity_shell.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

struct ActionFixture {
    std::vector<std::string> events;
    sao_status_t topmost_apply_status = SAO_STATUS_OK;
    sao_status_t topmost_rollback_status = SAO_STATUS_OK;
    sao_status_t topmost_persist_status = SAO_STATUS_OK;
    sao_status_t apply_status = SAO_STATUS_OK;
    sao_status_t rollback_status = SAO_STATUS_OK;
    sao_status_t persist_status = SAO_STATUS_OK;
    sao_status_t reload_status = SAO_STATUS_OK;
    sao_status_t refresh_status = SAO_STATUS_OK;
    sao_status_t refresh_rollback_status = SAO_STATUS_OK;
    std::size_t refresh_attempt = 0;
};

sao_status_t apply_topmost(bool enabled, void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.push_back(enabled ? "topmost-apply:on" : "topmost-apply:off");
    return enabled ? fixture.topmost_apply_status : fixture.topmost_rollback_status;
}

sao_status_t persist_topmost(bool enabled, void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.push_back(enabled ? "topmost-persist:on" : "topmost-persist:off");
    return fixture.topmost_persist_status;
}

sao_status_t apply_streaming(bool enabled, void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.push_back(enabled ? "apply:on" : "apply:off");
    return enabled ? fixture.apply_status : fixture.rollback_status;
}

sao_status_t persist_streaming(bool enabled, void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.push_back(enabled ? "persist:on" : "persist:off");
    return fixture.persist_status;
}

sao_status_t reload_plugins(void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.emplace_back("reload");
    return fixture.reload_status;
}

sao_status_t refresh_entity(void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.emplace_back("refresh");
    return fixture.refresh_attempt++ == 0 ? fixture.refresh_status
                                          : fixture.refresh_rollback_status;
}

sao::launcher::entity_builtin_action::Operations operations(ActionFixture& fixture) {
    return {
        &apply_topmost,  &persist_topmost, &apply_streaming, &persist_streaming,
        &reload_plugins, &refresh_entity,  &fixture,
    };
}

} // namespace

TEST_CASE("streaming action applies persists and publishes one transaction",
          "[launcher][entity][builtin_action][streaming][focused]") {
    ActionFixture fixture;
    sao::launcher::entity_builtin_action::State state{false, false, true};

    REQUIRE(sao::launcher::entity_builtin_action::dispatch(
                SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE, state, operations(fixture)) ==
            SAO_STATUS_OK);
    CHECK(state.streaming_mode);
    CHECK(fixture.events == std::vector<std::string>{"apply:on", "persist:on", "refresh"});
}

TEST_CASE("streaming action enforces entitlement and provider availability",
          "[launcher][entity][builtin_action][streaming][focused]") {
    ActionFixture fixture;
    auto provider = operations(fixture);
    sao::launcher::entity_builtin_action::State state{false, false, false};

    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE,
                                                         state,
                                                         provider) == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(fixture.events.empty());
    CHECK_FALSE(state.streaming_mode);

    state.streaming_entitled = true;
    provider.apply_streaming_mode = nullptr;
    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE,
                                                         state, provider) ==
          SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(fixture.events.empty());
}

TEST_CASE("streaming action rolls runtime back when persistence fails",
          "[launcher][entity][builtin_action][streaming][rollback][focused]") {
    ActionFixture fixture;
    fixture.persist_status = SAO_STATUS_ERR_OS_CALL_FAILED;
    sao::launcher::entity_builtin_action::State state{false, false, true};

    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE,
                                                         state, operations(fixture)) ==
          SAO_STATUS_ERR_OS_CALL_FAILED);
    CHECK_FALSE(state.streaming_mode);
    CHECK(fixture.events == std::vector<std::string>{"apply:on", "persist:on", "apply:off"});
}

TEST_CASE("streaming action rolls persistence and runtime back when publication fails",
          "[launcher][entity][builtin_action][streaming][rollback][focused]") {
    ActionFixture fixture;
    fixture.refresh_status = SAO_STATUS_ERR_CANCELLED;
    sao::launcher::entity_builtin_action::State state{false, false, true};

    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE,
                                                         state, operations(fixture)) ==
          SAO_STATUS_ERR_CANCELLED);
    CHECK_FALSE(state.streaming_mode);
    CHECK(fixture.events == std::vector<std::string>{"apply:on", "persist:on", "refresh",
                                                     "persist:off", "apply:off", "refresh"});
}

TEST_CASE("streaming compensation failure becomes one observable internal state",
          "[launcher][entity][builtin_action][streaming][rollback][degraded][focused]") {
    ActionFixture fixture;
    fixture.persist_status = SAO_STATUS_ERR_OS_CALL_FAILED;
    fixture.rollback_status = SAO_STATUS_ERR_TIMEOUT;
    sao::launcher::entity_builtin_action::State state{false, false, true};

    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE,
                                                         state, operations(fixture)) ==
          SAO_STATUS_ERR_UNKNOWN);
    CHECK(state.controls_degraded);
    CHECK(state.last_status == SAO_STATUS_ERR_UNKNOWN);
    CHECK_FALSE(state.streaming_mode);
    CHECK(fixture.events == std::vector<std::string>{"apply:on", "persist:on", "apply:off"});

    fixture.events.clear();
    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE,
                                                         state, operations(fixture)) ==
          SAO_STATUS_ERR_UNKNOWN);
    CHECK(fixture.events.empty());
}

TEST_CASE("topmost action applies persists publishes and rolls back as one transaction",
          "[launcher][entity][builtin_action][topmost][rollback][focused]") {
    ActionFixture fixture;
    sao::launcher::entity_builtin_action::State state{};

    REQUIRE(sao::launcher::entity_builtin_action::dispatch(
                SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST, state, operations(fixture)) == SAO_STATUS_OK);
    CHECK(state.topmost);
    CHECK(fixture.events ==
          std::vector<std::string>{"topmost-apply:on", "topmost-persist:on", "refresh"});

    fixture.events.clear();
    fixture.refresh_status = SAO_STATUS_ERR_CANCELLED;
    fixture.refresh_attempt = 0;
    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST, state,
                                                         operations(fixture)) ==
          SAO_STATUS_ERR_CANCELLED);
    CHECK(state.topmost);
    CHECK(fixture.events == std::vector<std::string>{"topmost-apply:off", "topmost-persist:off",
                                                     "refresh", "topmost-persist:on",
                                                     "topmost-apply:on", "refresh"});
}

TEST_CASE("topmost action requires the owning z-order and persistence authorities",
          "[launcher][entity][builtin_action][topmost][focused]") {
    ActionFixture fixture;
    auto provider = operations(fixture);
    provider.apply_topmost_mode = nullptr;
    sao::launcher::entity_builtin_action::State state{};

    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST, state,
                                                         provider) ==
          SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK_FALSE(state.topmost);
    CHECK(fixture.events.empty());
}

TEST_CASE("topmost compensation failure becomes one observable internal state",
          "[launcher][entity][builtin_action][topmost][rollback][degraded][focused]") {
    ActionFixture fixture;
    fixture.topmost_persist_status = SAO_STATUS_ERR_OS_CALL_FAILED;
    fixture.topmost_rollback_status = SAO_STATUS_ERR_TIMEOUT;
    sao::launcher::entity_builtin_action::State state{};

    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST, state,
                                                         operations(fixture)) ==
          SAO_STATUS_ERR_UNKNOWN);
    CHECK(state.controls_degraded);
    CHECK(state.last_status == SAO_STATUS_ERR_UNKNOWN);
    CHECK_FALSE(state.topmost);
}

TEST_CASE("plugin reload publishes only after the lifecycle succeeds",
          "[launcher][entity][builtin_action][plugins][focused]") {
    ActionFixture fixture;
    sao::launcher::entity_builtin_action::State state{};

    REQUIRE(sao::launcher::entity_builtin_action::dispatch(
                SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS, state, operations(fixture)) == SAO_STATUS_OK);
    CHECK(fixture.events == std::vector<std::string>{"reload", "refresh"});

    fixture.events.clear();
    fixture.reload_status = SAO_STATUS_ERR_OS_CALL_FAILED;
    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS, state,
                                                         operations(fixture)) ==
          SAO_STATUS_ERR_OS_CALL_FAILED);
    CHECK(fixture.events == std::vector<std::string>{"reload", "refresh"});
}

TEST_CASE("plugin reload surfaces failed generation refresh as compensation failure",
          "[launcher][entity][builtin_action][plugins][rollback][focused]") {
    ActionFixture fixture;
    fixture.reload_status = SAO_STATUS_ERR_OS_CALL_FAILED;
    fixture.refresh_status = SAO_STATUS_ERR_CANCELLED;
    sao::launcher::entity_builtin_action::State state{};

    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS, state,
                                                         operations(fixture)) ==
          SAO_STATUS_ERR_UNKNOWN);
    CHECK(state.controls_degraded);
    CHECK(state.last_status == SAO_STATUS_ERR_UNKNOWN);
    CHECK(fixture.events == std::vector<std::string>{"reload", "refresh"});
}

TEST_CASE("ownerless builtin actions stay explicit fail closed",
          "[launcher][entity][builtin_action][focused]") {
    sao::launcher::entity_builtin_action::State state{};
    const sao::launcher::entity_builtin_action::Operations provider{};
    constexpr SaoUiEntityAction unsupported[] = {
        SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL, SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE,
        SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP,          SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR,
        SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER,    SAO_UI_ENTITY_ACTION_PLUGIN_STATUS,
    };
    for (const auto action : unsupported) {
        CHECK(sao::launcher::entity_builtin_action::dispatch(action, state, provider) ==
              SAO_STATUS_ERR_NOT_IMPLEMENTED);
    }
    CHECK(sao::launcher::entity_builtin_action::dispatch(static_cast<SaoUiEntityAction>(-777),
                                                         state, provider) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
}
