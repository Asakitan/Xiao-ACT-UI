// SAO Auto — toast notification overlay implementation.

#include "sao/ui/widget_toast.h"

#include "panel_theme_internal.h"
#include "widget_paint_internal.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <cstring>
#include <deque>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct sao_ui_toast_s;

namespace {

using json = nlohmann::json;

constexpr int32_t kMaxVisible = 4;
constexpr int32_t kDefaultDurationMs = 3000;
constexpr int32_t kDefaultFadeMs = 240;
constexpr int32_t kDefaultFontSize = 14;
constexpr int32_t kDefaultPadX = 12;
constexpr int32_t kDefaultPadY = 8;
constexpr int32_t kDefaultRadius = 8;
constexpr int32_t kToastGap = 8;

enum class ToastPhase : uint8_t { entering, live, exiting };

SaoUiColorToken severity_token(SaoUiToastSeverity severity) {
    switch (severity) {
    case SAO_UI_TOAST_SUCCESS: return SAO_UI_TOKEN_APP_GREEN;
    case SAO_UI_TOAST_WARN:    return SAO_UI_TOKEN_APP_ORANGE;
    case SAO_UI_TOAST_ERROR:   return SAO_UI_TOKEN_APP_RED;
    case SAO_UI_TOAST_INFO:
    default:                   return SAO_UI_TOKEN_APP_BLUE;
    }
}

bool valid_theme_override(SaoUiThemeId theme_id) noexcept {
    const int32_t value = static_cast<int32_t>(theme_id);
    return value >= 0 && value <= SAO_UI_THEME_COUNT;
}

bool parse_argb(const json& value, uint32_t* out_argb) {
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
    const std::string text = value.get<std::string>();
    if ((text.size() != 7U && text.size() != 9U) || text.front() != '#')
        return false;
    uint32_t parsed = 0;
    for (size_t index = 1; index < text.size(); ++index) {
        const char ch = text[index];
        uint32_t nibble = 0;
        if (ch >= '0' && ch <= '9') nibble = static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') nibble = static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') nibble = static_cast<uint32_t>(ch - 'A' + 10);
        else return false;
        parsed = (parsed << 4U) | nibble;
    }
    *out_argb = text.size() == 7U ? 0xff000000U | parsed
                                  : ((parsed & 0xffU) << 24U) | (parsed >> 8U);
    return true;
}

bool parse_theme_override(const json& value, SaoUiThemeId* out_theme) {
    if (out_theme == nullptr)
        return false;
    if (value.is_number_integer()) {
        const int64_t raw = value.get<int64_t>();
        if (raw < 0 || raw > SAO_UI_THEME_COUNT)
            return false;
        *out_theme = static_cast<SaoUiThemeId>(raw);
        return true;
    }
    if (!value.is_string())
        return false;
    std::string name = value.get<std::string>();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (name == "inherit" || name == "global") *out_theme = SAO_UI_THEME_COUNT;
    else if (name == "dark") *out_theme = SAO_UI_THEME_DARK;
    else if (name == "light") *out_theme = SAO_UI_THEME_LIGHT;
    else if (name == "glass") *out_theme = SAO_UI_THEME_GLASS;
    else return false;
    return true;
}

struct ToastEntry {
    std::string text;
    SaoUiToastSeverity severity{SAO_UI_TOAST_INFO};
    int32_t duration_ms{kDefaultDurationMs};
    int32_t fade_ms{kDefaultFadeMs};
    int32_t elapsed_ms{0};
    int32_t phase_elapsed_ms{0};
    float alpha{0.0F};
    ToastPhase phase{ToastPhase::entering};
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

uint32_t theme_color(SaoUiThemeId theme_override, SaoUiColorToken token) {
    if (theme_override == SAO_UI_THEME_COUNT)
        return sao::ui::detail::panel_theme_color(token);
    const auto theme = sao::ui::detail::resolve_theme(
        theme_override, sao::ui::detail::process_theme_generation());
    return theme.colors[static_cast<size_t>(token)];
}

uint32_t resolve_or(const ToastEntry& entry, uint32_t override_value, SaoUiColorToken token) {
    return override_value != 0 && !sao::ui::detail::panel_theme_high_contrast()
               ? override_value
               : theme_color(entry.theme_override, token);
}

}  // namespace

