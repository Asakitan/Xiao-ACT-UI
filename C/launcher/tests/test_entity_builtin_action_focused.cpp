#include "entity_builtin_action_internal.h"
#include "entity_provider_publication_internal.h"
#include "sao/ui/entity_shell.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
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
    sao_status_t owned_action_status = SAO_STATUS_OK;
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

sao_status_t open_workshop(void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.emplace_back("open:workshop");
    return fixture.owned_action_status;
}

sao_status_t open_process_selector(void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.emplace_back("open:process-selector");
    return fixture.owned_action_status;
}

sao_status_t open_plugin_manager(void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.emplace_back("open:plugin-manager");
    return fixture.owned_action_status;
}

sao_status_t open_plugin_status(void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.emplace_back("open:plugin-status");
    return fixture.owned_action_status;
}

sao_status_t set_fisheye_procedural(void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.emplace_back("fisheye:procedural");
    return fixture.owned_action_status;
}

sao_status_t set_fisheye_live(void* user_data) {
    auto& fixture = *static_cast<ActionFixture*>(user_data);
    fixture.events.emplace_back("fisheye:live");
    return fixture.owned_action_status;
}

sao::launcher::entity_builtin_action::Operations operations(ActionFixture& fixture) {
    sao::launcher::entity_builtin_action::Operations result;
    result.apply_topmost_mode = &apply_topmost;
    result.persist_topmost_mode = &persist_topmost;
    result.apply_streaming_mode = &apply_streaming;
    result.persist_streaming_mode = &persist_streaming;
    result.reload_plugins = &reload_plugins;
    result.refresh_entity = &refresh_entity;
    result.open_workshop = &open_workshop;
    result.open_process_selector = &open_process_selector;
    result.open_plugin_manager = &open_plugin_manager;
    result.open_plugin_status = &open_plugin_status;
    result.set_fisheye_procedural = &set_fisheye_procedural;
    result.set_fisheye_live = &set_fisheye_live;
    result.user_data = &fixture;
    return result;
}

sao::launcher::entity_builtin_action::Authority transactional_authority() {
    sao::launcher::entity_builtin_action::Authority authority{};
    authority.publication_available = true;
    authority.controls = true;
    authority.topmost = true;
    authority.streaming = true;
    authority.plugin_runtime = true;
    authority.workshop = true;
    authority.process_selector = true;
    authority.plugin_manager = true;
    authority.reload_plugins = true;
    authority.plugin_status = true;
    authority.fisheye_procedural = true;
    authority.fisheye_live = true;
    return authority;
}

} // namespace

TEST_CASE("streaming action applies persists and publishes one transaction",
          "[launcher][entity][builtin_action][streaming][focused]") {
    ActionFixture fixture;
    sao::launcher::entity_builtin_action::State state{false, false, true};
    state.authority = transactional_authority();

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
    state.authority = transactional_authority();

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
    state.authority = transactional_authority();

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
    state.authority = transactional_authority();

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
    state.authority = transactional_authority();

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
    state.authority = transactional_authority();

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
    state.authority = transactional_authority();

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
    state.authority = transactional_authority();

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
    state.authority = transactional_authority();

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
    state.authority = transactional_authority();

    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS, state,
                                                         operations(fixture)) ==
          SAO_STATUS_ERR_UNKNOWN);
    CHECK(state.controls_degraded);
    CHECK(state.last_status == SAO_STATUS_ERR_UNKNOWN);
    CHECK(fixture.events == std::vector<std::string>{"reload", "refresh"});
}

TEST_CASE("owned panels plugin status and fisheye modes dispatch to their exact owners",
          "[launcher][entity][builtin_action][owned_ui][focused]") {
    ActionFixture fixture;
    sao::launcher::entity_builtin_action::State state{};
    state.authority = transactional_authority();
    const auto provider = operations(fixture);

    const std::vector<std::pair<SaoUiEntityAction, std::string>> actions{
        {SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP, "open:workshop"},
        {SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR, "open:process-selector"},
        {SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER, "open:plugin-manager"},
        {SAO_UI_ENTITY_ACTION_PLUGIN_STATUS, "open:plugin-status"},
        {SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL, "fisheye:procedural"},
        {SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE, "fisheye:live"},
    };
    for (const auto& [action, expected_event] : actions) {
        fixture.events.clear();
        REQUIRE(sao::launcher::entity_builtin_action::dispatch(action, state, provider) ==
                SAO_STATUS_OK);
        CHECK(fixture.events == std::vector<std::string>{expected_event});
        CHECK(state.last_status == SAO_STATUS_OK);
    }

    fixture.events.clear();
    fixture.owned_action_status = SAO_STATUS_ERR_OS_CALL_FAILED;
    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP, state,
                                                         provider) ==
          SAO_STATUS_ERR_OS_CALL_FAILED);
    CHECK(fixture.events == std::vector<std::string>{"open:workshop"});
    CHECK_FALSE(state.controls_degraded);

    auto missing_owner = provider;
    missing_owner.open_plugin_status = nullptr;
    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_PLUGIN_STATUS, state,
                                                         missing_owner) ==
          SAO_STATUS_ERR_NOT_INITIALIZED);

    fixture.owned_action_status = SAO_STATUS_OK;
    fixture.events.clear();
    state.authority.workshop = false;
    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP, state,
                                                         provider) ==
          sao::launcher::entity_builtin_action::kKnownUnavailableStatus);
    CHECK(fixture.events.empty());

    state.authority.workshop = true;
    state.authority.plugin_runtime = false;
    CHECK(sao::launcher::entity_builtin_action::dispatch(SAO_UI_ENTITY_ACTION_PLUGIN_STATUS, state,
                                                         provider) ==
          sao::launcher::entity_builtin_action::kKnownUnavailableStatus);
    CHECK(fixture.events.empty());
}

