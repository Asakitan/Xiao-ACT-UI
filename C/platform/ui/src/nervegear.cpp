// SAO Auto — NerveGear button state machine and Link Start timeline.
//
// This slice owns the 7-state state machine + the Link Start intro
// timeline.  The visual disc render, Tk input proxy, drag physics, and
// GPU compose path all land in later slices — this file gives us the
// pure C++ state transitions the header banner promises.
//
// State machine (mirrors sao_gui_nervegear_button.render_button() +
// the SAOLinkStart intro sequence in sao_theme/link_start.py):
//
//   IDLE      --mouse enter--> HOVER
//   HOVER     --mouse leave--> IDLE
//   HOVER     --mouse down --> PRESSED
//   PRESSED   --mouse up   --> LINKING     (start Link Start intro)
//   PRESSED   --mouse leave--> IDLE        (aborted click)
//   PRESSED   --drag begin --> DRAGGING
//   DRAGGING  --drag end   --> HOVER
//   LINKING   --progress≥1 --> LINKED      (t = timeline.total_duration)
//   LINKED    --logout ev  --> LOGOUT      (via explicit transition())
//   LOGOUT    --2000 ms    --> IDLE
//
// The 2000 ms LOGOUT-hold is the "exit pulse" documented in
// SAO_UI_NG_STATE_LOGOUT; it's shorter than the intro because it's
// just a fade-out (no tunnel, no colour swap).
//
// UTF-8 no BOM.

#include "sao/ui/nervegear.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(sizeof(sao_ui_nervegear_handle_t) == sizeof(void*),
              "nervegear handle must be pointer-width");
static_assert(SAO_UI_NERVEGEAR_SIZE == 72, "NerveGear disc size drifted from Python source");
static_assert(SAO_UI_NG_STATE_IDLE == 0, "state enum head drifted");
static_assert(SAO_UI_NG_STATE_LOGOUT == 6, "state enum tail drifted");

// ---------------------------------------------------------------------------
// Constants.
// ---------------------------------------------------------------------------

namespace {

// _HALF = 36; visible disc radius before palette-driven inset shrinking.
constexpr int32_t kHalfSize = SAO_UI_NERVEGEAR_SIZE / 2;
// Visible cyan border sits at pixel 8 inset from the sprite edge in
// render_button() (`disc = [S(8)…S(SIZE-8)…]`) — the *hit* radius is
// slightly generous (2px slop) so users don't have to click dead-centre.
constexpr int32_t kHitInset = 8;
constexpr int32_t kHitRadius = kHalfSize - kHitInset + 2;

// LOGOUT hold — SAOLinkStart doesn't define a logout pulse in the
// timeline struct; 2000 ms comes from the header comment
// "exit pulse".  Kept as a compile-time constant so the state machine
// doesn't need to plumb a caller-supplied value in this first slice.
constexpr int32_t kLogoutHoldMs = 2000;

// Default Link Start timeline — exact 1:1 with SAOLinkStart._P1_END..
// _P4_FADE_END in sao_theme/link_start.py.  Seconds, matches the
// SaoUiLinkStartTimeline field semantics documented in nervegear.h.
constexpr SaoUiLinkStartTimeline kDefaultTimeline = {
    /* startup_prelude */ 0.72f,
    /* p1_end          */ 3.5f,
    /* p2_start        */ 3.5f,
    /* p2_end          */ 5.5f,
    /* p3_start        */ 5.2f,
    /* p3_end          */ 7.5f,
    /* p4_start        */ 7.3f,
    /* p4_hold_end     */ 9.2f,
    /* p4_fade_end     */ 9.9f,
    /* total_duration  */ 10.0f,
};

} // namespace

// ---------------------------------------------------------------------------
// Internal types.
// ---------------------------------------------------------------------------

struct sao_ui_nervegear_s {
    sao_ui_compositor_handle_t compositor{nullptr};
    sao_ui_theme_handle_t theme{nullptr};
    SaoUiNerveGearPalette palette{SAO_UI_NG_PALETTE_DARK};

