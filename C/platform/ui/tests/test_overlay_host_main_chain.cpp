// Production coverage for the Win32 overlay-host main chain.

#include <catch2/catch_test_macros.hpp>

#include "sao/core/status.h"
#include "sao/ui/dc_mutation.h"
#include "sao/ui/gpu_overlay_window.h"
#include "sao/ui/input.h"
#include "sao/ui/overlay_host.h"
#include "sao/ui/z_order.h"

#include <mutex>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "sao/ui/abi.h"
extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_dc_mut_test_drain(sao_ui_dc_mutation_coordinator_handle_t handle, uint32_t timeout_ms);
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_dc_mut_test_dispatch_count(sao_ui_dc_mutation_coordinator_handle_t handle);

namespace {

SaoOverlayHostConfig test_config() {
    SaoOverlayHostConfig config{};
    config.width = 640;
    config.height = 480;
    config.origin_x = 20;
    config.origin_y = 30;
    config.title_utf16 = L"SAO overlay production-chain test";
    return config;
}

struct MouseLog {
    uint32_t message = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t button = -2;
    int32_t wheel_delta = 0;
    uint32_t count = 0;
};

struct HotkeyLog {
    uint32_t id = 0;
    uint32_t calls = 0;
};

void SAO_UI_CALL hotkey_callback(uint32_t id, void* user_data) {
    auto* log = static_cast<HotkeyLog*>(user_data);
    log->id = id;
    ++log->calls;
}

bool SAO_UI_CALL ll_callback(uint32_t, uint64_t, uint64_t, void*) {
    return false;
}

void SAO_UI_CALL mouse_callback(uint32_t message, int32_t x, int32_t y, int32_t button,
                                int32_t wheel_delta, void* user_data) {
    auto* log = static_cast<MouseLog*>(user_data);
    log->message = message;
    log->x = x;
    log->y = y;
    log->button = button;
    log->wheel_delta = wheel_delta;
    ++log->count;
}

bool SAO_UI_CALL never_hit(int32_t, int32_t, void*) {
    return false;
}

#if defined(_WIN32)

struct RectScrubObservation {
    void* hwnd = nullptr;
    SaoUiDcMutationRect fake_rect{};
    RECT real_rect{};
    uint32_t settle_ms = 0;
    uint32_t timeout_ms = 0;
    DWORD thread_id = 0;
};

struct RectScrubLog {
    std::mutex mutex;
    std::vector<RectScrubObservation> observations;
};

struct ProtectionLog {
    uint32_t enable_calls = 0u;
    uint32_t disable_calls = 0u;
    void* render = nullptr;
    void* control = nullptr;
    void* owner = nullptr;
    DWORD render_affinity_at_enable = 0u;
    DWORD control_affinity_at_enable = 0u;
    DWORD owner_affinity_at_enable = 0u;
    sao_ui_overlay_host_handle_t reentry_host = nullptr;
    bool reenter_on_enable = false;
    sao_status_t reentry_status = SAO_STATUS_ERR_UNKNOWN;
};

sao_status_t SAO_UI_CALL record_protection(
    void* render, void* control, void* owner, bool enable, void* user_data) {
    auto* log = static_cast<ProtectionLog*>(user_data);
    log->render = render;
    log->control = control;
    log->owner = owner;
    if (enable) {
        ++log->enable_calls;
        if (::GetWindowDisplayAffinity(static_cast<HWND>(render),
                                       &log->render_affinity_at_enable) == FALSE ||
            ::GetWindowDisplayAffinity(static_cast<HWND>(control),
                                       &log->control_affinity_at_enable) == FALSE ||
            ::GetWindowDisplayAffinity(static_cast<HWND>(owner),
                                       &log->owner_affinity_at_enable) == FALSE) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        if (log->reenter_on_enable && log->reentry_host != nullptr) {
            log->reenter_on_enable = false;
            log->reentry_status =
                sao_ui_overlay_host_set_capture_mode(log->reentry_host, true);
        }
    } else {
        ++log->disable_calls;
    }
    return SAO_STATUS_OK;
}

sao_status_t SAO_UI_CALL record_rect_scrub(void* user_data, void* hwnd,
                                           const SaoUiDcMutationRect* fake_rect, uint32_t settle_ms,
                                           uint32_t timeout_ms) {
    RectScrubObservation observation{};
    observation.hwnd = hwnd;
    observation.fake_rect = *fake_rect;
    observation.settle_ms = settle_ms;
    observation.timeout_ms = timeout_ms;
    observation.thread_id = ::GetCurrentThreadId();
    if (!::GetWindowRect(static_cast<HWND>(hwnd), &observation.real_rect)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    auto* log = static_cast<RectScrubLog*>(user_data);
    std::lock_guard<std::mutex> lock(log->mutex);
    log->observations.push_back(observation);
    return SAO_STATUS_OK;
}

RectScrubObservation scrub_at(RectScrubLog& log, size_t index) {
    std::lock_guard<std::mutex> lock(log.mutex);
    REQUIRE(index < log.observations.size());
    return log.observations[index];
}

void require_scrub_count(sao_ui_dc_mutation_coordinator_handle_t coordinator, RectScrubLog& log,
                         size_t expected) {
    REQUIRE(sao_ui_dc_mut_test_drain(coordinator, 2000));
    std::lock_guard<std::mutex> lock(log.mutex);
    REQUIRE(log.observations.size() == expected);
    REQUIRE(sao_ui_dc_mut_test_dispatch_count(coordinator) == expected);
}

void check_ordered_scrub(const RectScrubObservation& observation, HWND expected_hwnd,
                         const RECT& expected_real_rect) {
    CHECK(observation.hwnd == expected_hwnd);
    CHECK(observation.fake_rect.left == 0);
    CHECK(observation.fake_rect.top == 0);
    CHECK(observation.fake_rect.right == 1);
    CHECK(observation.fake_rect.bottom == 1);
    CHECK(observation.settle_ms == 40);
    CHECK(observation.timeout_ms == 2000);
    CHECK(observation.real_rect.left == expected_real_rect.left);
    CHECK(observation.real_rect.top == expected_real_rect.top);
    CHECK(observation.real_rect.right == expected_real_rect.right);
    CHECK(observation.real_rect.bottom == expected_real_rect.bottom);
}

#endif

} // namespace

#if defined(_WIN32)

TEST_CASE("overlay host production main chain", "[ui][overlay_host][production]") {
    RectScrubLog scrub_log;
    const SaoUiDcMutationProvider provider{&record_rect_scrub, &scrub_log};
    sao_ui_dc_mutation_coordinator_handle_t coordinator = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create_ex(&provider, &coordinator) == SAO_STATUS_OK);

