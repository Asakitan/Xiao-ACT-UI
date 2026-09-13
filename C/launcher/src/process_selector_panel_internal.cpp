#include "process_selector_panel_internal.h"

#include "sao/core/process.h"
#include "sao/rt_io/proxy.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_sdk.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sao::launcher::process_selector_panel {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumActionPayloadBytes = 16U * 1024U;
constexpr std::size_t kMaximumActionIdBytes = 64U;
constexpr std::size_t kMaximumStatusBytes = 1024U;
constexpr std::size_t kMaximumPanelSpecBytes = 256U * 1024U;
constexpr std::size_t kMaximumImagePathBytes = SAO_PROCESS_IMAGE_PATH_MAX - 1U;
constexpr std::size_t kMaximumBaseNameBytes = 1024U;
constexpr std::size_t kProcessesPerPage = 32U;
constexpr std::string_view kPreviousPageAction = "process_selector.page.previous";
constexpr std::string_view kNextPageAction = "process_selector.page.next";
constexpr std::size_t kMaximumSearchQueryBytes = 512U;
constexpr std::size_t kMaximumCollapsedPidSet = 4096U;
constexpr std::uint32_t kAutoRefreshIntervalMs = 5000U;
constexpr int kEnumerationAttempts = 3;

struct VisibleView {
    std::vector<ProcessRecord> processes;
    std::vector<std::uint8_t> depths;
    std::vector<std::uint8_t> has_children;
    std::vector<std::uint8_t> collapsed;
    std::uint32_t root_count{};
};

bool valid_utf8(std::string_view value) noexcept {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (offset + continuation_count >= value.size())
            return false;
        for (std::size_t index = 1; index <= continuation_count; ++index) {
            const auto next = static_cast<unsigned char>(value[offset + index]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80U) ||
                              (continuation_count == 2 && code_point < 0x800U) ||
                              (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        offset += continuation_count + 1U;
    }
    return true;
}

bool valid_text(std::string_view value, std::size_t maximum, bool required) noexcept {
    return (!required || !value.empty()) && value.size() <= maximum &&
           value.find('\0') == std::string_view::npos && valid_utf8(value);
}

std::optional<std::string_view> bounded_c_text(const char* value, std::size_t maximum) noexcept {
    if (value == nullptr)
        return std::nullopt;
    const void* terminator = std::memchr(value, '\0', maximum + 1U);
    if (terminator == nullptr)
        return std::nullopt;
    const auto length = static_cast<std::size_t>(static_cast<const char*>(terminator) - value);
    const std::string_view result(value, length);
    return valid_text(result, maximum, true) ? std::optional<std::string_view>(result)
                                             : std::nullopt;
}

struct CoreProcessCloser {
    void operator()(sao_core_process_handle_t process) const noexcept {
        sao_core_process_close(process);
    }
};

using CoreProcess = std::unique_ptr<sao_core_process_s, CoreProcessCloser>;

std::string lower_ascii(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return lowered;
}

std::string base_name_from_path(std::string_view path) {
    const std::size_t separator = path.find_last_of("\\/");
    if (separator == std::string_view::npos)
        return std::string(path);
    if (separator + 1U >= path.size())
        return {};
    return std::string(path.substr(separator + 1U));
}

std::string bounded_text(std::string value, std::size_t maximum) {
    if (value.size() <= maximum)
        return value;
    if (maximum <= 3U)
        return value.substr(0, maximum);
    std::size_t end = maximum - 3U;
    while (end > 0U && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U)
        --end;
    value.resize(end);
    value.append("...");
    return value;
}

enum class SnapshotState : std::uint8_t { loading, fresh, stale, failed, empty };

SnapshotState snapshot_state(const Snapshot& snapshot) {
    if (snapshot.loading)
        return SnapshotState::loading;
    if (snapshot.last_status != SAO_STATUS_OK)
        return snapshot.all_processes.empty() ? SnapshotState::failed : SnapshotState::stale;
    return snapshot.all_processes.empty() ? SnapshotState::empty : SnapshotState::fresh;
}

std::string_view snapshot_state_label(SnapshotState state) {
    switch (state) {
    case SnapshotState::loading:
        return "loading";
    case SnapshotState::fresh:
        return "fresh";
    case SnapshotState::stale:
        return "stale";
    case SnapshotState::failed:
        return "failed";
    case SnapshotState::empty:
        return "empty";
    }
    return "failed";
}

std::string status_description(sao_status_t status, std::string_view prefix) {
    std::string description(prefix);
    if (!description.empty())
        description.append(": ");
    description.append(sao_status_str(status));
    description.append(" (");
    description.append(std::to_string(status));
    description.push_back(')');
    return bounded_text(std::move(description), kMaximumStatusBytes);
}

std::vector<ProcessRecord> normalize_snapshot(std::vector<ProcessRecord> records,
                                              std::uint32_t current_process_id) {
    std::vector<ProcessRecord> normalized;
    normalized.reserve(records.size());
    for (ProcessRecord& record : records) {
        if (record.pid == 0U || record.pid == 4U || record.pid == current_process_id ||
            record.start_time_100ns == 0U ||
            !valid_text(record.image_path_utf8, kMaximumImagePathBytes, true) ||
            !valid_text(record.base_name_utf8, kMaximumBaseNameBytes, false)) {
            continue;
        }
        if (record.base_name_utf8.empty())
            record.base_name_utf8 = base_name_from_path(record.image_path_utf8);
        if (!valid_text(record.base_name_utf8, kMaximumBaseNameBytes, true))
            continue;
        normalized.push_back(std::move(record));
    }

    std::ranges::sort(normalized, [](const ProcessRecord& left, const ProcessRecord& right) {
        const std::string left_name = lower_ascii(left.base_name_utf8);
        const std::string right_name = lower_ascii(right.base_name_utf8);
        if (left_name != right_name)
            return left_name < right_name;
        if (left.pid != right.pid)
            return left.pid < right.pid;
        return left.start_time_100ns < right.start_time_100ns;
    });
    return normalized;
}

std::string search_terms_of(const ProcessRecord& record) {
    std::string terms = lower_ascii(record.base_name_utf8);
    terms.push_back(' ');
    terms += lower_ascii(record.image_path_utf8);
    terms.push_back(' ');
    terms += std::to_string(record.pid);
    terms.push_back(' ');
    terms += "0x";
    std::array<char, 8> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(),
                                         record.pid, 16);
    if (converted.ec == std::errc{})
        terms.append(digits.data(), converted.ptr);
    return terms;
}

bool matches_search(const ProcessRecord& record, std::string_view needle_lower, bool has_needle) {
    if (!has_needle)
        return true;
    return search_terms_of(record).find(needle_lower) != std::string::npos;
}

bool matches_filter(const ProcessRecord& record, FilterMode filter) {
    return filter == FilterMode::all || is_likely_game_process(record);
}

bool any_descendant_matches(const std::unordered_map<std::uint32_t, std::vector<std::size_t>>&
                                children_by_parent,
                            const std::vector<ProcessRecord>& sorted,
                            std::uint32_t pid, std::string_view needle_lower, FilterMode filter) {
    const auto children = children_by_parent.find(pid);
    if (children == children_by_parent.end())
        return false;
    for (const std::size_t child_index : children->second) {
        const ProcessRecord& child = sorted[child_index];
        if (matches_filter(child, filter) &&
            matches_search(child, needle_lower, !needle_lower.empty())) {
            return true;
        }
        if (any_descendant_matches(children_by_parent, sorted, child.pid, needle_lower, filter))
            return true;
    }
    return false;
}

VisibleView select_visible(const std::vector<ProcessRecord>& processes, FilterMode filter,
                           std::string_view search_query_lower, SortColumn sort_column,
                           SortDirection sort_direction, ViewMode view_mode,
                           const std::unordered_set<std::uint32_t>& collapsed_parents) {
    VisibleView view{};
    const bool has_search = !search_query_lower.empty();
    if (view_mode == ViewMode::flat) {
        view.processes.reserve(processes.size());
        for (const ProcessRecord& record : processes) {
            if (matches_filter(record, filter) &&
                matches_search(record, search_query_lower, has_search)) {
                view.processes.push_back(record);
            }
        }
        const bool descending = sort_direction == SortDirection::descending;
        const auto comparator = [sort_column, descending](const ProcessRecord& left,
                                                          const ProcessRecord& right) {
            bool less = false;
            switch (sort_column) {
            case SortColumn::name: {
                const std::string left_name = lower_ascii(left.base_name_utf8);
                const std::string right_name = lower_ascii(right.base_name_utf8);
                if (left_name != right_name)
                    less = left_name < right_name;
                else if (left.pid != right.pid)
                    less = left.pid < right.pid;
                else
                    less = left.start_time_100ns < right.start_time_100ns;
                break;
            }
            case SortColumn::pid:
                if (left.pid != right.pid)
                    less = left.pid < right.pid;
                else
                    less = left.start_time_100ns < right.start_time_100ns;
                break;
            case SortColumn::parent_pid:
                if (left.parent_pid != right.parent_pid)
                    less = left.parent_pid < right.parent_pid;
                else if (left.pid != right.pid)
                    less = left.pid < right.pid;
                else
                    less = left.start_time_100ns < right.start_time_100ns;
                break;
            }
            return descending ? !less && (left != right) : less;
        };
        std::ranges::sort(view.processes, comparator);
        view.root_count = static_cast<std::uint32_t>(view.processes.size());
        return view;
    }

    std::vector<ProcessRecord> sorted = processes;
    std::ranges::sort(sorted, [](const ProcessRecord& left, const ProcessRecord& right) {
        const std::string left_name = lower_ascii(left.base_name_utf8);
        const std::string right_name = lower_ascii(right.base_name_utf8);
        if (left_name != right_name)
            return left_name < right_name;
        if (left.pid != right.pid)
            return left.pid < right.pid;
        return left.start_time_100ns < right.start_time_100ns;
    });
    std::unordered_map<std::uint32_t, std::size_t> index_by_pid;
    index_by_pid.reserve(sorted.size());
    for (std::size_t index = 0; index < sorted.size(); ++index)
        index_by_pid.emplace(sorted[index].pid, index);
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> children_by_parent;
    std::unordered_set<std::uint32_t> known_pids;
    known_pids.reserve(sorted.size());
    for (const ProcessRecord& record : sorted)
        known_pids.insert(record.pid);
    std::vector<std::size_t> roots;
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        const ProcessRecord& record = sorted[index];
        if (record.parent_pid == 0U || known_pids.find(record.parent_pid) == known_pids.end() ||
            record.parent_pid == record.pid) {
            roots.push_back(index);
        } else {
            children_by_parent[record.parent_pid].push_back(index);
        }
    }
    const auto emit_subtree = [&](auto&& self, std::size_t index, std::uint8_t depth) -> void {
        const ProcessRecord& record = sorted[index];
        const bool matches =
            matches_filter(record, filter) && matches_search(record, search_query_lower, has_search);
        if (has_search || filter != FilterMode::all) {
            if (!matches &&
                !any_descendant_matches(children_by_parent, sorted, record.pid,
                                        search_query_lower, filter)) {
                return;
            }
        }
        const bool collapsed = collapsed_parents.find(record.pid) != collapsed_parents.end();
        const std::size_t row_index = view.processes.size();
        view.processes.push_back(record);
        view.depths.push_back(depth);
        view.has_children.push_back(0U);
        view.collapsed.push_back(collapsed ? 1U : 0U);
        if (collapsed)
            return;
        const auto children = children_by_parent.find(record.pid);
        if (children == children_by_parent.end())
            return;
        std::size_t emitted = 0;
        for (const std::size_t child_index : children->second) {
            const std::size_t before = view.processes.size();
            self(self, child_index, static_cast<std::uint8_t>(
                                        depth < 8U ? depth + 1U : 8U));
            emitted += view.processes.size() - before;
        }
        if (emitted != 0U)
            view.has_children[row_index] = 1U;
    };
    for (const std::size_t root_index : roots) {
        const std::size_t before = view.processes.size();
        emit_subtree(emit_subtree, root_index, 0U);
        if (view.processes.size() > before)
            ++view.root_count;
    }
    return view;
}

