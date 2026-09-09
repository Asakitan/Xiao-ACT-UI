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
#include <cstddef>
#include <cstdint>
#include <condition_variable>
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
#include <thread>
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
constexpr int kEnumerationAttempts = 3;

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

std::optional<std::string_view> bounded_c_text(const char* value,
                                               std::size_t maximum) noexcept {
    if (value == nullptr)
        return std::nullopt;
    const void* terminator = std::memchr(value, '\0', maximum + 1U);
    if (terminator == nullptr)
        return std::nullopt;
    const auto length =
        static_cast<std::size_t>(static_cast<const char*>(terminator) - value);
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
    case SnapshotState::loading: return "loading";
    case SnapshotState::fresh: return "fresh";
    case SnapshotState::stale: return "stale";
    case SnapshotState::failed: return "failed";
    case SnapshotState::empty: return "empty";
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

std::vector<ProcessRecord> select_visible(const std::vector<ProcessRecord>& processes,
                                          FilterMode filter) {
    if (filter == FilterMode::all)
        return processes;
    std::vector<ProcessRecord> visible;
    visible.reserve(processes.size());
    std::ranges::copy_if(processes, std::back_inserter(visible), is_likely_game_process);
    return visible;
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
              {"height", 30}};
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
    return Json{{"type", "card"}, {"title", bounded_text(std::move(title), 512U)},
                {"accent", accent}, {"children", std::move(children)}};
}

Json section_node(std::string title, Json children, std::string_view accent = "cyan") {
    return Json{{"type", "section"}, {"title", bounded_text(std::move(title), 512U)},
                {"accent", accent}, {"children", std::move(children)}};
}

Json dock_document(Json nodes, std::string content_id, int min_width = 560) {
    if (!nodes.is_array() || nodes.empty())
        return Json{{"version", 1}, {"title", ""}, {"layout", "dock"}, {"nodes", std::move(nodes)}};
    Json top = std::move(nodes.front());
    nodes.erase(nodes.begin());
    top["dock"] = "top";
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
                {"nodes", Json::array({std::move(top), std::move(content)})}};
}

