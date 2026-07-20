// Table widget state, sorting, filtering, and virtual-scroll tests.
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
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/widget_table.h"

extern "C" {

struct SaoUiPointF {
    float x;
    float y;
};

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_get_visible_row_count(sao_ui_widget_handle_t handle, size_t* out_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_table_get_row_id_at_view_index(
    sao_ui_widget_handle_t handle, size_t view_index, int64_t* out_row_id);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_table_get_visible_range(
    sao_ui_widget_handle_t handle, int32_t viewport_h_px, int32_t scroll_offset_px,
    size_t* first_row_out, size_t* last_row_out);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_table_hit_test(
    sao_ui_widget_handle_t handle, SaoUiPointF point, int32_t total_width_px,
    int32_t scroll_offset_px, size_t* out_row_view_index, int32_t* out_col_index);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_table_sort_by_index(sao_ui_widget_handle_t handle,
                                                                      int32_t col_index,
                                                                      bool ascending);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_table_fire_row_click(sao_ui_widget_handle_t handle, size_t view_index);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_table_fire_cell_action(
    sao_ui_widget_handle_t handle, size_t view_index, int32_t column_index);

SAO_UI_API void SAO_UI_CALL sao_ui_widget_table_family_destroy(sao_ui_widget_handle_t handle);

} // extern "C"

namespace {

using namespace std::chrono_literals;

constexpr sao_status_t kBusyStatus = static_cast<sao_status_t>(-102);

struct RowClickSink {
    std::atomic<int64_t> last_row_id{-1};
    std::atomic<int> hits{0};
};

void SAO_UI_CALL row_click_cb(int64_t row_id, void* user_data) {
    if (user_data == nullptr)
        return;
    auto* sink = static_cast<RowClickSink*>(user_data);
    sink->last_row_id.store(row_id);
    sink->hits.fetch_add(1);
}

struct BlockingCallback {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{false};
    bool release{false};
};

void block_callback(BlockingCallback& state) {
    std::unique_lock lock(state.mutex);
    state.entered = true;
    state.cv.notify_all();
    state.cv.wait(lock, [&] { return state.release; });
}

void release_callback(BlockingCallback& state) {
    {
        std::lock_guard lock(state.mutex);
        state.release = true;
    }
    state.cv.notify_all();
}

void SAO_UI_CALL blocking_row_click_cb(int64_t, void* user_data) {
    block_callback(*static_cast<BlockingCallback*>(user_data));
}

void SAO_UI_CALL blocking_cell_action_cb(int64_t, const char*, void* user_data) {
    block_callback(*static_cast<BlockingCallback*>(user_data));
}

void SAO_UI_CALL throwing_row_click_cb(int64_t, void*) {
    throw std::runtime_error("row click callback failure");
}

void SAO_UI_CALL throwing_cell_action_cb(int64_t, const char*, void*) {
    throw std::runtime_error("cell action callback failure");
}

struct SelfReplaceCapture {
    sao_ui_widget_handle_t handle{nullptr};
    sao_status_t status{SAO_STATUS_ERR_UNKNOWN};
};

void SAO_UI_CALL replace_row_click_from_callback(int64_t, void* user_data) {
    auto* capture = static_cast<SelfReplaceCapture*>(user_data);
    capture->status = sao_ui_table_set_row_click_handler(capture->handle, nullptr, nullptr);
}

void SAO_UI_CALL replace_cell_action_from_callback(int64_t, const char*, void* user_data) {
    auto* capture = static_cast<SelfReplaceCapture*>(user_data);
    capture->status = sao_ui_table_set_cell_action_handler(capture->handle, nullptr, nullptr);
}

struct SelfDestroyCapture {
    sao_ui_widget_handle_t handle{nullptr};
    std::atomic<uint32_t> calls{0};
};

void SAO_UI_CALL destroy_table_from_row_click(int64_t, void* user_data) {
    auto* capture = static_cast<SelfDestroyCapture*>(user_data);
    capture->calls.fetch_add(1);
    sao_ui_widget_table_family_destroy(capture->handle);
}

sao_ui_widget_handle_t make_dps_table() {
    static SaoUiTableColumn cols[3] = {};
    cols[0].key_utf8 = "name";
    cols[0].title_utf8 = "Name";
    cols[0].type = SAO_UI_COL_TEXT;
    cols[0].align = SAO_UI_ALIGN_LEFT;
    cols[0].min_width_px = 100;
    cols[0].flex_weight = 1.0f;
    cols[0].sortable = true;
    cols[0].filterable = true;

    cols[1].key_utf8 = "dps";
    cols[1].title_utf8 = "DPS";
    cols[1].type = SAO_UI_COL_NUMBER;
    cols[1].align = SAO_UI_ALIGN_RIGHT;
    cols[1].min_width_px = 80;
    cols[1].sortable = true;
    cols[1].filterable = false;

    cols[2].key_utf8 = "role";
    cols[2].title_utf8 = "Role";
    cols[2].type = SAO_UI_COL_TEXT;
    cols[2].align = SAO_UI_ALIGN_CENTER;
    cols[2].min_width_px = 60;
    cols[2].sortable = true;
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
    cell_alice[0].kind = SAO_UI_CELL_STRING;
    cell_alice[0].v.s_utf8 = "alice";
    cell_alice[1].kind = SAO_UI_CELL_INT64;
    cell_alice[1].v.i64 = 1000;
    cell_alice[2].kind = SAO_UI_CELL_STRING;
    cell_alice[2].v.s_utf8 = "dps";

    static SaoUiCellValue cell_bob[3] = {};
    cell_bob[0].kind = SAO_UI_CELL_STRING;
    cell_bob[0].v.s_utf8 = "bob";
    cell_bob[1].kind = SAO_UI_CELL_INT64;
    cell_bob[1].v.i64 = 500;
    cell_bob[2].kind = SAO_UI_CELL_STRING;
    cell_bob[2].v.s_utf8 = "heal";

    static SaoUiCellValue cell_charlie[3] = {};
    cell_charlie[0].kind = SAO_UI_CELL_STRING;
    cell_charlie[0].v.s_utf8 = "charlie";
    cell_charlie[1].kind = SAO_UI_CELL_INT64;
    cell_charlie[1].v.i64 = 1500;
    cell_charlie[2].kind = SAO_UI_CELL_STRING;
    cell_charlie[2].v.s_utf8 = "tank";

    static SaoUiCellValue cell_dave[3] = {};
    cell_dave[0].kind = SAO_UI_CELL_STRING;
    cell_dave[0].v.s_utf8 = "dave";
    cell_dave[1].kind = SAO_UI_CELL_INT64;
    cell_dave[1].v.i64 = 800;
    cell_dave[2].kind = SAO_UI_CELL_STRING;
    cell_dave[2].v.s_utf8 = "dps";

    SaoUiTableRow rows[4] = {};
    rows[0].row_id = 1;
    rows[0].cells = cell_alice;
    rows[0].cell_count = 3;
    rows[1].row_id = 2;
    rows[1].cells = cell_bob;
    rows[1].cell_count = 3;
    rows[2].row_id = 3;
    rows[2].cells = cell_charlie;
    rows[2].cell_count = 3;
    rows[3].row_id = 4;
    rows[3].cells = cell_dave;
    rows[3].cell_count = 3;

    REQUIRE(sao_ui_table_set_rows(h, rows, 4) == SAO_STATUS_OK);
}

} // namespace

TEST_CASE("table_create_and_set_rows_populates_view", "[ui][widget][table][runtime]") {
    sao_ui_widget_handle_t h = make_dps_table();
    populate_dps_rows(h);
    size_t n = 0;
    REQUIRE(sao_ui_widget_table_get_visible_row_count(h, &n) == SAO_STATUS_OK);
    REQUIRE(n == 4);
    int64_t row_id = 0;
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 0, &row_id) == SAO_STATUS_OK);
    REQUIRE(row_id == 1);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 3, &row_id) == SAO_STATUS_OK);
    REQUIRE(row_id == 4);
    sao_ui_widget_table_family_destroy(h);
}

