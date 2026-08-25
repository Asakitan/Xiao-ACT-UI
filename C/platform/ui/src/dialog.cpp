// SAO Auto - modal dialog state machine, focus, and keyboard logic.
//
// 1:1 with sao_theme/dialogs.py:
//   * SAODialog._show orchestrates EXPANDING (500ms width lerp) then
//     _clip_reveal on title/message; _close_alert runs a SHRINKING
//     width lerp before destroy.
//   * Callback fires on click (OK/Cancel).  Non-blocking; no grab_set,
//     no wait_window (matches Python's overrideredirect+grab silent-
//     hang bug fix).
//
// This slice owns the state machine, button focus + keyboard nav, and
// the fire-once callback contract.  Rendering is done by later slices
// (d2d_widgets + compositor); the header-declared show() call flips
// the machine IDLE -> EXPANDING and captures the spec.

#include "sao/ui/dialog.h"
#include "sao/ui/animator.h"
#include "sao/ui/d2d_effects.h"
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/overlay_host.h"

#include "panel_theme_internal.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

// ── Canonical button text ────────────────────────────────────────
// Fallback for SaoUiDialogButtonSpec::label_utf8 == nullptr.  Matches
// the fixed captions in dialogs.py (the icon buttons carry no text,
// but downstream renderers still want a name for a11y / tooltip).
const char* canonical_label(SaoUiDialogButton kind) {
    switch (kind) {
    case SAO_UI_DIALOG_BTN_OK:      return "OK";
    case SAO_UI_DIALOG_BTN_CANCEL:  return "Cancel";
    case SAO_UI_DIALOG_BTN_YES:     return "Yes";
    case SAO_UI_DIALOG_BTN_NO:      return "No";
    case SAO_UI_DIALOG_BTN_CUSTOM:  return "";
    case SAO_UI_DIALOG_BTN_DISMISS: return "";
    default:                        return "";
    }
}

uint32_t canonical_color(SaoUiDialogButton kind) {
    switch (kind) {
    case SAO_UI_DIALOG_BTN_OK:
    case SAO_UI_DIALOG_BTN_YES:
        return sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_OK_BLUE);
    case SAO_UI_DIALOG_BTN_CANCEL:
    case SAO_UI_DIALOG_BTN_NO:
    case SAO_UI_DIALOG_BTN_DISMISS:
        return sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_CLOSE_RED);
    case SAO_UI_DIALOG_BTN_CUSTOM:
    default:
        return sao::ui::detail::panel_theme_color(
            sao::ui::detail::PanelSemanticColorToken::DialogNeutral);
    }
}

// Default button set per kind.  INFO/WARNING/ERROR default to OK-only;
// ASK/INPUT default to OK + CANCEL.
void default_buttons_for_kind(SaoUiDialogKind kind,
                              std::vector<SaoUiDialogButton>* out) {
    out->clear();
    switch (kind) {
    case SAO_UI_DIALOG_INFO:
    case SAO_UI_DIALOG_WARNING:
    case SAO_UI_DIALOG_ERROR:
        out->push_back(SAO_UI_DIALOG_BTN_OK);
        break;
    case SAO_UI_DIALOG_ASK:
    case SAO_UI_DIALOG_INPUT:
        out->push_back(SAO_UI_DIALOG_BTN_OK);
        out->push_back(SAO_UI_DIALOG_BTN_CANCEL);
        break;
    default:
        out->push_back(SAO_UI_DIALOG_BTN_OK);
        break;
    }
}

// One resolved button entry (owns its label so we can outlive the
// caller's transient stack buffers).
struct ResolvedButton {
    SaoUiDialogButton kind = SAO_UI_DIALOG_BTN_OK;
    std::string       label;      // canonical or spec-provided
    uint32_t          color = 0;  // canonical or spec-provided
};

// Python SAODialog defaults. The two reveal paths are deliberately kept
// separate: title starts 100ms after expansion, message starts at 600ms.
constexpr int32_t kDefaultDialogWidth = 375;
constexpr int32_t kDefaultDialogHeight = 240;
constexpr int32_t kInitialDialogWidth = 135;
constexpr int32_t kHeaderHeight = 68;
constexpr int32_t kFooterHeight = 83;
constexpr int32_t kSeparatorHeight = 1;
constexpr int32_t kDefaultContentHeight =
    kDefaultDialogHeight - kHeaderHeight - kFooterHeight - 2 * kSeparatorHeight;
constexpr int32_t kDefaultExpandMs = 500;
constexpr int32_t kTitleRevealDelayMs = 100;
constexpr int32_t kDefaultTitleRevealMs = 400;
constexpr int32_t kMessageRevealDelayMs = 600;
constexpr int32_t kDefaultMessageRevealMs = 350;
constexpr int32_t kDefaultShrinkMs = 350;
constexpr int32_t kDefaultMirrorZ = 2000;
constexpr size_t kMaxDialogTextBytes = 16u * 1024u;
constexpr size_t kMaxDialogTotalTextBytes = 64u * 1024u;
constexpr size_t kMaxDialogButtonCount = 16u;
constexpr size_t kMaxDialogButtonLabelBytes = 32u;
constexpr int32_t kMaxDialogDimension = 4096;
constexpr uint64_t kMaxDialogRasterPixels = 16ull * 1024ull * 1024ull;
constexpr float kDialogTextSizeTitle = 20.0F;
constexpr float kDialogTextSizeBody = 15.0F;
constexpr float kDialogTextSizeButton = 14.0F;
constexpr float kDialogTextSizeInput = 14.0F;
constexpr int32_t kMinDialogContentHeight = 25;
constexpr float kDialogButtonMinWidth = 72.0F;
constexpr float kDialogButtonMinHeight = 20.0F;
constexpr float kDialogButtonMaxHeight = 38.0F;
constexpr float kDialogButtonGap = 8.0F;
constexpr float kDialogButtonRowGap = 6.0F;
constexpr float kDialogButtonPadding = 8.0F;
constexpr float kDialogButtonHorizontalInset = 14.0F;

std::atomic_uint64_t g_dialog_effect_sequence{};
std::atomic_uint64_t g_dialog_content_sequence{};

}  // namespace

// ── Handle ────────────────────────────────────────────────────────
struct sao_ui_dialog_s {
    std::mutex mu;

    // Configured at create.  Never mutated after.
    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_theme_handle_t      theme      = nullptr;
    sao_ui_layer_handle_t      modal_effect_layer = nullptr;
    sao_ui_layer_handle_t      content_layer = nullptr;

    // Show-time state (all guarded by mu).
    bool                            has_spec        = false;
    SaoUiDialogKind                 kind            = SAO_UI_DIALOG_INFO;
    std::string                     title;
    std::string                     message;
    std::string                     input_prompt;
    std::string                     input_default;
    int32_t                         input_max_length = 0;
    std::vector<ResolvedButton>     buttons;
    int32_t                         focused_index    = -1;

    // Callback (fires exactly once at SHRINKING -> IDLE).
    sao_ui_dialog_result_callback_t callback  = nullptr;
    void*                           user_data = nullptr;

    // Effective Python-authority layout and animation timings.
    int32_t width = kDefaultDialogWidth;
    int32_t height = kDefaultDialogHeight;
    int32_t expand_ms = kDefaultExpandMs;
    int32_t title_reveal_ms = kDefaultTitleRevealMs;
    int32_t message_reveal_ms = kDefaultMessageRevealMs;
    int32_t shrink_ms = kDefaultShrinkMs;
    int32_t mirror_z = kDefaultMirrorZ;
    int32_t shrink_start_width = kDefaultDialogWidth;

    // State machine.
    SaoUiDialogState state = SAO_UI_DIALOG_STATE_IDLE;
    // Cumulative dt spent in the current state, in milliseconds.
    int32_t state_elapsed_ms = 0;
    // The button the eventual callback will report.  Set on the entry
    // to SHRINKING (either from a click, ENTER, ESC, or explicit
    // dismiss).
    SaoUiDialogButton pending_pressed  = SAO_UI_DIALOG_BTN_DISMISS;
    bool              pending_dismiss  = true;
    // Whether we have a pending result to fire when SHRINKING completes.
    bool              have_pending_fire = false;

    std::vector<uint8_t> rendered_bgra;
    uint32_t rendered_width = 0;
    uint32_t rendered_height = 0;
    uint32_t rendered_stride = 0;
    int32_t rendered_x = 0;
    int32_t rendered_y = 0;
    int32_t rendered_mirror_z = kDefaultMirrorZ;
    float rendered_alpha = 0.0F;
    bool rendered_visible = false;
    bool self_owned = false;
    std::chrono::steady_clock::time_point auto_tick_at{};

    // Convenience: focus the first non-DISMISS button (matches dialogs.py
    // grabbing focus on the OK icon after expand).
    void reset_default_focus() {
        focused_index = -1;
        for (size_t i = 0; i < buttons.size(); ++i) {
            if (buttons[i].kind != SAO_UI_DIALOG_BTN_DISMISS) {
                focused_index = static_cast<int32_t>(i);
                return;
            }
        }
    }
};