struct sao_ui_toast_s {
    std::mutex mu;
    std::deque<ToastEntry> stack;
    int32_t overflow_count{0};
    size_t total_count{0};
    bool hovered{false};
    std::mutex lifecycle_mu;
    std::condition_variable lifecycle_cv;
    size_t operations_in_flight{0};
    size_t callbacks_in_flight{0};
    bool destroy_requested{false};
    bool finalizing{false};
    bool finalized{false};
};
struct ToastCallbackMarker {
    sao_ui_toast_s* handle{};
    ToastCallbackMarker* previous{};
};

thread_local ToastCallbackMarker* active_toast_callback = nullptr;

bool toast_callback_is_active(sao_ui_toast_s* handle) noexcept {
    for (ToastCallbackMarker* marker = active_toast_callback; marker != nullptr;
         marker = marker->previous) {
        if (marker->handle == handle)
            return true;
    }
    return false;
}

void finalize_toast_if_ready(sao_ui_toast_s* handle) noexcept {
    bool finalize = false;
    {
        std::lock_guard lock(handle->lifecycle_mu);
        if (handle->destroy_requested && !handle->finalizing && !handle->finalized &&
            handle->operations_in_flight == 0 && handle->callbacks_in_flight == 0) {
            handle->finalizing = true;
            finalize = true;
        }
    }
    if (!finalize)
        return;
    {
        std::lock_guard lock(handle->mu);
        handle->stack.clear();
        handle->overflow_count = 0;
        handle->total_count = 0;
    }
    {
        std::lock_guard lock(handle->lifecycle_mu);
        handle->finalized = true;
        handle->finalizing = false;
    }
    handle->lifecycle_cv.notify_all();
    delete handle;
}

class ToastOperation {
  public:
    explicit ToastOperation(sao_ui_toast_s* handle) : handle_(handle) {
        std::lock_guard lock(handle_->lifecycle_mu);
        if (handle_->destroy_requested || handle_->finalized)
            return;
        ++handle_->operations_in_flight;
        acquired_ = true;
    }
    ~ToastOperation() {
        if (!acquired_)
            return;
        {
            std::lock_guard lock(handle_->lifecycle_mu);
            --handle_->operations_in_flight;
        }
        handle_->lifecycle_cv.notify_all();
        finalize_toast_if_ready(handle_);
    }
    explicit operator bool() const noexcept { return acquired_; }
  private:
    sao_ui_toast_s* handle_{};
    bool acquired_{};
};

class ToastDestroyOperation {
  public:
    explicit ToastDestroyOperation(sao_ui_toast_s* handle) : handle_(handle) {
        if (toast_callback_is_active(handle_))
            return;
        std::unique_lock lock(handle_->lifecycle_mu);
        if (handle_->destroy_requested) {
            handle_->lifecycle_cv.wait(lock, [&] { return handle_->finalized; });
            return;
        }
        handle_->destroy_requested = true;
        ++handle_->operations_in_flight;
        acquired_ = true;
    }
    ~ToastDestroyOperation() {
        if (!acquired_)
            return;
        {
            std::lock_guard lock(handle_->lifecycle_mu);
            --handle_->operations_in_flight;
        }
        handle_->lifecycle_cv.notify_all();
        finalize_toast_if_ready(handle_);
    }
    explicit operator bool() const noexcept { return acquired_; }
  private:
    sao_ui_toast_s* handle_{};
    bool acquired_{};
};

class ToastCallbackLease {
  public:
    explicit ToastCallbackLease(sao_ui_toast_s* handle) : handle_(handle) {
        std::lock_guard lock(handle_->lifecycle_mu);
        if (handle_->finalized)
            return;
        ++handle_->callbacks_in_flight;
        acquired_ = true;
    }
    ~ToastCallbackLease() {
        if (!acquired_)
            return;
        {
            std::lock_guard lock(handle_->lifecycle_mu);
            --handle_->callbacks_in_flight;
        }
        handle_->lifecycle_cv.notify_all();
        finalize_toast_if_ready(handle_);
    }
    explicit operator bool() const noexcept { return acquired_; }
  private:
    sao_ui_toast_s* handle_{};
    bool acquired_{};
};