TEST_CASE("table_sort_ascending_and_descending_stable", "[ui][widget][table][runtime]") {
    sao_ui_widget_handle_t h = make_dps_table();
    populate_dps_rows(h);
    // Sort by "dps" ascending → bob(500), dave(800), alice(1000), charlie(1500).
    REQUIRE(sao_ui_widget_table_sort_by_index(h, 1, /*ascending=*/true) == SAO_STATUS_OK);
    int64_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 0, &r0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 1, &r1) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 2, &r2) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 3, &r3) == SAO_STATUS_OK);
    REQUIRE(r0 == 2); // bob
    REQUIRE(r1 == 4); // dave
    REQUIRE(r2 == 1); // alice
    REQUIRE(r3 == 3); // charlie
    // Descending.
    REQUIRE(sao_ui_widget_table_sort_by_index(h, 1, /*ascending=*/false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 0, &r0) == SAO_STATUS_OK);
    REQUIRE(r0 == 3); // charlie(1500) first
    // Sort by "role" (both alice and dave are 'dps' → stable order
    // preserves alice before dave from insertion order).
    REQUIRE(sao_ui_widget_table_sort_by_index(h, 2, /*ascending=*/true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 0, &r0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 1, &r1) == SAO_STATUS_OK);
    REQUIRE(r0 == 1); // alice(dps) — earlier insertion
    REQUIRE(r1 == 4); // dave(dps)
    sao_ui_widget_table_family_destroy(h);
}

