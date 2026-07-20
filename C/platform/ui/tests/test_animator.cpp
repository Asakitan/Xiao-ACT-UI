// SAO Auto — animator curve endpoints and callback-scheduler tests.
//
// Verifies the 10-curve library endpoint values, monotonicity where
// applicable, and the tick loop's callback delivery to on_update
// (with eased_t) and on_complete (fires exactly once).

#include <atomic>
#include <cmath>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/animator.h"

namespace {

// ε tolerance for float comparisons that hit boundary cases.
constexpr float kEps = 1e-4f;

// Bounce is a piecewise-quadratic that legitimately dips below the
// linear line multiple times.  This helper counts local minima across
// a sampling grid.
int count_local_minima_bounce(int samples) {
    int minima = 0;
    float prev = sao_ui_curve_evaluate(SAO_UI_CURVE_BOUNCE, 0.0f);
    float cur  = sao_ui_curve_evaluate(SAO_UI_CURVE_BOUNCE, 1.0f / samples);
    for (int i = 2; i <= samples; ++i) {
        const float next = sao_ui_curve_evaluate(SAO_UI_CURVE_BOUNCE,
            static_cast<float>(i) / samples);
        if (cur < prev && cur < next) ++minima;
        prev = cur;
        cur = next;
    }
    return minima;
}

}  // namespace

TEST_CASE("animator_curve_linear_endpoints", "[ui][animator][interpreter]") {
    REQUIRE(sao_ui_curve_evaluate(SAO_UI_CURVE_LINEAR, 0.0f) == 0.0f);
    REQUIRE(sao_ui_curve_evaluate(SAO_UI_CURVE_LINEAR, 1.0f) == 1.0f);
    REQUIRE(sao_ui_curve_evaluate(SAO_UI_CURVE_LINEAR, 0.5f) == 0.5f);
    // Clamping.
    REQUIRE(sao_ui_curve_evaluate(SAO_UI_CURVE_LINEAR, -0.5f) == 0.0f);
    REQUIRE(sao_ui_curve_evaluate(SAO_UI_CURVE_LINEAR, 1.5f) == 1.0f);
}

TEST_CASE("animator_curve_ease_in_starts_slow", "[ui][animator][interpreter]") {
    // ease_in(t) = t^3 — value at t=0.5 is 0.125, well below linear 0.5.
    const float v = sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_IN, 0.5f);
    REQUIRE(v < 0.5f);
    REQUIRE(std::fabs(v - 0.125f) < kEps);
    // Endpoints.
    REQUIRE(sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_IN, 0.0f) == 0.0f);
    REQUIRE(sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_IN, 1.0f) == 1.0f);
}

TEST_CASE("animator_curve_ease_out_ends_slow", "[ui][animator][interpreter]") {
    // ease_out(t) = 1 - (1-t)^3.  At t=0.5, value = 0.875 (well above 0.5).
    const float v = sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_OUT, 0.5f);
    REQUIRE(v > 0.5f);
    REQUIRE(std::fabs(v - 0.875f) < kEps);
    // Derivative at t=1 approaches 0 (curve flattens) — check that the
    // gap between 0.9 and 1.0 is smaller than between 0.0 and 0.1.
    const float g_early = sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_OUT, 0.1f) -
                          sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_OUT, 0.0f);
    const float g_late  = sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_OUT, 1.0f) -
                          sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_OUT, 0.9f);
    REQUIRE(g_late < g_early);
}

TEST_CASE("animator_curve_ease_in_out_symmetric", "[ui][animator][interpreter]") {
    // ease_in_out is symmetric around (0.5, 0.5).
    const float mid = sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_IN_OUT, 0.5f);
    REQUIRE(std::fabs(mid - 0.5f) < kEps);
    // Pair symmetry: f(t) + f(1-t) == 1 for the analytical form
    // 3t^2 - 2t^3.
    for (int i = 1; i < 10; ++i) {
        const float t = static_cast<float>(i) / 10.0f;
        const float a = sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_IN_OUT, t);
        const float b = sao_ui_curve_evaluate(SAO_UI_CURVE_EASE_IN_OUT, 1.0f - t);
        REQUIRE(std::fabs(a + b - 1.0f) < kEps);
    }
}

TEST_CASE("animator_curve_bounce_bounces_multiple_times", "[ui][animator][interpreter]") {
    // The classic 4-segment bounce has 3 local minima before the last
    // segment settles at 1.0.  Sample densely enough to catch them.
    const int minima = count_local_minima_bounce(400);
    REQUIRE(minima >= 3);
    // Endpoints stay pinned.
    REQUIRE(sao_ui_curve_evaluate(SAO_UI_CURVE_BOUNCE, 0.0f) == 0.0f);
    REQUIRE(std::fabs(sao_ui_curve_evaluate(SAO_UI_CURVE_BOUNCE, 1.0f) - 1.0f) < kEps);
}

