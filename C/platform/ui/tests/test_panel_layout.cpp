// SAO Auto — panel-layout mode, hit-test, and dirty-propagation tests.
//
// Covers the 6 layout modes (vertical/horizontal/grid/dock/absolute/
// flex) and the hit-test + dirty propagation basics.  Leaf widgets
// are added with a fixed size via SaoUiLayoutSpec.fixed_* so we don't
// require the (still-stub) widget size_hint plumbing.

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/panel_layout.h"

namespace {

SaoUiLayoutSpec make_spec_fixed(int32_t w, int32_t h) {
    SaoUiLayoutSpec s{};
    sao_ui_layout_spec_defaults(&s);
    s.fixed_width_px = w;
    s.fixed_height_px = h;
    return s;
}

SaoUiLayoutSpec make_spec_weight(float weight) {
    SaoUiLayoutSpec s{};
    sao_ui_layout_spec_defaults(&s);
    s.weight = weight;
    return s;
}

// Sentinel widget handle values — the layout engine only stores them
// as opaque pointers; nothing dereferences them.
sao_ui_widget_handle_t sentinel_widget(uintptr_t id) {
    return reinterpret_cast<sao_ui_widget_handle_t>(id);
}

void check_rect_nonnegative_and_within(const SaoUiRect& rect, const SaoUiRect& parent) {
    CHECK(rect.x_px >= 0);
    CHECK(rect.y_px >= 0);
    CHECK(rect.width_px >= 0);
    CHECK(rect.height_px >= 0);
    CHECK(static_cast<int64_t>(rect.x_px) + rect.width_px <= static_cast<int64_t>(parent.x_px) + parent.width_px);
    CHECK(static_cast<int64_t>(rect.y_px) + rect.height_px <= static_cast<int64_t>(parent.y_px) + parent.height_px);
}

}  // namespace