TEST_CASE("table_filter_matches_case_insensitive_substring", "[ui][widget][table][runtime]") {
    sao_ui_widget_handle_t h = make_dps_table();
    populate_dps_rows(h);
    REQUIRE(sao_ui_table_set_filter(h, "TA") == SAO_STATUS_OK);
    size_t n = 0;
    REQUIRE(sao_ui_widget_table_get_visible_row_count(h, &n) == SAO_STATUS_OK);
    // 'ta' appears in 'tank' only.
    REQUIRE(n == 1);
    int64_t rid = 0;
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(h, 0, &rid) == SAO_STATUS_OK);
    REQUIRE(rid == 3); // charlie's row
    // Clear filter.
    REQUIRE(sao_ui_table_set_filter(h, "") == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_visible_row_count(h, &n) == SAO_STATUS_OK);
    REQUIRE(n == 4);
    sao_ui_widget_table_family_destroy(h);
}

TEST_CASE("table_virtual_scroll_get_visible_range", "[ui][widget][table][runtime]") {
    sao_ui_widget_handle_t h = make_dps_table();
    populate_dps_rows(h);
    // Viewport 60px / row 20px → 3 rows visible from top.
    size_t first = 999, last = 999;
    REQUIRE(sao_ui_widget_table_get_visible_range(h, 60, 0, &first, &last) == SAO_STATUS_OK);
    REQUIRE(first == 0);
    REQUIRE(last == 2);
    // Scroll 25 px down → first row is partial, second row is fully in view.
    REQUIRE(sao_ui_widget_table_get_visible_range(h, 60, 25, &first, &last) == SAO_STATUS_OK);
    REQUIRE(first == 1);
    // Should reach the very last row (idx 3).
    REQUIRE(last == 3);
    sao_ui_widget_table_family_destroy(h);
}

