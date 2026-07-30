// SAO Auto — typed input-router focus and hotkey-matching tests.
//
// Focus areas (memory [快捷键架构]):
//   * mouse event dispatch honours the focus stack top
//   * push/pop focus maintains an ordered stack
//   * hotkey subset matcher — F5 fires for {F5}; Ctrl+F5 fires for
//     {Ctrl+F5}; when both are registered, plain F5 hits the plain
//     binding and Ctrl+F5 hits the ctrl-variant (most specific wins)
//   * modal barrier blocks route to targets not on the modal panel

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/compositor.h"
#include "sao/ui/input_router.h"
#include "sao/ui/widget_input.h"
#include "sao/ui/widget_kit.h"

// Test-only introspection surface from input_router.cpp.
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_input_router_hotkey_count(sao_ui_input_router_deep_handle_t handle);
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_input_router_focus_depth(sao_ui_input_router_deep_handle_t handle);
extern "C" SAO_UI_API sao_ui_widget_handle_t SAO_UI_CALL
sao_ui_input_router_last_route_target(sao_ui_input_router_deep_handle_t handle);
extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_input_router_has_modal_barrier(sao_ui_input_router_deep_handle_t handle);
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_input_router_test_set_create_failure_point(int32_t point);
extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_input_router_test_active_count();
extern "C" SAO_UI_API uint64_t SAO_UI_CALL
sao_ui_compositor_test_input_writer_revision(sao_ui_compositor_handle_t compositor);

// Public API declared on the header banner but implemented in the
// implementation-only surface — declare it here so the linker resolves the symbol.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_match_hotkey(
    sao_ui_input_router_deep_handle_t handle, const SaoUiInputEvent* event,
    sao_ui_hotkey_binding_t* out_binding);

namespace {

inline sao_ui_panel_handle_t fake_panel(uintptr_t n) {
    return reinterpret_cast<sao_ui_panel_handle_t>(0x2000u + n * 8u);
}

SaoUiInputEvent key_down_event(uint32_t vk, uint32_t modifiers) {
    SaoUiInputEvent e{};
    e.kind = SAO_UI_INPUT_KEY_DOWN;
    e.virtual_key = vk;
    e.modifiers = modifiers;
    return e;
}

SaoUiInputEvent mouse_move_event(int32_t x, int32_t y) {
    SaoUiInputEvent e{};
    e.kind = SAO_UI_INPUT_MOUSE_MOVE;
    e.screen_x_px = x;
    e.screen_y_px = y;
    return e;
}

struct HotkeyFireLog {
    std::atomic<int> hits{0};
    std::string last_id;
};

struct FocusEventLog {
    int gained{0};
    int lost{0};
};

struct FocusMutation {
    sao_ui_input_router_deep_handle_t router{nullptr};
    sao_ui_widget_handle_t widget{nullptr};
    sao_status_t status{SAO_STATUS_ERR_UNKNOWN};
};

struct RouterDestroyContext {
    sao_ui_input_router_deep_handle_t router{nullptr};
    int callbacks{0};
};

struct SelfUnregisterHotkey {
    sao_ui_input_router_deep_handle_t router{};
    sao_ui_hotkey_binding_t binding{};
    sao_status_t status{SAO_STATUS_ERR_UNKNOWN};
    size_t calls{};
};

struct BlockingHotkey {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    std::atomic_int calls{0};
};

struct ReentrantDrainHotkey {
    sao_ui_input_router_deep_handle_t router = nullptr;
    sao_ui_hotkey_binding_t binding = 0;
    std::mutex mutex;
    std::condition_variable condition;
    bool first_entered = false;
    bool second_entered = false;
    bool release_first = false;
    std::atomic_int calls{0};
    sao_status_t unregister_status{SAO_STATUS_ERR_UNKNOWN};
};

struct LayerRouteLog {
    int cursor_calls{};
};

class LegacyInputGateOff {
  public:
    LegacyInputGateOff() {
        const char* current = std::getenv("SAO_UI_LEGACY_TK_INPUT");
        if (current != nullptr) {
            had_previous_ = true;
            previous_ = current;
        }
#if defined(_WIN32)
        (void)_putenv_s("SAO_UI_LEGACY_TK_INPUT", "");
#else
        (void)unsetenv("SAO_UI_LEGACY_TK_INPUT");
#endif
    }

    ~LegacyInputGateOff() {
#if defined(_WIN32)
        (void)_putenv_s("SAO_UI_LEGACY_TK_INPUT", had_previous_ ? previous_.c_str() : "");
#else
        if (had_previous_)
            (void)setenv("SAO_UI_LEGACY_TK_INPUT", previous_.c_str(), 1);
        else
            (void)unsetenv("SAO_UI_LEGACY_TK_INPUT");
#endif
    }