static void fire_dismissed_callback(
    sao_ui_toast_s* handle, ToastEntry& entry, bool cancelled) noexcept {
    if (entry.dismissed_fired)
        return;
    entry.dismissed_fired = true;
    if (entry.callback == nullptr)
        return;
    ToastCallbackLease lease(handle);
    if (!lease)
        return;
    ToastCallbackMarker marker{handle, active_toast_callback};
    active_toast_callback = &marker;
    try {
        entry.callback(cancelled, entry.user_data);
    } catch (...) {
    }
    active_toast_callback = marker.previous;
}

size_t toast_utf8_glyphs(const std::string& text) noexcept {
    size_t count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0U) != 0x80U)
            ++count;
    }
    return count;
}

std::string toast_ellipsize(const std::string& text, int32_t width_px, int32_t font_size_px) {
    const size_t max_glyphs = std::max<size_t>(
        1, static_cast<size_t>(std::max(1, width_px) /
                               std::max(5, static_cast<int32_t>(font_size_px * 0.56F))));
    if (toast_utf8_glyphs(text) <= max_glyphs)
        return text;
    std::string result;
    size_t glyphs = 0;
    for (size_t index = 0; index < text.size() && glyphs + 1 < max_glyphs; ++index) {
        result.push_back(text[index]);
        if ((static_cast<unsigned char>(text[index]) & 0xC0U) != 0x80U)
            ++glyphs;
    }
    return result + "…";
}
void recompute_toast_overflow_locked(sao_ui_toast_s* handle) noexcept {
    if (handle->stack.empty()) {
        handle->total_count = 0;
        handle->overflow_count = 0;
        return;
    }
    const size_t hidden = handle->total_count > handle->stack.size()
        ? handle->total_count - handle->stack.size() : 0;
    handle->overflow_count = static_cast<int32_t>(std::min(
        hidden, static_cast<size_t>(std::numeric_limits<int32_t>::max())));
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_create(
    sao_ui_theme_handle_t, sao_ui_toast_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        *out_handle = new sao_ui_toast_s();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_toast_destroy(sao_ui_toast_handle_t handle) {
    if (handle == nullptr)
        return;
    ToastDestroyOperation operation(handle);
    if (!operation)
        return;
    std::deque<ToastEntry> dismissed;
    {
        std::lock_guard lock(handle->mu);
        dismissed.swap(handle->stack);
        handle->overflow_count = 0;
        handle->total_count = 0;
    }
    for (auto& entry : dismissed)
        fire_dismissed_callback(handle, entry, true);
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_show(
    sao_ui_toast_handle_t handle, const SaoUiToastSpec* spec,
    sao_ui_toast_dismissed_cb_t callback, void* user_data) {
    if (handle == nullptr || spec == nullptr || spec->text_utf8 == nullptr ||
        !valid_theme_override(spec->theme_override))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ToastOperation operation(handle);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
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
        std::vector<ToastEntry> evicted;
        {
            std::lock_guard lock(handle->mu);
            ++handle->total_count;
            while (static_cast<int32_t>(handle->stack.size()) >= kMaxVisible) {
                evicted.push_back(std::move(handle->stack.front()));
                handle->stack.pop_front();
            }
            handle->stack.push_back(std::move(entry));
            recompute_toast_overflow_locked(handle);
        }
        for (auto& old : evicted)
            fire_dismissed_callback(handle, old, true);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_dismiss(sao_ui_toast_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ToastOperation operation(handle);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    ToastEntry dismissed;
    bool has_dismissed = false;
    {
        std::lock_guard lock(handle->mu);
        if (!handle->stack.empty()) {
            dismissed = std::move(handle->stack.front());
            handle->stack.pop_front();
            if (handle->total_count > 0)
                --handle->total_count;
            has_dismissed = true;
        }
        recompute_toast_overflow_locked(handle);
    }
    if (has_dismissed)
        fire_dismissed_callback(handle, dismissed, true);
    return SAO_STATUS_OK;
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_click_at(
    sao_ui_toast_handle_t handle, int32_t x, int32_t y, int32_t width, int32_t height,
    bool* out_dismissed) {
    if (out_dismissed == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_dismissed = false;
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ToastOperation operation(handle);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    // Mirror paint geometry for the newest (top) entry: same item box,
    // same 10×10 × glyph centered at the right edge.
    bool hit = false;
    ToastEntry dismissed;
    {
        std::lock_guard lock(handle->mu);
        if (!handle->stack.empty()) {
            const int32_t item_width = std::max(1, std::min(std::max(1, width - 12), 360));
            const int32_t item_height = 42;
            const int32_t item_x = x + width - item_width - 6;
            const int32_t item_y = y + height - 8 - item_height;
            const int32_t cx = item_x + item_width - 13;
            const int32_t cy = item_y + 17;
            hit = x >= cx - 14 && x <= cx + 14 && y >= cy - 14 && y <= cy + 14;
            if (hit) {
                dismissed = std::move(handle->stack.back());
                handle->stack.pop_back();
                if (handle->total_count > 0)
                    --handle->total_count;
                recompute_toast_overflow_locked(handle);
            }
        }
    }
    if (hit)
        fire_dismissed_callback(handle, dismissed, true);
    *out_dismissed = hit;
    return SAO_STATUS_OK;
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_apply_props(
    sao_ui_toast_handle_t handle, const uint8_t* props_json_utf8, size_t props_len) {
    if (handle == nullptr || (props_json_utf8 == nullptr && props_len != 0))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ToastOperation operation(handle);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const json props = props_len == 0 ? json::object()
            : json::parse(props_json_utf8, props_json_utf8 + props_len, nullptr, false, false);
        if (props.is_discarded() || !props.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const char* const allowed[] = {"text", "severity", "duration_ms", "fade_ms", "theme_override",
            "bg", "fg", "border", "font_size", "pad_x", "pad_y", "radius"};
        for (auto it = props.begin(); it != props.end(); ++it) {
            if (std::find(std::begin(allowed), std::end(allowed), it.key()) == std::end(allowed))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard lock(handle->mu);
        if (handle->stack.empty())
            return SAO_STATUS_ERR_NOT_FOUND;
        ToastEntry candidate = handle->stack.back();
        if (const auto it = props.find("text"); it != props.end()) {
            if (!it->is_string()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            candidate.text = it->get<std::string>();
        }
        if (const auto it = props.find("severity"); it != props.end()) {
            if (!it->is_number_integer()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            const int64_t value = it->get<int64_t>();
            if (value < SAO_UI_TOAST_INFO || value > SAO_UI_TOAST_ERROR)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            candidate.severity = static_cast<SaoUiToastSeverity>(value);
        }
        const auto parse_positive = [&](const char* key, int32_t* target) {
            const auto it = props.find(key);
            if (it == props.end()) return true;
            if (!it->is_number_integer()) return false;
            const int64_t value = it->get<int64_t>();
            if (value <= 0 || value > INT32_MAX) return false;
            *target = static_cast<int32_t>(value);
            return true;
        };
        if (!parse_positive("duration_ms", &candidate.duration_ms) ||
            !parse_positive("fade_ms", &candidate.fade_ms) ||
            !parse_positive("font_size", &candidate.font_size_px) ||
            !parse_positive("pad_x", &candidate.pad_x_px) ||
            !parse_positive("pad_y", &candidate.pad_y_px) ||
            !parse_positive("radius", &candidate.radius_px))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (const auto it = props.find("theme_override"); it != props.end() &&
            !parse_theme_override(*it, &candidate.theme_override))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (const auto it = props.find("bg"); it != props.end() && !parse_argb(*it, &candidate.bg_argb))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (const auto it = props.find("fg"); it != props.end() && !parse_argb(*it, &candidate.fg_argb))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (const auto it = props.find("border"); it != props.end() && !parse_argb(*it, &candidate.border_argb))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        handle->stack.back() = std::move(candidate);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_visible_count(
    sao_ui_toast_handle_t handle, int32_t* out_count) {
    if (handle == nullptr || out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ToastOperation operation(handle);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard lock(handle->mu);
    *out_count = static_cast<int32_t>(handle->stack.size());
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_stack_top_text(
    sao_ui_toast_handle_t handle, char* out_utf8, size_t capacity, size_t* out_bytes_written) {
    if (handle == nullptr || out_bytes_written == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ToastOperation operation(handle);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    *out_bytes_written = 0;
    std::lock_guard lock(handle->mu);
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
    ToastOperation operation(handle);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    const int32_t delta = std::max(0, dt_ms);
    std::vector<ToastEntry> completed;
    {
        std::lock_guard lock(handle->mu);
        for (auto& entry : handle->stack) {
            int32_t remaining = delta;
            if (entry.phase == ToastPhase::entering) {
                const int32_t needed = std::max(1, entry.fade_ms) - entry.phase_elapsed_ms;
                const int32_t step = std::min(remaining, std::max(0, needed));
                entry.phase_elapsed_ms += step;
                remaining -= step;
                entry.alpha = std::min(1.0F, static_cast<float>(entry.phase_elapsed_ms) /
                    static_cast<float>(std::max(1, entry.fade_ms)));
                if (entry.phase_elapsed_ms >= std::max(1, entry.fade_ms)) {
                    entry.phase = ToastPhase::live;
                    entry.phase_elapsed_ms = 0;
                    entry.alpha = 1.0F;
                }
            }
            if (entry.phase == ToastPhase::live && !handle->hovered) {
                const int64_t total = static_cast<int64_t>(entry.elapsed_ms) + remaining;
                if (total >= entry.duration_ms) {
                    entry.phase = ToastPhase::exiting;
                    entry.phase_elapsed_ms = 0;
                    entry.alpha = 1.0F;
                    remaining = static_cast<int32_t>(std::min<int64_t>(
                        total - entry.duration_ms, INT32_MAX));
                } else {
                    entry.elapsed_ms = static_cast<int32_t>(total);
                    remaining = 0;
                }
            }
            if (entry.phase == ToastPhase::exiting && !handle->hovered) {
                entry.phase_elapsed_ms = std::min(INT32_MAX,
                    entry.phase_elapsed_ms + remaining);
                entry.alpha = std::max(0.0F, 1.0F - static_cast<float>(entry.phase_elapsed_ms) /
                    static_cast<float>(std::max(1, entry.fade_ms)));
                if (entry.phase_elapsed_ms >= std::max(1, entry.fade_ms)) {
                    completed.push_back(std::move(entry));
                    entry.text.clear();
                }
            }
        }
        handle->stack.erase(std::remove_if(handle->stack.begin(), handle->stack.end(),
            [](const ToastEntry& entry) { return entry.text.empty() && entry.dismissed_fired; }),
            handle->stack.end());
        std::deque<ToastEntry> remaining;
        for (auto& entry : handle->stack) {
            if (!entry.text.empty()) remaining.push_back(std::move(entry));
        }
        handle->stack = std::move(remaining);
        if (completed.size() >= handle->total_count)
            handle->total_count = 0;
        else
            handle->total_count -= completed.size();
        recompute_toast_overflow_locked(handle);
    }
    for (auto& entry : completed)
        fire_dismissed_callback(handle, entry, false);
    std::lock_guard lock(handle->mu);
    return handle->stack.empty() ? SAO_STATUS_ERR_NOT_FOUND : SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_set_hovered(
    sao_ui_toast_handle_t handle, bool hovered) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ToastOperation operation(handle);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard lock(handle->mu);
    handle->hovered = hovered;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_toast_paint(
    sao_ui_toast_handle_t handle, sao_ui_paint_ctx_handle_t context,
    int32_t x, int32_t y, int32_t width, int32_t height) {
    if (handle == nullptr || context == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ToastOperation operation(handle);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::deque<ToastEntry> entries;
    int32_t overflow = 0;
    {
        std::lock_guard lock(handle->mu);
        entries = handle->stack;
        overflow = handle->overflow_count;
    }
    int32_t cursor_y = y + height - 8;
    for (size_t reverse = 0; reverse < entries.size(); ++reverse) {
        const ToastEntry& entry = entries[entries.size() - 1 - reverse];
        const int32_t item_width = std::max(1, std::min(std::max(1, width - 12), 360));
        const int32_t item_height = 42;
        const int32_t item_x = x + width - item_width - 6;
        cursor_y -= item_height;
        const uint32_t rail = resolve_or(entry, entry.border_argb, severity_token(entry.severity));
        const uint32_t surface = resolve_or(entry, entry.bg_argb, SAO_UI_TOKEN_APP_CARD);
        const uint32_t fg = resolve_or(entry, entry.fg_argb, SAO_UI_TOKEN_APP_TEXT);
        const std::string text = toast_ellipsize(entry.text, item_width - 60, entry.font_size_px);
        sao_status_t status = sao_ui_paint_ctx_push_opacity(context, entry.alpha);
        if (status == SAO_STATUS_OK) status = sao::ui::detail::paint_elevation_shadow(
            context, static_cast<float>(item_x), static_cast<float>(cursor_y),
            static_cast<float>(item_width), static_cast<float>(item_height),
            static_cast<float>(entry.radius_px), 2, theme_color(entry.theme_override, SAO_UI_TOKEN_BLACK));
        if (status == SAO_STATUS_OK) status = sao::ui::detail::paint_rounded_rect(
            context, static_cast<float>(item_x), static_cast<float>(cursor_y),
            static_cast<float>(item_width), static_cast<float>(item_height),
            static_cast<float>(entry.radius_px), surface);
        if (status == SAO_STATUS_OK) status = sao_ui_paint_ctx_fill_rect(
            context, static_cast<float>(item_x), static_cast<float>(cursor_y), 4.0F,
            static_cast<float>(item_height), rail);
        if (status == SAO_STATUS_OK) status = sao_ui_paint_ctx_draw_utf8(
            context, static_cast<float>(item_x + 14), static_cast<float>(cursor_y + 10),
            entry.severity == SAO_UI_TOAST_ERROR ? "!" : entry.severity == SAO_UI_TOAST_SUCCESS ? "✓" : "i",
            13.0F, rail);
        if (status == SAO_STATUS_OK) status = sao_ui_paint_ctx_draw_utf8(
            context, static_cast<float>(item_x + 36), static_cast<float>(cursor_y + 9),
            text.c_str(), static_cast<float>(entry.font_size_px), fg);
        if (status == SAO_STATUS_OK) status = sao_ui_paint_ctx_stroke_line(
            context, static_cast<float>(item_x + item_width - 18), static_cast<float>(cursor_y + 12),
            static_cast<float>(item_x + item_width - 8), static_cast<float>(cursor_y + 22), 1.5F,
            theme_color(entry.theme_override, SAO_UI_TOKEN_CLOSE_RED));
        if (status == SAO_STATUS_OK) status = sao_ui_paint_ctx_stroke_line(
            context, static_cast<float>(item_x + item_width - 8), static_cast<float>(cursor_y + 12),
            static_cast<float>(item_x + item_width - 18), static_cast<float>(cursor_y + 22), 1.5F,
            theme_color(entry.theme_override, SAO_UI_TOKEN_CLOSE_RED));
        const sao_status_t pop_status = sao_ui_paint_ctx_pop_opacity(context);
        if (status == SAO_STATUS_OK) status = pop_status;
        if (status != SAO_STATUS_OK)
            return status;
        cursor_y -= kToastGap;
    }
    if (overflow > 0) {
        const std::string folded = "+" + std::to_string(overflow) + " more";
        return sao_ui_paint_ctx_draw_utf8(context, static_cast<float>(x + width - 84),
            static_cast<float>(y + height - 2), folded.c_str(), 10.0F,
            theme_color(SAO_UI_THEME_COUNT, SAO_UI_TOKEN_APP_TEXT_2));
    }
    return SAO_STATUS_OK;
}