std::size_t selected_process_count(const std::vector<ProcessRecord>& processes, FilterMode filter,
                                   std::string_view search_query_lower) {
    if (filter == FilterMode::all && search_query_lower.empty())
        return processes.size();
    std::size_t count = 0;
    for (const ProcessRecord& record : processes) {
        if (matches_filter(record, filter) &&
            matches_search(record, search_query_lower, !search_query_lower.empty())) {
            ++count;
        }
    }
    return count;
}

Json text_node(std::string text, std::string_view style = "value", int height = 24) {
    return Json{{"type", "text"},
                {"text", bounded_text(std::move(text), 4096U)},
                {"style", style},
                {"height", height}};
}

Json button_node(std::string id, std::string label, std::string action, Json payload,
                 std::string_view style = "default", bool disabled = false) {
    Json node{{"type", "button"},
              {"id", std::move(id)},
              {"label", std::move(label)},
              {"action", std::move(action)},
              {"style", style},
              {"height", 36}};
    if (!payload.empty())
        node["payload"] = std::move(payload);
    if (disabled)
        node["disabled"] = true;
    return node;
}

Json badge_node(std::string text, std::string_view style = "muted") {
    return Json{{"type", "badge"},
                {"text", bounded_text(std::move(text), 256U)},
                {"style", style},
                {"height", 22}};
}

Json row_node(Json children) {
    return Json{{"type", "row"}, {"align", "left"}, {"children", std::move(children)}};
}

Json card_node(std::string title, Json children, std::string_view accent = "cyan") {
    return Json{{"type", "card"},
                {"title", bounded_text(std::move(title), 512U)},
                {"accent", accent},
                {"children", std::move(children)}};
}

Json section_node(std::string title, Json children, std::string_view accent = "cyan") {
    return Json{{"type", "section"},
                {"title", bounded_text(std::move(title), 512U)},
                {"accent", accent},
                {"children", std::move(children)}};
}

Json dock_document(Json nodes, std::string content_id, int min_width = 560) {
    if (!nodes.is_array() || nodes.empty())
        return Json{{"version", 1}, {"title", ""}, {"layout", "dock"}, {"nodes", std::move(nodes)}};
    Json top = std::move(nodes.front());
    nodes.erase(nodes.begin());
    top["dock"] = "bottom";
    top["title"] = "";
    Json toolbar = nodes.empty() ? Json{{"type", "section"}, {"children", Json::array()}} : std::move(nodes.front());
    if (!nodes.empty()) nodes.erase(nodes.begin());
    toolbar["dock"] = "top";
    Json content{{"type", "section"},
                 {"id", std::move(content_id)},
                 {"container", true},
                 {"layout", "vertical"},
                 {"width", 0},
                 {"min_width", min_width},
                 {"weight", 1.0F},
                 {"dock", "fill"},
                 {"scroll", {{"axis", "vertical"}, {"bar", "auto"}, {"wheel", true}}},
                 {"children", std::move(nodes)}};
    return Json{{"version", 1},
                {"title", ""},
                {"layout", "dock"},
                {"nodes", Json::array({std::move(toolbar), std::move(top), std::move(content)})}};
}

std::string_view snapshot_accent(SnapshotState state) noexcept {
    if (state == SnapshotState::fresh)
        return "ok";
    if (state == SnapshotState::loading || state == SnapshotState::stale)
        return "gold";
    if (state == SnapshotState::failed)
        return "danger";
    return "cyan";
}

Json status_strip_node(SnapshotState state) {
    const std::string_view accent = snapshot_accent(state);
    std::string_view summary = "Ready / 就绪";
    if (state == SnapshotState::loading)
        summary = "Refreshing / 正在刷新";
    else if (state == SnapshotState::stale)
        summary = "Showing last successful scan / 显示上次成功结果";
    else if (state == SnapshotState::failed)
        summary = "Scan failed / 扫描失败";
    else if (state == SnapshotState::empty)
        summary = "No results yet / 暂无结果";
    return card_node("状态",
                     Json::array({row_node(
                         Json::array({badge_node(std::string(snapshot_state_label(state)), accent),
                                      text_node(std::string(summary), accent, 24)}))}),
                     accent);
}
std::string_view sort_column_label(SortColumn column) noexcept {
    switch (column) {
    case SortColumn::name:
        return "名称";
    case SortColumn::pid:
        return "PID";
    case SortColumn::parent_pid:
        return "父进程";
    }
    return "名称";
}