  private:
    bool had_previous_{};
    std::string previous_;
};

void SAO_UI_CALL hotkey_cb(const char* binding_id, const SaoUiInputEvent*, void* user_data) {
    auto* log = reinterpret_cast<HotkeyFireLog*>(user_data);
    log->hits.fetch_add(1);
    if (binding_id != nullptr)
        log->last_id = binding_id;
}

void SAO_UI_CALL self_unregister_hotkey_cb(const char*, const SaoUiInputEvent*, void* user_data) {
    auto* state = static_cast<SelfUnregisterHotkey*>(user_data);
    ++state->calls;
    state->status = sao_ui_input_router_unregister_hotkey(state->router, state->binding);
}

void SAO_UI_CALL blocking_hotkey_cb(const char*, const SaoUiInputEvent*, void* user_data) {
    auto* state = static_cast<BlockingHotkey*>(user_data);
    state->calls.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(state->mutex);
    state->entered = true;
    state->condition.notify_all();
    state->condition.wait(lock, [state] { return state->release; });
}

void SAO_UI_CALL reentrant_drain_hotkey_cb(const char*, const SaoUiInputEvent*, void* user_data) {
    auto* state = static_cast<ReentrantDrainHotkey*>(user_data);
    const int call = state->calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call == 1) {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->first_entered = true;
        state->condition.notify_all();
        state->condition.wait(lock, [state] { return state->release_first; });
    } else if (call == 2) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->second_entered = true;
        }
        state->condition.notify_all();
        state->unregister_status =
            sao_ui_input_router_unregister_hotkey(state->router, state->binding);
    }
}

void SAO_UI_CALL focus_event_cb(int32_t event_type, const uint8_t*, size_t, void* user_data) {
    auto* log = static_cast<FocusEventLog*>(user_data);
    if (event_type == SAO_UI_EVT_FOCUS_GAINED)
        log->gained++;
    if (event_type == SAO_UI_EVT_FOCUS_LOST)
        log->lost++;
}

void SAO_UI_CALL nested_focus_cb(int32_t event_type, const uint8_t*, size_t, void* user_data) {
    if (event_type != SAO_UI_EVT_FOCUS_LOST)
        return;
    auto* mutation = static_cast<FocusMutation*>(user_data);
    mutation->status = sao_ui_input_router_set_focus_widget(mutation->router, mutation->widget);
}

void SAO_UI_CALL destroy_peer_cb(int32_t event_type, const uint8_t*, size_t, void* user_data) {
    if (event_type == SAO_UI_EVT_FOCUS_LOST)
        sao_ui_widget_destroy(static_cast<FocusMutation*>(user_data)->widget);
}

void SAO_UI_CALL destroy_router_hover_cb(sao_ui_widget_handle_t, sao_ui_widget_handle_t,
                                         void* user_data) {
    auto* context = static_cast<RouterDestroyContext*>(user_data);
    context->callbacks++;
    sao_ui_input_router_deep_destroy(context->router);
}

void SAO_UI_CALL throwing_hover_cb(sao_ui_widget_handle_t, sao_ui_widget_handle_t, void*) {
    throw std::runtime_error("router callback failure");
}

void SAO_UI_CALL layer_cursor_cb(float, float, void* user_data) {
    ++static_cast<LayerRouteLog*>(user_data)->cursor_calls;
}

SaoLayerConfig input_layer_config(const char* name, int32_t z_order) {
    SaoLayerConfig config{};
    config.struct_size = sizeof(config);
    config.name_utf8 = name;
    config.width = 16;
    config.height = 16;
    config.z_order = z_order;
    config.click_through = false;
    config.rect_hit = true;
    return config;
}

sao_ui_widget_handle_t create_focus_button(const char* text) {
    SaoUiButtonSpec spec{};
    spec.text_utf8 = text;
    spec.kind = SAO_UI_BTN_NORMAL;
    spec.radius_px = 4;
    spec.pad_x_px = 8;
    spec.pad_y_px = 4;
    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_button_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    return handle;
}

sao_ui_widget_handle_t create_disabled_checkbox() {
    SaoUiCheckboxSpec spec{};
    spec.label_utf8 = "disabled";
    spec.disabled = true;
    spec.font_size_px = 14;
    spec.box_size_px = 16;
    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_checkbox_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    return handle;
}

} // namespace

