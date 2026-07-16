// Wave 4 tests for the Table widget (G3.8 first slice).
//
// Coverage (5 CASE):
//   * table_create_and_set_rows_populates_view
//   * table_sort_ascending_and_descending_stable
//   * table_filter_matches_case_insensitive_substring
//   * table_virtual_scroll_get_visible_range
//   * table_row_click_dispatches_callback_with_correct_row_id
//
// All tests are pure state — no D3D device.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/widget_table.h"

extern "C" {

struct SaoUiPointF { float x; float y; };

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_get_visible_row_count(
    sao_ui_widget_handle_t handle, size_t* out_count);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_get_row_id_at_view_index(
    sao_ui_widget_handle_t handle, size_t view_index, int64_t* out_row_id);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_get_visible_range(
    sao_ui_widget_handle_t handle,
    int32_t viewport_h_px,
    int32_t scroll_offset_px,
    size_t* first_row_out,
    size_t* last_row_out);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_hit_test(
    sao_ui_widget_handle_t handle,
    SaoUiPointF point,
    int32_t total_width_px,
    int32_t scroll_offset_px,
    size_t* out_row_view_index,
    int32_t* out_col_index);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_sort_by_index(
    sao_ui_widget_handle_t handle,
    int32_t col_index,
    bool ascending);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_fire_row_click(
    sao_ui_widget_handle_t handle,
    size_t view_index);

SAO_UI_API void SAO_UI_CALL sao_ui_widget_table_family_destroy(
    sao_ui_widget_handle_t handle);

}  // extern "C"

namespace {

struct RowClickSink {
    std::atomic<int64_t> last_row_id{-1};
    std::atomic<int>     hits{0};
};

extern "C" void SAO_UI_CALL row_click_cb(int64_t row_id, void* user_data) {
    if (user_data == nullptr) return;
    auto* sink = static_cast<RowClickSink*>(user_data);
    sink->last_row_id.store(row_id);
    sink->hits.fetch_add(1);
}

sao_ui_widget_handle_t make_dps_table() {
    static SaoUiTableColumn cols[3] = {};
    cols[0].key_utf8   = "name";
    cols[0].title_utf8 = "Name";
    cols[0].type       = SAO_UI_COL_TEXT;
    cols[0].align      = SAO_UI_ALIGN_LEFT;
    cols[0].min_width_px = 100;
    cols[0].flex_weight  = 1.0f;
    cols[0].sortable   = true;
    cols[0].filterable = true;

    cols[1].key_utf8   = "dps";
    cols[1].title_utf8 = "DPS";
    cols[1].type       = SAO_UI_COL_NUMBER;
    cols[1].align      = SAO_UI_ALIGN_RIGHT;
    cols[1].min_width_px = 80;
    cols[1].sortable   = true;
    cols[1].filterable = false;

    cols[2].key_utf8   = "role";
    cols[2].title_utf8 = "Role";
    cols[2].type       = SAO_UI_COL_TEXT;
    cols[2].align      = SAO_UI_ALIGN_CENTER;
    cols[2].min_width_px = 60;
    cols[2].sortable   = true;
    cols[2].filterable = true;

    SaoUiTableSpec spec{};
    spec.columns = cols;
    spec.column_count = 3;
    spec.row_height_px = 20;
    spec.header_height_px = 24;
    spec.show_header = true;
    spec.zebra_stripes = true;
    spec.row_hover_highlight = true;

    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_table_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    return h;
}

// Build 4 rows: alice/1000/dps, bob/500/heal, charlie/1500/tank, dave/800/dps.
void populate_dps_rows(sao_ui_widget_handle_t h) {
    static SaoUiCellValue cell_alice[3] = {};
    cell_alice[0].kind = SAO_UI_CELL_STRING; cell_alice[0].v.s_utf8 = "alice";
    cell_alice[1].kind = SAO_UI_CELL_INT64;  cell_alice[1].v.i64    = 1000;
    cell_alice[2].kind = SAO_UI_CELL_STRING; cell_alice[2].v.s_utf8 = "dps";

    static SaoUiCellValue cell_bob[3] = {};
    cell_bob[0].kind = SAO_UI_CELL_STRING;   cell_bob[0].v.s_utf8 = "bob";
    cell_bob[1].kind = SAO_UI_CELL_INT64;    cell_bob[1].v.i64    = 500;
    cell_bob[2].kind = SAO_UI_CELL_STRING;   cell_bob[2].v.s_utf8 = "heal";

    static SaoUiCellValue cell_charlie[3] = {};
    cell_charlie[0].kind = SAO_UI_CELL_STRING; cell_charlie[0].v.s_utf8 = "charlie";
    cell_charlie[1].kind = SAO_UI_CELL_INT64;  cell_charlie[1].v.i64    = 1500;
    cell_charlie[2].kind = SAO_UI_CELL_STRING; cell_charlie[2].v.s_utf8 = "tank";

    static SaoUiCellValue cell_dave[3] = {};
    cell_dave[0].kind = SAO_UI_CELL_STRING; cell_dave[0].v.s_utf8 = "dave";
    cell_dave[1].kind = SAO_UI_CELL_INT64;  cell_dave[1].v.i64    = 800;
    cell_dave[2].kind = SAO_UI_CELL_STRING; cell_dave[2].v.s_utf8 = "dps";

    SaoUiTableRow rows[4] = {};
    rows[0].row_id = 1; rows[0].cells = cell_alice;   rows[0].cell_count = 3;
    rows[1].row_id = 2; rows[1].cells = cell_bob;     rows[1].cell_count = 3;
    rows[2].row_id = 3; rows[2].cells = cell_charlie; rows[2].cell_count = 3;
    rows[3].row_id = 4; rows[3].cells = cell_dave;    rows[3].cell_count = 3;

    REQUIRE(sao_ui_table_set_rows(h, rows, 4) == SAO_STATUS_OK);
}

}  // namespace

