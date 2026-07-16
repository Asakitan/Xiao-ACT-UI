// SAO Auto — Wave 4 input router tests (G3.11 gate).
//
// Focus areas (memory [快捷键架构]):
//   * mouse event dispatch honours the focus stack top
//   * push/pop focus maintains an ordered stack
//   * hotkey subset matcher — F5 fires for {F5}; Ctrl+F5 fires for
//     {Ctrl+F5}; when both are registered, plain F5 hits the plain
//     binding and Ctrl+F5 hits the ctrl-variant (most specific wins)
//   * modal barrier blocks route to targets not on the modal panel

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/input_router.h"

// Test-only introspection surface from input_router.cpp.
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_input_router_hotkey_count(
    sao_ui_input_router_deep_handle_t handle);
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_input_router_focus_depth(
    sao_ui_input_router_deep_handle_t handle);
extern "C" SAO_UI_API sao_ui_widget_handle_t SAO_UI_CALL
sao_ui_input_router_last_route_target(
    sao_ui_input_router_deep_handle_t handle);
extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_input_router_has_modal_barrier(
    sao_ui_input_router_deep_handle_t handle);

// Public API declared on the header banner but implemented in the
// wave 4 slice — surface it here so the linker resolves the symbol.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_input_router_match_hotkey(
    sao_ui_input_router_deep_handle_t handle,
    const SaoUiInputEvent* event,
    sao_ui_hotkey_binding_t* out_binding);

namespace {

// Fake widget/panel pointers — the router only inspects identity.
inline sao_ui_widget_handle_t fake_widget(uintptr_t n) {
    return reinterpret_cast<sao_ui_widget_handle_t>(0x1000u + n * 8u);
}
inline sao_ui_panel_handle_t fake_panel(uintptr_t n) {
    return reinterpret_cast<sao_ui_panel_handle_t>(0x2000u + n * 8u);
}

SaoUiInputEvent key_down_event(uint32_t vk, uint32_t modifiers) {
    SaoUiInputEvent e{};
    e.kind        = SAO_UI_INPUT_KEY_DOWN;
    e.virtual_key = vk;
    e.modifiers   = modifiers;
    return e;
}

SaoUiInputEvent mouse_move_event(int32_t x, int32_t y) {
    SaoUiInputEvent e{};
    e.kind        = SAO_UI_INPUT_MOUSE_MOVE;
    e.screen_x_px = x;
    e.screen_y_px = y;
    return e;
}

struct HotkeyFireLog {
    std::atomic<int> hits{0};
    std::string      last_id;
};

void SAO_UI_CALL hotkey_cb(const char* binding_id,
                            const SaoUiInputEvent*,
                            void* user_data) {
    auto* log = reinterpret_cast<HotkeyFireLog*>(user_data);
    log->hits.fetch_add(1);
    if (binding_id != nullptr) log->last_id = binding_id;
}

}  // namespace

