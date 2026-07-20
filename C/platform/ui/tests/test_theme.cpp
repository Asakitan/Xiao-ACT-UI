// SAO Auto — flat-token theme table and callback tests.
//
// Verifies the three constexpr palettes are complete, the invalid-arg
// path returns SAO_STATUS_ERR_INVALID_ARGUMENT, and the change-callback
// registry fires exactly on active-theme transitions.

#include <atomic>
#include <cstring>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/theme.h"

namespace {

// Assert the dark palette carries the exact value copied from the
// Python source of truth for a handful of anchor tokens.  If any of
// these values drift, the constexpr table is wrong.
struct RgbaProbe {
    SaoUiColorToken token;
    uint8_t r, g, b, a;
    const char* label;
};

}  // namespace

TEST_CASE("theme_get_color_dark_returns_expected", "[ui][theme][host]") {
    // Values pulled verbatim from sao_theme/colors.py and element.json.
    const RgbaProbe kProbes[] = {
        {SAO_UI_TOKEN_APP_BG,        0x0A, 0x0E, 0x14, 0xFF, "APP_BG"},
        {SAO_UI_TOKEN_APP_GOLD,      0xFF, 0xD7, 0x00, 0xFF, "APP_GOLD"},
        {SAO_UI_TOKEN_APP_ACCENT,    0x4D, 0xE8, 0xF4, 0xFF, "APP_ACCENT"},
        {SAO_UI_TOKEN_CLOSE_RED,     0xD1, 0x3D, 0x4F, 0xFF, "CLOSE_RED"},
        {SAO_UI_TOKEN_HP_GREEN_R,    0x9A, 0xD3, 0x34, 0xFF, "HP_GREEN_R"},
        {SAO_UI_TOKEN_HP_BG,         0xCD, 0xDD, 0xF8, 0x80, "HP_BG"},
        {SAO_UI_TOKEN_ALERT_SHADOW,  0x00, 0x00, 0x00, 0x22, "ALERT_SHADOW"},
        {SAO_UI_TOKEN_BOSS_HP_RED,   0xEF, 0x68, 0x4E, 0xFF, "BOSS_HP_RED"},
        {SAO_UI_TOKEN_ELEM_FIRE,     0xFF, 0x6B, 0x35, 0xFF, "ELEM_FIRE"},
        {SAO_UI_TOKEN_ELEM_ELECTRIC, 0xB4, 0x5A, 0xFF, 0xFF, "ELEM_ELECTRIC"},
        {SAO_UI_TOKEN_TRANSPARENT_KEY, 0x01, 0x01, 0x01, 0xFF, "TRANSPARENT_KEY"},
        {SAO_UI_TOKEN_DPS_GOLD,      0xFF, 0xD7, 0x00, 0xFF, "DPS_GOLD"},
    };

    for (const auto& p : kProbes) {
        SaoColorRgba rgba{};
        const auto rc = sao_ui_theme_get_color_by_id(SAO_UI_THEME_DARK, p.token, &rgba);
        INFO("token=" << p.label);
        REQUIRE(rc == SAO_STATUS_OK);
        REQUIRE(int(rgba.r) == int(p.r));
        REQUIRE(int(rgba.g) == int(p.g));
        REQUIRE(int(rgba.b) == int(p.b));
        REQUIRE(int(rgba.a) == int(p.a));
    }
}

TEST_CASE("theme_get_color_light_all_populated", "[ui][theme][host]") {
    for (int32_t i = 0; i < SAO_UI_COLOR_TOKEN_COUNT; ++i) {
        SaoColorRgba rgba{};
        const auto rc = sao_ui_theme_get_color_by_id(
            SAO_UI_THEME_LIGHT, static_cast<SaoUiColorToken>(i), &rgba);
        REQUIRE(rc == SAO_STATUS_OK);
        INFO("light token index=" << i);
        REQUIRE(int(rgba.a) != 0);
    }
}

TEST_CASE("theme_get_color_glass_all_populated", "[ui][theme][host]") {
    for (int32_t i = 0; i < SAO_UI_COLOR_TOKEN_COUNT; ++i) {
        SaoColorRgba rgba{};
        const auto rc = sao_ui_theme_get_color_by_id(
            SAO_UI_THEME_GLASS, static_cast<SaoUiColorToken>(i), &rgba);
        REQUIRE(rc == SAO_STATUS_OK);
        INFO("glass token index=" << i);
        REQUIRE(int(rgba.a) != 0);
    }
}