TEST_CASE("layout_vertical_distributes_weight_equally", "[ui][layout][interpreter]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_VERTICAL, &root_spec, &root) == SAO_STATUS_OK);

    // Two weighted children, weight 1.0 each.  Arrange them into a
    // 100x300 rect — each should get ~150 height.
    SaoUiLayoutSpec ws = make_spec_weight(1.0f);
    sao_ui_layout_node_handle_t a = nullptr;
    sao_ui_layout_node_handle_t b = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &ws, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &ws, &b) == SAO_STATUS_OK);

    SaoUiSize measured{0, 0};
    REQUIRE(sao_ui_layout_measure(root, {100, 300}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 100, 300}) == SAO_STATUS_OK);
    SaoUiRect ra{}, rb{};
    REQUIRE(sao_ui_layout_node_get_rect(a, &ra) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(b, &rb) == SAO_STATUS_OK);
    // Heights should be ~equal (within 1px for rounding).
    REQUIRE(std::abs(ra.height_px - rb.height_px) <= 1);
    // Total should equal parent height.
    REQUIRE(ra.height_px + rb.height_px == 300);
    // Ordered top-down.
    REQUIRE(ra.y_px == 0);
    REQUIRE(rb.y_px == ra.height_px);

    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_horizontal_reduces_width_by_gap", "[ui][layout][interpreter]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    root_spec.gap_px = 10;
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_HORIZONTAL, &root_spec, &root) == SAO_STATUS_OK);
    // Three fixed 50-wide widgets in a 200-wide row (leaves 50 leftover
    // for gaps — the 2 gaps consume 20 total, 30 free).
    SaoUiLayoutSpec fs = make_spec_fixed(50, 30);
    sao_ui_layout_node_handle_t a = nullptr, b = nullptr, c = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &fs, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &fs, &b) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(3), &fs, &c) == SAO_STATUS_OK);
    SaoUiSize measured{0, 0};
    REQUIRE(sao_ui_layout_measure(root, {200, 30}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 200, 30}) == SAO_STATUS_OK);
    SaoUiRect ra{}, rb{}, rc{};
    REQUIRE(sao_ui_layout_node_get_rect(a, &ra) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(b, &rb) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(c, &rc) == SAO_STATUS_OK);
    // Gaps applied between consecutive rects: b.x - (a.x + a.w) == 10.
    REQUIRE(rb.x_px - (ra.x_px + ra.width_px) == 10);
    REQUIRE(rc.x_px - (rb.x_px + rb.width_px) == 10);
    // All widths preserved.
    REQUIRE(ra.width_px == 50);
    REQUIRE(rb.width_px == 50);
    REQUIRE(rc.width_px == 50);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_weighted_allocation_reallocates_after_max_constraint",
          "[ui][layout][interpreter][constraints]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_HORIZONTAL, &root_spec, &root) ==
            SAO_STATUS_OK);
    SaoUiLayoutSpec capped = make_spec_weight(1.0F);
    capped.max_width_px = 10;
    SaoUiLayoutSpec weighted = make_spec_weight(1.0F);
    sao_ui_layout_node_handle_t a = nullptr;
    sao_ui_layout_node_handle_t b = nullptr;
    sao_ui_layout_node_handle_t c = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &capped, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &weighted, &b) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(3), &weighted, &c) == SAO_STATUS_OK);
    SaoUiSize measured{};
    REQUIRE(sao_ui_layout_measure(root, {100, 20}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 100, 20}) == SAO_STATUS_OK);
    SaoUiRect ra{}, rb{}, rc{};
    REQUIRE(sao_ui_layout_node_get_rect(a, &ra) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(b, &rb) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(c, &rc) == SAO_STATUS_OK);
    CHECK(ra.width_px == 10);
    CHECK(rb.width_px == 45);
    CHECK(rc.width_px == 45);
    CHECK(rc.x_px + rc.width_px == 100);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_weighted_minimum_overflow_is_bounded",
          "[ui][layout][interpreter][constraints]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_VERTICAL, &root_spec, &root) ==
            SAO_STATUS_OK);
    SaoUiLayoutSpec first = make_spec_weight(1.0F);
    first.min_height_px = 80;
    SaoUiLayoutSpec second = make_spec_weight(1.0F);
    second.min_height_px = 80;
    sao_ui_layout_node_handle_t a = nullptr;
    sao_ui_layout_node_handle_t b = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &first, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &second, &b) == SAO_STATUS_OK);
    SaoUiSize measured{};
    REQUIRE(sao_ui_layout_measure(root, {40, 100}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 40, 100}) == SAO_STATUS_OK);
    SaoUiRect ra{}, rb{};
    REQUIRE(sao_ui_layout_node_get_rect(a, &ra) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(b, &rb) == SAO_STATUS_OK);
    CHECK(ra.height_px == 50);
    CHECK(rb.height_px == 50);
    CHECK(rb.y_px == 50);
    CHECK(ra.height_px + rb.height_px == 100);
    CHECK(rb.y_px + rb.height_px == 100);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_horizontal_fixed_main_size_precedes_weighted_distribution",
          "[ui][layout][interpreter][constraints]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_HORIZONTAL, &root_spec, &root) ==
            SAO_STATUS_OK);

    SaoUiLayoutSpec fixed = make_spec_fixed(20, 10);
    SaoUiLayoutSpec first = make_spec_weight(1.0F);
    SaoUiLayoutSpec second = make_spec_weight(2.0F);
    sao_ui_layout_node_handle_t a = nullptr;
    sao_ui_layout_node_handle_t b = nullptr;
    sao_ui_layout_node_handle_t c = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &fixed, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &first, &b) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(3), &second, &c) == SAO_STATUS_OK);

    SaoUiSize measured{};
    REQUIRE(sao_ui_layout_measure(root, {100, 20}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 100, 20}) == SAO_STATUS_OK);
    SaoUiRect ra{}, rb{}, rc{};
    REQUIRE(sao_ui_layout_node_get_rect(a, &ra) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(b, &rb) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(c, &rc) == SAO_STATUS_OK);
    CHECK(ra.width_px == 20);
    CHECK(rb.width_px == 27);
    CHECK(rc.width_px == 53);
    CHECK(rb.x_px == 20);
    CHECK(rc.x_px == rb.x_px + rb.width_px);
    CHECK(rc.x_px + rc.width_px == 100);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_vertical_max_freezes_and_redistributes_remaining_space",
          "[ui][layout][interpreter][constraints]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_VERTICAL, &root_spec, &root) ==
            SAO_STATUS_OK);

    SaoUiLayoutSpec capped = make_spec_weight(1.0F);
    capped.max_height_px = 10;
    SaoUiLayoutSpec weighted = make_spec_weight(1.0F);
    sao_ui_layout_node_handle_t a = nullptr;
    sao_ui_layout_node_handle_t b = nullptr;
    sao_ui_layout_node_handle_t c = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &capped, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &weighted, &b) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(3), &weighted, &c) == SAO_STATUS_OK);

    SaoUiSize measured{};
    REQUIRE(sao_ui_layout_measure(root, {20, 100}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 20, 100}) == SAO_STATUS_OK);
    SaoUiRect ra{}, rb{}, rc{};
    REQUIRE(sao_ui_layout_node_get_rect(a, &ra) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(b, &rb) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(c, &rc) == SAO_STATUS_OK);
    CHECK(ra.height_px == 10);
    CHECK(rb.height_px == 45);
    CHECK(rc.height_px == 45);
    CHECK(rb.y_px == 10);
    CHECK(rc.y_px == rb.y_px + rb.height_px);
    CHECK(rc.y_px + rc.height_px == 100);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_horizontal_minimum_overflow_is_deterministically_compressed",
          "[ui][layout][interpreter][constraints]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_HORIZONTAL, &root_spec, &root) ==
            SAO_STATUS_OK);

    SaoUiLayoutSpec first = make_spec_weight(1.0F);
    first.min_width_px = 80;
    SaoUiLayoutSpec second = make_spec_weight(1.0F);
    second.min_width_px = 40;
    SaoUiLayoutSpec third = make_spec_weight(1.0F);
    third.min_width_px = 40;
    sao_ui_layout_node_handle_t a = nullptr;
    sao_ui_layout_node_handle_t b = nullptr;
    sao_ui_layout_node_handle_t c = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &first, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &second, &b) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(3), &third, &c) == SAO_STATUS_OK);

    SaoUiSize measured{};
    REQUIRE(sao_ui_layout_measure(root, {100, 20}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 100, 20}) == SAO_STATUS_OK);
    SaoUiRect ra{}, rb{}, rc{};
    REQUIRE(sao_ui_layout_node_get_rect(a, &ra) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(b, &rb) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(c, &rc) == SAO_STATUS_OK);
    CHECK(ra.width_px == 50);
    CHECK(rb.width_px == 25);
    CHECK(rc.width_px == 25);
    CHECK(ra.width_px >= 0);
    CHECK(rb.width_px >= 0);
    CHECK(rc.width_px >= 0);
    CHECK(rc.x_px + rc.width_px == 100);
    sao_ui_layout_tree_destroy(tree);
}

    TEST_CASE("layout_grid_flex_tracks_use_exact_integer_remainders",
          "[ui][layout][grid][remainder]") {
        sao_ui_layout_tree_handle_t tree = nullptr;
        REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
        SaoUiLayoutSpec root_spec{};
        sao_ui_layout_spec_defaults(&root_spec);
        sao_ui_layout_node_handle_t root = nullptr;
        REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_GRID, &root_spec, &root) ==
            SAO_STATUS_OK);
        SaoUiTrackSize cols[2] = {
        {SAO_UI_TRACK_FLEX, 0, 1.0F, 0, {0, 0, 0, 0}},
        {SAO_UI_TRACK_FLEX, 0, 1.0F, 0, {0, 0, 0, 0}},
        };
        SaoUiTrackSize rows[1] = {
        {SAO_UI_TRACK_FIXED, 10, 0.0F, 0, {0, 0, 0, 0}},
        };
        SaoUiGridMode grid{};
        grid.rows = rows;
        grid.row_count = 1;
        grid.cols = cols;
        grid.col_count = 2;
        REQUIRE(sao_ui_layout_node_set_mode_config(root, &grid) == SAO_STATUS_OK);
        SaoUiLayoutSpec child_spec = make_spec_fixed(1, 10);
        sao_ui_layout_node_handle_t first = nullptr;
        sao_ui_layout_node_handle_t second = nullptr;
        REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &child_spec, &first) ==
            SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &child_spec, &second) ==
            SAO_STATUS_OK);
        SaoUiSize measured{};
        REQUIRE(sao_ui_layout_measure(root, {5, 10}, &measured) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_arrange(root, {0, 0, 5, 10}) == SAO_STATUS_OK);
        SaoUiRect a{}, b{};
        REQUIRE(sao_ui_layout_node_get_rect(first, &a) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_node_get_rect(second, &b) == SAO_STATUS_OK);
        CHECK(a.width_px == 3);
        CHECK(b.width_px == 2);
        CHECK(b.x_px == 3);
        CHECK(b.x_px + b.width_px == 5);
        sao_ui_layout_tree_destroy(tree);
    }

    TEST_CASE("layout_grid_ignores_non_finite_flex_weights",
          "[ui][layout][grid][weight][finite]") {
        sao_ui_layout_tree_handle_t tree = nullptr;
        REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
        SaoUiLayoutSpec root_spec{};
        sao_ui_layout_spec_defaults(&root_spec);
        sao_ui_layout_node_handle_t root = nullptr;
        REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_GRID, &root_spec, &root) ==
            SAO_STATUS_OK);
        SaoUiTrackSize cols[5] = {
        {SAO_UI_TRACK_FLEX, 0, std::numeric_limits<float>::quiet_NaN(), 0, {0, 0, 0, 0}},
        {SAO_UI_TRACK_FLEX, 0, std::numeric_limits<float>::infinity(), 0, {0, 0, 0, 0}},
            {SAO_UI_TRACK_FLEX, 0, -std::numeric_limits<float>::infinity(), 0, {0, 0, 0, 0}},
            {SAO_UI_TRACK_FLEX, 0, -1.0F, 0, {0, 0, 0, 0}},
        {SAO_UI_TRACK_FLEX, 0, 1.0F, 0, {0, 0, 0, 0}},
        };
        SaoUiTrackSize rows[1] = {
        {SAO_UI_TRACK_FIXED, 10, 0.0F, 0, {0, 0, 0, 0}},
        };
        SaoUiGridMode grid{};
        grid.rows = rows;
        grid.row_count = 1;
        grid.cols = cols;
        grid.col_count = 5;
        REQUIRE(sao_ui_layout_node_set_mode_config(root, &grid) == SAO_STATUS_OK);
        SaoUiLayoutSpec child_spec = make_spec_fixed(1, 10);
        sao_ui_layout_node_handle_t children[5]{};
        for (int index = 0; index < 5; ++index) {
        REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(index + 1), &child_spec,
                              &children[index]) == SAO_STATUS_OK);
        }
        SaoUiSize measured{};
        REQUIRE(sao_ui_layout_measure(root, {5, 10}, &measured) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_arrange(root, {0, 0, 5, 10}) == SAO_STATUS_OK);
        for (int index = 0; index < 4; ++index) {
            SaoUiRect invalid{};
            REQUIRE(sao_ui_layout_node_get_rect(children[index], &invalid) == SAO_STATUS_OK);
            CHECK(invalid.width_px == 0);
        }
        SaoUiRect valid{};
        REQUIRE(sao_ui_layout_node_get_rect(children[4], &valid) == SAO_STATUS_OK);
        CHECK(valid.width_px == 5);
        CHECK(valid.x_px + valid.width_px == 5);
        sao_ui_layout_tree_destroy(tree);
    }

    TEST_CASE("layout_flex_growth_and_reverse_stay_inside_parent",
          "[ui][layout][flex][remainder][reverse]") {
        const auto verify = [](int32_t direction, int32_t expected_first_x,
                   int32_t expected_second_x) {
        sao_ui_layout_tree_handle_t tree = nullptr;
        REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
        SaoUiLayoutSpec root_spec{};
        sao_ui_layout_spec_defaults(&root_spec);
        sao_ui_layout_node_handle_t root = nullptr;
        REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_FLEX, &root_spec, &root) ==
            SAO_STATUS_OK);
        SaoUiFlexMode flex{};
        flex.direction = direction;
        flex.align_items = SAO_UI_ALIGN_AXIS_STRETCH;
        REQUIRE(sao_ui_layout_node_set_mode_config(root, &flex) == SAO_STATUS_OK);
        SaoUiLayoutSpec child_spec = make_spec_fixed(100, 10);
        child_spec.weight = 1.0F;
        sao_ui_layout_node_handle_t first = nullptr;
        sao_ui_layout_node_handle_t second = nullptr;
        REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &child_spec, &first) ==
            SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &child_spec, &second) ==
            SAO_STATUS_OK);
        SaoUiSize measured{};
        REQUIRE(sao_ui_layout_measure(root, {205, 10}, &measured) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_arrange(root, {0, 0, 205, 10}) == SAO_STATUS_OK);
        SaoUiRect a{}, b{};
        REQUIRE(sao_ui_layout_node_get_rect(first, &a) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_node_get_rect(second, &b) == SAO_STATUS_OK);
        CHECK(a.width_px == 103);
        CHECK(b.width_px == 102);
        CHECK(a.x_px == expected_first_x);
        CHECK(b.x_px == expected_second_x);
        CHECK(a.x_px >= 0);
        CHECK(b.x_px >= 0);
        CHECK(a.x_px + a.width_px <= 205);
        CHECK(b.x_px + b.width_px <= 205);
        sao_ui_layout_tree_destroy(tree);
        };
        verify(SAO_UI_FLEX_ROW, 0, 103);
        verify(SAO_UI_FLEX_ROW_REVERSE, 102, 0);
    }

        TEST_CASE("layout_flex_column_reverse_places_first_child_at_the_end",
              "[ui][layout][flex][column][reverse]") {
        sao_ui_layout_tree_handle_t tree = nullptr;
        REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
        SaoUiLayoutSpec root_spec{};
        sao_ui_layout_spec_defaults(&root_spec);
        sao_ui_layout_node_handle_t root = nullptr;
            REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_FLEX, &root_spec, &root) ==
            SAO_STATUS_OK);
            SaoUiFlexMode flex{};
            flex.direction = SAO_UI_FLEX_COLUMN_REVERSE;
            flex.align_items = SAO_UI_ALIGN_AXIS_STRETCH;
            REQUIRE(sao_ui_layout_node_set_mode_config(root, &flex) == SAO_STATUS_OK);
            SaoUiLayoutSpec child_spec = make_spec_fixed(10, 100);
            child_spec.weight = 1.0F;
        sao_ui_layout_node_handle_t first = nullptr;
        sao_ui_layout_node_handle_t second = nullptr;
            REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &child_spec, &first) ==
            SAO_STATUS_OK);
            REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &child_spec, &second) ==
            SAO_STATUS_OK);
        SaoUiSize measured{};
            REQUIRE(sao_ui_layout_measure(root, {10, 205}, &measured) == SAO_STATUS_OK);
            REQUIRE(sao_ui_layout_arrange(root, {0, 0, 10, 205}) == SAO_STATUS_OK);
        SaoUiRect a{}, b{};
        REQUIRE(sao_ui_layout_node_get_rect(first, &a) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_node_get_rect(second, &b) == SAO_STATUS_OK);
            CHECK(a.height_px == 103);
            CHECK(b.height_px == 102);
            CHECK(a.y_px == 102);
            CHECK(b.y_px == 0);
            CHECK(a.y_px + a.height_px <= 205);
            CHECK(b.y_px + b.height_px <= 205);
        sao_ui_layout_tree_destroy(tree);
    }

        TEST_CASE("layout_weighted_and_flex_ignore_non_finite_weights",
              "[ui][layout][weight][finite]") {
            const auto verify_horizontal = [](float invalid_weight) {
            sao_ui_layout_tree_handle_t tree = nullptr;
            REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
            SaoUiLayoutSpec root_spec{};
            sao_ui_layout_spec_defaults(&root_spec);
            sao_ui_layout_node_handle_t root = nullptr;
            REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_HORIZONTAL, &root_spec, &root) ==
                SAO_STATUS_OK);
            SaoUiLayoutSpec invalid = make_spec_weight(invalid_weight);
            SaoUiLayoutSpec valid = make_spec_weight(1.0F);
            sao_ui_layout_node_handle_t first = nullptr;
            sao_ui_layout_node_handle_t second = nullptr;
            REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &invalid, &first) ==
                SAO_STATUS_OK);
            REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &valid, &second) ==
                SAO_STATUS_OK);
            SaoUiSize measured{};
            REQUIRE(sao_ui_layout_measure(root, {200, 20}, &measured) == SAO_STATUS_OK);
            REQUIRE(sao_ui_layout_arrange(root, {0, 0, 200, 20}) == SAO_STATUS_OK);
            SaoUiRect a{}, b{};
            REQUIRE(sao_ui_layout_node_get_rect(first, &a) == SAO_STATUS_OK);
            REQUIRE(sao_ui_layout_node_get_rect(second, &b) == SAO_STATUS_OK);
            CHECK(a.width_px == 100);
            CHECK(b.width_px == 100);
            CHECK(b.x_px + b.width_px == 200);
            sao_ui_layout_tree_destroy(tree);
            };
            verify_horizontal(std::numeric_limits<float>::quiet_NaN());
            verify_horizontal(std::numeric_limits<float>::infinity());
            verify_horizontal(-std::numeric_limits<float>::infinity());
            verify_horizontal(-1.0F);

            sao_ui_layout_tree_handle_t tree = nullptr;
            REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
            SaoUiLayoutSpec root_spec{};
            sao_ui_layout_spec_defaults(&root_spec);
            sao_ui_layout_node_handle_t root = nullptr;
            REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_FLEX, &root_spec, &root) ==
                SAO_STATUS_OK);
            SaoUiFlexMode flex{};
            flex.direction = SAO_UI_FLEX_ROW;
            flex.align_items = SAO_UI_ALIGN_AXIS_STRETCH;
            REQUIRE(sao_ui_layout_node_set_mode_config(root, &flex) == SAO_STATUS_OK);
            const float weights[5] = {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            -1.0F,
            1.0F,
            };
            sao_ui_layout_node_handle_t children[5]{};
            for (int index = 0; index < 5; ++index) {
            SaoUiLayoutSpec spec = make_spec_fixed(100, 10);
            spec.weight = weights[index];
            REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(index + 1), &spec,
                                  &children[index]) == SAO_STATUS_OK);
            }
            SaoUiSize measured{};
            REQUIRE(sao_ui_layout_measure(root, {505, 10}, &measured) == SAO_STATUS_OK);
            REQUIRE(sao_ui_layout_arrange(root, {0, 0, 505, 10}) == SAO_STATUS_OK);
            for (int index = 0; index < 5; ++index) {
            SaoUiRect rect{};
            REQUIRE(sao_ui_layout_node_get_rect(children[index], &rect) == SAO_STATUS_OK);
            CHECK(rect.width_px == (index == 4 ? 105 : 100));
            CHECK(rect.x_px + rect.width_px <= 505);
            }
            sao_ui_layout_tree_destroy(tree);
        }

