// SAO Auto — animator curve library + 60Hz scheduler first slice.
//
// Mirrors `sao_theme/utils.py` easing
// functions verbatim and the `sao_theme/animator.py` scheduler.
// Coupled with scheduler.h (Agent a) — this file exposes the pure
// math and the per-handle animation registry; the actual 60Hz pump
// call site is elsewhere.

#include "sao/ui/animator.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr float kPI = 3.14159265358979323846f;

// Clamp helper (std::clamp requires <algorithm>; keep local for clarity).
inline float clamp01(float t) {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

// De Casteljau cubic Bezier evaluated at parameter u.  Returns (x, y).
// The header contract for `sao_ui_curve_evaluate_bezier` takes t as the
// *time* input and expects the y-coordinate at the u where x(u) == t.
// We solve x(u)=t with 8 iterations of bisection (deterministic, no
// Newton flakiness on flat regions).
struct BezierPoint { float x; float y; };

inline BezierPoint bezier_eval(
    float u,
    float p1x, float p1y, float p2x, float p2y) {
    // P0=(0,0), P3=(1,1) implicit.
    const float mu = 1.0f - u;
    const float b0 = mu * mu * mu;
    const float b1 = 3.0f * mu * mu * u;
    const float b2 = 3.0f * mu * u * u;
    const float b3 = u * u * u;
    // P0.x = 0, P0.y = 0, P3.x = 1, P3.y = 1.
    return { b1 * p1x + b2 * p2x + b3, b1 * p1y + b2 * p2y + b3 };
}

inline float bezier_solve_y(
    float t,
    float p1x, float p1y, float p2x, float p2y) {
    // Bisection on u ∈ [0,1] until x(u) matches t within 1e-4.
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hi);
        const float x = bezier_eval(mid, p1x, p1y, p2x, p2y).x;
        if (x < t) lo = mid; else hi = mid;
    }
    return bezier_eval(0.5f * (lo + hi), p1x, p1y, p2x, p2y).y;
}

}  // namespace

// ── Direct curve evaluation ───────────────────────────────────────
extern "C" float SAO_UI_CALL sao_ui_curve_evaluate(
    SaoUiCurve curve, float t) {
    t = clamp01(t);
    switch (curve) {
    case SAO_UI_CURVE_LINEAR:
        return t;
    case SAO_UI_CURVE_EASE_IN:
        // matches utils.py: t ** 3
        return t * t * t;
    case SAO_UI_CURVE_EASE_OUT: {
        // matches utils.py: 1 - (1 - t) ** 3
        const float u = 1.0f - t;
        return 1.0f - u * u * u;
    }
    case SAO_UI_CURVE_EASE_IN_OUT:
        // matches utils.py: 3t^2 - 2t^3
        return 3.0f * t * t - 2.0f * t * t * t;
    case SAO_UI_CURVE_EASE_OUT_BACK_LITE: {
        // matches utils.py ease_out_back_lite: standard easeOutBack
        // cubic clipped to plateau at 1.0.  c1=1.70158, c3=c1+1.
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.0f;
        const float u = t - 1.0f;
        const float back = 1.0f + c3 * u * u * u + c1 * u * u;
        return back > 1.0f ? 1.0f : back;
    }
    case SAO_UI_CURVE_EASE_OUT_CUBIC: {
        // Explicit "cubic-out" — matches _sao_cy_uihelpers.ease_out_cubic.
        // Same formula as EASE_OUT but kept separate for grep-alignment
        // with Python enum.
        const float u = 1.0f - t;
        return 1.0f - u * u * u;
    }
    case SAO_UI_CURVE_SPRING: {
        // Damped-sine spring pop.  Formula:
        //   sin((t*13*PI)/2) * pow(2, -10*t) + 1
        // when t == 0 returns 1 (bad for animation start), so we invert
        // for a proper "starts at 0" progression:
        //   1 - (sin((1-t)*13*PI/2) * pow(2, -10*(1-t)))
        // which starts at 0 (t=0 → 1 - sin(13π/2)*2^-10 ≈ 1 - ε) — close
        // to but not exactly 0.  Force endpoints for safety.
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        const float u = 1.0f - t;
        const float envelope = std::pow(2.0f, -10.0f * u);
        const float wave = std::sin((u * 13.0f * kPI) / 2.0f);
        return 1.0f - wave * envelope;
    }
    case SAO_UI_CURVE_BOUNCE: {
        // Classic CSS "bounce-out" — 4 segments, all quadratic.  Matches
        // Robert Penner's easeOutBounce and Chrome's default keyframe
        // bounce curve.
        const float n1 = 7.5625f;
        const float d1 = 2.75f;
        if (t < 1.0f / d1) {
            return n1 * t * t;
        } else if (t < 2.0f / d1) {
            const float u = t - 1.5f / d1;
            return n1 * u * u + 0.75f;
        } else if (t < 2.5f / d1) {
            const float u = t - 2.25f / d1;
            return n1 * u * u + 0.9375f;
        } else {
            const float u = t - 2.625f / d1;
            return n1 * u * u + 0.984375f;
        }
    }
    case SAO_UI_CURVE_STEP:
        return t < 1.0f ? 0.0f : 1.0f;
    case SAO_UI_CURVE_CUBIC_BEZIER:
        // Requires bezier params — without them, degenerate to linear.
        // Real callers go through sao_ui_curve_evaluate_bezier.
        return t;
    default:
        return t;
    }
}

