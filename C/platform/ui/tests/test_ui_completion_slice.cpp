#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

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

namespace {

struct TreeSelection {
    int64_t node_id = 0;
    int32_t calls = 0;
};

void SAO_UI_CALL on_tree_select(int64_t node_id, void* user_data) {
    auto* selection = static_cast<TreeSelection*>(user_data);
    selection->node_id = node_id;
    ++selection->calls;
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