TEST_CASE("layer input router is the single state writer and honors z order",
          "[ui][input_router][single_writer][z_order]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    SaoLayerConfig lower_config = input_layer_config("router.z.lower", 10);
    SaoLayerConfig upper_config = input_layer_config("router.z.upper", 20);
    sao_ui_layer_handle_t lower = nullptr;
    sao_ui_layer_handle_t upper = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &lower_config, &lower) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_create(compositor, &upper_config, &upper) == SAO_STATUS_OK);
    LayerRouteLog lower_log;
    LayerRouteLog upper_log;
    REQUIRE(sao_ui_layer_set_input_callbacks(lower, &layer_cursor_cb, nullptr, nullptr, nullptr,
                                              &lower_log) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_input_callbacks(upper, &layer_cursor_cb, nullptr, nullptr, nullptr,
                                              &upper_log) == SAO_STATUS_OK);

    const uint64_t before = sao_ui_compositor_test_input_writer_revision(compositor);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, 0x0200, 4, 5, -1, 0) == SAO_STATUS_OK);
    CHECK(upper_log.cursor_calls == 1);
    CHECK(lower_log.cursor_calls == 0);
    const uint64_t after_upper = sao_ui_compositor_test_input_writer_revision(compositor);
    CHECK(after_upper > before);

    REQUIRE(sao_ui_layer_set_visible(upper, false) == SAO_STATUS_OK);
    CHECK(sao_ui_compositor_test_input_writer_revision(compositor) > after_upper);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, 0x0200, 4, 5, -1, 0) == SAO_STATUS_OK);
    CHECK(lower_log.cursor_calls == 1);

    sao_ui_layer_destroy(upper);
    sao_ui_layer_destroy(lower);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("legacy Tk input proxy gate defaults off",
          "[ui][input_router][legacy][tk_mirror][gate]") {
    LegacyInputGateOff gate;
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    SaoLayerConfig config = input_layer_config("router.legacy.off", 1);
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &config, &layer) == SAO_STATUS_OK);
    LayerRouteLog log;
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, &layer_cursor_cb, nullptr, nullptr, nullptr,
                                              &log) == SAO_STATUS_OK);
    CHECK(sao_ui_layer_enable_input_proxy(layer) == SAO_STATUS_ERR_NOT_IMPLEMENTED);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, 0x0200, 2, 3, -1, 0) == SAO_STATUS_OK);
    CHECK(log.cursor_calls == 1);
    sao_ui_layer_destroy(layer);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("router_dispatch_mouse_to_focus_target", "[ui][input_router][runtime]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    REQUIRE(router != nullptr);

    const auto widget_a = create_focus_button("route");
    REQUIRE(sao_ui_input_router_set_focus_widget(router, widget_a) == SAO_STATUS_OK);

    const auto move = mouse_move_event(120, 240);
    bool consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &move, &consumed) == SAO_STATUS_OK);
    REQUIRE(consumed == true);
    REQUIRE(sao_ui_input_router_last_route_target(router) == widget_a);

    // With no focus target, route_event still succeeds but reports
    // no consumer.
    sao_ui_input_router_deep_handle_t empty = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &empty) == SAO_STATUS_OK);
    bool empty_consumed = true;
    REQUIRE(sao_ui_input_router_route_event(empty, &move, &empty_consumed) == SAO_STATUS_OK);
    REQUIRE(empty_consumed == false);

    sao_ui_input_router_deep_destroy(empty);
    sao_ui_input_router_deep_destroy(router);
    sao_ui_widget_destroy(widget_a);
}

TEST_CASE("router_push_pop_focus_maintains_stack", "[ui][input_router][runtime]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);

    REQUIRE(sao_ui_input_router_focus_depth(router) == 0);

    const auto first = create_focus_button("first");
    const auto second = create_focus_button("second");
    REQUIRE(sao_ui_input_router_set_focus_widget(router, first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_depth(router) == 1);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, second) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_depth(router) == 2);

    sao_ui_widget_handle_t top_widget = nullptr;
    sao_ui_panel_handle_t top_panel = nullptr;
    REQUIRE(sao_ui_input_router_get_focus(router, &top_widget, &top_panel) == SAO_STATUS_OK);
    REQUIRE(top_widget == second);

    // Push modal barrier — depth grows and barrier flag flips.
    REQUIRE(sao_ui_input_router_has_modal_barrier(router) == false);
    REQUIRE(sao_ui_input_router_push_modal(router, fake_panel(9)) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_depth(router) == 3);
    REQUIRE(sao_ui_input_router_has_modal_barrier(router) == true);
    REQUIRE(sao_ui_input_router_pop_modal(router) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_depth(router) == 2);
    REQUIRE(sao_ui_input_router_has_modal_barrier(router) == false);

    sao_ui_input_router_deep_destroy(router);
    sao_ui_widget_destroy(second);
    sao_ui_widget_destroy(first);
}