    sao_ui_overlay_host_handle_t host = nullptr;
    SaoOverlayHostConfig config = test_config();
    config.dc_mutation_coordinator = coordinator;
    REQUIRE(sao_ui_overlay_host_create(&config, &host) == SAO_STATUS_OK);
    REQUIRE(host != nullptr);

    HWND render_hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    HWND control_hwnd = static_cast<HWND>(sao_ui_overlay_host_control_hwnd(host));
    REQUIRE(render_hwnd != nullptr);
    REQUIRE(control_hwnd != nullptr);
    REQUIRE(render_hwnd != control_hwnd);
    REQUIRE(::IsWindow(render_hwnd) != FALSE);
    REQUIRE(::IsWindow(control_hwnd) != FALSE);
    CHECK(::GetWindow(render_hwnd, GW_OWNER) == ::GetWindow(control_hwnd, GW_OWNER));

    // The host starts as an empty custom region and is therefore click-through.
    CHECK(sao_ui_overlay_host_input_passthrough(host));
    CHECK(::SendMessageW(render_hwnd, WM_NCHITTEST, 0, MAKELPARAM(40, 50)) == HTTRANSPARENT);

    sao_ui_input_router_handle_t input = nullptr;
    REQUIRE(sao_ui_input_router_create(host, &input) == SAO_STATUS_OK);
    HotkeyLog hotkey{};
    REQUIRE(sao_ui_input_router_register_global_hotkey(input, 17, 'K', SAO_UI_MOD_CTRL,
                                                       &hotkey_callback, &hotkey) == SAO_STATUS_OK);
    SaoHotkeyBinding bindings[2]{};
    size_t binding_count = 0;
    REQUIRE(sao_ui_input_router_list_hotkeys(input, bindings, 2, &binding_count) == SAO_STATUS_OK);
    CHECK(binding_count == 1);
    CHECK(bindings[0].hotkey_id == 17);
    REQUIRE(sao_ui_input_router_set_ll_hook_callbacks(input, &ll_callback, &ll_callback, nullptr) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_install_ll_hooks(input) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_uninstall_ll_hooks(input) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_unregister_global_hotkey(input, 17) == SAO_STATUS_OK);
    const SaoOverlayHostInputRect initial_region{10, 20, 100, 80};
    REQUIRE(sao_ui_input_router_set_regions(input, &initial_region, 1) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_rebuild_region(input) == SAO_STATUS_OK);
    CHECK_FALSE(sao_ui_overlay_host_input_passthrough(host));
    CHECK(::SendMessageW(render_hwnd, WM_NCHITTEST, 0, MAKELPARAM(40, 50)) == HTCLIENT);

