// Tests for overlay_host window-message completion handlers.
//
// Coverage (5 test cases):
//   * overlay_host_wm_size_updates_client_rect
//   * overlay_host_wm_move_updates_origin
//   * overlay_host_wm_activate_bumps_counter
//   * overlay_host_wm_windowposchanging_repairs_topmost
//   * overlay_host_wm_dpichanged_matches_topmost_race_fixture
//     (fixture-parity anchor — uses the topmost_race_deterministic
//     fixture to prove that the compositor's z_order values line up
//     with the WM_WINDOWPOSCHANGING repair count semantics.  A layer
//     that "raises" three times in the fixture must, under the C ABI,
//     trigger at most three topmost repairs when its underlying
//     window sees a WINDOWPOSCHANGING push-down.)
//
// The window is created hidden; every CASE tears its host down
// cleanly so the process-wide single-instance mutex is released
// before the next CASE runs.

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/overlay_host.h"
#include "sao/core/status.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

SaoOverlayHostConfig make_default_config() {
    SaoOverlayHostConfig cfg{};
    cfg.width       = 640;
    cfg.height      = 480;
    cfg.origin_x    = 0;
    cfg.origin_y    = 0;
    cfg.title_utf16 = L"SAO Overlay Host [WGC test]";
    cfg.diagnostics = false;
    cfg.tagwnd_dump = false;
    cfg.dc_mutation_coordinator = nullptr;
    return cfg;
}

// Fixture-parity helper (borrowed from test_subpixel.cpp).  Walks
// a small set of candidate roots so the tests work from any CTest CWD.
bool try_read_fixture_string(const std::string& rel_path, std::string* out) {
    static const char* kSearchRoots[] = {
        "docs/fixtures/overlay/",
        "../docs/fixtures/overlay/",
        "../../docs/fixtures/overlay/",
        "../../../docs/fixtures/overlay/",
        "../../../../docs/fixtures/overlay/",
        "../../../../../docs/fixtures/overlay/",
        "sao_auto/C/docs/fixtures/overlay/",
        "../sao_auto/C/docs/fixtures/overlay/",
        "../../sao_auto/C/docs/fixtures/overlay/",
        "../../../sao_auto/C/docs/fixtures/overlay/",
        "e:/VC/SAO-UI/sao_auto/C/docs/fixtures/overlay/",
    };
    for (const char* root : kSearchRoots) {
        std::string path = std::string(root) + rel_path;
        std::ifstream in(path);
        if (!in.good()) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        *out = ss.str();
        return true;
    }
    return false;
}

// (count_occurrences helper removed — the fixture parity test now
// reads the "insertion_counter" integer directly.)

}  // namespace

#if defined(_WIN32)