std::string build_panel_spec(const Snapshot& snapshot) {
    const SnapshotState state = snapshot_state(snapshot);
    const bool attach_allowed = state == SnapshotState::fresh && snapshot.attach_available;
    const std::size_t filtered_count = snapshot.visible_processes.size();
    const std::size_t page_count =
        filtered_count == 0U ? 1U : (filtered_count + kProcessesPerPage - 1U) / kProcessesPerPage;
    const std::size_t page_index = std::min(snapshot.page_index, page_count - 1U);
    const std::size_t page_begin = std::min(page_index * kProcessesPerPage, filtered_count);
    const std::size_t page_end = std::min(page_begin + kProcessesPerPage, filtered_count);
    Json nodes = Json::array();
    nodes.push_back(status_strip_node(state));
    Json filters = Json::array();
    filters.push_back(text_node("进程浏览器", "title", 36));
    Json actions = Json::array();
    actions.push_back(button_node("process.refresh", snapshot.loading ? "正在刷新…" : "刷新",
                                  kRefreshAction, Json::object(), "primary", snapshot.loading));
    actions.push_back(button_node("process.filter.all", "全部", kFilterAction, {{"mode", "all"}},
                                  snapshot.filter == FilterMode::all ? "primary" : "ghost"));
    actions.push_back(
        button_node("process.filter.game", "可能的游戏", kFilterAction, {{"mode", "likely_game"}},
                    snapshot.filter == FilterMode::likely_game ? "primary" : "ghost"));
    filters.push_back(row_node(std::move(actions)));
    Json controls = Json::array();
    Json search_input{{"type", "input"},
                      {"id", "process.search"},
                      {"value", snapshot.search_query},
                      {"action", kSearchAction},
                      {"payload", Json::object()},
                      {"height", 38},
                      {"weight", 1.0},
                      {"min_width", 180}};
    controls.push_back(std::move(search_input));
    Json sort_items = Json::array();
    const std::array<SortColumn, 3> columns{SortColumn::name, SortColumn::pid,
                                            SortColumn::parent_pid};
    for (std::size_t column_index = 0; column_index < columns.size(); ++column_index) {
        sort_items.push_back({{"id", static_cast<std::int32_t>(column_index + 1)},
                              {"label", std::string(sort_column_label(columns[column_index]))},
                              {"value", column_index == 0 ? "name"
                                        : column_index == 1 ? "pid"
                                                            : "ppid"}});
    }
    controls.push_back(Json{{"type", "dropdown"},
                            {"id", "process.sort.column"},
                            {"action", kSortAction},
                            {"payload", Json::object()},
                            {"items", std::move(sort_items)},
                            {"selected_id",
                             static_cast<std::int32_t>(
                                 snapshot.sort_column == SortColumn::name      ? 1
                                 : snapshot.sort_column == SortColumn::pid     ? 2
                                                                               : 3)},
                            {"height", 38},
                            {"width", 112}});
    controls.push_back(button_node(
        "process.sort.direction",
        snapshot.sort_direction == SortDirection::ascending ? "升序" : "降序", kSortAction,
        {{"toggle", true}}, "ghost"));
    controls.push_back(button_node(
        "process.view_mode",
        snapshot.view_mode == ViewMode::flat ? "列表视图" : "树状视图", kViewModeAction,
        {{"mode", snapshot.view_mode == ViewMode::flat ? "tree" : "flat"}}, "ghost"));
    controls.push_back(button_node(
        "process.auto_refresh", snapshot.auto_refresh ? "自动: 开" : "自动: 关",
        kAutoRefreshAction,
        {{"enabled", !snapshot.auto_refresh}},
        snapshot.auto_refresh ? "primary" : "ghost"));
    filters.push_back(row_node(std::move(controls)));
    Json badges = Json::array();
    badges.push_back(Json{{"type", "badge"},
                          {"text", std::string(snapshot_state_label(state))},
                          {"style", state == SnapshotState::fresh    ? "ok"
                                    : state == SnapshotState::stale  ? "warn"
                                    : state == SnapshotState::failed ? "bad"
                                                                     : "accent"},
                          {"height", 22}});
    badges.push_back(
        Json{{"type", "badge"},
             {"text", filtered_count == 0U
                          ? "0 shown"
                          : std::to_string(page_begin + 1U) + "-" + std::to_string(page_end) +
                                " of " + std::to_string(filtered_count) + " shown"},
             {"style", "accent"},
             {"height", 22}});
    badges.push_back(Json{{"type", "badge"},
                          {"text", std::to_string(snapshot.all_processes.size()) + " available"},
                          {"style", "muted"},
                          {"height", 22}});
    badges.push_back(
        badge_node(snapshot.attach_available ? "Attach on / 可附加" : "Attach off / 未连接",
                   snapshot.attach_available ? "ok" : "muted"));
    if (snapshot.view_mode == ViewMode::tree)
        badges.push_back(badge_node(
            "树根 " + std::to_string(snapshot.visible_root_count), "muted"));
    if (!snapshot.search_query.empty())
        badges.push_back(badge_node("搜索: " + snapshot.search_query, "accent"));
    filters.push_back(row_node(std::move(badges)));
    Json paging = Json::array();
    paging.push_back(button_node("process.page.previous", "上一页",
                                 std::string(kPreviousPageAction), Json::object(), "ghost",
                                 snapshot.loading || page_index == 0U));
    paging.push_back(badge_node(
        "第 " + std::to_string(page_index + 1U) + " / " + std::to_string(page_count) + " 页",
        "muted"));
    paging.push_back(button_node("process.page.next", "下一页", std::string(kNextPageAction),
                                 Json::object(), "ghost",
                                 snapshot.loading || page_index + 1U >= page_count));
    for (auto& item : paging)
        nodes.front()["children"][0]["children"].push_back(std::move(item));
    nodes.front()["padding"] = 8;
    nodes.front()["height"] = 60;
    nodes.push_back(section_node("", std::move(filters)));
    if (snapshot.attached_process.has_value()) {
        const ProcessRecord& process = *snapshot.attached_process;
        Json details = Json::array();
        details.push_back(
            row_node(Json::array({badge_node("Attached / 已附加", "ok"),
                                  badge_node("PID " + std::to_string(process.pid), "cyan")})));
        details.push_back(text_node(process.image_path_utf8, "mono", 28));
        Json attached_actions = Json::array();
        attached_actions.push_back(button_node(
            "process.attached.detach", "分离 / Detach", kDetachAction, Json::object(),
            "default", snapshot.loading));
        if (snapshot.memory_viewer_available) {
            attached_actions.push_back(button_node(
                "process.attached.memory_viewer", "打开内存查看器", kOpenMemoryViewerAction,
                Json::object(), "primary", snapshot.loading));
        }
        details.push_back(row_node(std::move(attached_actions)));
        nodes.push_back(section_node(
            "Attached / 已附加",
            Json::array({card_node(process.base_name_utf8, std::move(details), "ok")}), "ok"));
    }
    if (!snapshot.status_text.empty()) {
        const std::string_view style = snapshot.last_status == SAO_STATUS_OK ? "muted" : "bad";
        Json status = Json::array();
        status.push_back(text_node(snapshot.status_text, style, 28));
        if (snapshot.last_status != SAO_STATUS_OK)
            status.push_back(button_node("process.status-retry", "重试", kRefreshAction,
                                         Json::object(), "primary"));
        nodes.push_back(section_node(snapshot.last_status == SAO_STATUS_OK ? "状态" : "错误",
                                     std::move(status),
                                     snapshot.last_status == SAO_STATUS_OK ? "cyan" : "danger"));
    }
    if (snapshot.visible_processes.empty()) {
        Json empty = Json::array();
        empty.push_back(text_node(state == SnapshotState::loading
                                      ? "Scanning running processes... / 正在扫描运行中的进程..."
                                  : !snapshot.search_query.empty()
                                      ? "No processes matched the search. Clear the box or "
                                        "refresh. / 未匹配到搜索内容，请清空搜索框或刷新。"
                                  : snapshot.filter == FilterMode::likely_game
                                      ? "No likely game processes matched. Switch to Show all or "
                                        "refresh. / 未匹配到可能的游戏进程，请切换全部或刷新。"
                                      : "No queryable processes are available. Refresh to try "
                                        "again. / 没有可查询的进程，请刷新重试。",
                                  "muted", 44));
        empty.push_back(button_node("process.empty-refresh", "刷新", kRefreshAction, Json::object(),
                                    "primary", snapshot.loading));
        nodes.push_back(section_node("Process List / 进程列表", std::move(empty)));
    } else {
        Json cards = Json::array();
        for (std::size_t index = page_begin; index < page_end; ++index) {
            const ProcessRecord& process = snapshot.visible_processes[index];
            const std::uint8_t depth =
                index < snapshot.visible_depths.size() ? snapshot.visible_depths[index] : 0U;
            const bool has_children = index < snapshot.visible_has_children.size() &&
                                      snapshot.visible_has_children[index] != 0U;
            const bool collapsed = index < snapshot.visible_collapsed.size() &&
                                   snapshot.visible_collapsed[index] != 0U;
            std::string display_name = process.base_name_utf8;
            if (depth != 0U)
                display_name.insert(0, static_cast<std::size_t>(depth) * 2U, ' ');
            Json identity =
                section_node("", Json::array({
                                     text_node(std::move(display_name), "label", 28),
                                     text_node(process.image_path_utf8, "muted", 24)}));
            identity["padding"] = 0;
            identity["gap"] = 0;
            identity["weight"] = 1.0;
            Json pid = badge_node("PID " + std::to_string(process.pid), "muted");
            pid["width"] = 108;
            if (snapshot.view_mode == ViewMode::tree && process.parent_pid != 0U) {
                pid["text"] = "PID " + std::to_string(process.pid) + " ▸" +
                              std::to_string(process.parent_pid);
            }
            Json category = badge_node(is_likely_game_process(process) ? "可能的游戏" : "进程",
                                       is_likely_game_process(process) ? "ok" : "muted");
            category["width"] = 104;
            Json row_children = Json::array();
            if (snapshot.view_mode == ViewMode::tree && has_children) {
                Json twist = button_node("process.expand." + std::to_string(process.pid),
                                         collapsed ? "▸" : "▾",
                                         std::string(kExpandCollapseAction),
                                         {{"parent_pid", process.pid}}, "ghost");
                twist["width"] = 34;
                row_children.push_back(std::move(twist));
            }
            row_children.push_back(std::move(identity));
            row_children.push_back(std::move(pid));
            row_children.push_back(std::move(category));
            Json attach = button_node(
                "process.attach." + std::to_string(process.pid), "选择并附加", kAttachAction,
                {{"pid", process.pid}, {"start_time_100ns", process.start_time_100ns}}, "default",
                !attach_allowed);
            attach["width"] = 120;
            row_children.push_back(std::move(attach));
            Json row = row_node(std::move(row_children));
            Json item = card_node("", Json::array({std::move(row)}));
            item["padding"] = 12;
            if (depth != 0U)
                item["padding"] = Json{{"top", 12},
                                       {"right", 12},
                                       {"bottom", 12},
                                       {"left", 12 + static_cast<int>(depth) * 14}};
            cards.push_back(std::move(item));
        }
        nodes.push_back(section_node("Process List / 进程列表", std::move(cards), "cyan"));
    }
    std::string serialized = dock_document(std::move(nodes), "process-content").dump();
    if (serialized.size() <= kMaximumPanelSpecBytes)
        return serialized;
    Json compact = Json::array();
    compact.push_back(text_node(
        "The process list exceeded the panel budget. Narrow the search or use Likely games.",
        "bad", 48));
    compact.push_back(button_node("process.refresh.compact", "Refresh", kRefreshAction,
                                  Json::object(), "primary"));
    compact.push_back(button_node("process.filter.compact", "Likely games", kFilterAction,
                                  {{"mode", "likely_game"}}, "primary"));
    return Json{{"version", 1}, {"title", ""}, {"nodes", std::move(compact)}}.dump();
}
sao_status_t query_with_core(std::uint32_t pid, ProcessRecord& out) {
    if (pid == 0U)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        sao_core_process_handle_t raw_process = nullptr;
        sao_status_t status = sao_core_process_open(pid, SAO_PROCESS_ACCESS_INFO, &raw_process);
        if (status != SAO_STATUS_OK)
            return status;
        CoreProcess process(raw_process);
        SaoProcessInfo info{};
        std::array<char, SAO_PROCESS_IMAGE_PATH_MAX> image_path{};
        status =
            sao_core_process_get_info(process.get(), &info, image_path.data(), image_path.size());
        if (status != SAO_STATUS_OK)
            return status;
        ProcessRecord candidate{};
        candidate.pid = info.pid;
        candidate.parent_pid = info.parent_pid;
        candidate.start_time_100ns = info.start_time_100ns;
        const void* terminator = std::memchr(image_path.data(), '\0', image_path.size());
        if (terminator == nullptr)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        const auto length =
            static_cast<std::size_t>(static_cast<const char*>(terminator) - image_path.data());
        candidate.image_path_utf8.assign(image_path.data(), length);
        if (!valid_text(candidate.image_path_utf8, kMaximumImagePathBytes, true))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        candidate.base_name_utf8 = base_name_from_path(candidate.image_path_utf8);
        if (!valid_text(candidate.base_name_utf8, kMaximumBaseNameBytes, true))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        out = std::move(candidate);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t enumerate_with_core(std::vector<ProcessRecord>& out) {
    try {
        for (int attempt = 0; attempt < kEnumerationAttempts; ++attempt) {
            std::size_t count = 0;
            sao_status_t status = sao_core_process_enumerate(nullptr, 0, &count);
            if (status != SAO_STATUS_OK)
                return status;
            std::vector<std::uint32_t> pids(count);
            if (count != 0U) {
                status = sao_core_process_enumerate(pids.data(), pids.size(), &count);
                if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL)
                    continue;
                if (status != SAO_STATUS_OK)
                    return status;
                pids.resize(count);
            }

            std::vector<ProcessRecord> snapshot;
            snapshot.reserve(pids.size());
            for (const std::uint32_t pid : pids) {
                if (pid == 0U || pid == 4U || pid == GetCurrentProcessId())
                    continue;
                ProcessRecord record{};
                if (query_with_core(pid, record) != SAO_STATUS_OK)
                    continue;
                snapshot.push_back(std::move(record));
            }
            out = std::move(snapshot);
            return SAO_STATUS_OK;
        }
        return SAO_STATUS_ERR_CANCELLED;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

std::optional<std::uint64_t> json_unsigned(const Json& object, std::string_view key) {
    const auto value = object.find(std::string(key));
    if (value == object.end())
        return std::nullopt;
    if (value->is_number_unsigned())
        return value->get<std::uint64_t>();
    if (value->is_number_integer()) {
        const std::int64_t signed_value = value->get<std::int64_t>();
        if (signed_value >= 0)
            return static_cast<std::uint64_t>(signed_value);
    }
    return std::nullopt;
}

Json parse_payload(std::string_view payload_json, bool& valid) {
    valid = false;
    if (payload_json.size() > kMaximumActionPayloadBytes ||
        payload_json.find('\0') != std::string_view::npos || !valid_utf8(payload_json)) {
        return {};
    }
    if (payload_json.empty()) {
        valid = true;
        return Json::object();
    }
    Json payload = Json::parse(payload_json.begin(), payload_json.end(), nullptr, false, false);
    valid = !payload.is_discarded() && payload.is_object();
    return valid ? std::move(payload) : Json{};
}

} // namespace

struct Owner::State {
    static std::mutex deferred_mutex;
    static std::vector<std::unique_ptr<State>> deferred_cleanup;
    enum class WorkKind : std::uint8_t {
        refresh,
        attach,
        detach,
    };

    struct WorkItem {
        WorkKind kind{WorkKind::refresh};
        std::uint64_t generation{};
        ProcessIdentity identity{};
        std::chrono::steady_clock::time_point not_before{};
    };

    struct Completion {
        WorkKind kind{WorkKind::refresh};
        std::uint64_t generation{};
        sao_status_t status{SAO_STATUS_OK};
        std::string status_text;
        std::vector<ProcessRecord> processes;
        std::optional<ProcessRecord> attached_process;
    };

    State(sao_ui_compositor_handle_t borrowed_compositor, Operations value)
        : compositor(borrowed_compositor), operations(std::move(value)) {
        if (operations.current_process_id == 0U)
            operations.current_process_id = GetCurrentProcessId();
        worker = std::jthread([this](std::stop_token stop) { worker_main(stop); });
    }

    void worker_main(std::stop_token stop) noexcept {
        while (!stop.stop_requested()) {
            WorkItem item;
            std::function<sao_status_t(std::vector<ProcessRecord>&)> enumerate;
            std::function<sao_status_t(std::uint32_t, ProcessRecord&)> query_process;
            std::function<sao_status_t(std::uint32_t)> attach_operation;
            std::function<sao_status_t()> detach_operation;
            std::uint32_t current_process_id = 0U;
            try {
                std::unique_lock lock(mutex);
                const auto ready = [this] {
                    if (work_items.empty())
                        return false;
                    return work_items.front().not_before <=
                           std::chrono::steady_clock::now();
                };
                if (!worker_cv.wait(lock, stop, ready))
                    break;
                item = std::move(work_items.front());
                work_items.pop_front();
                worker_active = true;
                enumerate = operations.enumerate_snapshot;
                query_process = operations.query_process;
                attach_operation = operations.attach;
                detach_operation = operations.detach;
                current_process_id = operations.current_process_id;
            } catch (...) {
                Completion failure{};
                failure.kind = item.kind;
                failure.generation = item.generation;
                failure.status = SAO_STATUS_ERR_UNKNOWN;
                std::lock_guard lock(mutex);
                worker_active = false;
                completions.push_back(std::move(failure));
                continue;
            }

            Completion completion{};
            completion.kind = item.kind;
            completion.generation = item.generation;
            try {
                if (item.kind == WorkKind::refresh) {
                    if (!enumerate) {
                        completion.status = SAO_STATUS_ERR_NOT_INITIALIZED;
                    } else {
                        completion.status = enumerate(completion.processes);
                        if (completion.status == SAO_STATUS_OK) {
                            completion.processes = normalize_snapshot(
                                std::move(completion.processes), current_process_id);
                        }
                    }
                } else if (item.kind == WorkKind::detach) {
                    if (!detach_operation) {
                        completion.status = SAO_STATUS_ERR_NOT_INITIALIZED;
                        completion.status_text = "Detach operation is not configured.";
                    } else {
                        completion.status = detach_operation();
                        completion.status_text =
                            completion.status == SAO_STATUS_OK
                                ? "Detached the memory target."
                                : status_description(completion.status, "Detach failed");
                    }
                } else if (!query_process || !attach_operation || !detach_operation) {
                    completion.status = SAO_STATUS_ERR_NOT_INITIALIZED;
                    completion.status_text =
                        "Attach, detach, and identity operations are not configured.";
                } else {
                    ProcessRecord candidate{};
                    completion.status = query_process(item.identity.pid, candidate);
                    if (completion.status != SAO_STATUS_OK) {
                        completion.status_text =
                            status_description(completion.status, "Identity recheck failed");
                    } else if (candidate.pid != item.identity.pid ||
                               candidate.start_time_100ns == 0U ||
                               !valid_text(candidate.image_path_utf8, kMaximumImagePathBytes,
                                           true) ||
                               !valid_text(candidate.base_name_utf8, kMaximumBaseNameBytes,
                                           false)) {
                        completion.status = SAO_STATUS_ERR_INVALID_ARGUMENT;
                        completion.status_text =
                            "Identity recheck returned invalid UTF-8 or length.";
                    } else {
                        std::vector<ProcessRecord> normalized = normalize_snapshot(
                            std::vector<ProcessRecord>{std::move(candidate)}, current_process_id);
                        const bool process_missing =
                            normalized.empty() || normalized.front().pid != item.identity.pid;
                        if (process_missing ||
                            normalized.front().start_time_100ns != item.identity.start_time_100ns) {
                            completion.status = SAO_STATUS_ERR_PROCESS_GONE;
                            completion.status_text =
                                process_missing
                                    ? "Attach rejected: process exited before attach."
                                    : "Attach rejected: PID identity changed before attach.";
                        } else {
                            completion.status = attach_operation(item.identity.pid);
                            if (completion.status == SAO_STATUS_OK) {
                                ProcessRecord attached_candidate{};
                                completion.status =
                                    query_process(item.identity.pid, attached_candidate);
                                std::vector<ProcessRecord> attached_snapshot;
                                if (completion.status == SAO_STATUS_OK) {
                                    attached_snapshot = normalize_snapshot(
                                        std::vector<ProcessRecord>{
                                            std::move(attached_candidate)},
                                        current_process_id);
                                    if (attached_snapshot.empty() ||
                                        attached_snapshot.front().identity() != item.identity) {
                                        completion.status = SAO_STATUS_ERR_PROCESS_GONE;
                                    }
                                }
                                if (completion.status == SAO_STATUS_OK) {
                                    ProcessRecord attached =
                                        std::move(attached_snapshot.front());
                                    completion.status_text =
                                        "Attached PID " + std::to_string(attached.pid) +
                                        " · " + attached.base_name_utf8;
                                    completion.attached_process = std::move(attached);
                                } else {
                                    const sao_status_t identity_status = completion.status;
                                    const sao_status_t detach_status = detach_operation();
                                    completion.status = detach_status == SAO_STATUS_OK
                                                            ? identity_status
                                                            : detach_status;
                                    completion.status_text = status_description(
                                        completion.status,
                                        detach_status == SAO_STATUS_OK
                                            ? "Post-attach identity verification failed"
                                            : "Post-attach identity rollback failed");
                                }
                            } else {
                                completion.status_text =
                                    status_description(completion.status, "Attach failed");
                            }
                        }
                    }
                }
            } catch (const std::bad_alloc&) {
                completion.status = SAO_STATUS_ERR_UNKNOWN;
            } catch (...) {
                completion.status = SAO_STATUS_ERR_OS_CALL_FAILED;
            }

            {
                std::lock_guard lock(mutex);
                worker_active = false;
                completions.push_back(std::move(completion));
                while (completions.size() > 16U)
                    completions.pop_front();
            }
        }
    }

    void request_shutdown() noexcept {
        {
            std::lock_guard lock(mutex);
            accepting = false;
            work_items.clear();
            pending_attach.reset();
            attached_notification_pending.reset();
            detached_notification_pending = false;
            if (requested_generation != (std::numeric_limits<std::uint64_t>::max)())
                ++requested_generation;
        }
        worker_cv.notify_all();
        if (worker.joinable())
            worker.request_stop();
    }

    Owner* owner{};
    sao_status_t fail_next_unregister_status{SAO_STATUS_OK};
    sao_status_t fail_next_action_restore_status{SAO_STATUS_OK};
    sao_status_t fail_next_event_restore_status{SAO_STATUS_OK};
    sao_ui_compositor_handle_t compositor{};
    Operations operations;
    mutable std::mutex mutex;
    std::condition_variable_any worker_cv;
    std::deque<WorkItem> work_items;
    std::deque<Completion> completions;
    std::optional<WorkItem> pending_attach;
    std::optional<std::pair<ProcessRecord, std::uint64_t>> attached_notification_pending;
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    std::size_t operations_in_flight{};
    std::size_t callbacks_in_flight{};
    bool visible{};
    bool accepting{true};
    bool creating{};
    bool retiring{};
    bool action_handler_attached{};
    bool event_handler_attached{};
    bool loading{};
    bool worker_active{};
    std::chrono::steady_clock::time_point worker_clock_starting{};
    bool dirty{true};
    bool refresh_authoritative{};
    bool detached_notification_pending{};
    std::uint64_t requested_generation{};
    FilterMode filter{FilterMode::all};
    std::string search_query;
    std::string search_query_lower;
    SortColumn sort_column{SortColumn::name};
    SortDirection sort_direction{SortDirection::ascending};
    ViewMode view_mode{ViewMode::flat};
    bool auto_refresh{};
    std::chrono::steady_clock::time_point busy_since{};
    std::chrono::steady_clock::time_point next_auto_refresh_at{};
    std::unordered_set<std::uint32_t> collapsed_parents;
    std::size_t page_index{};
    sao_status_t last_status{SAO_STATUS_OK};
    std::string status_text{"Not refreshed yet."};
    std::vector<ProcessRecord> processes;
    std::optional<ProcessRecord> attached_process;
    std::string rendered_spec_json;
    std::jthread worker;
};

std::mutex Owner::deferred_mutex_;
std::vector<std::unique_ptr<Owner::State>> Owner::deferred_cleanup_;

void Owner::defer_state(std::unique_ptr<State> state) noexcept {
    if (state == nullptr)
        return;
    state->owner = nullptr;
    {
        std::lock_guard lock(state->mutex);
        state->accepting = false;
        state->retiring = false;
    }
    std::lock_guard lock(deferred_mutex_);
    deferred_cleanup_.push_back(std::move(state));
}

void Owner::drain_deferred_cleanup() noexcept {
    std::vector<std::unique_ptr<State>> pending;
    {
        std::lock_guard lock(deferred_mutex_);
        pending.swap(deferred_cleanup_);
    }
    std::vector<std::unique_ptr<State>> retry;
    for (auto& state : pending) {
        auto owner =
            std::unique_ptr<Owner>(new (std::nothrow) Owner(std::move(state), AdoptStateTag{}));
        if (owner == nullptr) {
            retry.push_back(std::move(state));
            continue;
        }
        const sao_status_t status = owner->take_offline();
        state = std::move(owner->state_);
        if (status != SAO_STATUS_OK)
            retry.push_back(std::move(state));
    }
    if (!retry.empty()) {
        std::lock_guard lock(deferred_mutex_);
        for (auto& state : retry)
            deferred_cleanup_.push_back(std::move(state));
    }
}

void Owner::drain_deferred_cleanup_for_owner() noexcept {
    drain_deferred_cleanup();
}

void Owner::drain_deferred_cleanup_for_testing() noexcept {
    drain_deferred_cleanup_for_owner();
}

struct Owner::OperationGuard {
    explicit OperationGuard(Owner& value) noexcept
        : owner(&value), status(value.begin_operation()) {
        if (status != SAO_STATUS_OK)
            owner = nullptr;
    }

    ~OperationGuard() {
        if (owner != nullptr)
            owner->end_operation();
    }

    Owner* owner{};
    sao_status_t status{SAO_STATUS_ERR_NOT_INITIALIZED};
};

Operations make_default_operations(sao_rt_io_proxy_handle_t proxy) {
    Operations operations{};
    operations.current_process_id = GetCurrentProcessId();
    operations.enumerate_snapshot = &enumerate_with_core;
    operations.query_process = &query_with_core;
    if (proxy != nullptr) {
        operations.attach = [proxy](std::uint32_t pid) {
            return sao_rt_io_proxy_attach(proxy, pid);
        };
        operations.detach = [proxy] {
            return sao_rt_io_proxy_detach(proxy);
        };
    }
    return operations;
}

bool is_likely_game_process(const ProcessRecord& process) {
    const std::string name =
        lower_ascii(process.base_name_utf8.empty() ? base_name_from_path(process.image_path_utf8)
                                                   : process.base_name_utf8);
    const std::string path = lower_ascii(process.image_path_utf8);
    const bool executable = name.ends_with(".exe") || name.ends_with(".com");
    if (!executable)
        return false;

    constexpr std::array<std::string_view, 8> path_markers{
        "\\steamapps\\common\\", "/steamapps/common/", "\\epic games\\", "\\gog galaxy\\games\\",
        "\\riot games\\",        "\\xboxgames\\",      "\\games\\",      "/games/"};
    if (std::ranges::any_of(path_markers, [&](std::string_view marker) {
            return path.find(marker) != std::string::npos;
        })) {
        return true;
    }

    constexpr std::array<std::string_view, 9> name_markers{
        "-win64-shipping.exe", "_win64_shipping.exe", "game.exe",
        "gameclient.exe",      "game-client.exe",     "unity.exe",
        "unreal.exe",          "client-win64",        "shipping.exe"};
    return std::ranges::any_of(name_markers, [&](std::string_view marker) {
        return name.find(marker) != std::string::npos;
    });
}

Owner::Owner(sao_ui_compositor_handle_t compositor, sao_rt_io_proxy_handle_t proxy)
    : Owner(compositor, make_default_operations(proxy)) {}

Owner::Owner(sao_ui_compositor_handle_t compositor, Operations operations)
    : state_(std::make_unique<State>(compositor, std::move(operations))) {
    state_->owner = this;
}

Owner::Owner(std::unique_ptr<State> state, AdoptStateTag) noexcept : state_(std::move(state)) {
    if (state_)
        state_->owner = this;
}

Owner::~Owner() noexcept {
    if (!state_)
        return;
    if (take_offline() == SAO_STATUS_OK)
        return;
    auto state = std::move(state_);
    defer_state(std::move(state));
}

sao_status_t Owner::require_owner_thread() const noexcept {
    if (!state_ || state_->compositor == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return sao_ui_compositor_require_owner_thread(state_->compositor);
}

sao_status_t Owner::begin_operation() noexcept {
    const sao_status_t owner_status = require_owner_thread();
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
    std::lock_guard lock(state_->mutex);
    if (!state_->accepting || state_->retiring)
        return SAO_STATUS_ERR_CANCELLED;
    ++state_->operations_in_flight;
    return SAO_STATUS_OK;
}

void Owner::end_operation() noexcept {
    if (!state_)
        return;
    std::lock_guard lock(state_->mutex);
    if (state_->operations_in_flight != 0U)
        --state_->operations_in_flight;
}

bool Owner::begin_callback() noexcept {
    if (!state_)
        return false;
    std::lock_guard lock(state_->mutex);
    if (!state_->accepting || state_->retiring)
        return false;
    ++state_->callbacks_in_flight;
    return true;
}

void Owner::fail_next_unregister_for_testing(sao_status_t status) noexcept {
    if (state_) {
        std::lock_guard lock(state_->mutex);
        state_->fail_next_unregister_status = status;
    }
}

void Owner::fail_next_handler_restore_for_testing(sao_status_t action_status,
                                                  sao_status_t event_status) noexcept {
    if (state_) {
        std::lock_guard lock(state_->mutex);
        state_->fail_next_action_restore_status = action_status;
        state_->fail_next_event_restore_status = event_status;
    }
}

void Owner::end_callback() noexcept {
    if (!state_)
        return;
    std::lock_guard lock(state_->mutex);
    if (state_->callbacks_in_flight != 0U)
        --state_->callbacks_in_flight;
}

void SAO_UI_CALL Owner::panel_action_callback(const char* action_id_utf8,
                                              const std::uint8_t* payload_json_utf8,
                                              std::size_t payload_len, void* user_data) noexcept {
    auto* state = static_cast<State*>(user_data);
    Owner* owner = state == nullptr ? nullptr : state->owner;
    const auto action = bounded_c_text(action_id_utf8, kMaximumActionIdBytes);
    if (owner == nullptr || !action.has_value() ||
        (payload_json_utf8 == nullptr && payload_len != 0U) || !owner->begin_callback()) {
        return;
    }
    struct CallbackGuard {
        Owner& owner;
        ~CallbackGuard() {
            owner.end_callback();
        }
    } callback{*owner};
    try {
        const std::string_view payload(
            payload_json_utf8 == nullptr ? "" : reinterpret_cast<const char*>(payload_json_utf8),
            payload_len);
        (void)owner->dispatch_action(*action, payload);
    } catch (...) {
    }
}

void SAO_UI_CALL Owner::panel_event_callback(std::int32_t event_kind, void* user_data) noexcept {
    auto* state = static_cast<State*>(user_data);
    Owner* owner = state == nullptr ? nullptr : state->owner;
    if (owner == nullptr || !owner->begin_callback())
        return;
    struct CallbackGuard {
        Owner& owner;
        ~CallbackGuard() {
            owner.end_callback();
        }
    } callback{*owner};
    try {
        owner->handle_panel_event(event_kind);
    } catch (...) {
    }
}

void Owner::handle_panel_event(std::int32_t event_kind) noexcept {
    if (event_kind == SAO_UI_PANEL_EVENT_CLOSE) {
        (void)close();
        return;
    }
    if (event_kind != SAO_UI_PANEL_EVENT_SHOW && event_kind != SAO_UI_PANEL_EVENT_HIDE)
        return;
    std::lock_guard lock(state_->mutex);
    state_->visible = event_kind == SAO_UI_PANEL_EVENT_SHOW;
}

sao_status_t Owner::ensure_panel() noexcept {
    const sao_status_t owner_status = require_owner_thread();
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->accepting || state_->retiring)
            return SAO_STATUS_ERR_CANCELLED;
        if (state_->panel != nullptr && state_->body != nullptr)
            return SAO_STATUS_OK;
        if (state_->panel != nullptr || state_->body != nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (state_->creating)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        state_->creating = true;
    }
    struct CreationGuard {
        State& state;
        ~CreationGuard() {
            std::lock_guard lock(state.mutex);
            state.creating = false;
        }
    } creation{*state_};

    SaoPanelDescriptor descriptor{};
    descriptor.struct_size = sizeof(SaoPanelDescriptor);
    descriptor.panel_id_utf8 = kPanelId;
    descriptor.title_utf8 = kPanelTitle;
    descriptor.anchor = SAO_UI_PANEL_ANCHOR_CENTER;
    descriptor.default_width_px = 760;
    descriptor.default_height_px = 680;
    descriptor.min_width_px = 560;
    descriptor.min_height_px = 420;
    descriptor.max_width_px = 1280;
    descriptor.max_height_px = 1080;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = false;
    descriptor.remember_geometry = true;
    descriptor.modal = false;
    descriptor.overlay_style = false;
    descriptor.z_class = SAO_UI_PANEL_Z_NORMAL;
    descriptor.theme_override_json_utf8 = nullptr;
    descriptor.initial_opacity = 1.0F;
    descriptor.auto_scroll = true;

    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    sao_status_t status = sao_ui_panel_register(state_->compositor, &descriptor, &panel, &body);
    if (status != SAO_STATUS_OK)
        return status;

    bool action_attached = false;
    bool event_attached = false;
    status = sao_ui_panel_set_action_handler(panel, &Owner::panel_action_callback, state_.get());
    action_attached = status == SAO_STATUS_OK;
    if (status == SAO_STATUS_OK) {
        status = sao_ui_panel_set_event_handler(panel, &Owner::panel_event_callback, state_.get());
        event_attached = status == SAO_STATUS_OK;
    }
    if (status != SAO_STATUS_OK) {
        sao_status_t rollback_status = SAO_STATUS_OK;
        if (event_attached) {
            const sao_status_t detach_status =
                sao_ui_panel_set_event_handler(panel, nullptr, nullptr);
            if (detach_status == SAO_STATUS_OK)
                event_attached = false;
            else
                rollback_status = detach_status;
        }
        if (action_attached) {
            const sao_status_t detach_status =
                sao_ui_panel_set_action_handler(panel, nullptr, nullptr);
            if (detach_status == SAO_STATUS_OK)
                action_attached = false;
            else if (rollback_status == SAO_STATUS_OK)
                rollback_status = detach_status;
        }
        const sao_status_t unregister_status = sao_ui_panel_unregister(panel);
        if (unregister_status == SAO_STATUS_OK)
            return rollback_status == SAO_STATUS_OK ? status : rollback_status;
        {
            std::lock_guard lock(state_->mutex);
            state_->panel = panel;
            state_->body = body;
            state_->action_handler_attached = action_attached;
            state_->event_handler_attached = event_attached;
            state_->accepting = false;
        }
        return rollback_status == SAO_STATUS_OK ? unregister_status
                                                : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
    }

    {
        std::lock_guard lock(state_->mutex);
        state_->panel = panel;
        state_->body = body;
        state_->action_handler_attached = true;
        state_->event_handler_attached = true;
        state_->dirty = true;
    }
    return SAO_STATUS_OK;
}

sao_status_t Owner::publish() noexcept {
    try {
        Snapshot view{};
        sao_ui_panel_body_handle_t body = nullptr;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->body == nullptr)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (!state_->dirty && !state_->rendered_spec_json.empty())
                return SAO_STATUS_OK;
            body = state_->body;
            state_->dirty = false;
            view.panel_created = true;
            view.visible = state_->visible;
            view.loading = state_->loading;
            view.filter = state_->filter;
            view.search_query = state_->search_query;
            view.sort_column = state_->sort_column;
            view.sort_direction = state_->sort_direction;
            view.view_mode = state_->view_mode;
            view.auto_refresh = state_->auto_refresh;
            view.auto_refresh_interval_ms = kAutoRefreshIntervalMs;
            view.page_index = state_->page_index;
            view.attach_available = state_->operations.query_process &&
                                    state_->operations.attach && state_->operations.detach;
            view.last_status = state_->last_status;
            view.status_text = state_->status_text;
            view.all_processes = state_->processes;
            VisibleView visible_view =
                select_visible(view.all_processes, view.filter, state_->search_query_lower,
                               view.sort_column, view.sort_direction, view.view_mode,
                               state_->collapsed_parents);
            view.visible_root_count = visible_view.root_count;
            view.visible_processes = std::move(visible_view.processes);
            view.visible_depths = std::move(visible_view.depths);
            view.visible_has_children = std::move(visible_view.has_children);
            view.visible_collapsed = std::move(visible_view.collapsed);
            view.attached_process = state_->attached_process;
            view.memory_viewer_available = static_cast<bool>(state_->operations.open_memory_viewer);
        }
        std::string spec = build_panel_spec(view);
        const sao_status_t status = sao_ui_panel_body_set_spec(
            body, reinterpret_cast<const std::uint8_t*>(spec.data()), spec.size());
        std::lock_guard lock(state_->mutex);
        if (state_->body != body)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (status == SAO_STATUS_OK)
            state_->rendered_spec_json = std::move(spec);
        else
            state_->dirty = true;
        return status;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

void Owner::request_shutdown() noexcept {
    if (state_ != nullptr)
        state_->request_shutdown();
}

sao_status_t Owner::open() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;

    const sao_status_t refresh_status = refresh();
    sao_ui_panel_handle_t panel = nullptr;
    {
        std::lock_guard lock(state_->mutex);
        panel = state_->panel;
    }
    if (panel == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    const sao_status_t show_status = sao_ui_panel_show(panel);
    if (show_status != SAO_STATUS_OK)
        return show_status;
    const sao_status_t front_status = sao_ui_panel_bring_to_front(panel);
    if (front_status != SAO_STATUS_OK)
        return front_status;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->panel == panel)
            state_->visible = true;
    }
    return refresh_status;
}