TEST_CASE("layout_grid_positions_cells", "[ui][layout][interpreter]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_GRID, &root_spec, &root) == SAO_STATUS_OK);

    // 2x2 grid, all cells fixed 40 wide, 30 tall.
    SaoUiTrackSize cols[2] = {
        {SAO_UI_TRACK_FIXED, 40, 0.0f, 0, {0,0,0,0}},
        {SAO_UI_TRACK_FIXED, 40, 0.0f, 0, {0,0,0,0}},
    };
    SaoUiTrackSize rows[2] = {
        {SAO_UI_TRACK_FIXED, 30, 0.0f, 0, {0,0,0,0}},
        {SAO_UI_TRACK_FIXED, 30, 0.0f, 0, {0,0,0,0}},
    };
    SaoUiGridMode gm{};
    gm.rows = rows; gm.row_count = 2;
    gm.cols = cols; gm.col_count = 2;
    gm.row_gap_px = 0;
    gm.col_gap_px = 0;
    REQUIRE(sao_ui_layout_node_set_mode_config(root, &gm) == SAO_STATUS_OK);

    SaoUiLayoutSpec cell_spec = make_spec_fixed(40, 30);
    sao_ui_layout_node_handle_t cells[4] = {nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < 4; ++i) {
        REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1 + i), &cell_spec, &cells[i]) == SAO_STATUS_OK);
    }
    SaoUiSize measured{0, 0};
    REQUIRE(sao_ui_layout_measure(root, {80, 60}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 80, 60}) == SAO_STATUS_OK);
    SaoUiRect r{};
    REQUIRE(sao_ui_layout_node_get_rect(cells[0], &r) == SAO_STATUS_OK);
    REQUIRE(r.x_px == 0);   REQUIRE(r.y_px == 0);
    REQUIRE(sao_ui_layout_node_get_rect(cells[1], &r) == SAO_STATUS_OK);
    REQUIRE(r.x_px == 40);  REQUIRE(r.y_px == 0);
    REQUIRE(sao_ui_layout_node_get_rect(cells[2], &r) == SAO_STATUS_OK);
    REQUIRE(r.x_px == 0);   REQUIRE(r.y_px == 30);
    REQUIRE(sao_ui_layout_node_get_rect(cells[3], &r) == SAO_STATUS_OK);
    REQUIRE(r.x_px == 40);  REQUIRE(r.y_px == 30);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_dock_top_takes_available_width", "[ui][layout][interpreter]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_DOCK, &root_spec, &root) == SAO_STATUS_OK);
    SaoUiDockMode dm{};
    dm.last_child_fills = true;
    REQUIRE(sao_ui_layout_node_set_mode_config(root, &dm) == SAO_STATUS_OK);

    // Top-docked child, height=40 → occupies full width.
    SaoUiLayoutSpec top_spec = make_spec_fixed(0, 40);
    top_spec.dock_side = SAO_UI_DOCK_TOP;
    sao_ui_layout_node_handle_t top = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &top_spec, &top) == SAO_STATUS_OK);

    SaoUiSize measured{0, 0};
    REQUIRE(sao_ui_layout_measure(root, {200, 100}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 200, 100}) == SAO_STATUS_OK);
    SaoUiRect r{};
    REQUIRE(sao_ui_layout_node_get_rect(top, &r) == SAO_STATUS_OK);
    REQUIRE(r.x_px == 0);
    REQUIRE(r.y_px == 0);
    REQUIRE(r.width_px == 200);   // dock TOP spans full parent width
    REQUIRE(r.height_px == 40);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_dock_center_fills_remaining", "[ui][layout][interpreter]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_DOCK, &root_spec, &root) == SAO_STATUS_OK);
    SaoUiDockMode dm{}; dm.last_child_fills = true;
    REQUIRE(sao_ui_layout_node_set_mode_config(root, &dm) == SAO_STATUS_OK);

    // top=30, bottom=20, center fills.
    SaoUiLayoutSpec top_spec = make_spec_fixed(0, 30);
    top_spec.dock_side = SAO_UI_DOCK_TOP;
    sao_ui_layout_node_handle_t top = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &top_spec, &top) == SAO_STATUS_OK);
    SaoUiLayoutSpec bot_spec = make_spec_fixed(0, 20);
    bot_spec.dock_side = SAO_UI_DOCK_BOTTOM;
    sao_ui_layout_node_handle_t bot = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &bot_spec, &bot) == SAO_STATUS_OK);
    SaoUiLayoutSpec ct_spec{};
    sao_ui_layout_spec_defaults(&ct_spec);
    ct_spec.dock_side = SAO_UI_DOCK_CENTER;
    sao_ui_layout_node_handle_t ct = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(3), &ct_spec, &ct) == SAO_STATUS_OK);

    SaoUiSize measured{0, 0};
    REQUIRE(sao_ui_layout_measure(root, {200, 100}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 200, 100}) == SAO_STATUS_OK);
    SaoUiRect r{};
    REQUIRE(sao_ui_layout_node_get_rect(ct, &r) == SAO_STATUS_OK);
    // Center = between top(30) and bottom(20) → y=30, h=50.
    REQUIRE(r.x_px == 0);
    REQUIRE(r.y_px == 30);
    REQUIRE(r.width_px == 200);
    REQUIRE(r.height_px == 50);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_absolute_uses_child_spec_pos", "[ui][layout][interpreter]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_ABSOLUTE, &root_spec, &root) == SAO_STATUS_OK);
    SaoUiAbsoluteMode am{};
    REQUIRE(sao_ui_layout_node_set_mode_config(root, &am) == SAO_STATUS_OK);

    SaoUiLayoutSpec cs = make_spec_fixed(50, 40);
    cs.absolute_x_px = 25;
    cs.absolute_y_px = 60;
    sao_ui_layout_node_handle_t child = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &cs, &child) == SAO_STATUS_OK);

    SaoUiSize measured{0, 0};
    REQUIRE(sao_ui_layout_measure(root, {200, 200}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 200, 200}) == SAO_STATUS_OK);
    SaoUiRect r{};
    REQUIRE(sao_ui_layout_node_get_rect(child, &r) == SAO_STATUS_OK);
    REQUIRE(r.x_px == 25);
    REQUIRE(r.y_px == 60);
    REQUIRE(r.width_px == 50);
    REQUIRE(r.height_px == 40);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_hit_test_finds_leaf_widget", "[ui][layout][interpreter]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_HORIZONTAL, &root_spec, &root) == SAO_STATUS_OK);

    SaoUiLayoutSpec fs = make_spec_fixed(60, 40);
    sao_ui_layout_node_handle_t a = nullptr, b = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(0xAA), &fs, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(0xBB), &fs, &b) == SAO_STATUS_OK);

    SaoUiSize measured{0, 0};
    REQUIRE(sao_ui_layout_measure(root, {200, 40}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 200, 40}) == SAO_STATUS_OK);

    SaoUiHitResult hit{};
    REQUIRE(sao_ui_layout_hit_test(root, 30, 20, &hit) == SAO_STATUS_OK);
    REQUIRE(hit.node == a);
    REQUIRE(hit.widget == sentinel_widget(0xAA));

    // Point on second widget (starts at x=60).
    REQUIRE(sao_ui_layout_hit_test(root, 90, 20, &hit) == SAO_STATUS_OK);
    REQUIRE(hit.node == b);
    REQUIRE(hit.widget == sentinel_widget(0xBB));

    // Off-canvas → NOT_FOUND (or hit clears to null).
    hit.node = nullptr;
    hit.widget = nullptr;
    const sao_status_t st = sao_ui_layout_hit_test(root, -5, -5, &hit);
    REQUIRE((st == SAO_STATUS_ERR_NOT_FOUND || hit.node == nullptr));

    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_mark_dirty_propagates_up", "[ui][layout][interpreter]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_VERTICAL, &root_spec, &root) == SAO_STATUS_OK);
    SaoUiLayoutSpec cs{};
    sao_ui_layout_spec_defaults(&cs);
    sao_ui_layout_node_handle_t container_a = nullptr;
    REQUIRE(sao_ui_layout_node_add_container(root, SAO_UI_LAYOUT_HORIZONTAL, &cs, &container_a) == SAO_STATUS_OK);
    SaoUiLayoutSpec fs = make_spec_fixed(40, 40);
    sao_ui_layout_node_handle_t leaf = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(container_a, sentinel_widget(1), &fs, &leaf) == SAO_STATUS_OK);

    // Measure once so everyone is clean.
    SaoUiSize m{0, 0};
    REQUIRE(sao_ui_layout_measure(root, {200, 200}, &m) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 200, 200}) == SAO_STATUS_OK);

    // Invalidate leaf → root should be dirty too.  We can't peek at
    // the dirty flag directly, but changing the spec + re-arranging
    // must reflect the new size.  Change leaf to 80x80.
    SaoUiLayoutSpec fs2 = make_spec_fixed(80, 80);
    REQUIRE(sao_ui_layout_node_set_spec(leaf, &fs2) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_invalidate(leaf) == SAO_STATUS_OK);
    // Re-measure/arrange picks up the change.
    REQUIRE(sao_ui_layout_measure(root, {200, 200}, &m) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 200, 200}) == SAO_STATUS_OK);
    SaoUiRect r{};
    REQUIRE(sao_ui_layout_node_get_rect(leaf, &r) == SAO_STATUS_OK);
    REQUIRE(r.width_px == 80);
    REQUIRE(r.height_px == 80);

    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_dump_json_contains_tree_geometry", "[ui][layout][json]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    root_spec.gap_px = 6;
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(
        tree, SAO_UI_LAYOUT_VERTICAL, &root_spec, &root) == SAO_STATUS_OK);
    SaoUiLayoutSpec child_spec = make_spec_fixed(80, 32);
    sao_ui_layout_node_handle_t child = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(
        root, sentinel_widget(7), &child_spec, &child) == SAO_STATUS_OK);
    SaoUiSize measured{};
    REQUIRE(sao_ui_layout_measure(root, {200, 100}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {10, 20, 200, 100}) == SAO_STATUS_OK);

    size_t required = 0;
    CHECK(sao_ui_layout_dump_json(tree, nullptr, 0, &required) ==
          SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(required > 1);
    std::vector<uint8_t> json(required);
    REQUIRE(sao_ui_layout_dump_json(tree, json.data(), json.size(), &required) ==
            SAO_STATUS_OK);
    const std::string text(reinterpret_cast<const char*>(json.data()));
    CHECK(text.find("\"mode\": \"vertical\"") != std::string::npos);
    CHECK(text.find("\"width\": 200") != std::string::npos);
    CHECK(text.find("\"widget\": 7") != std::string::npos);
    sao_ui_layout_tree_destroy(tree);
}