TEST_CASE("theme_get_metric_returns_valid", "[ui][theme][host]") {
    // A handful of anchor metrics must return the concrete values from
    // sao_theme/colors.py + widget-kit spacing scale.
    struct MetricProbe {
        SaoUiMetricToken metric;
        int32_t expected;
        const char* label;
    };
    const MetricProbe kProbes[] = {
        {SAO_UI_METRIC_BORDER_RADIUS_SMALL,  4,  "BORDER_RADIUS_SMALL"},
        {SAO_UI_METRIC_BORDER_RADIUS_MEDIUM, 8,  "BORDER_RADIUS_MEDIUM"},
        {SAO_UI_METRIC_MENU_BTN_SIZE,        46, "MENU_BTN_SIZE"},
        {SAO_UI_METRIC_MENU_BTN_MAX_SIZE,    62, "MENU_BTN_MAX_SIZE"},
        {SAO_UI_METRIC_MENU_SLOT,            70, "MENU_SLOT"},
        {SAO_UI_METRIC_HUD_MARGIN,           18, "HUD_MARGIN"},
        {SAO_UI_METRIC_HUD_PAD,               8, "HUD_PAD"},
    };
    for (const auto& p : kProbes) {
        int32_t value = -1;
        const auto rc = sao_ui_theme_get_metric_by_id(
            SAO_UI_THEME_DARK, p.metric, &value);
        INFO("metric=" << p.label);
        REQUIRE(rc == SAO_STATUS_OK);
        REQUIRE(value == p.expected);
        REQUIRE(value > 0);
    }
    // Sweep the rest — none may be zero.
    for (int32_t i = 0; i < SAO_UI_METRIC_TOKEN_COUNT; ++i) {
        int32_t value = -1;
        const auto rc = sao_ui_theme_get_metric_by_id(
            SAO_UI_THEME_DARK, static_cast<SaoUiMetricToken>(i), &value);
        REQUIRE(rc == SAO_STATUS_OK);
        REQUIRE(value > 0);
    }
}

TEST_CASE("theme_set_get_active_roundtrip", "[ui][theme][host]") {
    // Save so the test doesn't perturb sibling suites.
    SaoUiThemeId saved = SAO_UI_THEME_DARK;
    REQUIRE(sao_ui_theme_get_active_id(&saved) == SAO_STATUS_OK);

    for (auto tid : {SAO_UI_THEME_DARK, SAO_UI_THEME_LIGHT, SAO_UI_THEME_GLASS}) {
        REQUIRE(sao_ui_theme_set_active_id(tid) == SAO_STATUS_OK);
        SaoUiThemeId got = SAO_UI_THEME_COUNT;
        REQUIRE(sao_ui_theme_get_active_id(&got) == SAO_STATUS_OK);
        REQUIRE(got == tid);
    }

    REQUIRE(sao_ui_theme_set_active_id(saved) == SAO_STATUS_OK);
}