TEST_CASE("router_hotkey_exact_match", "[ui][input_router][runtime]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);

    HotkeyFireLog log;

    // VK_S = 'S' = 0x53; Ctrl+S binding.
    SaoUiHotkeyBindingSpec spec{};
    spec.binding_id_utf8 = "save";
    spec.virtual_key = 0x53;
    spec.modifiers = SAO_UI_MOD_CTRL_BIT;
    spec.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    sao_ui_hotkey_binding_t handle = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &spec, &hotkey_cb, &log, &handle) ==
            SAO_STATUS_OK);
    REQUIRE(handle != 0);
    REQUIRE(sao_ui_input_router_hotkey_count(router) == 1);

    // Ctrl+S press hits.
    auto ev = key_down_event(0x53, SAO_UI_MOD_CTRL_BIT);
    sao_ui_hotkey_binding_t hit = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(router, &ev, &hit) == SAO_STATUS_OK);
    REQUIRE(hit == handle);
    REQUIRE(log.hits.load() == 1);
    REQUIRE(log.last_id == "save");

    // Plain S press misses (strict-match, not subset in the other
    // direction: observed lacks CTRL, binding demands it).
    ev = key_down_event(0x53, SAO_UI_MOD_NONE);
    hit = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(router, &ev, &hit) == SAO_STATUS_ERR_NOT_FOUND);
    REQUIRE(hit == 0);
    REQUIRE(log.hits.load() == 1);

    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router_hotkey_subset_match", "[ui][input_router][runtime]") {
    // Ctrl+F5 and plain F5 both registered.  A plain F5 press should
    // fire the plain binding only — subset matching still respects
    // exact-modifier semantics for the observed side.
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);

    HotkeyFireLog log_plain;
    HotkeyFireLog log_ctrl;

    SaoUiHotkeyBindingSpec plain{};
    plain.binding_id_utf8 = "f5_plain";
    plain.virtual_key = 0x74; // VK_F5
    plain.modifiers = SAO_UI_MOD_NONE;
    plain.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    SaoUiHotkeyBindingSpec ctrl{};
    ctrl.binding_id_utf8 = "f5_ctrl";
    ctrl.virtual_key = 0x74;
    ctrl.modifiers = SAO_UI_MOD_CTRL_BIT;
    ctrl.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    sao_ui_hotkey_binding_t plain_handle = 0;
    sao_ui_hotkey_binding_t ctrl_handle = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &plain, &hotkey_cb, &log_plain,
                                                &plain_handle) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &ctrl, &hotkey_cb, &log_ctrl,
                                                &ctrl_handle) == SAO_STATUS_OK);

    // Plain F5 press.
    auto ev = key_down_event(0x74, SAO_UI_MOD_NONE);
    sao_ui_hotkey_binding_t hit = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(router, &ev, &hit) == SAO_STATUS_OK);
    REQUIRE(hit == plain_handle);
    REQUIRE(log_plain.hits.load() == 1);
    REQUIRE(log_ctrl.hits.load() == 0);

    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router_hotkey_subset_match_most_specific", "[ui][input_router][runtime]") {
    // Both plain F5 and Ctrl+F5 registered.  Ctrl+F5 press must hit
    // the ctrl-variant, not the plain one.  This is the exact
    // scenario memory [快捷键架构] flags as "modifier reality +
    // most-specific-wins".
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);

    HotkeyFireLog log_plain;
    HotkeyFireLog log_ctrl;

    SaoUiHotkeyBindingSpec plain{};
    plain.binding_id_utf8 = "f5_plain";
    plain.virtual_key = 0x74;
    plain.modifiers = SAO_UI_MOD_NONE;
    plain.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    SaoUiHotkeyBindingSpec ctrl{};
    ctrl.binding_id_utf8 = "f5_ctrl";
    ctrl.virtual_key = 0x74;
    ctrl.modifiers = SAO_UI_MOD_CTRL_BIT;
    ctrl.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    sao_ui_hotkey_binding_t plain_handle = 0;
    sao_ui_hotkey_binding_t ctrl_handle = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &plain, &hotkey_cb, &log_plain,
                                                &plain_handle) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &ctrl, &hotkey_cb, &log_ctrl,
                                                &ctrl_handle) == SAO_STATUS_OK);

    // Ctrl+F5 press — most specific wins.
    auto ev = key_down_event(0x74, SAO_UI_MOD_CTRL_BIT);
    sao_ui_hotkey_binding_t hit = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(router, &ev, &hit) == SAO_STATUS_OK);
    REQUIRE(hit == ctrl_handle);
    REQUIRE(log_ctrl.hits.load() == 1);
    REQUIRE(log_plain.hits.load() == 0);

    // Now flip registration order and confirm winner is still the
    // ctrl variant (order does NOT beat specificity).
    sao_ui_input_router_deep_handle_t flipped = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &flipped) == SAO_STATUS_OK);
    HotkeyFireLog log_plain2;
    HotkeyFireLog log_ctrl2;
    sao_ui_hotkey_binding_t c2 = 0;
    sao_ui_hotkey_binding_t p2 = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(flipped, "core", &ctrl, &hotkey_cb, &log_ctrl2,
                                                &c2) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_register_hotkey(flipped, "core", &plain, &hotkey_cb, &log_plain2,
                                                &p2) == SAO_STATUS_OK);
    ev = key_down_event(0x74, SAO_UI_MOD_CTRL_BIT);
    hit = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(flipped, &ev, &hit) == SAO_STATUS_OK);
    REQUIRE(hit == c2);
    REQUIRE(log_ctrl2.hits.load() == 1);

    sao_ui_input_router_deep_destroy(flipped);
    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router_hotkey_unregister", "[ui][input_router][runtime]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);

    SaoUiHotkeyBindingSpec spec_a{};
    spec_a.binding_id_utf8 = "alpha";
    spec_a.virtual_key = 0x41; // 'A'
    spec_a.modifiers = SAO_UI_MOD_ALT_BIT;
    spec_a.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    SaoUiHotkeyBindingSpec spec_b{};
    spec_b.binding_id_utf8 = "beta";
    spec_b.virtual_key = 0x42; // 'B'
    spec_b.modifiers = SAO_UI_MOD_ALT_BIT;
    spec_b.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;

    HotkeyFireLog log_a;
    HotkeyFireLog log_b;
    sao_ui_hotkey_binding_t ha = 0;
    sao_ui_hotkey_binding_t hb = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(router, "plugin_x", &spec_a, &hotkey_cb, &log_a,
                                                &ha) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_register_hotkey(router, "plugin_x", &spec_b, &hotkey_cb, &log_b,
                                                &hb) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_hotkey_count(router) == 2);

    // Unregister a single binding.
    REQUIRE(sao_ui_input_router_unregister_hotkey(router, ha) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_hotkey_count(router) == 1);
    REQUIRE(sao_ui_input_router_unregister_hotkey(router, ha) == SAO_STATUS_ERR_NOT_FOUND);

    // Bulk unregister by plugin id.
    REQUIRE(sao_ui_input_router_unregister_plugin_hotkeys(router, "plugin_x") == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_hotkey_count(router) == 0);

    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router hotkey self unregister completes once and stops later routes",
          "[ui][input_router][hotkey][lifetime]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    SelfUnregisterHotkey state{};
    state.router = router;
    SaoUiHotkeyBindingSpec spec{};
    spec.binding_id_utf8 = "one-shot";
    spec.virtual_key = 0x75;
    spec.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;
    spec.prevent_default = true;
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &spec,
                                                &self_unregister_hotkey_cb, &state,
                                                &state.binding) == SAO_STATUS_OK);
    const auto event = key_down_event(0x75, SAO_UI_MOD_NONE);
    bool consumed = false;
    CHECK(sao_ui_input_router_route_event(router, &event, &consumed) == SAO_STATUS_OK);
    CHECK(consumed);
    CHECK(state.status == SAO_STATUS_OK);
    CHECK(state.calls == 1);
    consumed = true;
    CHECK(sao_ui_input_router_route_event(router, &event, &consumed) == SAO_STATUS_OK);
    CHECK_FALSE(consumed);
    CHECK(state.calls == 1);
    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router hotkey unregister drains another thread before owner release",
          "[ui][input_router][hotkey][lifetime][concurrency]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    BlockingHotkey state;
    SaoUiHotkeyBindingSpec spec{};
    spec.binding_id_utf8 = "drain";
    spec.virtual_key = 0x78;
    spec.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;
    sao_ui_hotkey_binding_t binding = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &spec, blocking_hotkey_cb,
                                                &state, &binding) == SAO_STATUS_OK);
    const auto event = key_down_event(0x78, SAO_UI_MOD_NONE);
    auto dispatch = std::async(std::launch::async, [&] {
        sao_ui_hotkey_binding_t matched = 0;
        return sao_ui_input_router_match_hotkey(router, &event, &matched);
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        REQUIRE(state.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&state] { return state.entered; }));
    }
    auto unregister = std::async(std::launch::async,
                                 [&] { return sao_ui_input_router_unregister_hotkey(router,
                                                                                     binding); });
    CHECK(unregister.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.release = true;
    }
    state.condition.notify_all();
    REQUIRE(dispatch.get() == SAO_STATUS_OK);
    REQUIRE(unregister.get() == SAO_STATUS_OK);
    CHECK(state.calls.load(std::memory_order_relaxed) == 1);
    sao_ui_hotkey_binding_t matched = 0;
    CHECK(sao_ui_input_router_match_hotkey(router, &event, &matched) ==
          SAO_STATUS_ERR_NOT_FOUND);
    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router hotkey self unregister drains only callbacks owned by other threads",
          "[ui][input_router][hotkey][lifetime][concurrency][reentry]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    ReentrantDrainHotkey state;
    state.router = router;
    SaoUiHotkeyBindingSpec spec{};
    spec.binding_id_utf8 = "self-drain";
    spec.virtual_key = 0x79;
    spec.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &spec,
                                                reentrant_drain_hotkey_cb, &state,
                                                &state.binding) == SAO_STATUS_OK);
    const auto event = key_down_event(0x79, SAO_UI_MOD_NONE);

    auto first_dispatch = std::async(std::launch::async, [&] {
        sao_ui_hotkey_binding_t matched = 0;
        return sao_ui_input_router_match_hotkey(router, &event, &matched);
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        REQUIRE(state.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&state] { return state.first_entered; }));
    }

    auto second_dispatch = std::async(std::launch::async, [&] {
        sao_ui_hotkey_binding_t matched = 0;
        return sao_ui_input_router_match_hotkey(router, &event, &matched);
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        REQUIRE(state.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&state] { return state.second_entered; }));
    }
    CHECK(second_dispatch.wait_for(std::chrono::milliseconds(50)) ==
          std::future_status::timeout);

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.release_first = true;
    }
    state.condition.notify_all();
    REQUIRE(first_dispatch.get() == SAO_STATUS_OK);
    REQUIRE(second_dispatch.get() == SAO_STATUS_OK);
    CHECK(state.unregister_status == SAO_STATUS_OK);
    CHECK(state.calls.load(std::memory_order_relaxed) == 2);
    sao_ui_hotkey_binding_t matched = 0;
    CHECK(sao_ui_input_router_match_hotkey(router, &event, &matched) ==
          SAO_STATUS_ERR_NOT_FOUND);
    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("router create rolls back a partially published handle",
          "[ui][input_router][create][rollback]") {
    const size_t baseline = sao_ui_input_router_test_active_count();
    sao_ui_input_router_test_set_create_failure_point(1);
    sao_ui_input_router_deep_handle_t router =
        reinterpret_cast<sao_ui_input_router_deep_handle_t>(uintptr_t{1});
    CHECK(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_ERR_UNKNOWN);
    sao_ui_input_router_test_set_create_failure_point(0);
    CHECK(router == nullptr);
    CHECK(sao_ui_input_router_test_active_count() == baseline);
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    sao_ui_input_router_deep_destroy(router);
    CHECK(sao_ui_input_router_test_active_count() == baseline);
}