TEST_CASE("layout_measure_extreme_values_saturate_for_all_modes",
          "[ui][layout][overflow][measure]") {
    const int32_t max_value = std::numeric_limits<int32_t>::max();
    const auto verify = [&](int32_t mode) {
        sao_ui_layout_tree_handle_t tree = nullptr;
        REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
        SaoUiLayoutSpec root_spec{};
        sao_ui_layout_spec_defaults(&root_spec);
        root_spec.gap_px = max_value;
        root_spec.pad_top_px = max_value;
        root_spec.pad_right_px = max_value;
        root_spec.pad_bottom_px = max_value;
        root_spec.pad_left_px = max_value;
        root_spec.margin_top_px = max_value;
        root_spec.margin_right_px = max_value;
        root_spec.margin_bottom_px = max_value;
        root_spec.margin_left_px = max_value;
        sao_ui_layout_node_handle_t root = nullptr;
        REQUIRE(sao_ui_layout_tree_set_root(tree, mode, &root_spec, &root) == SAO_STATUS_OK);
        if (mode == SAO_UI_LAYOUT_GRID) {
            SaoUiTrackSize cols[2] = {
                {SAO_UI_TRACK_FIXED, max_value, 0.0f, 0, {0, 0, 0, 0}},
                {SAO_UI_TRACK_FIXED, max_value, 0.0f, 0, {0, 0, 0, 0}},
            };
            SaoUiTrackSize rows[2] = {
                {SAO_UI_TRACK_FIXED, max_value, 0.0f, 0, {0, 0, 0, 0}},
                {SAO_UI_TRACK_FIXED, max_value, 0.0f, 0, {0, 0, 0, 0}},
            };
            SaoUiGridMode grid{};
            grid.rows = rows;
            grid.row_count = 2;
            grid.cols = cols;
            grid.col_count = 2;
            grid.row_gap_px = max_value;
            grid.col_gap_px = max_value;
            REQUIRE(sao_ui_layout_node_set_mode_config(root, &grid) == SAO_STATUS_OK);
        } else if (mode == SAO_UI_LAYOUT_FLEX) {
            SaoUiFlexMode flex{};
            flex.direction = SAO_UI_FLEX_ROW;
            flex.gap_main_px = max_value;
            REQUIRE(sao_ui_layout_node_set_mode_config(root, &flex) == SAO_STATUS_OK);
        }
        SaoUiLayoutSpec fixed = make_spec_fixed(max_value, max_value);
        fixed.min_width_px = max_value;
        fixed.min_height_px = max_value;
        sao_ui_layout_node_handle_t first = nullptr;
        sao_ui_layout_node_handle_t second = nullptr;
        REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &fixed, &first) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &fixed, &second) == SAO_STATUS_OK);
        if (mode == SAO_UI_LAYOUT_ABSOLUTE) {
            SaoUiLayoutSpec positioned = fixed;
            positioned.absolute_x_px = max_value;
            positioned.absolute_y_px = max_value;
            REQUIRE(sao_ui_layout_node_set_spec(first, &positioned) == SAO_STATUS_OK);
        } else if (mode == SAO_UI_LAYOUT_DOCK) {
            SaoUiLayoutSpec top = fixed;
            top.dock_side = SAO_UI_DOCK_TOP;
            SaoUiLayoutSpec left = fixed;
            left.dock_side = SAO_UI_DOCK_LEFT;
            REQUIRE(sao_ui_layout_node_set_spec(first, &top) == SAO_STATUS_OK);
            REQUIRE(sao_ui_layout_node_set_spec(second, &left) == SAO_STATUS_OK);
        }
        SaoUiSize preferred{};
        REQUIRE(sao_ui_layout_measure(root, {max_value, max_value}, &preferred) == SAO_STATUS_OK);
        CHECK(preferred.width_px == max_value);
        CHECK(preferred.height_px == max_value);
        sao_ui_layout_tree_destroy(tree);
    };
    verify(SAO_UI_LAYOUT_VERTICAL);
    verify(SAO_UI_LAYOUT_HORIZONTAL);
    verify(SAO_UI_LAYOUT_GRID);
    verify(SAO_UI_LAYOUT_ABSOLUTE);
    verify(SAO_UI_LAYOUT_FLEX);
    verify(SAO_UI_LAYOUT_DOCK);
}