// ── Helpers ───────────────────────────────────────────────────────
namespace {

bool valid_utf8(const char* text, size_t length, size_t* out_codepoints) noexcept {
    if (text == nullptr)
        return length == 0;
    size_t codepoints = 0;
    for (size_t i = 0; i < length;) {
        const uint8_t lead = static_cast<uint8_t>(text[i]);
        size_t width = 0;
        uint32_t value = 0;
        if (lead <= 0x7fu) {
            width = 1;
            value = lead;
        } else if (lead >= 0xc2u && lead <= 0xdfu) {
            width = 2;
            value = lead & 0x1fu;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            width = 3;
            value = lead & 0x0fu;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            width = 4;
            value = lead & 0x07u;
        } else {
            return false;
        }
        if (i + width > length)
            return false;
        for (size_t continuation = 1; continuation < width; ++continuation) {
            const uint8_t byte = static_cast<uint8_t>(text[i + continuation]);
            if ((byte & 0xc0u) != 0x80u)
                return false;
            value = (value << 6u) | (byte & 0x3fu);
        }
        if ((width == 2 && value < 0x80u) ||
            (width == 3 && value < 0x800u) ||
            (width == 4 && value < 0x10000u) ||
            (value >= 0xd800u && value <= 0xdfffu) || value > 0x10ffffu)
            return false;
        ++codepoints;
        i += width;
    }
    if (out_codepoints != nullptr)
        *out_codepoints = codepoints;
    return true;
}

bool copy_bounded_utf8(const char* input, size_t max_bytes, std::string* out,
                       size_t* out_codepoints = nullptr) {
    if (out == nullptr)
        return false;
    out->clear();
    if (input == nullptr)
        return true;
    size_t length = 0;
    while (length <= max_bytes && input[length] != '\0')
        ++length;
    if (length > max_bytes || !valid_utf8(input, length, out_codepoints))
        return false;
    try {
        out->assign(input, length);
        return true;
    } catch (...) {
        return false;
    }
}

size_t utf8_sequence_width(const std::string& text, size_t offset) noexcept {
    if (offset >= text.size())
        return 0;
    const uint8_t lead = static_cast<uint8_t>(text[offset]);
    if (lead <= 0x7fu)
        return 1;
    if (lead >= 0xc2u && lead <= 0xdfu)
        return 2;
    if (lead >= 0xe0u && lead <= 0xefu)
        return 3;
    if (lead >= 0xf0u && lead <= 0xf4u)
        return 4;
    return 1;
}

float utf8_glyph_width(const std::string& text, size_t offset, float font_size) noexcept {
    const size_t width = utf8_sequence_width(text, offset);
    if (width == 0)
        return 0.0F;
    const uint8_t lead = static_cast<uint8_t>(text[offset]);
    if (width == 1) {
        if (lead == ' ')
            return font_size * 0.35F;
        if (lead == '\t')
            return font_size * 1.5F;
        return font_size * 0.55F;
    }
    return font_size * (width == 2 ? 0.75F : 1.0F);
}

float utf8_text_width(const std::string& text, float font_size) noexcept {
    float width = 0.0F;
    for (size_t offset = 0; offset < text.size();) {
        width += utf8_glyph_width(text, offset, font_size);
        offset += std::max<size_t>(1, utf8_sequence_width(text, offset));
    }
    return width;
}

std::string clip_utf8_single_line(const std::string& text, float max_width,
                                  float font_size) {
    if (max_width <= 0.0F)
        return {};
    constexpr char kEllipsis[] = "\xE2\x80\xA6";
    const float ellipsis_width = font_size * 0.95F;
    std::string result;
    result.reserve(text.size());
    bool truncated = false;
    float used_width = 0.0F;
    for (size_t offset = 0; offset < text.size();) {
        const size_t sequence_width = std::max<size_t>(1, utf8_sequence_width(text, offset));
        const uint8_t lead = static_cast<uint8_t>(text[offset]);
        const float glyph_width = utf8_glyph_width(text, offset, font_size);
        if (used_width + glyph_width > max_width) {
            truncated = true;
            break;
        }
        if (lead == '\r' || lead == '\n')
            result.push_back(' ');
        else
            result.append(text, offset, sequence_width);
        used_width += glyph_width;
        offset += sequence_width;
    }
    if (!truncated)
        return result;
    while (!result.empty() && utf8_text_width(result, font_size) + ellipsis_width > max_width) {
        size_t last = result.size() - 1;
        while (last > 0 && (static_cast<uint8_t>(result[last]) & 0xc0u) == 0x80u)
            --last;
        result.erase(last);
    }
    if (ellipsis_width <= max_width)
        result.append(kEllipsis);
    return result;
}

std::vector<std::string> wrap_utf8_message(const std::string& text, float max_width,
                                           float font_size, size_t max_lines) {
    std::vector<std::string> lines;
    if (max_width <= 0.0F || max_lines == 0)
        return lines;
    std::string line;
    bool truncated = false;
    for (size_t offset = 0; offset < text.size();) {
        const size_t sequence_width = std::max<size_t>(1, utf8_sequence_width(text, offset));
        const uint8_t lead = static_cast<uint8_t>(text[offset]);
        if (lead == '\r' || lead == '\n') {
            lines.push_back(line);
            line.clear();
            if (lead == '\r' && offset + sequence_width < text.size() &&
                text[offset + sequence_width] == '\n') {
                ++offset;
            }
            offset += sequence_width;
            if (lines.size() >= max_lines) {
                truncated = offset < text.size();
                break;
            }
            continue;
        }
        const float glyph_width = utf8_glyph_width(text, offset, font_size);
        if (!line.empty() && utf8_text_width(line, font_size) + glyph_width > max_width) {
            lines.push_back(line);
            line.clear();
            if (lines.size() >= max_lines) {
                truncated = true;
                break;
            }
        }
        if (line.empty() && lead == ' ') {
            offset += sequence_width;
            continue;
        }
        line.append(text, offset, sequence_width);
        offset += sequence_width;
    }
    if (lines.size() < max_lines && (!line.empty() || lines.empty()))
        lines.push_back(line);
    if (truncated && !lines.empty())
        lines.back() = clip_utf8_single_line(lines.back() + "\xE2\x80\xA6", max_width,
                                             font_size);
    return lines;
}

uint32_t contrasting_button_text(uint32_t argb) noexcept {
    const auto linear = [](float value) noexcept {
        return value <= 0.03928F ? value / 12.92F :
                                    std::pow((value + 0.055F) / 1.055F, 2.4F);
    };
    const float red = linear(static_cast<float>((argb >> 16u) & 0xffu) / 255.0F);
    const float green = linear(static_cast<float>((argb >> 8u) & 0xffu) / 255.0F);
    const float blue = linear(static_cast<float>(argb & 0xffu) / 255.0F);
    const float luminance = 0.2126F * red + 0.7152F * green + 0.0722F * blue;
    const float white_contrast = 1.05F / (luminance + 0.05F);
    const float black_contrast = (luminance + 0.05F) / 0.05F;
    return white_contrast >= black_contrast ? 0xffffffffu : 0xff000000u;
}

bool valid_dialog_dimensions(int32_t width, int32_t height) noexcept {
    return width > 0 && height > 0 && width <= kMaxDialogDimension &&
           height <= kMaxDialogDimension &&
           static_cast<uint64_t>(width) * static_cast<uint64_t>(height) <=
               kMaxDialogRasterPixels;
}

int32_t dialog_maximum_footer_height(int32_t height) noexcept {
    return height - kHeaderHeight - 2 * kSeparatorHeight - kMinDialogContentHeight;
}

size_t dialog_maximum_rows_for_height(int32_t height) noexcept {
    const int32_t maximum_footer = dialog_maximum_footer_height(height);
    const float usable = static_cast<float>(maximum_footer) - 2.0F * kDialogButtonPadding;
    if (usable < kDialogButtonMinHeight)
        return 0;
    return static_cast<size_t>(std::floor(
        (usable + kDialogButtonRowGap) /
        (kDialogButtonMinHeight + kDialogButtonRowGap)));
}

size_t dialog_maximum_columns_for_width(int32_t width, size_t button_count) noexcept {
    if (button_count == 0)
        return 0;
    const float available = static_cast<float>(width) -
                            2.0F * kDialogButtonHorizontalInset;
    if (available < kDialogButtonMinWidth)
        return 0;
    const size_t columns = static_cast<size_t>(std::floor(
        (available + kDialogButtonGap) /
        (kDialogButtonMinWidth + kDialogButtonGap)));
    return std::min(button_count, columns);
}

int32_t dialog_minimum_width_for_height(size_t button_count, int32_t height) noexcept {
    if (button_count == 0)
        return 1;
    const size_t maximum_rows = dialog_maximum_rows_for_height(height);
    if (maximum_rows == 0)
        return kMaxDialogDimension + 1;
    const size_t columns = (button_count + maximum_rows - 1U) / maximum_rows;
    const float required = 2.0F * kDialogButtonHorizontalInset +
                           static_cast<float>(columns) * kDialogButtonMinWidth +
                           static_cast<float>(columns - 1U) * kDialogButtonGap;
    return static_cast<int32_t>(std::ceil(required));
}

bool dialog_dimensions_fit_controls(int32_t width, int32_t height,
                                    size_t button_count) noexcept {
    const int32_t maximum_footer = dialog_maximum_footer_height(height);
    if (maximum_footer < kFooterHeight)
        return false;
    if (button_count == 0)
        return true;
    const size_t columns = dialog_maximum_columns_for_width(width, button_count);
    if (columns == 0)
        return false;
    const size_t rows = (button_count + columns - 1U) / columns;
    const float required_footer = std::max(
        static_cast<float>(kFooterHeight),
        2.0F * kDialogButtonPadding +
            static_cast<float>(rows) * kDialogButtonMinHeight +
            static_cast<float>(rows - 1U) * kDialogButtonRowGap);
    return static_cast<float>(maximum_footer) >= required_footer;
}

sao_status_t resolve_buttons(const SaoUiDialogSpec& spec,
                             std::vector<ResolvedButton>* out) {
    if (out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    out->clear();
    if (spec.buttons != nullptr && spec.button_count > 0) {
        if (spec.button_count > kMaxDialogButtonCount)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        try {
            out->reserve(spec.button_count);
            for (size_t i = 0; i < spec.button_count; ++i) {
                const SaoUiDialogButtonSpec& s = spec.buttons[i];
                if (s.kind < SAO_UI_DIALOG_BTN_OK || s.kind > SAO_UI_DIALOG_BTN_DISMISS)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                ResolvedButton rb;
                rb.kind = s.kind;
                if (s.label_utf8 != nullptr && s.label_utf8[0] != '\0') {
                    if (!copy_bounded_utf8(s.label_utf8, kMaxDialogButtonLabelBytes,
                                           &rb.label))
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                } else {
                    rb.label = canonical_label(s.kind);
                }
                rb.color = s.color_argb != 0u ? s.color_argb : canonical_color(s.kind);
                out->push_back(std::move(rb));
            }
        } catch (...) {
            out->clear();
            return SAO_STATUS_ERR_UNKNOWN;
        }
        return SAO_STATUS_OK;
    }
    if (spec.button_count != 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::vector<SaoUiDialogButton> defaults;
    try {
        default_buttons_for_kind(spec.kind, &defaults);
        out->reserve(defaults.size());
        for (SaoUiDialogButton kind : defaults) {
            ResolvedButton rb;
            rb.kind = kind;
            rb.label = canonical_label(kind);
            rb.color = canonical_color(kind);
            out->push_back(std::move(rb));
        }
    } catch (...) {
        out->clear();
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return SAO_STATUS_OK;
}

// Snapshot for callback delivery outside the lock.
struct FireSnapshot {
    sao_ui_dialog_result_callback_t cb;
    SaoUiDialogButton pressed;
    void* user_data;
    // Optional input text (owned copy so callback stays valid past
    // any state mutation).
    std::string input_text;
    bool has_input = false;
};

// Fill in a fire snapshot from the current handle state.  Caller must
// hold the mutex.  Clears the have_pending_fire flag as a side-effect.
FireSnapshot take_pending_fire(sao_ui_dialog_s* d) {
    FireSnapshot snap;
    snap.cb = d->callback;
    snap.user_data = d->user_data;
    snap.pressed = d->pending_pressed;
    if (d->kind == SAO_UI_DIALOG_INPUT) {
        snap.has_input = true;
        snap.input_text = d->input_default;
    }
    d->have_pending_fire = false;
    d->callback = nullptr;
    d->user_data = nullptr;
    return snap;
}

struct DialogStateBackup {
    bool has_spec = false;
    SaoUiDialogKind kind = SAO_UI_DIALOG_INFO;
    std::string title;
    std::string message;
    std::string input_prompt;
    std::string input_default;
    int32_t input_max_length = 0;
    std::vector<ResolvedButton> buttons;
    int32_t focused_index = -1;
    sao_ui_dialog_result_callback_t callback = nullptr;
    void* user_data = nullptr;
    int32_t width = kDefaultDialogWidth;
    int32_t height = kDefaultDialogHeight;
    int32_t expand_ms = kDefaultExpandMs;
    int32_t title_reveal_ms = kDefaultTitleRevealMs;
    int32_t message_reveal_ms = kDefaultMessageRevealMs;
    int32_t shrink_ms = kDefaultShrinkMs;
    int32_t mirror_z = kDefaultMirrorZ;
    int32_t shrink_start_width = kDefaultDialogWidth;
    SaoUiDialogState state = SAO_UI_DIALOG_STATE_IDLE;
    int32_t state_elapsed_ms = 0;
    SaoUiDialogButton pending_pressed = SAO_UI_DIALOG_BTN_DISMISS;
    bool pending_dismiss = true;
    bool have_pending_fire = false;
};

bool save_dialog_state_locked(const sao_ui_dialog_s& d, DialogStateBackup* b) {
    if (b == nullptr)
        return false;
    try {
        b->has_spec = d.has_spec;
        b->kind = d.kind;
        b->title = d.title;
        b->message = d.message;
        b->input_prompt = d.input_prompt;
        b->input_default = d.input_default;
        b->input_max_length = d.input_max_length;
        b->buttons = d.buttons;
        b->focused_index = d.focused_index;
        b->callback = d.callback;
        b->user_data = d.user_data;
        b->width = d.width;
        b->height = d.height;
        b->expand_ms = d.expand_ms;
        b->title_reveal_ms = d.title_reveal_ms;
        b->message_reveal_ms = d.message_reveal_ms;
        b->shrink_ms = d.shrink_ms;
        b->mirror_z = d.mirror_z;
        b->shrink_start_width = d.shrink_start_width;
        b->state = d.state;
        b->state_elapsed_ms = d.state_elapsed_ms;
        b->pending_pressed = d.pending_pressed;
        b->pending_dismiss = d.pending_dismiss;
        b->have_pending_fire = d.have_pending_fire;
        return true;
    } catch (...) {
        return false;
    }
}

void restore_dialog_state_locked(sao_ui_dialog_s* d, DialogStateBackup* b) noexcept {
    d->has_spec = b->has_spec;
    d->kind = b->kind;
    d->title = std::move(b->title);
    d->message = std::move(b->message);
    d->input_prompt = std::move(b->input_prompt);
    d->input_default = std::move(b->input_default);
    d->input_max_length = b->input_max_length;
    d->buttons = std::move(b->buttons);
    d->focused_index = b->focused_index;
    d->callback = b->callback;
    d->user_data = b->user_data;
    d->width = b->width;
    d->height = b->height;
    d->expand_ms = b->expand_ms;
    d->title_reveal_ms = b->title_reveal_ms;
    d->message_reveal_ms = b->message_reveal_ms;
    d->shrink_ms = b->shrink_ms;
    d->mirror_z = b->mirror_z;
    d->shrink_start_width = b->shrink_start_width;
    d->state = b->state;
    d->state_elapsed_ms = b->state_elapsed_ms;
    d->pending_pressed = b->pending_pressed;
    d->pending_dismiss = b->pending_dismiss;
    d->have_pending_fire = b->have_pending_fire;
}

int32_t dialog_effect_z(int32_t z) noexcept {
    return z == std::numeric_limits<int32_t>::min() ? z : z - 1;
}

void SAO_UI_CALL dialog_layer_button(int32_t button, int32_t action,
                                     int32_t mods, float x, float y,
                                     void* user_data);
void SAO_UI_CALL dialog_auto_tick(void* gl_context, float time_seconds,
                                  void* user_data);

sao_status_t ensure_dialog_layers_locked(sao_ui_dialog_s* d, bool* created_modal,
                                         bool* created_content) {
    if (created_modal != nullptr)
        *created_modal = false;
    if (created_content != nullptr)
        *created_content = false;
    if (d->compositor == nullptr)
        return SAO_STATUS_OK;
    if (d->modal_effect_layer == nullptr) {
        const std::string name = "dialog.modal." + std::to_string(
            g_dialog_effect_sequence.fetch_add(1u, std::memory_order_relaxed) + 1u);
        SaoLayerConfig config{};
        config.struct_size = sizeof(SaoLayerConfig);
        config.name_utf8 = name.c_str();
        config.width = 1;
        config.height = 1;
        config.z_order = dialog_effect_z(d->mirror_z);
        config.click_through = true;
        config.bgra_swizzle = true;
        sao_ui_layer_handle_t layer = nullptr;
        sao_status_t status = sao_ui_layer_create(d->compositor, &config, &layer);
        SaoUiLayerEffects effects{};
        if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_effects_init(SAO_UI_LAYER_EFFECT_PRESET_MODAL, &effects);
            effects.flags &= ~SAO_UI_LAYER_EFFECT_SHADOW;
        }
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_effects(layer, &effects);
        const uint8_t clear_pixel[4]{};
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_update_bgra(layer, clear_pixel, 1u, 1u, 4u);
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_visible(layer, false);
        if (status != SAO_STATUS_OK) {
            sao_ui_layer_destroy(layer);
            return status;
        }
        d->modal_effect_layer = layer;
        if (created_modal != nullptr)
            *created_modal = true;
    }
    if (d->content_layer == nullptr) {
        const std::string name = "dialog.content." + std::to_string(
            g_dialog_content_sequence.fetch_add(1u, std::memory_order_relaxed) + 1u);
        SaoLayerConfig config{};
        config.struct_size = sizeof(SaoLayerConfig);
        config.name_utf8 = name.c_str();
        config.width = 1;
        config.height = 1;
        config.z_order = d->mirror_z;
        config.click_through = false;
        config.rect_hit = true;
        config.bgra_swizzle = true;
        sao_ui_layer_handle_t layer = nullptr;
        sao_status_t status = sao_ui_layer_create(d->compositor, &config, &layer);
        if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_set_input_callbacks(
                layer, nullptr, nullptr, &dialog_layer_button, nullptr, d);
        }
        if (status != SAO_STATUS_OK) {
            sao_ui_layer_destroy(layer);
            return status;
        }
        if (sao_ui_layer_set_visible(layer, false) != SAO_STATUS_OK) {
            sao_ui_layer_destroy(layer);
            return SAO_STATUS_ERR_UNKNOWN;
        }
        d->content_layer = layer;
        if (created_content != nullptr)
            *created_content = true;
    }
    return SAO_STATUS_OK;
}


struct DialogButtonRect {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct DialogFooterGeometry {
    int32_t header_height = 0;
    int32_t separator_height = 0;
    int32_t footer_y = 0;
    int32_t footer_height = 0;
    int32_t content_height = 0;
    size_t columns = 0;
    size_t rows = 0;
    size_t button_count = 0;
    float button_gap = 0.0F;
    float row_gap = 0.0F;
    float button_height = 0.0F;
    std::array<DialogButtonRect, kMaxDialogButtonCount> buttons{};
};

DialogFooterGeometry make_footer_geometry(const sao_ui_dialog_s& d,
                                          int32_t width, int32_t height) {
    DialogFooterGeometry geometry;
    const float safe_width = static_cast<float>(std::max(0, width));
    const int32_t safe_height = std::max(0, height);
    geometry.header_height = std::min(kHeaderHeight, safe_height);
    geometry.separator_height = std::min(
        kSeparatorHeight, std::max(0, safe_height - geometry.header_height));
    const int32_t body_and_footer_height = std::max(
        0, safe_height - geometry.header_height - 2 * geometry.separator_height);
    const int32_t minimum_content = std::min(kMinDialogContentHeight,
                                              body_and_footer_height);
    const int32_t maximum_footer = body_and_footer_height - minimum_content;
    geometry.button_count = std::min(d.buttons.size(), kMaxDialogButtonCount);
    const float inset = std::min(kDialogButtonHorizontalInset, safe_width * 0.5F);
    const float available_width = std::max(0.0F, safe_width - 2.0F * inset);

    if (geometry.button_count == 0) {
        geometry.footer_height = std::min(kFooterHeight, maximum_footer);
        geometry.footer_y = safe_height - geometry.footer_height;
        geometry.content_height = std::max(
            0, geometry.footer_y - geometry.header_height - 2 * geometry.separator_height);
        return geometry;
    }

    size_t columns = 1;
    if (available_width >= kDialogButtonMinWidth) {
        columns = std::max<size_t>(
            1, static_cast<size_t>((available_width + kDialogButtonGap) /
                                    (kDialogButtonMinWidth + kDialogButtonGap)));
        columns = std::min(columns, geometry.button_count);
    }
    size_t maximum_rows = 1;
    if (maximum_footer > static_cast<int32_t>(2.0F * kDialogButtonPadding)) {
        const float available_footer = static_cast<float>(maximum_footer) -
                                        2.0F * kDialogButtonPadding;
        maximum_rows = std::max<size_t>(
            1, static_cast<size_t>((available_footer + kDialogButtonRowGap) /
                                   (kDialogButtonMinHeight + kDialogButtonRowGap)));
    }
    columns = std::max(columns, (geometry.button_count + maximum_rows - 1) / maximum_rows);
    columns = std::min(columns, geometry.button_count);
    geometry.columns = columns;
    geometry.rows = (geometry.button_count + columns - 1) / columns;

    const int32_t compact_height = static_cast<int32_t>(
        geometry.rows <= 1 ? kDialogButtonMaxHeight :
                             std::max(kDialogButtonMinHeight,
                                      kDialogButtonMaxHeight -
                                          4.0F * static_cast<float>(geometry.rows - 1)));
    const int32_t desired_footer = static_cast<int32_t>(std::max(
        static_cast<float>(kFooterHeight),
        2.0F * kDialogButtonPadding +
            static_cast<float>(geometry.rows) * static_cast<float>(compact_height) +
            static_cast<float>(geometry.rows > 0 ? geometry.rows - 1 : 0) *
                kDialogButtonRowGap));
    geometry.footer_height = std::clamp(desired_footer, 0, maximum_footer);
    geometry.footer_y = safe_height - geometry.footer_height;
    geometry.content_height = std::max(
        0, geometry.footer_y - geometry.header_height - 2 * geometry.separator_height);

    const float footer_height = static_cast<float>(geometry.footer_height);
    const float vertical_padding = std::min(kDialogButtonPadding, footer_height * 0.5F);
    geometry.row_gap = geometry.rows > 1
                           ? std::min(kDialogButtonRowGap,
                                      std::max(0.0F, (footer_height - 2.0F * vertical_padding) /
                                                       static_cast<float>(geometry.rows)))
                           : 0.0F;
    geometry.button_height = geometry.rows == 0
                                 ? 0.0F
                                 : std::max(0.0F,
                                            (footer_height - 2.0F * vertical_padding -
                                             geometry.row_gap * static_cast<float>(geometry.rows - 1)) /
                                                static_cast<float>(geometry.rows));
    geometry.button_gap = geometry.columns > 1
                              ? std::min(kDialogButtonGap,
                                         available_width /
                                             static_cast<float>(geometry.columns + 1))
                              : 0.0F;
    const float button_width = geometry.columns == 0
                                   ? 0.0F
                                   : std::max(0.0F,
                                              (available_width - geometry.button_gap *
                                                                      static_cast<float>(geometry.columns - 1)) /
                                                  static_cast<float>(geometry.columns));
    for (size_t index = 0; index < geometry.button_count; ++index) {
        const size_t row = index / geometry.columns;
        const size_t column = index % geometry.columns;
        geometry.buttons[index] = {
            inset + static_cast<float>(column) * (button_width + geometry.button_gap),
            static_cast<float>(geometry.footer_y) + vertical_padding +
                static_cast<float>(row) * (geometry.button_height + geometry.row_gap),
            button_width,
            geometry.button_height};
    }
    return geometry;
}

int32_t dialog_initial_width(const sao_ui_dialog_s& dialog) noexcept {
    const int32_t required = dialog_minimum_width_for_height(
        dialog.buttons.size(), dialog.height);
    return std::clamp(std::max(kInitialDialogWidth, required), 1, dialog.width);
}

SaoUiDialogLayoutSnapshot make_layout_snapshot_locked(const sao_ui_dialog_s& d,
                                                       SaoUiDialogState state,
                                                       int32_t elapsed) {
    int32_t width = d.width;
    const int32_t initial_width = dialog_initial_width(d);
    float alpha = 1.0F;
    if (state == SAO_UI_DIALOG_STATE_EXPANDING) {
        const float progress = d.expand_ms <= 0 ? 1.0F : std::clamp(
            static_cast<float>(elapsed) / static_cast<float>(d.expand_ms), 0.0F, 1.0F);
        width = initial_width + static_cast<int32_t>(
            static_cast<float>(d.width - initial_width) * progress);
    } else if (state == SAO_UI_DIALOG_STATE_SHRINKING) {
        const float progress = d.shrink_ms <= 0 ? 1.0F : std::clamp(
            static_cast<float>(elapsed) / static_cast<float>(d.shrink_ms), 0.0F, 1.0F);
        width = std::max(1, d.shrink_start_width - static_cast<int32_t>(
            static_cast<float>(d.shrink_start_width) * progress));
        alpha = 1.0F - progress;
    }
    const DialogFooterGeometry footer = make_footer_geometry(d, width, d.height);
    SaoUiDialogLayoutSnapshot snapshot{};
    snapshot.width = width;
    snapshot.height = d.height;
    snapshot.initial_width = initial_width;
    snapshot.header_height = footer.header_height;
    snapshot.content_height = footer.content_height;
    snapshot.footer_height = footer.footer_height;
    snapshot.separator_height = footer.separator_height;
    snapshot.expand_ms = d.expand_ms;
    snapshot.title_reveal_delay_ms = kTitleRevealDelayMs;
    snapshot.title_reveal_ms = d.title_reveal_ms;
    snapshot.message_reveal_delay_ms = kMessageRevealDelayMs;
    snapshot.message_reveal_ms = d.message_reveal_ms;
    snapshot.shrink_ms = d.shrink_ms;
    snapshot.mirror_z = d.mirror_z;
    snapshot.alpha = alpha;
    return snapshot;
}

float reveal_progress(const sao_ui_dialog_s& d, bool title) noexcept {
    const int32_t duration = title ? d.title_reveal_ms : d.message_reveal_ms;
    if (duration <= 0 || d.state == SAO_UI_DIALOG_STATE_SHRINKING)
        return 1.0F;
    const int32_t delay = title ? kTitleRevealDelayMs :
                                 std::max(0, kMessageRevealDelayMs - d.expand_ms);
    return std::clamp(static_cast<float>(d.state_elapsed_ms - delay) /
                          static_cast<float>(std::max(1, duration)),
                      0.0F, 1.0F);
}

uint32_t dialog_kind_accent(SaoUiDialogKind kind) noexcept {
    switch (kind) {
    case SAO_UI_DIALOG_WARNING:
        return sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ORANGE);
    case SAO_UI_DIALOG_ERROR:
        return sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_RED);
    default:
        return sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BLUE);
    }
}