TEST_CASE("router_modal_barrier_blocks_others", "[ui][input_router][runtime]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);

    // Outer focus target.
    const auto outer_widget = create_focus_button("outer");
    REQUIRE(sao_ui_input_router_set_focus_widget(router, outer_widget) == SAO_STATUS_OK);

    // Sanity — event routes to outer.
    auto ev = mouse_move_event(50, 50);
    bool consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &ev, &consumed) == SAO_STATUS_OK);
    REQUIRE(consumed == true);

    // Push modal barrier.  Now the focus stack top is the modal
    // entry (widget == nullptr) — an event to the outer widget must
    // be blocked.
    REQUIRE(sao_ui_input_router_push_modal(router, fake_panel(9)) == SAO_STATUS_OK);
    consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &ev, &consumed) == SAO_STATUS_OK);
    // The modal barrier consumes without dispatching downstream.
    REQUIRE(consumed == true);

    // If we set focus on a widget above the modal, the event routes.
    const auto modal_widget = create_focus_button("modal");
    REQUIRE(sao_ui_input_router_set_focus_widget(router, modal_widget) == SAO_STATUS_OK);
    consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &ev, &consumed) == SAO_STATUS_OK);
    REQUIRE(consumed == true);

    REQUIRE(sao_ui_input_router_pop_modal(router) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_has_modal_barrier(router) == false);

    sao_ui_input_router_deep_destroy(router);
    sao_ui_widget_destroy(modal_widget);
    sao_ui_widget_destroy(outer_widget);
}

