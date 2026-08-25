// SAO Auto - fisheye focus math and stateful lens animation.
//
// 1:1 with sao_theme/menu_bar.py + sao_theme/circle_button.py:
//   * ring / column layout populated by client, sizes computed here
//   * hover_target item snaps to max_size
//   * items within |dist| <= falloff_neighbors receive a gamma-shaped
//     falloff back toward base_size
//   * items beyond the falloff window stay at base_size
//   * hit-test uses the *current* (i.e. post-lens) sprite size so the
//     hover target's hitbox grows with the visual - matches SAO ring
//     menu feel and the header contract note.
//
// The header exposes two APIs:
//   1. Pure-function API (sao_ui_fisheye_apply / _hit_test / _ring_layout /
//      _column_layout) — the caller owns the clock and calls apply every
//      frame with now_seconds.
//   2. Stateful lens (sao_ui_fisheye_create / _animate / _apply_
//      column_layout / _destroy) — the handle owns the last hover state
//      and blends via dt_ms.  Preferred for unit tests + script bindings.

#include "sao/ui/fisheye.h"
#include "menu_visual_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace {

constexpr float kPI = 3.14159265358979323846f;

// Default config — matches sao_theme constants in menu_bar_layout.py +
// circle_button.py.  Kept in one place so tests + code agree.
constexpr SaoUiFisheyeConfig kDefaultConfig = {
    /*base_size=*/sao::ui::menu_visual::kVisualButtonBaseSize,
    /*max_size=*/sao::ui::menu_visual::kVisualButtonMaxSize,
    /*slot_size=*/70,
    /*falloff_neighbors=*/SAO_UI_FISHEYE_DEFAULT_FALLOFF_NEIGHBORS,
    /*scale_curve_gamma=*/2.0f,
    /*grow_speed_lerp=*/0.28f,
    /*grow_epsilon=*/0.18f,
    /*hover_ease_ms=*/200.0f,
    /*subpixel_snap=*/true,
    /*_pad=*/{false, false, false},
};

inline float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

SaoUiFisheyeConfig sanitize_config(const SaoUiFisheyeConfig* input) {
    SaoUiFisheyeConfig out = kDefaultConfig;
    if (input == nullptr)
        return out;
    if (input->base_size > 0)
        out.base_size = input->base_size;
    if (input->max_size > 0)
        out.max_size = input->max_size;
    if (input->slot_size > 0)
        out.slot_size = input->slot_size;
    if (input->falloff_neighbors >= 0)
        out.falloff_neighbors = input->falloff_neighbors;
    if (std::isfinite(input->scale_curve_gamma) && input->scale_curve_gamma > 0.0F)
        out.scale_curve_gamma = input->scale_curve_gamma;
    if (std::isfinite(input->grow_speed_lerp) && input->grow_speed_lerp >= 0.0F)
        out.grow_speed_lerp = clampf(input->grow_speed_lerp, 0.0F, 1.0F);
    if (std::isfinite(input->grow_epsilon) && input->grow_epsilon > 0.0F)
        out.grow_epsilon = input->grow_epsilon;
    if (std::isfinite(input->hover_ease_ms) && input->hover_ease_ms > 0.0F)
        out.hover_ease_ms = input->hover_ease_ms;
    out.base_size = std::max(1, out.base_size);
    out.max_size = std::max(out.base_size, out.max_size);
    out.slot_size = std::max(out.max_size, out.slot_size);
    out.falloff_neighbors = std::clamp(out.falloff_neighbors, 0, 4096);
    out.subpixel_snap = input->subpixel_snap;
    return out;
}