TEST_CASE("table_row_click_dispatches_callback_with_correct_row_id",
          "[ui][widget][table][runtime]") {
    sao_ui_widget_handle_t h = make_dps_table();
    populate_dps_rows(h);
    RowClickSink sink;
    REQUIRE(sao_ui_table_set_row_click_handler(h, row_click_cb, &sink) == SAO_STATUS_OK);
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

TEST_CASE("table callback throws translate to unknown and release their leases",
          "[ui][widget][table][runtime][callback][exception]") {
    const auto table = make_dps_table();
    populate_dps_rows(table);

    REQUIRE(sao_ui_table_set_row_click_handler(table, throwing_row_click_cb, nullptr) ==
            SAO_STATUS_OK);
    CHECK(sao_ui_widget_table_fire_row_click(table, 0) == SAO_STATUS_ERR_UNKNOWN);
    CHECK(sao_ui_table_set_row_click_handler(table, nullptr, nullptr) == SAO_STATUS_OK);

    REQUIRE(sao_ui_table_set_cell_action_handler(table, throwing_cell_action_cb, nullptr) ==
            SAO_STATUS_OK);
    CHECK(sao_ui_widget_table_fire_cell_action(table, 0, 2) == SAO_STATUS_ERR_UNKNOWN);
    CHECK(sao_ui_table_set_cell_action_handler(table, nullptr, nullptr) == SAO_STATUS_OK);

    sao_ui_widget_table_family_destroy(table);
}

TEST_CASE("table callback self replacement returns busy without waiting",
          "[ui][widget][table][runtime][callback][reentry]") {
    SECTION("row click") {
        const auto table = make_dps_table();
        populate_dps_rows(table);
        SelfReplaceCapture capture{table};
        REQUIRE(sao_ui_table_set_row_click_handler(table, replace_row_click_from_callback,
                                                   &capture) == SAO_STATUS_OK);
        CHECK(sao_ui_widget_table_fire_row_click(table, 0) == SAO_STATUS_OK);
        CHECK(capture.status == kBusyStatus);
        sao_ui_widget_table_family_destroy(table);
    }

    SECTION("cell action") {
        const auto table = make_dps_table();
        populate_dps_rows(table);
        SelfReplaceCapture capture{table};
        REQUIRE(sao_ui_table_set_cell_action_handler(table, replace_cell_action_from_callback,
                                                     &capture) == SAO_STATUS_OK);
        CHECK(sao_ui_widget_table_fire_cell_action(table, 0, 2) == SAO_STATUS_OK);
        CHECK(capture.status == kBusyStatus);
        sao_ui_widget_table_family_destroy(table);
    }
}

TEST_CASE("table callback self destroy is deferred and invalidates the outer dispatch",
          "[ui][widget][table][runtime][callback][destroy][reentry]") {
    const auto table = make_dps_table();
    populate_dps_rows(table);
    SelfDestroyCapture capture{table};
    REQUIRE(sao_ui_table_set_row_click_handler(table, destroy_table_from_row_click, &capture) ==
            SAO_STATUS_OK);

    CHECK(sao_ui_widget_table_fire_row_click(table, 0) == kBusyStatus);
    CHECK(capture.calls.load() == 1);
    size_t visible = 0;
    CHECK(sao_ui_widget_table_get_visible_row_count(table, &visible) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
}

TEST_CASE("table callback replacement waits for the old generation",
          "[ui][widget][table][runtime][callback][race]") {
    SECTION("row click") {
        const auto table = make_dps_table();
        populate_dps_rows(table);
        BlockingCallback state;
        REQUIRE(sao_ui_table_set_row_click_handler(table, blocking_row_click_cb, &state) ==
                SAO_STATUS_OK);
        std::atomic<sao_status_t> dispatch_status{SAO_STATUS_ERR_UNKNOWN};
        std::thread dispatch(
            [&] { dispatch_status.store(sao_ui_widget_table_fire_row_click(table, 0)); });
        {
            std::unique_lock lock(state.mutex);
            REQUIRE(state.cv.wait_for(lock, 1s, [&] { return state.entered; }));
        }

        std::atomic_bool replaced{false};
        std::atomic<sao_status_t> replacement_status{SAO_STATUS_ERR_UNKNOWN};
        std::thread replacement([&] {
            replacement_status.store(
                sao_ui_table_set_row_click_handler(table, nullptr, nullptr));
            replaced.store(true);
        });
        std::this_thread::sleep_for(50ms);
        CHECK_FALSE(replaced.load());
        release_callback(state);
        dispatch.join();
        replacement.join();

        CHECK(dispatch_status.load() == kBusyStatus);
        CHECK(replacement_status.load() == SAO_STATUS_OK);
        CHECK(replaced.load());
        sao_ui_widget_table_family_destroy(table);
    }

    SECTION("cell action") {
        const auto table = make_dps_table();
        populate_dps_rows(table);
        BlockingCallback state;
        REQUIRE(sao_ui_table_set_cell_action_handler(table, blocking_cell_action_cb, &state) ==
                SAO_STATUS_OK);
        std::atomic<sao_status_t> dispatch_status{SAO_STATUS_ERR_UNKNOWN};
        std::thread dispatch([&] {
            dispatch_status.store(sao_ui_widget_table_fire_cell_action(table, 0, 2));
        });
        {
            std::unique_lock lock(state.mutex);
            REQUIRE(state.cv.wait_for(lock, 1s, [&] { return state.entered; }));
        }

        std::atomic_bool replaced{false};
        std::atomic<sao_status_t> replacement_status{SAO_STATUS_ERR_UNKNOWN};
        std::thread replacement([&] {
            replacement_status.store(
                sao_ui_table_set_cell_action_handler(table, nullptr, nullptr));
            replaced.store(true);
        });
        std::this_thread::sleep_for(50ms);
        CHECK_FALSE(replaced.load());
        release_callback(state);
        dispatch.join();
        replacement.join();

        CHECK(dispatch_status.load() == kBusyStatus);
        CHECK(replacement_status.load() == SAO_STATUS_OK);
        CHECK(replaced.load());
        sao_ui_widget_table_family_destroy(table);
    }
}

TEST_CASE("table concurrent replacements serialize behind the oldest generation",
          "[ui][widget][table][runtime][callback][race][generation]") {
    const auto table = make_dps_table();
    populate_dps_rows(table);
    BlockingCallback state;
    REQUIRE(sao_ui_table_set_row_click_handler(table, blocking_row_click_cb, &state) ==
            SAO_STATUS_OK);
    std::atomic<sao_status_t> dispatch_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread dispatch(
        [&] { dispatch_status.store(sao_ui_widget_table_fire_row_click(table, 0)); });
    {
        std::unique_lock lock(state.mutex);
        REQUIRE(state.cv.wait_for(lock, 1s, [&] { return state.entered; }));
    }

    std::atomic_bool first_started{false};
    std::atomic_bool first_finished{false};
    std::atomic<sao_status_t> first_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread first_replacement([&] {
        first_started.store(true);
        first_status.store(sao_ui_table_set_row_click_handler(table, row_click_cb, nullptr));
        first_finished.store(true);
    });
    while (!first_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(50ms);
    REQUIRE_FALSE(first_finished.load());

    std::atomic_bool second_finished{false};
    std::atomic<sao_status_t> second_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread second_replacement([&] {
        second_status.store(sao_ui_table_set_row_click_handler(table, nullptr, nullptr));
        second_finished.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(second_finished.load());

    release_callback(state);
    dispatch.join();
    first_replacement.join();
    second_replacement.join();

    CHECK(dispatch_status.load() == kBusyStatus);
    CHECK(first_status.load() == SAO_STATUS_OK);
    CHECK(second_status.load() == SAO_STATUS_OK);
    CHECK(second_finished.load());
    sao_ui_widget_table_family_destroy(table);
}

TEST_CASE("table destroy waits for every active callback generation",
          "[ui][widget][table][runtime][callback][destroy][race]") {
    SECTION("row click") {
        const auto table = make_dps_table();
        populate_dps_rows(table);
        BlockingCallback state;
        REQUIRE(sao_ui_table_set_row_click_handler(table, blocking_row_click_cb, &state) ==
                SAO_STATUS_OK);
        std::atomic<sao_status_t> dispatch_status{SAO_STATUS_ERR_UNKNOWN};
        std::thread dispatch(
            [&] { dispatch_status.store(sao_ui_widget_table_fire_row_click(table, 0)); });
        {
            std::unique_lock lock(state.mutex);
            REQUIRE(state.cv.wait_for(lock, 1s, [&] { return state.entered; }));
        }

        std::atomic_bool destroyed{false};
        std::thread destroy([&] {
            sao_ui_widget_table_family_destroy(table);
            destroyed.store(true);
        });
        std::this_thread::sleep_for(50ms);
        CHECK_FALSE(destroyed.load());
        release_callback(state);
        dispatch.join();
        destroy.join();

        CHECK(dispatch_status.load() == kBusyStatus);
        CHECK(destroyed.load());
        size_t visible = 0;
        CHECK(sao_ui_widget_table_get_visible_row_count(table, &visible) ==
              SAO_STATUS_ERR_HANDLE_INVALID);
    }

    SECTION("cell action") {
        const auto table = make_dps_table();
        populate_dps_rows(table);
        BlockingCallback state;
        REQUIRE(sao_ui_table_set_cell_action_handler(table, blocking_cell_action_cb, &state) ==
                SAO_STATUS_OK);
        std::atomic<sao_status_t> dispatch_status{SAO_STATUS_ERR_UNKNOWN};
        std::thread dispatch([&] {
            dispatch_status.store(sao_ui_widget_table_fire_cell_action(table, 0, 2));
        });
        {
            std::unique_lock lock(state.mutex);
            REQUIRE(state.cv.wait_for(lock, 1s, [&] { return state.entered; }));
        }

        std::atomic_bool destroyed{false};
        std::thread destroy([&] {
            sao_ui_widget_table_family_destroy(table);
            destroyed.store(true);
        });
        std::this_thread::sleep_for(50ms);
        CHECK_FALSE(destroyed.load());
        release_callback(state);
        dispatch.join();
        destroy.join();

        CHECK(dispatch_status.load() == kBusyStatus);
        CHECK(destroyed.load());
        size_t visible = 0;
        CHECK(sao_ui_widget_table_get_visible_row_count(table, &visible) ==
              SAO_STATUS_ERR_HANDLE_INVALID);
    }
}

TEST_CASE("table_rejects_unbounded_or_null_nested_inputs",
          "[ui][widget][table][runtime][hardening]") {
    SaoUiTableSpec invalid_spec{};
    invalid_spec.column_count = 1;
    auto output = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    REQUIRE(sao_ui_table_create(nullptr, &invalid_spec, &output) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(output == nullptr);

    SaoUiTableColumn dummy_column{};
    invalid_spec.columns = &dummy_column;
    invalid_spec.column_count = std::numeric_limits<size_t>::max();
    output = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    REQUIRE(sao_ui_table_create(nullptr, &invalid_spec, &output) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(output == nullptr);

    std::string overlong_key(4001, 'k');
    dummy_column.key_utf8 = overlong_key.c_str();
    invalid_spec.column_count = 1;
    output = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    REQUIRE(sao_ui_table_create(nullptr, &invalid_spec, &output) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(output == nullptr);

    const auto table = make_dps_table();
    populate_dps_rows(table);

    SaoUiTableRow row_with_null_cells{};
    row_with_null_cells.row_id = 99;
    row_with_null_cells.cell_count = 1;
    REQUIRE(sao_ui_table_set_rows(table, &row_with_null_cells, 1) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_table_upsert_row(table, &row_with_null_cells) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);

    SaoUiCellValue dummy_cell{};
    SaoUiTableRow row_over_budget{};
    row_over_budget.row_id = 100;
    row_over_budget.cells = &dummy_cell;
    row_over_budget.cell_count = std::numeric_limits<size_t>::max();
    REQUIRE(sao_ui_table_set_rows(table, &row_over_budget, 1) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_table_upsert_row(table, &row_over_budget) == SAO_STATUS_ERR_INVALID_ARGUMENT);

    REQUIRE(
        sao_ui_table_set_rows(table, &row_with_null_cells, std::numeric_limits<size_t>::max()) ==
        SAO_STATUS_ERR_INVALID_ARGUMENT);
    sao_ui_widget_table_family_destroy(table);
}

TEST_CASE("table_copy_in_failures_preserve_the_previous_view",
          "[ui][widget][table][runtime][hardening][transaction]") {
    const auto table = make_dps_table();
    populate_dps_rows(table);

    SaoUiCellValue valid_cells[3]{};
    valid_cells[0].kind = SAO_UI_CELL_STRING;
    valid_cells[0].v.s_utf8 = "replacement";
    valid_cells[1].kind = SAO_UI_CELL_INT64;
    valid_cells[1].v.i64 = 42;
    valid_cells[2].kind = SAO_UI_CELL_STRING;
    valid_cells[2].v.s_utf8 = "dps";
    SaoUiTableRow candidate_rows[2]{};
    candidate_rows[0].row_id = 50;
    candidate_rows[0].cells = valid_cells;
    candidate_rows[0].cell_count = 3;
    candidate_rows[1].row_id = 51;
    candidate_rows[1].cell_count = 1;

    REQUIRE(sao_ui_table_set_rows(table, candidate_rows, 2) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    size_t visible = 0;
    REQUIRE(sao_ui_widget_table_get_visible_row_count(table, &visible) == SAO_STATUS_OK);
    CHECK(visible == 4);
    int64_t row_id = 0;
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(table, 0, &row_id) == SAO_STATUS_OK);
    CHECK(row_id == 1);

    std::string overlong_cell(4001, 'x');
    valid_cells[0].v.s_utf8 = overlong_cell.c_str();
    SaoUiTableRow replacement{};
    replacement.row_id = 1;
    replacement.cells = valid_cells;
    replacement.cell_count = 3;
    REQUIRE(sao_ui_table_upsert_row(table, &replacement) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_table_set_filter(table, "alice") == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_table_get_visible_row_count(table, &visible) == SAO_STATUS_OK);
    CHECK(visible == 1);
    REQUIRE(sao_ui_widget_table_get_row_id_at_view_index(table, 0, &row_id) == SAO_STATUS_OK);
    CHECK(row_id == 1);

    sao_ui_widget_table_family_destroy(table);
}