    // A callback refines the default host hit test while leaving the region
    // responsible for cross-process routing.
    REQUIRE(sao_ui_overlay_host_set_hit_test(host, &never_hit, nullptr) == SAO_STATUS_OK);
    CHECK(::SendMessageW(render_hwnd, WM_NCHITTEST, 0, MAKELPARAM(40, 50)) == HTTRANSPARENT);
    REQUIRE(sao_ui_overlay_host_set_hit_test(host, nullptr, nullptr) == SAO_STATUS_OK);

    // Replacing the region keeps the old rectangle for one application cycle.
    const SaoOverlayHostInputRect moved_region{300, 200, 50, 40};
    REQUIRE(sao_ui_input_router_set_regions(input, &moved_region, 1) == SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_rebuild_region(input) == SAO_STATUS_OK);
    HRGN observed = ::CreateRectRgn(0, 0, 0, 0);
    REQUIRE(observed != nullptr);
    CHECK(::GetWindowRgn(render_hwnd, observed) != ERROR);
    CHECK(::PtInRegion(observed, 20, 30) != FALSE);
    CHECK(::PtInRegion(observed, 310, 210) != FALSE);
    ::DeleteObject(observed);

    MouseLog mouse{};
    REQUIRE(sao_ui_overlay_host_set_mouse(host, &mouse_callback, &mouse) == SAO_STATUS_OK);
    ::SendMessageW(render_hwnd, WM_LBUTTONDOWN, 0, MAKELPARAM(12, 22));
    CHECK(mouse.count == 1);
    CHECK(mouse.message == WM_LBUTTONDOWN);
    CHECK(mouse.button == 0);