// Falloff formula:
//   normalized_distance = |dist| / (falloff_neighbors + 1)
//   size_bonus = (1 - normalized_distance^gamma) if |dist| <= falloff_neighbors else 0
//   target_size = base_size + (max_size - base_size) * size_bonus
//
// Rationale for gamma=2.0 (default): a quadratic falloff makes the two
// immediate neighbors of the hovered button clearly larger than the
// edge buttons — the tell-tale SAO / Dock lens shape.  gamma > 1 pushes
// the lens "shoulder" outward (more items look almost hovered); gamma
// < 1 tightens it into a sharp spike.
inline float target_size_for_distance(
    int32_t distance, int32_t falloff_neighbors,
    float base_size, float max_size, float gamma) {
    const int64_t abs_dist = distance < 0 ? -static_cast<int64_t>(distance) : distance;
    if (falloff_neighbors <= 0) {
        return (abs_dist == 0) ? max_size : base_size;
    }
    if (abs_dist > falloff_neighbors) {
        return base_size;
    }
    const float norm = static_cast<float>(abs_dist) /
                       static_cast<float>(falloff_neighbors + 1);
    const float bonus = 1.0f - std::pow(norm, gamma);
    // clamp to [0,1] against gamma numerical drift
    const float bonus_c = clampf(bonus, 0.0f, 1.0f);
    return base_size + (max_size - base_size) * bonus_c;
}

// Compute target_size for every button relative to hover_target_index.
// hover_target_index == -1 means "no hover"; all buttons collapse to
// base_size.
void compute_targets_column(
    const SaoUiFisheyeConfig& cfg,
    SaoUiFisheyeButton* buttons, size_t count,
    int32_t hover_target_index) {
    const float base_f = static_cast<float>(cfg.base_size > 0
                                               ? cfg.base_size
                                               : sao::ui::menu_visual::kVisualButtonBaseSize);
    const float max_f = static_cast<float>(cfg.max_size > 0
                                              ? cfg.max_size
                                              : sao::ui::menu_visual::kVisualButtonMaxSize);
    for (size_t i = 0; i < count; ++i) {
        SaoUiFisheyeButton& b = buttons[i];
        b.index = static_cast<int32_t>(i);
        if (hover_target_index < 0) {
            b.target_size = base_f;
            b.hover_t = 0.0f;
            continue;
        }
        const int32_t dist = static_cast<int32_t>(i) - hover_target_index;
        b.target_size = target_size_for_distance(
            dist, cfg.falloff_neighbors, base_f, max_f, cfg.scale_curve_gamma);
        // hover_t is a normalized "focus" value in [0,1] for the
        // renderer — 1 at the hovered button, 0 for edge items.  We
        // derive it from target_size relative to the max travel so it
        // tracks the visual bulge one-for-one.
        const float travel = max_f - base_f;
        b.hover_t = (travel > 0.0f) ? (b.target_size - base_f) / travel : 0.0f;
        b.hover_t = clampf(b.hover_t, 0.0f, 1.0f);
    }
}

// Blend current_size toward target_size using a per-frame lerp.  A
// tick advances by dt_ms; the grow_speed_lerp scales the fraction per
// ~16.67ms frame (matches 60Hz Python constant).  We integrate over
// dt_ms with the same formula the Python side uses.
void step_toward_targets(
    const SaoUiFisheyeConfig& cfg,
    SaoUiFisheyeButton* buttons, size_t count,
    int32_t dt_ms) {
    // Convert per-frame lerp fraction to a per-ms rate: assuming
    // grow_speed_lerp is calibrated for a 60Hz tick (16.67ms), the
    // effective decay per ms is grow_speed_lerp/16.67.  We cap
    // multiplied dt to avoid overshoot on huge dt values.
    const float dt = static_cast<float>(dt_ms) / 16.6667f;
    float k = 1.0f - std::pow(1.0f - cfg.grow_speed_lerp, dt);
    k = clampf(k, 0.0f, 1.0f);
    for (size_t i = 0; i < count; ++i) {
        SaoUiFisheyeButton& b = buttons[i];
        if (!std::isfinite(b.current_size))
            b.current_size = b.target_size;
        const float delta = b.target_size - b.current_size;
        if (std::fabs(delta) < cfg.grow_epsilon) {
            b.current_size = b.target_size;
            b.is_animating = false;
        } else {
            b.current_size = b.current_size + delta * k;
        b.is_animating = std::fabs(b.target_size - b.current_size) >= cfg.grow_epsilon;
        }
        if (cfg.subpixel_snap) {
            // Quantize to 0.25 px like the Python side.
            const float snapped = std::round(b.current_size * 4.0f) / 4.0f;
            b.current_size = snapped;
        }
        // sprite center = slot center (matches menu_bar; the fisheye
        // grows in-place, it does NOT reposition the slots).
        b.sprite_center_x = b.slot_center_x;
        b.sprite_center_y = b.slot_center_y;
    }
}

}  // namespace