TEST_CASE("table_create_and_set_rows_populates_view",
          "[ui][widget][table][wave4]") {
    sao_ui_widget_handle_t h = make_dps_table();
    populate_dps_rows(h);
    size_t n = 0;
    REQUIRE(sao_ui_widget_table_get_visible_row_count(h, &n) == SAO_STATUS_OK);
    REQUIRE(n == 4);
    int64_t row_id = 0;
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 0, &row_id)
            == SAO_STATUS_OK);
    REQUIRE(row_id == 1);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 3, &row_id)
            == SAO_STATUS_OK);
    REQUIRE(row_id == 4);
    sao_ui_widget_table_family_destroy(h);
}

TEST_CASE("table_sort_ascending_and_descending_stable",
          "[ui][widget][table][wave4]") {
    sao_ui_widget_handle_t h = make_dps_table();
    populate_dps_rows(h);
    // Sort by "dps" ascending → bob(500), dave(800), alice(1000), charlie(1500).
    REQUIRE(sao_ui_widget_table_sort_by_index(h, 1, /*ascending=*/true)
            == SAO_STATUS_OK);
    int64_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 0, &r0)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 1, &r1)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 2, &r2)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 3, &r3)
            == SAO_STATUS_OK);
    REQUIRE(r0 == 2);   // bob
    REQUIRE(r1 == 4);   // dave
    REQUIRE(r2 == 1);   // alice
    REQUIRE(r3 == 3);   // charlie
    // Descending.
    REQUIRE(sao_ui_widget_table_sort_by_index(h, 1, /*ascending=*/false)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 0, &r0)
            == SAO_STATUS_OK);
    REQUIRE(r0 == 3);   // charlie(1500) first
    // Sort by "role" (both alice and dave are 'dps' → stable order
    // preserves alice before dave from insertion order).
    REQUIRE(sao_ui_widget_table_sort_by_index(h, 2, /*ascending=*/true)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 0, &r0)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 1, &r1)
            == SAO_STATUS_OK);
    REQUIRE(r0 == 1);   // alice(dps) — earlier insertion
    REQUIRE(r1 == 4);   // dave(dps)
    sao_ui_widget_table_family_destroy(h);
}

TEST_CASE("table_filter_matches_case_insensitive_substring",
          "[ui][widget][table][wave4]") {
    sao_ui_widget_handle_t h = make_dps_table();
    populate_dps_rows(h);
    REQUIRE(sao_ui_table_set_filter(h, "TA") == SAO_STATUS_OK);
    size_t n = 0;
    REQUIRE(sao_ui_widget_table_get_visible_row_count(h, &n) == SAO_STATUS_OK);
    // 'ta' appears in 'tank' only.
    REQUIRE(n == 1);
    int64_t rid = 0;
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 0, &rid)
            == SAO_STATUS_OK);
    REQUIRE(rid == 3);   // charlie's row
    // Clear filter.
    REQUIRE(sao_ui_table_set_filter(h, "") == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_visible_row_count(h, &n) == SAO_STATUS_OK);
    REQUIRE(n == 4);
    sao_ui_widget_table_family_destroy(h);
}

TEST_CASE("table_virtual_scroll_get_visible_range",
          "[ui][widget][table][wave4]") {
    sao_ui_widget_handle_t h = make_dps_table();
    populate_dps_rows(h);
    // Viewport 60px / row 20px → 3 rows visible from top.
    size_t first = 999, last = 999;
    REQUIRE(sao_ui_widget_table_get_visible_range(h, 60, 0, &first, &last)
            == SAO_STATUS_OK);
    REQUIRE(first == 0);
    REQUIRE(last  == 2);
    // Scroll 25 px down → first row is partial, second row is fully in view.
    REQUIRE(sao_ui_widget_table_get_visible_range(h, 60, 25, &first, &last)
            == SAO_STATUS_OK);
    REQUIRE(first == 1);
    // Should reach the very last row (idx 3).
    REQUIRE(last  == 3);
    sao_ui_widget_table_family_destroy(h);
}

TEST_CASE("table_row_click_dispatches_callback_with_correct_row_id",
          "[ui][widget][table][wave4]") {
    sao_ui_widget_handle_t h = make_dps_table();
    populate_dps_rows(h);
    RowClickSink sink;
    REQUIRE(sao_ui_table_set_row_click_handler(h, row_click_cb, &sink)
            == SAO_STATUS_OK);
    // Sort by dps ascending → view index 0 = bob (row_id=2).
    REQUIRE(sao_ui_widget_table_sort_by_index(h, 1, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_fire_row_click(h, 0) == SAO_STATUS_OK);
    REQUIRE(sink.hits.load() == 1);
    REQUIRE(sink.last_row_id.load() == 2);
    // Click on view index 3 = charlie (row_id=3).
    REQUIRE(sao_ui_widget_table_fire_row_click(h, 3) == SAO_STATUS_OK);
    REQUIRE(sink.hits.load() == 2);
    REQUIRE(sink.last_row_id.load() == 3);
    sao_ui_widget_table_family_destroy(h);
}