TEST_CASE("portable_focus_next_wraps_both_directions_and_skips_disabled",
          "[ui][input_router][portable]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    const auto first = create_focus_button("first");
    const auto disabled = create_disabled_checkbox();
    const auto second = create_focus_button("second");
    const auto third = create_focus_button("third");

    REQUIRE(sao_ui_input_router_set_focus_widget(router, first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, disabled) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, second) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, third) == SAO_STATUS_OK);

    FocusEventLog first_log;
    FocusEventLog third_log;
    uint64_t first_gain = 0;
    uint64_t first_loss = 0;
    uint64_t third_gain = 0;
    uint64_t third_loss = 0;
    REQUIRE(sao_ui_widget_add_event_handler(first, SAO_UI_EVT_FOCUS_GAINED, focus_event_cb,
                                            &first_log, &first_gain) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(first, SAO_UI_EVT_FOCUS_LOST, focus_event_cb,
                                            &first_log, &first_loss) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(third, SAO_UI_EVT_FOCUS_GAINED, focus_event_cb,
                                            &third_log, &third_gain) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(third, SAO_UI_EVT_FOCUS_LOST, focus_event_cb,
                                            &third_log, &third_loss) == SAO_STATUS_OK);

    REQUIRE(sao_ui_input_router_focus_next(router, true) == SAO_STATUS_OK);
    sao_ui_widget_handle_t focused = nullptr;
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    REQUIRE(focused == second);
    REQUIRE(third_log.lost == 1);

    REQUIRE(sao_ui_input_router_focus_next(router, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    REQUIRE(focused == first);
    REQUIRE(first_log.gained == 1);

    REQUIRE(sao_ui_input_router_focus_next(router, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    REQUIRE(focused == second);
    REQUIRE(first_log.lost == 1);

    REQUIRE(sao_ui_input_router_focus_next(router, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    REQUIRE(focused == third);
    REQUIRE(third_log.gained == 1);

    sao_ui_input_router_deep_destroy(router);
    sao_ui_widget_destroy(third);
    sao_ui_widget_destroy(second);
    sao_ui_widget_destroy(disabled);
    sao_ui_widget_destroy(first);
}

TEST_CASE("portable_focus_next_stays_above_modal_barrier", "[ui][input_router][portable]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    const auto outer = create_focus_button("outer");
    const auto modal_first = create_focus_button("modal-first");
    const auto modal_disabled = create_disabled_checkbox();
    const auto modal_second = create_focus_button("modal-second");

    REQUIRE(sao_ui_input_router_set_focus_widget(router, outer) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_push_modal(router, fake_panel(42)) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, modal_first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, modal_disabled) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, modal_second) == SAO_STATUS_OK);

    REQUIRE(sao_ui_input_router_focus_next(router, true) == SAO_STATUS_OK);
    sao_ui_widget_handle_t focused = nullptr;
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    REQUIRE(focused == modal_first);
    REQUIRE(focused != outer);
    REQUIRE(sao_ui_input_router_focus_next(router, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    REQUIRE(focused == modal_second);

    REQUIRE(sao_ui_input_router_pop_modal(router) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    REQUIRE(focused == outer);

    sao_ui_input_router_deep_destroy(router);
    sao_ui_widget_destroy(modal_second);
    sao_ui_widget_destroy(modal_disabled);
    sao_ui_widget_destroy(modal_first);
    sao_ui_widget_destroy(outer);
}

TEST_CASE("portable_focus_next_reports_not_found_without_focusable_candidates",
          "[ui][input_router][portable]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    const auto disabled = create_disabled_checkbox();
    REQUIRE(sao_ui_input_router_set_focus_widget(router, disabled) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_next(router, false) == SAO_STATUS_ERR_NOT_FOUND);
    sao_ui_input_router_deep_destroy(router);
    sao_ui_widget_destroy(disabled);
}

TEST_CASE("portable_modal_validation_and_empty_pop_preserve_focus",
          "[ui][input_router][hardening]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    const auto first = create_focus_button("first");
    const auto second = create_focus_button("second");
    REQUIRE(sao_ui_input_router_set_focus_widget(router, first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, second) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_push_modal(router, nullptr) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_input_router_pop_modal(router) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_depth(router) == 2);
    sao_ui_widget_handle_t focused = nullptr;
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    REQUIRE(focused == second);
    sao_ui_input_router_deep_destroy(router);
    sao_ui_widget_destroy(second);
    sao_ui_widget_destroy(first);
}

TEST_CASE("portable_router_prunes_retired_focus_hover_and_capture",
          "[ui][input_router][hardening]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    const auto widget = create_focus_button("retired");
    REQUIRE(sao_ui_input_router_set_focus_widget(router, widget) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_capture_mouse(router, widget) == SAO_STATUS_OK);
    auto move = mouse_move_event(10, 10);
    bool consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &move, &consumed) == SAO_STATUS_OK);
    REQUIRE(consumed);
    REQUIRE(sao_ui_input_router_last_route_target(router) == widget);

    sao_ui_widget_destroy(widget);
    sao_ui_widget_handle_t focused = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    REQUIRE(focused == nullptr);
    REQUIRE(sao_ui_input_router_focus_depth(router) == 0);
    consumed = true;
    REQUIRE(sao_ui_input_router_route_event(router, &move, &consumed) == SAO_STATUS_OK);
    REQUIRE_FALSE(consumed);
    REQUIRE(sao_ui_input_router_last_route_target(router) == nullptr);
    REQUIRE(sao_ui_input_router_release_mouse(router) == SAO_STATUS_OK);
    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("portable_nested_focus_transition_returns_busy_and_stops_dispatch",
          "[ui][input_router][hardening]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    const auto first = create_focus_button("first");
    const auto second = create_focus_button("second");
    const auto nested = create_focus_button("nested");
    REQUIRE(sao_ui_input_router_set_focus_widget(router, first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, second) == SAO_STATUS_OK);

    FocusMutation mutation{router, nested};
    FocusEventLog first_log;
    uint64_t lost_token = 0;
    uint64_t gained_token = 0;
    REQUIRE(sao_ui_widget_add_event_handler(second, SAO_UI_EVT_FOCUS_LOST, nested_focus_cb,
                                            &mutation, &lost_token) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(first, SAO_UI_EVT_FOCUS_GAINED, focus_event_cb,
                                            &first_log, &gained_token) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_next(router, true) == SAO_UI_STATUS_ERR_BUSY);
    REQUIRE(mutation.status == SAO_STATUS_OK);
    REQUIRE(first_log.gained == 0);
    sao_ui_widget_handle_t focused = nullptr;
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    REQUIRE(focused == nested);

    sao_ui_input_router_deep_destroy(router);
    sao_ui_widget_destroy(nested);
    sao_ui_widget_destroy(second);
    sao_ui_widget_destroy(first);
}

TEST_CASE("portable_peer_destroy_during_focus_transition_returns_busy",
          "[ui][input_router][hardening]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    const auto first = create_focus_button("first");
    const auto second = create_focus_button("second");
    REQUIRE(sao_ui_input_router_set_focus_widget(router, first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, second) == SAO_STATUS_OK);
    FocusMutation mutation{router, first};
    uint64_t token = 0;
    REQUIRE(sao_ui_widget_add_event_handler(second, SAO_UI_EVT_FOCUS_LOST, destroy_peer_cb,
                                            &mutation, &token) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_focus_next(router, true) == SAO_UI_STATUS_ERR_BUSY);
    bool focusable = true;
    REQUIRE(sao_ui_widget_input_is_focusable(first, &focusable) == SAO_STATUS_ERR_HANDLE_INVALID);
    sao_ui_input_router_deep_destroy(router);
    sao_ui_widget_destroy(second);
}

TEST_CASE("portable_router_self_destroy_and_callback_exceptions_are_contained",
          "[ui][input_router][hardening]") {
    const auto widget = create_focus_button("hover");
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, widget) == SAO_STATUS_OK);
    RouterDestroyContext context{router};
    REQUIRE(sao_ui_input_router_set_hover_change_handler(router, destroy_router_hover_cb,
                                                         &context) == SAO_STATUS_OK);
    auto move = mouse_move_event(1, 1);
    bool consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &move, &consumed) == SAO_UI_STATUS_ERR_BUSY);
    REQUIRE(context.callbacks == 1);
    REQUIRE(sao_ui_input_router_route_event(router, &move, &consumed) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
    sao_ui_input_router_deep_destroy(router);

    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_focus_widget(router, widget) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_hover_change_handler(router, throwing_hover_cb, nullptr) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_route_event(router, &move, &consumed) == SAO_STATUS_ERR_UNKNOWN);
    sao_ui_input_router_deep_destroy(router);
    sao_ui_widget_destroy(widget);
}

TEST_CASE("router compositor rebind preserves handle focus and hotkeys",
          "[ui][input_router][lifecycle][rebind][state]") {
    sao_ui_compositor_handle_t first = nullptr;
    sao_ui_compositor_handle_t second = nullptr;
    sao_ui_compositor_handle_t third = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &second) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &third) == SAO_STATUS_OK);
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(first, &router) == SAO_STATUS_OK);
    auto* const identity = router;

    const auto widget = create_focus_button("rebind-focus");
    REQUIRE(sao_ui_input_router_set_focus_widget(router, widget) == SAO_STATUS_OK);
    HotkeyFireLog log;
    SaoUiHotkeyBindingSpec spec{};
    spec.binding_id_utf8 = "rebind-hotkey";
    spec.virtual_key = 0x77;
    spec.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;
    sao_ui_hotkey_binding_t binding = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &spec, &hotkey_cb, &log,
                                                 &binding) == SAO_STATUS_OK);

    CHECK(sao_ui_input_router_deep_rebind_compositor(router, second, third) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(router == identity);
    CHECK(sao_ui_input_router_focus_depth(router) == 1);
    CHECK(sao_ui_input_router_hotkey_count(router) == 1);
    REQUIRE(sao_ui_input_router_deep_rebind_compositor(router, first, second) == SAO_STATUS_OK);
    CHECK(router == identity);
    CHECK(sao_ui_input_router_focus_depth(router) == 1);
    CHECK(sao_ui_input_router_hotkey_count(router) == 1);
    CHECK(sao_ui_input_router_deep_rebind_compositor(router, first, third) ==
          SAO_STATUS_ERR_HANDLE_INVALID);

    sao_ui_widget_handle_t focused = nullptr;
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    CHECK(focused == widget);
    const auto event = key_down_event(spec.virtual_key, 0);
    sao_ui_hotkey_binding_t matched = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(router, &event, &matched) == SAO_STATUS_OK);
    CHECK(matched == binding);
    CHECK(log.hits.load() == 1);

    REQUIRE(sao_ui_input_router_deep_try_destroy(router) == SAO_STATUS_OK);
    sao_ui_widget_destroy(widget);
    REQUIRE(sao_ui_compositor_try_destroy(third) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(second) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(first) == SAO_STATUS_OK);
}