sao_status_t rasterize_dialog_locked(const sao_ui_dialog_s& d,
                                     const SaoUiDialogLayoutSnapshot& layout,
                                     std::vector<uint8_t>* pixels) {
    if (pixels == nullptr || !valid_dialog_dimensions(layout.width, layout.height))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    SaoUiOffscreenRasterDesc desc{};
    desc.width_px = static_cast<uint32_t>(layout.width);
    desc.height_px = static_cast<uint32_t>(layout.height);
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_status_t status = sao_ui_offscreen_raster_create(&desc, &raster);
    if (status != SAO_STATUS_OK)
        return status;
    sao_ui_paint_ctx_handle_t ctx = nullptr;
    status = sao_ui_paint_ctx_create_offscreen(raster, &ctx);
    if (status != SAO_STATUS_OK) {
        sao_ui_offscreen_raster_destroy(raster);
        return status;
    }
    status = sao_ui_paint_ctx_begin_frame(ctx);
    const float width = static_cast<float>(layout.width);
    const float height = static_cast<float>(layout.height);
    const DialogFooterGeometry footer = make_footer_geometry(d, layout.width, layout.height);
    const float footer_y = static_cast<float>(footer.footer_y);
    const float left = std::min(14.0F, width * 0.5F);
    const float right = std::max(left, width - left);
    const float text_width = std::max(0.0F, right - left);
    const uint32_t panel = sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_ALERT_PANEL);
    const uint32_t border = sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BORDER);
    const uint32_t title = sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_ALERT_TITLE_FG);
    const uint32_t body = sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_ALERT_CONTENT_FG);
    const uint32_t accent = dialog_kind_accent(d.kind);
    const uint32_t focus = sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_FOCUS_RING);
    if (status == SAO_STATUS_OK)
        status = sao_ui_paint_ctx_fill_rounded_rect(ctx, 0.0F, 0.0F, width, height, 4.0F, panel);
    if (status == SAO_STATUS_OK)
        status = sao_ui_paint_ctx_stroke_line(ctx, std::min(1.0F, width),
                                              std::min(1.0F, height),
                                              std::max(std::min(1.0F, width), width - 1.0F),
                                              std::min(1.0F, height), 1.0F, border);
    if (status == SAO_STATUS_OK && width > 0.0F)
        status = sao_ui_paint_ctx_fill_rect(ctx, 0.0F, 0.0F, width, std::min(1.0F, height), accent);
    const std::string clipped_title = clip_utf8_single_line(
        d.title, text_width, kDialogTextSizeTitle);
    if (status == SAO_STATUS_OK && !clipped_title.empty() && text_width > 0.0F) {
        const float title_clip_width = text_width * reveal_progress(d, true);
        if (title_clip_width > 0.0F) {
            status = sao_ui_paint_ctx_push_clip(ctx, left, std::min(10.0F, height),
                                                title_clip_width,
                                                std::max(0.0F, std::min(32.0F, height - 10.0F)));
            if (status == SAO_STATUS_OK)
                status = sao_ui_paint_ctx_draw_utf8(ctx, left, std::min(12.0F, height),
                                                    clipped_title.c_str(),
                                                    kDialogTextSizeTitle, title);
            if (status == SAO_STATUS_OK)
                status = sao_ui_paint_ctx_pop_clip(ctx);
        }
    }
    const float header_y = std::min(static_cast<float>(footer.header_height), height);
    if (status == SAO_STATUS_OK && width > 0.0F)
        status = sao_ui_paint_ctx_stroke_line(ctx, left, header_y, right, header_y,
                                              1.0F, sao::ui::detail::panel_theme_color(
                                                  SAO_UI_TOKEN_ALERT_BG));
    const float body_y = std::min(height, header_y + 12.0F);
    const float content_bottom = std::max(body_y, footer_y - 8.0F);
    const float message_bottom = d.kind == SAO_UI_DIALOG_INPUT
                                     ? std::max(body_y, footer_y - 60.0F)
                                     : content_bottom;
    const float message_height = std::max(0.0F, message_bottom - body_y);
    const size_t max_message_lines = message_height < 1.0F
                                         ? 0
                                         : std::max<size_t>(
                                               1, static_cast<size_t>((message_height + 2.0F) / 20.0F));
    const std::vector<std::string> message_lines = wrap_utf8_message(
        d.message, text_width, kDialogTextSizeBody, max_message_lines);
    if (status == SAO_STATUS_OK && !message_lines.empty() && text_width > 0.0F) {
        const float message_clip_width = text_width * reveal_progress(d, false);
        if (message_clip_width > 0.0F) {
            status = sao_ui_paint_ctx_push_clip(ctx, left, body_y, message_clip_width,
                                                message_height);
            for (size_t index = 0; index < message_lines.size() && status == SAO_STATUS_OK; ++index) {
                status = sao_ui_paint_ctx_draw_utf8(
                    ctx, left, body_y + static_cast<float>(index) * 20.0F,
                    message_lines[index].c_str(), kDialogTextSizeBody, body);
            }
            if (status == SAO_STATUS_OK)
                status = sao_ui_paint_ctx_pop_clip(ctx);
        }
    }
    if (status == SAO_STATUS_OK && d.kind == SAO_UI_DIALOG_INPUT) {
        const float prompt_y = std::clamp(footer_y - 52.0F, body_y,
                                          std::max(body_y, footer_y - 32.0F));
        const float entry_y = prompt_y + 20.0F;
        const float entry_bottom = std::max(entry_y, footer_y - 8.0F);
        const float entry_height = std::min(28.0F, std::max(0.0F, entry_bottom - entry_y));
        const std::string clipped_prompt = clip_utf8_single_line(
            d.input_prompt, text_width, kDialogTextSizeInput);
        if (!clipped_prompt.empty() && text_width > 0.0F)
            status = sao_ui_paint_ctx_draw_utf8(ctx, left, prompt_y, clipped_prompt.c_str(),
                                                kDialogTextSizeInput, body);
        if (status == SAO_STATUS_OK && entry_height > 0.0F && text_width > 0.0F)
            status = sao_ui_paint_ctx_fill_rounded_rect(
                ctx, left, entry_y, text_width, entry_height, 2.0F,
                sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BG));
        if (status == SAO_STATUS_OK && entry_height > 0.0F && text_width > 0.0F) {
            const std::string clipped_default = clip_utf8_single_line(
                d.input_default, std::max(0.0F, text_width - 12.0F), kDialogTextSizeInput);
            if (!clipped_default.empty())
                status = sao_ui_paint_ctx_draw_utf8(
                    ctx, left + 6.0F, entry_y + std::min(6.0F, entry_height),
                    clipped_default.c_str(), kDialogTextSizeInput, title);
        }
    }
    if (status == SAO_STATUS_OK && width > 0.0F)
        status = sao_ui_paint_ctx_stroke_line(
            ctx, left, footer_y, right, footer_y, 1.0F,
            sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_ALERT_BG));
    if (status == SAO_STATUS_OK) {
        for (size_t index = 0; index < footer.button_count && status == SAO_STATUS_OK; ++index) {
            const DialogButtonRect& rect = footer.buttons[index];
            if (rect.width <= 0.0F || rect.height <= 0.0F)
                continue;
            const ResolvedButton& button = d.buttons[index];
            const bool high_contrast = sao::ui::detail::panel_theme_high_contrast();
            const uint32_t button_fill = high_contrast
                                   ? sao::ui::detail::panel_theme_color(
                                       SAO_UI_TOKEN_SELECTION)
                                   : button.color;
            const uint32_t button_text = high_contrast
                                   ? sao::ui::detail::panel_theme_color(
                                       SAO_UI_TOKEN_WHITE)
                                   : contrasting_button_text(button_fill);
            status = sao_ui_paint_ctx_fill_rounded_rect(ctx, rect.x, rect.y,
                                                        rect.width, rect.height, 2.0F,
                                          button_fill);
            const std::string clipped_label = clip_utf8_single_line(
                button.label, std::max(0.0F, rect.width - 16.0F), kDialogTextSizeButton);
            if (status == SAO_STATUS_OK && !clipped_label.empty())
                status = sao_ui_paint_ctx_draw_utf8(
                    ctx, rect.x + std::min(8.0F, rect.width * 0.5F),
                    rect.y + std::min(9.0F, rect.height * 0.5F), clipped_label.c_str(),
                    kDialogTextSizeButton, button_text);
            if (status == SAO_STATUS_OK && static_cast<int32_t>(index) == d.focused_index) {
                const float dialog_right = width;
                const float dialog_bottom = height;
                const float ring_left = std::clamp(rect.x - 2.0F, 0.0F, dialog_right);
                const float ring_top = std::clamp(rect.y - 2.0F, 0.0F, dialog_bottom);
                const float ring_right = std::clamp(rect.x + rect.width + 2.0F, 0.0F, dialog_right);
                const float ring_bottom = std::clamp(rect.y + rect.height + 2.0F, 0.0F, dialog_bottom);
                status = sao_ui_paint_ctx_stroke_line(ctx, ring_left, ring_top,
                                                      ring_right, ring_top, 1.0F, focus);
                if (status == SAO_STATUS_OK)
                    status = sao_ui_paint_ctx_stroke_line(ctx, ring_right, ring_top,
                                                          ring_right, ring_bottom, 1.0F, focus);
                if (status == SAO_STATUS_OK)
                    status = sao_ui_paint_ctx_stroke_line(ctx, ring_right, ring_bottom,
                                                          ring_left, ring_bottom, 1.0F, focus);
                if (status == SAO_STATUS_OK)
                    status = sao_ui_paint_ctx_stroke_line(ctx, ring_left, ring_bottom,
                                                          ring_left, ring_top, 1.0F, focus);
            }
        }
    }
    if (status == SAO_STATUS_OK)
        status = sao_ui_paint_ctx_end_frame(ctx);
    else
        (void)sao_ui_paint_ctx_end_frame(ctx);
    if (status == SAO_STATUS_OK) {
        size_t bytes = 0;
        uint32_t snapshot_width = 0;
        uint32_t snapshot_height = 0;
        uint32_t stride = 0;
        status = sao_ui_offscreen_raster_snapshot(raster, nullptr, 0, &bytes,
                                                   &snapshot_width, &snapshot_height, &stride);
        if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
            try {
                pixels->resize(bytes);
            } catch (...) {
                status = SAO_STATUS_ERR_UNKNOWN;
            }
            if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL)
                status = sao_ui_offscreen_raster_snapshot(raster, pixels->data(), pixels->size(),
                                                           &bytes, &snapshot_width,
                                                           &snapshot_height, &stride);
        }
    }
    sao_ui_paint_ctx_destroy(ctx);
    sao_ui_offscreen_raster_destroy(raster);
    return status;
}

