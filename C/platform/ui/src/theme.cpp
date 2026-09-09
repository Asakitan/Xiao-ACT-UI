// SAO Auto — theme implementation (flat-token tables + handle API).
//
// The flat-token API is production-ready: three constexpr colour tables
// (dark / light / glass) + one metric table live in theme.h, and all
// runtime accessors below just read from them.  The handle-based API
// also layers per-panel overrides on top.

#include "sao/ui/theme.h"

#include "panel_theme_internal.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct ThemePanelOverride {
    sao_ui_theme_owner_t owner{};
    std::string panel_key;
    SaoUiThemeId theme_id{SAO_UI_THEME_DARK};
    SaoUiColorToken token{SAO_UI_TOKEN_OVERLAY_BG};
    uint32_t argb{};
};

struct ThemeListener {
    sao_ui_theme_changed_callback_t callback{};
    void* user_data{};
};

struct sao_ui_theme_s {
    SaoUiThemeId active{SAO_UI_THEME_LIGHT};
    std::vector<ThemePanelOverride> overrides;
    std::vector<ThemeListener> listeners;
    std::array<uint32_t, SAO_UI_TOKEN_COUNT> color_overrides{};
    std::array<bool, SAO_UI_TOKEN_COUNT> has_color_overrides{};
    std::mutex mutex;
};

