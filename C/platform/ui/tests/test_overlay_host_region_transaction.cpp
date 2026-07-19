#include <catch2/catch_test_macros.hpp>

#include "sao/core/status.h"
#include "sao/ui/overlay_host.h"
#include "input_win32_api.h"

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

#if defined(_WIN32)

using OverlayHostWin32Api = sao::ui::overlay_host_detail::Win32Api;

struct FakeWin32State {
    uint32_t set_window_rgn_fail_on_call = 0;
    int set_window_pos_failures = 0;
    uint32_t set_window_rgn_calls = 0;
    uint32_t set_window_pos_calls = 0;
    HRGN last_transferred_region = nullptr;
    bool deleted_transferred_region = false;
};

FakeWin32State* g_fake = nullptr;

int WINAPI fake_set_window_rgn(HWND hwnd, HRGN region, BOOL redraw) {
    ++g_fake->set_window_rgn_calls;
    if (g_fake->set_window_rgn_calls == g_fake->set_window_rgn_fail_on_call) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const int result = ::SetWindowRgn(hwnd, region, redraw);
    if (result != FALSE) g_fake->last_transferred_region = region;
    return result;
}

int WINAPI fake_get_window_rgn(HWND hwnd, HRGN region) {
    return ::GetWindowRgn(hwnd, region);
}

LONG_PTR WINAPI fake_get_window_long_ptr_w(HWND hwnd, int index) {
    return ::GetWindowLongPtrW(hwnd, index);
}

LONG_PTR WINAPI fake_set_window_long_ptr_w(HWND hwnd, int index, LONG_PTR value) {
    return ::SetWindowLongPtrW(hwnd, index, value);
}

BOOL WINAPI fake_set_window_pos(HWND hwnd, HWND insert_after, int x, int y, int width,
                                int height, UINT flags) {
    ++g_fake->set_window_pos_calls;
    if (g_fake->set_window_pos_failures > 0) {
        --g_fake->set_window_pos_failures;
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return ::SetWindowPos(hwnd, insert_after, x, y, width, height, flags);
}

BOOL WINAPI fake_delete_object(HGDIOBJ object) {
    if (object == g_fake->last_transferred_region) {
        g_fake->deleted_transferred_region = true;
    }
    return ::DeleteObject(object);
}

const OverlayHostWin32Api kFakeApi{
    &fake_set_window_rgn,
    &fake_get_window_rgn,
    &fake_get_window_long_ptr_w,
    &fake_set_window_long_ptr_w,
    &fake_set_window_pos,
    &fake_delete_object,
};

SaoOverlayHostConfig test_config() {
    SaoOverlayHostConfig config{};
    config.width = 640;
    config.height = 480;
    config.origin_x = 20;
    config.origin_y = 30;
    config.title_utf16 = L"SAO overlay region transaction test";
    return config;
}

class HostFixture {
public:
    HostFixture() {
        const SaoOverlayHostConfig config = test_config();
        REQUIRE(sao_ui_overlay_host_create(&config, &host) == SAO_STATUS_OK);
        hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(host));
        REQUIRE(hwnd != nullptr);
        g_fake = &fake;
        sao::ui::overlay_host_detail::set_win32_api_for_testing(&kFakeApi);
    }

    ~HostFixture() {
        sao::ui::overlay_host_detail::reset_win32_api_for_testing();
        g_fake = nullptr;
        if (host != nullptr) {
            (void)sao_ui_overlay_host_destroy(host);
        }
    }

    HostFixture(const HostFixture&) = delete;
    HostFixture& operator=(const HostFixture&) = delete;

    FakeWin32State fake{};
    sao_ui_overlay_host_handle_t host = nullptr;
    HWND hwnd = nullptr;
};

bool region_contains(HWND hwnd, int x, int y) {
    HRGN observed = ::CreateRectRgn(0, 0, 0, 0);
    REQUIRE(observed != nullptr);
    REQUIRE(::GetWindowRgn(hwnd, observed) != ERROR);
    const bool contains = ::PtInRegion(observed, x, y) != FALSE;
    REQUIRE(::DeleteObject(observed) != FALSE);
    return contains;
}

bool transparent_style(HWND hwnd) {
    return (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
}

LRESULT hit_test(HWND hwnd, int screen_x, int screen_y) {
    return ::SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screen_x, screen_y));
}

#endif

}  // namespace

#if defined(_WIN32)

