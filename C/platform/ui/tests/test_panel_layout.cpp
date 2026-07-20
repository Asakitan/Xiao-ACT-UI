// SAO Auto — panel-layout mode, hit-test, and dirty-propagation tests.
//
// Covers the 6 layout modes (vertical/horizontal/grid/dock/absolute/
// flex) and the hit-test + dirty propagation basics.  Leaf widgets
// are added with a fixed size via SaoUiLayoutSpec.fixed_* so we don't
// require the (still-stub) widget size_hint plumbing.

#include <cstring>
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
