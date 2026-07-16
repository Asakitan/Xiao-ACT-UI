// Production coverage for the Win32 overlay-host main chain.

#include <catch2/catch_test_macros.hpp>

#include "sao/core/status.h"
#include "sao/ui/gpu_overlay_window.h"
#include "sao/ui/input.h"
#include "sao/ui/overlay_host.h"
#include "sao/ui/z_order.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

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

bool SAO_UI_CALL ll_callback(uint32_t, uint64_t, uint64_t, void*) { return false; }

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

bool SAO_UI_CALL never_hit(int32_t, int32_t, void*) { return false; }

}  // namespace

#if defined(_WIN32)

TEST_CASE("overlay host production main chain", "[ui][overlay_host][production]") {
	sao_ui_overlay_host_handle_t host = nullptr;
	const SaoOverlayHostConfig config = test_config();
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
	REQUIRE(sao_ui_input_router_register_global_hotkey(
		input, 17, 'K', SAO_UI_MOD_CTRL, &hotkey_callback, &hotkey) == SAO_STATUS_OK);
	SaoHotkeyBinding bindings[2]{};
	size_t binding_count = 0;
	REQUIRE(sao_ui_input_router_list_hotkeys(
		input, bindings, 2, &binding_count) == SAO_STATUS_OK);
	CHECK(binding_count == 1);
	CHECK(bindings[0].hotkey_id == 17);
	REQUIRE(sao_ui_input_router_set_ll_hook_callbacks(
		input, &ll_callback, &ll_callback, nullptr) == SAO_STATUS_OK);
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
	CHECK(::IsWindowVisible(render_hwnd) != FALSE);
	CHECK(sao_ui_overlay_host_visible(host));
	SaoOverlayHostState host_state{};
	REQUIRE(sao_ui_overlay_host_get_state(host, &host_state) == SAO_STATUS_OK);
	CHECK(host_state.visible);
	CHECK_FALSE(host_state.input_passthrough);
	REQUIRE(sao_ui_overlay_host_set_visible(host, false) == SAO_STATUS_OK);
	CHECK(::IsWindowVisible(render_hwnd) == FALSE);
	CHECK_FALSE(sao_ui_overlay_host_visible(host));

	REQUIRE(sao_ui_overlay_host_set_bounds(host, -200, 120, 800, 600) == SAO_STATUS_OK);
	SaoOverlayHostClientRect bounds{};
	REQUIRE(sao_ui_overlay_host_get_client_rect(host, &bounds) == SAO_STATUS_OK);
	CHECK(bounds.x == -200);
	CHECK(bounds.y == 120);
	CHECK(bounds.width == 800);
	CHECK(bounds.height == 600);
	CHECK(sao_ui_overlay_host_current_dpi(host) >= 96);
	REQUIRE(sao_ui_overlay_host_pump_messages(host) == SAO_STATUS_OK);
	REQUIRE(sao_ui_overlay_host_msg_wait(host, 0) == SAO_STATUS_OK);

	sao_ui_z_order_manager_handle_t z_order = nullptr;
	REQUIRE(sao_ui_z_order_manager_create(host, &z_order) == SAO_STATUS_OK);
	REQUIRE(sao_ui_z_order_set_policy(z_order, SAO_UI_TOPMOST_NEVER) == SAO_STATUS_OK);
	REQUIRE(sao_ui_z_order_enforce(z_order, nullptr, false, false) == SAO_STATUS_OK);
	SaoZOrderStatus z_status{};
	REQUIRE(sao_ui_z_order_status(z_order, &z_status) == SAO_STATUS_OK);
	CHECK(z_status.policy == SAO_UI_TOPMOST_NEVER);
	REQUIRE(sao_ui_z_order_set_policy(z_order, SAO_UI_TOPMOST_FOLLOW_GAME) == SAO_STATUS_OK);
	REQUIRE(sao_ui_z_order_enforce(z_order, nullptr, false, false) == SAO_STATUS_OK);
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
}

#else

TEST_CASE("overlay host production main chain requires Windows", "[ui][overlay_host][production]") {
	SUCCEED("Windows-only target");
}

#endif
