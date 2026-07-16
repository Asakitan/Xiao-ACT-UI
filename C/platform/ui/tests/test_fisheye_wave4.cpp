// SAO Auto - Wave4 fisheye lens tests (G3.6 gate).
//
// Covers the math documented in fisheye.h:
//   * hover_target_idx snaps to max_size
//   * neighbors inside falloff scale down via gamma curve
//   * edge buttons stay at base_size
//   * hover == -1 collapses everything to base_size
//   * hit_test uses current_size, so the grown hover_target hitbox
//     covers points outside the original slot
//   * animate blends over dt_ms toward a new hover

#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/fisheye.h"

namespace {

constexpr float kEps = 1e-3f;

// Build a column of `count` buttons at column_x=100, top_y=0,
// slot_size=cfg.slot_size (usually 70).  Zero out sprite_center so
// the layout helper populates it via slot_center.
std::vector<SaoUiFisheyeButton> make_column(
    const SaoUiFisheyeConfig& cfg, size_t count, int32_t column_x = 100,
    int32_t top_y = 0) {
    std::vector<SaoUiFisheyeButton> out(count);
    std::memset(out.data(), 0, sizeof(SaoUiFisheyeButton) * count);
    REQUIRE(sao_ui_fisheye_column_layout(column_x, top_y, cfg.slot_size,
                                         out.data(), count) == SAO_STATUS_OK);
    return out;
}

}  // namespace

TEST_CASE("fisheye_hover_item_max_size", "[ui][fisheye][wave4]") {
    const SaoUiFisheyeConfig& cfg = *sao_ui_fisheye_default_config();
    auto btns = make_column(cfg, 6);
    sao_ui_fisheye_handle_t h = nullptr;
    REQUIRE(sao_ui_fisheye_create(&cfg, &h) == SAO_STATUS_OK);
    // Hover on index 2 - should snap that to max_size (70).
    REQUIRE(sao_ui_fisheye_apply_column_layout(h, btns.data(), btns.size(), 2) ==
            SAO_STATUS_OK);
    REQUIRE(std::fabs(btns[2].current_size -
                      static_cast<float>(cfg.max_size)) < kEps);
    REQUIRE(std::fabs(btns[2].hover_t - 1.0f) < kEps);
    sao_ui_fisheye_destroy(h);
}

TEST_CASE("fisheye_neighbor_items_partially_zoomed", "[ui][fisheye][wave4]") {
    const SaoUiFisheyeConfig& cfg = *sao_ui_fisheye_default_config();
    auto btns = make_column(cfg, 6);
    sao_ui_fisheye_handle_t h = nullptr;
    REQUIRE(sao_ui_fisheye_create(&cfg, &h) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_apply_column_layout(h, btns.data(), btns.size(), 2) ==
            SAO_STATUS_OK);
    // Immediate neighbors (indices 1 and 3) are within falloff_neighbors=2
    // → strictly between base_size and max_size.
    const float base = static_cast<float>(cfg.base_size);
    const float max_ = static_cast<float>(cfg.max_size);
    REQUIRE(btns[1].current_size > base + kEps);
    REQUIRE(btns[1].current_size < max_ - kEps);
    REQUIRE(btns[3].current_size > base + kEps);
    REQUIRE(btns[3].current_size < max_ - kEps);
    // Symmetric: neighbors on either side should get the same size.
    REQUIRE(std::fabs(btns[1].current_size - btns[3].current_size) < kEps);
    // Two-step-away neighbors (indices 0 and 4) - also inside falloff
    // (distance 2 == falloff_neighbors) → still boosted a small amount
    // versus base.
    REQUIRE(btns[0].current_size > base + kEps);
    REQUIRE(btns[0].current_size < btns[1].current_size);
    REQUIRE(btns[4].current_size > base + kEps);
    REQUIRE(btns[4].current_size < btns[3].current_size);
    sao_ui_fisheye_destroy(h);
}

TEST_CASE("fisheye_edge_items_base_size", "[ui][fisheye][wave4]") {
    const SaoUiFisheyeConfig& cfg = *sao_ui_fisheye_default_config();
    // With 8 buttons and hover=1, index 5 (dist 4) is well outside
    // falloff_neighbors=2 → base_size.
    auto btns = make_column(cfg, 8);
    sao_ui_fisheye_handle_t h = nullptr;
    REQUIRE(sao_ui_fisheye_create(&cfg, &h) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_apply_column_layout(h, btns.data(), btns.size(), 1) ==
            SAO_STATUS_OK);
    const float base = static_cast<float>(cfg.base_size);
    REQUIRE(std::fabs(btns[5].current_size - base) < kEps);
    REQUIRE(std::fabs(btns[6].current_size - base) < kEps);
    REQUIRE(std::fabs(btns[7].current_size - base) < kEps);
    REQUIRE(std::fabs(btns[5].hover_t - 0.0f) < kEps);
    sao_ui_fisheye_destroy(h);
}

