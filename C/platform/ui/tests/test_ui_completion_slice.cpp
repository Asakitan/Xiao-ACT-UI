#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sao/ui/widget_chart.h"
#include "sao/ui/widget_data.h"
#include "sao/ui/widget_kit.h"
#include "sao/ui/widget_table.h"

static_assert(SAO_UI_WIDGET_KIT_VERSION == ((1u << 16) | 2u));
static_assert(static_cast<int32_t>(SAO_UI_WIDGET_LABEL) >
              static_cast<int32_t>(SAO_UI_WIDGET_ICON));
static_assert(SAO_UI_WIDGET_SPARKLINE > SAO_UI_WIDGET_LABEL);

extern "C" void SAO_UI_CALL sao_ui_widget_chart_family_destroy(
    sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_data_family_destroy(
    sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_table_family_destroy(
    sao_ui_widget_handle_t handle);
extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_tree_get_selected_node(
    sao_ui_widget_handle_t handle, int64_t* out_node_id);

namespace {

using namespace std::chrono_literals;

constexpr sao_status_t kBusyStatus = static_cast<sao_status_t>(-102);
constexpr size_t kMaxTreeNodes = 1024;
constexpr size_t kMaxTreeStringBytes = 4096;
constexpr size_t kMaxTreeUtf8Bytes = 64U * 1024U;
constexpr size_t kMaxTreeDepth = 64;

struct TreeSelection {
    int64_t node_id = 0;
    int32_t calls = 0;
};

void SAO_UI_CALL on_tree_select(int64_t node_id, void* user_data) {
    auto* selection = static_cast<TreeSelection*>(user_data);
    selection->node_id = node_id;
    ++selection->calls;
}

struct BlockingTreeCallback {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{false};
    bool release{false};
};

void SAO_UI_CALL blocking_tree_select(int64_t, void* user_data) {
    auto* state = static_cast<BlockingTreeCallback*>(user_data);
    std::unique_lock lock(state->mutex);
    state->entered = true;
    state->cv.notify_all();
    state->cv.wait(lock, [&] { return state->release; });
}

void release_tree_callback(BlockingTreeCallback& state) {
    {
        std::lock_guard lock(state.mutex);
        state.release = true;
    }
    state.cv.notify_all();
}

void SAO_UI_CALL throwing_tree_select(int64_t, void*) {
    throw std::runtime_error("tree select callback failure");
}

struct TreeSelfReplaceCapture {
    sao_ui_widget_handle_t tree{nullptr};
    sao_status_t status{SAO_STATUS_ERR_UNKNOWN};
};

void SAO_UI_CALL replace_tree_select_from_callback(int64_t, void* user_data) {
    auto* capture = static_cast<TreeSelfReplaceCapture*>(user_data);
    capture->status = sao_ui_tree_view_set_select_handler(capture->tree, nullptr, nullptr);
}

struct TreeSelfDestroyCapture {
    sao_ui_widget_handle_t tree{nullptr};
    std::atomic<uint32_t> calls{0};
};

void SAO_UI_CALL destroy_tree_from_callback(int64_t, void* user_data) {
    auto* capture = static_cast<TreeSelfDestroyCapture*>(user_data);
    capture->calls.fetch_add(1);
    sao_ui_widget_table_family_destroy(capture->tree);
}

sao_ui_widget_handle_t make_callback_tree() {
    SaoUiTreeViewSpec spec{};
    sao_ui_widget_handle_t tree = nullptr;
    REQUIRE(sao_ui_tree_view_create(nullptr, &spec, &tree) == SAO_STATUS_OK);
    const SaoUiTreeNode node{1, 0, "root", "", -1, true, true, {}, 0, 0};
    REQUIRE(sao_ui_tree_view_set_nodes(tree, &node, 1) == SAO_STATUS_OK);
    return tree;
}

std::vector<SaoUiTreeNode> make_root_nodes(size_t count, const char* label = "",
                                           const char* detail = "") {
    std::vector<SaoUiTreeNode> nodes(count);
    for (size_t index = 0; index < nodes.size(); ++index) {
        nodes[index].node_id = static_cast<int64_t>(index + 1);
        nodes[index].label_utf8 = label;
        nodes[index].detail_utf8 = detail;
        nodes[index].icon_slot = -1;
        nodes[index].selectable = true;
    }
    return nodes;
}

std::vector<SaoUiTreeNode> make_expanded_chain(size_t count) {
    auto nodes = make_root_nodes(count);
    for (size_t index = 0; index < nodes.size(); ++index) {
        nodes[index].parent_id = index == 0 ? 0 : static_cast<int64_t>(index);
        nodes[index].expanded_default = true;
    }
    return nodes;
}

}  // namespace

TEST_CASE("tree view maintains hierarchy visibility and selection", "[ui][completion][tree]") {
    SaoUiTreeViewSpec spec{};
    spec.row_height_px = 22;
    spec.indent_px = 14;
    sao_ui_widget_handle_t tree = nullptr;
    REQUIRE(sao_ui_tree_view_create(nullptr, &spec, &tree) == SAO_STATUS_OK);

    const SaoUiTreeNode nodes[] = {
        {1, 0, "root", "", -1, true, true, {}, 0, 0},
        {2, 1, "child", "", -1, false, true, {}, 0, 0},
        {3, 2, "grandchild", "", -1, false, true, {}, 0, 0},
        {4, 0, "sibling", "", -1, false, true, {}, 0, 0},
    };
    REQUIRE(sao_ui_tree_view_set_nodes(tree, nodes, 4) == SAO_STATUS_OK);

    size_t visible = 0;
    REQUIRE(sao_ui_tree_view_get_visible_count(tree, &visible) == SAO_STATUS_OK);
    CHECK(visible == 3);
    int64_t node_id = 0;
    int32_t depth = -1;
    REQUIRE(sao_ui_tree_view_get_visible_node(tree, 1, &node_id, &depth) == SAO_STATUS_OK);
    CHECK(node_id == 2);
    CHECK(depth == 1);

    REQUIRE(sao_ui_tree_view_expand_node(tree, 2, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_tree_view_get_visible_count(tree, &visible) == SAO_STATUS_OK);
    CHECK(visible == 4);

    TreeSelection selection{};
    REQUIRE(sao_ui_tree_view_set_select_handler(tree, on_tree_select, &selection) == SAO_STATUS_OK);
    REQUIRE(sao_ui_tree_view_select_node(tree, 3) == SAO_STATUS_OK);
    CHECK(selection.calls == 1);
    CHECK(selection.node_id == 3);
    sao_ui_widget_table_family_destroy(tree);
}

TEST_CASE("tree select callback exceptions translate to unknown and release the lease",
          "[ui][completion][tree][callback][exception]") {
    const auto tree = make_callback_tree();
    REQUIRE(sao_ui_tree_view_set_select_handler(tree, throwing_tree_select, nullptr) ==
            SAO_STATUS_OK);
    CHECK(sao_ui_tree_view_select_node(tree, 1) == SAO_STATUS_ERR_UNKNOWN);
    CHECK(sao_ui_tree_view_set_select_handler(tree, nullptr, nullptr) == SAO_STATUS_OK);
    sao_ui_widget_table_family_destroy(tree);
}

TEST_CASE("tree select callback reentry never waits for itself",
          "[ui][completion][tree][callback][reentry]") {
    SECTION("replacement returns busy") {
        const auto tree = make_callback_tree();
        TreeSelfReplaceCapture capture{tree};
        REQUIRE(sao_ui_tree_view_set_select_handler(tree, replace_tree_select_from_callback,
                                                    &capture) == SAO_STATUS_OK);
        CHECK(sao_ui_tree_view_select_node(tree, 1) == SAO_STATUS_OK);
        CHECK(capture.status == kBusyStatus);
        sao_ui_widget_table_family_destroy(tree);
    }

    SECTION("destroy is deferred") {
        const auto tree = make_callback_tree();
        TreeSelfDestroyCapture capture{tree};
        REQUIRE(sao_ui_tree_view_set_select_handler(tree, destroy_tree_from_callback, &capture) ==
                SAO_STATUS_OK);
        CHECK(sao_ui_tree_view_select_node(tree, 1) == kBusyStatus);
        CHECK(capture.calls.load() == 1);
        size_t visible = 0;
        CHECK(sao_ui_tree_view_get_visible_count(tree, &visible) ==
              SAO_STATUS_ERR_HANDLE_INVALID);
    }
}

TEST_CASE("tree callback replacement waits for the old generation",
          "[ui][completion][tree][callback][race]") {
    const auto tree = make_callback_tree();
    BlockingTreeCallback state;
    REQUIRE(sao_ui_tree_view_set_select_handler(tree, blocking_tree_select, &state) ==
            SAO_STATUS_OK);
    std::atomic<sao_status_t> dispatch_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread dispatch([&] { dispatch_status.store(sao_ui_tree_view_select_node(tree, 1)); });
    {
        std::unique_lock lock(state.mutex);
        REQUIRE(state.cv.wait_for(lock, 1s, [&] { return state.entered; }));
    }

    std::atomic_bool replaced{false};
    std::atomic<sao_status_t> replacement_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread replacement([&] {
        replacement_status.store(sao_ui_tree_view_set_select_handler(tree, nullptr, nullptr));
        replaced.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(replaced.load());
    release_tree_callback(state);
    dispatch.join();
    replacement.join();

    CHECK(dispatch_status.load() == kBusyStatus);
    CHECK(replacement_status.load() == SAO_STATUS_OK);
    CHECK(replaced.load());
    sao_ui_widget_table_family_destroy(tree);
}

TEST_CASE("tree destroy waits for the active callback generation",
          "[ui][completion][tree][callback][destroy][race]") {
    const auto tree = make_callback_tree();
    BlockingTreeCallback state;
    REQUIRE(sao_ui_tree_view_set_select_handler(tree, blocking_tree_select, &state) ==
            SAO_STATUS_OK);
    std::atomic<sao_status_t> dispatch_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread dispatch([&] { dispatch_status.store(sao_ui_tree_view_select_node(tree, 1)); });
    {
        std::unique_lock lock(state.mutex);
        REQUIRE(state.cv.wait_for(lock, 1s, [&] { return state.entered; }));
    }

    std::atomic_bool destroyed{false};
    std::thread destroy([&] {
        sao_ui_widget_table_family_destroy(tree);
        destroyed.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(destroyed.load());
    release_tree_callback(state);
    dispatch.join();
    destroy.join();

    CHECK(dispatch_status.load() == kBusyStatus);
    CHECK(destroyed.load());
    size_t visible = 0;
    CHECK(sao_ui_tree_view_get_visible_count(tree, &visible) == SAO_STATUS_ERR_HANDLE_INVALID);
}

TEST_CASE("tree node copy-in rejects excessive count strings total bytes and depth atomically",
          "[ui][completion][tree][hardening][budget][transaction]") {
    const auto tree = make_callback_tree();
    const SaoUiTreeNode baseline[] = {
        {1, 0, "root", "root detail", -1, true, true, {}, 0, 0},
        {2, 1, "child", "child detail", -1, false, true, {}, 0, 0},
    };
    REQUIRE(sao_ui_tree_view_set_nodes(tree, baseline, 2) == SAO_STATUS_OK);
    REQUIRE(sao_ui_tree_view_select_node(tree, 2) == SAO_STATUS_OK);

    const auto require_baseline = [&] {
        size_t visible = 0;
        REQUIRE(sao_ui_tree_view_get_visible_count(tree, &visible) == SAO_STATUS_OK);
        CHECK(visible == 2);
        int64_t node_id = 0;
        int32_t depth = -1;
        REQUIRE(sao_ui_tree_view_get_visible_node(tree, 1, &node_id, &depth) == SAO_STATUS_OK);
        CHECK(node_id == 2);
        CHECK(depth == 1);
        int64_t selected = 0;
        REQUIRE(sao_ui_widget_tree_get_selected_node(tree, &selected) == SAO_STATUS_OK);
        CHECK(selected == 2);
    };

    auto excessive_nodes = make_root_nodes(kMaxTreeNodes + 1);
    CHECK(sao_ui_tree_view_set_nodes(tree, excessive_nodes.data(), excessive_nodes.size()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    require_baseline();

    CHECK(sao_ui_tree_view_set_nodes(tree, baseline, std::numeric_limits<size_t>::max()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    require_baseline();

    std::string overlong(kMaxTreeStringBytes + 1, 'x');
    const SaoUiTreeNode overlong_node{3, 0, overlong.c_str(), "", -1, false, true, {}, 0, 0};
    CHECK(sao_ui_tree_view_set_nodes(tree, &overlong_node, 1) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    require_baseline();

    std::string maximum_string(kMaxTreeStringBytes, 'y');
    auto over_total = make_root_nodes(kMaxTreeUtf8Bytes / (2 * kMaxTreeStringBytes) + 1,
                                      maximum_string.c_str(), maximum_string.c_str());
    CHECK(sao_ui_tree_view_set_nodes(tree, over_total.data(), over_total.size()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    require_baseline();

    auto over_depth = make_expanded_chain(kMaxTreeDepth + 2);
    CHECK(sao_ui_tree_view_set_nodes(tree, over_depth.data(), over_depth.size()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    require_baseline();

    sao_ui_widget_table_family_destroy(tree);
}

TEST_CASE("tree node copy-in accepts every exact budget boundary",
          "[ui][completion][tree][hardening][budget][boundary]") {
    const auto tree = make_callback_tree();

    auto exact_count = make_root_nodes(kMaxTreeNodes);
    REQUIRE(sao_ui_tree_view_set_nodes(tree, exact_count.data(), exact_count.size()) ==
            SAO_STATUS_OK);
    size_t visible = 0;
    REQUIRE(sao_ui_tree_view_get_visible_count(tree, &visible) == SAO_STATUS_OK);
    CHECK(visible == kMaxTreeNodes);

    std::string maximum_string(kMaxTreeStringBytes, 'z');
    auto exact_total = make_root_nodes(kMaxTreeUtf8Bytes / (2 * kMaxTreeStringBytes),
                                      maximum_string.c_str(), maximum_string.c_str());
    REQUIRE(sao_ui_tree_view_set_nodes(tree, exact_total.data(), exact_total.size()) ==
            SAO_STATUS_OK);

    auto exact_depth = make_expanded_chain(kMaxTreeDepth + 1);
    REQUIRE(sao_ui_tree_view_set_nodes(tree, exact_depth.data(), exact_depth.size()) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_tree_view_get_visible_count(tree, &visible) == SAO_STATUS_OK);
    CHECK(visible == kMaxTreeDepth + 1);
    int64_t node_id = 0;
    int32_t depth = -1;
    REQUIRE(sao_ui_tree_view_get_visible_node(tree, visible - 1, &node_id, &depth) ==
            SAO_STATUS_OK);
    CHECK(depth == static_cast<int32_t>(kMaxTreeDepth));

    sao_ui_widget_table_family_destroy(tree);
}

TEST_CASE("bar line and sparkline widgets own caller data", "[ui][completion][chart]") {
    SaoUiBarChartBar bars[] = {
        {"low", 2.0, 0xff112233, 0, 0.0, 0},
        {"high", 9.0, 0xff445566, 0, 0.0, 0},
        {"mid", 5.0, 0xff778899, 0, 0.0, 0},
    };
    SaoUiBarChartSpec bar_spec{};
    bar_spec.bars = bars;
    bar_spec.bar_count = 3;
    bar_spec.sort_desc = true;
    bar_spec.max_visible_bars = 2;
    sao_ui_widget_handle_t bar = nullptr;
    REQUIRE(sao_ui_bar_chart_create(nullptr, &bar_spec, &bar) == SAO_STATUS_OK);
    CHECK(sao_ui_widget_kit_version() == SAO_UI_WIDGET_KIT_VERSION);
    int32_t widget_kind = -1;
    REQUIRE(sao_ui_widget_get_kind(bar, &widget_kind) == SAO_STATUS_OK);
    CHECK(widget_kind == SAO_UI_WIDGET_BAR_CHART);
    SaoUiWidgetSizeHint size_hint{};
    REQUIRE(sao_ui_widget_get_size_hint(bar, 240, 120, &size_hint) == SAO_STATUS_OK);
    CHECK(size_hint.preferred_width_px == 240);
    CHECK(size_hint.preferred_height_px == 120);
    bars[1].value = -1.0;
    size_t shown = 0;
    size_t hidden = 0;
    REQUIRE(sao_ui_bar_chart_get_bar_count(bar, &shown, &hidden) == SAO_STATUS_OK);
    CHECK(shown == 2);
    CHECK(hidden == 1);
    SaoUiBarChartBar first{};
    REQUIRE(sao_ui_bar_chart_get_bar(bar, 0, &first) == SAO_STATUS_OK);
    CHECK(std::string(first.label_utf8) == "high");
    CHECK(first.value == 9.0);

    SaoUiLinePoint points[] = {{1.0, 2.0}, {3.0, 4.0}};
    SaoUiLineChartSeries series{};
    series.series_id_utf8 = "series";
    series.label_utf8 = "Series";
    series.points = points;
    series.point_count = 2;
    SaoUiLineChartSpec line_spec{};
    line_spec.series = &series;
    line_spec.series_count = 1;
    sao_ui_widget_handle_t line = nullptr;
    REQUIRE(sao_ui_line_chart_create(nullptr, &line_spec, &line) == SAO_STATUS_OK);
    points[1].y = 99.0;
    SaoUiLinePoint copied{};
    REQUIRE(sao_ui_line_chart_get_point(line, 0, 1, &copied) == SAO_STATUS_OK);
    CHECK(copied.y == 4.0);

    const double values[] = {4.0, 1.0, 7.0};
    SaoUiSparklineSpec spark_spec{};
    spark_spec.values = values;
    spark_spec.value_count = 3;
    spark_spec.max_points = 3;
    sao_ui_widget_handle_t spark = nullptr;
    REQUIRE(sao_ui_sparkline_create(nullptr, &spark_spec, &spark) == SAO_STATUS_OK);
    REQUIRE(sao_ui_sparkline_append(spark, 8.0) == SAO_STATUS_OK);
    double min_value = 0.0;
    double max_value = 0.0;
    size_t count = 0;
    REQUIRE(sao_ui_sparkline_get_range(spark, &min_value, &max_value, &count) == SAO_STATUS_OK);
    CHECK(count == 3);
    CHECK(min_value == 1.0);
    CHECK(max_value == 8.0);
    REQUIRE(sao_ui_sparkline_set_values(spark, nullptr, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_sparkline_get_range(spark, &min_value, &max_value, &count) == SAO_STATUS_OK);
    CHECK(count == 0);
    CHECK(min_value == 0.0);
    CHECK(max_value == 0.0);

    sao_ui_widget_chart_family_destroy(spark);
    sao_ui_widget_chart_family_destroy(line);
    sao_ui_widget_chart_family_destroy(bar);
}

TEST_CASE("data widgets expose mutable production state", "[ui][completion][data]") {
    SaoUiGaugeSpec gauge_spec{};
    gauge_spec.value = 25.0f;
    gauge_spec.max_value = 100.0f;
    sao_ui_widget_handle_t gauge = nullptr;
    REQUIRE(sao_ui_gauge_create(nullptr, &gauge_spec, &gauge) == SAO_STATUS_OK);
    float ratio = 0.0f;
    REQUIRE(sao_ui_gauge_get_ratio(gauge, &ratio) == SAO_STATUS_OK);
    CHECK(ratio == 0.25f);

    SaoUiStatusBadgeSpec badge_spec{};
    badge_spec.text_utf8 = "ready";
    sao_ui_widget_handle_t badge = nullptr;
    REQUIRE(sao_ui_status_badge_create(nullptr, &badge_spec, &badge) == SAO_STATUS_OK);
    REQUIRE(sao_ui_status_badge_set_text(badge, "running") == SAO_STATUS_OK);
    char text[32]{};
    size_t bytes = 0;
    REQUIRE(sao_ui_status_badge_get_text(badge, text, sizeof(text), &bytes) == SAO_STATUS_OK);
    CHECK(std::string(text) == "running");
    CHECK(bytes == 7);

    SaoUiMetricSpec metric_spec{};
    metric_spec.label_utf8 = "Latency";
    metric_spec.value_utf8 = "14";
    metric_spec.unit_utf8 = "ms";
    sao_ui_widget_handle_t metric = nullptr;
    REQUIRE(sao_ui_metric_create(nullptr, &metric_spec, &metric) == SAO_STATUS_OK);
    REQUIRE(sao_ui_metric_set_value(metric, "9") == SAO_STATUS_OK);
    REQUIRE(sao_ui_metric_get_value(metric, text, sizeof(text), &bytes) == SAO_STATUS_OK);
    CHECK(std::string(text) == "9");

    SaoUiEmptyStateSpec empty_spec{};
    empty_spec.title_utf8 = "No data";
    empty_spec.detail_utf8 = "Waiting";
    sao_ui_widget_handle_t empty = nullptr;
    REQUIRE(sao_ui_empty_state_create(nullptr, &empty_spec, &empty) == SAO_STATUS_OK);
    REQUIRE(sao_ui_empty_state_set_detail(empty, "Connected") == SAO_STATUS_OK);
    REQUIRE(sao_ui_empty_state_get_detail(empty, text, sizeof(text), &bytes) == SAO_STATUS_OK);
    CHECK(std::string(text) == "Connected");

    SaoUiMoreIndicatorSpec more_spec{};
    more_spec.hidden_count = 4;
    sao_ui_widget_handle_t more = nullptr;
    REQUIRE(sao_ui_more_indicator_create(nullptr, &more_spec, &more) == SAO_STATUS_OK);
    REQUIRE(sao_ui_more_indicator_set_count(more, 12) == SAO_STATUS_OK);
    int32_t hidden_count = 0;
    REQUIRE(sao_ui_more_indicator_get_count(more, &hidden_count) == SAO_STATUS_OK);
    CHECK(hidden_count == 12);

    sao_ui_widget_data_family_destroy(more);
    sao_ui_widget_data_family_destroy(empty);
    sao_ui_widget_data_family_destroy(metric);
    sao_ui_widget_data_family_destroy(badge);
    sao_ui_widget_data_family_destroy(gauge);
}
