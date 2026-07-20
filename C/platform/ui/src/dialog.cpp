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

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
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

// Default palette matches sao_theme/colors.py:
//   OK_BLUE  = #428ce6
//   CLOSE_RED = #d13d4f
uint32_t canonical_color(SaoUiDialogButton kind) {
    switch (kind) {
    case SAO_UI_DIALOG_BTN_OK:
    case SAO_UI_DIALOG_BTN_YES:
        return 0xFF428CE6u;
    case SAO_UI_DIALOG_BTN_CANCEL:
    case SAO_UI_DIALOG_BTN_NO:
    case SAO_UI_DIALOG_BTN_DISMISS:
        return 0xFFD13D4Fu;
    case SAO_UI_DIALOG_BTN_CUSTOM:
    default:
        return 0xFF808080u;
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

}  // namespace

// ── Handle ────────────────────────────────────────────────────────
struct sao_ui_dialog_s {
    std::mutex mu;

    // Configured at create.  Never mutated after.
    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_theme_handle_t      theme      = nullptr;

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

void resolve_buttons(const SaoUiDialogSpec& spec,
                     std::vector<ResolvedButton>* out) {
    out->clear();
    if (spec.buttons != nullptr && spec.button_count > 0) {
        out->reserve(spec.button_count);
        for (size_t i = 0; i < spec.button_count; ++i) {
            const SaoUiDialogButtonSpec& s = spec.buttons[i];
            ResolvedButton rb;
            rb.kind  = s.kind;
            rb.label = (s.label_utf8 != nullptr && s.label_utf8[0] != '\0')
                       ? s.label_utf8 : canonical_label(s.kind);
            rb.color = (s.color_argb != 0u)
                       ? s.color_argb : canonical_color(s.kind);
            out->push_back(std::move(rb));
        }
        return;
    }
    // No custom buttons - synthesize from kind default.
    std::vector<SaoUiDialogButton> defaults;
    default_buttons_for_kind(spec.kind, &defaults);
    out->reserve(defaults.size());
    for (SaoUiDialogButton bk : defaults) {
        ResolvedButton rb;
        rb.kind  = bk;
        rb.label = canonical_label(bk);
        rb.color = canonical_color(bk);
        out->push_back(std::move(rb));
    }
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
    {
        std::lock_guard<std::mutex> lk(handle->mu);
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
    delete handle;
}

// ── show / hide (spec captures + state machine kick-off) ─────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_show(
    sao_ui_dialog_handle_t handle,
    const SaoUiDialogSpec* spec,
    sao_ui_dialog_result_callback_t callback,
    void* user_data) {
    if (handle == nullptr || spec == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    // Capture spec fields into owned storage.
    handle->kind = spec->kind;
    handle->title = (spec->title_utf8 != nullptr) ? spec->title_utf8 : "";
    handle->message = (spec->message_utf8 != nullptr) ? spec->message_utf8 : "";
    handle->input_prompt = (spec->input_prompt_utf8 != nullptr)
        ? spec->input_prompt_utf8 : "";
    handle->input_default = (spec->input_default_utf8 != nullptr)
        ? spec->input_default_utf8 : "";
    handle->input_max_length = spec->input_max_length;
    resolve_buttons(*spec, &handle->buttons);
    handle->width = spec->width > 0 ? spec->width : kDefaultDialogWidth;
    handle->height = spec->height > 0 ? spec->height : kDefaultDialogHeight;
    handle->expand_ms = spec->expand_ms > 0 ? spec->expand_ms : kDefaultExpandMs;
    handle->title_reveal_ms = spec->clip_reveal_ms_title > 0
        ? spec->clip_reveal_ms_title : kDefaultTitleRevealMs;
    handle->message_reveal_ms = spec->clip_reveal_ms_message > 0
        ? spec->clip_reveal_ms_message : kDefaultMessageRevealMs;
    handle->shrink_ms = spec->shrink_ms > 0 ? spec->shrink_ms : kDefaultShrinkMs;
    handle->mirror_z = spec->mirror_z != 0 ? spec->mirror_z : kDefaultMirrorZ;
    handle->shrink_start_width = handle->width;

    handle->callback = callback;
    handle->user_data = user_data;
    handle->reset_default_focus();
    handle->has_spec = true;
    handle->state = SAO_UI_DIALOG_STATE_EXPANDING;
    handle->state_elapsed_ms = 0;
    handle->pending_pressed = SAO_UI_DIALOG_BTN_DISMISS;
    handle->pending_dismiss = true;
    handle->have_pending_fire = false;
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
    if (handle == nullptr || out_snapshot == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(handle->mu);
    if (!handle->has_spec && handle->state == SAO_UI_DIALOG_STATE_IDLE) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }

    const int32_t content_height = std::max(
        25, handle->height - kHeaderHeight - kFooterHeight - 2 * kSeparatorHeight);
    int32_t current_width = handle->width;
    float alpha = 1.0f;
    if (handle->state == SAO_UI_DIALOG_STATE_EXPANDING) {
        const float progress = handle->expand_ms <= 0 ? 1.0f : std::clamp(
            static_cast<float>(handle->state_elapsed_ms) /
                static_cast<float>(handle->expand_ms),
            0.0f, 1.0f);
        current_width = kInitialDialogWidth + static_cast<int32_t>(
            static_cast<float>(handle->width - kInitialDialogWidth) * progress);
    } else if (handle->state == SAO_UI_DIALOG_STATE_SHRINKING) {
        const float progress = handle->shrink_ms <= 0 ? 1.0f : std::clamp(
            static_cast<float>(handle->state_elapsed_ms) /
                static_cast<float>(handle->shrink_ms),
            0.0f, 1.0f);
        current_width = std::max(1, handle->shrink_start_width - static_cast<int32_t>(
            static_cast<float>(handle->shrink_start_width) * progress));
        alpha = 1.0f - progress;
    }

    *out_snapshot = {
        current_width,
        handle->height,
        kInitialDialogWidth,
        kHeaderHeight,
        content_height,
        kFooterHeight,
        kSeparatorHeight,
        handle->expand_ms,
        kTitleRevealDelayMs,
        handle->title_reveal_ms,
        kMessageRevealDelayMs,
        handle->message_reveal_ms,
        handle->shrink_ms,
        handle->mirror_z,
        alpha,
    };
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_tick(
    sao_ui_dialog_handle_t handle, int32_t dt_ms) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (dt_ms < 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    FireSnapshot snap;
    bool should_fire = false;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        if (handle->state == SAO_UI_DIALOG_STATE_IDLE) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        handle->state_elapsed_ms += dt_ms;
        while (true) {
            switch (handle->state) {
            case SAO_UI_DIALOG_STATE_EXPANDING: {
                if (handle->state_elapsed_ms >= handle->expand_ms) {
                    handle->state_elapsed_ms -= handle->expand_ms;
                    handle->state = SAO_UI_DIALOG_STATE_CLIP_REVEALED;
                    continue;   // consume any remainder into the next state
                }
                break;
            }
            case SAO_UI_DIALOG_STATE_CLIP_REVEALED: {
                // Hold indefinitely - only dismiss/dispatch_key/hide
                // moves us on.  We *do* still tick down the clip_reveal
                // window as a hint for the renderer, but state doesn't
                // change until an external event.
                break;
            }
            case SAO_UI_DIALOG_STATE_SHRINKING: {
                if (handle->state_elapsed_ms >= handle->shrink_ms) {
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
                break;
            }
            case SAO_UI_DIALOG_STATE_IDLE:
            default:
                break;
            }
            break;   // fall through to exit the while
        }
    }
    if (should_fire && snap.cb != nullptr) {
        const char* text = snap.has_input ? snap.input_text.c_str() : nullptr;
        size_t text_len = snap.has_input ? snap.input_text.size() : 0;
        snap.cb(snap.pressed, text, text_len, snap.user_data);
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_dismiss(
    sao_ui_dialog_handle_t handle, SaoUiDialogButton pressed) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    if (handle->state == SAO_UI_DIALOG_STATE_IDLE && !handle->has_spec) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    handle->pending_pressed = pressed;
    handle->pending_dismiss = (pressed == SAO_UI_DIALOG_BTN_DISMISS);
    handle->have_pending_fire = true;
    if (handle->state != SAO_UI_DIALOG_STATE_SHRINKING) {
        // The mutex is already held, so compute the width inline rather
        // than call the public snapshot function and recurse on the lock.
        if (handle->state == SAO_UI_DIALOG_STATE_EXPANDING && handle->expand_ms > 0) {
            const float progress = std::clamp(
                static_cast<float>(handle->state_elapsed_ms) /
                    static_cast<float>(handle->expand_ms),
                0.0f, 1.0f);
            handle->shrink_start_width = kInitialDialogWidth + static_cast<int32_t>(
                static_cast<float>(handle->width - kInitialDialogWidth) * progress);
        } else {
            handle->shrink_start_width = handle->width;
        }
        handle->state = SAO_UI_DIALOG_STATE_SHRINKING;
        handle->state_elapsed_ms = 0;
    }
    return SAO_STATUS_OK;
}

// ── Button focus + query ─────────────────────────────────────────
extern "C" sao_status_t SAO_UI_CALL sao_ui_dialog_set_button_focus(
    sao_ui_dialog_handle_t handle, int32_t button_index) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    if (!handle->has_spec) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (button_index == -1) {
        handle->focused_index = -1;
        return SAO_STATUS_OK;
    }
    if (button_index < 0 ||
        static_cast<size_t>(button_index) >= handle->buttons.size()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    handle->focused_index = button_index;
    return SAO_STATUS_OK;
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
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    if (!handle->has_spec ||
        handle->state == SAO_UI_DIALOG_STATE_IDLE ||
        handle->state == SAO_UI_DIALOG_STATE_SHRINKING) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    switch (vk) {
    case kVkTab: {
        const int32_t dir = shift_held ? -1 : +1;
        handle->focused_index = advance_focus(handle->buttons,
                                              handle->focused_index, dir);
        return SAO_STATUS_OK;
    }
    case kVkReturn: {
        if (handle->focused_index < 0 ||
            static_cast<size_t>(handle->focused_index) >= handle->buttons.size()) {
            // No focus target => treat like DISMISS.
            handle->pending_pressed = SAO_UI_DIALOG_BTN_DISMISS;
            handle->pending_dismiss = true;
        } else {
            handle->pending_pressed =
                handle->buttons[static_cast<size_t>(handle->focused_index)].kind;
            handle->pending_dismiss =
                (handle->pending_pressed == SAO_UI_DIALOG_BTN_DISMISS);
        }
        handle->have_pending_fire = true;
        handle->state = SAO_UI_DIALOG_STATE_SHRINKING;
        handle->state_elapsed_ms = 0;
        return SAO_STATUS_OK;
    }
    case kVkEscape: {
        handle->pending_pressed = SAO_UI_DIALOG_BTN_DISMISS;
        handle->pending_dismiss = true;
        handle->have_pending_fire = true;
        handle->state = SAO_UI_DIALOG_STATE_SHRINKING;
        handle->state_elapsed_ms = 0;
        return SAO_STATUS_OK;
    }
    default:
        // Unknown key - not consumed, but not an error either.
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
    // Ownership transferred to the internal state machine; the caller
    // is responsible for calling destroy after the callback fires (or
    // on shutdown).  We return here so the API stays fire-and-forget.
    // NOTE: for a real render backend this would attach the handle to
    // a self-destruction queue.  In this state-machine slice we leak
    // to the caller by design - the helpers are convenience only.
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