sao_status_t Owner::service_ui() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    try {
        std::deque<State::Completion> completions;
        bool publish_needed = false;
        bool wake_worker = false;
        std::optional<State::WorkItem> pending_attach;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->panel == nullptr || state_->body == nullptr)
                return SAO_STATUS_OK;
            pending_attach = state_->pending_attach;
        }
        if (pending_attach.has_value()) {
            const sao_status_t drain_status = state_->operations.before_attach
                                                  ? state_->operations.before_attach()
                                                  : SAO_STATUS_OK;
            std::lock_guard lock(state_->mutex);
            if (state_->pending_attach.has_value() &&
                state_->pending_attach->generation == pending_attach->generation &&
                state_->pending_attach->identity == pending_attach->identity &&
                state_->requested_generation == pending_attach->generation) {
                if (drain_status == SAO_STATUS_OK) {
                    state_->work_items.push_back(*pending_attach);
                    state_->pending_attach.reset();
                    state_->attached_notification_pending.reset();
                    state_->detached_notification_pending = false;
                    state_->attached_process.reset();
                    state_->last_status = SAO_STATUS_OK;
                    state_->status_text =
                        "Verifying identity and attaching PID " +
                        std::to_string(pending_attach->identity.pid) + ".";
                    wake_worker = true;
                } else {
                    state_->last_status = drain_status;
                    state_->status_text =
                        "Waiting for the previous memory target to drain (status " +
                        std::to_string(drain_status) + ").";
                }
                state_->dirty = true;
                publish_needed = true;
            }
        }
        if (wake_worker)
            state_->worker_cv.notify_all();

        std::optional<std::pair<ProcessRecord, std::uint64_t>> attached_notification;
        bool detached_notification = false;
        const auto service_now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(state_->mutex);
            if (state_->auto_refresh && state_->refresh_authoritative &&
                !state_->loading && !state_->worker_active &&
                state_->work_items.empty() && !state_->pending_attach.has_value()) {
                const auto now = std::chrono::steady_clock::now();
                if (state_->busy_since.time_since_epoch().count() != 0 &&
                    now >= state_->busy_since + std::chrono::seconds(15U)) {
                    state_->busy_since = {};
                    state_->auto_refresh = false;
                    state_->last_status = SAO_STATUS_OK;
                    state_->status_text =
                        "Auto refresh paused: an operation was busy for 15 seconds. Toggle "
                        "auto refresh or press 刷新 to resume.";
                    state_->dirty = true;
                    publish_needed = true;
                } else if (state_->next_auto_refresh_at.time_since_epoch().count() == 0) {
                    state_->next_auto_refresh_at = now;
                } else if (now >= state_->next_auto_refresh_at) {
                    const std::chrono::steady_clock::time_point due =
                        state_->next_auto_refresh_at + std::chrono::milliseconds(
                                                           kAutoRefreshIntervalMs);
                    state_->next_auto_refresh_at = due;
                    state_->busy_since = {};
                    if (state_->requested_generation !=
                        (std::numeric_limits<std::uint64_t>::max)()) {
                        ++state_->requested_generation;
                        State::WorkItem follow_up{State::WorkKind::refresh,
                                                  state_->requested_generation, {}, due};
                        state_->work_items.push_back(std::move(follow_up));
                        state_->loading = true;
                        state_->refresh_authoritative = false;
                        state_->last_status = SAO_STATUS_OK;
                        state_->status_text = "Auto refreshing running processes...";
                        wake_worker = true;
                        state_->dirty = true;
                        publish_needed = true;
                    } else {
                        state_->busy_since = now;
                    }
                }
            }
            completions.swap(state_->completions);
            while (!completions.empty()) {
                State::Completion completion = std::move(completions.front());
                completions.pop_front();
                if (completion.generation != state_->requested_generation)
                    continue;
                state_->loading = false;
                state_->last_status = completion.status;
                if (completion.kind == State::WorkKind::refresh) {
                    state_->refresh_authoritative = completion.status == SAO_STATUS_OK;
                    if (completion.status == SAO_STATUS_OK) {
                        state_->processes = std::move(completion.processes);
                        if (state_->attached_process.has_value() &&
                            !std::ranges::any_of(
                                state_->processes,
                                [&](const ProcessRecord& process) {
                                    return process.identity() ==
                                           state_->attached_process->identity();
                                })) {
                            state_->attached_process.reset();
                            state_->attached_notification_pending.reset();
                            state_->detached_notification_pending = true;
                        }
                        const std::size_t count =
                            selected_process_count(state_->processes, state_->filter,
                                                   state_->search_query_lower);
                        const std::size_t maximum_page =
                            count == 0U ? 0U : (count - 1U) / kProcessesPerPage;
                        state_->page_index = std::min(state_->page_index, maximum_page);
                        state_->status_text =
                            "Refresh complete: " + std::to_string(state_->processes.size()) +
                            " queryable processes.";
                    } else {
                        state_->status_text =
                            status_description(completion.status, "Refresh failed");
                        if (state_->auto_refresh)
                            state_->busy_since = service_now;
                    }
                } else {
                    state_->status_text =
                        completion.status_text.empty()
                            ? status_description(completion.status, "Operation failed")
                            : std::move(completion.status_text);
                    if (completion.kind == State::WorkKind::detach) {
                        if (completion.status == SAO_STATUS_OK) {
                            state_->attached_process.reset();
                            state_->attached_notification_pending.reset();
                            state_->detached_notification_pending = true;
                        } else if (state_->auto_refresh) {
                            state_->busy_since = service_now;
                        }
                    } else if (completion.status == SAO_STATUS_OK &&
                               completion.attached_process.has_value()) {
                        state_->attached_notification_pending.emplace(
                            *completion.attached_process,
                            completion.generation);
                        state_->detached_notification_pending = false;
                        state_->attached_process = std::move(completion.attached_process);
                    } else if (state_->auto_refresh && completion.status != SAO_STATUS_OK) {
                        state_->busy_since = service_now;
                    }
                }
                state_->dirty = true;
                publish_needed = true;
            }
            attached_notification = state_->attached_notification_pending;
            detached_notification = state_->detached_notification_pending;
            publish_needed = publish_needed || state_->dirty;
        }
        if (attached_notification.has_value()) {
            const sao_status_t notification_status = state_->operations.on_attached
                                                         ? state_->operations.on_attached(
                                                               attached_notification->first,
                                                               attached_notification->second)
                                                         : SAO_STATUS_OK;
            std::lock_guard lock(state_->mutex);
            if (state_->attached_notification_pending == attached_notification) {
                if (notification_status == SAO_STATUS_OK) {
                    state_->attached_notification_pending.reset();
                    state_->last_status = SAO_STATUS_OK;
                    state_->status_text =
                        "Attached PID " + std::to_string(attached_notification->first.pid) +
                        " · " + attached_notification->first.base_name_utf8;
                } else if (notification_status == SAO_STATUS_ERR_NOT_FOUND ||
                           notification_status == SAO_STATUS_ERR_PROCESS_GONE) {
                    state_->attached_notification_pending.reset();
                    if (state_->attached_process.has_value() &&
                        state_->attached_process->identity() ==
                            attached_notification->first.identity()) {
                        state_->attached_process.reset();
                    }
                    state_->detached_notification_pending = true;
                    state_->last_status = SAO_STATUS_ERR_PROCESS_GONE;
                    state_->status_text =
                        "Attached process exited before memory handoff completed.";
                } else {
                    state_->last_status = notification_status;
                    state_->status_text =
                        "AI Editor memory handoff is pending (status " +
                        std::to_string(notification_status) + ").";
                }
                state_->dirty = true;
                publish_needed = true;
            }
        }
        if (detached_notification) {
            const sao_status_t notification_status = state_->operations.on_detached
                                                         ? state_->operations.on_detached()
                                                         : SAO_STATUS_OK;
            std::lock_guard lock(state_->mutex);
            if (state_->detached_notification_pending) {
                if (notification_status == SAO_STATUS_OK) {
                    state_->detached_notification_pending = false;
                } else {
                    state_->last_status = notification_status;
                    state_->status_text =
                        "AI Editor memory detach is pending (status " +
                        std::to_string(notification_status) + ").";
                }
                state_->dirty = true;
                publish_needed = true;
            }
        }
        return publish_needed ? publish() : SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t Owner::close() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    sao_ui_panel_handle_t panel = nullptr;
    {
        std::lock_guard lock(state_->mutex);
        panel = state_->panel;
    }
    if (panel == nullptr)
        return SAO_STATUS_OK;
    const sao_status_t status = sao_ui_panel_hide(panel);
    if (status == SAO_STATUS_OK) {
        std::lock_guard lock(state_->mutex);
        if (state_->panel == panel)
            state_->visible = false;
    }
    return status;
}