// ── Default config ────────────────────────────────────────────────
extern "C" const SaoUiFisheyeConfig* SAO_UI_CALL
sao_ui_fisheye_default_config(void) {
    return &kDefaultConfig;
}

// ── Pure-function API (menu_bar.py-style, caller owns clock) ──────
extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_apply(
    const SaoUiFisheyeConfig* config,
    SaoUiFisheyeButton* buttons,
    size_t button_count,
    int32_t hover_target_idx,
    double now_seconds) {
    if (!std::isfinite(now_seconds))
        now_seconds = 0.0;
    if (!std::isfinite(now_seconds)) now_seconds = 0.0;
    if (buttons == nullptr || button_count == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const SaoUiFisheyeConfig cfg = sanitize_config(config);
    std::vector<float> previous_hover(button_count);
    for (size_t i = 0; i < button_count; ++i) previous_hover[i] = std::isfinite(buttons[i].hover_t) ? buttons[i].hover_t : 0.0F;
    static thread_local double previous_now = -1.0;
    const bool first_call = previous_now < 0.0 || now_seconds < previous_now;
    const float elapsed_ms = first_call ? cfg.hover_ease_ms : static_cast<float>((now_seconds - previous_now) * 1000.0);
    const float hover_k = cfg.hover_ease_ms <= 0.0F ? 1.0F : clampf(elapsed_ms / cfg.hover_ease_ms, 0.0F, 1.0F);
    previous_now = now_seconds;
    compute_targets_column(cfg, buttons, button_count, hover_target_idx);
    // Snap current == target so the pure API returns settled sizes
    // in one call (matches menu_bar's per-frame call that already
    // integrates its own lerp elsewhere).
    for (size_t i = 0; i < button_count; ++i) {
        SaoUiFisheyeButton& b = buttons[i];
        // On first ever call, current_size is zero — jump to target
        // so hit test sees the right hitbox from frame 1.
        if (!std::isfinite(b.current_size) || b.current_size <= 0.0f) {
            b.current_size = b.target_size;
        } else {
            // Follow same per-tick lerp path as the stateful animator.
            const float delta = b.target_size - b.current_size;
            const float k = clampf(cfg.grow_speed_lerp, 0.0f, 1.0f);
            b.current_size = std::fabs(delta) < cfg.grow_epsilon
                             ? b.target_size
                             : b.current_size + delta * k;
        }
        b.hover_t = first_call ? b.hover_t : previous_hover[i] + (b.hover_t - previous_hover[i]) * hover_k;
        b.is_animating = std::fabs(b.target_size - b.current_size) >= cfg.grow_epsilon;
        if (cfg.subpixel_snap) {
            b.current_size = std::round(b.current_size * 4.0f) / 4.0f;
        }
        b.sprite_center_x = b.slot_center_x;
        b.sprite_center_y = b.slot_center_y;
    }
    return SAO_STATUS_OK;
}

extern "C" bool SAO_UI_CALL sao_ui_fisheye_any_animating(
    const SaoUiFisheyeButton* buttons, size_t button_count) {
    if (buttons == nullptr) return false;
    for (size_t i = 0; i < button_count; ++i) {
        if (buttons[i].is_animating) return true;
    }
    return false;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_ring_layout(
    int32_t center_x, int32_t center_y,
    int32_t radius,
    float start_angle_rad,
    SaoUiFisheyeButton* buttons,
    size_t button_count) {
    if (buttons == nullptr || button_count == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < button_count; ++i) {
        const float theta = (std::isfinite(start_angle_rad) ? start_angle_rad : 0.0F) +
            2.0f * kPI * static_cast<float>(i) / static_cast<float>(button_count);
        buttons[i].index = static_cast<int32_t>(i);
        buttons[i].slot_center_x = center_x +
            static_cast<int32_t>(std::lround(static_cast<float>(radius) * std::cos(theta)));
        buttons[i].slot_center_y = center_y -
            static_cast<int32_t>(std::lround(static_cast<float>(radius) * std::sin(theta)));
        buttons[i].sprite_center_x = buttons[i].slot_center_x;
        buttons[i].sprite_center_y = buttons[i].slot_center_y;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_column_layout(
    int32_t column_x, int32_t top_y,
    int32_t slot_size,
    SaoUiFisheyeButton* buttons,
    size_t button_count) {
    if (buttons == nullptr || button_count == 0 || slot_size <= 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < button_count; ++i) {
        buttons[i].index = static_cast<int32_t>(i);
        buttons[i].slot_center_x = column_x;
        buttons[i].slot_center_y = top_y + slot_size / 2 +
            static_cast<int32_t>(i) * slot_size;
        buttons[i].sprite_center_x = buttons[i].slot_center_x;
        buttons[i].sprite_center_y = buttons[i].slot_center_y;
    }
    return SAO_STATUS_OK;
}

// ── Hit test — uses current_size, NOT slot ────────────────────────
extern "C" int32_t SAO_UI_CALL sao_ui_fisheye_hit_test(
    const SaoUiFisheyeButton* buttons, size_t button_count,
    int32_t x, int32_t y) {
    if (buttons == nullptr || button_count == 0) return -1;
    // Prefer the hovered / animating buttons first — their hitboxes
    // may fully or partially cover a neighbor's slot rect.  We scan
    // the largest-current-size candidate first so the hovered item
    // wins ties at the boundary.
    int32_t hit = -1;
    float best_size = -1.0f;
    for (size_t i = 0; i < button_count; ++i) {
        const SaoUiFisheyeButton& b = buttons[i];
        const float half = b.current_size * 0.5f;
        if (!std::isfinite(b.current_size) || half <= 0.0f) continue;
        const float dx = static_cast<float>(x - b.sprite_center_x);
        const float dy = static_cast<float>(y - b.sprite_center_y);
        // Circular hit test (SAOCircleButton sprites are round icons).
        if (dx * dx + dy * dy <= half * half) {
            if (b.current_size > best_size) {
                best_size = b.current_size;
                hit = static_cast<int32_t>(i);
            }
        }
    }
    return hit;
}

// ── Stateful lens ─────────────────────────────────────────────────
struct sao_ui_fisheye_s {
    std::mutex mu;
    SaoUiFisheyeConfig cfg = kDefaultConfig;
    int32_t last_hover = -1;
};

extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_create(
    const SaoUiFisheyeConfig* config,
    sao_ui_fisheye_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    auto* h = new (std::nothrow) sao_ui_fisheye_s;
    if (h == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    h->cfg = sanitize_config(config);
    *out_handle = h;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_fisheye_destroy(
    sao_ui_fisheye_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_apply_column_layout(
    sao_ui_fisheye_handle_t handle,
    SaoUiFisheyeButton* buttons,
    size_t button_count,
    int32_t hover_target_index) {
    if (handle == nullptr || buttons == nullptr || button_count == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    SaoUiFisheyeConfig cfg;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        cfg = sanitize_config(&handle->cfg);
        handle->last_hover = hover_target_index;
    }
    compute_targets_column(cfg, buttons, button_count, hover_target_index);
    // Snap current to target for the layout-apply variant (used by
    // callers who don't want animation - just "compute the shape").
    for (size_t i = 0; i < button_count; ++i) {
        SaoUiFisheyeButton& b = buttons[i];
        b.current_size = b.target_size;
        b.is_animating = false;
        b.sprite_center_x = b.slot_center_x;
        b.sprite_center_y = b.slot_center_y;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_animate(
    sao_ui_fisheye_handle_t handle,
    SaoUiFisheyeButton* buttons,
    size_t button_count,
    int32_t from_hover,
    int32_t to_hover,
    int32_t dt_ms) {
    if (handle == nullptr || buttons == nullptr || button_count == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (dt_ms < 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    SaoUiFisheyeConfig cfg;
    {
        std::lock_guard<std::mutex> lk(handle->mu);
        cfg = sanitize_config(&handle->cfg);
        handle->last_hover = to_hover;
    }
    (void)from_hover;   // from_hover is implicit in the buttons'
                        // current_size — we blend from wherever the
                        // buttons currently are toward the new target.
    // First: compute *targets* for the to_hover state.
    compute_targets_column(cfg, buttons, button_count, to_hover);
    // Then: integrate toward those targets by dt_ms.
    step_toward_targets(cfg, buttons, button_count, dt_ms);
    return SAO_STATUS_OK;
}