    REQUIRE(sao_ui_overlay_host_set_visible(host, true) == SAO_STATUS_OK);
    require_scrub_count(coordinator, scrub_log, 1);
    const RECT initial_real{config.origin_x, config.origin_y, config.origin_x + config.width,
                            config.origin_y + config.height};
    check_ordered_scrub(scrub_at(scrub_log, 0), render_hwnd, initial_real);
    CHECK(::IsWindowVisible(render_hwnd) != FALSE);
    CHECK(sao_ui_overlay_host_visible(host));
    REQUIRE(::SetWindowPos(render_hwnd, nullptr, 0, 0, 1, 1, SWP_NOACTIVATE | SWP_NOZORDER));
    REQUIRE(sao_ui_overlay_host_set_visible(host, true) == SAO_STATUS_OK);
    require_scrub_count(coordinator, scrub_log, 2);
    check_ordered_scrub(scrub_at(scrub_log, 1), render_hwnd, initial_real);
    RECT recovered_rect{};
    REQUIRE(::GetWindowRect(render_hwnd, &recovered_rect));
    CHECK(recovered_rect.left == config.origin_x);
    CHECK(recovered_rect.top == config.origin_y);
    CHECK(recovered_rect.right - recovered_rect.left == config.width);
    CHECK(recovered_rect.bottom - recovered_rect.top == config.height);
    SaoOverlayHostState host_state{};
    REQUIRE(sao_ui_overlay_host_get_state(host, &host_state) == SAO_STATUS_OK);
    CHECK(host_state.visible);
    CHECK_FALSE(host_state.input_passthrough);
    REQUIRE(sao_ui_overlay_host_set_visible(host, false) == SAO_STATUS_OK);
    require_scrub_count(coordinator, scrub_log, 2);
    CHECK(::IsWindowVisible(render_hwnd) == FALSE);
    CHECK_FALSE(sao_ui_overlay_host_visible(host));

    REQUIRE(sao_ui_overlay_host_set_bounds(host, -200, 120, 800, 600) == SAO_STATUS_OK);
    require_scrub_count(coordinator, scrub_log, 3);
    const RECT bounds_real{-200, 120, 600, 720};
    check_ordered_scrub(scrub_at(scrub_log, 2), render_hwnd, bounds_real);
    SaoOverlayHostClientRect bounds{};
    REQUIRE(sao_ui_overlay_host_get_client_rect(host, &bounds) == SAO_STATUS_OK);
    CHECK(bounds.x == -200);
    CHECK(bounds.y == 120);
    CHECK(bounds.width == 800);
    CHECK(bounds.height == 600);
    CHECK(sao_ui_overlay_host_current_dpi(host) >= 96);

    RECT dpi_suggested{140, 160, 1140, 860};
    ::SendMessageW(render_hwnd, WM_DPICHANGED, MAKEWPARAM(144, 144),
                   reinterpret_cast<LPARAM>(&dpi_suggested));
    require_scrub_count(coordinator, scrub_log, 4);
    check_ordered_scrub(scrub_at(scrub_log, 3), render_hwnd, dpi_suggested);
    REQUIRE(sao_ui_overlay_host_get_client_rect(host, &bounds) == SAO_STATUS_OK);
    CHECK(bounds.x == dpi_suggested.left);
    CHECK(bounds.y == dpi_suggested.top);
    CHECK(bounds.width == dpi_suggested.right - dpi_suggested.left);
    CHECK(bounds.height == dpi_suggested.bottom - dpi_suggested.top);
    SaoOverlayHostClientRect desired_after_dpi{};
    REQUIRE(sao_ui_overlay_host_get_desired_bounds(host, &desired_after_dpi) == SAO_STATUS_OK);
    CHECK(desired_after_dpi.x == bounds.x);
    CHECK(desired_after_dpi.y == bounds.y);
    CHECK(desired_after_dpi.width == bounds.width);
    CHECK(desired_after_dpi.height == bounds.height);
    REQUIRE(sao_ui_overlay_host_pump_messages(host) == SAO_STATUS_OK);
    REQUIRE(sao_ui_overlay_host_msg_wait(host, 0) == SAO_STATUS_OK);