TEST_CASE("router_dispatch_mouse_to_focus_target",
          "[ui][input_router][wave4]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router)
            == SAO_STATUS_OK);
    REQUIRE(router != nullptr);

    const auto widget_a = fake_widget(1);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, widget_a)
            == SAO_STATUS_OK);

    const auto move = mouse_move_event(120, 240);
    bool consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &move, &consumed)
            == SAO_STATUS_OK);
    REQUIRE(consumed == true);
    REQUIRE(sao_ui_input_router_last_route_target(router) == widget_a);

    // With no focus target, route_event still succeeds but reports
    // no consumer.
    sao_ui_input_router_deep_handle_t empty = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &empty)
            == SAO_STATUS_OK);
    bool empty_consumed = true;
    REQUIRE(sao_ui_input_router_route_event(empty, &move, &empty_consumed)
            == SAO_STATUS_OK);
    REQUIRE(empty_consumed == false);

    sao_ui_input_router_deep_destroy(empty);
    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router_push_pop_focus_maintains_stack",
          "[ui][input_router][wave4]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router)
            == SAO_STATUS_OK);

    REQUIRE(sao_ui_input_router_focus_depth(router) == 0);

    REQUIRE(sao_ui_input_router_set_focus_widget(router, fake_widget(1))
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_depth(router) == 1);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, fake_widget(2))
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_depth(router) == 2);

    sao_ui_widget_handle_t top_widget = nullptr;
    sao_ui_panel_handle_t  top_panel  = nullptr;
    REQUIRE(sao_ui_input_router_get_focus(router, &top_widget, &top_panel)
            == SAO_STATUS_OK);
    REQUIRE(top_widget == fake_widget(2));

    // Push modal barrier — depth grows and barrier flag flips.
    REQUIRE(sao_ui_input_router_has_modal_barrier(router) == false);
    REQUIRE(sao_ui_input_router_push_modal(router, fake_panel(9))
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_depth(router) == 3);
    REQUIRE(sao_ui_input_router_has_modal_barrier(router) == true);
    REQUIRE(sao_ui_input_router_pop_modal(router) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_depth(router) == 2);
    REQUIRE(sao_ui_input_router_has_modal_barrier(router) == false);

    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router_hotkey_exact_match", "[ui][input_router][wave4]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router)
            == SAO_STATUS_OK);

    HotkeyFireLog log;

    // VK_S = 'S' = 0x53; Ctrl+S binding.
    SaoUiHotkeyBindingSpec spec{};
    spec.binding_id_utf8 = "save";
    spec.virtual_key     = 0x53;
    spec.modifiers       = SAO_UI_MOD_CTRL_BIT;
    spec.scope           = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    sao_ui_hotkey_binding_t handle = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(
                router, "core", &spec, &hotkey_cb, &log, &handle)
            == SAO_STATUS_OK);
    REQUIRE(handle != 0);
    REQUIRE(sao_ui_input_router_hotkey_count(router) == 1);

    // Ctrl+S press hits.
    auto ev = key_down_event(0x53, SAO_UI_MOD_CTRL_BIT);
    sao_ui_hotkey_binding_t hit = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(router, &ev, &hit)
            == SAO_STATUS_OK);
    REQUIRE(hit == handle);
    REQUIRE(log.hits.load() == 1);
    REQUIRE(log.last_id == "save");

    // Plain S press misses (strict-match, not subset in the other
    // direction: observed lacks CTRL, binding demands it).
    ev = key_down_event(0x53, SAO_UI_MOD_NONE);
    hit = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(router, &ev, &hit)
            == SAO_STATUS_ERR_NOT_FOUND);
    REQUIRE(hit == 0);
    REQUIRE(log.hits.load() == 1);

    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router_hotkey_subset_match", "[ui][input_router][wave4]") {
    // Ctrl+F5 and plain F5 both registered.  A plain F5 press should
    // fire the plain binding only — subset matching still respects
    // exact-modifier semantics for the observed side.
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router)
            == SAO_STATUS_OK);

    HotkeyFireLog log_plain;
    HotkeyFireLog log_ctrl;

    SaoUiHotkeyBindingSpec plain{};
    plain.binding_id_utf8 = "f5_plain";
    plain.virtual_key     = 0x74;  // VK_F5
    plain.modifiers       = SAO_UI_MOD_NONE;
    plain.scope           = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    SaoUiHotkeyBindingSpec ctrl{};
    ctrl.binding_id_utf8 = "f5_ctrl";
    ctrl.virtual_key     = 0x74;
    ctrl.modifiers       = SAO_UI_MOD_CTRL_BIT;
    ctrl.scope           = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    sao_ui_hotkey_binding_t plain_handle = 0;
    sao_ui_hotkey_binding_t ctrl_handle = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(
                router, "core", &plain, &hotkey_cb, &log_plain,
                &plain_handle) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_register_hotkey(
                router, "core", &ctrl, &hotkey_cb, &log_ctrl,
                &ctrl_handle) == SAO_STATUS_OK);

    // Plain F5 press.
    auto ev = key_down_event(0x74, SAO_UI_MOD_NONE);
    sao_ui_hotkey_binding_t hit = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(router, &ev, &hit)
            == SAO_STATUS_OK);
    REQUIRE(hit == plain_handle);
    REQUIRE(log_plain.hits.load() == 1);
    REQUIRE(log_ctrl.hits.load() == 0);

    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router_hotkey_subset_match_most_specific",
          "[ui][input_router][wave4]") {
    // Both plain F5 and Ctrl+F5 registered.  Ctrl+F5 press must hit
    // the ctrl-variant, not the plain one.  This is the exact
    // scenario memory [快捷键架构] flags as "modifier reality +
    // most-specific-wins".
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router)
            == SAO_STATUS_OK);

    HotkeyFireLog log_plain;
    HotkeyFireLog log_ctrl;

    SaoUiHotkeyBindingSpec plain{};
    plain.binding_id_utf8 = "f5_plain";
    plain.virtual_key     = 0x74;
    plain.modifiers       = SAO_UI_MOD_NONE;
    plain.scope           = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    SaoUiHotkeyBindingSpec ctrl{};
    ctrl.binding_id_utf8 = "f5_ctrl";
    ctrl.virtual_key     = 0x74;
    ctrl.modifiers       = SAO_UI_MOD_CTRL_BIT;
    ctrl.scope           = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    sao_ui_hotkey_binding_t plain_handle = 0;
    sao_ui_hotkey_binding_t ctrl_handle = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(
                router, "core", &plain, &hotkey_cb, &log_plain,
                &plain_handle) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_register_hotkey(
                router, "core", &ctrl, &hotkey_cb, &log_ctrl,
                &ctrl_handle) == SAO_STATUS_OK);

    // Ctrl+F5 press — most specific wins.
    auto ev = key_down_event(0x74, SAO_UI_MOD_CTRL_BIT);
    sao_ui_hotkey_binding_t hit = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(router, &ev, &hit)
            == SAO_STATUS_OK);
    REQUIRE(hit == ctrl_handle);
    REQUIRE(log_ctrl.hits.load() == 1);
    REQUIRE(log_plain.hits.load() == 0);

    // Now flip registration order and confirm winner is still the
    // ctrl variant (order does NOT beat specificity).
    sao_ui_input_router_deep_handle_t flipped = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &flipped)
            == SAO_STATUS_OK);
    HotkeyFireLog log_plain2;
    HotkeyFireLog log_ctrl2;
    sao_ui_hotkey_binding_t c2 = 0;
    sao_ui_hotkey_binding_t p2 = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(
                flipped, "core", &ctrl, &hotkey_cb, &log_ctrl2, &c2)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_register_hotkey(
                flipped, "core", &plain, &hotkey_cb, &log_plain2, &p2)
            == SAO_STATUS_OK);
    ev = key_down_event(0x74, SAO_UI_MOD_CTRL_BIT);
    hit = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(flipped, &ev, &hit)
            == SAO_STATUS_OK);
    REQUIRE(hit == c2);
    REQUIRE(log_ctrl2.hits.load() == 1);

    sao_ui_input_router_deep_destroy(flipped);
    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router_hotkey_unregister", "[ui][input_router][wave4]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router)
            == SAO_STATUS_OK);

    SaoUiHotkeyBindingSpec spec_a{};
    spec_a.binding_id_utf8 = "alpha";
    spec_a.virtual_key     = 0x41;   // 'A'
    spec_a.modifiers       = SAO_UI_MOD_ALT_BIT;
    spec_a.scope           = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    SaoUiHotkeyBindingSpec spec_b{};
    spec_b.binding_id_utf8 = "beta";
    spec_b.virtual_key     = 0x42;   // 'B'
    spec_b.modifiers       = SAO_UI_MOD_ALT_BIT;
    spec_b.scope           = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    HotkeyFireLog log_a;
    HotkeyFireLog log_b;
    sao_ui_hotkey_binding_t ha = 0;
    sao_ui_hotkey_binding_t hb = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(
                router, "plugin_x", &spec_a, &hotkey_cb, &log_a, &ha)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_register_hotkey(
                router, "plugin_x", &spec_b, &hotkey_cb, &log_b, &hb)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_hotkey_count(router) == 2);

    // Unregister a single binding.
    REQUIRE(sao_ui_input_router_unregister_hotkey(router, ha)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_hotkey_count(router) == 1);
    REQUIRE(sao_ui_input_router_unregister_hotkey(router, ha)
            == SAO_STATUS_ERR_NOT_FOUND);

    // Bulk unregister by plugin id.
    REQUIRE(sao_ui_input_router_unregister_plugin_hotkeys(router, "plugin_x")
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_hotkey_count(router) == 0);

    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router_modal_barrier_blocks_others",
          "[ui][input_router][wave4]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router)
            == SAO_STATUS_OK);

    // Outer focus target.
    const auto outer_widget = fake_widget(1);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, outer_widget)
            == SAO_STATUS_OK);

    // Sanity — event routes to outer.
    auto ev = mouse_move_event(50, 50);
    bool consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &ev, &consumed)
            == SAO_STATUS_OK);
    REQUIRE(consumed == true);

    // Push modal barrier.  Now the focus stack top is the modal
    // entry (widget == nullptr) — an event to the outer widget must
    // be blocked.
    REQUIRE(sao_ui_input_router_push_modal(router, fake_panel(9))
            == SAO_STATUS_OK);
    consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &ev, &consumed)
            == SAO_STATUS_OK);
    // The modal barrier consumes without dispatching downstream.
    REQUIRE(consumed == true);

    // If we set focus on a widget above the modal, the event routes.
    const auto modal_widget = fake_widget(2);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, modal_widget)
            == SAO_STATUS_OK);
    consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &ev, &consumed)
            == SAO_STATUS_OK);
    REQUIRE(consumed == true);

    REQUIRE(sao_ui_input_router_pop_modal(router) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_has_modal_barrier(router) == false);

    sao_ui_input_router_deep_destroy(router);
}