TEST_CASE("layout_measure_many_children_and_extreme_minimums_do_not_wrap",
          "[ui][layout][overflow][arrange]") {
    const int32_t max_value = std::numeric_limits<int32_t>::max();
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    root_spec.gap_px = max_value;
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_VERTICAL, &root_spec, &root) == SAO_STATUS_OK);
    SaoUiLayoutSpec child_spec = make_spec_fixed(1, 1);
    child_spec.min_height_px = max_value;
    child_spec.weight = 1.0f;
    constexpr int child_count = 2048;
    std::vector<sao_ui_layout_node_handle_t> children;
    children.reserve(child_count);
    for (int index = 0; index < child_count; ++index) {
        sao_ui_layout_node_handle_t child = nullptr;
        REQUIRE(sao_ui_layout_node_add_widget(
                    root, sentinel_widget(static_cast<uintptr_t>(index + 1)), &child_spec, &child) ==
                SAO_STATUS_OK);
        children.push_back(child);
    }
    SaoUiSize preferred{};
    REQUIRE(sao_ui_layout_measure(root, {64, 64}, &preferred) == SAO_STATUS_OK);
    CHECK(preferred.width_px >= 0);
    CHECK(preferred.height_px == max_value);
    REQUIRE(sao_ui_layout_arrange(root, {-max_value, -max_value, 64, 64}) == SAO_STATUS_OK);
    for (const auto child : children) {
        SaoUiRect rect{};
        REQUIRE(sao_ui_layout_node_get_rect(child, &rect) == SAO_STATUS_OK);
        CHECK(rect.x_px >= 0);
        CHECK(rect.y_px >= 0);
        CHECK(rect.width_px >= 0);
        CHECK(rect.height_px >= 0);
        CHECK(static_cast<int64_t>(rect.x_px) + rect.width_px >= rect.x_px);
        CHECK(static_cast<int64_t>(rect.y_px) + rect.height_px >= rect.y_px);
    }
    sao_ui_layout_tree_destroy(tree);
}
TEST_CASE("layout_flex_reverse_respects_origin_padding_and_gap",
          "[ui][layout][flex][reverse][geometry]") {
    {
        sao_ui_layout_tree_handle_t tree = nullptr;
        REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
        SaoUiLayoutSpec root_spec{};
        sao_ui_layout_spec_defaults(&root_spec);
        root_spec.pad_left_px = 11;
        root_spec.pad_right_px = 13;
        root_spec.pad_top_px = 7;
        root_spec.pad_bottom_px = 9;
        sao_ui_layout_node_handle_t root = nullptr;
        REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_FLEX, &root_spec, &root) ==
                SAO_STATUS_OK);
        SaoUiFlexMode flex{};
        flex.direction = SAO_UI_FLEX_ROW_REVERSE;
        flex.gap_main_px = 5;
        flex.align_items = SAO_UI_ALIGN_AXIS_STRETCH;
        REQUIRE(sao_ui_layout_node_set_mode_config(root, &flex) == SAO_STATUS_OK);
        const SaoUiLayoutSpec specs[3] = {
            make_spec_fixed(30, 20), make_spec_fixed(40, 20), make_spec_fixed(50, 20)};
        sao_ui_layout_node_handle_t children[3]{};
        for (int index = 0; index < 3; ++index) {
            REQUIRE(sao_ui_layout_node_add_widget(
                        root, sentinel_widget(index + 1), &specs[index], &children[index]) ==
                    SAO_STATUS_OK);
        }
        SaoUiSize measured{};
        REQUIRE(sao_ui_layout_measure(root, {240, 120}, &measured) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_arrange(root, {40, 60, 240, 120}) == SAO_STATUS_OK);
        SaoUiRect parent{};
        REQUIRE(sao_ui_layout_node_get_rect(root, &parent) == SAO_STATUS_OK);
        SaoUiRect rects[3]{};
        for (int index = 0; index < 3; ++index) {
            REQUIRE(sao_ui_layout_node_get_rect(children[index], &rects[index]) == SAO_STATUS_OK);
            check_rect_nonnegative_and_within(rects[index], parent);
        }
        CHECK(rects[0].x_px == 237);
        CHECK(rects[1].x_px == 192);
        CHECK(rects[2].x_px == 137);
        CHECK(rects[0].height_px == 104);
        CHECK(rects[0].x_px + rects[0].width_px == 267);
        sao_ui_layout_tree_destroy(tree);
    }

    {
        sao_ui_layout_tree_handle_t tree = nullptr;
        REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
        SaoUiLayoutSpec root_spec{};
        sao_ui_layout_spec_defaults(&root_spec);
        root_spec.pad_left_px = 7;
        root_spec.pad_right_px = 9;
        root_spec.pad_top_px = 11;
        root_spec.pad_bottom_px = 13;
        sao_ui_layout_node_handle_t root = nullptr;
        REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_FLEX, &root_spec, &root) ==
                SAO_STATUS_OK);
        SaoUiFlexMode flex{};
        flex.direction = SAO_UI_FLEX_COLUMN_REVERSE;
        flex.gap_main_px = 6;
        flex.align_items = SAO_UI_ALIGN_AXIS_STRETCH;
        REQUIRE(sao_ui_layout_node_set_mode_config(root, &flex) == SAO_STATUS_OK);
        const SaoUiLayoutSpec specs[3] = {
            make_spec_fixed(20, 30), make_spec_fixed(20, 40), make_spec_fixed(20, 50)};
        sao_ui_layout_node_handle_t children[3]{};
        for (int index = 0; index < 3; ++index) {
            REQUIRE(sao_ui_layout_node_add_widget(
                        root, sentinel_widget(index + 11), &specs[index], &children[index]) ==
                    SAO_STATUS_OK);
        }
        SaoUiSize measured{};
        REQUIRE(sao_ui_layout_measure(root, {160, 240}, &measured) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_arrange(root, {40, 60, 160, 240}) == SAO_STATUS_OK);
        SaoUiRect parent{};
        REQUIRE(sao_ui_layout_node_get_rect(root, &parent) == SAO_STATUS_OK);
        SaoUiRect rects[3]{};
        for (int index = 0; index < 3; ++index) {
            REQUIRE(sao_ui_layout_node_get_rect(children[index], &rects[index]) == SAO_STATUS_OK);
            check_rect_nonnegative_and_within(rects[index], parent);
        }
        CHECK(rects[0].y_px == 257);
        CHECK(rects[1].y_px == 211);
        CHECK(rects[2].y_px == 155);
        CHECK(rects[0].width_px == 144);
        CHECK(rects[0].y_px + rects[0].height_px == 287);
        sao_ui_layout_tree_destroy(tree);
    }
}