    // Position — top-left of the 72×72 sprite bbox in screen coords.
    int32_t x{0};
    int32_t y{0};
    bool visible{false};

    // State machine.
    SaoUiNerveGearState state{SAO_UI_NG_STATE_IDLE};
    // ms spent in the current state.  Used by tick() for LINKING and
    // LOGOUT progressions.
    int32_t state_elapsed_ms{0};

    // Alpha (master fade).  0..1.
    float alpha{1.0f};
    // Glow phase (0..2π).  Advanced automatically by tick() unless
    // an external set_glow_phase() call overrides it.
    float glow_phase{0.0f};

    // Link Start timeline (can be overridden per-handle).
    SaoUiLinkStartTimeline timeline{kDefaultTimeline};

    // Event callback.
    sao_ui_nervegear_event_callback_t callback{nullptr};
    void* callback_user_data{nullptr};

    mutable std::mutex mtx;
};

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

namespace {

// Fire an event to the caller-registered callback, without the mutex
// held (avoids re-entrancy deadlock if the callback re-enters the API).
void fire_event(sao_ui_nervegear_s* ng, SaoUiNerveGearEvent event, int32_t x, int32_t y) {
    if (ng == nullptr)
        return;
    const auto cb = ng->callback;
    void* ud = ng->callback_user_data;
    if (cb != nullptr)
        cb(event, x, y, ud);
}

// State-machine transition guard.  The header documents the legal
// forced targets:
//   IDLE → LINKING     (start Link Start intro)
//   LINKED → LOGOUT    (start exit pulse)
//   any → IDLE         (abort animation)
// Everything else is driven by mouse events / tick().
bool is_legal_forced_transition(SaoUiNerveGearState from, SaoUiNerveGearState to) {
    if (to == SAO_UI_NG_STATE_IDLE)
        return true; // abort is always ok
    if (from == SAO_UI_NG_STATE_IDLE && to == SAO_UI_NG_STATE_LINKING)
        return true;
    if (from == SAO_UI_NG_STATE_LINKED && to == SAO_UI_NG_STATE_LOGOUT)
        return true;
    return false;
}

// LINKING progress: t is milliseconds since we entered LINKING; total
// duration is timeline.total_duration seconds.  Clamped to [0, 1].
float compute_linking_progress(int32_t elapsed_ms, const SaoUiLinkStartTimeline& tl) {
    if (tl.total_duration <= 0.0f)
        return 1.0f;
    const float seconds = static_cast<float>(elapsed_ms) / 1000.0f;
    float p = seconds / tl.total_duration;
    if (p < 0.0f)
        p = 0.0f;
    if (p > 1.0f)
        p = 1.0f;
    return p;
}

bool valid_timeline(const SaoUiLinkStartTimeline& timeline) noexcept {
    const float values[] = {
        timeline.startup_prelude, timeline.p1_end,         timeline.p2_start, timeline.p2_end,
        timeline.p3_start,        timeline.p3_end,         timeline.p4_start, timeline.p4_hold_end,
        timeline.p4_fade_end,     timeline.total_duration,
    };
    for (const float value : values) {
        if (!std::isfinite(value) || value < 0.0F)
            return false;
    }
    return timeline.total_duration > 0.0F && timeline.startup_prelude <= timeline.p1_end &&
           timeline.p1_end <= timeline.p2_start && timeline.p2_start <= timeline.p2_end &&
           timeline.p2_start <= timeline.p3_start && timeline.p2_end <= timeline.p3_end &&
           timeline.p3_start <= timeline.p3_end && timeline.p3_start <= timeline.p4_start &&
           timeline.p3_end <= timeline.p4_hold_end && timeline.p4_start <= timeline.p4_hold_end &&
           timeline.p4_hold_end <= timeline.p4_fade_end &&
           timeline.p4_fade_end <= timeline.total_duration;
}

} // namespace

