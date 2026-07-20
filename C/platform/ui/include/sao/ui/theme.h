// SAO Auto — theme tokens.
//
// Python source of truth:
//   - `sao_auto/python/sao_theme/colors.py`  (SAOColors class, 50+ tokens)
//   - `sao_auto/python/sao_theme/theme_manager.py`  (panel-theme registry)
//   - `render/tk_mirror.py`  (_theme_color runtime resolver)
//
// Design: token IDs are compile-time enum values; each theme is a
// `constexpr` static table of ARGB values indexed by token.  Zero
// runtime cost to look up "what colour is this?" — every access is a
// single array load.
//
// The Python side's SAOColors is a class with 50+ named hex strings.
// Runtime code calls `_theme_color(key, fallback)` which resolves
// against the currently active panel theme, falling back to the
// hardcoded SAOColors constant.  Here we consolidate: every token has
// exactly one value per theme, chosen from the light/dark/glass tables.

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
    SAO_UI_THEME_DARK  = 0,   // Entity default; APP_BG-based
    SAO_UI_THEME_LIGHT = 1,   // Web / panel light-mode
    SAO_UI_THEME_GLASS = 2,   // Frosted-glass HUD (buffmon / identity)
    SAO_UI_THEME_COUNT = 3,
};

// ── Color tokens (mirrors SAOColors class) ────────────────────────
// Each token maps to exactly one ARGB per theme.  Any token that has
// no meaningful light-mode counterpart falls back to the dark value.
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

    SAO_UI_TOKEN_COUNT                      = 75,
};

// Canonical count alias (matches G3.1 spec vocabulary).
#define SAO_UI_COLOR_TOKEN_COUNT SAO_UI_TOKEN_COUNT

// ── Static color table (compile-time constant) ────────────────────
// Each row = one theme; each column = one token.  ARGB layout:
// 0xAARRGGBB.  Populated by src/theme_tables.cpp at translation-unit
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
    SAO_UI_METRIC_COUNT                     = 15,
};

// Canonical metric-count alias.
#define SAO_UI_METRIC_TOKEN_COUNT SAO_UI_METRIC_COUNT

