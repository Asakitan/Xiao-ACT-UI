// SAO Auto — theme implementation (Wave2 flat-token tables + handle stub).
//
// The Wave2 API is production-ready: three constexpr colour tables
// (dark / light / glass) + one metric table live in theme.h, and all
// runtime accessors below just read from them.  The handle-based API
// remains a skeleton — Wave3 will layer per-panel overrides on top.

#include "sao/ui/theme.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
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
    SaoUiThemeId active{SAO_UI_THEME_DARK};
    std::vector<ThemePanelOverride> overrides;
    std::vector<ThemeListener> listeners;
    std::mutex mutex;
};

// ── Wave2 static state ────────────────────────────────────────────
namespace {

// Process-wide active theme id.  atomic so the getter is lock-free;
// mutations still take the callback mutex so listeners fire in a
// consistent order.
std::atomic<int32_t> g_active_theme_id{SAO_UI_THEME_DARK};

struct ThemeCallbackSlot {
    sao_ui_theme_callback_handle_t   handle;
    sao_ui_theme_change_callback_t   callback;
    void*                            user_data;
};

std::mutex& callbacks_mutex() {
    static std::mutex m;
    return m;
}

std::vector<ThemeCallbackSlot>& callbacks_storage() {
    static std::vector<ThemeCallbackSlot> v;
    return v;
}

std::atomic<uint64_t> g_next_callback_handle{1};

// Compile-time RGBA -> ARGB shim: keeps the pre-existing
// SaoUiColorTable / resolve_color pipeline compatible with the wave2
// tables in theme.h.  ARGB byte layout is 0xAA_RR_GG_BB.
constexpr uint32_t rgba_to_argb(SaoColorRgba c) {
    return (static_cast<uint32_t>(c.a) << 24) |
           (static_cast<uint32_t>(c.r) << 16) |
           (static_cast<uint32_t>(c.g) <<  8) |
            static_cast<uint32_t>(c.b);
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

}  // namespace

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
        return 0xff000000u;
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

// ── Wave2 handle-less API (G3.1 delivery) ─────────────────────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_get_color_by_id(
    SaoUiThemeId theme_id,
    SaoUiColorToken token,
    SaoColorRgba* out_rgba) {
    if (out_rgba == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!is_valid_theme_id(theme_id) || !is_valid_color_token(token)) {
        *out_rgba = SaoColorRgba{0, 0, 0, 0xFF};
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_rgba = sao::ui::theme_colors_for(theme_id)[token];
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
    // Metrics are theme-agnostic in Wave2, so we ignore theme_id past
    // the range check (the enum-count-3 table is identical per row).
    *out_value = sao::ui::kSaoThemeMetrics[metric];
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_set_active_id(
    SaoUiThemeId theme_id) {
    if (!is_valid_theme_id(theme_id)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    // Snapshot old value; fire callbacks only on a real change.  We
    // copy the callback list under the mutex, then release it before
    // invoking each callback so re-entrant unregister calls don't
    // deadlock.
    const int32_t prev = g_active_theme_id.exchange(theme_id);
    if (prev == theme_id) {
        return SAO_STATUS_OK;
    }
    std::vector<ThemeCallbackSlot> snapshot;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex());
        snapshot = callbacks_storage();
    }
    for (const auto& slot : snapshot) {
        if (slot.callback != nullptr) {
            slot.callback(theme_id, slot.user_data);
        }
    }
    return SAO_STATUS_OK;
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
    const auto handle = g_next_callback_handle.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex());
        callbacks_storage().push_back({handle, callback, user_data});
    }
    *out_handle = handle;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_unregister_change_callback(
    sao_ui_theme_callback_handle_t handle) {
    if (handle == SAO_UI_THEME_CALLBACK_HANDLE_INVALID) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(callbacks_mutex());
    auto& slots = callbacks_storage();
    for (auto it = slots.begin(); it != slots.end(); ++it) {
        if (it->handle == handle) {
            slots.erase(it);
            return SAO_STATUS_OK;
        }
    }
    return SAO_STATUS_ERR_NOT_FOUND;
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
        if (listener.callback != nullptr) listener.callback(theme_id, listener.user_data);
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
    *out_argb = sao_ui_theme_resolve_color(handle->active, token);
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
    *out_argb = sao_ui_theme_resolve_color(handle->active, token);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_theme_load_json(
    sao_ui_theme_handle_t handle, const uint8_t* json_utf8, size_t json_len) {
    if (handle == nullptr || (json_utf8 == nullptr && json_len != 0U)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const std::string text(reinterpret_cast<const char*>(json_utf8), json_len);
    SaoUiThemeId selected = SAO_UI_THEME_DARK;
    if (text.find("\"light\"") != std::string::npos || text.find("\"theme_id\":1") != std::string::npos) selected = SAO_UI_THEME_LIGHT;
    if (text.find("\"glass\"") != std::string::npos || text.find("\"theme_id\":2") != std::string::npos) selected = SAO_UI_THEME_GLASS;
    return sao_ui_theme_set_active(handle, selected);
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