sao_status_t Owner::take_offline() noexcept {
    if (!state_)
        return SAO_STATUS_OK;
    const auto attachment_cleanup_needed = [](const State& state) {
        return state.attached_process.has_value() ||
               state.attached_notification_pending.has_value() ||
               state.detached_notification_pending ||
               std::ranges::any_of(state.completions, [](const State::Completion& completion) {
                   return completion.kind == State::WorkKind::attach;
               });
    };
    {
        std::lock_guard lock(state_->mutex);
        if (state_->panel == nullptr && state_->body == nullptr && !state_->creating &&
            !state_->retiring && state_->operations_in_flight == 0U &&
            state_->callbacks_in_flight == 0U && !attachment_cleanup_needed(*state_)) {
            return SAO_STATUS_OK;
        }
    }
    const sao_status_t owner_status = require_owner_thread();
    if (owner_status != SAO_STATUS_OK)
        return owner_status;

    sao_ui_panel_handle_t panel = nullptr;
    bool had_action_handler = false;
    bool had_event_handler = false;
    bool was_accepting = false;
    bool cleanup_required = false;
    std::function<sao_status_t()> detach_notification;
    std::function<sao_status_t()> detach_operation;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->creating || state_->retiring || state_->operations_in_flight != 0U ||
            state_->callbacks_in_flight != 0U || state_->worker_active ||
            !state_->work_items.empty()) {
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        }
        if ((state_->panel == nullptr) != (state_->body == nullptr))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        state_->retiring = true;
        was_accepting = state_->accepting;
        state_->accepting = false;
        cleanup_required = attachment_cleanup_needed(*state_);
        detach_notification = state_->operations.on_detached;
        detach_operation = state_->operations.detach;
        panel = state_->panel;
        had_action_handler = state_->action_handler_attached;
        had_event_handler = state_->event_handler_attached;
    }

    if (cleanup_required) {
        sao_status_t cleanup_status = SAO_STATUS_OK;
        try {
            if (detach_notification) {
                cleanup_status = detach_notification();
                if (cleanup_status == SAO_STATUS_ERR_NOT_INITIALIZED && detach_operation)
                    cleanup_status = detach_operation();
            } else if (detach_operation) {
                cleanup_status = detach_operation();
            } else {
                cleanup_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            }
        } catch (const std::bad_alloc&) {
            cleanup_status = SAO_STATUS_ERR_UNKNOWN;
        } catch (...) {
            cleanup_status = SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        if (cleanup_status != SAO_STATUS_OK) {
            std::lock_guard lock(state_->mutex);
            state_->accepting = was_accepting;
            state_->retiring = false;
            return cleanup_status;
        }
    }

    {
        std::lock_guard lock(state_->mutex);
        if (!state_->retiring || state_->panel != panel) {
            state_->retiring = false;
            return SAO_STATUS_ERR_HANDLE_INVALID;
        }
        if (state_->requested_generation != (std::numeric_limits<std::uint64_t>::max)())
            ++state_->requested_generation;
        state_->work_items.clear();
        state_->pending_attach.reset();
        state_->attached_notification_pending.reset();
        state_->detached_notification_pending = false;
        state_->attached_process.reset();
        if (cleanup_required) {
            state_->last_status = SAO_STATUS_OK;
            state_->status_text = "Memory target detached.";
        }
        state_->loading = false;
        state_->refresh_authoritative = false;
        state_->completions.clear();
        state_->dirty = true;
        if (panel == nullptr) {
            state_->visible = false;
            state_->accepting = true;
            state_->retiring = false;
            return SAO_STATUS_OK;
        }
    }

    bool action_attached = had_action_handler;
    bool event_attached = had_event_handler;
    sao_status_t status = SAO_STATUS_OK;
    if (had_event_handler) {
        status = sao_ui_panel_set_event_handler(panel, nullptr, nullptr);
        if (status == SAO_STATUS_OK)
            event_attached = false;
    }
    if (status == SAO_STATUS_OK && had_action_handler) {
        status = sao_ui_panel_set_action_handler(panel, nullptr, nullptr);
        if (status == SAO_STATUS_OK)
            action_attached = false;
    }
    if (status == SAO_STATUS_OK) {
        sao_status_t injected = SAO_STATUS_OK;
        {
            std::lock_guard lock(state_->mutex);
            injected = std::exchange(state_->fail_next_unregister_status, SAO_STATUS_OK);
        }
        status = injected == SAO_STATUS_OK ? sao_ui_panel_unregister(panel) : injected;
    }

    if (status == SAO_STATUS_OK) {
        std::lock_guard lock(state_->mutex);
        if (state_->panel != panel) {
            state_->retiring = false;
            return SAO_STATUS_ERR_HANDLE_INVALID;
        }
        state_->panel = nullptr;
        state_->body = nullptr;
        state_->visible = false;
        state_->action_handler_attached = false;
        state_->event_handler_attached = false;
        state_->auto_refresh = false;
        state_->busy_since = {};
        state_->next_auto_refresh_at = {};
        state_->rendered_spec_json.clear();
        state_->dirty = true;
        state_->accepting = true;
        state_->retiring = false;
        return SAO_STATUS_OK;
    }

    bool rollback_ok = true;
    if (had_action_handler && !action_attached) {
        sao_status_t injected = SAO_STATUS_OK;
        {
            std::lock_guard lock(state_->mutex);
            injected = std::exchange(state_->fail_next_action_restore_status, SAO_STATUS_OK);
        }
        const sao_status_t restore_status =
            injected == SAO_STATUS_OK ? sao_ui_panel_set_action_handler(
                                            panel, &Owner::panel_action_callback, state_.get())
                                      : injected;
        action_attached = restore_status == SAO_STATUS_OK;
        rollback_ok = rollback_ok && action_attached;
    }
    if (had_event_handler && !event_attached) {
        sao_status_t injected = SAO_STATUS_OK;
        {
            std::lock_guard lock(state_->mutex);
            injected = std::exchange(state_->fail_next_event_restore_status, SAO_STATUS_OK);
        }
        const sao_status_t restore_status =
            injected == SAO_STATUS_OK
                ? sao_ui_panel_set_event_handler(panel, &Owner::panel_event_callback, state_.get())
                : injected;
        event_attached = restore_status == SAO_STATUS_OK;
        rollback_ok = rollback_ok && event_attached;
    }
    {
        std::lock_guard lock(state_->mutex);
        state_->action_handler_attached = action_attached;
        state_->event_handler_attached = event_attached;
        state_->accepting = was_accepting && rollback_ok && action_attached == had_action_handler &&
                            event_attached == had_event_handler;
        state_->retiring = false;
    }
    return rollback_ok ? status : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
}