    sao_ui_z_order_manager_handle_t z_order = nullptr;
    REQUIRE(sao_ui_z_order_manager_create(host, &z_order) == SAO_STATUS_OK);
    REQUIRE(sao_ui_z_order_set_policy(z_order, SAO_UI_TOPMOST_NEVER) == SAO_STATUS_OK);
    REQUIRE(sao_ui_z_order_enforce(z_order, nullptr, false, false) == SAO_STATUS_OK);
    require_scrub_count(coordinator, scrub_log, 5);
    check_ordered_scrub(scrub_at(scrub_log, 4), render_hwnd, dpi_suggested);
    SaoZOrderStatus z_status{};
    REQUIRE(sao_ui_z_order_status(z_order, &z_status) == SAO_STATUS_OK);
    CHECK(z_status.policy == SAO_UI_TOPMOST_NEVER);
    REQUIRE(sao_ui_z_order_set_policy(z_order, SAO_UI_TOPMOST_FOLLOW_GAME) == SAO_STATUS_OK);
    REQUIRE(::SetWindowPos(render_hwnd, nullptr, 0, 0, 1, 1, SWP_NOACTIVATE | SWP_NOZORDER));
    REQUIRE(sao_ui_z_order_enforce(z_order, nullptr, false, false) == SAO_STATUS_OK);
    require_scrub_count(coordinator, scrub_log, 6);
    check_ordered_scrub(scrub_at(scrub_log, 5), render_hwnd, dpi_suggested);
    REQUIRE(::GetWindowRect(render_hwnd, &recovered_rect));
    CHECK(recovered_rect.left == bounds.x);
    CHECK(recovered_rect.top == bounds.y);
    CHECK(recovered_rect.right - recovered_rect.left == bounds.width);
    CHECK(recovered_rect.bottom - recovered_rect.top == bounds.height);
    REQUIRE(sao_ui_z_order_enforce(z_order, nullptr, false, false) == SAO_STATUS_OK);
    require_scrub_count(coordinator, scrub_log, 6);
    REQUIRE(sao_ui_z_order_set_policy(z_order, SAO_UI_TOPMOST_FOLLOW_GAME) == SAO_STATUS_OK);
    HWND game_hwnd =
        ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"z-order game fixture", WS_POPUP, 0, 0, 16,
                          16, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    REQUIRE(game_hwnd != nullptr);
    REQUIRE(sao_ui_z_order_enforce(z_order, game_hwnd, false, true) == SAO_STATUS_OK);
    require_scrub_count(coordinator, scrub_log, 7);
    check_ordered_scrub(scrub_at(scrub_log, 6), render_hwnd, dpi_suggested);
    REQUIRE(::DestroyWindow(game_hwnd));
    REQUIRE(sao_ui_z_order_check_leak_patterns(z_order, 0) == SAO_STATUS_OK);
    CHECK(sao_ui_z_order_check_leak_patterns(z_order, SAO_UI_WS_EX_LAYERED) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    sao_ui_z_order_manager_destroy(z_order);

    // WGL is openly unsupported by the D3D/DComp production contract.
    CHECK_FALSE(sao_ui_gpu_overlay_supported());
    CHECK(sao_ui_overlay_host_hglrc(host) == nullptr);
    CHECK(sao_ui_overlay_host_make_current(host) == SAO_STATUS_ERR_NOT_IMPLEMENTED);

    sao_ui_input_router_destroy(input);

    // Host destruction owns the final affinity reset.  This must happen before
    // either HWND is destroyed so the security facade can verify both resets.
    REQUIRE(sao_ui_overlay_host_set_capture_mode(host, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_overlay_host_destroy(host));
    CHECK(::IsWindow(render_hwnd) == FALSE);
    CHECK(::IsWindow(control_hwnd) == FALSE);
    sao_ui_dc_mutation_coordinator_destroy(coordinator);
}

TEST_CASE("overlay host accepts a coordinator without a rect provider as USER32 fallback",
          "[ui][overlay_host][production][rect_scrub][fallback]") {
    sao_ui_dc_mutation_coordinator_handle_t coordinator = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&coordinator) == SAO_STATUS_OK);
    SaoOverlayHostConfig config = test_config();
    config.dc_mutation_coordinator = coordinator;
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&config, &host) == SAO_STATUS_OK);
    HWND hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);

    REQUIRE(sao_ui_overlay_host_set_bounds(host, 75, 85, 425, 315) == SAO_STATUS_OK);
    REQUIRE(sao_ui_overlay_host_set_visible(host, true) == SAO_STATUS_OK);
    RECT rect{};
    REQUIRE(::GetWindowRect(hwnd, &rect));
    CHECK(rect.left == 75);
    CHECK(rect.top == 85);
    CHECK(rect.right - rect.left == 425);
    CHECK(rect.bottom - rect.top == 315);
    CHECK(sao_ui_dc_mut_test_dispatch_count(coordinator) == 0);

    REQUIRE(sao_ui_overlay_host_destroy(host));
    sao_ui_dc_mutation_coordinator_destroy(coordinator);
}