TEST_CASE("overlay region distinguishes initial empty from retained empty",
          "[ui][overlay_host][region][transaction]") {
    HostFixture fixture;
    constexpr int kScreenX = 40;
    constexpr int kScreenY = 50;
    const SaoOverlayHostInputRect region_a{10, 20, 100, 80};

    SECTION("initial empty is immediately passthrough") {
        REQUIRE(sao_ui_overlay_host_set_input_region(fixture.host, nullptr, 0) ==
                SAO_STATUS_OK);
        CHECK_FALSE(region_contains(fixture.hwnd, 20, 30));
        CHECK(transparent_style(fixture.hwnd));
        CHECK(sao_ui_overlay_host_input_passthrough(fixture.host));
        CHECK(hit_test(fixture.hwnd, kScreenX, kScreenY) == HTTRANSPARENT);
        CHECK(sao_ui_overlay_host_input_sync_state(fixture.host) ==
              SAO_UI_OVERLAY_INPUT_SYNCHRONIZED);
    }

    SECTION("A to empty to empty retains exactly one empty update") {
        REQUIRE(sao_ui_overlay_host_set_input_region(fixture.host, &region_a, 1) ==
                SAO_STATUS_OK);
        CHECK(region_contains(fixture.hwnd, 20, 30));
        CHECK_FALSE(transparent_style(fixture.hwnd));
        CHECK_FALSE(sao_ui_overlay_host_input_passthrough(fixture.host));
        CHECK(hit_test(fixture.hwnd, kScreenX, kScreenY) == HTCLIENT);

        REQUIRE(sao_ui_overlay_host_set_input_region(fixture.host, nullptr, 0) ==
                SAO_STATUS_OK);
        CHECK(region_contains(fixture.hwnd, 20, 30));
        CHECK_FALSE(transparent_style(fixture.hwnd));
        CHECK_FALSE(sao_ui_overlay_host_input_passthrough(fixture.host));
        CHECK(hit_test(fixture.hwnd, kScreenX, kScreenY) == HTCLIENT);

        REQUIRE(sao_ui_overlay_host_set_input_region(fixture.host, nullptr, 0) ==
                SAO_STATUS_OK);
        CHECK_FALSE(region_contains(fixture.hwnd, 20, 30));
        CHECK(transparent_style(fixture.hwnd));
        CHECK(sao_ui_overlay_host_input_passthrough(fixture.host));
        CHECK(hit_test(fixture.hwnd, kScreenX, kScreenY) == HTTRANSPARENT);
        CHECK(sao_ui_overlay_host_input_sync_state(fixture.host) ==
              SAO_UI_OVERLAY_INPUT_SYNCHRONIZED);
    }
}

TEST_CASE("overlay region rolls back SetWindowRgn when style commit fails",
          "[ui][overlay_host][region][rollback]") {
    HostFixture fixture;
    const SaoOverlayHostInputRect region_a{10, 20, 100, 80};
    fixture.fake.set_window_pos_failures = 1;

    REQUIRE(sao_ui_overlay_host_set_input_region(fixture.host, &region_a, 1) ==
            SAO_STATUS_ERR_OS_CALL_FAILED);
    CHECK(fixture.fake.set_window_rgn_calls == 2);
    CHECK(fixture.fake.set_window_pos_calls == 2);
    REQUIRE(fixture.fake.last_transferred_region != nullptr);
    CHECK_FALSE(fixture.fake.deleted_transferred_region);
    CHECK_FALSE(region_contains(fixture.hwnd, 20, 30));
    CHECK(transparent_style(fixture.hwnd));
    CHECK(sao_ui_overlay_host_input_passthrough(fixture.host));
    CHECK(hit_test(fixture.hwnd, 40, 50) == HTTRANSPARENT);
    CHECK(sao_ui_overlay_host_input_sync_state(fixture.host) ==
          SAO_UI_OVERLAY_INPUT_SYNCHRONIZED);

    REQUIRE(sao_ui_overlay_host_set_input_region(fixture.host, nullptr, 0) ==
            SAO_STATUS_OK);
    CHECK_FALSE(region_contains(fixture.hwnd, 20, 30));

    REQUIRE(sao_ui_overlay_host_set_input_region(fixture.host, &region_a, 1) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_overlay_host_set_input_region(fixture.host, nullptr, 0) ==
            SAO_STATUS_OK);
    CHECK(region_contains(fixture.hwnd, 20, 30));
}

TEST_CASE("overlay region exposes failed transactional rollback",
          "[ui][overlay_host][region][partial]") {
    HostFixture fixture;
    const SaoOverlayHostInputRect region_a{10, 20, 100, 80};
    fixture.fake.set_window_pos_failures = 1;
    fixture.fake.set_window_rgn_fail_on_call = 2;

    REQUIRE(sao_ui_overlay_host_set_input_region(fixture.host, &region_a, 1) ==
            SAO_STATUS_ERR_OS_CALL_FAILED);
    CHECK(fixture.fake.set_window_rgn_calls == 2);
    CHECK(fixture.fake.set_window_pos_calls == 2);
    CHECK(region_contains(fixture.hwnd, 20, 30));
    CHECK(transparent_style(fixture.hwnd));
    CHECK(sao_ui_overlay_host_input_passthrough(fixture.host));
    CHECK(hit_test(fixture.hwnd, 40, 50) == HTTRANSPARENT);
    CHECK(sao_ui_overlay_host_input_sync_state(fixture.host) ==
          SAO_UI_OVERLAY_INPUT_PARTIAL);

    REQUIRE(sao_ui_overlay_host_set_input_region(fixture.host, nullptr, 0) ==
            SAO_STATUS_OK);
    CHECK_FALSE(region_contains(fixture.hwnd, 20, 30));
    CHECK(sao_ui_overlay_host_input_sync_state(fixture.host) ==
          SAO_UI_OVERLAY_INPUT_SYNCHRONIZED);
}

#else

TEST_CASE("overlay region transaction tests require Windows",
          "[ui][overlay_host][region][transaction]") {
    SUCCEED("Windows-only target");
}

#endif