sao_status_t Owner::refresh() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;

    return enqueue_refresh();
}

sao_status_t Owner::enqueue_refresh() noexcept {
    sao_status_t enqueue_status = SAO_STATUS_OK;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->accepting || state_->retiring)
            return SAO_STATUS_ERR_CANCELLED;
        if (state_->loading || state_->worker_active || !state_->work_items.empty() ||
            state_->pending_attach.has_value())
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        if (!state_->operations.enumerate_snapshot) {
            state_->loading = false;
            state_->refresh_authoritative = false;
            state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            state_->status_text = "Process enumeration operation is not configured.";
            enqueue_status = SAO_STATUS_ERR_NOT_INITIALIZED;
        } else {
            if (state_->requested_generation ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                state_->last_status = SAO_STATUS_ERR_BUFFER_TOO_SMALL;
                state_->status_text = "Process selection generation is exhausted.";
                enqueue_status = SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            } else {
                ++state_->requested_generation;
                state_->work_items.push_back(
                    State::WorkItem{State::WorkKind::refresh, state_->requested_generation, {}});
                state_->loading = true;
                state_->refresh_authoritative = false;
                state_->last_status = SAO_STATUS_OK;
                state_->status_text = "Scanning running processes...";
            }
        }
        state_->dirty = true;
    }
    state_->worker_cv.notify_all();
    const sao_status_t publish_status = publish();
    return publish_status == SAO_STATUS_OK ? enqueue_status : publish_status;
}