TEST_CASE("router compositor rebind is busy during an in-flight callback and retries unchanged",
          "[ui][input_router][lifecycle][rebind][callback][concurrency]") {
    sao_ui_compositor_handle_t first = nullptr;
    sao_ui_compositor_handle_t second = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &second) == SAO_STATUS_OK);
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(first, &router) == SAO_STATUS_OK);
    const auto widget = create_focus_button("rebind-busy-focus");
    REQUIRE(sao_ui_input_router_set_focus_widget(router, widget) == SAO_STATUS_OK);

    BlockingHotkey state;
    SaoUiHotkeyBindingSpec spec{};
    spec.binding_id_utf8 = "rebind-busy";
    spec.virtual_key = 0x78;
    spec.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;
    sao_ui_hotkey_binding_t binding = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &spec, &blocking_hotkey_cb,
                                                 &state, &binding) == SAO_STATUS_OK);
    const auto event = key_down_event(spec.virtual_key, 0);
    auto dispatch = std::async(std::launch::async, [&] {
        sao_ui_hotkey_binding_t matched = 0;
        return sao_ui_input_router_match_hotkey(router, &event, &matched);
    });
    {
        std::unique_lock lock(state.mutex);
        REQUIRE(state.condition.wait_for(lock, std::chrono::seconds(5),
                                         [&state] { return state.entered; }));
    }

    CHECK(sao_ui_input_router_deep_rebind_compositor(router, first, second) ==
          SAO_UI_STATUS_ERR_BUSY);
    CHECK(sao_ui_input_router_focus_depth(router) == 1);
    CHECK(sao_ui_input_router_hotkey_count(router) == 1);
    {
        std::lock_guard lock(state.mutex);
        state.release = true;
    }
    state.condition.notify_all();
    REQUIRE(dispatch.get() == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_deep_rebind_compositor(router, first, second) == SAO_STATUS_OK);
    sao_ui_widget_handle_t focused = nullptr;
    REQUIRE(sao_ui_input_router_get_focus(router, &focused, nullptr) == SAO_STATUS_OK);
    CHECK(focused == widget);
    CHECK(sao_ui_input_router_hotkey_count(router) == 1);

    REQUIRE(sao_ui_input_router_deep_try_destroy(router) == SAO_STATUS_OK);
    sao_ui_widget_destroy(widget);
    REQUIRE(sao_ui_compositor_try_destroy(second) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(first) == SAO_STATUS_OK);
}