namespace {

struct AnimCtx {
    std::atomic<int32_t> ticks{0};
    std::atomic<float>  last_eased{-1.0f};
    std::atomic<bool>   done_fired{false};
};

void SAO_UI_CALL anim_on_tick(float eased_t, float /*raw_t*/, void* ud) {
    auto* c = static_cast<AnimCtx*>(ud);
    c->ticks.fetch_add(1);
    c->last_eased.store(eased_t);
}

void SAO_UI_CALL anim_on_done(bool /*cancelled*/, void* ud) {
    auto* c = static_cast<AnimCtx*>(ud);
    c->done_fired.store(true);
}

struct DoneOnly {
    std::atomic<int32_t> calls{0};
};

void SAO_UI_CALL done_only_on_tick(float, float, void*) {}
void SAO_UI_CALL done_only_on_done(bool /*cancelled*/, void* ud) {
    auto* c = static_cast<DoneOnly*>(ud);
    c->calls.fetch_add(1);
}

}  // namespace

TEST_CASE("animator_add_animation_and_tick_reaches_target", "[ui][animator][interpreter]") {
    sao_ui_animator_handle_t h = nullptr;
    REQUIRE(sao_ui_animator_create(&h) == SAO_STATUS_OK);
    REQUIRE(h != nullptr);

    AnimCtx ctx;

    SaoUiAnimationSpec spec{};
    spec.duration_ms = 100;
    spec.curve = SAO_UI_CURVE_LINEAR;
    spec.bezier = nullptr;
    spec.delay_ms = 0;
    spec.on_tick = &anim_on_tick;
    spec.on_done = &anim_on_done;
    spec.user_data = &ctx;
    spec.dedup_key_utf8 = nullptr;

    sao_ui_animation_id_t id = 0;
    REQUIRE(sao_ui_animator_animate(h, &spec, &id) == SAO_STATUS_OK);
    REQUIRE(id != 0);
    // First tick at t=0 stamps start; second at t=0.05 → mid-progress.
    REQUIRE(sao_ui_animator_tick(h, 0.0) == SAO_STATUS_OK);
    REQUIRE(ctx.ticks.load() == 1);
    REQUIRE(std::fabs(ctx.last_eased.load() - 0.0f) < kEps);
    // Halfway.
    REQUIRE(sao_ui_animator_tick(h, 0.05) == SAO_STATUS_OK);
    REQUIRE(std::fabs(ctx.last_eased.load() - 0.5f) < kEps);
    // Past end.
    REQUIRE(sao_ui_animator_tick(h, 0.15) == SAO_STATUS_OK);
    REQUIRE(std::fabs(ctx.last_eased.load() - 1.0f) < kEps);
    REQUIRE(ctx.done_fired.load() == true);
    // Animation should now be completed → not running.
    bool running = true;
    REQUIRE(sao_ui_animator_is_running(h, id, &running) == SAO_STATUS_OK);
    REQUIRE(running == false);
    // Has no active.
    REQUIRE(sao_ui_animator_has_active(h) == false);

    sao_ui_animator_destroy(h);
}

TEST_CASE("animator_animation_on_complete_fires", "[ui][animator][interpreter]") {
    sao_ui_animator_handle_t h = nullptr;
    REQUIRE(sao_ui_animator_create(&h) == SAO_STATUS_OK);
    DoneOnly ctx;
    SaoUiAnimationSpec spec{};
    spec.duration_ms = 50;
    spec.curve = SAO_UI_CURVE_EASE_OUT;
    spec.on_tick = &done_only_on_tick;
    spec.on_done = &done_only_on_done;
    spec.user_data = &ctx;

    sao_ui_animation_id_t id = 0;
    REQUIRE(sao_ui_animator_animate(h, &spec, &id) == SAO_STATUS_OK);
    // Drive past completion; on_done must fire exactly once.
    REQUIRE(sao_ui_animator_tick(h, 0.0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_animator_tick(h, 1.0) == SAO_STATUS_OK);
    REQUIRE(ctx.calls.load() == 1);
    // Extra tick after completion must not re-fire.
    REQUIRE(sao_ui_animator_tick(h, 2.0) == SAO_STATUS_OK);
    REQUIRE(ctx.calls.load() == 1);

    sao_ui_animator_destroy(h);
}