extern "C" float SAO_UI_CALL sao_ui_curve_evaluate_bezier(
    const SaoUiBezierParams* params, float t) {
    if (params == nullptr) return clamp01(t);
    t = clamp01(t);
    return bezier_solve_y(t, params->p1x, params->p1y, params->p2x, params->p2y);
}

// ── linear interp ─────────────────────────────────────────────────
extern "C" float SAO_UI_CALL sao_ui_lerp_f32(float a, float b, float t) {
    return a + (b - a) * t;
}

extern "C" int32_t SAO_UI_CALL sao_ui_lerp_i32(int32_t a, int32_t b, float t) {
    return static_cast<int32_t>(std::lround(
        static_cast<float>(a) + (static_cast<float>(b - a)) * t));
}

extern "C" uint32_t SAO_UI_CALL sao_ui_lerp_argb(
    uint32_t argb_a, uint32_t argb_b, float t) {
    // Preserve alpha lerp behavior of _sao_cy_uihelpers.lerp_hex_color.
    t = clamp01(t);
    const uint32_t a0 = (argb_a >> 24) & 0xFFu;
    const uint32_t r0 = (argb_a >> 16) & 0xFFu;
    const uint32_t g0 = (argb_a >> 8) & 0xFFu;
    const uint32_t b0 = argb_a & 0xFFu;
    const uint32_t a1 = (argb_b >> 24) & 0xFFu;
    const uint32_t r1 = (argb_b >> 16) & 0xFFu;
    const uint32_t g1 = (argb_b >> 8) & 0xFFu;
    const uint32_t b1 = argb_b & 0xFFu;
    auto blend = [t](uint32_t a, uint32_t b) -> uint32_t {
        return static_cast<uint32_t>(std::lround(
            static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t));
    };
    const uint32_t a = blend(a0, a1) & 0xFFu;
    const uint32_t r = blend(r0, r1) & 0xFFu;
    const uint32_t g = blend(g0, g1) & 0xFFu;
    const uint32_t b = blend(b0, b1) & 0xFFu;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// ── animator instance ────────────────────────────────────────────
namespace {

struct Animation {
    sao_ui_animation_id_t id = 0;
    int32_t duration_ms = 0;
    int32_t delay_ms = 0;
    SaoUiCurve curve = SAO_UI_CURVE_LINEAR;
    // Owned copy so caller's stack-allocated params can go out of scope.
    SaoUiBezierParams bezier{};
    bool has_bezier = false;
    sao_ui_animation_tick_callback_t on_tick = nullptr;
    sao_ui_animation_done_callback_t on_done = nullptr;
    void* user_data = nullptr;
    std::string dedup_key;

    // Runtime.
    double start_seconds = 0.0;
    double last_tick_seconds = 0.0;
    bool started = false;
    bool completed = false;
};

}  // namespace

struct sao_ui_animator_s {
    std::mutex mu;
    std::atomic<uint64_t> next_id{1};
    std::vector<Animation> animations;
    // First-tick synthesis: when start_seconds is 0 and we haven't
    // stamped it yet, we take now_seconds as the origin.  This lets
    // callers add animations before the scheduler starts ticking.
};

extern "C" sao_status_t SAO_UI_CALL sao_ui_animator_create(
    sao_ui_animator_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    auto* a = new (std::nothrow) sao_ui_animator_s;
    if (a == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    *out_handle = a;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_animator_destroy(
    sao_ui_animator_handle_t handle) {
    if (handle == nullptr) return;
    // Fire cancel callbacks for any in-flight animations so caller
    // resources tied to user_data get released.  Snapshot first to
    // avoid holding the lock across callbacks.
    std::vector<Animation> to_cancel;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        for (Animation& a : handle->animations) {
            if (!a.completed) to_cancel.push_back(a);
        }
        handle->animations.clear();
    }
    for (const Animation& a : to_cancel) {
        if (a.on_done != nullptr) a.on_done(/*cancelled=*/true, a.user_data);
    }
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animator_animate(
    sao_ui_animator_handle_t handle,
    const SaoUiAnimationSpec* spec,
    sao_ui_animation_id_t* out_id) {
    if (handle == nullptr || spec == nullptr || spec->on_tick == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_id != nullptr) *out_id = 0;
    // Cancel dedup key match first (outside lock — caller callbacks).
    std::string dedup;
    if (spec->dedup_key_utf8 != nullptr && spec->dedup_key_utf8[0] != '\0') {
        dedup = spec->dedup_key_utf8;
    }
    Animation cancelled_snapshot;
    bool have_cancel = false;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        if (!dedup.empty()) {
            for (auto it = handle->animations.begin(); it != handle->animations.end(); ++it) {
                if (it->dedup_key == dedup && !it->completed) {
                    cancelled_snapshot = *it;
                    have_cancel = true;
                    handle->animations.erase(it);
                    break;
                }
            }
        }
        Animation a;
        a.id = handle->next_id.fetch_add(1);
        a.duration_ms = spec->duration_ms > 0 ? spec->duration_ms : 1;
        a.delay_ms = spec->delay_ms > 0 ? spec->delay_ms : 0;
        a.curve = spec->curve;
        if (spec->curve == SAO_UI_CURVE_CUBIC_BEZIER && spec->bezier != nullptr) {
            a.bezier = *spec->bezier;
            a.has_bezier = true;
        }
        a.on_tick = spec->on_tick;
        a.on_done = spec->on_done;
        a.user_data = spec->user_data;
        a.dedup_key = std::move(dedup);
        if (out_id != nullptr) *out_id = a.id;
        handle->animations.push_back(std::move(a));
    }
    if (have_cancel && cancelled_snapshot.on_done != nullptr) {
        cancelled_snapshot.on_done(/*cancelled=*/true, cancelled_snapshot.user_data);
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animator_cancel(
    sao_ui_animator_handle_t handle, sao_ui_animation_id_t id) {
    if (handle == nullptr || id == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    Animation snapshot;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        for (auto it = handle->animations.begin(); it != handle->animations.end(); ++it) {
            if (it->id == id && !it->completed) {
                snapshot = *it;
                found = true;
                handle->animations.erase(it);
                break;
            }
        }
    }
    if (!found) return SAO_STATUS_ERR_NOT_FOUND;
    if (snapshot.on_done != nullptr) {
        snapshot.on_done(/*cancelled=*/true, snapshot.user_data);
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animator_cancel_all(
    sao_ui_animator_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::vector<Animation> to_cancel;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        for (Animation& a : handle->animations) {
            if (!a.completed) to_cancel.push_back(a);
        }
        handle->animations.clear();
    }
    for (const Animation& a : to_cancel) {
        if (a.on_done != nullptr) a.on_done(/*cancelled=*/true, a.user_data);
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animator_is_running(
    sao_ui_animator_handle_t handle,
    sao_ui_animation_id_t id, bool* out_running) {
    if (handle == nullptr || out_running == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_running = false;
    std::lock_guard<std::mutex> lk(handle->mu);
    for (const Animation& a : handle->animations) {
        if (a.id == id && !a.completed) {
            *out_running = true;
            return SAO_STATUS_OK;
        }
    }
    return SAO_STATUS_OK;
}

// Structure to snapshot a tick call so callbacks fire outside the
// lock — otherwise a callback that re-enters animate() would deadlock.
namespace {
struct TickCall {
    sao_ui_animation_tick_callback_t on_tick;
    float eased_t;
    float raw_t;
    void* user_data;
};
struct DoneCall {
    sao_ui_animation_done_callback_t on_done;
    bool cancelled;
    void* user_data;
};
}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_animator_tick(
    sao_ui_animator_handle_t handle, double now_seconds) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::vector<TickCall> ticks;
    std::vector<DoneCall> dones;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        // Advance every active animation.  Newly-added ones have
        // start_seconds == 0; stamp them here.
        for (Animation& a : handle->animations) {
            if (a.completed) continue;
            if (!a.started) {
                a.start_seconds = now_seconds;
                a.last_tick_seconds = now_seconds;
                a.started = true;
            }
            const double effective_start =
                a.start_seconds + static_cast<double>(a.delay_ms) / 1000.0;
            if (now_seconds < effective_start) continue;
            const double elapsed = now_seconds - effective_start;
            const double duration_s = static_cast<double>(a.duration_ms) / 1000.0;
            double raw = duration_s > 0.0 ? elapsed / duration_s : 1.0;
            if (raw < 0.0) raw = 0.0;
            if (raw > 1.0) raw = 1.0;
            const float raw_f = static_cast<float>(raw);
            float eased;
            if (a.curve == SAO_UI_CURVE_CUBIC_BEZIER && a.has_bezier) {
                eased = sao_ui_curve_evaluate_bezier(&a.bezier, raw_f);
            } else {
                eased = sao_ui_curve_evaluate(a.curve, raw_f);
            }
            ticks.push_back({a.on_tick, eased, raw_f, a.user_data});
            a.last_tick_seconds = now_seconds;
            if (raw >= 1.0) {
                a.completed = true;
                if (a.on_done != nullptr) {
                    dones.push_back({a.on_done, /*cancelled=*/false, a.user_data});
                }
            }
        }
        // Drop completed animations from the list.
        handle->animations.erase(
            std::remove_if(handle->animations.begin(), handle->animations.end(),
                           [](const Animation& a) { return a.completed; }),
            handle->animations.end());
    }
    for (const TickCall& c : ticks) {
        c.on_tick(c.eased_t, c.raw_t, c.user_data);
    }
    for (const DoneCall& d : dones) {
        d.on_done(d.cancelled, d.user_data);
    }
    return SAO_STATUS_OK;
}

extern "C" bool SAO_UI_CALL sao_ui_animator_has_active(
    sao_ui_animator_handle_t handle) {
    if (handle == nullptr) return false;
    std::lock_guard<std::mutex> lk(handle->mu);
    for (const Animation& a : handle->animations) {
        if (!a.completed) return true;
    }
    return false;
}