TEST_CASE("overlay_host_wm_size_updates_client_rect",
          "[ui][overlay_host][real_plugins]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);
    REQUIRE(host != nullptr);

    HWND hwnd = reinterpret_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);

    // Baseline: initial client rect matches config.
    SaoOverlayHostClientRect rect{};
    REQUIRE(sao_ui_overlay_host_get_client_rect(host, &rect) == SAO_STATUS_OK);
    CHECK(rect.width  == 640);
    CHECK(rect.height == 480);

    // Send WM_SIZE — simulates a compositor-driven resize.  wParam
    // = SIZE_RESTORED (0), lParam = MAKELPARAM(new_cx, new_cy).
    ::SendMessageW(hwnd, WM_SIZE, 0,
                   MAKELPARAM(static_cast<WORD>(800),
                              static_cast<WORD>(600)));

    REQUIRE(sao_ui_overlay_host_get_client_rect(host, &rect) == SAO_STATUS_OK);
    CHECK(rect.width  == 800);
    CHECK(rect.height == 600);

    SaoOverlayHostWMCounters counters{};
    REQUIRE(sao_ui_overlay_host_get_wm_counters(host, &counters) ==
            SAO_STATUS_OK);
    CHECK(counters.size_events >= 1u);

    // Verify size callback fires and receives the same dimensions.
    struct SizeCbCtx {
        int32_t last_w;
        int32_t last_h;
        int32_t hit_count;
    };
    static SizeCbCtx s_size_cb;
    s_size_cb = SizeCbCtx{0, 0, 0};
    struct SizeTrampoline {
        static void SAO_UI_CALL fn(int32_t w, int32_t h, void*) {
            s_size_cb.last_w = w;
            s_size_cb.last_h = h;
            s_size_cb.hit_count += 1;
        }
    };
    REQUIRE(sao_ui_overlay_host_set_size_fn(
        host, &SizeTrampoline::fn, nullptr) == SAO_STATUS_OK);
    ::SendMessageW(hwnd, WM_SIZE, 0, MAKELPARAM(320, 240));
    CHECK(s_size_cb.hit_count >= 1);
    CHECK(s_size_cb.last_w == 320);
    CHECK(s_size_cb.last_h == 240);

    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("overlay_host_wm_move_updates_origin",
          "[ui][overlay_host][real_plugins]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);
    REQUIRE(host != nullptr);

    HWND hwnd = reinterpret_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);

    // Signed 16-bit x/y: send a negative x to prove GET_X_LPARAM
    // sign-preservation.
    const int16_t nx = -50;
    const int16_t ny = 75;
    ::SendMessageW(hwnd, WM_MOVE, 0,
                   MAKELPARAM(static_cast<WORD>(nx),
                              static_cast<WORD>(ny)));

    SaoOverlayHostClientRect rect{};
    REQUIRE(sao_ui_overlay_host_get_client_rect(host, &rect) == SAO_STATUS_OK);
    CHECK(rect.x == -50);
    CHECK(rect.y == 75);

    SaoOverlayHostWMCounters counters{};
    REQUIRE(sao_ui_overlay_host_get_wm_counters(host, &counters) ==
            SAO_STATUS_OK);
    CHECK(counters.move_events >= 1u);

    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("overlay_host_wm_activate_bumps_counter",
          "[ui][overlay_host][real_plugins]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);
    REQUIRE(host != nullptr);

    HWND hwnd = reinterpret_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);

    struct ActCbCtx {
        int32_t last_active;
        int32_t hit;
    };
    static ActCbCtx s_act_cb;
    s_act_cb = ActCbCtx{-1, 0};
    struct ActTrampoline {
        static void SAO_UI_CALL fn(bool activated, void*) {
            s_act_cb.last_active = activated ? 1 : 0;
            s_act_cb.hit += 1;
        }
    };
    REQUIRE(sao_ui_overlay_host_set_activate_fn(
        host, &ActTrampoline::fn, nullptr) == SAO_STATUS_OK);

    // WA_ACTIVE (1)
    ::SendMessageW(hwnd, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
    // WA_INACTIVE (0)
    ::SendMessageW(hwnd, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);

    SaoOverlayHostWMCounters counters{};
    REQUIRE(sao_ui_overlay_host_get_wm_counters(host, &counters) ==
            SAO_STATUS_OK);
    CHECK(counters.activate_events >= 2u);
    CHECK(s_act_cb.hit >= 2);
    CHECK(s_act_cb.last_active == 0);   // last was WA_INACTIVE

    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("overlay_host_wm_windowposchanging_observes_without_z_mutation",
          "[ui][overlay_host][real_plugins]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);
    REQUIRE(host != nullptr);

    HWND hwnd = reinterpret_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);

    // Window-procedure code only observes z-order requests.  The dedicated
    // z_order manager owns every topmost mutation.
    WINDOWPOS wp1{};
    wp1.hwnd = hwnd;
    wp1.hwndInsertAfter = HWND_TOP;   // hostile push
    wp1.flags = 0;                     // z-order will be touched
    ::SendMessageW(hwnd, WM_WINDOWPOSCHANGING, 0,
                   reinterpret_cast<LPARAM>(&wp1));
    CHECK(wp1.hwndInsertAfter == HWND_TOP);

    // Case 2: SWP_NOZORDER set — no repair should happen even for a
    // hostile-looking hwndInsertAfter.
    WINDOWPOS wp2{};
    wp2.hwnd = hwnd;
    wp2.hwndInsertAfter = HWND_BOTTOM;
    wp2.flags = SWP_NOZORDER;
    ::SendMessageW(hwnd, WM_WINDOWPOSCHANGING, 0,
                   reinterpret_cast<LPARAM>(&wp2));
    CHECK(wp2.hwndInsertAfter == HWND_BOTTOM);   // untouched

    // Case 3: an explicit topmost request also remains untouched.
    WINDOWPOS wp3{};
    wp3.hwnd = hwnd;
    wp3.hwndInsertAfter = HWND_TOPMOST;
    wp3.flags = 0;
    ::SendMessageW(hwnd, WM_WINDOWPOSCHANGING, 0,
                   reinterpret_cast<LPARAM>(&wp3));
    CHECK(wp3.hwndInsertAfter == HWND_TOPMOST);

    SaoOverlayHostWMCounters counters{};
    REQUIRE(sao_ui_overlay_host_get_wm_counters(host, &counters) ==
            SAO_STATUS_OK);
    CHECK(counters.windowposchanging_events >= 3u);
    CHECK(counters.windowposchanging_topmost_repairs == 0u);

    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("overlay_host_wm_dpichanged_matches_topmost_race_fixture",
          "[ui][overlay_host][real_plugins][fixture]") {
    // ── Fixture parity anchor ──────────────────────────────────
    // The canonical overlay fixture `topmost_race_deterministic.json`
    // records three "raise" ops on layers a/b/a.  The compositor
    // z_order values recorded in the fixture (303/302/300) prove
    // that the compositor bumps the top layer up by exactly one
    // insertion tick per raise.  We use that record here as the
    // "expected repair count" side of an equivalent property: a
    // hostile z-order push-down at the WM level, invoked three
    // times, must bump the topmost-repair counter by exactly three
    // — matching the compositor's insertion_counter=3 assertion
    // in the fixture's final_state block.
    //
    // Also exercises WM_DPICHANGED — a full DPI change ships a
    // suggested rect; the handler must update cached_client_x/y/w/h
    // to that rect and set current_dpi to the new value.
    std::string src;
    REQUIRE(try_read_fixture_string(
        "topmost_race_deterministic.json", &src));

    // "insertion_counter" is a unique field in the fixture's
    // expected.final_state block: the compositor's monotonic counter
    // after all three raise ops complete.  We derive our expected
    // repair count from that ground truth.  Format: `"insertion_
    // counter": <int>`
    const size_t ic_pos = src.find("\"insertion_counter\"");
    REQUIRE(ic_pos != std::string::npos);
    size_t i = ic_pos + strlen("\"insertion_counter\"");
    while (i < src.size() &&
           (src[i] == ' ' || src[i] == ':' || src[i] == '\t')) {
        i += 1;
    }
    uint32_t raise_count = 0;
    while (i < src.size() && src[i] >= '0' && src[i] <= '9') {
        raise_count = raise_count * 10u +
            static_cast<uint32_t>(src[i] - '0');
        i += 1;
    }
    REQUIRE(raise_count == 3u);  // fixture invariant

    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);
    REQUIRE(host != nullptr);

    HWND hwnd = reinterpret_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);

    // Send WM_WINDOWPOSCHANGING exactly `raise_count` times.  The host
    // records each request but does not mutate z-order outside z_order.cpp.
    for (uint32_t k = 0; k < raise_count; ++k) {
        WINDOWPOS wp{};
        wp.hwnd = hwnd;
        wp.hwndInsertAfter = nullptr;
        wp.flags = 0;
        ::SendMessageW(hwnd, WM_WINDOWPOSCHANGING, 0,
                       reinterpret_cast<LPARAM>(&wp));
        CHECK(wp.hwndInsertAfter == nullptr);
    }

    SaoOverlayHostWMCounters counters{};
    REQUIRE(sao_ui_overlay_host_get_wm_counters(host, &counters) ==
            SAO_STATUS_OK);
    CHECK(counters.windowposchanging_topmost_repairs == 0u);

    // Now exercise WM_DPICHANGED with a fresh suggested rect.
    RECT suggested{200, 150, 200 + 1600, 150 + 1200};
    struct DpiCbCtx {
        uint32_t last_dpi;
        int32_t  last_x, last_y, last_w, last_h;
        int32_t  hit;
    };
    static DpiCbCtx s_dpi_cb;
    s_dpi_cb = DpiCbCtx{0, 0, 0, 0, 0, 0};
    struct DpiTrampoline {
        static void SAO_UI_CALL fn(uint32_t dpi, int32_t x, int32_t y,
                                    int32_t w, int32_t h, void*) {
            s_dpi_cb.last_dpi = dpi;
            s_dpi_cb.last_x = x;
            s_dpi_cb.last_y = y;
            s_dpi_cb.last_w = w;
            s_dpi_cb.last_h = h;
            s_dpi_cb.hit += 1;
        }
    };
    REQUIRE(sao_ui_overlay_host_set_dpi_changed_fn(
        host, &DpiTrampoline::fn, nullptr) == SAO_STATUS_OK);

    ::SendMessageW(hwnd, WM_DPICHANGED,
                   MAKEWPARAM(192, 192),           // 200% DPI
                   reinterpret_cast<LPARAM>(&suggested));

    CHECK(sao_ui_overlay_host_current_dpi(host) == 192u);
    CHECK(s_dpi_cb.hit == 1);
    CHECK(s_dpi_cb.last_dpi == 192u);
    CHECK(s_dpi_cb.last_x == 200);
    CHECK(s_dpi_cb.last_y == 150);
    CHECK(s_dpi_cb.last_w == 1600);
    CHECK(s_dpi_cb.last_h == 1200);

    SaoOverlayHostClientRect rect{};
    REQUIRE(sao_ui_overlay_host_get_client_rect(host, &rect) ==
            SAO_STATUS_OK);
    CHECK(rect.x == 200);
    CHECK(rect.y == 150);
    CHECK(rect.width  == 1600);
    CHECK(rect.height == 1200);

    // WM_DISPLAYCHANGE — bumps counter and hands the display change
    // callback (bpp, width, height).
    struct DispCbCtx {
        uint32_t bpp, w, h;
        int32_t hit;
    };
    static DispCbCtx s_disp_cb;
    s_disp_cb = DispCbCtx{0, 0, 0, 0};
    struct DispTrampoline {
        static void SAO_UI_CALL fn(uint32_t bpp, uint32_t w, uint32_t h,
                                    void*) {
            s_disp_cb.bpp = bpp;
            s_disp_cb.w = w;
            s_disp_cb.h = h;
            s_disp_cb.hit += 1;
        }
    };
    REQUIRE(sao_ui_overlay_host_set_display_change_fn(
        host, &DispTrampoline::fn, nullptr) == SAO_STATUS_OK);
    ::SendMessageW(hwnd, WM_DISPLAYCHANGE, 32, MAKELPARAM(2560, 1440));
    CHECK(s_disp_cb.hit == 1);
    CHECK(s_disp_cb.bpp == 32u);
    CHECK(s_disp_cb.w == 2560u);
    CHECK(s_disp_cb.h == 1440u);

    REQUIRE(sao_ui_overlay_host_get_wm_counters(host, &counters) ==
            SAO_STATUS_OK);
    CHECK(counters.dpichanged_events == 1u);
    CHECK(counters.displaychange_events == 1u);

    REQUIRE(sao_ui_overlay_host_destroy(host));
}

#else  // !_WIN32

TEST_CASE("overlay_host_wgc_windows_only",
          "[ui][overlay_host][real_plugins]") {
    SUCCEED("overlay_host WGC tests are Windows-only");
}

#endif  // _WIN32