struct SaoUiMetricTable {
    int32_t values[SAO_UI_METRIC_COUNT];
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
// Returns SAO_STATUS_ERR_NOT_FOUND if no override registered.
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

// JSON load — optional runtime path for user themes.  Ignores unknown
// keys; missing keys keep their static value.
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
// Populated verbatim from `sao_theme/colors.py` (SAOColors class),
// `sao_gui_bosshp.py` (RED gradient), and `assets/name_tables/
// element.json` (EDamageProperty 0..8).  Alpha defaults to 0xFF for
// every solid token; tokens that carry a Python alpha suffix keep
// that literal alpha (e.g. HP_BG's `#cdddf880` → alpha=0x80).
//
// Layout: kSaoTheme<Name>Colors[SAO_UI_COLOR_TOKEN_COUNT], indexed
// directly by SaoUiColorToken.  All three arrays MUST hold exactly
// SAO_UI_COLOR_TOKEN_COUNT entries (enforced by static_assert below).
//
// The light-mode variant tracks the original SAOColors class (the CSS
// source of truth was already a light UI); the dark-mode variant
// replaces the "APP_*" band + several panel surfaces with the deep-
// space palette used by Entity (`APP_BG`, `APP_CARD`, …); the glass
// variant keeps the frosted-glass HUD values and overlays a soft
// translucent surface derived from `SURFACE_LIGHT` + alpha=0x40.
//
// Any token without a distinct per-theme value simply reuses the
// canonical value (dark inherits from the SAOColors static; glass
// inherits from dark unless explicitly overridden). This matches the
// Python `_theme_color(key, fallback)` runtime behaviour.

#ifdef __cplusplus

namespace sao::ui {

// Alias so the constexpr arrays below can name the RGBA type
// without the C-style prefix in every entry.
using SaoColorRgba = ::SaoColorRgba;

// Sentinel helpers.  `rgba(r,g,b)` defaults alpha to 0xFF; the 4-arg
// form is used only where the Python source pinned an explicit alpha.
constexpr SaoColorRgba rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF) {
    return SaoColorRgba{r, g, b, a};
}

// ── Dark theme (Entity default) ────────────────────────────────────
inline constexpr SaoColorRgba kSaoThemeDarkColors[SAO_UI_COLOR_TOKEN_COUNT] = {
    // Overlay / background — dark uses APP_BG-based backdrop, not white
    /* [ 0] OVERLAY_BG          */ rgba(0x00, 0x00, 0x00, 0xB3),  // OVERLAY_ALPHA 0.70 × 255
    /* [ 1] APP_BG              */ rgba(0x0A, 0x0E, 0x14),         // #0a0e14
    /* [ 2] APP_CARD            */ rgba(0x11, 0x18, 0x20),         // #111820
    /* [ 3] APP_BORDER          */ rgba(0x1A, 0x3A, 0x4E),         // #1a3a4e
    /* [ 4] APP_TEXT            */ rgba(0xE8, 0xF4, 0xF8),         // #e8f4f8
    /* [ 5] APP_TEXT_2          */ rgba(0x7E, 0xB8, 0xC9),         // #7eb8c9
    /* [ 6] APP_TEXT_DIM        */ rgba(0x3D, 0x60, 0x70),         // #3d6070
    /* [ 7] APP_ACCENT          */ rgba(0x4D, 0xE8, 0xF4),         // #4de8f4
    /* [ 8] APP_BLUE            */ rgba(0x21, 0x96, 0xF3),         // #2196f3
    /* [ 9] APP_GREEN           */ rgba(0x4C, 0xAF, 0x50),         // #4caf50
    /* [10] APP_RED             */ rgba(0xFF, 0x44, 0x44),         // #ff4444
    /* [11] APP_ORANGE          */ rgba(0xFF, 0x98, 0x00),         // #ff9800
    /* [12] APP_GOLD            */ rgba(0xFF, 0xD7, 0x00),         // #ffd700
    // Circle button — SAOCircleButton (verbatim from SAOColors)
    /* [13] CIRCLE_BORDER       */ rgba(0xBC, 0xC4, 0xCA),         // #bcc4ca
    /* [14] CIRCLE_BG           */ rgba(0xF7, 0xF8, 0xF8),         // #f7f8f8
    /* [15] CIRCLE_ICON         */ rgba(0x95, 0x9A, 0xA0),         // #959aa0
    /* [16] CIRCLE_ACTIVE_BORDER*/ rgba(0xF3, 0xAF, 0x12),         // #f3af12
    /* [17] CIRCLE_ACTIVE_BG    */ rgba(0xF4, 0xEB, 0xD7),         // #f4ebd7
    /* [18] CIRCLE_ACTIVE_ICON  */ rgba(0x6D, 0x5D, 0x40),         // #6d5d40
    /* [19] CIRCLE_HOVER_BG     */ rgba(0xED, 0xF7, 0xFA),         // #edf7fa
    /* [20] CIRCLE_HOVER_ICON   */ rgba(0x71, 0x89, 0x95),         // #718995
    // Child menu (ChildBar)
    /* [21] CHILD_BG            */ rgba(0xF8, 0xF8, 0xF8),         // #f8f8f8
    /* [22] CHILD_HOVER         */ rgba(0xF4, 0xEE, 0xE1),         // #f4eee1
    /* [23] CHILD_HOVER_FG      */ rgba(0x62, 0x58, 0x46),         // #625846
    /* [24] CHILD_TEXT          */ rgba(0x64, 0x63, 0x64),         // #646364
    /* [25] CHILD_LINE          */ rgba(0xBC, 0xC4, 0xCA),         // #bcc4ca
    /* [26] CHILD_ICON          */ rgba(0x8F, 0x95, 0x9B),         // #8f959b
    // Left info panel (LeftInfo)
    /* [27] INFO_BG             */ rgba(0xFB, 0xFB, 0xFB),         // #fbfbfb
    /* [28] INFO_BOTTOM         */ rgba(0xEC, 0xEB, 0xEA),         // #ecebea
    /* [29] INFO_TITLE_BORDER   */ rgba(0xC7, 0xCC, 0xD0),         // #c7ccd0
    /* [30] INFO_TRIANGLE       */ rgba(0xF4, 0xF4, 0xF4),         // #f4f4f4
    // Alert / dialog (Python explicit alpha preserved)
    /* [31] ALERT_BG            */ rgba(0xFF, 0xFF, 0xFF, 0xE6),   // #ffffffe6
    /* [32] ALERT_PANEL         */ rgba(0xEC, 0xEB, 0xEA, 0xC9),   // #ecebeac9
    /* [33] ALERT_TITLE_FG      */ rgba(0x64, 0x63, 0x64),         // #646364
    /* [34] ALERT_CONTENT_FG    */ rgba(0x64, 0x60, 0x60),         // #646060
    /* [35] ALERT_SHADOW        */ rgba(0x00, 0x00, 0x00, 0x22),   // #00000022
    /* [36] CLOSE_RED           */ rgba(0xD1, 0x3D, 0x4F),         // #d13d4f
    /* [37] OK_BLUE             */ rgba(0x42, 0x8C, 0xE6),         // #428ce6
    // HP bar (SAOColors HP_* — explicit alpha suffix preserved)
    /* [38] HP_BG               */ rgba(0xCD, 0xDD, 0xF8, 0x80),   // #cdddf880
    /* [39] HP_HOVER            */ rgba(0xE5, 0xE7, 0xEC, 0x99),   // #e5e7ec99
    /* [40] HP_FONT_COLOR       */ rgba(0xE1, 0xDE, 0xDE),         // #e1dede
    /* [41] HP_GREEN_L          */ rgba(0xD3, 0xEA, 0x7C),         // #d3ea7c
    /* [42] HP_GREEN_R          */ rgba(0x9A, 0xD3, 0x34),         // #9ad334
    /* [43] HP_YELLOW_L         */ rgba(0xEB, 0xEE, 0x70),         // #ebee70
    /* [44] HP_YELLOW_R         */ rgba(0xF4, 0xFA, 0x49),         // #f4fa49
    /* [45] HP_RED_L            */ rgba(0xF8, 0x8C, 0x7A),         // #f88c7a
    /* [46] HP_RED_R            */ rgba(0xEF, 0x68, 0x4E),         // #ef684e
    /* [47] HP_BORDER           */ rgba(0xDA, 0xD7, 0xD7),         // #dad7d7
    // Boss HP bar (BossHpOverlay class constants — 4-tuple → alpha 0xFF)
    /* [48] BOSS_HP_RED         */ rgba(0xEF, 0x68, 0x4E),         // BossHpOverlay.RED tail
    /* [49] BOSS_HP_BREAK       */ rgba(0xD4, 0x9C, 0x17),         // BREAK strong (GOLD_STRONG family)
    /* [50] BOSS_HP_SHIELD      */ rgba(0x48, 0x9C, 0xE8, 0x6B),   // BossHp SHIELD_A
    // Frosted-Glass HUD (SAOColors *_HEX in colors.py)
    /* [51] SURFACE_LIGHT       */ rgba(0xF8, 0xF8, 0xF8),         // SURFACE_LIGHT_HEX
    /* [52] TEXT_PRIMARY        */ rgba(0x64, 0x63, 0x64),         // TEXT_PRIMARY_HEX
    /* [53] TEXT_SECONDARY      */ rgba(0x8C, 0x87, 0x8A),         // TEXT_SECONDARY_HEX
    /* [54] ACCENT_GOLD_WARM    */ rgba(0xD4, 0x9C, 0x17),         // ACCENT_GOLD_WARM_HEX
    /* [55] ACCENT_CYAN_SOFT    */ rgba(0x58, 0x98, 0xBE),         // ACCENT_CYAN_SOFT_HEX
    /* [56] CORNER_CYAN         */ rgba(0x68, 0xE4, 0xFF),         // CORNER_CYAN tuple
    /* [57] CORNER_GOLD         */ rgba(0xD4, 0x9C, 0x17),         // CORNER_GOLD tuple
    // Common
    /* [58] WHITE               */ rgba(0xFF, 0xFF, 0xFF),         // #ffffff
    /* [59] WHITE_85            */ rgba(0xFF, 0xFF, 0xFF, 0xD9),   // #ffffffd9
    /* [60] BLACK               */ rgba(0x00, 0x00, 0x00),         // #000000
    /* [61] TRANSPARENT_KEY     */ rgba(0x01, 0x01, 0x01),         // #010101 chroma-key
    // Damage / DPS panel (APP_GOLD family + APP_CARD tints)
    /* [62] DPS_GOLD            */ rgba(0xFF, 0xD7, 0x00),         // APP_GOLD
    /* [63] DPS_ROW_ALT         */ rgba(0x11, 0x18, 0x20, 0x80),   // APP_CARD × alpha 0x80
    /* [64] DPS_ROW_SELF        */ rgba(0x1A, 0x3A, 0x4E, 0xB3),   // APP_BORDER × alpha 0.70
    /* [65] DPS_ROW_HOVER       */ rgba(0x4D, 0xE8, 0xF4, 0x40),   // APP_ACCENT × alpha 0.25
    // Element tints (assets/name_tables/element.json 0..8)
    /* [66] ELEM_FIRE           */ rgba(0xFF, 0x6B, 0x35),         // #FF6B35 (id 1)
    /* [67] ELEM_WATER          */ rgba(0x2E, 0x9B, 0xFF),         // #2E9BFF (id 2)
    /* [68] ELEM_ELECTRIC       */ rgba(0xB4, 0x5A, 0xFF),         // #B45AFF (id 3)
    /* [69] ELEM_WOOD           */ rgba(0x3F, 0xBF, 0x5F),         // #3FBF5F (id 4)
    /* [70] ELEM_WIND           */ rgba(0x46, 0xE0, 0xB0),         // #46E0B0 (id 5)
    /* [71] ELEM_ROCK           */ rgba(0xC8, 0x92, 0x3C),         // #C8923C (id 6)
    /* [72] ELEM_LIGHT          */ rgba(0xFF, 0xD9, 0x5A),         // #FFD95A (id 7)
    /* [73] ELEM_DARK           */ rgba(0x9B, 0x6B, 0xD6),         // #9B6BD6 (id 8)
    /* [74] ELEM_GENERIC        */ rgba(0xB0, 0xB8, 0xC4),         // #B0B8C4 (id 0)
};

// ── Light theme (SAOColors canonical CSS palette) ─────────────────
// The Python SAOColors class is already a light-mode design; the
// dark-only band (APP_BG..APP_GOLD) is replaced here with the CSS
// counterpart from the original Vue source: white surfaces, dark
// text on light background.
inline constexpr SaoColorRgba kSaoThemeLightColors[SAO_UI_COLOR_TOKEN_COUNT] = {
    /* [ 0] OVERLAY_BG          */ rgba(0x00, 0x00, 0x00, 0xB3),
    /* [ 1] APP_BG              */ rgba(0xFB, 0xFB, 0xFB),         // INFO_BG re-used as page bg
    /* [ 2] APP_CARD            */ rgba(0xFF, 0xFF, 0xFF),         // WHITE card surface
    /* [ 3] APP_BORDER          */ rgba(0xC7, 0xCC, 0xD0),         // INFO_TITLE_BORDER
    /* [ 4] APP_TEXT            */ rgba(0x64, 0x63, 0x64),         // TEXT_PRIMARY on light
    /* [ 5] APP_TEXT_2          */ rgba(0x8C, 0x87, 0x8A),         // TEXT_SECONDARY
    /* [ 6] APP_TEXT_DIM        */ rgba(0xBC, 0xC4, 0xCA),         // CIRCLE_BORDER neutral
    /* [ 7] APP_ACCENT          */ rgba(0x42, 0x8C, 0xE6),         // OK_BLUE accent on light
    /* [ 8] APP_BLUE            */ rgba(0x42, 0x8C, 0xE6),
    /* [ 9] APP_GREEN           */ rgba(0x9A, 0xD3, 0x34),         // HP_GREEN_R
    /* [10] APP_RED             */ rgba(0xD1, 0x3D, 0x4F),         // CLOSE_RED
    /* [11] APP_ORANGE          */ rgba(0xF3, 0xAF, 0x12),         // ACTIVE_BORDER
    /* [12] APP_GOLD            */ rgba(0xD4, 0x9C, 0x17),         // ACCENT_GOLD_WARM
    /* [13] CIRCLE_BORDER       */ rgba(0xBC, 0xC4, 0xCA),
    /* [14] CIRCLE_BG           */ rgba(0xF7, 0xF8, 0xF8),
    /* [15] CIRCLE_ICON         */ rgba(0x95, 0x9A, 0xA0),
    /* [16] CIRCLE_ACTIVE_BORDER*/ rgba(0xF3, 0xAF, 0x12),
    /* [17] CIRCLE_ACTIVE_BG    */ rgba(0xF4, 0xEB, 0xD7),
    /* [18] CIRCLE_ACTIVE_ICON  */ rgba(0x6D, 0x5D, 0x40),
    /* [19] CIRCLE_HOVER_BG     */ rgba(0xED, 0xF7, 0xFA),
    /* [20] CIRCLE_HOVER_ICON   */ rgba(0x71, 0x89, 0x95),
    /* [21] CHILD_BG            */ rgba(0xF8, 0xF8, 0xF8),
    /* [22] CHILD_HOVER         */ rgba(0xF4, 0xEE, 0xE1),
    /* [23] CHILD_HOVER_FG      */ rgba(0x62, 0x58, 0x46),
    /* [24] CHILD_TEXT          */ rgba(0x64, 0x63, 0x64),
    /* [25] CHILD_LINE          */ rgba(0xBC, 0xC4, 0xCA),
    /* [26] CHILD_ICON          */ rgba(0x8F, 0x95, 0x9B),
    /* [27] INFO_BG             */ rgba(0xFB, 0xFB, 0xFB),
    /* [28] INFO_BOTTOM         */ rgba(0xEC, 0xEB, 0xEA),
    /* [29] INFO_TITLE_BORDER   */ rgba(0xC7, 0xCC, 0xD0),
    /* [30] INFO_TRIANGLE       */ rgba(0xF4, 0xF4, 0xF4),
    /* [31] ALERT_BG            */ rgba(0xFF, 0xFF, 0xFF, 0xE6),
    /* [32] ALERT_PANEL         */ rgba(0xEC, 0xEB, 0xEA, 0xC9),
    /* [33] ALERT_TITLE_FG      */ rgba(0x64, 0x63, 0x64),
    /* [34] ALERT_CONTENT_FG    */ rgba(0x64, 0x60, 0x60),
    /* [35] ALERT_SHADOW        */ rgba(0x00, 0x00, 0x00, 0x22),
    /* [36] CLOSE_RED           */ rgba(0xD1, 0x3D, 0x4F),
    /* [37] OK_BLUE             */ rgba(0x42, 0x8C, 0xE6),
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
    /* [51] SURFACE_LIGHT       */ rgba(0xF8, 0xF8, 0xF8),
    /* [52] TEXT_PRIMARY        */ rgba(0x64, 0x63, 0x64),
    /* [53] TEXT_SECONDARY      */ rgba(0x8C, 0x87, 0x8A),
    /* [54] ACCENT_GOLD_WARM    */ rgba(0xD4, 0x9C, 0x17),
    /* [55] ACCENT_CYAN_SOFT    */ rgba(0x58, 0x98, 0xBE),
    /* [56] CORNER_CYAN         */ rgba(0x68, 0xE4, 0xFF),
    /* [57] CORNER_GOLD         */ rgba(0xD4, 0x9C, 0x17),
    /* [58] WHITE               */ rgba(0xFF, 0xFF, 0xFF),
    /* [59] WHITE_85            */ rgba(0xFF, 0xFF, 0xFF, 0xD9),
    /* [60] BLACK               */ rgba(0x00, 0x00, 0x00),
    /* [61] TRANSPARENT_KEY     */ rgba(0x01, 0x01, 0x01),
    /* [62] DPS_GOLD            */ rgba(0xD4, 0x9C, 0x17),         // warm gold on light
    /* [63] DPS_ROW_ALT         */ rgba(0xF4, 0xF4, 0xF4),
    /* [64] DPS_ROW_SELF        */ rgba(0xF4, 0xEB, 0xD7),         // CIRCLE_ACTIVE_BG
    /* [65] DPS_ROW_HOVER       */ rgba(0xED, 0xF7, 0xFA),         // CIRCLE_HOVER_BG
    /* [66] ELEM_FIRE           */ rgba(0xFF, 0x6B, 0x35),
    /* [67] ELEM_WATER          */ rgba(0x2E, 0x9B, 0xFF),
    /* [68] ELEM_ELECTRIC       */ rgba(0xB4, 0x5A, 0xFF),
    /* [69] ELEM_WOOD           */ rgba(0x3F, 0xBF, 0x5F),
    /* [70] ELEM_WIND           */ rgba(0x46, 0xE0, 0xB0),
    /* [71] ELEM_ROCK           */ rgba(0xC8, 0x92, 0x3C),
    /* [72] ELEM_LIGHT          */ rgba(0xFF, 0xD9, 0x5A),
    /* [73] ELEM_DARK           */ rgba(0x9B, 0x6B, 0xD6),
    /* [74] ELEM_GENERIC        */ rgba(0xB0, 0xB8, 0xC4),
};

// ── Glass theme (frosted-glass HUD — dark base + translucent tint) ─
// Inherits the dark palette but overlays SURFACE_LIGHT at low alpha
// on the surface tokens and boosts the CORNER_* / ACCENT_* saturation
// per SAO identity-panel conventions (dark surroundings, glass sheen).
inline constexpr SaoColorRgba kSaoThemeGlassColors[SAO_UI_COLOR_TOKEN_COUNT] = {
    /* [ 0] OVERLAY_BG          */ rgba(0x00, 0x00, 0x00, 0x99),   // slightly clearer
    /* [ 1] APP_BG              */ rgba(0x0A, 0x0E, 0x14, 0xC0),   // frosted dark backdrop
    /* [ 2] APP_CARD            */ rgba(0xF8, 0xF8, 0xF8, 0x40),   // SURFACE_LIGHT × alpha 0x40
    /* [ 3] APP_BORDER          */ rgba(0x68, 0xE4, 0xFF, 0x99),   // CORNER_CYAN with alpha
    /* [ 4] APP_TEXT            */ rgba(0xE8, 0xF4, 0xF8),
    /* [ 5] APP_TEXT_2          */ rgba(0x8C, 0x87, 0x8A),
    /* [ 6] APP_TEXT_DIM        */ rgba(0x58, 0x98, 0xBE, 0xA0),   // ACCENT_CYAN dimmed
    /* [ 7] APP_ACCENT          */ rgba(0x68, 0xE4, 0xFF),         // CORNER_CYAN
    /* [ 8] APP_BLUE            */ rgba(0x58, 0x98, 0xBE),
    /* [ 9] APP_GREEN           */ rgba(0x9A, 0xD3, 0x34),
    /* [10] APP_RED             */ rgba(0xEF, 0x68, 0x4E),
    /* [11] APP_ORANGE          */ rgba(0xF3, 0xAF, 0x12),
    /* [12] APP_GOLD            */ rgba(0xD4, 0x9C, 0x17),         // ACCENT_GOLD_WARM
    /* [13] CIRCLE_BORDER       */ rgba(0xBC, 0xC4, 0xCA, 0xC0),
    /* [14] CIRCLE_BG           */ rgba(0xF7, 0xF8, 0xF8, 0x60),   // translucent
    /* [15] CIRCLE_ICON         */ rgba(0x95, 0x9A, 0xA0),
    /* [16] CIRCLE_ACTIVE_BORDER*/ rgba(0xF3, 0xAF, 0x12),
    /* [17] CIRCLE_ACTIVE_BG    */ rgba(0xF4, 0xEB, 0xD7, 0x80),
    /* [18] CIRCLE_ACTIVE_ICON  */ rgba(0x6D, 0x5D, 0x40),
    /* [19] CIRCLE_HOVER_BG     */ rgba(0xED, 0xF7, 0xFA, 0x80),
    /* [20] CIRCLE_HOVER_ICON   */ rgba(0x71, 0x89, 0x95),
    /* [21] CHILD_BG            */ rgba(0xF8, 0xF8, 0xF8, 0x60),
    /* [22] CHILD_HOVER         */ rgba(0xF4, 0xEE, 0xE1, 0x80),
    /* [23] CHILD_HOVER_FG      */ rgba(0x62, 0x58, 0x46),
    /* [24] CHILD_TEXT          */ rgba(0x64, 0x63, 0x64),
    /* [25] CHILD_LINE          */ rgba(0xBC, 0xC4, 0xCA, 0xA0),
    /* [26] CHILD_ICON          */ rgba(0x8F, 0x95, 0x9B),
    /* [27] INFO_BG             */ rgba(0xFB, 0xFB, 0xFB, 0x50),
    /* [28] INFO_BOTTOM         */ rgba(0xEC, 0xEB, 0xEA, 0x60),
    /* [29] INFO_TITLE_BORDER   */ rgba(0x68, 0xE4, 0xFF, 0x80),
    /* [30] INFO_TRIANGLE       */ rgba(0xF4, 0xF4, 0xF4, 0x60),
    /* [31] ALERT_BG            */ rgba(0xFF, 0xFF, 0xFF, 0xC0),
    /* [32] ALERT_PANEL         */ rgba(0xEC, 0xEB, 0xEA, 0xA0),
    /* [33] ALERT_TITLE_FG      */ rgba(0x64, 0x63, 0x64),
    /* [34] ALERT_CONTENT_FG    */ rgba(0x64, 0x60, 0x60),
    /* [35] ALERT_SHADOW        */ rgba(0x00, 0x00, 0x00, 0x44),
    /* [36] CLOSE_RED           */ rgba(0xD1, 0x3D, 0x4F),
    /* [37] OK_BLUE             */ rgba(0x42, 0x8C, 0xE6),
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
    /* [49] BOSS_HP_BREAK       */ rgba(0xDE, 0xA6, 0x20),         // GOLD_STRONG
    /* [50] BOSS_HP_SHIELD      */ rgba(0x62, 0xD0, 0xFF, 0x85),   // BossHp SHIELD_B
    /* [51] SURFACE_LIGHT       */ rgba(0xF8, 0xF8, 0xF8, 0x40),   // authentic frosted glass
    /* [52] TEXT_PRIMARY        */ rgba(0x64, 0x63, 0x64),
    /* [53] TEXT_SECONDARY      */ rgba(0x8C, 0x87, 0x8A),
    /* [54] ACCENT_GOLD_WARM    */ rgba(0xD4, 0x9C, 0x17),
    /* [55] ACCENT_CYAN_SOFT    */ rgba(0x58, 0x98, 0xBE),
    /* [56] CORNER_CYAN         */ rgba(0x68, 0xE4, 0xFF),
    /* [57] CORNER_GOLD         */ rgba(0xD4, 0x9C, 0x17),
    /* [58] WHITE               */ rgba(0xFF, 0xFF, 0xFF),
    /* [59] WHITE_85            */ rgba(0xFF, 0xFF, 0xFF, 0xD9),
    /* [60] BLACK               */ rgba(0x00, 0x00, 0x00),
    /* [61] TRANSPARENT_KEY     */ rgba(0x01, 0x01, 0x01),
    /* [62] DPS_GOLD            */ rgba(0xD4, 0x9C, 0x17),
    /* [63] DPS_ROW_ALT         */ rgba(0xF8, 0xF8, 0xF8, 0x28),
    /* [64] DPS_ROW_SELF        */ rgba(0x68, 0xE4, 0xFF, 0x40),
    /* [65] DPS_ROW_HOVER       */ rgba(0x68, 0xE4, 0xFF, 0x30),
    /* [66] ELEM_FIRE           */ rgba(0xFF, 0x6B, 0x35),
    /* [67] ELEM_WATER          */ rgba(0x2E, 0x9B, 0xFF),
    /* [68] ELEM_ELECTRIC       */ rgba(0xB4, 0x5A, 0xFF),
    /* [69] ELEM_WOOD           */ rgba(0x3F, 0xBF, 0x5F),
    /* [70] ELEM_WIND           */ rgba(0x46, 0xE0, 0xB0),
    /* [71] ELEM_ROCK           */ rgba(0xC8, 0x92, 0x3C),
    /* [72] ELEM_LIGHT          */ rgba(0xFF, 0xD9, 0x5A),
    /* [73] ELEM_DARK           */ rgba(0x9B, 0x6B, 0xD6),
    /* [74] ELEM_GENERIC        */ rgba(0xB0, 0xB8, 0xC4),
};

// ── Metrics (design-token integer values, dpi-scale-free) ──────────
// Values pulled from Python `SAOCircleButton.SIZE/MAX_SIZE`, the
// `SAOMenuBar._SLOT` constant, and the widget-kit spacing scale used
// across Entity / Web parity panels.
inline constexpr int32_t kSaoThemeMetrics[SAO_UI_METRIC_TOKEN_COUNT] = {
    /* [ 0] BORDER_RADIUS_SMALL   */  4,
    /* [ 1] BORDER_RADIUS_MEDIUM  */  8,
    /* [ 2] BORDER_RADIUS_LARGE   */ 14,
    /* [ 3] PADDING_XS            */  2,
    /* [ 4] PADDING_S             */  4,
    /* [ 5] PADDING_M             */  8,
    /* [ 6] PADDING_L             */ 12,
    /* [ 7] GAP_S                 */  4,
    /* [ 8] GAP_M                 */  8,
    /* [ 9] GAP_L                 */ 12,
    /* [10] MENU_BTN_SIZE         */ 46,   // SAOCircleButton.SIZE
    /* [11] MENU_BTN_MAX_SIZE     */ 62,   // SAOCircleButton.MAX_SIZE
    /* [12] MENU_SLOT             */ 70,   // SAOMenuBar._SLOT
    /* [13] HUD_MARGIN            */ 18,   // HUD bracket inset
    /* [14] HUD_PAD               */  8,   // HUD sprite margin
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
static_assert(SAO_UI_COLOR_TOKEN_COUNT == 75,
              "Theme tables were built against 75 tokens; update them "
              "before re-numbering SaoUiColorToken");
static_assert(SAO_UI_METRIC_TOKEN_COUNT == 15,
              "Theme metric table was built against 15 metrics; update "
              "kSaoThemeMetrics before re-numbering SaoUiMetricToken");

// Table dispatch — inline so this is a single load in optimized builds.
inline constexpr const SaoColorRgba* theme_colors_for(SaoUiThemeId theme_id) {
    return theme_id == SAO_UI_THEME_LIGHT ? kSaoThemeLightColors
         : theme_id == SAO_UI_THEME_GLASS ? kSaoThemeGlassColors
         : kSaoThemeDarkColors;  // DARK is the default fallback
}

}  // namespace sao::ui

#endif  // __cplusplus
