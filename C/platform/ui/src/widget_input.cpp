// SAO Auto — input widgets first slice (Wave 4 / Agent d, G3.8).
//
// This slice implements the Button portion of widget_input.h:
//   * sao_ui_button_create / _update / _set_text / _set_active /
//     _set_disabled / _set_click_handler
//
// Plus wave4 helper API for hit-test + event dispatch, mirroring the
// menu.cpp pattern of exposing wave-specific helpers that the test
// binary can call directly.
//
// The other widget_input.h families (icon button / dropdown / checkbox
// / radio / slider) are stubbed as later slices; only Button is Wave4a.
//
// UTF-8 no BOM.

#include "sao/ui/widget_input.h"
#include "sao/ui/widget_kit.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(SAO_UI_BTN_NORMAL == 0, "button kind enum drifted");
static_assert(SAO_UI_BTN_DANGER == 4, "button kind enum drifted");
static_assert(SAO_UI_DROPDOWN_SEPARATOR == -1,
              "dropdown separator sentinel drifted");

namespace {

constexpr int32_t kButtonTag = 120;   // aligned with SAO_UI_WIDGET_BUTTON

struct ButtonState {
    int32_t             tag{kButtonTag};
    SaoUiButtonSpec     spec{};
    std::string         text;                       // owns text bytes
    // Layout cache — cheap to recompute but paid at every hit test.
    int32_t             cached_width{0};
    int32_t             cached_height{0};
    // Callback wiring.
    sao_ui_click_cb_t   click_cb{nullptr};
    void*               click_user_data{nullptr};
    // Interaction state — driven by dispatch_event.
    bool                pressed{false};             // mouse-down within bounds
    bool                hovered{false};             // last cursor was inside
    mutable std::mutex  mtx;
};

int32_t peek_tag(sao_ui_widget_handle_t h) {
    if (h == nullptr) return -1;
    return *reinterpret_cast<const int32_t*>(h);
}

ButtonState* as_button(sao_ui_widget_handle_t h) {
    if (peek_tag(h) != kButtonTag) return nullptr;
    return reinterpret_cast<ButtonState*>(h);
}

// Fixed-width glyph model matches widget_text.cpp — Wave4 first slice
// uses an 8-px advance so measurements are stable in unit tests.
constexpr int32_t kAsciiGlyphAdvancePx = 8;

size_t utf8_glyph_count(const std::string& text) {
    size_t n = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

// Compute preferred size from text + padding, mirroring the Python
// action_button intrinsic sizing (label glyph count × advance + 2 × pad).
void recompute_size_locked(ButtonState& s) {
    const int32_t pad_x = s.spec.pad_x_px > 0 ? s.spec.pad_x_px : 10;
    const int32_t pad_y = s.spec.pad_y_px > 0 ? s.spec.pad_y_px : 6;
    const int32_t glyph_h = 16;   // fixed line height for now
    const int32_t text_w =
        static_cast<int32_t>(utf8_glyph_count(s.text)) * kAsciiGlyphAdvancePx;
    s.cached_width  = text_w + 2 * pad_x;
    s.cached_height = glyph_h + 2 * pad_y;
}

void apply_button_spec_no_lock(ButtonState& s, const SaoUiButtonSpec* spec) {
    s.spec = *spec;
    s.text = spec->text_utf8 ? spec->text_utf8 : "";
    s.spec.text_utf8 = nullptr;
    recompute_size_locked(s);
}

}  // namespace

// ---------------------------------------------------------------------------
// Button ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_create(
    void* /*d3d_device_ptr*/,
    const SaoUiButtonSpec* spec,
    sao_ui_widget_handle_t* out_handle) {
    if (spec == nullptr || out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* s = new ButtonState();
    apply_button_spec_no_lock(*s, spec);
    *out_handle = reinterpret_cast<sao_ui_widget_handle_t>(s);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_update(
    sao_ui_widget_handle_t handle,
    const SaoUiButtonSpec* spec) {
    ButtonState* s = as_button(handle);
    if (s == nullptr || spec == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    apply_button_spec_no_lock(*s, spec);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_set_text(
    sao_ui_widget_handle_t handle,
    const char* text_utf8) {
    ButtonState* s = as_button(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->text = text_utf8 ? text_utf8 : "";
    recompute_size_locked(*s);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_set_active(
    sao_ui_widget_handle_t handle, bool active) {
    ButtonState* s = as_button(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->spec.active = active;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_set_disabled(
    sao_ui_widget_handle_t handle, bool disabled) {
    ButtonState* s = as_button(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->spec.disabled = disabled;
    // Disabled buttons must not stay pressed if we get a subsequent up.
    s->pressed = false;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_set_click_handler(
    sao_ui_widget_handle_t handle,
    sao_ui_click_cb_t callback,
    void* user_data) {
    ButtonState* s = as_button(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->click_cb = callback;
    s->click_user_data = user_data;
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Wave 4 helper API — hit test + event dispatch + preferred-size query.
// Not part of widget_input.h (yet); exposed for the test binary to
// exercise the state machine without dragging in the compositor.
// ---------------------------------------------------------------------------

struct SaoUiPointF {
    float x;
    float y;
};

// Preferred size in pixels (matches action_button intrinsic sizing).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_button_preferred_size(
    sao_ui_widget_handle_t handle,
    int32_t* out_width,
    int32_t* out_height) {
    ButtonState* s = as_button(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (out_width)  *out_width  = s->cached_width;
    if (out_height) *out_height = s->cached_height;
    return SAO_STATUS_OK;
}

// Hit test relative to the button's local coord (0..width, 0..height).
// Disabled buttons still report inside/outside but the caller can
// gate downstream dispatch on that separately.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_button_hit_test(
    sao_ui_widget_handle_t handle,
    SaoUiPointF point,
    bool* out_hit) {
    ButtonState* s = as_button(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    const bool hit = (point.x >= 0.0f && point.y >= 0.0f &&
                      point.x <  static_cast<float>(s->cached_width) &&
                      point.y <  static_cast<float>(s->cached_height));
    if (out_hit) *out_hit = hit;
    return SAO_STATUS_OK;
}

// Wave 4 event codes — locally aligned with widget_kit.h
// SAO_UI_EVT_* range but redeclared to keep this TU independent.
enum ButtonEvent : int32_t {
    kBtnEvtHoverEnter = 3,
    kBtnEvtHoverLeave = 4,
    kBtnEvtMouseDown  = 100,
    kBtnEvtMouseUp    = 101,
    kBtnEvtClick      = 0,        // aligns with SAO_UI_EVT_CLICK
};

// Dispatch a mouse event.  mouse_down → sets pressed; mouse_up → if
// still pressed AND still hovered, fires click.  Disabled buttons
// consume the event silently and never fire the callback.  Returns
// out_action_id = kind on click (caller-visible identifier), -1 otherwise.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_button_dispatch_event(
    sao_ui_widget_handle_t handle,
    int32_t event_type,
    int32_t* out_action_id) {
    ButtonState* s = as_button(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_action_id) *out_action_id = -1;
    sao_ui_click_cb_t cb = nullptr;
    void* user = nullptr;
    bool fire = false;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        switch (event_type) {
            case kBtnEvtHoverEnter:
                s->hovered = true;
                break;
            case kBtnEvtHoverLeave:
                s->hovered = false;
                // Losing hover mid-press cancels the incipient click.
                s->pressed = false;
                break;
            case kBtnEvtMouseDown:
                if (!s->spec.disabled) s->pressed = true;
                break;
            case kBtnEvtMouseUp:
                if (!s->spec.disabled && s->pressed) {
                    fire = true;
                    if (out_action_id) *out_action_id = s->spec.kind;
                    cb = s->click_cb;
                    user = s->click_user_data;
                }
                s->pressed = false;
                break;
            case kBtnEvtClick:
                // Synthetic click (keyboard invoke): fires unconditionally.
                if (!s->spec.disabled) {
                    fire = true;
                    if (out_action_id) *out_action_id = s->spec.kind;
                    cb = s->click_cb;
                    user = s->click_user_data;
                }
                break;
            default:
                break;
        }
    }
    if (fire && cb != nullptr) cb(user);
    return SAO_STATUS_OK;
}

// Query whether the button is in its "active" style (matches
// action_button.set_active).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_button_is_active(
    sao_ui_widget_handle_t handle,
    bool* out_active) {
    ButtonState* s = as_button(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (out_active) *out_active = s->spec.active;
    return SAO_STATUS_OK;
}

// Shared destroy helper.  Follows the same tag-dispatch convention as
// widget_text.cpp.
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_input_family_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr) return;
    uint32_t removed = 0;
    (void)sao_ui_widget_release_event_handlers(handle, &removed);
    if (peek_tag(handle) == kButtonTag) {
        delete reinterpret_cast<ButtonState*>(handle);
    }
}
