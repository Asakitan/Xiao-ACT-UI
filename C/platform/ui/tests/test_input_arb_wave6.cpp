// SAO Auto — Wave 6 input arbitration tests.
//
// Coverage:
//   * defaults: an unconfigured VK admits every source (SHARED)
//   * reservation blocks non-reserved plugins
//   * conflict detection flags VKs observed by two plugins
//   * policy registration overrides default SHARED
//   * user source is always allowed, regardless of policy

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/input_arbitration.h"

TEST_CASE("input_arb_default_shared",
          "[ui][input_arb][wave6]") {
    sao_ui_input_arb_handle_t arb = nullptr;
    REQUIRE(sao_ui_input_arb_create(nullptr, &arb) == SAO_STATUS_OK);
    REQUIRE(arb != nullptr);

    // A never-seen VK admits every plugin by default.
    bool allowed = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "plugin_a", &allowed)
            == SAO_STATUS_OK);
    REQUIRE(allowed == true);

    allowed = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "plugin_b", &allowed)
            == SAO_STATUS_OK);
    REQUIRE(allowed == true);

    sao_ui_input_arb_destroy(arb);
}

TEST_CASE("input_arb_reserve_blocks_others",
          "[ui][input_arb][wave6]") {
    sao_ui_input_arb_handle_t arb = nullptr;
    REQUIRE(sao_ui_input_arb_create(nullptr, &arb) == SAO_STATUS_OK);

    REQUIRE(sao_ui_input_arb_reserve(arb, 0x74, "auto_key_engine")
            == SAO_STATUS_OK);

    bool allowed_owner = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "auto_key_engine",
                                    &allowed_owner)
            == SAO_STATUS_OK);
    REQUIRE(allowed_owner == true);

    bool allowed_other = true;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "hide_seek", &allowed_other)
            == SAO_STATUS_OK);
    REQUIRE(allowed_other == false);

    // User source (physical input) is never blocked.
    bool allowed_user = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "user", &allowed_user)
            == SAO_STATUS_OK);
    REQUIRE(allowed_user == true);

    // Releasing the reservation restores the SHARED default.
    REQUIRE(sao_ui_input_arb_reserve(arb, 0x74, nullptr)
            == SAO_STATUS_OK);
    allowed_other = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "hide_seek", &allowed_other)
            == SAO_STATUS_OK);
    REQUIRE(allowed_other == true);

    sao_ui_input_arb_destroy(arb);
}

TEST_CASE("input_arb_conflict_detection",
          "[ui][input_arb][wave6]") {
    sao_ui_input_arb_handle_t arb = nullptr;
    REQUIRE(sao_ui_input_arb_create(nullptr, &arb) == SAO_STATUS_OK);

    // Two plugins observe the same VK.
    bool allowed = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "plugin_a", &allowed)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "plugin_b", &allowed)
            == SAO_STATUS_OK);
    // "user" doesn't count as a plugin contention participant.
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "user", &allowed)
            == SAO_STATUS_OK);
    // One plugin on a different VK — no conflict.
    REQUIRE(sao_ui_input_arb_check(arb, 0x75, "plugin_a", &allowed)
            == SAO_STATUS_OK);

    // Query mode.
    size_t count = 0;
    REQUIRE(sao_ui_input_arb_get_conflicts(arb, nullptr, 0, &count)
            == SAO_STATUS_OK);
    REQUIRE(count == 1);

    // Actual fetch.
    std::vector<SaoUiInputArbConflict> conflicts(4);
    size_t written = 0;
    REQUIRE(sao_ui_input_arb_get_conflicts(arb, conflicts.data(),
                                            conflicts.size(),
                                            &written)
            == SAO_STATUS_OK);
    REQUIRE(written == 1);
    REQUIRE(conflicts[0].virtual_key == 0x74);
    REQUIRE(conflicts[0].observed_source_count == 2);

    sao_ui_input_arb_destroy(arb);
}

