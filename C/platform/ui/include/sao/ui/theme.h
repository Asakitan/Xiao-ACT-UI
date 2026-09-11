// SAO Auto — stable theme tokens and native palette tables.
// Ordinary surfaces use warm porcelain or graphite while semantic colors retain their roles.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
#include <iterator>  // std::size for compile-time size checks
extern "C" {
#endif

typedef struct sao_ui_theme_s* sao_ui_theme_handle_t;
typedef uint64_t sao_ui_theme_owner_t;

// ── Canonical RGBA vector + change-callback handle ────────────────
// Kept alongside the ARGB uint32_t path so both encodings are cheap.
//
// RGBA byte layout (r, g, b, a) mirrors 32-bit texture uploads and the
// Python `_alpha_hex` / `RGBA tuple` conventions used in
// `sao_theme/theme_manager.py`.
struct SaoColorRgba {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

typedef uint64_t sao_ui_theme_callback_handle_t;
#define SAO_UI_THEME_CALLBACK_HANDLE_INVALID ((sao_ui_theme_callback_handle_t)0)

// ── Theme identity ────────────────────────────────────────────────
enum SaoUiThemeId : int32_t {
    SAO_UI_THEME_DARK  = 0,   // Graphite surfaces
    SAO_UI_THEME_LIGHT = 1,   // Warm porcelain surfaces
    SAO_UI_THEME_GLASS = 2,   // Translucent porcelain surfaces
    SAO_UI_THEME_COUNT = 3,
};

// ── Color tokens (mirrors SAOColors class) ────────────────────────
// Each token maps to exactly one ARGB per theme and keeps its stable numeric ID.
enum SaoUiColorToken : int32_t {
    // Overlay / background
    SAO_UI_TOKEN_OVERLAY_BG                 = 0,
    SAO_UI_TOKEN_APP_BG                     = 1,
    SAO_UI_TOKEN_APP_CARD                   = 2,
    SAO_UI_TOKEN_APP_BORDER                 = 3,
    SAO_UI_TOKEN_APP_TEXT                   = 4,
    SAO_UI_TOKEN_APP_TEXT_2                 = 5,
    SAO_UI_TOKEN_APP_TEXT_DIM               = 6,
    SAO_UI_TOKEN_APP_ACCENT                 = 7,
    SAO_UI_TOKEN_APP_BLUE                   = 8,
    SAO_UI_TOKEN_APP_GREEN                  = 9,
    SAO_UI_TOKEN_APP_RED                    = 10,
    SAO_UI_TOKEN_APP_ORANGE                 = 11,
    SAO_UI_TOKEN_APP_GOLD                   = 12,

    // Circle button (menu buttons) — SAOCircleButton
    SAO_UI_TOKEN_CIRCLE_BORDER              = 13,
    SAO_UI_TOKEN_CIRCLE_BG                  = 14,
    SAO_UI_TOKEN_CIRCLE_ICON                = 15,
    SAO_UI_TOKEN_CIRCLE_ACTIVE_BORDER       = 16,
    SAO_UI_TOKEN_CIRCLE_ACTIVE_BG           = 17,
    SAO_UI_TOKEN_CIRCLE_ACTIVE_ICON         = 18,
    SAO_UI_TOKEN_CIRCLE_HOVER_BG            = 19,
    SAO_UI_TOKEN_CIRCLE_HOVER_ICON          = 20,

    // Child menu (ChildBar)
    SAO_UI_TOKEN_CHILD_BG                   = 21,
    SAO_UI_TOKEN_CHILD_HOVER                = 22,
    SAO_UI_TOKEN_CHILD_HOVER_FG             = 23,
    SAO_UI_TOKEN_CHILD_TEXT                 = 24,
    SAO_UI_TOKEN_CHILD_LINE                 = 25,
    SAO_UI_TOKEN_CHILD_ICON                 = 26,

    // Left info panel (LeftInfo)
    SAO_UI_TOKEN_INFO_BG                    = 27,
    SAO_UI_TOKEN_INFO_BOTTOM                = 28,
    SAO_UI_TOKEN_INFO_TITLE_BORDER          = 29,
    SAO_UI_TOKEN_INFO_TRIANGLE              = 30,

    // Alert / dialog
    SAO_UI_TOKEN_ALERT_BG                   = 31,
    SAO_UI_TOKEN_ALERT_PANEL                = 32,
    SAO_UI_TOKEN_ALERT_TITLE_FG             = 33,
    SAO_UI_TOKEN_ALERT_CONTENT_FG           = 34,
    SAO_UI_TOKEN_ALERT_SHADOW               = 35,
    SAO_UI_TOKEN_CLOSE_RED                  = 36,
    SAO_UI_TOKEN_OK_BLUE                    = 37,

    // HP bar
    SAO_UI_TOKEN_HP_BG                      = 38,
    SAO_UI_TOKEN_HP_HOVER                   = 39,
    SAO_UI_TOKEN_HP_FONT_COLOR              = 40,
    SAO_UI_TOKEN_HP_GREEN_L                 = 41,
    SAO_UI_TOKEN_HP_GREEN_R                 = 42,
    SAO_UI_TOKEN_HP_YELLOW_L                = 43,
    SAO_UI_TOKEN_HP_YELLOW_R                = 44,
    SAO_UI_TOKEN_HP_RED_L                   = 45,
    SAO_UI_TOKEN_HP_RED_R                   = 46,
    SAO_UI_TOKEN_HP_BORDER                  = 47,

    // Boss HP bar (raid)
    SAO_UI_TOKEN_BOSS_HP_RED                = 48,
    SAO_UI_TOKEN_BOSS_HP_BREAK              = 49,
    SAO_UI_TOKEN_BOSS_HP_SHIELD             = 50,

    // Frosted-glass HUD (identity panel)
    SAO_UI_TOKEN_SURFACE_LIGHT              = 51,
    SAO_UI_TOKEN_TEXT_PRIMARY               = 52,
    SAO_UI_TOKEN_TEXT_SECONDARY             = 53,
    SAO_UI_TOKEN_ACCENT_GOLD_WARM           = 54,
    SAO_UI_TOKEN_ACCENT_CYAN_SOFT           = 55,
    SAO_UI_TOKEN_CORNER_CYAN                = 56,
    SAO_UI_TOKEN_CORNER_GOLD                = 57,

    // Common
    SAO_UI_TOKEN_WHITE                      = 58,
    SAO_UI_TOKEN_WHITE_85                   = 59,   // alpha 0xd9
    SAO_UI_TOKEN_BLACK                      = 60,
    SAO_UI_TOKEN_TRANSPARENT_KEY            = 61,   // #010101 chroma key

    // Damage / DPS panel
    SAO_UI_TOKEN_DPS_GOLD                   = 62,
    SAO_UI_TOKEN_DPS_ROW_ALT                = 63,
    SAO_UI_TOKEN_DPS_ROW_SELF               = 64,
    SAO_UI_TOKEN_DPS_ROW_HOVER              = 65,

    // Element (damage element tint)
    SAO_UI_TOKEN_ELEM_FIRE                  = 66,
    SAO_UI_TOKEN_ELEM_WATER                 = 67,
    SAO_UI_TOKEN_ELEM_ELECTRIC              = 68,
    SAO_UI_TOKEN_ELEM_WOOD                  = 69,
    SAO_UI_TOKEN_ELEM_WIND                  = 70,
    SAO_UI_TOKEN_ELEM_ROCK                  = 71,
    SAO_UI_TOKEN_ELEM_LIGHT                 = 72,
    SAO_UI_TOKEN_ELEM_DARK                  = 73,
    SAO_UI_TOKEN_ELEM_GENERIC               = 74,

    // Semantic control states and feedback surfaces.
    SAO_UI_TOKEN_DISABLED_FG                = 75,
    SAO_UI_TOKEN_DISABLED_BG                = 76,
    SAO_UI_TOKEN_DISABLED_BORDER            = 77,
    SAO_UI_TOKEN_HOVER_SURFACE              = 78,
    SAO_UI_TOKEN_FOCUS_RING                 = 79,
    SAO_UI_TOKEN_PRESSED_SURFACE            = 80,
    SAO_UI_TOKEN_PLACEHOLDER                = 81,
    SAO_UI_TOKEN_SELECTION                  = 82,
    SAO_UI_TOKEN_SCROLLBAR_TRACK            = 83,
    SAO_UI_TOKEN_SCROLLBAR_THUMB            = 84,
    SAO_UI_TOKEN_SCROLLBAR_HOVER            = 85,
    SAO_UI_TOKEN_SCROLLBAR_PRESSED          = 86,
    SAO_UI_TOKEN_TOOLTIP_SURFACE            = 87,
    SAO_UI_TOKEN_LOADING                    = 88,
    SAO_UI_TOKEN_SKELETON                   = 89,
    SAO_UI_TOKEN_ERROR_SURFACE              = 90,
    SAO_UI_TOKEN_ERROR_ICON                 = 91,

    SAO_UI_TOKEN_COUNT                      = 92,
};

// Canonical count alias (matches G3.1 spec vocabulary).
#define SAO_UI_COLOR_TOKEN_COUNT SAO_UI_TOKEN_COUNT

// ── Static color table (compile-time constant) ────────────────────
// Each row = one theme; each column = one token.  ARGB layout:
// 0xAARRGGBB.  Populated by src/theme.cpp at translation-unit
// scope with `constexpr` — zero runtime cost.
struct SaoUiColorTable {
    uint32_t argb[SAO_UI_TOKEN_COUNT];
};

// Metric tokens (int32 values, e.g. corner radius, padding)
enum SaoUiMetricToken : int32_t {
    SAO_UI_METRIC_BORDER_RADIUS_SMALL       = 0,   // rounded_panel small
    SAO_UI_METRIC_BORDER_RADIUS_MEDIUM      = 1,
    SAO_UI_METRIC_BORDER_RADIUS_LARGE       = 2,
    SAO_UI_METRIC_PADDING_XS                = 3,
    SAO_UI_METRIC_PADDING_S                 = 4,
    SAO_UI_METRIC_PADDING_M                 = 5,
    SAO_UI_METRIC_PADDING_L                 = 6,
    SAO_UI_METRIC_GAP_S                     = 7,
    SAO_UI_METRIC_GAP_M                     = 8,
    SAO_UI_METRIC_GAP_L                     = 9,
    SAO_UI_METRIC_MENU_BTN_SIZE             = 10,  // SAOCircleButton.SIZE
    SAO_UI_METRIC_MENU_BTN_MAX_SIZE         = 11,  // SAOCircleButton.MAX_SIZE
    SAO_UI_METRIC_MENU_SLOT                 = 12,  // SAOMenuBar._SLOT (70)
    SAO_UI_METRIC_HUD_MARGIN                = 13,  // HUD bracket inset
    SAO_UI_METRIC_HUD_PAD                   = 14,  // HUD sprite margin
    SAO_UI_METRIC_CONTROL_HEIGHT_SM         = 15,
    SAO_UI_METRIC_CONTROL_HEIGHT_MD         = 16,
    SAO_UI_METRIC_CONTROL_HEIGHT_LG         = 17,
    SAO_UI_METRIC_ICON_SIZE                 = 18,
    SAO_UI_METRIC_TOUCH_TARGET              = 19,
    SAO_UI_METRIC_TABLE_ROW_HEIGHT          = 20,
    SAO_UI_METRIC_HEADER_HEIGHT             = 21,
    SAO_UI_METRIC_TOOLTIP_MAX_WIDTH         = 22,
    SAO_UI_METRIC_SCROLLBAR_WIDTH           = 23,
    SAO_UI_METRIC_SCROLLBAR_MIN_THUMB       = 24,
    SAO_UI_METRIC_SCROLLBAR_HIT_AREA        = 25,
    SAO_UI_METRIC_COUNT                     = 26,
};

// Canonical metric-count alias.
#define SAO_UI_METRIC_TOKEN_COUNT SAO_UI_METRIC_COUNT

struct SaoUiMetricTable {
    int32_t values[SAO_UI_METRIC_COUNT];
};

struct SaoUiShadowPreset {
    int32_t offset_x;
    int32_t offset_y;
    int32_t blur_radius;
    int32_t spread;
    uint8_t alpha;
    uint8_t _pad[3];
};

// ── Compile-time table access ─────────────────────────────────────
// Returns a pointer to the const table.  Callers can index directly
// or use the resolvers below.
SAO_UI_API const SaoUiColorTable* SAO_UI_CALL sao_ui_theme_static_colors(
    SaoUiThemeId theme_id);

SAO_UI_API const SaoUiMetricTable* SAO_UI_CALL sao_ui_theme_static_metrics(
    SaoUiThemeId theme_id);

// Direct resolvers (single-array-load).  Return 0xff000000 (opaque
// black) for out-of-range tokens; the handle-based API below returns
// SAO_STATUS_ERR_INVALID_ARGUMENT instead.
SAO_UI_API uint32_t SAO_UI_CALL sao_ui_theme_resolve_color(
    SaoUiThemeId theme_id, SaoUiColorToken token);

SAO_UI_API int32_t SAO_UI_CALL sao_ui_theme_resolve_metric(
    SaoUiThemeId theme_id, SaoUiMetricToken metric);

// ── Runtime handle API (theme swaps + listeners) ─────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_create(
    sao_ui_theme_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_theme_destroy(
    sao_ui_theme_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_set_active(
    sao_ui_theme_handle_t handle, SaoUiThemeId theme_id);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_get_active(
    sao_ui_theme_handle_t handle, SaoUiThemeId* out_theme_id);

// Handle-based lookups (broadcast listener updates).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_get_color(
    sao_ui_theme_handle_t handle, SaoUiColorToken token, uint32_t* out_argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_get_metric(
    sao_ui_theme_handle_t handle, SaoUiMetricToken metric, int32_t* out_value);

// Per-panel theme override (mirrors theme_manager.register_panel_theme).
// panel_key_utf8 is the plugin-side namespace (e.g. "metrics", "raid").
// Registration replaces the matching owner/panel/theme/token entry; lookup
// falls back to the active static token when no override exists.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_register_panel_override(
    sao_ui_theme_handle_t handle,
    const char* panel_key_utf8,
    SaoUiThemeId theme_id,
    SaoUiColorToken token,
    uint32_t argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_register_panel_override_for_owner(
    sao_ui_theme_handle_t handle,
    sao_ui_theme_owner_t owner,
    const char* panel_key_utf8,
    SaoUiThemeId theme_id,
    SaoUiColorToken token,
    uint32_t argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_release_owner(
    sao_ui_theme_handle_t handle,
    sao_ui_theme_owner_t owner);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_get_panel_color(
    sao_ui_theme_handle_t handle,
    const char* panel_key_utf8,
    SaoUiColorToken token,
    uint32_t* out_argb);

// JSON load — bounded transactional runtime path for user themes.  The
// documented shape is {"theme_id":"dark|light|glass|0|1|2",
// "colors":{"APP_BG":"#RRGGBB[AA]"}}.  `theme_id` is optional;
// color names may also appear at the top level.  Unknown keys are ignored,
// known colors must be valid, and a failed parse leaves the handle unchanged.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_load_json(
    sao_ui_theme_handle_t handle,
    const uint8_t* json_utf8,
    size_t json_len);

// ── Theme-change listener ─────────────────────────────────────────
// 1:1 with the Python _apply_theme GPU cache flush requirement.
typedef void (SAO_UI_CALL* sao_ui_theme_changed_callback_t)(
    SaoUiThemeId new_theme_id, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_add_listener(
    sao_ui_theme_handle_t handle,
    sao_ui_theme_changed_callback_t callback,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_remove_listener(
    sao_ui_theme_handle_t handle,
    sao_ui_theme_changed_callback_t callback,
    void* user_data);

// ── Handle-less flat-token API (G3.1) ─────────────────────────────
// Zero-cost accessors over the compile-time tables + a process-wide
// active theme + a change-callback registry.  Used by native panels
// and tests that don't need per-instance handle state.
//
// Naming convention: `_by_id` / `_id` disambiguates from the handle
// based API above (both live in the same C ABI translation unit).
// All flat-token functions return `SAO_STATUS_OK` on success and
// `SAO_STATUS_ERR_INVALID_ARGUMENT` on out-of-range enum / null.

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_get_color_by_id(
    SaoUiThemeId theme_id,
    SaoUiColorToken token,
    SaoColorRgba* out_rgba);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_get_metric_by_id(
    SaoUiThemeId theme_id,
    SaoUiMetricToken metric,
    int32_t* out_value);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_set_active_id(
    SaoUiThemeId theme_id);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_get_active_id(
    SaoUiThemeId* out_theme_id);

// Change-callback registry (process-wide).  `out_handle` receives a
// non-zero handle for later unregistration; the callback fires
// synchronously inside `sao_ui_theme_set_active_id` on the calling
// thread whenever the active theme id actually changes.
typedef void (SAO_UI_CALL* sao_ui_theme_change_callback_t)(
    SaoUiThemeId new_theme_id, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_register_change_callback(
    sao_ui_theme_change_callback_t callback,
    void* user_data,
    sao_ui_theme_callback_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_unregister_change_callback(
    sao_ui_theme_callback_handle_t handle);

// Copies the ASCII token name (e.g. "APP_BG") into `out_name` which
// must have room for `capacity` bytes including the NUL terminator.
// Returns `SAO_STATUS_ERR_BUFFER_TOO_SMALL` if the buffer is too small,
// `SAO_STATUS_ERR_INVALID_ARGUMENT` on out-of-range token / null.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_theme_get_token_name(
    SaoUiColorToken token,
    char* out_name,
    size_t capacity);

// Runtime accessor for tests that mirror the header-side static_assert.
SAO_UI_API int32_t SAO_UI_CALL sao_ui_theme_get_color_token_count(void);
SAO_UI_API int32_t SAO_UI_CALL sao_ui_theme_get_metric_token_count(void);

#ifdef __cplusplus
}  // extern "C"
#endif

// ── Constexpr colour + metric tables ──────────────────────────────
// Stable HP, element, success, and error colors remain distinct from ordinary chrome.
// Each array is indexed directly by SaoUiColorToken and keeps all 92 entries.
// Native workbenches use porcelain, neutral rules, ink, and restrained warm selection.

#ifdef __cplusplus

namespace sao::ui {

// Alias so the constexpr arrays below can name the RGBA type
// without the C-style prefix in every entry.
using SaoColorRgba = ::SaoColorRgba;

// `rgba(r,g,b)` defaults alpha to 0xFF; translucent roles use the four-argument form.
constexpr SaoColorRgba rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF) {
    return SaoColorRgba{r, g, b, a};
}

// ── Dark theme (graphite) ──────────────────────────────────────────
inline constexpr SaoColorRgba kSaoThemeDarkColors[SAO_UI_COLOR_TOKEN_COUNT] = {
    /* [ 0] OVERLAY_BG          */ rgba(0x20, 0x24, 0x21, 0xB3),
    /* [ 1] APP_BG              */ rgba(0x20, 0x24, 0x21),
    /* [ 2] APP_CARD            */ rgba(0x2B, 0x30, 0x2C),
    /* [ 3] APP_BORDER          */ rgba(0x45, 0x4B, 0x46),
    /* [ 4] APP_TEXT            */ rgba(0xEE, 0xF0, 0xE8),
    /* [ 5] APP_TEXT_2          */ rgba(0xB2, 0xB8, 0xB1),
    /* [ 6] APP_TEXT_DIM        */ rgba(0x8E, 0x95, 0x8F),
    /* [ 7] APP_ACCENT          */ rgba(0xED, 0xB4, 0x5B),
    /* [ 8] APP_BLUE            */ rgba(0x6F, 0x98, 0xA4),
    /* [ 9] APP_GREEN           */ rgba(0x4E, 0xC9, 0xB0),
    /* [10] APP_RED             */ rgba(0xF1, 0x4C, 0x4C),
    /* [11] APP_ORANGE          */ rgba(0xE3, 0xA4, 0x4B),
    /* [12] APP_GOLD            */ rgba(0xED, 0xB4, 0x5B),
    /* [13] CIRCLE_BORDER       */ rgba(0x4E, 0x55, 0x4F),
    /* [14] CIRCLE_BG           */ rgba(0x2B, 0x30, 0x2C),
    /* [15] CIRCLE_ICON         */ rgba(0xC4, 0xCA, 0xC2),
    /* [16] CIRCLE_ACTIVE_BORDER*/ rgba(0xED, 0xB4, 0x5B),
    /* [17] CIRCLE_ACTIVE_BG    */ rgba(0xED, 0xB4, 0x5B),
    /* [18] CIRCLE_ACTIVE_ICON  */ rgba(0x20, 0x24, 0x21),
    /* [19] CIRCLE_HOVER_BG     */ rgba(0x39, 0x36, 0x2F),
    /* [20] CIRCLE_HOVER_ICON   */ rgba(0xF0, 0xD3, 0xA4),
    /* [21] CHILD_BG            */ rgba(0x2B, 0x30, 0x2C),
    /* [22] CHILD_HOVER         */ rgba(0x39, 0x36, 0x2F),
    /* [23] CHILD_HOVER_FG      */ rgba(0xEE, 0xF0, 0xE8),
    /* [24] CHILD_TEXT          */ rgba(0xEE, 0xF0, 0xE8),
    /* [25] CHILD_LINE          */ rgba(0x45, 0x4B, 0x46),
    /* [26] CHILD_ICON          */ rgba(0xBC, 0xC2, 0xBA),
    /* [27] INFO_BG             */ rgba(0x2B, 0x30, 0x2C),
    /* [28] INFO_BOTTOM         */ rgba(0x25, 0x2A, 0x26),
    /* [29] INFO_TITLE_BORDER   */ rgba(0x45, 0x4B, 0x46),
    /* [30] INFO_TRIANGLE       */ rgba(0x2B, 0x30, 0x2C),
    /* [31] ALERT_BG            */ rgba(0x20, 0x24, 0x21),
    /* [32] ALERT_PANEL         */ rgba(0x2B, 0x30, 0x2C),
    /* [33] ALERT_TITLE_FG      */ rgba(0xEE, 0xF0, 0xE8),
    /* [34] ALERT_CONTENT_FG    */ rgba(0xB2, 0xB8, 0xB1),
    /* [35] ALERT_SHADOW        */ rgba(0x00, 0x00, 0x00, 0x30),
    /* [36] CLOSE_RED           */ rgba(0xD1, 0x3D, 0x4F),
    /* [37] OK_BLUE             */ rgba(0xED, 0xB4, 0x5B),
    /* [38] HP_BG               */ rgba(0xCD, 0xDD, 0xF8, 0x80),
    /* [39] HP_HOVER            */ rgba(0xE5, 0xE7, 0xEC, 0x99),
    /* [40] HP_FONT_COLOR       */ rgba(0xE1, 0xDE, 0xDE),
    /* [41] HP_GREEN_L          */ rgba(0xD3, 0xEA, 0x7C),
    /* [42] HP_GREEN_R          */ rgba(0x9A, 0xD3, 0x34),
    /* [43] HP_YELLOW_L         */ rgba(0xEB, 0xEE, 0x70),
    /* [44] HP_YELLOW_R         */ rgba(0xF4, 0xFA, 0x49),
    /* [45] HP_RED_L            */ rgba(0xF8, 0x8C, 0x7A),
    /* [46] HP_RED_R            */ rgba(0xEF, 0x68, 0x4E),
    /* [47] HP_BORDER           */ rgba(0xDA, 0xD7, 0xD7),
    /* [48] BOSS_HP_RED         */ rgba(0xEF, 0x68, 0x4E),
    /* [49] BOSS_HP_BREAK       */ rgba(0xD4, 0x9C, 0x17),
    /* [50] BOSS_HP_SHIELD      */ rgba(0x48, 0x9C, 0xE8, 0x6B),
    /* [51] SURFACE_LIGHT       */ rgba(0x2B, 0x30, 0x2C),
    /* [52] TEXT_PRIMARY        */ rgba(0xEE, 0xF0, 0xE8),
    /* [53] TEXT_SECONDARY      */ rgba(0xAA, 0xB0, 0xA9),
    /* [54] ACCENT_GOLD_WARM    */ rgba(0xED, 0xB4, 0x5B),
    /* [55] ACCENT_CYAN_SOFT    */ rgba(0xAA, 0xB0, 0xA9),
    /* [56] CORNER_CYAN         */ rgba(0x77, 0x7E, 0x78),
    /* [57] CORNER_GOLD         */ rgba(0xED, 0xB4, 0x5B),
    /* [58] WHITE               */ rgba(0xFA, 0xF9, 0xF6),
    /* [59] WHITE_85            */ rgba(0xFA, 0xF9, 0xF6, 0xD9),
    /* [60] BLACK               */ rgba(0x20, 0x24, 0x21),
    /* [61] TRANSPARENT_KEY     */ rgba(0x01, 0x01, 0x01),
    /* [62] DPS_GOLD            */ rgba(0xED, 0xB4, 0x5B),
    /* [63] DPS_ROW_ALT         */ rgba(0x25, 0x2A, 0x26, 0x80),
    /* [64] DPS_ROW_SELF        */ rgba(0x45, 0x3A, 0x2A, 0xB3),
    /* [65] DPS_ROW_HOVER       */ rgba(0xED, 0xB4, 0x5B, 0x40),
    /* [66] ELEM_FIRE           */ rgba(0xFF, 0x6B, 0x35),
    /* [67] ELEM_WATER          */ rgba(0x2E, 0x9B, 0xFF),
    /* [68] ELEM_ELECTRIC       */ rgba(0xB4, 0x5A, 0xFF),
    /* [69] ELEM_WOOD           */ rgba(0x3F, 0xBF, 0x5F),
    /* [70] ELEM_WIND           */ rgba(0x46, 0xE0, 0xB0),
    /* [71] ELEM_ROCK           */ rgba(0xC8, 0x92, 0x3C),
    /* [72] ELEM_LIGHT          */ rgba(0xFF, 0xD9, 0x5A),
    /* [73] ELEM_DARK           */ rgba(0x9B, 0x6B, 0xD6),
    /* [74] ELEM_GENERIC        */ rgba(0xB0, 0xB8, 0xC4),
    /* [75] DISABLED_FG         */ rgba(0x85, 0x8C, 0x86),
    /* [76] DISABLED_BG         */ rgba(0x25, 0x2A, 0x26),
    /* [77] DISABLED_BORDER     */ rgba(0x3C, 0x42, 0x3D),
    /* [78] HOVER_SURFACE       */ rgba(0x39, 0x36, 0x2F),
    /* [79] FOCUS_RING          */ rgba(0xED, 0xB4, 0x5B),
    /* [80] PRESSED_SURFACE     */ rgba(0x4A, 0x3B, 0x27),
    /* [81] PLACEHOLDER         */ rgba(0x85, 0x8C, 0x86),
    /* [82] SELECTION           */ rgba(0x6A, 0x4E, 0x28),
    /* [83] SCROLLBAR_TRACK     */ rgba(0x30, 0x35, 0x30),
    /* [84] SCROLLBAR_THUMB     */ rgba(0x62, 0x68, 0x62),
    /* [85] SCROLLBAR_HOVER     */ rgba(0x8F, 0x96, 0x8F),
    /* [86] SCROLLBAR_PRESSED   */ rgba(0xED, 0xB4, 0x5B),
    /* [87] TOOLTIP_SURFACE     */ rgba(0x2B, 0x30, 0x2C),
    /* [88] LOADING             */ rgba(0x4A, 0x3C, 0x29),
    /* [89] SKELETON            */ rgba(0x35, 0x3A, 0x36),
    /* [90] ERROR_SURFACE       */ rgba(0xF1, 0x4C, 0x4C, 0x24),
    /* [91] ERROR_ICON          */ rgba(0xF1, 0x4C, 0x4C),
};

// ── Light theme (warm porcelain) ──────────────────────────────────
inline constexpr SaoColorRgba kSaoThemeLightColors[SAO_UI_COLOR_TOKEN_COUNT] = {
    /* [ 0] OVERLAY_BG          */ rgba(0x23, 0x27, 0x24, 0xB3),
    /* [ 1] APP_BG              */ rgba(0xEE, 0xEA, 0xE4),
    /* [ 2] APP_CARD            */ rgba(0xFA, 0xF9, 0xF6),
    /* [ 3] APP_BORDER          */ rgba(0xD6, 0xD6, 0xCE),
    /* [ 4] APP_TEXT            */ rgba(0x23, 0x27, 0x24),
    /* [ 5] APP_TEXT_2          */ rgba(0x64, 0x6A, 0x65),
    /* [ 6] APP_TEXT_DIM        */ rgba(0x85, 0x8A, 0x85),
    /* [ 7] APP_ACCENT          */ rgba(0xD9, 0x95, 0x36),
    /* [ 8] APP_BLUE            */ rgba(0x4C, 0x74, 0x84),
    /* [ 9] APP_GREEN           */ rgba(0x2E, 0x7D, 0x32),
    /* [10] APP_RED             */ rgba(0xC7, 0x2E, 0x2E),
    /* [11] APP_ORANGE          */ rgba(0x9D, 0x62, 0x17),
    /* [12] APP_GOLD            */ rgba(0x96, 0x61, 0x1F),
    /* [13] CIRCLE_BORDER       */ rgba(0xD6, 0xD6, 0xCE),
    /* [14] CIRCLE_BG           */ rgba(0xFA, 0xF9, 0xF6),
    /* [15] CIRCLE_ICON         */ rgba(0x64, 0x6A, 0x65),
    /* [16] CIRCLE_ACTIVE_BORDER*/ rgba(0xD9, 0x95, 0x36),
    /* [17] CIRCLE_ACTIVE_BG    */ rgba(0xD9, 0x95, 0x36),
    /* [18] CIRCLE_ACTIVE_ICON  */ rgba(0x23, 0x27, 0x24),
    /* [19] CIRCLE_HOVER_BG     */ rgba(0xF2, 0xE3, 0xCE),
    /* [20] CIRCLE_HOVER_ICON   */ rgba(0x6C, 0x4B, 0x22),
    /* [21] CHILD_BG            */ rgba(0xFA, 0xF9, 0xF6),
    /* [22] CHILD_HOVER         */ rgba(0xF2, 0xE3, 0xCE),
    /* [23] CHILD_HOVER_FG      */ rgba(0x23, 0x27, 0x24),
    /* [24] CHILD_TEXT          */ rgba(0x23, 0x27, 0x24),
    /* [25] CHILD_LINE          */ rgba(0xD6, 0xD6, 0xCE),
    /* [26] CHILD_ICON          */ rgba(0x64, 0x6A, 0x65),
    /* [27] INFO_BG             */ rgba(0xFA, 0xF9, 0xF6),
    /* [28] INFO_BOTTOM         */ rgba(0xEE, 0xEA, 0xE4),
    /* [29] INFO_TITLE_BORDER   */ rgba(0xD6, 0xD6, 0xCE),
    /* [30] INFO_TRIANGLE       */ rgba(0xFA, 0xF9, 0xF6),
    /* [31] ALERT_BG            */ rgba(0xEE, 0xEA, 0xE4),
    /* [32] ALERT_PANEL         */ rgba(0xFA, 0xF9, 0xF6),
    /* [33] ALERT_TITLE_FG      */ rgba(0x23, 0x27, 0x24),
    /* [34] ALERT_CONTENT_FG    */ rgba(0x64, 0x6A, 0x65),
    /* [35] ALERT_SHADOW        */ rgba(0x00, 0x00, 0x00, 0x22),
    /* [36] CLOSE_RED           */ rgba(0xD1, 0x3D, 0x4F),
    /* [37] OK_BLUE             */ rgba(0xD9, 0x95, 0x36),
    /* [38] HP_BG               */ rgba(0xCD, 0xDD, 0xF8, 0x80),
    /* [39] HP_HOVER            */ rgba(0xE5, 0xE7, 0xEC, 0x99),
    /* [40] HP_FONT_COLOR       */ rgba(0xE1, 0xDE, 0xDE),
    /* [41] HP_GREEN_L          */ rgba(0xD3, 0xEA, 0x7C),
    /* [42] HP_GREEN_R          */ rgba(0x9A, 0xD3, 0x34),
    /* [43] HP_YELLOW_L         */ rgba(0xEB, 0xEE, 0x70),
    /* [44] HP_YELLOW_R         */ rgba(0xF4, 0xFA, 0x49),
    /* [45] HP_RED_L            */ rgba(0xF8, 0x8C, 0x7A),
    /* [46] HP_RED_R            */ rgba(0xEF, 0x68, 0x4E),
    /* [47] HP_BORDER           */ rgba(0xDA, 0xD7, 0xD7),
    /* [48] BOSS_HP_RED         */ rgba(0xEF, 0x68, 0x4E),
    /* [49] BOSS_HP_BREAK       */ rgba(0xD4, 0x9C, 0x17),
    /* [50] BOSS_HP_SHIELD      */ rgba(0x48, 0x9C, 0xE8, 0x6B),
    /* [51] SURFACE_LIGHT       */ rgba(0xFA, 0xF9, 0xF6),
    /* [52] TEXT_PRIMARY        */ rgba(0x23, 0x27, 0x24),
    /* [53] TEXT_SECONDARY      */ rgba(0x64, 0x6A, 0x65),
    /* [54] ACCENT_GOLD_WARM    */ rgba(0xD9, 0x95, 0x36),
    /* [55] ACCENT_CYAN_SOFT    */ rgba(0x9A, 0x9F, 0x99),
    /* [56] CORNER_CYAN         */ rgba(0x8A, 0x90, 0x8A),
    /* [57] CORNER_GOLD         */ rgba(0xD9, 0x95, 0x36),
    /* [58] WHITE               */ rgba(0xFA, 0xF9, 0xF6),
    /* [59] WHITE_85            */ rgba(0xFA, 0xF9, 0xF6, 0xD9),
    /* [60] BLACK               */ rgba(0x23, 0x27, 0x24),
    /* [61] TRANSPARENT_KEY     */ rgba(0x01, 0x01, 0x01),
    /* [62] DPS_GOLD            */ rgba(0xA6, 0x6E, 0x26),
    /* [63] DPS_ROW_ALT         */ rgba(0xF3, 0xF0, 0xEA),
    /* [64] DPS_ROW_SELF        */ rgba(0xF2, 0xE3, 0xCE),
    /* [65] DPS_ROW_HOVER       */ rgba(0xF6, 0xEB, 0xDD),
    /* [66] ELEM_FIRE           */ rgba(0xFF, 0x6B, 0x35),
    /* [67] ELEM_WATER          */ rgba(0x2E, 0x9B, 0xFF),
    /* [68] ELEM_ELECTRIC       */ rgba(0xB4, 0x5A, 0xFF),
    /* [69] ELEM_WOOD           */ rgba(0x3F, 0xBF, 0x5F),
    /* [70] ELEM_WIND           */ rgba(0x46, 0xE0, 0xB0),
    /* [71] ELEM_ROCK           */ rgba(0xC8, 0x92, 0x3C),
    /* [72] ELEM_LIGHT          */ rgba(0xFF, 0xD9, 0x5A),
    /* [73] ELEM_DARK           */ rgba(0x9B, 0x6B, 0xD6),
    /* [74] ELEM_GENERIC        */ rgba(0xB0, 0xB8, 0xC4),
    /* [75] DISABLED_FG         */ rgba(0x8A, 0x8F, 0x8A),
    /* [76] DISABLED_BG         */ rgba(0xE6, 0xE2, 0xDC),
    /* [77] DISABLED_BORDER     */ rgba(0xCB, 0xCB, 0xC3),
    /* [78] HOVER_SURFACE       */ rgba(0xF2, 0xE3, 0xCE),
    /* [79] FOCUS_RING          */ rgba(0xD9, 0x95, 0x36),
    /* [80] PRESSED_SURFACE     */ rgba(0xE8, 0xC3, 0x8C),
    /* [81] PLACEHOLDER         */ rgba(0x7B, 0x81, 0x7C),
    /* [82] SELECTION           */ rgba(0xE8, 0xC3, 0x8C),
    /* [83] SCROLLBAR_TRACK     */ rgba(0xE2, 0xDE, 0xD7),
    /* [84] SCROLLBAR_THUMB     */ rgba(0xAB, 0xA9, 0xA2),
    /* [85] SCROLLBAR_HOVER     */ rgba(0x7D, 0x83, 0x7E),
    /* [86] SCROLLBAR_PRESSED   */ rgba(0xD9, 0x95, 0x36),
    /* [87] TOOLTIP_SURFACE     */ rgba(0xFA, 0xF9, 0xF6),
    /* [88] LOADING             */ rgba(0xEF, 0xE0, 0xC8),
    /* [89] SKELETON            */ rgba(0xE2, 0xDE, 0xD7),
    /* [90] ERROR_SURFACE       */ rgba(0xD1, 0x3D, 0x4F, 0x18),
    /* [91] ERROR_ICON          */ rgba(0xD1, 0x3D, 0x4F),
};

// ── Glass theme (translucent porcelain) ────────────────────────────
inline constexpr SaoColorRgba kSaoThemeGlassColors[SAO_UI_COLOR_TOKEN_COUNT] = {
    /* [ 0] OVERLAY_BG          */ rgba(0x23, 0x27, 0x24, 0x99),
    /* [ 1] APP_BG              */ rgba(0xEE, 0xEA, 0xE4, 0xEB),
    /* [ 2] APP_CARD            */ rgba(0xFA, 0xF9, 0xF6, 0xEB),
    /* [ 3] APP_BORDER          */ rgba(0xD6, 0xD6, 0xCE),
    /* [ 4] APP_TEXT            */ rgba(0x23, 0x27, 0x24),
    /* [ 5] APP_TEXT_2          */ rgba(0x64, 0x6A, 0x65),
    /* [ 6] APP_TEXT_DIM        */ rgba(0x85, 0x8A, 0x85),
    /* [ 7] APP_ACCENT          */ rgba(0xD9, 0x95, 0x36),
    /* [ 8] APP_BLUE            */ rgba(0x4C, 0x74, 0x84),
    /* [ 9] APP_GREEN           */ rgba(0x2E, 0x7D, 0x32),
    /* [10] APP_RED             */ rgba(0xC7, 0x2E, 0x2E),
    /* [11] APP_ORANGE          */ rgba(0x9D, 0x62, 0x17),
    /* [12] APP_GOLD            */ rgba(0x96, 0x61, 0x1F),
    /* [13] CIRCLE_BORDER       */ rgba(0xD6, 0xD6, 0xCE),
    /* [14] CIRCLE_BG           */ rgba(0xFA, 0xF9, 0xF6, 0xF0),
    /* [15] CIRCLE_ICON         */ rgba(0x64, 0x6A, 0x65),
    /* [16] CIRCLE_ACTIVE_BORDER*/ rgba(0xD9, 0x95, 0x36),
    /* [17] CIRCLE_ACTIVE_BG    */ rgba(0xD9, 0x95, 0x36),
    /* [18] CIRCLE_ACTIVE_ICON  */ rgba(0x23, 0x27, 0x24),
    /* [19] CIRCLE_HOVER_BG     */ rgba(0xF2, 0xE3, 0xCE, 0xF0),
    /* [20] CIRCLE_HOVER_ICON   */ rgba(0x6C, 0x4B, 0x22),
    /* [21] CHILD_BG            */ rgba(0xFA, 0xF9, 0xF6, 0xF0),
    /* [22] CHILD_HOVER         */ rgba(0xF2, 0xE3, 0xCE, 0xF0),
    /* [23] CHILD_HOVER_FG      */ rgba(0x23, 0x27, 0x24),
    /* [24] CHILD_TEXT          */ rgba(0x23, 0x27, 0x24),
    /* [25] CHILD_LINE          */ rgba(0xD6, 0xD6, 0xCE),
    /* [26] CHILD_ICON          */ rgba(0x64, 0x6A, 0x65),
    /* [27] INFO_BG             */ rgba(0xFA, 0xF9, 0xF6, 0xEB),
    /* [28] INFO_BOTTOM         */ rgba(0xEE, 0xEA, 0xE4, 0xEB),
    /* [29] INFO_TITLE_BORDER   */ rgba(0xD6, 0xD6, 0xCE),
    /* [30] INFO_TRIANGLE       */ rgba(0xFA, 0xF9, 0xF6, 0xEB),
    /* [31] ALERT_BG            */ rgba(0xEE, 0xEA, 0xE4, 0xEB),
    /* [32] ALERT_PANEL         */ rgba(0xFA, 0xF9, 0xF6, 0xEB),
    /* [33] ALERT_TITLE_FG      */ rgba(0x23, 0x27, 0x24),
    /* [34] ALERT_CONTENT_FG    */ rgba(0x64, 0x6A, 0x65),
    /* [35] ALERT_SHADOW        */ rgba(0x00, 0x00, 0x00, 0x34),
    /* [36] CLOSE_RED           */ rgba(0xD1, 0x3D, 0x4F),
    /* [37] OK_BLUE             */ rgba(0xD9, 0x95, 0x36),
    /* [38] HP_BG               */ rgba(0xCD, 0xDD, 0xF8, 0x60),
    /* [39] HP_HOVER            */ rgba(0xE5, 0xE7, 0xEC, 0x80),
    /* [40] HP_FONT_COLOR       */ rgba(0xE1, 0xDE, 0xDE),
    /* [41] HP_GREEN_L          */ rgba(0xD3, 0xEA, 0x7C),
    /* [42] HP_GREEN_R          */ rgba(0x9A, 0xD3, 0x34),
    /* [43] HP_YELLOW_L         */ rgba(0xEB, 0xEE, 0x70),
    /* [44] HP_YELLOW_R         */ rgba(0xF4, 0xFA, 0x49),
    /* [45] HP_RED_L            */ rgba(0xF8, 0x8C, 0x7A),
    /* [46] HP_RED_R            */ rgba(0xEF, 0x68, 0x4E),
    /* [47] HP_BORDER           */ rgba(0xDA, 0xD7, 0xD7),
    /* [48] BOSS_HP_RED         */ rgba(0xEF, 0x68, 0x4E),
    /* [49] BOSS_HP_BREAK       */ rgba(0xDE, 0xA6, 0x20),
    /* [50] BOSS_HP_SHIELD      */ rgba(0x62, 0xD0, 0xFF, 0x85),
    /* [51] SURFACE_LIGHT       */ rgba(0xFA, 0xF9, 0xF6, 0x50),
    /* [52] TEXT_PRIMARY        */ rgba(0x23, 0x27, 0x24),
    /* [53] TEXT_SECONDARY      */ rgba(0x64, 0x6A, 0x65),
    /* [54] ACCENT_GOLD_WARM    */ rgba(0xD9, 0x95, 0x36),
    /* [55] ACCENT_CYAN_SOFT    */ rgba(0x9A, 0x9F, 0x99),
    /* [56] CORNER_CYAN         */ rgba(0x8A, 0x90, 0x8A),
    /* [57] CORNER_GOLD         */ rgba(0xD9, 0x95, 0x36),
    /* [58] WHITE               */ rgba(0xFA, 0xF9, 0xF6),
    /* [59] WHITE_85            */ rgba(0xFA, 0xF9, 0xF6, 0xD9),
    /* [60] BLACK               */ rgba(0x23, 0x27, 0x24),
    /* [61] TRANSPARENT_KEY     */ rgba(0x01, 0x01, 0x01),
    /* [62] DPS_GOLD            */ rgba(0xA6, 0x6E, 0x26),
    /* [63] DPS_ROW_ALT         */ rgba(0xFA, 0xF9, 0xF6, 0x28),
    /* [64] DPS_ROW_SELF        */ rgba(0xD9, 0x95, 0x36, 0x30),
    /* [65] DPS_ROW_HOVER       */ rgba(0xD9, 0x95, 0x36, 0x20),
    /* [66] ELEM_FIRE           */ rgba(0xFF, 0x6B, 0x35),
    /* [67] ELEM_WATER          */ rgba(0x2E, 0x9B, 0xFF),
    /* [68] ELEM_ELECTRIC       */ rgba(0xB4, 0x5A, 0xFF),
    /* [69] ELEM_WOOD           */ rgba(0x3F, 0xBF, 0x5F),
    /* [70] ELEM_WIND           */ rgba(0x46, 0xE0, 0xB0),
    /* [71] ELEM_ROCK           */ rgba(0xC8, 0x92, 0x3C),
    /* [72] ELEM_LIGHT          */ rgba(0xFF, 0xD9, 0x5A),
    /* [73] ELEM_DARK           */ rgba(0x9B, 0x6B, 0xD6),
    /* [74] ELEM_GENERIC        */ rgba(0xB0, 0xB8, 0xC4),
    /* [75] DISABLED_FG         */ rgba(0x8A, 0x8F, 0x8A),
    /* [76] DISABLED_BG         */ rgba(0xE6, 0xE2, 0xDC, 0xE8),
    /* [77] DISABLED_BORDER     */ rgba(0xCB, 0xCB, 0xC3),
    /* [78] HOVER_SURFACE       */ rgba(0xF2, 0xE3, 0xCE, 0xF0),
    /* [79] FOCUS_RING          */ rgba(0xD9, 0x95, 0x36),
    /* [80] PRESSED_SURFACE     */ rgba(0xE8, 0xC3, 0x8C, 0xF0),
    /* [81] PLACEHOLDER         */ rgba(0x7B, 0x81, 0x7C),
    /* [82] SELECTION           */ rgba(0xE8, 0xC3, 0x8C, 0xE8),
    /* [83] SCROLLBAR_TRACK     */ rgba(0xE2, 0xDE, 0xD7, 0xD0),
    /* [84] SCROLLBAR_THUMB     */ rgba(0xAB, 0xA9, 0xA2),
    /* [85] SCROLLBAR_HOVER     */ rgba(0x7D, 0x83, 0x7E),
    /* [86] SCROLLBAR_PRESSED   */ rgba(0xD9, 0x95, 0x36),
    /* [87] TOOLTIP_SURFACE     */ rgba(0xFA, 0xF9, 0xF6, 0xF2),
    /* [88] LOADING             */ rgba(0xEF, 0xE0, 0xC8, 0xE8),
    /* [89] SKELETON            */ rgba(0xE2, 0xDE, 0xD7, 0xE8),
    /* [90] ERROR_SURFACE       */ rgba(0xEF, 0x68, 0x4E, 0x24),
    /* [91] ERROR_ICON          */ rgba(0xEF, 0x68, 0x4E),
};

// ── Metrics (design-token integer values, dpi-scale-free) ──────────
// Values pulled from Python `SAOCircleButton.SIZE/MAX_SIZE`, the
// `SAOMenuBar._SLOT` constant, and the widget-kit spacing scale used
// across Entity / Web parity panels.
inline constexpr int32_t kSaoThemeMetrics[SAO_UI_METRIC_TOKEN_COUNT] = {
    /* [ 0] BORDER_RADIUS_SMALL   */ 3,
    /* [ 1] BORDER_RADIUS_MEDIUM  */ 4,
    /* [ 2] BORDER_RADIUS_LARGE   */ 4,
    /* [ 3] PADDING_XS            */ 2,
    /* [ 4] PADDING_S             */ 4,
    /* [ 5] PADDING_M             */ 10,
    /* [ 6] PADDING_L             */ 16,
    /* [ 7] GAP_S                 */ 4,
    /* [ 8] GAP_M                 */ 10,
    /* [ 9] GAP_L                 */ 16,
    /* [10] MENU_BTN_SIZE         */ 54, // SAOCircleButton.SIZE
    /* [11] MENU_BTN_MAX_SIZE     */ 70, // SAOCircleButton.MAX_SIZE
    /* [12] MENU_SLOT             */ 70, // SAOMenuBar._SLOT
    /* [13] HUD_MARGIN            */ 18, // HUD bracket inset
    /* [14] HUD_PAD               */ 8,  // HUD sprite margin

    /* [15] CONTROL_HEIGHT_SM     */ 28,
    /* [16] CONTROL_HEIGHT_MD     */ 36,
    /* [17] CONTROL_HEIGHT_LG     */ 40,
    /* [18] ICON_SIZE             */ 16,
    /* [19] TOUCH_TARGET          */ 24,
    /* [20] TABLE_ROW_HEIGHT      */ 30,
    /* [21] HEADER_HEIGHT         */ 36,
    /* [22] TOOLTIP_MAX_WIDTH     */ 360,
    /* [23] SCROLLBAR_WIDTH       */ 8,
    /* [24] SCROLLBAR_MIN_THUMB   */ 18,
    /* [25] SCROLLBAR_HIT_AREA    */ 16,
};

inline constexpr SaoUiShadowPreset kSaoThemeElevationPresets[4] = {
    /* elevation_0 */ {0, 0, 0, 0, 0x00, {0, 0, 0}},
    /* elevation_1 */ {0, 2, 3, 1, 0x20, {0, 0, 0}},
    /* elevation_2 */ {0, 4, 6, 2, 0x30, {0, 0, 0}},
    /* elevation_3 */ {0, 8, 12, 3, 0x40, {0, 0, 0}},
};

// ── Compile-time size correctness (G3.1 gate) ─────────────────────
static_assert(std::size(kSaoThemeDarkColors)  == SAO_UI_COLOR_TOKEN_COUNT,
              "kSaoThemeDarkColors size must match SAO_UI_COLOR_TOKEN_COUNT");
static_assert(std::size(kSaoThemeLightColors) == SAO_UI_COLOR_TOKEN_COUNT,
              "kSaoThemeLightColors size must match SAO_UI_COLOR_TOKEN_COUNT");
static_assert(std::size(kSaoThemeGlassColors) == SAO_UI_COLOR_TOKEN_COUNT,
              "kSaoThemeGlassColors size must match SAO_UI_COLOR_TOKEN_COUNT");
static_assert(std::size(kSaoThemeMetrics)     == SAO_UI_METRIC_TOKEN_COUNT,
              "kSaoThemeMetrics size must match SAO_UI_METRIC_TOKEN_COUNT");
// Sentinel guard so a future refactor that renumbers the enums also
// updates the sentinel here (light + glass never grew independently).
static_assert(SAO_UI_COLOR_TOKEN_COUNT == 92,
              "Theme tables were built against 92 tokens; update them "
              "before re-numbering SaoUiColorToken");
static_assert(SAO_UI_METRIC_TOKEN_COUNT == 26,
              "Theme metric table was built against 26 metrics; update "
              "kSaoThemeMetrics before re-numbering SaoUiMetricToken");

// Table dispatch — inline so this is a single load in optimized builds.
inline constexpr const SaoColorRgba* theme_colors_for(SaoUiThemeId theme_id) {
    return theme_id == SAO_UI_THEME_LIGHT ? kSaoThemeLightColors
         : theme_id == SAO_UI_THEME_GLASS ? kSaoThemeGlassColors
         : kSaoThemeDarkColors;  // DARK is the default fallback
}

}  // namespace sao::ui

#endif  // __cplusplus