TEST_CASE("theme_invalid_token_returns_error", "[ui][theme][host]") {
    SaoColorRgba rgba{0xAA, 0xBB, 0xCC, 0xDD};

    // Negative token.
    auto rc1 = sao_ui_theme_get_color_by_id(
        SAO_UI_THEME_DARK, static_cast<SaoUiColorToken>(-1), &rgba);
    REQUIRE(rc1 == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // == COUNT.
    auto rc2 = sao_ui_theme_get_color_by_id(
        SAO_UI_THEME_DARK, static_cast<SaoUiColorToken>(SAO_UI_TOKEN_COUNT), &rgba);
    REQUIRE(rc2 == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // > COUNT.
    auto rc3 = sao_ui_theme_get_color_by_id(
        SAO_UI_THEME_DARK, static_cast<SaoUiColorToken>(SAO_UI_TOKEN_COUNT + 10), &rgba);
    REQUIRE(rc3 == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Null out pointer.
    auto rc4 = sao_ui_theme_get_color_by_id(
        SAO_UI_THEME_DARK, SAO_UI_TOKEN_APP_BG, nullptr);
    REQUIRE(rc4 == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Same for metrics.
    int32_t value = 0;
    auto rc5 = sao_ui_theme_get_metric_by_id(
        SAO_UI_THEME_DARK, static_cast<SaoUiMetricToken>(-1), &value);
    REQUIRE(rc5 == SAO_STATUS_ERR_INVALID_ARGUMENT);
    auto rc6 = sao_ui_theme_get_metric_by_id(
        SAO_UI_THEME_DARK, static_cast<SaoUiMetricToken>(SAO_UI_METRIC_COUNT), &value);
    REQUIRE(rc6 == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("theme_invalid_theme_id_returns_error", "[ui][theme][host]") {
    SaoColorRgba rgba{};
    int32_t value = 0;

    REQUIRE(sao_ui_theme_get_color_by_id(
                static_cast<SaoUiThemeId>(-1), SAO_UI_TOKEN_APP_BG, &rgba)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_theme_get_color_by_id(
                static_cast<SaoUiThemeId>(SAO_UI_THEME_COUNT), SAO_UI_TOKEN_APP_BG, &rgba)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);

    REQUIRE(sao_ui_theme_get_metric_by_id(
                static_cast<SaoUiThemeId>(-1), SAO_UI_METRIC_MENU_SLOT, &value)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_theme_get_metric_by_id(
                static_cast<SaoUiThemeId>(SAO_UI_THEME_COUNT), SAO_UI_METRIC_MENU_SLOT, &value)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);

    REQUIRE(sao_ui_theme_set_active_id(static_cast<SaoUiThemeId>(-1))
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_theme_set_active_id(static_cast<SaoUiThemeId>(SAO_UI_THEME_COUNT))
            == SAO_STATUS_ERR_INVALID_ARGUMENT);

    REQUIRE(sao_ui_theme_get_active_id(nullptr) == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

namespace {

struct CallbackFixture {
    static void reset() { s_count = 0; s_last = SAO_UI_THEME_COUNT; }
    static void SAO_UI_CALL cb(SaoUiThemeId id, void* user) {
        (void)user;
        s_last = id;
        ++s_count;
    }
    static std::atomic<int> s_count;
    static std::atomic<int> s_last;
};

std::atomic<int> CallbackFixture::s_count{0};
std::atomic<int> CallbackFixture::s_last{SAO_UI_THEME_COUNT};

}  // namespace

TEST_CASE("theme_change_callback_fires_on_set_active", "[ui][theme][host]") {
    // Prime to a known baseline.
    REQUIRE(sao_ui_theme_set_active_id(SAO_UI_THEME_DARK) == SAO_STATUS_OK);
    CallbackFixture::reset();

    sao_ui_theme_callback_handle_t handle = SAO_UI_THEME_CALLBACK_HANDLE_INVALID;
    REQUIRE(sao_ui_theme_register_change_callback(
                &CallbackFixture::cb, nullptr, &handle) == SAO_STATUS_OK);
    REQUIRE(handle != SAO_UI_THEME_CALLBACK_HANDLE_INVALID);

    // Same-theme set-active is a no-op → no callback fires.
    REQUIRE(sao_ui_theme_set_active_id(SAO_UI_THEME_DARK) == SAO_STATUS_OK);
    REQUIRE(CallbackFixture::s_count.load() == 0);

    // Real transition — one fire.
    REQUIRE(sao_ui_theme_set_active_id(SAO_UI_THEME_LIGHT) == SAO_STATUS_OK);
    REQUIRE(CallbackFixture::s_count.load() == 1);
    REQUIRE(CallbackFixture::s_last.load() == SAO_UI_THEME_LIGHT);

    // Another transition — second fire.
    REQUIRE(sao_ui_theme_set_active_id(SAO_UI_THEME_GLASS) == SAO_STATUS_OK);
    REQUIRE(CallbackFixture::s_count.load() == 2);
    REQUIRE(CallbackFixture::s_last.load() == SAO_UI_THEME_GLASS);

    // Unregister — subsequent transitions no longer fire.
    REQUIRE(sao_ui_theme_unregister_change_callback(handle) == SAO_STATUS_OK);
    REQUIRE(sao_ui_theme_set_active_id(SAO_UI_THEME_DARK) == SAO_STATUS_OK);
    REQUIRE(CallbackFixture::s_count.load() == 2);

    // Double-unregister is not found.
    REQUIRE(sao_ui_theme_unregister_change_callback(handle)
            == SAO_STATUS_ERR_NOT_FOUND);

    // Invalid handle rejected.
    REQUIRE(sao_ui_theme_unregister_change_callback(
                SAO_UI_THEME_CALLBACK_HANDLE_INVALID)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Null callback rejected.
    sao_ui_theme_callback_handle_t bogus = 42;
    REQUIRE(sao_ui_theme_register_change_callback(nullptr, nullptr, &bogus)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(bogus == SAO_UI_THEME_CALLBACK_HANDLE_INVALID);
}

TEST_CASE("theme_token_name_returns_nonempty", "[ui][theme][host]") {
    struct NameProbe {
        SaoUiColorToken token;
        const char* expected;
    };
    const NameProbe kProbes[] = {
        {SAO_UI_TOKEN_APP_BG,       "APP_BG"},
        {SAO_UI_TOKEN_APP_GOLD,     "APP_GOLD"},
        {SAO_UI_TOKEN_ALERT_SHADOW, "ALERT_SHADOW"},
        {SAO_UI_TOKEN_BOSS_HP_RED,  "BOSS_HP_RED"},
        {SAO_UI_TOKEN_ELEM_GENERIC, "ELEM_GENERIC"},
    };
    for (const auto& p : kProbes) {
        char buf[48] = {};
        const auto rc = sao_ui_theme_get_token_name(p.token, buf, sizeof(buf));
        INFO("expected=" << p.expected);
        REQUIRE(rc == SAO_STATUS_OK);
        REQUIRE(std::strcmp(buf, p.expected) == 0);
        REQUIRE(std::strlen(buf) > 0);
    }

    // Sweep for non-empty coverage — every token must produce a name.
    for (int32_t i = 0; i < SAO_UI_COLOR_TOKEN_COUNT; ++i) {
        char buf[64] = {};
        const auto rc = sao_ui_theme_get_token_name(
            static_cast<SaoUiColorToken>(i), buf, sizeof(buf));
        REQUIRE(rc == SAO_STATUS_OK);
        REQUIRE(buf[0] != '\0');
    }

    // Undersized buffer → BUFFER_TOO_SMALL and a null-terminated empty string.
    char tiny[3] = {'x', 'x', 'x'};
    REQUIRE(sao_ui_theme_get_token_name(SAO_UI_TOKEN_ALERT_SHADOW, tiny, sizeof(tiny))
            == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(tiny[0] == '\0');

    // Invalid token → INVALID_ARGUMENT, empty output.
    char scratch[16] = {'q'};
    REQUIRE(sao_ui_theme_get_token_name(
                static_cast<SaoUiColorToken>(-1), scratch, sizeof(scratch))
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(scratch[0] == '\0');

    // Null out buffer → INVALID_ARGUMENT.
    REQUIRE(sao_ui_theme_get_token_name(SAO_UI_TOKEN_APP_BG, nullptr, 16)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
    // Zero capacity → INVALID_ARGUMENT.
    REQUIRE(sao_ui_theme_get_token_name(SAO_UI_TOKEN_APP_BG, scratch, 0)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("theme_static_assert_size_correctness", "[ui][theme][host]") {
    // Compile-time invariants — mirror the static_assert in theme.h.
    REQUIRE(sao_ui_theme_get_color_token_count() == 75);
    REQUIRE(sao_ui_theme_get_color_token_count() == SAO_UI_COLOR_TOKEN_COUNT);
    REQUIRE(sao_ui_theme_get_metric_token_count() == 15);
    REQUIRE(sao_ui_theme_get_metric_token_count() == SAO_UI_METRIC_TOKEN_COUNT);

    // Runtime check that the constexpr tables are visible to callers
    // via both the ARGB path (legacy resolve) and the RGBA path
    // (flat-token API) and both encode the same colour.
    for (int32_t i = 0; i < SAO_UI_COLOR_TOKEN_COUNT; ++i) {
        SaoColorRgba rgba{};
        REQUIRE(sao_ui_theme_get_color_by_id(
                    SAO_UI_THEME_DARK,
                    static_cast<SaoUiColorToken>(i),
                    &rgba) == SAO_STATUS_OK);
        const uint32_t argb = sao_ui_theme_resolve_color(
            SAO_UI_THEME_DARK, static_cast<SaoUiColorToken>(i));
        const uint32_t expected =
            (static_cast<uint32_t>(rgba.a) << 24) |
            (static_cast<uint32_t>(rgba.r) << 16) |
            (static_cast<uint32_t>(rgba.g) <<  8) |
             static_cast<uint32_t>(rgba.b);
        INFO("token index=" << i);
        REQUIRE(argb == expected);
    }
}