sao_status_t Owner::set_filter(FilterMode filter) noexcept {
    if (filter != FilterMode::all && filter != FilterMode::likely_game)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    {
        std::lock_guard lock(state_->mutex);
        state_->filter = filter;
        state_->page_index = 0U;
        state_->busy_since = {};
        state_->status_text = filter == FilterMode::all
                                  ? "Filter changed: showing all queryable processes."
                                  : "Filter changed: showing likely game processes.";
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::set_page(std::size_t page_index) noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    {
        std::lock_guard lock(state_->mutex);
        const std::size_t count = selected_process_count(state_->processes, state_->filter,
                                                         state_->search_query_lower);
        const std::size_t page_count =
            count == 0U ? 1U : (count + kProcessesPerPage - 1U) / kProcessesPerPage;
        if (page_index >= page_count)
            return SAO_STATUS_ERR_NOT_FOUND;
        if (state_->page_index == page_index)
            return SAO_STATUS_OK;
        state_->page_index = page_index;
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::set_search_query(std::string_view query) noexcept {
    if (!valid_utf8(query) || query.find('\0') != std::string_view::npos)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::string bounded(query.substr(0, query.size() > kMaximumSearchQueryBytes
                                            ? kMaximumSearchQueryBytes
                                            : query.size()));
    while (!bounded.empty() &&
           (static_cast<unsigned char>(bounded.back()) & 0xc0U) == 0x80U) {
        bounded.pop_back();
    }
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->search_query == bounded)
            return SAO_STATUS_OK;
        state_->search_query = bounded;
        state_->search_query_lower = lower_ascii(bounded);
        state_->page_index = 0U;
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::set_sort(SortColumn column,
                             std::optional<bool> direction_ascending) noexcept {
    if (column != SortColumn::name && column != SortColumn::pid &&
        column != SortColumn::parent_pid) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    {
        std::lock_guard lock(state_->mutex);
        state_->sort_column = column;
        if (direction_ascending.has_value())
            state_->sort_direction =
                *direction_ascending ? SortDirection::ascending : SortDirection::descending;
        else
            state_->sort_direction = state_->sort_direction == SortDirection::ascending
                                         ? SortDirection::descending
                                         : SortDirection::ascending;
        state_->page_index = 0U;
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::set_view_mode(ViewMode mode) noexcept {
    if (mode != ViewMode::flat && mode != ViewMode::tree)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->view_mode == mode)
            return SAO_STATUS_OK;
        state_->view_mode = mode;
        state_->page_index = 0U;
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::set_auto_refresh(bool enabled) noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->operations.enumerate_snapshot ||
            state_->operations.current_process_id == 0U) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        if (state_->auto_refresh == enabled)
            return SAO_STATUS_OK;
        state_->auto_refresh = enabled;
        state_->busy_since = {};
        state_->next_auto_refresh_at =
            enabled ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
        state_->status_text = enabled
                                  ? "Auto refresh on (every 5s, worker-driven)."
                                  : "Auto refresh off.";
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::toggle_expanded(std::uint32_t parent_pid) noexcept {
    if (parent_pid == 0U)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->collapsed_parents.erase(parent_pid) == 0U) {
            if (state_->collapsed_parents.size() >= kMaximumCollapsedPidSet)
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            state_->collapsed_parents.insert(parent_pid);
        }
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::detach() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;

    sao_status_t enqueue_status = SAO_STATUS_OK;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->loading || state_->worker_active || !state_->work_items.empty() ||
            state_->pending_attach.has_value()) {
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        }
        if (!state_->attached_process.has_value() &&
            !state_->attached_notification_pending.has_value() &&
            !state_->detached_notification_pending) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        if (!state_->operations.detach) {
            state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            state_->status_text = "Detach operation is not configured.";
            state_->dirty = true;
            enqueue_status = SAO_STATUS_ERR_NOT_INITIALIZED;
        } else if (state_->requested_generation ==
                   (std::numeric_limits<std::uint64_t>::max)()) {
            state_->last_status = SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            state_->status_text = "Process selection generation is exhausted.";
            state_->dirty = true;
            enqueue_status = SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        } else {
            ++state_->requested_generation;
            state_->work_items.push_back(State::WorkItem{State::WorkKind::detach,
                                                         state_->requested_generation, {}, {}});
            state_->loading = true;
            state_->last_status = SAO_STATUS_OK;
            state_->status_text = "Detaching the memory target.";
            state_->dirty = true;
        }
    }
    if (enqueue_status == SAO_STATUS_OK)
        state_->worker_cv.notify_all();
    const sao_status_t publish_status = publish();
    return publish_status == SAO_STATUS_OK ? enqueue_status : publish_status;
}

sao_status_t Owner::open_memory_viewer() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    std::function<sao_status_t()> open_hook;
    bool attached = false;
    {
        std::lock_guard lock(state_->mutex);
        attached = state_->attached_process.has_value();
        open_hook = state_->operations.open_memory_viewer;
    }
    sao_status_t open_status = SAO_STATUS_OK;
    std::string message;
    if (!attached) {
        open_status = SAO_STATUS_ERR_NOT_FOUND;
        message = "Memory viewer requires an attached process; attach a PID first.";
    } else if (!open_hook) {
        open_status = SAO_STATUS_ERR_NOT_INITIALIZED;
        message = "The memory viewer owner hook is not configured.";
    } else {
        open_status = open_hook();
        if (open_status == SAO_STATUS_OK)
            message = "Memory viewer opened for the attached process.";
        else
            message = status_description(open_status, "Opening the memory viewer failed");
    }
    {
        std::lock_guard lock(state_->mutex);
        state_->last_status = open_status;
        state_->status_text = std::move(message);
        state_->dirty = true;
    }
    const sao_status_t publish_status = publish();
    return publish_status == SAO_STATUS_OK ? open_status : publish_status;
}