void dialog_origin_locked(const sao_ui_dialog_s& d, int32_t width, int32_t height,
                          int32_t* out_x, int32_t* out_y) {
    int32_t host_width = 0;
    int32_t host_height = 0;
    if (d.compositor != nullptr) {
        sao_ui_overlay_host_handle_t host = sao_ui_compositor_host(d.compositor);
        SaoOverlayHostClientRect bounds{};
        if (host != nullptr && sao_ui_overlay_host_get_desired_bounds(host, &bounds) == SAO_STATUS_OK) {
            host_width = bounds.width;
            host_height = bounds.height;
        }
    }
    *out_x = host_width > 0 ? std::max(0, (host_width - width) / 2) : 0;
    *out_y = host_height > 0 ? std::max(0, (host_height - height) / 2) : 0;
}

sao_status_t apply_dialog_visual_locked(sao_ui_dialog_s* d,
                                        const SaoUiDialogLayoutSnapshot& layout,
                                        bool visible) {
    if (d->compositor == nullptr)
        return SAO_STATUS_OK;
    std::vector<uint8_t> pixels;
    sao_status_t status = rasterize_dialog_locked(*d, layout, &pixels);
    if (status != SAO_STATUS_OK)
        return status;
    int32_t x = 0;
    int32_t y = 0;
    dialog_origin_locked(*d, layout.width, layout.height, &x, &y);
    const bool had_previous = !d->rendered_bgra.empty();
    const int32_t old_x = d->rendered_x;
    const int32_t old_y = d->rendered_y;
    const int32_t old_z = d->rendered_mirror_z;
    const float old_alpha = d->rendered_alpha;
    const bool old_visible = d->rendered_visible;
    const int32_t z = layout.mirror_z;
    auto rollback = [&]() {
        (void)sao_ui_layer_set_z_order(d->modal_effect_layer, dialog_effect_z(old_z));
        (void)sao_ui_layer_set_z_order(d->content_layer, old_z);
        if (had_previous) {
            (void)sao_ui_layer_update_bgra(d->content_layer, d->rendered_bgra.data(),
                                           d->rendered_width, d->rendered_height,
                                           d->rendered_stride);
            (void)sao_ui_layer_set_position(d->content_layer, old_x, old_y);
        }
        (void)sao_ui_layer_set_alpha(d->content_layer, old_alpha);
        (void)sao_ui_layer_set_alpha(d->modal_effect_layer, old_alpha);
        (void)sao_ui_layer_set_visible(d->content_layer, old_visible);
        (void)sao_ui_layer_set_visible(d->modal_effect_layer, old_visible);
    };
    status = sao_ui_layer_set_z_order(d->modal_effect_layer, dialog_effect_z(z));
    if (status == SAO_STATUS_OK)
        status = sao_ui_layer_set_z_order(d->content_layer, z);
    if (status == SAO_STATUS_OK)
        status = sao_ui_layer_update_bgra(d->content_layer, pixels.data(),
                                          static_cast<uint32_t>(layout.width),
                                          static_cast<uint32_t>(layout.height),
                                          static_cast<uint32_t>(layout.width) * 4u);
    if (status == SAO_STATUS_OK)
        status = sao_ui_layer_set_position(d->content_layer, x, y);
    if (status == SAO_STATUS_OK)
        status = sao_ui_layer_set_alpha(d->content_layer, layout.alpha);
    if (status == SAO_STATUS_OK)
        status = sao_ui_layer_set_alpha(d->modal_effect_layer, layout.alpha);
    if (status == SAO_STATUS_OK)
        status = sao_ui_layer_set_visible(d->content_layer, visible);
    if (status == SAO_STATUS_OK)
        status = sao_ui_layer_set_visible(d->modal_effect_layer, visible);
    if (status != SAO_STATUS_OK) {
        rollback();
        return status;
    }
    d->rendered_bgra = std::move(pixels);
    d->rendered_width = static_cast<uint32_t>(layout.width);
    d->rendered_height = static_cast<uint32_t>(layout.height);
    d->rendered_stride = static_cast<uint32_t>(layout.width) * 4u;
    d->rendered_x = x;
    d->rendered_y = y;
    d->rendered_mirror_z = z;
    d->rendered_alpha = layout.alpha;
    d->rendered_visible = visible;
    return SAO_STATUS_OK;
}