TEST_CASE("overlay host applies screencap protection during creation",
          "[ui][overlay_host][production][screencap]") {
    ProtectionLog log;
    SaoOverlayHostConfig config = test_config();
    config.sao_screencap_protection = true;
    config.protection_provider = &record_protection;
    config.protection_provider_user_data = &log;
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&config, &host) == SAO_STATUS_OK);
    REQUIRE(host != nullptr);
    HWND render = static_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    HWND control = static_cast<HWND>(sao_ui_overlay_host_control_hwnd(host));
    HWND owner = static_cast<HWND>(sao_ui_overlay_host_owner_hwnd(host));
    REQUIRE(render != nullptr);
    REQUIRE(control != nullptr);
    REQUIRE(owner != nullptr);
    CHECK(log.enable_calls == 1u);
    CHECK(log.disable_calls == 0u);
    CHECK(log.render == render);
    CHECK(log.control == control);
    CHECK(log.owner == owner);
    CHECK(log.render_affinity_at_enable != WDA_NONE);
    CHECK(log.control_affinity_at_enable != WDA_NONE);
    CHECK(log.owner_affinity_at_enable != WDA_NONE);
    CHECK(sao_ui_overlay_host_capture_excluded(host));
    REQUIRE(sao_ui_overlay_host_destroy(host));
    CHECK(log.disable_calls == 1u);
}

TEST_CASE("overlay host leaves creation unprotected when setting is off",
          "[ui][overlay_host][production][screencap]") {
    ProtectionLog log;
    SaoOverlayHostConfig config = test_config();
    config.sao_screencap_protection = false;
    config.protection_provider = &record_protection;
    config.protection_provider_user_data = &log;
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&config, &host) == SAO_STATUS_OK);
    DWORD affinity = 0xFFFFFFFFu;
    REQUIRE(::GetWindowDisplayAffinity(
                static_cast<HWND>(sao_ui_overlay_host_hwnd(host)),
                &affinity) != FALSE);
    CHECK(affinity == WDA_NONE);
    CHECK(log.enable_calls == 0u);
    CHECK(log.disable_calls == 0u);
    CHECK_FALSE(sao_ui_overlay_host_capture_excluded(host));
    REQUIRE(sao_ui_overlay_host_destroy(host));
    CHECK(log.disable_calls == 0u);
}

TEST_CASE("overlay capture provider same-thread reentry returns BUSY",
          "[ui][overlay_host][production][screencap][provider][reentry]") {
    ProtectionLog log;
    SaoOverlayHostConfig config = test_config();
    config.protection_provider = &record_protection;
    config.protection_provider_user_data = &log;
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&config, &host) == SAO_STATUS_OK);
    log.reentry_host = host;
    log.reenter_on_enable = true;
    REQUIRE(sao_ui_overlay_host_set_capture_mode(host, true) == SAO_STATUS_OK);
    CHECK(log.reentry_status == static_cast<sao_status_t>(-102));
    REQUIRE(sao_ui_overlay_host_destroy(host));
}

#else

TEST_CASE("overlay host production main chain requires Windows", "[ui][overlay_host][production]") {
    SUCCEED("Windows-only target");
}

#endif