TEST_CASE("layout_negative_parent_rect_zeroes_all_output_rects",
          "[ui][layout][contract][nonnegative]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_VERTICAL, &root_spec, &root) ==
            SAO_STATUS_OK);
    SaoUiLayoutSpec child_spec = make_spec_fixed(80, 40);
    sao_ui_layout_node_handle_t child = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &child_spec, &child) ==
            SAO_STATUS_OK);
    SaoUiSize measured{};
    REQUIRE(sao_ui_layout_measure(root, {100, 100}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {-20, -30, -40, -50}) == SAO_STATUS_OK);
    SaoUiRect parent{}, rect{};
    REQUIRE(sao_ui_layout_node_get_rect(root, &parent) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_node_get_rect(child, &rect) == SAO_STATUS_OK);
    CHECK(parent.x_px == 0);
    CHECK(parent.y_px == 0);
    CHECK(parent.width_px == 0);
    CHECK(parent.height_px == 0);
    check_rect_nonnegative_and_within(parent, parent);
    check_rect_nonnegative_and_within(rect, parent);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout_extreme_finite_weights_and_max_gaps_stay_bounded",
          "[ui][layout][grid][flex][weight][overflow]") {
    const int32_t max_value = std::numeric_limits<int32_t>::max();
    const float extreme_weight = std::numeric_limits<float>::max();

    {
        sao_ui_layout_tree_handle_t tree = nullptr;
        REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
        SaoUiLayoutSpec root_spec{};
        sao_ui_layout_spec_defaults(&root_spec);
        sao_ui_layout_node_handle_t root = nullptr;
        REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_GRID, &root_spec, &root) ==
                SAO_STATUS_OK);
        constexpr int track_count = 11;
        SaoUiTrackSize cols[track_count]{};
        for (auto& col : cols) {
            col.kind = SAO_UI_TRACK_FLEX;
            col.flex_weight = extreme_weight;
        }
        SaoUiTrackSize rows[1] = {{SAO_UI_TRACK_FIXED, 10, 0.0f, 0, {0, 0, 0, 0}}};
        SaoUiGridMode grid{};
        grid.rows = rows;
        grid.row_count = 1;
        grid.cols = cols;
        grid.col_count = track_count;
        grid.col_gap_px = max_value;
        REQUIRE(sao_ui_layout_node_set_mode_config(root, &grid) == SAO_STATUS_OK);
        SaoUiLayoutSpec child_spec = make_spec_fixed(1, 10);
        sao_ui_layout_node_handle_t children[track_count]{};
        for (int index = 0; index < track_count; ++index) {
            REQUIRE(sao_ui_layout_node_add_widget(
                        root, sentinel_widget(index + 1), &child_spec, &children[index]) ==
                    SAO_STATUS_OK);
        }
        SaoUiSize measured{};
        REQUIRE(sao_ui_layout_measure(root, {257, 10}, &measured) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_arrange(root, {17, 23, 257, 10}) == SAO_STATUS_OK);
        SaoUiRect parent{};
        REQUIRE(sao_ui_layout_node_get_rect(root, &parent) == SAO_STATUS_OK);
        check_rect_nonnegative_and_within(parent, parent);
        for (const auto child_node : children) {
            SaoUiRect rect{};
            REQUIRE(sao_ui_layout_node_get_rect(child_node, &rect) == SAO_STATUS_OK);
            check_rect_nonnegative_and_within(rect, parent);
        }
        sao_ui_layout_tree_destroy(tree);
    }

    {
        sao_ui_layout_tree_handle_t tree = nullptr;
        REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
        SaoUiLayoutSpec root_spec{};
        sao_ui_layout_spec_defaults(&root_spec);
        sao_ui_layout_node_handle_t root = nullptr;
        REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_FLEX, &root_spec, &root) ==
                SAO_STATUS_OK);
        SaoUiFlexMode flex{};
        flex.direction = SAO_UI_FLEX_ROW;
        flex.gap_main_px = 1;
        flex.align_items = SAO_UI_ALIGN_AXIS_STRETCH;
        REQUIRE(sao_ui_layout_node_set_mode_config(root, &flex) == SAO_STATUS_OK);
        constexpr int child_count = 11;
        sao_ui_layout_node_handle_t children[child_count]{};
        for (int index = 0; index < child_count; ++index) {
            SaoUiLayoutSpec child_spec = make_spec_fixed(1, 10);
            child_spec.weight = extreme_weight;
            REQUIRE(sao_ui_layout_node_add_widget(
                        root, sentinel_widget(index + 101), &child_spec, &children[index]) ==
                    SAO_STATUS_OK);
        }
        SaoUiSize measured{};
        REQUIRE(sao_ui_layout_measure(root, {512, 10}, &measured) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layout_arrange(root, {31, 29, 512, 10}) == SAO_STATUS_OK);
        SaoUiRect parent{};
        REQUIRE(sao_ui_layout_node_get_rect(root, &parent) == SAO_STATUS_OK);
        for (const auto child_node : children) {
            SaoUiRect rect{};
            REQUIRE(sao_ui_layout_node_get_rect(child_node, &rect) == SAO_STATUS_OK);
            check_rect_nonnegative_and_within(rect, parent);
        }
        sao_ui_layout_tree_destroy(tree);
    }
}