sao_status_t begin_shrink_locked(sao_ui_dialog_s* d, SaoUiDialogButton pressed) {
    const SaoUiDialogState old_state = d->state;
    const int32_t old_elapsed = d->state_elapsed_ms;
    const int32_t old_start_width = d->shrink_start_width;
    const SaoUiDialogButton old_pressed = d->pending_pressed;
    const bool old_dismiss = d->pending_dismiss;
    const bool old_fire = d->have_pending_fire;
    d->pending_pressed = pressed;
    d->pending_dismiss = pressed == SAO_UI_DIALOG_BTN_DISMISS;
    d->have_pending_fire = true;
    if (d->state != SAO_UI_DIALOG_STATE_SHRINKING) {
        if (d->state == SAO_UI_DIALOG_STATE_EXPANDING && d->expand_ms > 0) {
            const float progress = std::clamp(static_cast<float>(d->state_elapsed_ms) /
                                                  static_cast<float>(d->expand_ms), 0.0F, 1.0F);
            const int32_t initial_width = dialog_initial_width(*d);
            d->shrink_start_width = initial_width + static_cast<int32_t>(
                static_cast<float>(d->width - initial_width) * progress);
        } else {
            d->shrink_start_width = d->width;
        }
        d->state = SAO_UI_DIALOG_STATE_SHRINKING;
        d->state_elapsed_ms = 0;
    }
    const sao_status_t status = apply_dialog_visual_locked(
        d, make_layout_snapshot_locked(*d, d->state, d->state_elapsed_ms), true);
    if (status != SAO_STATUS_OK) {
        d->state = old_state;
        d->state_elapsed_ms = old_elapsed;
        d->shrink_start_width = old_start_width;
        d->pending_pressed = old_pressed;
        d->pending_dismiss = old_dismiss;
        d->have_pending_fire = old_fire;
    }
    return status;
}