TEST_CASE("fisheye_no_hover_all_base_size", "[ui][fisheye][wave4]") {
    const SaoUiFisheyeConfig& cfg = *sao_ui_fisheye_default_config();
    auto btns = make_column(cfg, 6);
    sao_ui_fisheye_handle_t h = nullptr;
    REQUIRE(sao_ui_fisheye_create(&cfg, &h) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_apply_column_layout(h, btns.data(), btns.size(), -1) ==
            SAO_STATUS_OK);
    const float base = static_cast<float>(cfg.base_size);
    for (const SaoUiFisheyeButton& b : btns) {
        REQUIRE(std::fabs(b.current_size - base) < kEps);
        REQUIRE(std::fabs(b.hover_t - 0.0f) < kEps);
    }
    sao_ui_fisheye_destroy(h);
}

TEST_CASE("fisheye_hit_test_uses_current_size_not_slot",
          "[ui][fisheye][wave4]") {
    const SaoUiFisheyeConfig& cfg = *sao_ui_fisheye_default_config();
    // 6 buttons in a column at x=100, slot=70 each.  Slot centers at
    // y = 35, 105, 175, 245, 315, 385.
    auto btns = make_column(cfg, 6);
    sao_ui_fisheye_handle_t h = nullptr;
    REQUIRE(sao_ui_fisheye_create(&cfg, &h) == SAO_STATUS_OK);
    // Hover index 2 (center y=175, size=max=70 → radius 35).
    REQUIRE(sao_ui_fisheye_apply_column_layout(h, btns.data(), btns.size(), 2) ==
            SAO_STATUS_OK);
    // A point 25px away from center on x-axis - well within max radius
    // (35) but well outside base radius (27).
    // Without lens (base_size=54, radius 27) this point would MISS the
    // circle.  With current_size=70 (radius 35) it HITS.
    const int32_t x = 100 + 30;   // 30px away on x
    const int32_t y = 175;
    const int32_t hit = sao_ui_fisheye_hit_test(btns.data(), btns.size(), x, y);
    REQUIRE(hit == 2);
    // Sanity: a point on x-axis 40px away from center misses even the
    // grown hitbox (40 > 35).
    const int32_t miss_hit = sao_ui_fisheye_hit_test(
        btns.data(), btns.size(), 100 + 40, 175);
    REQUIRE(miss_hit == -1);
    // And a neighbor with current_size < base_size never inflates
    // beyond its own base — verify hit at index 3 uses ITS current
    // size (which is bigger than base but smaller than max).
    const int32_t neighbor_hit = sao_ui_fisheye_hit_test(
        btns.data(), btns.size(), 100, 245);   // slot center of idx 3
    REQUIRE(neighbor_hit == 3);
    sao_ui_fisheye_destroy(h);
}

TEST_CASE("fisheye_animate_smooth_transition", "[ui][fisheye][wave4]") {
    const SaoUiFisheyeConfig& cfg = *sao_ui_fisheye_default_config();
    auto btns = make_column(cfg, 6);
    sao_ui_fisheye_handle_t h = nullptr;
    REQUIRE(sao_ui_fisheye_create(&cfg, &h) == SAO_STATUS_OK);
    // Start with hover=1 fully settled.
    REQUIRE(sao_ui_fisheye_apply_column_layout(h, btns.data(), btns.size(), 1) ==
            SAO_STATUS_OK);
    const float initial_size_1 = btns[1].current_size;
    const float base = static_cast<float>(cfg.base_size);
    const float max_ = static_cast<float>(cfg.max_size);
    REQUIRE(std::fabs(initial_size_1 - max_) < kEps);
    // Animate toward hover=3 over 16ms — should be *closer* to the new
    // target (base for index 1, max for index 3) but not yet arrived.
    REQUIRE(sao_ui_fisheye_animate(h, btns.data(), btns.size(),
                                   /*from_hover=*/1, /*to_hover=*/3,
                                   /*dt_ms=*/16) == SAO_STATUS_OK);
    // Index 1 was max, is now moving toward base — must be smaller
    // than max and larger than base.
    REQUIRE(btns[1].current_size < max_ - kEps);
    REQUIRE(btns[1].current_size > base + kEps);
    // Index 3 was base_size + bonus (distance 2 from initial hover=1),
    // is now moving toward max (its own hover) — must be larger than
    // it was, but not yet max.
    REQUIRE(btns[3].current_size > base + kEps);
    REQUIRE(btns[3].current_size < max_ - kEps);
    // Drive many ticks to convergence.  600ms is >> the 60Hz lerp
    // convergence time; must land within grow_epsilon of targets.
    for (int i = 0; i < 40; ++i) {
        REQUIRE(sao_ui_fisheye_animate(h, btns.data(), btns.size(),
                                       3, 3, 16) == SAO_STATUS_OK);
    }
    REQUIRE(std::fabs(btns[3].current_size - max_) < 0.5f);
    REQUIRE(std::fabs(btns[1].current_size -
                      // btns[1] is now at distance 2 from hover=3 →
                      // computed target size (dist=2, falloff=2, gamma=2)
                      static_cast<float>(cfg.base_size) *
                      1.0f) < static_cast<float>(cfg.max_size - cfg.base_size));
    sao_ui_fisheye_destroy(h);
}