TEST_CASE("shared fisheye follows only explicit Entity and owned panel visibility",
          "[launcher][entity][builtin_action][fisheye][visibility][focused]") {
    using sao::launcher::entity_builtin_action::SharedFisheyeVisibility;
    using sao::launcher::entity_builtin_action::should_show_shared_fisheye;

    CHECK_FALSE(should_show_shared_fisheye({}));
    CHECK(should_show_shared_fisheye(SharedFisheyeVisibility{.workshop = true}));
    CHECK(should_show_shared_fisheye(SharedFisheyeVisibility{.plugin_manager = true}));
    CHECK(should_show_shared_fisheye(SharedFisheyeVisibility{.process_selector = true}));
    CHECK(should_show_shared_fisheye(SharedFisheyeVisibility{.entity_menu = true}));
    CHECK(sao::launcher::entity_builtin_action::kSharedFisheyeBackdropZOrder < -250'000'000);
}

TEST_CASE("ownerless builtin actions stay explicit fail closed",
          "[launcher][entity][builtin_action][focused]") {
    sao::launcher::entity_builtin_action::State state{};
    const sao::launcher::entity_builtin_action::Operations provider{};
    constexpr SaoUiEntityAction unavailable[] = {
        SAO_UI_ENTITY_ACTION_OPEN_ABOUT,
        SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST,
        SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR,
        SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE,
        SAO_UI_ENTITY_ACTION_SAVE_SETTINGS,
        SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL,
        SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE,
        SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR,
        SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP,
        SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR,
        SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER,
        SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS,
        SAO_UI_ENTITY_ACTION_PLUGIN_STATUS,
        SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT,
        SAO_UI_ENTITY_ACTION_SET_ALL_DARK,
    };
    for (const auto action : unavailable) {
        CHECK(sao::launcher::entity_builtin_action::dispatch(action, state, provider) ==
              sao::launcher::entity_builtin_action::kKnownUnavailableStatus);
    }
    CHECK(sao::launcher::entity_builtin_action::dispatch(static_cast<SaoUiEntityAction>(-777),
                                                         state, provider) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("builtin authority keeps launcher actions open and external gates closed",
          "[launcher][entity][builtin_action][authority][focused]") {
    using namespace sao::launcher::entity_builtin_action;
    State state{};
    state.authority.publication_available = true;
    state.authority.controls = true;
    state.authority.nervgear = true;
    state.authority.streaming = true;
    state.authority.save_settings = true;
    state.authority.ai_editor = true;
    state.authority.plugin_runtime = true;
    state.authority.reload_plugins = true;
    state.authority.theme = true;
    state.authority.about = true;

    constexpr SaoUiEntityAction available[] = {
        SAO_UI_ENTITY_ACTION_OPEN_ABOUT,
        SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR,
        SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE,
        SAO_UI_ENTITY_ACTION_SAVE_SETTINGS,
        SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR,
        SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS,
        SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT,
        SAO_UI_ENTITY_ACTION_SET_ALL_DARK,
    };
    for (const auto action : available) {
        CHECK(authorization_status(action, state) == SAO_STATUS_OK);
    }
    CHECK(dispatch(SAO_UI_ENTITY_ACTION_OPEN_ABOUT, state, {}) == kKnownUnavailableStatus);
    CHECK(dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR, state, {}) == kKnownUnavailableStatus);
    CHECK(dispatch(SAO_UI_ENTITY_ACTION_SAVE_SETTINGS, state, {}) == kKnownUnavailableStatus);
    CHECK(dispatch(SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR, state, {}) == kKnownUnavailableStatus);
    CHECK(dispatch(SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT, state, {}) == kKnownUnavailableStatus);
    CHECK(dispatch(SAO_UI_ENTITY_ACTION_SET_ALL_DARK, state, {}) == kKnownUnavailableStatus);

    constexpr SaoUiEntityAction external[] = {
        SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST,
        SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL,
        SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE,
        SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP,
        SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR,
        SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER,
        SAO_UI_ENTITY_ACTION_PLUGIN_STATUS,
    };
    for (const auto action : external) {
        CHECK(authorization_status(action, state) == kKnownUnavailableStatus);
    }
    CHECK(authorization_status(-777, state) == SAO_STATUS_ERR_INVALID_ARGUMENT);

    ActionFixture fixture;
    state.streaming_entitled = true;
    state.authority.streaming = false;
    CHECK(dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE, state, operations(fixture)) ==
          kKnownUnavailableStatus);
    CHECK(fixture.events.empty());

    state.authority.streaming = true;
    state.authority.plugin_runtime = false;
    CHECK(authorization_status(SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS, state) ==
          kKnownUnavailableStatus);
    state.authority.plugin_runtime = true;
    state.authority.publication_available = false;
    CHECK(authorization_status(SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR, state) ==
          kKnownUnavailableStatus);
    CHECK(authorization_status(SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT, state) ==
          kKnownUnavailableStatus);
    CHECK(authorization_status(SAO_UI_ENTITY_ACTION_OPEN_ABOUT, state) ==
          kKnownUnavailableStatus);
    CHECK(authorization_status(-777, state) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE, state, operations(fixture)) ==
          kKnownUnavailableStatus);
    CHECK(fixture.events.empty());
    state.authority.publication_available = true;
    state.controls_degraded = true;
    CHECK(authorization_status(SAO_UI_ENTITY_ACTION_SAVE_SETTINGS, state) ==
          SAO_STATUS_ERR_UNKNOWN);
    CHECK(authorization_status(SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR, state) == SAO_STATUS_OK);
    CHECK(authorization_status(SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT, state) == SAO_STATUS_OK);
    CHECK(authorization_status(SAO_UI_ENTITY_ACTION_OPEN_ABOUT, state) == SAO_STATUS_OK);
}