// ── Process-wide flat-token state ────────────────────────────────
namespace {

// Process-wide active theme id.  Atomic keeps the getter lock-free;
// each setter dispatches the transition it wins synchronously on its
// own calling thread.
std::atomic<int32_t> g_active_theme_id{SAO_UI_THEME_LIGHT};
std::atomic<uint64_t> g_active_theme_generation{1};
std::atomic<uint64_t> g_native_contrast_generation{};
std::atomic<uint64_t> g_native_contrast_signature{};

struct ThemeCallbackSlot {
    sao_ui_theme_callback_handle_t handle{};
    sao_ui_theme_change_callback_t callback{};
    void* user_data{};
    std::mutex mutex;
    std::condition_variable cv;
    size_t in_flight{};
    bool active{true};
};

struct ActiveThemeCallback {
    ThemeCallbackSlot* slot{};
    ActiveThemeCallback* previous{};
};

thread_local ActiveThemeCallback* active_theme_callback = nullptr;

std::mutex& callbacks_mutex() {
    static std::mutex m;
    return m;
}

std::vector<std::shared_ptr<ThemeCallbackSlot>>& callbacks_storage() {
    static std::vector<std::shared_ptr<ThemeCallbackSlot>> v;
    return v;
}

struct ThreadThemeTransitionQueue {
    std::deque<SaoUiThemeId> pending;
};

thread_local ThreadThemeTransitionQueue theme_transition_queue;

std::mutex& theme_transition_mutex() {
    static std::mutex m;
    return m;
}

std::condition_variable& theme_transition_cv() {
    static std::condition_variable cv;
    return cv;
}

std::thread::id g_theme_transition_owner;
bool g_theme_transition_active{};

std::atomic<uint64_t> g_next_callback_handle{1};

constexpr size_t kMaxThemeJsonBytes = 1U << 20U;
using json = nlohmann::json;

struct ThemeJsonUpdate {
    bool has_theme{};
    SaoUiThemeId theme_id{SAO_UI_THEME_DARK};
    std::array<uint32_t, SAO_UI_TOKEN_COUNT> colors{};
    std::array<bool, SAO_UI_TOKEN_COUNT> has_colors{};
};

// Compile-time RGBA -> ARGB shim: keeps the pre-existing
// SaoUiColorTable / resolve_color pipeline compatible with the RGBA
// tables in theme.h.  ARGB byte layout is 0xAA_RR_GG_BB.
constexpr uint32_t rgba_to_argb(SaoColorRgba c) {
    return (static_cast<uint32_t>(c.a) << 24) |
           (static_cast<uint32_t>(c.r) << 16) |
           (static_cast<uint32_t>(c.g) <<  8) |
            static_cast<uint32_t>(c.b);
}

constexpr SaoColorRgba argb_to_rgba(uint32_t argb) {
    return SaoColorRgba{
        static_cast<uint8_t>((argb >> 16U) & 0xffU),
        static_cast<uint8_t>((argb >> 8U) & 0xffU),
        static_cast<uint8_t>(argb & 0xffU),
        static_cast<uint8_t>((argb >> 24U) & 0xffU),
    };
}

// Build the ARGB tables at translation-unit scope so the existing
// `sao_ui_theme_static_colors` / `sao_ui_theme_resolve_color`
// interface returns the real values.  Constexpr — zero runtime cost.
constexpr SaoUiColorTable make_color_table(const SaoColorRgba (&rgba)[SAO_UI_TOKEN_COUNT]) {
    SaoUiColorTable t{};
    for (int i = 0; i < SAO_UI_TOKEN_COUNT; ++i) {
        t.argb[i] = rgba_to_argb(rgba[i]);
    }
    return t;
}

constexpr SaoUiMetricTable make_metric_table(const int32_t (&values)[SAO_UI_METRIC_COUNT]) {
    SaoUiMetricTable t{};
    for (int i = 0; i < SAO_UI_METRIC_COUNT; ++i) {
        t.values[i] = values[i];
    }
    return t;
}

constexpr SaoUiColorTable kColorTables[SAO_UI_THEME_COUNT] = {
    make_color_table(sao::ui::kSaoThemeDarkColors),
    make_color_table(sao::ui::kSaoThemeLightColors),
    make_color_table(sao::ui::kSaoThemeGlassColors),
};

constexpr SaoUiMetricTable kMetricTables[SAO_UI_THEME_COUNT] = {
    make_metric_table(sao::ui::kSaoThemeMetrics),
    make_metric_table(sao::ui::kSaoThemeMetrics),
    make_metric_table(sao::ui::kSaoThemeMetrics),
};

constexpr uint32_t kPanelSemanticColorTable[SAO_UI_THEME_COUNT]
                                                [static_cast<size_t>(
                                                    sao::ui::detail::PanelSemanticColorToken::
                                                        Count)] = {
    {kColorTables[SAO_UI_THEME_DARK].argb[SAO_UI_TOKEN_APP_TEXT_2]},
    {kColorTables[SAO_UI_THEME_LIGHT].argb[SAO_UI_TOKEN_APP_TEXT_2]},
    {kColorTables[SAO_UI_THEME_GLASS].argb[SAO_UI_TOKEN_APP_TEXT_2]},
};

// ── Token name table (parallel to SaoUiColorToken order) ──────────
// Kept ASCII-only + short so the strings fit in caller buffers as
// small as 32 bytes.  Order MUST match the enum verbatim; a compile
// time static_assert below pins the size.
constexpr const char* kColorTokenNames[SAO_UI_TOKEN_COUNT] = {
    "OVERLAY_BG",           "APP_BG",                "APP_CARD",
    "APP_BORDER",           "APP_TEXT",              "APP_TEXT_2",
    "APP_TEXT_DIM",         "APP_ACCENT",            "APP_BLUE",
    "APP_GREEN",            "APP_RED",               "APP_ORANGE",
    "APP_GOLD",             "CIRCLE_BORDER",         "CIRCLE_BG",
    "CIRCLE_ICON",          "CIRCLE_ACTIVE_BORDER",  "CIRCLE_ACTIVE_BG",
    "CIRCLE_ACTIVE_ICON",   "CIRCLE_HOVER_BG",       "CIRCLE_HOVER_ICON",
    "CHILD_BG",             "CHILD_HOVER",           "CHILD_HOVER_FG",
    "CHILD_TEXT",           "CHILD_LINE",            "CHILD_ICON",
    "INFO_BG",              "INFO_BOTTOM",           "INFO_TITLE_BORDER",
    "INFO_TRIANGLE",        "ALERT_BG",              "ALERT_PANEL",
    "ALERT_TITLE_FG",       "ALERT_CONTENT_FG",      "ALERT_SHADOW",
    "CLOSE_RED",            "OK_BLUE",               "HP_BG",
    "HP_HOVER",             "HP_FONT_COLOR",         "HP_GREEN_L",
    "HP_GREEN_R",           "HP_YELLOW_L",           "HP_YELLOW_R",
    "HP_RED_L",             "HP_RED_R",              "HP_BORDER",
    "BOSS_HP_RED",          "BOSS_HP_BREAK",         "BOSS_HP_SHIELD",
    "SURFACE_LIGHT",        "TEXT_PRIMARY",          "TEXT_SECONDARY",
    "ACCENT_GOLD_WARM",     "ACCENT_CYAN_SOFT",      "CORNER_CYAN",
    "CORNER_GOLD",          "WHITE",                 "WHITE_85",
    "BLACK",                "TRANSPARENT_KEY",       "DPS_GOLD",
    "DPS_ROW_ALT",          "DPS_ROW_SELF",          "DPS_ROW_HOVER",
    "ELEM_FIRE",            "ELEM_WATER",            "ELEM_ELECTRIC",
    "ELEM_WOOD",            "ELEM_WIND",             "ELEM_ROCK",
    "ELEM_LIGHT",           "ELEM_DARK",             "ELEM_GENERIC",
    "DISABLED_FG",         "DISABLED_BG",          "DISABLED_BORDER",
    "HOVER_SURFACE",       "FOCUS_RING",           "PRESSED_SURFACE",
    "PLACEHOLDER",         "SELECTION",             "SCROLLBAR_TRACK",
    "SCROLLBAR_THUMB",     "SCROLLBAR_HOVER",       "SCROLLBAR_PRESSED",
    "TOOLTIP_SURFACE",     "LOADING",               "SKELETON",
    "ERROR_SURFACE",       "ERROR_ICON",
};

static_assert(sizeof(kColorTokenNames) / sizeof(kColorTokenNames[0]) ==
                  SAO_UI_TOKEN_COUNT,
              "kColorTokenNames size must equal SAO_UI_TOKEN_COUNT");

inline bool is_valid_theme_id(int32_t id) {
    return id >= 0 && id < SAO_UI_THEME_COUNT;
}

inline bool is_valid_color_token(int32_t token) {
    return token >= 0 && token < SAO_UI_TOKEN_COUNT;
}

inline bool is_valid_metric_token(int32_t metric) {
    return metric >= 0 && metric < SAO_UI_METRIC_COUNT;
}

struct NativeContrastPalette {
    bool active{};
    uint32_t background{0xff000000U};
    uint32_t foreground{0xffffffffU};
    uint32_t disabled{0xff808080U};
    uint32_t border{0xffffffffU};
    uint32_t highlight{0xffffffffU};
    uint32_t highlight_text{0xff000000U};
};

#if defined(_WIN32)
uint32_t system_color_argb(int32_t index) noexcept {
    const COLORREF color = ::GetSysColor(index);
    return 0xff000000U | (static_cast<uint32_t>(GetRValue(color)) << 16U) |
           (static_cast<uint32_t>(GetGValue(color)) << 8U) |
           static_cast<uint32_t>(GetBValue(color));
}
#endif

NativeContrastPalette native_contrast_palette() noexcept {
#if defined(_WIN32)
    thread_local uint64_t sampled_at{};
    thread_local NativeContrastPalette cached{};
    const uint64_t now = ::GetTickCount64();
    if (sampled_at == 0U || now - sampled_at >= 250U) {
        HIGHCONTRASTW contrast{};
        contrast.cbSize = sizeof(contrast);
        cached.active = ::SystemParametersInfoW(
                            SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) != FALSE &&
                        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0U;
        if (cached.active) {
            cached.background = system_color_argb(COLOR_WINDOW);
            cached.foreground = system_color_argb(COLOR_WINDOWTEXT);
            cached.disabled = system_color_argb(COLOR_GRAYTEXT);
            cached.border = system_color_argb(COLOR_WINDOWTEXT);
            cached.highlight = system_color_argb(COLOR_HIGHLIGHT);
            cached.highlight_text = system_color_argb(COLOR_HIGHLIGHTTEXT);
        }
        uint64_t signature = cached.active ? 1469598103934665603ULL : 0ULL;
        if (cached.active) {
            for (const uint32_t color : {cached.background, cached.foreground,
                                         cached.disabled, cached.border,
                                         cached.highlight, cached.highlight_text}) {
                signature ^= color;
                signature *= 1099511628211ULL;
            }
        }
        uint64_t previous =
            g_native_contrast_signature.load(std::memory_order_acquire);
        while (previous != signature &&
               !g_native_contrast_signature.compare_exchange_weak(
                   previous, signature, std::memory_order_acq_rel)) {
        }
        if (previous != signature)
            g_native_contrast_generation.fetch_add(1U, std::memory_order_acq_rel);
        sampled_at = now == 0U ? 1U : now;
    }
    return cached;
#else
    return {};
#endif
}

uint32_t native_contrast_color(SaoUiColorToken token,
                               const NativeContrastPalette& palette,
                               uint32_t fallback) noexcept {
    if (!palette.active)
        return fallback;
    switch (token) {
    case SAO_UI_TOKEN_OVERLAY_BG:
    case SAO_UI_TOKEN_APP_BG:
    case SAO_UI_TOKEN_APP_CARD:
    case SAO_UI_TOKEN_CIRCLE_BG:
    case SAO_UI_TOKEN_CIRCLE_ACTIVE_BG:
    case SAO_UI_TOKEN_CIRCLE_HOVER_BG:
    case SAO_UI_TOKEN_CHILD_BG:
    case SAO_UI_TOKEN_CHILD_HOVER:
    case SAO_UI_TOKEN_INFO_BG:
    case SAO_UI_TOKEN_INFO_BOTTOM:
    case SAO_UI_TOKEN_ALERT_BG:
    case SAO_UI_TOKEN_ALERT_PANEL:
    case SAO_UI_TOKEN_ALERT_SHADOW:
    case SAO_UI_TOKEN_HP_BG:
    case SAO_UI_TOKEN_HP_HOVER:
    case SAO_UI_TOKEN_SURFACE_LIGHT:
    case SAO_UI_TOKEN_DPS_ROW_ALT:
    case SAO_UI_TOKEN_DPS_ROW_SELF:
    case SAO_UI_TOKEN_DPS_ROW_HOVER:
    case SAO_UI_TOKEN_DISABLED_BG:
    case SAO_UI_TOKEN_HOVER_SURFACE:
    case SAO_UI_TOKEN_PRESSED_SURFACE:
    case SAO_UI_TOKEN_SCROLLBAR_TRACK:
    case SAO_UI_TOKEN_TOOLTIP_SURFACE:
    case SAO_UI_TOKEN_LOADING:
    case SAO_UI_TOKEN_SKELETON:
    case SAO_UI_TOKEN_ERROR_SURFACE:
        return palette.background;
    case SAO_UI_TOKEN_APP_BORDER:
    case SAO_UI_TOKEN_CIRCLE_BORDER:
    case SAO_UI_TOKEN_CIRCLE_ACTIVE_BORDER:
    case SAO_UI_TOKEN_CHILD_LINE:
    case SAO_UI_TOKEN_INFO_TITLE_BORDER:
    case SAO_UI_TOKEN_HP_BORDER:
    case SAO_UI_TOKEN_DISABLED_BORDER:
    case SAO_UI_TOKEN_SCROLLBAR_THUMB:
    case SAO_UI_TOKEN_SCROLLBAR_HOVER:
    case SAO_UI_TOKEN_SCROLLBAR_PRESSED:
        return palette.border;
    case SAO_UI_TOKEN_DISABLED_FG:
    case SAO_UI_TOKEN_PLACEHOLDER:
        return palette.disabled;
    case SAO_UI_TOKEN_APP_TEXT:
    case SAO_UI_TOKEN_APP_TEXT_2:
    case SAO_UI_TOKEN_APP_TEXT_DIM:
    case SAO_UI_TOKEN_CIRCLE_ICON:
    case SAO_UI_TOKEN_CIRCLE_ACTIVE_ICON:
    case SAO_UI_TOKEN_CIRCLE_HOVER_ICON:
    case SAO_UI_TOKEN_CHILD_HOVER_FG:
    case SAO_UI_TOKEN_CHILD_TEXT:
    case SAO_UI_TOKEN_CHILD_ICON:
    case SAO_UI_TOKEN_ALERT_TITLE_FG:
    case SAO_UI_TOKEN_ALERT_CONTENT_FG:
    case SAO_UI_TOKEN_HP_FONT_COLOR:
    case SAO_UI_TOKEN_TEXT_PRIMARY:
    case SAO_UI_TOKEN_TEXT_SECONDARY:
    case SAO_UI_TOKEN_BLACK:
        return palette.foreground;
    case SAO_UI_TOKEN_WHITE:
    case SAO_UI_TOKEN_WHITE_85:
        return palette.highlight_text;
    case SAO_UI_TOKEN_APP_ACCENT:
    case SAO_UI_TOKEN_APP_BLUE:
    case SAO_UI_TOKEN_APP_GREEN:
    case SAO_UI_TOKEN_APP_RED:
    case SAO_UI_TOKEN_APP_ORANGE:
    case SAO_UI_TOKEN_APP_GOLD:
    case SAO_UI_TOKEN_INFO_TRIANGLE:
    case SAO_UI_TOKEN_CLOSE_RED:
    case SAO_UI_TOKEN_OK_BLUE:
    case SAO_UI_TOKEN_HP_GREEN_L:
    case SAO_UI_TOKEN_HP_GREEN_R:
    case SAO_UI_TOKEN_HP_YELLOW_L:
    case SAO_UI_TOKEN_HP_YELLOW_R:
    case SAO_UI_TOKEN_HP_RED_L:
    case SAO_UI_TOKEN_HP_RED_R:
    case SAO_UI_TOKEN_BOSS_HP_RED:
    case SAO_UI_TOKEN_BOSS_HP_BREAK:
    case SAO_UI_TOKEN_BOSS_HP_SHIELD:
    case SAO_UI_TOKEN_ACCENT_GOLD_WARM:
    case SAO_UI_TOKEN_ACCENT_CYAN_SOFT:
    case SAO_UI_TOKEN_CORNER_CYAN:
    case SAO_UI_TOKEN_CORNER_GOLD:
    case SAO_UI_TOKEN_DPS_GOLD:
    case SAO_UI_TOKEN_ELEM_FIRE:
    case SAO_UI_TOKEN_ELEM_WATER:
    case SAO_UI_TOKEN_ELEM_ELECTRIC:
    case SAO_UI_TOKEN_ELEM_WOOD:
    case SAO_UI_TOKEN_ELEM_WIND:
    case SAO_UI_TOKEN_ELEM_ROCK:
    case SAO_UI_TOKEN_ELEM_LIGHT:
    case SAO_UI_TOKEN_ELEM_DARK:
    case SAO_UI_TOKEN_ELEM_GENERIC:
    case SAO_UI_TOKEN_FOCUS_RING:
    case SAO_UI_TOKEN_SELECTION:
    case SAO_UI_TOKEN_ERROR_ICON:
        return palette.highlight;
    default:
        return fallback;
    }
}

std::optional<SaoUiThemeId> theme_id_from_json(const json& value) {
    if (value.is_number_integer()) {
        const int64_t id = value.get<int64_t>();
        if (id >= 0 && id < SAO_UI_THEME_COUNT)
            return static_cast<SaoUiThemeId>(id);
        return std::nullopt;
    }
    if (!value.is_string())
        return std::nullopt;
    std::string name = value.get<std::string>();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (name == "dark")
        return SAO_UI_THEME_DARK;
    if (name == "light")
        return SAO_UI_THEME_LIGHT;
    if (name == "glass")
        return SAO_UI_THEME_GLASS;
    return std::nullopt;
}

std::optional<SaoUiColorToken> color_token_from_name(std::string_view name) {
    std::array<char, 64> token_name{};
    for (int32_t index = 0; index < SAO_UI_COLOR_TOKEN_COUNT; ++index) {
        const auto token = static_cast<SaoUiColorToken>(index);
        if (sao_ui_theme_get_token_name(token, token_name.data(), token_name.size()) ==
                SAO_STATUS_OK &&
            name == token_name.data()) {
            return token;
        }
    }
    return std::nullopt;
}

bool argb_from_json(const json& value, uint32_t* out_argb) {
    if (out_argb == nullptr)
        return false;
    if (value.is_number_unsigned()) {
        const uint64_t raw = value.get<uint64_t>();
        if (raw > UINT32_MAX)
            return false;
        *out_argb = static_cast<uint32_t>(raw);
        return true;
    }
    if (!value.is_string())
        return false;
    const std::string& text = value.get_ref<const std::string&>();
    if ((text.size() != 7U && text.size() != 9U) || text.front() != '#')
        return false;
    uint32_t parsed = 0;
    for (size_t index = 1; index < text.size(); ++index) {
        const char ch = text[index];
        uint32_t nibble = 0;
        if (ch >= '0' && ch <= '9')
            nibble = static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            nibble = static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            nibble = static_cast<uint32_t>(ch - 'A' + 10);
        else
            return false;
        parsed = (parsed << 4U) | nibble;
    }
    *out_argb = text.size() == 7U ? 0xff000000U | parsed
                                  : ((parsed & 0xffU) << 24U) | (parsed >> 8U);
    return true;
}

sao_status_t parse_theme_json(const uint8_t* json_utf8, size_t json_len,
                              ThemeJsonUpdate* out_update) {
    if (out_update == nullptr || json_utf8 == nullptr || json_len == 0U ||
        json_len > kMaxThemeJsonBytes)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        const auto* begin = reinterpret_cast<const char*>(json_utf8);
        const json document = json::parse(begin, begin + json_len, nullptr, false, false);
        if (document.is_discarded() || !document.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;

        ThemeJsonUpdate parsed{};
        const auto theme_property = document.find("theme_id");
        if (theme_property != document.end()) {
            const auto theme_id = theme_id_from_json(*theme_property);
            if (!theme_id.has_value())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            parsed.has_theme = true;
            parsed.theme_id = *theme_id;
        }

        const auto colors_property = document.find("colors");
        if (colors_property != document.end() && !colors_property->is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const auto parse_colors = [&parsed](const json& object) {
            for (auto property = object.begin(); property != object.end(); ++property) {
                const auto token = color_token_from_name(property.key());
                if (!token.has_value())
                    continue;
                uint32_t argb = 0;
                if (!argb_from_json(property.value(), &argb))
                    return false;
                const size_t index = static_cast<size_t>(*token);
                parsed.colors[index] = argb;
                parsed.has_colors[index] = true;
            }
            return true;
        };
        if (!parse_colors(document))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (colors_property != document.end() && !parse_colors(*colors_property))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        *out_update = parsed;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

bool callback_is_active_on_this_thread(const ThemeCallbackSlot* slot) noexcept {
    for (const ActiveThemeCallback* active = active_theme_callback; active != nullptr;
         active = active->previous) {
        if (active->slot == slot)
            return true;
    }
    return false;
}

void invoke_theme_callbacks(SaoUiThemeId theme_id) noexcept {
    std::vector<std::shared_ptr<ThemeCallbackSlot>> snapshot;
    try {
        std::lock_guard lock(callbacks_mutex());
        snapshot = callbacks_storage();
    } catch (...) {
        return;
    }
    for (const auto& slot : snapshot) {
        sao_ui_theme_change_callback_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard slot_lock(slot->mutex);
            if (!slot->active || slot->callback == nullptr)
                continue;
            ++slot->in_flight;
            callback = slot->callback;
            user_data = slot->user_data;
        }
        ActiveThemeCallback marker{slot.get(), active_theme_callback};
        active_theme_callback = &marker;
        try {
            callback(theme_id, user_data);
        } catch (...) {
        }
        active_theme_callback = marker.previous;
        {
            std::lock_guard slot_lock(slot->mutex);
            --slot->in_flight;
        }
        slot->cv.notify_all();
    }
}

}  // namespace

namespace sao::ui::detail {

std::array<std::atomic<int32_t>, SAO_UI_METRIC_TOKEN_COUNT>
    g_panel_metric_test_overrides{};
thread_local const PanelResolvedTheme* g_panel_paint_theme = nullptr;

uint64_t process_theme_generation() noexcept {
    (void)native_contrast_palette();
    return g_active_theme_generation.load(std::memory_order_acquire) +
           g_native_contrast_generation.load(std::memory_order_acquire);
}

int32_t resolve_panel_metric(SaoUiThemeId theme_id, SaoUiMetricToken metric) noexcept {
    if (!is_valid_metric_token(metric))
        return 0;
    const int32_t test_override =
        g_panel_metric_test_overrides[static_cast<size_t>(metric)].load(std::memory_order_acquire);
    if (test_override > 0)
        return test_override;
    const SaoUiThemeId resolved_theme =
        is_valid_theme_id(theme_id) ? theme_id : SAO_UI_THEME_DARK;
    return kMetricTables[resolved_theme].values[metric];
}

PanelResolvedTheme resolve_theme(SaoUiThemeId theme_id, uint64_t generation) noexcept {
    const SaoUiThemeId resolved_theme =
        is_valid_theme_id(theme_id) ? theme_id : SAO_UI_THEME_DARK;
    PanelResolvedTheme resolved{};
    resolved.theme_id = resolved_theme;
    resolved.generation = generation;
    const NativeContrastPalette contrast = native_contrast_palette();
    resolved.high_contrast = contrast.active;
    for (int32_t token = 0; token < SAO_UI_COLOR_TOKEN_COUNT; ++token) {
        resolved.colors[static_cast<size_t>(token)] = native_contrast_color(
            static_cast<SaoUiColorToken>(token), contrast,
            kColorTables[resolved_theme].argb[token]);
    }
    for (int32_t metric = 0; metric < SAO_UI_METRIC_TOKEN_COUNT; ++metric) {
        resolved.metrics[static_cast<size_t>(metric)] = resolve_panel_metric(
            resolved_theme, static_cast<SaoUiMetricToken>(metric));
    }
    return resolved;
}

PanelResolvedTheme resolve_process_theme() noexcept {
    SaoUiThemeId theme_id = SAO_UI_THEME_DARK;
    uint64_t before = 0;
    uint64_t after = 0;
    do {
        before = process_theme_generation();
        (void)sao_ui_theme_get_active_id(&theme_id);
        after = process_theme_generation();
    } while (before != after);
    return resolve_theme(theme_id, after);
}

uint32_t panel_theme_color(SaoUiColorToken token) noexcept {
    if (!is_valid_color_token(token))
        return kColorTables[SAO_UI_THEME_DARK].argb[SAO_UI_TOKEN_BLACK];
    if (g_panel_paint_theme != nullptr)
        return g_panel_paint_theme->colors[static_cast<size_t>(token)];
    const SaoUiThemeId theme_id =
        static_cast<SaoUiThemeId>(g_active_theme_id.load(std::memory_order_acquire));
    const SaoUiThemeId resolved_theme =
        is_valid_theme_id(theme_id) ? theme_id : SAO_UI_THEME_DARK;
    return native_contrast_color(
        token, native_contrast_palette(), kColorTables[resolved_theme].argb[token]);
}

uint32_t panel_theme_color(PanelSemanticColorToken token) noexcept {
    const int32_t token_index = static_cast<int32_t>(token);
    if (token_index < 0 || token_index >= static_cast<int32_t>(PanelSemanticColorToken::Count))
        return panel_theme_color(SAO_UI_TOKEN_BLACK);
    if (g_panel_paint_theme != nullptr && g_panel_paint_theme->high_contrast)
        return g_panel_paint_theme->colors[SAO_UI_TOKEN_APP_TEXT];
    const NativeContrastPalette contrast = native_contrast_palette();
    if (contrast.active)
        return contrast.foreground;
    SaoUiThemeId theme_id = SAO_UI_THEME_DARK;
    if (g_panel_paint_theme != nullptr) {
        theme_id = g_panel_paint_theme->theme_id;
    } else {
        theme_id = static_cast<SaoUiThemeId>(
            g_active_theme_id.load(std::memory_order_acquire));
    }
    if (!is_valid_theme_id(theme_id))
        theme_id = SAO_UI_THEME_DARK;
    return kPanelSemanticColorTable[theme_id][static_cast<size_t>(token_index)];
}

int32_t panel_theme_metric(SaoUiMetricToken metric) noexcept {
    if (!is_valid_metric_token(metric))
        return 0;
    if (g_panel_paint_theme != nullptr)
        return g_panel_paint_theme->metrics[static_cast<size_t>(metric)];
    const SaoUiThemeId theme_id =
        static_cast<SaoUiThemeId>(g_active_theme_id.load(std::memory_order_acquire));
    return resolve_panel_metric(theme_id, metric);
}

bool panel_theme_high_contrast() noexcept {
    if (g_panel_paint_theme != nullptr)
        return g_panel_paint_theme->high_contrast;
    return native_contrast_palette().active;
}

} // namespace sao::ui::detail

// ── Static table access (unchanged public API) ────────────────────
extern "C" const SaoUiColorTable* SAO_UI_CALL sao_ui_theme_static_colors(
    SaoUiThemeId theme_id) {
    if (!is_valid_theme_id(theme_id)) {
        return &kColorTables[SAO_UI_THEME_DARK];
    }
    return &kColorTables[theme_id];
}

extern "C" const SaoUiMetricTable* SAO_UI_CALL sao_ui_theme_static_metrics(
    SaoUiThemeId theme_id) {
    if (!is_valid_theme_id(theme_id)) {
        return &kMetricTables[SAO_UI_THEME_DARK];
    }
    return &kMetricTables[theme_id];
}

extern "C" uint32_t SAO_UI_CALL sao_ui_theme_resolve_color(
    SaoUiThemeId theme_id, SaoUiColorToken token) {
    if (!is_valid_theme_id(theme_id) || !is_valid_color_token(token)) {
        return kColorTables[SAO_UI_THEME_DARK].argb[SAO_UI_TOKEN_BLACK];
    }
    return kColorTables[theme_id].argb[token];
}

extern "C" int32_t SAO_UI_CALL sao_ui_theme_resolve_metric(
    SaoUiThemeId theme_id, SaoUiMetricToken metric) {
    if (!is_valid_theme_id(theme_id) || !is_valid_metric_token(metric)) {
        return 0;
    }
    return kMetricTables[theme_id].values[metric];
}

// ── Handle-less flat-token API (G3.1) ─────────────────────────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_get_color_by_id(
    SaoUiThemeId theme_id,
    SaoUiColorToken token,
    SaoColorRgba* out_rgba) {
    if (out_rgba == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!is_valid_theme_id(theme_id) || !is_valid_color_token(token)) {
        *out_rgba = argb_to_rgba(
            kColorTables[SAO_UI_THEME_DARK].argb[SAO_UI_TOKEN_BLACK]);
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_rgba = argb_to_rgba(kColorTables[theme_id].argb[token]);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_get_metric_by_id(
    SaoUiThemeId theme_id,
    SaoUiMetricToken metric,
    int32_t* out_value) {
    if (out_value == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!is_valid_theme_id(theme_id) || !is_valid_metric_token(metric)) {
        *out_value = 0;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_value = kMetricTables[theme_id].values[metric];
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_set_active_id(
    SaoUiThemeId theme_id) {
    if (!is_valid_theme_id(theme_id))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto& queue = theme_transition_queue;
        const std::thread::id caller = std::this_thread::get_id();
        {
            std::unique_lock lock(theme_transition_mutex());
            if (g_theme_transition_active && g_theme_transition_owner != caller)
                theme_transition_cv().wait(lock, [] { return !g_theme_transition_active; });
            if (g_theme_transition_active) {
                queue.pending.push_back(theme_id);
                return SAO_STATUS_OK;
            }
            g_theme_transition_active = true;
            g_theme_transition_owner = caller;
            queue.pending.push_back(theme_id);
        }
        while (true) {
            SaoUiThemeId requested = SAO_UI_THEME_DARK;
            {
                std::lock_guard lock(theme_transition_mutex());
                if (queue.pending.empty()) {
                    g_theme_transition_active = false;
                    g_theme_transition_owner = {};
                    theme_transition_cv().notify_all();
                    return SAO_STATUS_OK;
                }
                requested = queue.pending.front();
                queue.pending.pop_front();
            }
            const int32_t previous = g_active_theme_id.exchange(requested);
            if (previous != requested) {
                g_active_theme_generation.fetch_add(1, std::memory_order_acq_rel);
                invoke_theme_callbacks(requested);
            }
        }
    } catch (...) {
        std::lock_guard lock(theme_transition_mutex());
        theme_transition_queue.pending.clear();
        if (g_theme_transition_owner == std::this_thread::get_id()) {
            g_theme_transition_active = false;
            g_theme_transition_owner = {};
            theme_transition_cv().notify_all();
        }
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_get_active_id(
    SaoUiThemeId* out_theme_id) {
    if (out_theme_id == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_theme_id = static_cast<SaoUiThemeId>(g_active_theme_id.load());
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_register_change_callback(
    sao_ui_theme_change_callback_t callback,
    void* user_data,
    sao_ui_theme_callback_handle_t* out_handle) {
    if (callback == nullptr || out_handle == nullptr) {
        if (out_handle != nullptr) *out_handle = SAO_UI_THEME_CALLBACK_HANDLE_INVALID;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = SAO_UI_THEME_CALLBACK_HANDLE_INVALID;
    try {
        auto slot = std::make_shared<ThemeCallbackSlot>();
        slot->handle = g_next_callback_handle.fetch_add(1);
        if (slot->handle == SAO_UI_THEME_CALLBACK_HANDLE_INVALID)
            slot->handle = g_next_callback_handle.fetch_add(1);
        slot->callback = callback;
        slot->user_data = user_data;
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex());
            callbacks_storage().push_back(slot);
        }
        *out_handle = slot->handle;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_unregister_change_callback(
    sao_ui_theme_callback_handle_t handle) {
    if (handle == SAO_UI_THEME_CALLBACK_HANDLE_INVALID) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::shared_ptr<ThemeCallbackSlot> slot;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex());
        auto& slots = callbacks_storage();
        for (auto it = slots.begin(); it != slots.end(); ++it) {
            if ((*it)->handle == handle) {
                slot = *it;
                slots.erase(it);
                break;
            }
        }
    }
    if (slot == nullptr)
        return SAO_STATUS_ERR_NOT_FOUND;
    std::unique_lock slot_lock(slot->mutex);
    slot->active = false;
    if (!callback_is_active_on_this_thread(slot.get())) {
        slot->cv.wait(slot_lock, [&slot] { return slot->in_flight == 0; });
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_get_token_name(
    SaoUiColorToken token,
    char* out_name,
    size_t capacity) {
    if (out_name == nullptr || capacity == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!is_valid_color_token(token)) {
        out_name[0] = '\0';
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const char* name = kColorTokenNames[token];
    const size_t need = std::strlen(name) + 1;
    if (capacity < need) {
        // Still terminate what we can — makes logging easier.
        out_name[0] = '\0';
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(out_name, name, need);
    return SAO_STATUS_OK;
}

extern "C" int32_t SAO_UI_CALL sao_ui_theme_get_color_token_count(void) {
    return SAO_UI_TOKEN_COUNT;
}

extern "C" int32_t SAO_UI_CALL sao_ui_theme_get_metric_token_count(void) {
    return SAO_UI_METRIC_COUNT;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_create(
    sao_ui_theme_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = new (std::nothrow) sao_ui_theme_s();
    return *out_handle == nullptr ? SAO_STATUS_ERR_UNKNOWN : SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_theme_destroy(sao_ui_theme_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_set_active(
    sao_ui_theme_handle_t handle, SaoUiThemeId theme_id) {
    if (handle == nullptr || !is_valid_theme_id(theme_id)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::vector<ThemeListener> listeners;
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (handle->active == theme_id) return SAO_STATUS_OK;
        handle->active = theme_id;
        listeners = handle->listeners;
    }
    for (const ThemeListener& listener : listeners) {
        if (listener.callback == nullptr) continue;
        try {
            listener.callback(theme_id, listener.user_data);
        } catch (...) {
        }
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_get_active(
    sao_ui_theme_handle_t handle, SaoUiThemeId* out_theme_id) {
    if (handle == nullptr || out_theme_id == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    *out_theme_id = handle->active;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_get_color(
    sao_ui_theme_handle_t handle, SaoUiColorToken token, uint32_t* out_argb) {
    if (handle == nullptr || out_argb == nullptr || !is_valid_color_token(token)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    const size_t index = static_cast<size_t>(token);
    *out_argb = handle->has_color_overrides[index]
                    ? handle->color_overrides[index]
                    : sao_ui_theme_resolve_color(handle->active, token);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_get_metric(
    sao_ui_theme_handle_t handle, SaoUiMetricToken metric, int32_t* out_value) {
    if (handle == nullptr || out_value == nullptr || !is_valid_metric_token(metric)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    *out_value = sao_ui_theme_resolve_metric(handle->active, metric);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_register_panel_override(
    sao_ui_theme_handle_t handle, const char* panel_key_utf8, SaoUiThemeId theme_id,
    SaoUiColorToken token, uint32_t argb) {
    return sao_ui_theme_register_panel_override_for_owner(handle, 0, panel_key_utf8, theme_id, token, argb);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_register_panel_override_for_owner(
    sao_ui_theme_handle_t handle, sao_ui_theme_owner_t owner, const char* panel_key_utf8,
    SaoUiThemeId theme_id, SaoUiColorToken token, uint32_t argb) {
    if (handle == nullptr || panel_key_utf8 == nullptr || panel_key_utf8[0] == '\0' || !is_valid_theme_id(theme_id) || !is_valid_color_token(token)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    for (ThemePanelOverride& entry : handle->overrides) {
        if (entry.owner == owner && entry.panel_key == panel_key_utf8 && entry.theme_id == theme_id && entry.token == token) {
            entry.argb = argb;
            return SAO_STATUS_OK;
        }
    }
    handle->overrides.push_back({owner, panel_key_utf8, theme_id, token, argb});
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_release_owner(
    sao_ui_theme_handle_t handle, sao_ui_theme_owner_t owner) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    auto& entries = handle->overrides;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [owner](const ThemePanelOverride& entry) { return entry.owner == owner; }), entries.end());
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_get_panel_color(
    sao_ui_theme_handle_t handle, const char* panel_key_utf8, SaoUiColorToken token,
    uint32_t* out_argb) {
    if (handle == nullptr || panel_key_utf8 == nullptr || out_argb == nullptr || !is_valid_color_token(token)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    for (auto entry = handle->overrides.rbegin(); entry != handle->overrides.rend(); ++entry) {
        if (entry->panel_key == panel_key_utf8 && entry->theme_id == handle->active && entry->token == token) {
            *out_argb = entry->argb;
            return SAO_STATUS_OK;
        }
    }
    const size_t index = static_cast<size_t>(token);
    *out_argb = handle->has_color_overrides[index]
                    ? handle->color_overrides[index]
                    : sao_ui_theme_resolve_color(handle->active, token);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_load_json(
    sao_ui_theme_handle_t handle, const uint8_t* json_utf8, size_t json_len) {
    if (handle == nullptr || (json_utf8 == nullptr && json_len != 0U))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ThemeJsonUpdate update{};
    const sao_status_t parse_status = parse_theme_json(json_utf8, json_len, &update);
    if (parse_status != SAO_STATUS_OK)
        return parse_status;

    std::vector<ThemeListener> listeners;
    SaoUiThemeId selected = SAO_UI_THEME_DARK;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        selected = update.has_theme ? update.theme_id : handle->active;
        changed = handle->active != selected;
        handle->active = selected;
        handle->color_overrides = update.colors;
        handle->has_color_overrides = update.has_colors;
        if (changed)
            listeners = handle->listeners;
    }
    if (changed) {
        for (const ThemeListener& listener : listeners) {
            if (listener.callback == nullptr)
                continue;
            try {
                listener.callback(selected, listener.user_data);
            } catch (...) {
            }
        }
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_add_listener(
    sao_ui_theme_handle_t handle, sao_ui_theme_changed_callback_t callback, void* user_data) {
    if (handle == nullptr || callback == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    handle->listeners.push_back({callback, user_data});
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_remove_listener(
    sao_ui_theme_handle_t handle, sao_ui_theme_changed_callback_t callback, void* user_data) {
    if (handle == nullptr || callback == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    auto& listeners = handle->listeners;
    const auto before = listeners.size();
    listeners.erase(std::remove_if(listeners.begin(), listeners.end(), [callback, user_data](const ThemeListener& listener) { return listener.callback == callback && listener.user_data == user_data; }), listeners.end());
    return listeners.size() == before ? SAO_STATUS_ERR_NOT_FOUND : SAO_STATUS_OK;
}