int32_t dialog_button_at_locked(const sao_ui_dialog_s& d, float x, float y) {
    const SaoUiDialogLayoutSnapshot layout = make_layout_snapshot_locked(
        d, d.state, d.state_elapsed_ms);
    const DialogFooterGeometry footer = make_footer_geometry(d, layout.width, layout.height);
    for (size_t index = 0; index < footer.button_count; ++index) {
        const DialogButtonRect& rect = footer.buttons[index];
        if (x >= rect.x && x < rect.x + rect.width && y >= rect.y &&
            y < rect.y + rect.height)
            return static_cast<int32_t>(index);
    }
    return -1;
}

void SAO_UI_CALL dialog_layer_button(int32_t button, int32_t action,
                                     int32_t mods, float x, float y,
                                     void* user_data) {
    (void)mods;
    if (button != 0 || action != 1 || user_data == nullptr)
        return;
    auto* dialog = static_cast<sao_ui_dialog_s*>(user_data);
    std::lock_guard lock(dialog->mu);
    if (!dialog->has_spec || dialog->state == SAO_UI_DIALOG_STATE_IDLE ||
        dialog->state == SAO_UI_DIALOG_STATE_SHRINKING) {
        return;
    }
    const int32_t index = dialog_button_at_locked(*dialog, x, y);
    if (index < 0 || static_cast<size_t>(index) >= dialog->buttons.size())
        return;
    dialog->focused_index = index;
    (void)begin_shrink_locked(
        dialog, dialog->buttons[static_cast<size_t>(index)].kind);
}