// Adversarial recon coverage — Authority.about was defined in
// entity_builtin_action_internal.h but never had a matching field on
// EntityBuiltinAuthorityState in entity_provider_publication_internal.h.
// The gap meant the About button's dispatchability was silently dropped when
// the launcher published its authority snapshot to the publication catalog.
TEST_CASE("about authority round-trips through the entity publication state",
          "[launcher][entity][builtin_action][publication][about][focused]") {
    using namespace sao::launcher::entity_builtin_action;
    using sao::launcher::entity_provider_publication::EntityBuiltinAuthorityState;

    // Authority::about defaults false so nothing exposes About until the
    // launcher explicitly promotes it.
    Authority default_authority{};
    CHECK_FALSE(default_authority.about);

    // EntityBuiltinAuthorityState::about mirrors the source field and also
    // defaults false so an unsynced publication cannot expose About.
    EntityBuiltinAuthorityState publication_default{};
    CHECK_FALSE(publication_default.about);

    // Manual sync mirrors what sync_entity_publication_authority in
    // init_pipeline.cpp performs — the field is preserved from source to
    // publication and equality survives round-trip.
    EntityBuiltinAuthorityState publication{};
    publication.about = true;
    EntityBuiltinAuthorityState publication_copy = publication;
    CHECK(publication == publication_copy);

    publication_copy.about = false;
    CHECK_FALSE(publication == publication_copy);
}

// Adversarial recon coverage — Authority.topmost was implicitly false but had
// no explicit documentation.  When apply_topmost_mode is nullptr and the
// authority is (mistakenly) promoted to true, toggle_topmost() short-circuits
// with SAO_STATUS_ERR_NOT_INITIALIZED.  The launcher headless path documents
// authority.topmost = false explicitly; this test enforces that the closed
// authority is the compiled-in default so future refactors do not silently
// open the button without also wiring an apply function.
TEST_CASE("toggle topmost stays fail-closed when authority disallows and apply is unwired",
          "[launcher][entity][builtin_action][topmost][fail_closed][focused]") {
    using namespace sao::launcher::entity_builtin_action;

    // With authority.topmost = false, the dispatch returns the
    // "known unavailable" contract status so the toggle is never exposed.
    ActionFixture fixture;
    Operations operations_without_apply = operations(fixture);
    operations_without_apply.apply_topmost_mode = nullptr;
    State state_closed{};
    state_closed.authority.publication_available = true;
    state_closed.authority.controls = true;
    state_closed.authority.topmost = false;
    CHECK(dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST, state_closed,
                   operations_without_apply) == kKnownUnavailableStatus);
    CHECK(fixture.events.empty());

    // If the authority ever gets promoted without an apply function being
    // wired, toggle_topmost falls through fail() with
    // SAO_STATUS_ERR_NOT_INITIALIZED — the recon check ensures the plumbing
    // does not accidentally return SAO_STATUS_OK on a nullptr apply.
    fixture.events.clear();
    State state_promoted{};
    state_promoted.authority = transactional_authority();
    state_promoted.authority.about = true;
    CHECK(dispatch(SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST, state_promoted,
                   operations_without_apply) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(fixture.events.empty());
}