TEST_CASE("input_arb_policy_registration",
          "[ui][input_arb][wave6]") {
    sao_ui_input_arb_handle_t arb = nullptr;
    REQUIRE(sao_ui_input_arb_create(nullptr, &arb) == SAO_STATUS_OK);

    // GAME_ONLY blocks plugins but admits user.
    REQUIRE(sao_ui_input_arb_register_policy(
                arb, 0x74, SAO_UI_AUTO_KEY_POLICY_GAME_ONLY)
            == SAO_STATUS_OK);
    bool allowed_plugin = true;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "plugin_a", &allowed_plugin)
            == SAO_STATUS_OK);
    REQUIRE(allowed_plugin == false);
    bool allowed_user = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "user", &allowed_user)
            == SAO_STATUS_OK);
    REQUIRE(allowed_user == true);

    // Overwrite with SHARED — plugin allowed again.
    REQUIRE(sao_ui_input_arb_register_policy(
                arb, 0x74, SAO_UI_AUTO_KEY_POLICY_SHARED)
            == SAO_STATUS_OK);
    allowed_plugin = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "plugin_a", &allowed_plugin)
            == SAO_STATUS_OK);
    REQUIRE(allowed_plugin == true);

    // Bogus policy value is rejected without touching state.
    REQUIRE(sao_ui_input_arb_register_policy(arb, 0x74, 999)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Create-time config seeds the policy table.
    sao_ui_input_arb_handle_t seeded = nullptr;
    SaoUiInputArbConfigEntry entries[] = {
        { 0x80, SAO_UI_AUTO_KEY_POLICY_BLOCKED },
    };
    SaoUiInputArbConfig cfg{ entries, 1 };
    REQUIRE(sao_ui_input_arb_create(&cfg, &seeded) == SAO_STATUS_OK);
    bool blocked = true;
    REQUIRE(sao_ui_input_arb_check(seeded, 0x80, "plugin_c", &blocked)
            == SAO_STATUS_OK);
    REQUIRE(blocked == false);
    sao_ui_input_arb_destroy(seeded);

    sao_ui_input_arb_destroy(arb);
}

TEST_CASE("input_arb_check_from_user_always_ok",
          "[ui][input_arb][wave6]") {
    sao_ui_input_arb_handle_t arb = nullptr;
    REQUIRE(sao_ui_input_arb_create(nullptr, &arb) == SAO_STATUS_OK);

    // Reserved for a plugin — user still allowed.
    REQUIRE(sao_ui_input_arb_reserve(arb, 0x74, "some_plugin")
            == SAO_STATUS_OK);
    bool ok = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "user", &ok)
            == SAO_STATUS_OK);
    REQUIRE(ok == true);

    // BLOCKED policy — user still allowed.
    REQUIRE(sao_ui_input_arb_reserve(arb, 0x74, nullptr)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_arb_register_policy(
                arb, 0x74, SAO_UI_AUTO_KEY_POLICY_BLOCKED)
            == SAO_STATUS_OK);
    ok = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "user", &ok)
            == SAO_STATUS_OK);
    REQUIRE(ok == true);

    // GAME_ONLY — user allowed.
    REQUIRE(sao_ui_input_arb_register_policy(
                arb, 0x74, SAO_UI_AUTO_KEY_POLICY_GAME_ONLY)
            == SAO_STATUS_OK);
    ok = false;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "user", &ok)
            == SAO_STATUS_OK);
    REQUIRE(ok == true);

    // PLUGIN_ONLY — user *blocked* (this policy explicitly excludes
    // physical input).  Callers who really want to also block user in
    // GAME_ONLY windows are responsible for the LL-hook layer.
    REQUIRE(sao_ui_input_arb_register_policy(
                arb, 0x74, SAO_UI_AUTO_KEY_POLICY_PLUGIN_ONLY)
            == SAO_STATUS_OK);
    ok = true;
    REQUIRE(sao_ui_input_arb_check(arb, 0x74, "user", &ok)
            == SAO_STATUS_OK);
    REQUIRE(ok == false);

    sao_ui_input_arb_destroy(arb);
}