TEST_CASE("layout grid config validates pointer counts and rejects leaves",
          "[ui][layout][grid][contract]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_GRID, &root_spec, &root) ==
            SAO_STATUS_OK);

    SaoUiGridMode invalid_rows{};
    invalid_rows.row_count = 1U;
    CHECK(sao_ui_layout_node_set_mode_config(root, &invalid_rows) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    SaoUiGridMode invalid_cols{};
    invalid_cols.col_count = 1U;
    CHECK(sao_ui_layout_node_set_mode_config(root, &invalid_cols) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    SaoUiTrackSize rows[1] = {{SAO_UI_TRACK_FIXED, 12, 0.0F, 0, {0, 0, 0, 0}}};
    SaoUiTrackSize cols[1] = {{SAO_UI_TRACK_FIXED, 12, 0.0F, 0, {0, 0, 0, 0}}};
    SaoUiGridMode valid{};
    valid.rows = rows;
    valid.row_count = 1U;
    valid.cols = cols;
    valid.col_count = 1U;
    REQUIRE(sao_ui_layout_node_set_mode_config(root, &valid) == SAO_STATUS_OK);

    SaoUiLayoutSpec leaf_spec = make_spec_fixed(12, 12);
    sao_ui_layout_node_handle_t leaf = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(1), &leaf_spec, &leaf) ==
            SAO_STATUS_OK);
    SaoUiVerticalMode leaf_config{};
    CHECK(sao_ui_layout_node_set_mode_config(leaf, &leaf_config) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    sao_ui_layout_tree_destroy(tree);
}