void SAO_UI_CALL dialog_auto_tick(void* gl_context, float time_seconds,
                                  void* user_data) {
    (void)gl_context;
    (void)time_seconds;
    if (user_data == nullptr)
        return;
    auto* dialog = static_cast<sao_ui_dialog_s*>(user_data);
    const auto now = std::chrono::steady_clock::now();
    int32_t elapsed_ms = 0;
    {
        std::lock_guard lock(dialog->mu);
        if (!dialog->self_owned)
            return;
        if (dialog->auto_tick_at.time_since_epoch().count() != 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - dialog->auto_tick_at).count();
            elapsed_ms = static_cast<int32_t>(std::clamp<int64_t>(elapsed, 0, 250));
        }
        dialog->auto_tick_at = now;
    }
    const sao_status_t tick_status = sao_ui_dialog_tick(dialog, elapsed_ms);
    if (tick_status != SAO_STATUS_OK &&
        tick_status != SAO_STATUS_ERR_NOT_INITIALIZED) {
        return;
    }
    SaoUiDialogState state = SAO_UI_DIALOG_STATE_IDLE;
    if (sao_ui_dialog_get_state(dialog, &state) == SAO_STATUS_OK &&
        state == SAO_UI_DIALOG_STATE_IDLE) {
        sao_ui_dialog_destroy(dialog);
    }
}

}  // namespace