std::string_view snapshot_accent(SnapshotState state) noexcept {
    if (state == SnapshotState::fresh) return "ok";
    if (state == SnapshotState::loading || state == SnapshotState::stale) return "gold";
    if (state == SnapshotState::failed) return "danger";
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
std::string build_panel_spec(const Snapshot& snapshot) {
    const SnapshotState state = snapshot_state(snapshot);
    const bool attach_allowed = state == SnapshotState::fresh;
    Json nodes = Json::array();
    nodes.push_back(status_strip_node(state));
    Json filters = Json::array();
    Json actions = Json::array();
    actions.push_back(button_node("process.refresh", snapshot.loading ? "正在刷新…" : "刷新",
                                  kRefreshAction, Json::object(), "primary", snapshot.loading));
    actions.push_back(button_node("process.filter.all", "全部", kFilterAction, {{"mode", "all"}},
                                  snapshot.filter == FilterMode::all ? "primary" : "ghost"));
    actions.push_back(
        button_node("process.filter.game", "可能的游戏", kFilterAction, {{"mode", "likely_game"}},
                    snapshot.filter == FilterMode::likely_game ? "primary" : "ghost"));
    filters.push_back(row_node(std::move(actions)));
    Json badges = Json::array();
    badges.push_back(Json{{"type", "badge"}, {"text", std::string(snapshot_state_label(state))},
                          {"style", state == SnapshotState::fresh ? "ok" : state == SnapshotState::stale ? "warn" : state == SnapshotState::failed ? "bad" : "accent"}, {"height", 22}});
    badges.push_back(Json{{"type", "badge"},
                          {"text", std::to_string(snapshot.visible_processes.size()) + " shown"},
                          {"style", "accent"}, {"height", 22}});
    badges.push_back(Json{{"type", "badge"},
                          {"text", std::to_string(snapshot.all_processes.size()) + " available"},
                          {"style", "muted"}, {"height", 22}});
    filters.push_back(row_node(std::move(badges)));
    nodes.push_back(section_node("Filters / 筛选", std::move(filters)));
    if (snapshot.attached_process.has_value()) {
        const ProcessRecord& process = *snapshot.attached_process;
        Json details = Json::array();
        details.push_back(row_node(Json::array({badge_node("Attached / 已附加", "ok"),
                                                badge_node("PID " + std::to_string(process.pid), "cyan")})));
        details.push_back(text_node(process.image_path_utf8, "mono", 34));
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
                                      : snapshot.filter == FilterMode::likely_game
                                            ? "No likely game processes matched. Switch to Show all or refresh. / 未匹配到可能的游戏进程，请切换全部或刷新。"
                                            : "No queryable processes are available. Refresh to try again. / 没有可查询的进程，请刷新重试。",
                                  "muted", 44));
        empty.push_back(button_node("process.empty-refresh", "刷新", kRefreshAction, Json::object(),
                                    "primary", snapshot.loading));
        nodes.push_back(section_node("Process List / 进程列表", std::move(empty)));
    } else {
        Json cards = Json::array();
        for (const ProcessRecord& process : snapshot.visible_processes) {
            Json details = Json::array();
            details.push_back(row_node(Json::array({
                badge_node("PID " + std::to_string(process.pid),
                           is_likely_game_process(process) ? "ok" : "muted"),
                badge_node(is_likely_game_process(process) ? "Likely game / 可能的游戏" : "Process / 进程", "muted")})));
            details.push_back(text_node(process.image_path_utf8, "mono", 34));
            details.push_back(button_node(
                "process.attach." + std::to_string(process.pid), "选择并附加", kAttachAction,
                {{"pid", process.pid}, {"start_time_100ns", process.start_time_100ns}}, "primary",
                !attach_allowed));
            cards.push_back(card_node(process.base_name_utf8, std::move(details),
                                      is_likely_game_process(process) ? "cyan" : "muted"));
        }
        nodes.push_back(section_node("Process List / 进程列表", std::move(cards), "cyan"));
    }
    std::string serialized = dock_document(std::move(nodes), "process-content").dump();
    if (serialized.size() <= kMaximumPanelSpecBytes)
        return serialized;
    Json compact = Json::array();
    compact.push_back(text_node("The process list exceeded the panel budget. Use Likely games to narrow it.",
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
        const auto length = static_cast<std::size_t>(
            static_cast<const char*>(terminator) - image_path.data());
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
    };

    struct WorkItem {
        WorkKind kind{WorkKind::refresh};
        std::uint64_t generation{};
        ProcessIdentity identity{};
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
            std::uint32_t current_process_id = 0U;
            try {
                std::unique_lock lock(mutex);
                if (!worker_cv.wait(lock, stop, [this] { return !work_items.empty(); }))
                    break;
                item = std::move(work_items.front());
                work_items.pop_front();
                worker_active = true;
                enumerate = operations.enumerate_snapshot;
                query_process = operations.query_process;
                attach_operation = operations.attach;
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
                } else if (!query_process || !attach_operation) {
                    completion.status = SAO_STATUS_ERR_NOT_INITIALIZED;
                    completion.status_text = "Attach operations are not configured.";
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
                        if (process_missing || normalized.front().start_time_100ns !=
                                                   item.identity.start_time_100ns) {
                            completion.status = SAO_STATUS_ERR_PROCESS_GONE;
                            completion.status_text =
                                process_missing
                                    ? "Attach rejected: process exited before attach."
                                    : "Attach rejected: PID identity changed before attach.";
                        } else {
                            ProcessRecord validated = std::move(normalized.front());
                            completion.status = attach_operation(item.identity.pid);
                            if (completion.status == SAO_STATUS_OK) {
                                completion.status_text =
                                    "Attached PID " + std::to_string(validated.pid) + " · " +
                                    validated.base_name_utf8;
                                completion.attached_process = std::move(validated);
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
    bool dirty{true};
    bool refresh_authoritative{};
    std::uint64_t requested_generation{};
    FilterMode filter{FilterMode::all};
    sao_status_t last_status{SAO_STATUS_OK};
    std::string status_text{"Not refreshed yet."};
    std::vector<ProcessRecord> processes;
    std::optional<ProcessRecord> attached_process;
    std::string rendered_spec_json;
    std::jthread worker;
};

std::mutex Owner::deferred_mutex_;
std::vector<std::unique_ptr<Owner::State>> Owner::deferred_cleanup_;

void Owner::defer_state(std::unique_ptr<State> state) noexcept { if (state == nullptr) return; state->owner = nullptr; { std::lock_guard lock(state->mutex); state->accepting = false; state->retiring = false; } std::lock_guard lock(deferred_mutex_); deferred_cleanup_.push_back(std::move(state)); }

void Owner::drain_deferred_cleanup() noexcept { std::vector<std::unique_ptr<State>> pending; { std::lock_guard lock(deferred_mutex_); pending.swap(deferred_cleanup_); } std::vector<std::unique_ptr<State>> retry; for (auto& state : pending) { auto owner = std::unique_ptr<Owner>(new (std::nothrow) Owner(std::move(state), AdoptStateTag{})); if (owner == nullptr) { retry.push_back(std::move(state)); continue; } const sao_status_t status = owner->take_offline(); state = std::move(owner->state_); if (status != SAO_STATUS_OK) retry.push_back(std::move(state)); } if (!retry.empty()) { std::lock_guard lock(deferred_mutex_); for (auto& state : retry) deferred_cleanup_.push_back(std::move(state)); } }

void Owner::drain_deferred_cleanup_for_owner() noexcept { drain_deferred_cleanup(); }

void Owner::drain_deferred_cleanup_for_testing() noexcept { drain_deferred_cleanup_for_owner(); }

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
    operations.attach = [proxy](std::uint32_t pid) -> sao_status_t {
        if (proxy == nullptr)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        return sao_rt_io_proxy_attach(proxy, pid);
    };
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
    : state_(std::make_unique<State>(compositor, std::move(operations))) { state_->owner = this; }

Owner::Owner(std::unique_ptr<State> state, AdoptStateTag) noexcept : state_(std::move(state)) { if (state_) state_->owner = this; }

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

void Owner::fail_next_unregister_for_testing(sao_status_t status) noexcept { if (state_) { std::lock_guard lock(state_->mutex); state_->fail_next_unregister_status = status; } }

void Owner::fail_next_handler_restore_for_testing(sao_status_t action_status, sao_status_t event_status) noexcept { if (state_) { std::lock_guard lock(state_->mutex); state_->fail_next_action_restore_status = action_status; state_->fail_next_event_restore_status = event_status; } }

void Owner::end_callback() noexcept {
    if (!state_)
        return;
    std::lock_guard lock(state_->mutex);
    if (state_->callbacks_in_flight != 0U)
        --state_->callbacks_in_flight;
}

void SAO_UI_CALL Owner::panel_action_callback(const char* action_id_utf8,
                                              const std::uint8_t* payload_json_utf8,
                                              std::size_t payload_len,
                                              void* user_data) noexcept {
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

void SAO_UI_CALL Owner::panel_event_callback(std::int32_t event_kind,
                                             void* user_data) noexcept {
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
            view.last_status = state_->last_status;
            view.status_text = state_->status_text;
            view.all_processes = state_->processes;
            view.visible_processes = select_visible(view.all_processes, view.filter);
            view.attached_process = state_->attached_process;
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
        {
            std::lock_guard lock(state_->mutex);
            if (state_->panel == nullptr || state_->body == nullptr)
                return SAO_STATUS_OK;
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
                        state_->status_text =
                            "Refresh complete: " + std::to_string(state_->processes.size()) +
                            " queryable processes.";
                    } else {
                        state_->status_text =
                            status_description(completion.status, "Refresh failed");
                    }
                } else {
                    state_->status_text = completion.status_text.empty()
                                              ? status_description(completion.status,
                                                                   "Attach failed")
                                              : std::move(completion.status_text);
                    if (completion.status == SAO_STATUS_OK)
                        state_->attached_process = std::move(completion.attached_process);
                }
                state_->dirty = true;
                publish_needed = true;
            }
            publish_needed = publish_needed || state_->dirty;
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
    {
        std::lock_guard lock(state_->mutex);
        if (state_->panel == nullptr && state_->body == nullptr && !state_->creating &&
            !state_->retiring && state_->operations_in_flight == 0U &&
            state_->callbacks_in_flight == 0U) {
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
    {
        std::lock_guard lock(state_->mutex);
        if (state_->creating || state_->retiring || state_->operations_in_flight != 0U ||
            state_->callbacks_in_flight != 0U || state_->worker_active ||
            !state_->work_items.empty()) {
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        }
        if (state_->panel == nullptr) {
            if (state_->body != nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            state_->visible = false;
            return SAO_STATUS_OK;
        }
        if (state_->body == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        state_->retiring = true;
        was_accepting = state_->accepting;
        state_->accepting = false;
        ++state_->requested_generation;
        state_->work_items.clear();
        state_->loading = false;
        state_->refresh_authoritative = false;
        state_->completions.clear();
        panel = state_->panel;
        had_action_handler = state_->action_handler_attached;
        had_event_handler = state_->event_handler_attached;
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
        { std::lock_guard lock(state_->mutex); injected = std::exchange(state_->fail_next_unregister_status, SAO_STATUS_OK); }
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
            injected == SAO_STATUS_OK
                ? sao_ui_panel_set_action_handler(panel, &Owner::panel_action_callback, state_.get())
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
        if (state_->loading || state_->worker_active || !state_->work_items.empty())
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        if (!state_->operations.enumerate_snapshot) {
            state_->loading = false;
            state_->refresh_authoritative = false;
            state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            state_->status_text = "Process enumeration operation is not configured.";
            enqueue_status = SAO_STATUS_ERR_NOT_INITIALIZED;
        } else {
            ++state_->requested_generation;
            state_->work_items.push_back(
                State::WorkItem{State::WorkKind::refresh, state_->requested_generation, {}});
            state_->loading = true;
            state_->refresh_authoritative = false;
            state_->last_status = SAO_STATUS_OK;
            state_->status_text = "Scanning running processes...";
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
        state_->status_text = filter == FilterMode::all
                                  ? "Filter changed: showing all queryable processes."
                                  : "Filter changed: showing likely game processes.";
        state_->dirty = true;
    }
    return publish();
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
        if (state_->loading || state_->worker_active || !state_->work_items.empty())
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        if (!state_->refresh_authoritative) {
            state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            state_->status_text =
                "Attach rejected: stale snapshot; refresh before attaching.";
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
            } else if (!state_->operations.query_process || !state_->operations.attach) {
                state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
                state_->status_text = "Attach operations are not configured.";
                state_->dirty = true;
                enqueue_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            } else {
                ++state_->requested_generation;
                state_->work_items.push_back(State::WorkItem{
                    State::WorkKind::attach, state_->requested_generation, identity});
                state_->loading = true;
                state_->last_status = SAO_STATUS_OK;
                state_->status_text =
                    "Verifying identity and attaching PID " + std::to_string(identity.pid) + ".";
                state_->dirty = true;
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
    if (action_id == kAttachAction) {
        const std::optional<std::uint64_t> pid = json_unsigned(payload, "pid");
        const std::optional<std::uint64_t> start_time =
            json_unsigned(payload, "start_time_100ns");
        if (!pid.has_value() || !start_time.has_value() ||
            *pid > (std::numeric_limits<std::uint32_t>::max)()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        return attach({static_cast<std::uint32_t>(*pid), *start_time});
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
            copy.last_status = state_->last_status;
            copy.status_text = state_->status_text;
            copy.all_processes = state_->processes;
            copy.visible_processes = select_visible(copy.all_processes, copy.filter);
            copy.attached_process = state_->attached_process;
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
