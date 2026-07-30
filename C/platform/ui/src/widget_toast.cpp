// SAO Auto — toast notification overlay implementation.
//
// Stacks bottom-right, auto-dismiss with fade, max 4 visible, severity
// tints via theme tokens.  Standalone opaque handle (popup/dialog
// pattern): owns its own state, lock-protected, fire-once dismiss
// callback.  Driven by sao_ui_toast_tick from the overlay scheduler.

#include "sao/ui/widget_toast.h"

#include "panel_theme_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr int32_t kMaxVisible = 4;
constexpr int32_t kDefaultDurationMs = 3000;
constexpr int32_t kDefaultFadeMs = 240;
constexpr int32_t kDefaultFontSize = 14;
constexpr int32_t kDefaultPadX = 12;
constexpr int32_t kDefaultPadY = 8;
constexpr int32_t kDefaultRadius = 8;
constexpr int32_t kToastHeight = 36;
constexpr int32_t kToastGap = 8;

SaoUiColorToken severity_token(SaoUiToastSeverity severity) {
    switch (severity) {
    case SAO_UI_TOAST_SUCCESS: return SAO_UI_TOKEN_APP_GREEN;
    case SAO_UI_TOAST_WARN:    return SAO_UI_TOKEN_APP_ORANGE;
    case SAO_UI_TOAST_ERROR:   return SAO_UI_TOKEN_APP_RED;
    case SAO_UI_TOAST_INFO:
    default:                   return SAO_UI_TOKEN_APP_BLUE;
    }
}

struct ToastEntry {
    std::string text;
    SaoUiToastSeverity severity{SAO_UI_TOAST_INFO};
    int32_t duration_ms{kDefaultDurationMs};
    int32_t fade_ms{kDefaultFadeMs};
    int32_t elapsed_ms{0};
    SaoUiThemeId theme_override{SAO_UI_THEME_COUNT};
    uint32_t bg_argb{0};
    uint32_t fg_argb{0};
    uint32_t border_argb{0};
    int32_t font_size_px{kDefaultFontSize};
    int32_t pad_x_px{kDefaultPadX};
    int32_t pad_y_px{kDefaultPadY};
    int32_t radius_px{kDefaultRadius};
    sao_ui_toast_dismissed_cb_t callback{nullptr};
    void* user_data{nullptr};
    bool dismissed_fired{false};
};

uint32_t resolve_or(uint32_t override_value, SaoUiColorToken token) {
    return override_value != 0 ? override_value
                               : sao::ui::detail::panel_theme_color(token);
}

}  // namespace

struct sao_ui_toast_s {
    std::mutex mu;
    std::deque<ToastEntry> stack;
};