// ---------------------------------------------------------------------------
// Header-declared entry points.
// ---------------------------------------------------------------------------

extern "C" const SaoUiLinkStartTimeline* SAO_UI_CALL sao_ui_nervegear_default_timeline(void) {
    return &kDefaultTimeline;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_nervegear_validate_timeline(const SaoUiLinkStartTimeline* timeline) {
    return timeline != nullptr && valid_timeline(*timeline) ? SAO_STATUS_OK
                                                            : SAO_STATUS_ERR_INVALID_ARGUMENT;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_create(sao_ui_compositor_handle_t compositor,
                                                            sao_ui_theme_handle_t theme,
                                                            int32_t initial_x, int32_t initial_y,
                                                            SaoUiNerveGearPalette palette,
                                                            sao_ui_nervegear_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (palette != SAO_UI_NG_PALETTE_DARK && palette != SAO_UI_NG_PALETTE_LIGHT) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto ng = std::make_unique<sao_ui_nervegear_s>();
    ng->compositor = compositor;
    ng->theme = theme;
    ng->palette = palette;
    ng->x = initial_x;
    ng->y = initial_y;
    ng->visible = false;
    ng->state = SAO_UI_NG_STATE_IDLE;
    ng->alpha = 1.0f;
    ng->glow_phase = 0.0f;
    ng->timeline = kDefaultTimeline;
    *out_handle = ng.release();
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_nervegear_destroy(sao_ui_nervegear_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_show(sao_ui_nervegear_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    handle->visible = true;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_hide(sao_ui_nervegear_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    handle->visible = false;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_nervegear_raise_topmost(sao_ui_nervegear_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    // The compose path (later slice) is responsible for actually
    // touching z_order.h — in this slice we just ack the request.
    // Returning OK matches the header contract without pretending to
    // have done work we haven't done yet.
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_set_position(sao_ui_nervegear_handle_t handle,
                                                                  int32_t x, int32_t y) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    handle->x = x;
    handle->y = y;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_get_position(sao_ui_nervegear_handle_t handle,
                                                                  int32_t* out_x, int32_t* out_y) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_x == nullptr || out_y == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mtx);
    *out_x = handle->x;
    *out_y = handle->y;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_set_palette(sao_ui_nervegear_handle_t handle,
                                                                 SaoUiNerveGearPalette palette) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (palette != SAO_UI_NG_PALETTE_DARK && palette != SAO_UI_NG_PALETTE_LIGHT) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mtx);
    handle->palette = palette;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_get_state(sao_ui_nervegear_handle_t handle,
                                                               SaoUiNerveGearState* out_state) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_state == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    *out_state = handle->state;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_transition(sao_ui_nervegear_handle_t handle,
                                                                SaoUiNerveGearState target_state) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (target_state < SAO_UI_NG_STATE_IDLE || target_state > SAO_UI_NG_STATE_LOGOUT) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    SaoUiNerveGearEvent evt_to_fire = SAO_UI_NG_EV_HOVER_ENTER; // dummy
    bool should_fire = false;
    int32_t fire_x = 0, fire_y = 0;
    {
        std::lock_guard<std::mutex> lock(handle->mtx);
        if (!is_legal_forced_transition(handle->state, target_state)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (handle->state != target_state) {
            handle->state = target_state;
            handle->state_elapsed_ms = 0;
            if (target_state == SAO_UI_NG_STATE_LINKING) {
                evt_to_fire = SAO_UI_NG_EV_LINK_STARTED;
                should_fire = true;
                fire_x = handle->x + kHalfSize;
                fire_y = handle->y + kHalfSize;
            }
        }
    }
    if (should_fire)
        fire_event(handle, evt_to_fire, fire_x, fire_y);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_set_timeline(
    sao_ui_nervegear_handle_t handle, const SaoUiLinkStartTimeline* timeline) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (timeline != nullptr && sao_ui_nervegear_validate_timeline(timeline) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    if (timeline == nullptr) {
        handle->timeline = kDefaultTimeline;
    } else {
        handle->timeline = *timeline;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_set_alpha(sao_ui_nervegear_handle_t handle,
                                                               float alpha) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!(alpha == alpha))
        return SAO_STATUS_ERR_INVALID_ARGUMENT; // NaN guard
    if (alpha < 0.0f)
        alpha = 0.0f;
    if (alpha > 1.0f)
        alpha = 1.0f;
    std::lock_guard<std::mutex> lock(handle->mtx);
    handle->alpha = alpha;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_nervegear_set_glow_phase(sao_ui_nervegear_handle_t handle, float glow_phase) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!(glow_phase == glow_phase))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    handle->glow_phase = glow_phase;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_get_hit_shape(sao_ui_nervegear_handle_t handle,
                                                                   int32_t* out_center_x,
                                                                   int32_t* out_center_y,
                                                                   int32_t* out_radius) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_center_x == nullptr || out_center_y == nullptr || out_radius == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mtx);
    *out_center_x = handle->x + kHalfSize;
    *out_center_y = handle->y + kHalfSize;
    *out_radius = kHitRadius;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_nervegear_set_event_callback(
    sao_ui_nervegear_handle_t handle, sao_ui_nervegear_event_callback_t callback, void* user_data) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    handle->callback = callback;
    handle->callback_user_data = user_data;
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// NerveGear helper API (exported for tests + future input path).
//
// These are not in nervegear.h; they cover the state-machine bring-up
// tests and the mouse-driven transitions.  Once the Tk input proxy
// path lands, some of these might promote into nervegear.h.
// ---------------------------------------------------------------------------

// Advance the state machine by dt_ms.  Handles:
//   * glow_phase auto-tick (~2π rad / 8 s = π/4 rad/s, per header)
//   * LINKING → LINKED once elapsed_ms ≥ total_duration
//   * LOGOUT  → IDLE   once elapsed_ms ≥ kLogoutHoldMs
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_tick(sao_ui_nervegear_handle_t handle, int32_t dt_ms) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (dt_ms < 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    SaoUiNerveGearEvent evt_to_fire = SAO_UI_NG_EV_HOVER_ENTER;
    bool should_fire = false;
    int32_t fire_x = 0, fire_y = 0;
    {
        std::lock_guard<std::mutex> lock(handle->mtx);
        // Glow phase advances continuously (header: "~2π/8 rad/s").
        // In practice we only care about *some* linear advance so
        // the shader has a moving t; the exact rate doesn't matter
        // in this state-machine slice.
        static constexpr double kGlowRateRadPerMs = 3.14159265358979323846 / 4000.0;
        handle->glow_phase =
            static_cast<float>(std::fmod(static_cast<double>(handle->glow_phase) +
                                             static_cast<double>(dt_ms) * kGlowRateRadPerMs,
                                         2.0 * 3.14159265358979323846));
        handle->state_elapsed_ms += dt_ms;
        switch (handle->state) {
        case SAO_UI_NG_STATE_LINKING: {
            const float p = compute_linking_progress(handle->state_elapsed_ms, handle->timeline);
            if (p >= 1.0f) {
                handle->state = SAO_UI_NG_STATE_LINKED;
                handle->state_elapsed_ms = 0;
                evt_to_fire = SAO_UI_NG_EV_LINK_DONE;
                should_fire = true;
                fire_x = handle->x + kHalfSize;
                fire_y = handle->y + kHalfSize;
            }
            break;
        }
        case SAO_UI_NG_STATE_LOGOUT:
            if (handle->state_elapsed_ms >= kLogoutHoldMs) {
                handle->state = SAO_UI_NG_STATE_IDLE;
                handle->state_elapsed_ms = 0;
                evt_to_fire = SAO_UI_NG_EV_LOGOUT_DONE;
                should_fire = true;
                fire_x = handle->x + kHalfSize;
                fire_y = handle->y + kHalfSize;
            }
            break;
        default:
            break;
        }
    }
    if (should_fire)
        fire_event(handle, evt_to_fire, fire_x, fire_y);
    return SAO_STATUS_OK;
}

// LINKING progress query — 0..1.  Returns 0 when not in LINKING state.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_get_link_progress(sao_ui_nervegear_handle_t handle, float* out_progress) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_progress == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    switch (handle->state) {
    case SAO_UI_NG_STATE_LINKING:
        *out_progress = compute_linking_progress(handle->state_elapsed_ms, handle->timeline);
        break;
    case SAO_UI_NG_STATE_LINKED:
        *out_progress = 1.0f;
        break;
    default:
        *out_progress = 0.0f;
        break;
    }
    return SAO_STATUS_OK;
}

// Circular hit test — 72×72 sprite, visible disc = kHitRadius.  Corners
// of the sprite bbox miss (matches the SetWindowRgn accumulation contract
// documented in the header banner).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_hit_test(sao_ui_nervegear_handle_t handle, int32_t px, int32_t py, bool* out_hit) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_hit == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mtx);
    const int32_t cx = handle->x + kHalfSize;
    const int32_t cy = handle->y + kHalfSize;
    const int32_t dx = px - cx;
    const int32_t dy = py - cy;
    const int32_t d2 = dx * dx + dy * dy;
    const int32_t r2 = kHitRadius * kHitRadius;
    *out_hit = (d2 <= r2);
    return SAO_STATUS_OK;
}

// Mouse-driven transitions.  These are the paths not covered by
// transition() (which is limited to forced targets per the header
// contract).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_on_mouse_enter(sao_ui_nervegear_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    bool should_fire = false;
    int32_t fx = 0, fy = 0;
    {
        std::lock_guard<std::mutex> lock(handle->mtx);
        if (handle->state == SAO_UI_NG_STATE_IDLE) {
            handle->state = SAO_UI_NG_STATE_HOVER;
            handle->state_elapsed_ms = 0;
            should_fire = true;
            fx = handle->x + kHalfSize;
            fy = handle->y + kHalfSize;
        }
    }
    if (should_fire)
        fire_event(handle, SAO_UI_NG_EV_HOVER_ENTER, fx, fy);
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_on_mouse_leave(sao_ui_nervegear_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    bool should_fire = false;
    int32_t fx = 0, fy = 0;
    {
        std::lock_guard<std::mutex> lock(handle->mtx);
        if (handle->state == SAO_UI_NG_STATE_HOVER || handle->state == SAO_UI_NG_STATE_PRESSED) {
            handle->state = SAO_UI_NG_STATE_IDLE;
            handle->state_elapsed_ms = 0;
            should_fire = true;
            fx = handle->x + kHalfSize;
            fy = handle->y + kHalfSize;
        }
    }
    if (should_fire)
        fire_event(handle, SAO_UI_NG_EV_HOVER_LEAVE, fx, fy);
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_on_mouse_down(sao_ui_nervegear_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mtx);
    if (handle->state == SAO_UI_NG_STATE_HOVER) {
        handle->state = SAO_UI_NG_STATE_PRESSED;
        handle->state_elapsed_ms = 0;
    }
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_on_mouse_up(sao_ui_nervegear_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    bool should_fire_click = false;
    bool should_fire_link = false;
    int32_t fx = 0, fy = 0;
    {
        std::lock_guard<std::mutex> lock(handle->mtx);
        if (handle->state == SAO_UI_NG_STATE_PRESSED) {
            handle->state = SAO_UI_NG_STATE_LINKING;
            handle->state_elapsed_ms = 0;
            should_fire_click = true;
            should_fire_link = true;
            fx = handle->x + kHalfSize;
            fy = handle->y + kHalfSize;
        }
    }
    if (should_fire_click)
        fire_event(handle, SAO_UI_NG_EV_LEFT_CLICK, fx, fy);
    if (should_fire_link)
        fire_event(handle, SAO_UI_NG_EV_LINK_STARTED, fx, fy);
    return SAO_STATUS_OK;
}