// ── Lifecycle ─────────────────────────────────────────────────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_create(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    sao_ui_dialog_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    auto* d = new (std::nothrow) sao_ui_dialog_s;
    if (d == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    d->compositor = compositor;
    d->theme = theme;
    *out_handle = d;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_dialog_destroy(
    sao_ui_dialog_handle_t handle) {
    if (handle == nullptr) return;
    // If a result callback is still pending (caller destroyed mid-flight),
    // fire it now with DISMISS/cancelled so any user_data resources
    // downstream get released.
    FireSnapshot snap;
    bool fire = false;
    sao_ui_layer_handle_t modal_effect_layer = nullptr;
    sao_ui_layer_handle_t content_layer = nullptr;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        modal_effect_layer = handle->modal_effect_layer;
        content_layer = handle->content_layer;
        handle->modal_effect_layer = nullptr;
        handle->content_layer = nullptr;
        if (handle->callback != nullptr) {
            snap.cb = handle->callback;
            snap.user_data = handle->user_data;
            snap.pressed = SAO_UI_DIALOG_BTN_DISMISS;
            handle->callback = nullptr;
            handle->user_data = nullptr;
            fire = true;
        }
    }
    if (fire && snap.cb != nullptr) {
        snap.cb(snap.pressed, /*input_text_utf8=*/nullptr,
                /*input_text_len=*/0, snap.user_data);
    }
    if (content_layer != nullptr) {
        (void)sao_ui_layer_set_render_fn(content_layer, nullptr, nullptr);
        (void)sao_ui_layer_set_input_callbacks(
            content_layer, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    sao_ui_layer_destroy(content_layer);
    sao_ui_layer_destroy(modal_effect_layer);
    delete handle;
}

// ── show / hide (spec captures + state machine kick-off) ─────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_show(
    sao_ui_dialog_handle_t handle,
    const SaoUiDialogSpec* spec,
    sao_ui_dialog_result_callback_t callback,
    void* user_data) {
    if (handle == nullptr || spec == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (spec->kind < SAO_UI_DIALOG_INFO || spec->kind > SAO_UI_DIALOG_INPUT ||
        spec->input_max_length < 0 || spec->width < 0 || spec->height < 0 ||
        spec->expand_ms < 0 || spec->clip_reveal_ms_title < 0 ||
        spec->clip_reveal_ms_message < 0 || spec->shrink_ms < 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::string title;
    std::string message;
    std::string input_prompt;
    std::string input_default;
    size_t input_codepoints = 0;
    std::vector<ResolvedButton> buttons;
    if (!copy_bounded_utf8(spec->title_utf8, kMaxDialogTextBytes, &title) ||
        !copy_bounded_utf8(spec->message_utf8, kMaxDialogTextBytes, &message) ||
        !copy_bounded_utf8(spec->input_prompt_utf8, kMaxDialogTextBytes, &input_prompt) ||
        !copy_bounded_utf8(spec->input_default_utf8, kMaxDialogTextBytes, &input_default,
                           &input_codepoints))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (title.size() + message.size() + input_prompt.size() + input_default.size() >
        kMaxDialogTotalTextBytes)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (spec->input_max_length > 0 &&
        input_codepoints > static_cast<size_t>(spec->input_max_length))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const int32_t width = spec->width > 0 ? spec->width : kDefaultDialogWidth;
    const int32_t height = spec->height > 0 ? spec->height : kDefaultDialogHeight;
    if (!valid_dialog_dimensions(width, height))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_status_t status = resolve_buttons(*spec, &buttons);
    if (status != SAO_STATUS_OK)
        return status;
    if (!dialog_dimensions_fit_controls(width, height, buttons.size()))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const int32_t expand_ms = sao_ui_animation_duration_ms(
        spec->expand_ms > 0 ? spec->expand_ms : kDefaultExpandMs);
    const int32_t title_reveal_ms = sao_ui_animation_duration_ms(
        spec->clip_reveal_ms_title > 0 ? spec->clip_reveal_ms_title : kDefaultTitleRevealMs);
    const int32_t message_reveal_ms = sao_ui_animation_duration_ms(
        spec->clip_reveal_ms_message > 0 ? spec->clip_reveal_ms_message : kDefaultMessageRevealMs);
    const int32_t shrink_ms = sao_ui_animation_duration_ms(
        spec->shrink_ms > 0 ? spec->shrink_ms : kDefaultShrinkMs);

    std::lock_guard<std::mutex> lk(handle->mu);
    DialogStateBackup backup;
    if (!save_dialog_state_locked(*handle, &backup))
        return SAO_STATUS_ERR_UNKNOWN;
    handle->kind = spec->kind;
    handle->title = std::move(title);
    handle->message = std::move(message);
    handle->input_prompt = std::move(input_prompt);
    handle->input_default = std::move(input_default);
    handle->input_max_length = spec->input_max_length;
    handle->buttons = std::move(buttons);
    handle->width = width;
    handle->height = height;
    handle->expand_ms = expand_ms;
    handle->title_reveal_ms = title_reveal_ms;
    handle->message_reveal_ms = message_reveal_ms;
    handle->shrink_ms = shrink_ms;
    handle->mirror_z = spec->mirror_z != 0 ? spec->mirror_z : kDefaultMirrorZ;
    handle->shrink_start_width = width;
    handle->callback = callback;
    handle->user_data = user_data;
    handle->reset_default_focus();
    handle->has_spec = true;
    handle->state = SAO_UI_DIALOG_STATE_EXPANDING;
    handle->state_elapsed_ms = 0;
    handle->pending_pressed = SAO_UI_DIALOG_BTN_DISMISS;
    handle->pending_dismiss = true;
    handle->have_pending_fire = false;

    bool created_modal = false;
    bool created_content = false;
    status = ensure_dialog_layers_locked(handle, &created_modal, &created_content);
    if (status == SAO_STATUS_OK)
        status = apply_dialog_visual_locked(
            handle, make_layout_snapshot_locked(*handle, handle->state, 0), true);
    if (status != SAO_STATUS_OK) {
        restore_dialog_state_locked(handle, &backup);
        if (created_content) {
            sao_ui_layer_destroy(handle->content_layer);
            handle->content_layer = nullptr;
        }
        if (created_modal) {
            sao_ui_layer_destroy(handle->modal_effect_layer);
            handle->modal_effect_layer = nullptr;
        }
        return status;
    }
    return SAO_STATUS_OK;
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_hide(
    sao_ui_dialog_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return sao_ui_dialog_dismiss(handle, SAO_UI_DIALOG_BTN_DISMISS);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_is_visible(
    sao_ui_dialog_handle_t handle, bool* out_visible) {
    if (handle == nullptr || out_visible == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    *out_visible = (handle->state != SAO_UI_DIALOG_STATE_IDLE);
    return SAO_STATUS_OK;
}

// ── State machine (tick / dismiss / query) ────────────────────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_get_state(
    sao_ui_dialog_handle_t handle, SaoUiDialogState* out_state) {
    if (handle == nullptr || out_state == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    *out_state = handle->state;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_get_layout_snapshot(
    sao_ui_dialog_handle_t handle,
    SaoUiDialogLayoutSnapshot* out_snapshot) {
    if (handle == nullptr || out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    if (!handle->has_spec && handle->state == SAO_UI_DIALOG_STATE_IDLE)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    *out_snapshot = make_layout_snapshot_locked(*handle, handle->state,
                                                 handle->state_elapsed_ms);
    return SAO_STATUS_OK;
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_tick(
    sao_ui_dialog_handle_t handle, int32_t dt_ms) {
    if (handle == nullptr || dt_ms < 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    FireSnapshot snap;
    bool should_fire = false;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        if (handle->state == SAO_UI_DIALOG_STATE_IDLE)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        const SaoUiDialogState old_state = handle->state;
        const int32_t old_elapsed = handle->state_elapsed_ms;
        const int64_t next_elapsed = static_cast<int64_t>(handle->state_elapsed_ms) + dt_ms;
        handle->state_elapsed_ms = next_elapsed > std::numeric_limits<int32_t>::max()
                                      ? std::numeric_limits<int32_t>::max()
                                      : static_cast<int32_t>(next_elapsed);
        while (handle->state == SAO_UI_DIALOG_STATE_EXPANDING &&
               handle->state_elapsed_ms >= handle->expand_ms) {
            handle->state_elapsed_ms -= handle->expand_ms;
            handle->state = SAO_UI_DIALOG_STATE_CLIP_REVEALED;
            if (handle->expand_ms == 0)
                break;
        }
        bool closing = false;
        if (handle->state == SAO_UI_DIALOG_STATE_SHRINKING &&
            handle->state_elapsed_ms >= handle->shrink_ms) {
            closing = true;
            const sao_status_t visual_status = apply_dialog_visual_locked(
                handle, make_layout_snapshot_locked(*handle, handle->state, handle->shrink_ms), false);
            if (visual_status != SAO_STATUS_OK) {
                handle->state = old_state;
                handle->state_elapsed_ms = old_elapsed;
                return visual_status;
            }
            handle->state = SAO_UI_DIALOG_STATE_IDLE;
            handle->state_elapsed_ms = 0;
            if (handle->have_pending_fire) {
                snap = take_pending_fire(handle);
                should_fire = true;
            }
            handle->has_spec = false;
            handle->buttons.clear();
            handle->focused_index = -1;
        }
        if (!closing) {
            const sao_status_t visual_status = apply_dialog_visual_locked(
                handle, make_layout_snapshot_locked(*handle, handle->state,
                                                     handle->state_elapsed_ms), true);
            if (visual_status != SAO_STATUS_OK) {
                handle->state = old_state;
                handle->state_elapsed_ms = old_elapsed;
                return visual_status;
            }
        }
    }
    if (should_fire && snap.cb != nullptr) {
        const char* text = snap.has_input ? snap.input_text.c_str() : nullptr;
        const size_t text_len = snap.has_input ? snap.input_text.size() : 0;
        snap.cb(snap.pressed, text, text_len, snap.user_data);
    }
    return SAO_STATUS_OK;
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_dismiss(
    sao_ui_dialog_handle_t handle, SaoUiDialogButton pressed) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    if (handle->state == SAO_UI_DIALOG_STATE_IDLE && !handle->has_spec)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    return begin_shrink_locked(handle, pressed);
}

// ── Button focus + query ─────────────────────────────────────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_set_button_focus(
    sao_ui_dialog_handle_t handle, int32_t button_index) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    if (!handle->has_spec)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (button_index < -1 || (button_index >= 0 &&
        static_cast<size_t>(button_index) >= handle->buttons.size()))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const int32_t old_focus = handle->focused_index;
    handle->focused_index = button_index;
    const sao_status_t status = apply_dialog_visual_locked(
        handle, make_layout_snapshot_locked(*handle, handle->state,
                                             handle->state_elapsed_ms), true);
    if (status != SAO_STATUS_OK)
        handle->focused_index = old_focus;
    return status;
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_get_button_focus(
    sao_ui_dialog_handle_t handle, int32_t* out_index) {
    if (handle == nullptr || out_index == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    *out_index = handle->focused_index;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_get_button_count(
    sao_ui_dialog_handle_t handle, int32_t* out_count) {
    if (handle == nullptr || out_count == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    *out_count = static_cast<int32_t>(handle->buttons.size());
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_get_button_at(
    sao_ui_dialog_handle_t handle, int32_t index,
    SaoUiDialogButtonInfo* out_info) {
    if (handle == nullptr || out_info == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    if (index < 0 || static_cast<size_t>(index) >= handle->buttons.size()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const ResolvedButton& rb = handle->buttons[static_cast<size_t>(index)];
    out_info->kind = rb.kind;
    out_info->label_utf8 = rb.label.c_str();
    out_info->color_argb = rb.color;
    out_info->is_focused = (index == handle->focused_index);
    for (bool& p : out_info->_pad) p = false;
    return SAO_STATUS_OK;
}

// ── Keyboard dispatch ────────────────────────────────────────────
namespace {

// Advance focus by direction (+1 = next, -1 = prev).  Skips DISMISS
// buttons (they're not a real focusable target) but wraps around.
int32_t advance_focus(const std::vector<ResolvedButton>& buttons,
                      int32_t current, int32_t direction) {
    if (buttons.empty()) return -1;
    const int32_t n = static_cast<int32_t>(buttons.size());
    int32_t start = (current < 0) ? (direction > 0 ? -1 : n) : current;
    for (int32_t step = 0; step < n; ++step) {
        int32_t candidate = start + direction * (step + 1);
        candidate = ((candidate % n) + n) % n;
        if (buttons[static_cast<size_t>(candidate)].kind != SAO_UI_DIALOG_BTN_DISMISS) {
            return candidate;
        }
    }
    // All buttons are DISMISS - fall through to any of them.
    return (direction > 0) ? 0 : (n - 1);
}

constexpr uint32_t kVkTab    = 0x09u;
constexpr uint32_t kVkReturn = 0x0Du;
constexpr uint32_t kVkEscape = 0x1Bu;

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_dispatch_key(
    sao_ui_dialog_handle_t handle, uint32_t vk, bool shift_held) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    if (!handle->has_spec || handle->state == SAO_UI_DIALOG_STATE_IDLE ||
        handle->state == SAO_UI_DIALOG_STATE_SHRINKING)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    switch (vk) {
    case kVkTab: {
        const int32_t old_focus = handle->focused_index;
        const int32_t dir = shift_held ? -1 : 1;
        handle->focused_index = advance_focus(handle->buttons, handle->focused_index, dir);
        const sao_status_t status = apply_dialog_visual_locked(
            handle, make_layout_snapshot_locked(*handle, handle->state,
                                                 handle->state_elapsed_ms), true);
        if (status != SAO_STATUS_OK)
            handle->focused_index = old_focus;
        return status;
    }
    case kVkReturn:
        if (handle->focused_index < 0 ||
            static_cast<size_t>(handle->focused_index) >= handle->buttons.size())
            return begin_shrink_locked(handle, SAO_UI_DIALOG_BTN_DISMISS);
        return begin_shrink_locked(
            handle, handle->buttons[static_cast<size_t>(handle->focused_index)].kind);
    case kVkEscape:
        return begin_shrink_locked(handle, SAO_UI_DIALOG_BTN_DISMISS);
    default:
        return SAO_STATUS_OK;
    }
}


// ── Convenience one-shot helpers ─────────────────────────────────
namespace {

sao_status_t show_kind_helper(
    SaoUiDialogKind kind,
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    const char* title_utf8, const char* message_utf8,
    sao_ui_dialog_result_callback_t callback, void* user_data) {
    sao_ui_dialog_handle_t h = nullptr;
    sao_status_t rc = sao_ui_dialog_create(compositor, theme, &h);
    if (rc != SAO_STATUS_OK) return rc;
    SaoUiDialogSpec spec{};
    spec.kind = kind;
    spec.title_utf8 = title_utf8;
    spec.message_utf8 = message_utf8;
    spec.dismiss_on_esc = true;
    spec.draggable = true;
    rc = sao_ui_dialog_show(h, &spec, callback, user_data);
    if (rc != SAO_STATUS_OK) {
        sao_ui_dialog_destroy(h);
        return rc;
    }
    sao_ui_layer_handle_t content_layer = nullptr;
    {
        std::lock_guard lock(h->mu);
        h->self_owned = true;
        h->auto_tick_at = std::chrono::steady_clock::now();
        content_layer = h->content_layer;
    }
    if (content_layer == nullptr) {
        sao_ui_dialog_destroy(h);
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    rc = sao_ui_layer_set_render_fn(content_layer, &dialog_auto_tick, h);
    if (rc != SAO_STATUS_OK) {
        {
            std::lock_guard lock(h->mu);
            h->self_owned = false;
        }
        sao_ui_dialog_destroy(h);
        return rc;
    }
    return SAO_STATUS_OK;
}

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_show_info(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    const char* title_utf8, const char* message_utf8,
    sao_ui_dialog_result_callback_t callback, void* user_data) {
    return show_kind_helper(SAO_UI_DIALOG_INFO, compositor, theme,
                            title_utf8, message_utf8, callback, user_data);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_show_warning(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    const char* title_utf8, const char* message_utf8,
    sao_ui_dialog_result_callback_t callback, void* user_data) {
    return show_kind_helper(SAO_UI_DIALOG_WARNING, compositor, theme,
                            title_utf8, message_utf8, callback, user_data);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_show_error(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    const char* title_utf8, const char* message_utf8,
    sao_ui_dialog_result_callback_t callback, void* user_data) {
    return show_kind_helper(SAO_UI_DIALOG_ERROR, compositor, theme,
                            title_utf8, message_utf8, callback, user_data);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_show_ask(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    const char* title_utf8, const char* message_utf8,
    sao_ui_dialog_result_callback_t callback, void* user_data) {
    return show_kind_helper(SAO_UI_DIALOG_ASK, compositor, theme,
                            title_utf8, message_utf8, callback, user_data);
}