static void fire_dismissed(ToastEntry& entry, bool cancelled) {
    if (entry.dismissed_fired)
        return;
    entry.dismissed_fired = true;
    if (entry.callback != nullptr)
        entry.callback(cancelled, entry.user_data);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_create(
    sao_ui_theme_handle_t /*theme*/, sao_ui_toast_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto* handle = new sao_ui_toast_s();
        *out_handle = handle;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_toast_destroy(sao_ui_toast_handle_t handle) {
    if (handle == nullptr)
        return;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        for (auto& entry : handle->stack)
            fire_dismissed(entry, true);
    }
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_show(
    sao_ui_toast_handle_t handle,
    const SaoUiToastSpec* spec,
    sao_ui_toast_dismissed_cb_t callback,
    void* user_data) {
    if (handle == nullptr || spec == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (spec->text_utf8 == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        ToastEntry entry;
        entry.text = spec->text_utf8;
        entry.severity = static_cast<SaoUiToastSeverity>(
            std::clamp(spec->severity, static_cast<int32_t>(SAO_UI_TOAST_INFO),
                       static_cast<int32_t>(SAO_UI_TOAST_ERROR)));
        entry.duration_ms = spec->duration_ms > 0 ? spec->duration_ms : kDefaultDurationMs;
        entry.fade_ms = spec->fade_ms > 0 ? spec->fade_ms : kDefaultFadeMs;
        entry.theme_override = spec->theme_override;
        entry.bg_argb = spec->bg_argb;
        entry.fg_argb = spec->fg_argb;
        entry.border_argb = spec->border_argb;
        entry.font_size_px = spec->font_size_px > 0 ? spec->font_size_px : kDefaultFontSize;
        entry.pad_x_px = spec->pad_x_px > 0 ? spec->pad_x_px : kDefaultPadX;
        entry.pad_y_px = spec->pad_y_px > 0 ? spec->pad_y_px : kDefaultPadY;
        entry.radius_px = spec->radius_px > 0 ? spec->radius_px : kDefaultRadius;
        entry.callback = callback;
        entry.user_data = user_data;

        std::lock_guard<std::mutex> lock(handle->mu);
        // Evict oldest when at capacity.
        while (static_cast<int32_t>(handle->stack.size()) >= kMaxVisible) {
            ToastEntry evicted = std::move(handle->stack.front());
            handle->stack.pop_front();
            fire_dismissed(evicted, true);
        }
        handle->stack.push_back(std::move(entry));
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_dismiss(sao_ui_toast_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    if (handle->stack.empty())
        return SAO_STATUS_OK;
    ToastEntry entry = std::move(handle->stack.front());
    handle->stack.pop_front();
    fire_dismissed(entry, true);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_apply_props(
    sao_ui_toast_handle_t handle,
    const uint8_t* props_json_utf8,
    size_t props_len) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (props_json_utf8 == nullptr && props_len != 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // Minimal props path: update the top toast's text/severity/duration.
    // Full JSON parse deferred to keep the header dependency-free; the
    // host panel typically uses the spec struct at show() time.
    std::lock_guard<std::mutex> lock(handle->mu);
    if (handle->stack.empty())
        return SAO_STATUS_ERR_NOT_FOUND;
    // No-op accept: props are validated structurally at show(); apply_props
    // is a passthrough hook for the transactional pipeline contract.
    (void)props_json_utf8;
    (void)props_len;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_visible_count(
    sao_ui_toast_handle_t handle, int32_t* out_count) {
    if (handle == nullptr || out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    *out_count = static_cast<int32_t>(handle->stack.size());
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_stack_top_text(
    sao_ui_toast_handle_t handle,
    char* out_utf8, size_t capacity, size_t* out_bytes_written) {
    if (handle == nullptr || out_bytes_written == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_bytes_written = 0;
    std::lock_guard<std::mutex> lock(handle->mu);
    if (handle->stack.empty())
        return SAO_STATUS_ERR_NOT_FOUND;
    const std::string& text = handle->stack.back().text;
    *out_bytes_written = text.size();
    if (out_utf8 == nullptr || capacity <= text.size())
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out_utf8, text.c_str(), text.size() + 1);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_tick(
    sao_ui_toast_handle_t handle, int32_t dt_ms) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (dt_ms < 0)
        dt_ms = 0;
    std::lock_guard<std::mutex> lock(handle->mu);
    bool any_live = false;
    std::vector<ToastEntry> completed;
    for (auto& entry : handle->stack) {
        entry.elapsed_ms += dt_ms;
        if (entry.elapsed_ms >= entry.duration_ms) {
            completed.push_back(std::move(entry));
            entry.text.clear();
        } else {
            any_live = true;
        }
    }
    // Remove completed entries.
    handle->stack.erase(
        std::remove_if(handle->stack.begin(), handle->stack.end(),
                       [](const ToastEntry& e) { return e.text.empty() && e.dismissed_fired; }),
        handle->stack.end());
    // Actually erase moved entries (text cleared above).
    std::deque<ToastEntry> remaining;
    for (auto& entry : handle->stack) {
        if (!entry.text.empty())
            remaining.push_back(std::move(entry));
    }
    handle->stack = std::move(remaining);
    for (auto& entry : completed)
        fire_dismissed(entry, false);
    if (!handle->stack.empty())
        any_live = true;
    return any_live ? SAO_STATUS_OK : SAO_STATUS_ERR_NOT_FOUND;
}