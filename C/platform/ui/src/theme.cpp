// SAO Auto — theme implementation (flat-token tables + handle API).
//
// The flat-token API is production-ready: three constexpr colour tables
// (dark / light / glass) + one metric table live in theme.h, and all
// runtime accessors below just read from them.  The handle-based API
// also layers per-panel overrides on top.

#include "sao/ui/theme.h"

#include "panel_theme_internal.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
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

// ── Process-wide flat-token state ────────────────────────────────
namespace {

// Process-wide active theme id.  Atomic keeps the getter lock-free;
// each setter dispatches the transition it wins synchronously on its
// own calling thread.
std::atomic<int32_t> g_active_theme_id{SAO_UI_THEME_DARK};
std::atomic<uint64_t> g_active_theme_generation{1};

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
    bool dispatching{};
};

thread_local ThreadThemeTransitionQueue theme_transition_queue;

std::atomic<uint64_t> g_next_callback_handle{1};

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
    {0xff808080U},
    {0xff808080U},
    {0xff808080U},
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
    return g_active_theme_generation.load(std::memory_order_acquire);
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
    for (int32_t token = 0; token < SAO_UI_COLOR_TOKEN_COUNT; ++token) {
        resolved.colors[static_cast<size_t>(token)] = kColorTables[resolved_theme].argb[token];
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
    return kColorTables[is_valid_theme_id(theme_id) ? theme_id : SAO_UI_THEME_DARK].argb[token];
}

uint32_t panel_theme_color(PanelSemanticColorToken token) noexcept {
    const int32_t token_index = static_cast<int32_t>(token);
    if (token_index < 0 || token_index >= static_cast<int32_t>(PanelSemanticColorToken::Count))
        return panel_theme_color(SAO_UI_TOKEN_BLACK);
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
    if (!is_valid_theme_id(theme_id)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto& queue = theme_transition_queue;
        queue.pending.push_back(theme_id);
        if (queue.dispatching)
            return SAO_STATUS_OK;

        queue.dispatching = true;
        while (!queue.pending.empty()) {
            const SaoUiThemeId requested = queue.pending.front();
            queue.pending.pop_front();
            const int32_t previous = g_active_theme_id.exchange(requested);
            if (previous != requested) {
                g_active_theme_generation.fetch_add(1, std::memory_order_acq_rel);
                invoke_theme_callbacks(requested);
            }
        }
        queue.dispatching = false;
        return SAO_STATUS_OK;
    } catch (...) {
        theme_transition_queue.pending.clear();
        theme_transition_queue.dispatching = false;
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