sao_status_t Owner::attach(ProcessIdentity identity) noexcept {
    if (identity.pid == 0U || identity.pid == 4U || identity.start_time_100ns == 0U)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;

    sao_status_t enqueue_status = SAO_STATUS_OK;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->loading || state_->worker_active || !state_->work_items.empty() ||
            state_->pending_attach.has_value())
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        if (!state_->refresh_authoritative) {
            state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            state_->status_text = "Attach rejected: stale snapshot; refresh before attaching.";
            state_->dirty = true;
            enqueue_status = SAO_STATUS_ERR_NOT_INITIALIZED;
        } else {
            const bool selected_identity_exists =
                std::ranges::any_of(state_->processes, [&](const ProcessRecord& process) {
                    return process.identity() == identity;
                });
            if (!selected_identity_exists) {
                state_->last_status = SAO_STATUS_ERR_NOT_FOUND;
                state_->status_text =
                    "Attach rejected: selection is no longer in the current snapshot.";
                state_->dirty = true;
                enqueue_status = SAO_STATUS_ERR_NOT_FOUND;
            } else if (!state_->operations.query_process || !state_->operations.attach ||
                       !state_->operations.detach) {
                state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
                state_->status_text =
                    "Attach, detach, and identity operations are not configured.";
                state_->dirty = true;
                enqueue_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            } else {
                if (state_->requested_generation ==
                    (std::numeric_limits<std::uint64_t>::max)()) {
                    state_->last_status = SAO_STATUS_ERR_BUFFER_TOO_SMALL;
                    state_->status_text = "Process selection generation is exhausted.";
                    state_->dirty = true;
                    enqueue_status = SAO_STATUS_ERR_BUFFER_TOO_SMALL;
                } else {
                    ++state_->requested_generation;
                    State::WorkItem work{State::WorkKind::attach,
                                         state_->requested_generation, identity};
                    if (state_->operations.before_attach)
                        state_->pending_attach = work;
                    else
                        state_->work_items.push_back(work);
                    state_->attached_notification_pending.reset();
                    state_->loading = true;
                    state_->last_status = SAO_STATUS_OK;
                    state_->status_text = state_->operations.before_attach
                                              ? "Draining the previous memory target."
                                              : "Verifying identity and attaching PID " +
                                                    std::to_string(identity.pid) + ".";
                    state_->dirty = true;
                }
            }
        }
    }
    if (enqueue_status == SAO_STATUS_OK)
        state_->worker_cv.notify_all();
    const sao_status_t publish_status = publish();
    return publish_status == SAO_STATUS_OK ? enqueue_status : publish_status;
}

sao_status_t Owner::dispatch_action(std::string_view action_id,
                                    std::string_view payload_json) noexcept {
    if (!valid_text(action_id, kMaximumActionIdBytes, true))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    bool valid = false;
    Json payload = parse_payload(payload_json, valid);
    if (!valid)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;

    if (action_id == kRefreshAction)
        return refresh();
    if (action_id == kFilterAction) {
        const auto mode = payload.find("mode");
        if (mode == payload.end() || !mode->is_string())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const std::string value = mode->get<std::string>();
        if (!valid_text(value, 32U, true))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (value == "all")
            return set_filter(FilterMode::all);
        if (value == "likely_game")
            return set_filter(FilterMode::likely_game);
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (action_id == kPreviousPageAction || action_id == kNextPageAction) {
        if (!payload.empty())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::size_t page_index = 0U;
        {
            std::lock_guard lock(state_->mutex);
            page_index = state_->page_index;
        }
        if (action_id == kPreviousPageAction) {
            if (page_index == 0U)
                return SAO_STATUS_ERR_NOT_FOUND;
            return set_page(page_index - 1U);
        }
        return set_page(page_index + 1U);
    }
    if (action_id == kAttachAction) {
        const std::optional<std::uint64_t> pid = json_unsigned(payload, "pid");
        const std::optional<std::uint64_t> start_time = json_unsigned(payload, "start_time_100ns");
        if (!pid.has_value() || !start_time.has_value() ||
            *pid > (std::numeric_limits<std::uint32_t>::max)()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        return attach({static_cast<std::uint32_t>(*pid), *start_time});
    }
    if (action_id == kDetachAction) {
        if (!payload.empty())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return detach();
    }
    if (action_id == kSearchAction) {
        const auto text = payload.find("text");
        const auto value = payload.find("value");
        const auto phase = payload.find("phase");
        const bool have_text = text != payload.end();
        const bool have_value = value != payload.end();
        const auto* chosen = have_text ? &(*text) : have_value ? &(*value) : nullptr;
        if (phase != payload.end() && !phase->is_string())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (chosen != nullptr && !chosen->is_string())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return set_search_query(chosen != nullptr ? chosen->get<std::string>()
                                                  : std::string_view{});
    }
    if (action_id == kSortAction) {
        const auto selection = payload.find("selected_id");
        const auto value = payload.find("value");
        const auto toggle = payload.find("toggle");
        if (toggle != payload.end()) {
            if (!toggle->is_boolean() || !toggle->get<bool>())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            SortColumn column{};
            {
                std::lock_guard lock(state_->mutex);
                column = state_->sort_column;
            }
            return set_sort(column, std::optional<bool>{});
        }
        SortColumn column = SortColumn::name;
        bool have_column = false;
        if (selection != payload.end() && selection->is_number_integer()) {
            const std::int64_t id = selection->get<std::int64_t>();
            if (id >= 1 && id <= 3) {
                column = id == 1 ? SortColumn::name : id == 2 ? SortColumn::pid
                                                              : SortColumn::parent_pid;
                have_column = true;
            }
        }
        if (!have_column && value != payload.end() && value->is_string()) {
            const std::string name = value->get<std::string>();
            if (name == "name") {
                column = SortColumn::name;
                have_column = true;
            } else if (name == "pid") {
                column = SortColumn::pid;
                have_column = true;
            } else if (name == "ppid") {
                column = SortColumn::parent_pid;
                have_column = true;
            }
        }
        if (!have_column)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        SortColumn current{};
        {
            std::lock_guard lock(state_->mutex);
            current = state_->sort_column;
        }
        if (column == current)
            return set_sort(column, std::optional<bool>{});
        return set_sort(column, true);
    }
    if (action_id == kViewModeAction) {
        const auto mode = payload.find("mode");
        if (mode == payload.end() || !mode->is_string())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const std::string value = mode->get<std::string>();
        if (!valid_text(value, 16U, true))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (value == "flat")
            return set_view_mode(ViewMode::flat);
        if (value == "tree")
            return set_view_mode(ViewMode::tree);
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (action_id == kAutoRefreshAction) {
        const auto enabled = payload.find("enabled");
        if (enabled == payload.end() || !enabled->is_boolean())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return set_auto_refresh(enabled->get<bool>());
    }
    if (action_id == kExpandCollapseAction) {
        const std::optional<std::uint64_t> parent_pid = json_unsigned(payload, "parent_pid");
        if (!parent_pid.has_value() ||
            *parent_pid > (std::numeric_limits<std::uint32_t>::max)()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        return toggle_expanded(static_cast<std::uint32_t>(*parent_pid));
    }
    if (action_id == kOpenMemoryViewerAction) {
        if (!payload.empty())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return open_memory_viewer();
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

sao_status_t Owner::snapshot(Snapshot& out) const noexcept {
    if (!state_)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    try {
        Snapshot copy{};
        {
            std::lock_guard lock(state_->mutex);
            copy.panel_created = state_->panel != nullptr;
            copy.action_handler_attached = state_->action_handler_attached;
            copy.event_handler_attached = state_->event_handler_attached;
            copy.visible = state_->visible;
            copy.loading = state_->loading;
            copy.filter = state_->filter;
            copy.search_query = state_->search_query;
            copy.sort_column = state_->sort_column;
            copy.sort_direction = state_->sort_direction;
            copy.view_mode = state_->view_mode;
            copy.auto_refresh = state_->auto_refresh;
            copy.auto_refresh_interval_ms = kAutoRefreshIntervalMs;
            copy.page_index = state_->page_index;
            copy.attach_available = state_->operations.query_process &&
                                    state_->operations.attach && state_->operations.detach;
            copy.last_status = state_->last_status;
            copy.status_text = state_->status_text;
            copy.all_processes = state_->processes;
            VisibleView visible_view =
                select_visible(copy.all_processes, copy.filter, state_->search_query_lower,
                               copy.sort_column, copy.sort_direction, copy.view_mode,
                               state_->collapsed_parents);
            copy.visible_root_count = visible_view.root_count;
            copy.visible_processes = std::move(visible_view.processes);
            copy.visible_depths = std::move(visible_view.depths);
            copy.visible_has_children = std::move(visible_view.has_children);
            copy.visible_collapsed = std::move(visible_view.collapsed);
            copy.attached_process = state_->attached_process;
            copy.memory_viewer_available =
                static_cast<bool>(state_->operations.open_memory_viewer);
            copy.rendered_spec_json = state_->rendered_spec_json;
        }
        out = std::move(copy);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::launcher::process_selector_panel