TEST_CASE("layout dirty rects are populated by production arrange boundaries",
          "[ui][layout][dirty][rects]") {
    sao_ui_layout_tree_handle_t tree = nullptr;
    REQUIRE(sao_ui_layout_tree_create(&tree) == SAO_STATUS_OK);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    sao_ui_layout_node_handle_t root = nullptr;
    REQUIRE(sao_ui_layout_tree_set_root(tree, SAO_UI_LAYOUT_VERTICAL, &root_spec, &root) ==
            SAO_STATUS_OK);
    SaoUiLayoutSpec leaf_spec = make_spec_fixed(40, 20);
    sao_ui_layout_node_handle_t leaf = nullptr;
    REQUIRE(sao_ui_layout_node_add_widget(root, sentinel_widget(2), &leaf_spec, &leaf) ==
            SAO_STATUS_OK);

    SaoUiSize measured{};
    REQUIRE(sao_ui_layout_measure(root, {80, 40}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 80, 40}) == SAO_STATUS_OK);
    SaoUiRect dirty[8]{};
    size_t written = 0;
    REQUIRE(sao_ui_layout_take_dirty_rects(tree, dirty, 8U, &written) == SAO_STATUS_OK);
    CHECK(written > 0U);

    REQUIRE(sao_ui_layout_measure(root, {80, 40}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 80, 40}) == SAO_STATUS_OK);
    written = 0;
    REQUIRE(sao_ui_layout_take_dirty_rects(tree, dirty, 8U, &written) == SAO_STATUS_OK);
    CHECK(written == 0U);

    REQUIRE(sao_ui_layout_node_invalidate(leaf) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_measure(root, {80, 40}, &measured) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layout_arrange(root, {0, 0, 80, 40}) == SAO_STATUS_OK);
    written = 0;
    REQUIRE(sao_ui_layout_take_dirty_rects(tree, dirty, 8U, &written) == SAO_STATUS_OK);
    REQUIRE(written > 0U);
    SaoUiRect leaf_rect{};
    REQUIRE(sao_ui_layout_node_get_rect(leaf, &leaf_rect) == SAO_STATUS_OK);
    bool found_leaf = false;
    for (size_t index = 0; index < std::min(written, size_t{8}); ++index) {
        found_leaf = found_leaf ||
                     (dirty[index].x_px == leaf_rect.x_px && dirty[index].y_px == leaf_rect.y_px &&
                      dirty[index].width_px == leaf_rect.width_px &&
                      dirty[index].height_px == leaf_rect.height_px);
    }
    CHECK(found_leaf);
    sao_ui_layout_tree_destroy(tree);
}
