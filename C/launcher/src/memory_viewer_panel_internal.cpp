#include "memory_viewer_panel_internal.h"

#include "sao/core/process.h"
#include "sao/rt_io/proxy.h"
#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_sdk.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sao::launcher::memory_viewer_panel {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumActionPayloadBytes = 16U * 1024U;
constexpr std::size_t kMaximumActionIdBytes = 64U;
constexpr std::size_t kMaximumStatusBytes = 1024U;
constexpr std::size_t kMaximumPanelSpecBytes = 256U * 1024U;
constexpr std::size_t kMaximumFieldBytes = 64U;
constexpr std::size_t kMaximumPathBytes = SAO_PROCESS_IMAGE_PATH_MAX - 1U;

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

std::string format_hex(std::uint64_t value) {
    std::array<char, 2 + 16 + 1> buffer{};
    auto out = std::to_chars(buffer.data() + 2, buffer.data() + 2 + 16, value, 16);
    buffer[0] = '0';
    buffer[1] = 'x';
    std::array<char, 19> digits{};
    const std::size_t count = static_cast<std::size_t>(out.ptr - (buffer.data() + 2));
    const std::size_t pad = count < 16U ? 16U - count : 0U;
    std::fill_n(digits.data(), pad, '0');
    std::memcpy(digits.data() + pad, buffer.data() + 2, count);
    return std::string("0x") + std::string(digits.data(), 16U);
}

std::string format_bytes(std::uint64_t bytes) {
    if (bytes < 1024U)
        return std::to_string(bytes) + " B";
    if (bytes < 1024U * 1024U)
        return std::to_string(bytes / 1024U) + " KiB";
    if (bytes < 1024ULL * 1024ULL * 1024ULL)
        return std::to_string(bytes / (1024U * 1024U)) + " MiB";
    return std::to_string(bytes / (1024ULL * 1024ULL * 1024ULL)) + " GiB";
}

std::string protect_label(std::uint32_t protect) {
    // Mirror Python memory view: coarse page-protection buckets.
    if (protect == 0U)
        return "---";
    const auto readable = (protect & 0xF6U) != 0U; // EXECUTE_READ* / READ*
    const auto writable = (protect & 0xCCU) != 0U; // WRITE* / READWRITE
    const auto guard = (protect & 0x100U) != 0U;
    std::string label;
    label += readable ? "R" : "-";
    label += writable ? "W" : "-";
    label += (protect & 0xF0U) != 0U ? "X" : "-";
    if (guard)
        label += " G";
    return label;
}

bool parse_hex_address(std::string_view text, std::uint64_t& value_out) {
    std::string_view trimmed = text;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
        trimmed.remove_prefix(1U);
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.remove_suffix(1U);
    if (trimmed.size() > 2U && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X'))
        trimmed.remove_prefix(2U);
    if (trimmed.empty() || trimmed.size() > 16U)
        return false;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != trimmed.data() + trimmed.size())
        return false;
    value_out = value;
    return true;
}

bool parse_size(std::string_view text, std::size_t& value_out) {
    std::string_view trimmed = text;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
        trimmed.remove_prefix(1U);
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.remove_suffix(1U);
    if (trimmed.empty() || trimmed.size() > 8U)
        return false;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != trimmed.data() + trimmed.size())
        return false;
    if (value == 0U || value > kMaximumReadBytes)
        return false;
    value_out = static_cast<std::size_t>(value);
    return true;
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

std::string_view health_label(TargetHealth health) noexcept {
    switch (health) {
    case TargetHealth::ok:
        return "attached";
    case TargetHealth::detached:
        return "detached";
    case TargetHealth::stale:
        return "stale";
    case TargetHealth::exited:
        return "exited";
    case TargetHealth::unreadable:
        return "unreadable";
    }
    return "detached";
}

std::string_view health_accent(TargetHealth health) noexcept {
    switch (health) {
    case TargetHealth::ok:
        return "ok";
    case TargetHealth::stale:
        return "gold";
    case TargetHealth::unreadable:
        return "bad";
    default:
        return "muted";
    }
}

std::string build_panel_spec(const Snapshot& snapshot) {
    Json nodes = Json::array();
    Json status_children = Json::array();
    status_children.push_back(row_node(Json::array(
        {badge_node(std::string(health_label(snapshot.target_health)),
                    std::string(health_accent(snapshot.target_health))),
         badge_node(snapshot.busy ? "busy / 繁忙" : "ready / 就绪",
                    snapshot.busy ? "gold" : "ok")})));
    if (!snapshot.status_text.empty())
        status_children.push_back(text_node(snapshot.status_text,
                                            snapshot.last_status == SAO_STATUS_OK ? "muted" : "bad", 28));
    nodes.push_back(card_node("状态", std::move(status_children)));
    nodes.front()["padding"] = 8;
    nodes.front()["height"] = 96;

    Json toolbar = Json::array();
    toolbar.push_back(text_node("内存查看器", "title", 36));
    Json toolbar_actions = Json::array();
    toolbar_actions.push_back(button_node("memory_viewer.refresh", "刷新目标",
                                          kRefreshTargetAction, Json::object(), "primary",
                                          snapshot.busy));
    toolbar_actions.push_back(button_node("memory_viewer.enum_regions", "枚举区域",
                                          kEnumRegionsAction, Json::object(), "default",
                                          snapshot.busy || snapshot.target_health != TargetHealth::ok));
    toolbar_actions.push_back(button_node("memory_viewer.reset", "清除会话",
                                          kResetSessionAction, Json::object(), "ghost",
                                          snapshot.busy));
    toolbar.push_back(row_node(std::move(toolbar_actions)));
    nodes.push_back(section_node("", std::move(toolbar)));

    if (snapshot.target.has_value()) {
        const TargetBinding& target = *snapshot.target;
        Json target_children = Json::array();
        target_children.push_back(row_node(Json::array(
            {badge_node("PID " + std::to_string(target.pid), "cyan"),
             badge_node("gen " + std::to_string(target.generation), "muted")})));
        target_children.push_back(text_node(target.base_name_utf8, "label", 28));
        if (!target.image_path_utf8.empty())
            target_children.push_back(text_node(target.image_path_utf8, "mono", 28));
        nodes.push_back(section_node("附加目标 / Attached Target",
                                     Json::array({card_node(target.base_name_utf8,
                                                           std::move(target_children), "ok")}),
                                     "ok"));
    } else {
        nodes.push_back(section_node(
            "附加目标 / Attached Target",
            Json::array({text_node("没有活动的附加。请先在 Process Selector 中附加进程并重新打开。",
                                   "muted", 44)})));
    }
    Json read_inputs = Json::array();
    Json address_input{{"type", "input"},
                       {"id", "memory_viewer.address"},
                       {"value", snapshot.address_input},
                       {"action", kAddressInputAction},
                       {"payload", Json::object()},
                       {"height", 38},
                       {"width", 220}};
    read_inputs.push_back(std::move(address_input));
    Json size_input{{"type", "input"},
                    {"id", "memory_viewer.size"},
                    {"value", snapshot.size_input},
                    {"action", kSizeInputAction},
                    {"payload", Json::object()},
                    {"height", 38},
                    {"width", 120}};
    read_inputs.push_back(std::move(size_input));
    read_inputs.push_back(button_node(
        "memory_viewer.read", "读取 / Read", kReadAction, Json::object(), "primary",
        snapshot.busy || snapshot.read_busy || snapshot.target_health != TargetHealth::ok));
    nodes.push_back(section_node("读取 / Read", Json::array({row_node(std::move(read_inputs))}),
                                 "cyan"));

    if (snapshot.last_read.has_value()) {
        const ReadResult& read = *snapshot.last_read;
        Json read_children = Json::array();
        read_children.push_back(row_node(Json::array(
            {badge_node(format_hex(read.address), "cyan"),
             badge_node(std::to_string(read.bytes_read) + " / " +
                            std::to_string(read.requested_size) + " bytes",
                        read.bytes_read == read.requested_size ? "ok" : "gold")})));
        if (read.bytes_read != 0U) {
            std::string hex_preview;
            std::string ascii_preview;
            const std::size_t preview_bytes = std::min<std::size_t>(read.bytes_read, 64U);
            for (std::size_t index = 0; index < preview_bytes; ++index) {
                const std::uint8_t byte = read.bytes[index];
                std::array<char, 3> pair{};
                (void)std::to_chars(pair.data(), pair.data() + 2, byte, 16);
                if (index != 0U)
                    hex_preview.push_back(' ');
                if (pair.data()[1] == '\0') {
                    hex_preview.push_back('0');
                    hex_preview.push_back(pair.data()[0]);
                } else {
                    hex_preview.append(pair.data(), 2U);
                }
                ascii_preview.push_back(byte >= 0x20U && byte <= 0x7eU
                                            ? static_cast<char>(byte)
                                            : '.');
            }
            if (read.bytes_read > preview_bytes)
                hex_preview.append(" ...");
            read_children.push_back(text_node(hex_preview, "mono", 28));
            read_children.push_back(text_node(ascii_preview, "mono", 28));
        }
        nodes.push_back(section_node("结果 / Result",
                                     Json::array({card_node("读取", std::move(read_children),
                                                             read.bytes_read == read.requested_size
                                                                 ? "ok" : "gold")}),
                                     "ok"));
    }

    if (!snapshot.regions.empty()) {
        Json region_rows = Json::array();
        const std::size_t page_begin =
            std::min(snapshot.regions_page_index * kRegionsPerPage, snapshot.regions_total);
        const std::size_t page_end = std::min(
            page_begin + kRegionsPerPage,
            std::min(snapshot.regions_total, snapshot.regions.size()));
        for (std::size_t index = page_begin; index < page_end; ++index) {
            const RegionRecord& region = snapshot.regions[index];
            region_rows.push_back(row_node(Json::array(
                {badge_node(format_hex(region.base), "cyan"),
                 badge_node(format_hex(region.base + region.size), "muted"),
                 badge_node(format_bytes(region.size), "muted"),
                 badge_node(protect_label(region.protect),
                            (region.protect & 0xF0U) != 0U ? "ok" : "muted"),
                 badge_node("T" + std::to_string(region.region_type), "muted")})));
        }
        const std::size_t page_count =
            snapshot.regions_total == 0U
                ? 1U
                : (snapshot.regions_total + kRegionsPerPage - 1U) / kRegionsPerPage;
        Json paging = Json::array();
        paging.push_back(button_node("memory_viewer.regions_page.previous", "上一页",
                                     std::string(kRegionsPageAction),
                                     {{"page", snapshot.regions_page_index == 0U
                                                   ? 0U
                                                   : snapshot.regions_page_index - 1U}},
                                     "ghost", snapshot.busy || snapshot.regions_page_index == 0U));
        paging.push_back(badge_node("第 " + std::to_string(snapshot.regions_page_index + 1U) +
                                        " / " + std::to_string(page_count) + " 页",
                                    "muted"));
        paging.push_back(button_node("memory_viewer.regions_page.next", "下一页",
                                     std::string(kRegionsPageAction),
                                     {{"page", snapshot.regions_page_index + 1U}}, "ghost",
                                     snapshot.busy ||
                                         snapshot.regions_page_index + 1U >= page_count));
        region_rows.push_back(row_node(std::move(paging)));
        nodes.push_back(section_node("内存区域 / Regions (" +
                                         std::to_string(snapshot.regions_total) + ")",
                                     std::move(region_rows), "cyan"));
    }

    std::string serialized = dock_document(std::move(nodes), "memory-content").dump();
    if (serialized.size() <= kMaximumPanelSpecBytes)
        return serialized;
    Json compact = Json::array();
    compact.push_back(text_node(
        "The memory view exceeded the panel budget. Use a smaller page or clear the region list.",
        "bad", 48));
    compact.push_back(button_node("memory_viewer.compact_reset", "清除会话",
                                  kResetSessionAction, Json::object(), "primary"));
    return Json{{"version", 1}, {"title", ""}, {"nodes", std::move(compact)}}.dump();
}

TargetHealth validate_target_identity(const TargetBinding& target,
                                      std::uint64_t highest_generation) noexcept {
    if (!target.valid())
        return TargetHealth::detached;
    if (target.generation < highest_generation)
        return TargetHealth::stale;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, target.pid);
    if (process == nullptr)
        return TargetHealth::exited;
    struct HandleGuard {
        HANDLE handle;
        ~HandleGuard() {
            if (handle != nullptr)
                CloseHandle(handle);
        }
    } guard{process};
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user) == FALSE)
        return TargetHealth::unreadable;
    const std::uint64_t current_start =
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
    if (current_start == 0U || current_start != target.start_time_100ns)
        return TargetHealth::stale;
    if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
        return TargetHealth::exited;
    return TargetHealth::ok;
}

} // namespace

struct Owner::State {
    static std::mutex deferred_mutex;
    static std::vector<std::unique_ptr<State>> deferred_cleanup;

    explicit State(sao_ui_compositor_handle_t borrowed_compositor)
        : compositor(borrowed_compositor) {}

    void request_shutdown() noexcept {
        std::lock_guard lock(mutex);
        accepting = false;
        if (generation != (std::numeric_limits<std::uint64_t>::max)())
            ++generation;
    }

    Owner* owner{};
    sao_status_t fail_next_unregister_status{SAO_STATUS_OK};
    sao_status_t fail_next_action_restore_status{SAO_STATUS_OK};
    sao_status_t fail_next_event_restore_status{SAO_STATUS_OK};
    sao_ui_compositor_handle_t compositor{};
    sao_rt_io_proxy_handle_t borrowed_proxy{};
    mutable std::mutex mutex;
    std::size_t operations_in_flight{};
    std::size_t callbacks_in_flight{};
    bool visible{};
    bool accepting{true};
    bool creating{};
    bool retiring{};
    bool action_handler_attached{};
    bool event_handler_attached{};
    bool dirty{true};
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};

    bool busy{};
    bool read_busy{};
    bool scan_busy{};
    std::optional<TargetBinding> target;
    std::uint64_t highest_seen_generation{};
    std::uint64_t generation{};
    TargetHealth target_health{TargetHealth::detached};
    std::size_t regions_page_index{};
    std::vector<RegionRecord> regions;
    std::size_t regions_total{};
    std::string address_input;
    std::string size_input;
    std::optional<ReadResult> last_read;
    sao_status_t last_status{SAO_STATUS_OK};
    std::string status_text{"No target yet. Attach a process from the Process Selector first."};
    std::string rendered_spec_json;
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
        auto owner = std::unique_ptr<Owner>(new (std::nothrow) Owner(std::move(state), AdoptStateTag{}));
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

Owner::Owner(sao_ui_compositor_handle_t compositor)
    : state_(std::make_unique<State>(compositor)) {
    state_->owner = this;
}

Owner::Owner(sao_ui_compositor_handle_t compositor, sao_rt_io_proxy_handle_t borrowed_proxy)
    : Owner(compositor) {
    state_->borrowed_proxy = borrowed_proxy;
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

void Owner::end_callback() noexcept {
    if (!state_)
        return;
    std::lock_guard lock(state_->mutex);
    if (state_->callbacks_in_flight != 0U)
        --state_->callbacks_in_flight;
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
        // Close cancels any outstanding user-visible read intent; the panel
        // merely hides and the borrowed proxy is left attached for the owner.
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
    descriptor.default_width_px = 720;
    descriptor.default_height_px = 640;
    descriptor.min_width_px = 560;
    descriptor.min_height_px = 420;
    descriptor.max_width_px = 1440;
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

sao_status_t Owner::check_identity_locked(TargetHealth& health_out) noexcept {
    health_out = TargetHealth::detached;
    if (!state_->target.has_value()) {
        state_->target_health = TargetHealth::detached;
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    const TargetBinding target = *state_->target;
    const std::uint64_t seen_generation = state_->highest_seen_generation;
    // The handle is checked under state lock; no proxy I/O happens here.
    health_out = validate_target_identity(target, seen_generation);
    state_->target_health = health_out;
    switch (health_out) {
    case TargetHealth::ok:
        return SAO_STATUS_OK;
    case TargetHealth::stale:
        return SAO_STATUS_ERR_PROCESS_GONE;
    case TargetHealth::exited:
        return SAO_STATUS_ERR_PROCESS_GONE;
    case TargetHealth::unreadable:
        return SAO_STATUS_ERR_ACCESS_DENIED;
    default:
        return SAO_STATUS_ERR_NOT_FOUND;
    }
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
            view.busy = state_->busy;
            view.read_busy = state_->read_busy;
            view.scan_busy = state_->scan_busy;
            view.target = state_->target;
            view.target_health = state_->target_health;
            view.highest_seen_generation = state_->highest_seen_generation;
            view.regions_page_index = state_->regions_page_index;
            view.regions = state_->regions;
            view.regions_total = state_->regions_total;
            view.address_input = state_->address_input;
            view.size_input = state_->size_input;
            view.last_read = state_->last_read;
            view.last_status = state_->last_status;
            view.status_text = state_->status_text;
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

    TargetHealth health = TargetHealth::detached;
    {
        std::lock_guard lock(state_->mutex);
        (void)check_identity_locked(health);
        state_->dirty = true;
    }
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
    return publish();
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

sao_status_t Owner::service_ui() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    std::lock_guard lock(state_->mutex);
    if (state_->panel == nullptr || state_->body == nullptr)
        return SAO_STATUS_OK;
    return SAO_STATUS_OK;
}

sao_status_t Owner::take_offline() noexcept {
    if (!state_)
        return SAO_STATUS_OK;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->panel == nullptr && state_->body == nullptr && !state_->creating &&
            !state_->retiring && state_->operations_in_flight == 0U &&
            state_->callbacks_in_flight == 0U && !state_->busy) {
            if (state_->generation != (std::numeric_limits<std::uint64_t>::max)())
                ++state_->generation;
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
            state_->callbacks_in_flight != 0U || state_->busy) {
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        }
        if ((state_->panel == nullptr) != (state_->body == nullptr))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        state_->retiring = true;
        was_accepting = state_->accepting;
        state_->accepting = false;
        panel = state_->panel;
        had_action_handler = state_->action_handler_attached;
        had_event_handler = state_->event_handler_attached;
        if (state_->generation != (std::numeric_limits<std::uint64_t>::max)())
            ++state_->generation;
        state_->target.reset();
        state_->target_health = TargetHealth::detached;
        state_->last_read.reset();
        state_->dirty = true;
    }

    if (panel == nullptr) {
        std::lock_guard lock(state_->mutex);
        state_->retiring = false;
        state_->accepting = true;
        state_->visible = false;
        return SAO_STATUS_OK;
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

sao_status_t Owner::bind_target(TargetBinding binding) noexcept {
    if (!binding.valid())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!valid_text(binding.image_path_utf8, kMaximumPathBytes, false) ||
        !valid_text(binding.base_name_utf8, 1024U, true)) {
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
        // A smaller generation is always stale; an equal generation for a
        // different identity is also stale (same-epoch rebinds are only
        // valid for the identical target identity).  See the contract on the
        // header declaration of bind_target().
        if (binding.generation < state_->highest_seen_generation)
            return SAO_STATUS_ERR_PROCESS_GONE;
        if (binding.generation == state_->highest_seen_generation &&
            state_->target.has_value() &&
            (state_->target->pid != binding.pid ||
             state_->target->start_time_100ns != binding.start_time_100ns)) {
            return SAO_STATUS_ERR_PROCESS_GONE;
        }
        if (binding.generation > state_->highest_seen_generation)
            state_->highest_seen_generation = binding.generation;
        state_->target = std::move(binding);
        state_->borrowed_proxy = state_->target->proxy;
        state_->target_health = TargetHealth::ok;
        state_->regions.clear();
        state_->regions_total = 0U;
        state_->regions_page_index = 0U;
        state_->last_read.reset();
        state_->last_status = SAO_STATUS_OK;
        state_->status_text = "Target bound from process selector.";
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::clear_target(std::uint64_t generation) noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    {
        std::lock_guard lock(state_->mutex);
        if (generation < state_->highest_seen_generation)
            return SAO_STATUS_ERR_PROCESS_GONE;
        if (generation > state_->highest_seen_generation)
            state_->highest_seen_generation = generation;
        state_->target.reset();
        state_->target_health = TargetHealth::detached;
        state_->regions.clear();
        state_->regions_total = 0U;
        state_->regions_page_index = 0U;
        state_->last_read.reset();
        state_->status_text = "Attachment cleared by the launcher.";
        state_->last_status = SAO_STATUS_OK;
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::refresh_target() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    TargetHealth health = TargetHealth::detached;
    {
        std::lock_guard lock(state_->mutex);
        const sao_status_t identity_status = check_identity_locked(health);
        if (identity_status != SAO_STATUS_OK) {
            state_->last_status = identity_status;
            state_->status_text = status_description(identity_status, "Target health check failed");
            state_->dirty = true;
        } else {
            state_->last_status = SAO_STATUS_OK;
            state_->status_text = "Target still live.";
            state_->dirty = true;
        }
    }
    return publish();
}

sao_status_t Owner::enum_regions() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;

    sao_rt_io_proxy_handle_t proxy = nullptr;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->busy || state_->scan_busy)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        TargetHealth health = TargetHealth::detached;
        const sao_status_t identity_status = check_identity_locked(health);
        if (identity_status != SAO_STATUS_OK) {
            state_->last_status = identity_status;
            state_->status_text =
                status_description(identity_status,
                                   health == TargetHealth::stale
                                       ? "Region enumeration rejected: binding is stale"
                                   : health == TargetHealth::exited
                                       ? "Region enumeration rejected: target exited"
                                   : health == TargetHealth::unreadable
                                       ? "Region enumeration rejected: target unreadable"
                                       : "Region enumeration rejected: no attached target");
            state_->dirty = true;
            return publish();
        }
        if (state_->borrowed_proxy == nullptr) {
            state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            state_->status_text = "No borrowed proxy is available for reads.";
            state_->dirty = true;
            return publish();
        }
        state_->scan_busy = true;
        state_->busy = true;
        proxy = state_->borrowed_proxy;
    }

    std::vector<SaoRtIoRegionInfo> raw(kRegionsPerPage);
    std::size_t raw_count = 0U;
    const sao_status_t enum_status = sao_rt_io_proxy_enum_regions(proxy, raw.data(), raw.size(), &raw_count);
    std::vector<RegionRecord> collected;
    if (enum_status == SAO_STATUS_OK || enum_status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
        raw.resize(raw_count);
        collected.reserve(raw_count);
        for (const SaoRtIoRegionInfo& entry : raw) {
            if (entry.base == 0U || entry.size == 0U)
                continue;
            collected.push_back({entry.base, entry.size, entry.protect, entry.region_type});
        }
        raw.clear();
    }

    {
        std::lock_guard lock(state_->mutex);
        state_->scan_busy = false;
        state_->busy = false;
        if (enum_status == SAO_STATUS_OK) {
            state_->regions = std::move(collected);
            state_->regions_total = state_->regions.size();
            state_->regions_page_index = 0U;
            state_->last_status = SAO_STATUS_OK;
            state_->status_text = "Enumerated " + std::to_string(state_->regions_total) +
                                  " readable regions.";
        } else if (enum_status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
            state_->regions = std::move(collected);
            state_->regions_total = state_->regions.size();
            state_->regions_page_index = 0U;
            state_->last_status = SAO_STATUS_OK;
            state_->status_text = "Enumerated " + std::to_string(state_->regions_total) +
                                  " regions (page limit reached).";
        } else {
            state_->last_status = enum_status;
            state_->status_text = status_description(enum_status, "Region enumeration failed");
        }
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::read(std::uint64_t address, std::size_t size) noexcept {
    if (size == 0U || size > kMaximumReadBytes)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;

    sao_rt_io_proxy_handle_t proxy = nullptr;
    std::uint64_t generation = 0U;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->busy || state_->read_busy)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        TargetHealth health = TargetHealth::detached;
        const sao_status_t identity_status = check_identity_locked(health);
        if (identity_status != SAO_STATUS_OK) {
            state_->last_status = identity_status;
            state_->status_text =
                status_description(identity_status,
                                   health == TargetHealth::stale
                                       ? "Read rejected: binding is stale"
                                   : health == TargetHealth::exited
                                       ? "Read rejected: target exited"
                                   : health == TargetHealth::unreadable
                                       ? "Read rejected: target unreadable"
                                       : "Read rejected: no attached target");
            state_->dirty = true;
            return publish();
        }
        if (state_->borrowed_proxy == nullptr) {
            state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            state_->status_text = "No borrowed proxy is available for reads.";
            state_->dirty = true;
            return publish();
        }
        state_->read_busy = true;
        state_->busy = true;
        proxy = state_->borrowed_proxy;
        generation = state_->target->generation;
    }

    std::vector<std::uint8_t> buffer(size);
    std::size_t bytes_read = 0U;
    const sao_status_t read_status =
        sao_rt_io_proxy_read(proxy, address, buffer.data(), buffer.size(), &bytes_read);

    {
        std::lock_guard lock(state_->mutex);
        state_->read_busy = false;
        state_->busy = false;
        // A rebind while the read was in flight invalidates the result even
        // when the helper returned OK.
        if (!state_->target.has_value() || state_->target->generation != generation) {
            state_->last_status = SAO_STATUS_ERR_PROCESS_GONE;
            state_->status_text = "Read discarded: attachment generation changed in flight.";
            state_->last_read.reset();
            state_->dirty = true;
            return publish();
        }
        if (read_status == SAO_STATUS_OK && bytes_read != 0U) {
            ReadResult result{};
            result.address = address;
            result.requested_size = size;
            result.bytes_read = bytes_read;
            buffer.resize(bytes_read);
            result.bytes = std::move(buffer);
            state_->last_read = std::move(result);
            state_->last_status = SAO_STATUS_OK;
            state_->status_text = "Read " + format_hex(address) + " (" +
                                  std::to_string(bytes_read) + " bytes).";
        } else if (read_status == SAO_STATUS_OK) {
            state_->last_status = SAO_RT_IO_ERR_ADDRESS_UNREADABLE;
            state_->status_text = "Read rejected: no bytes returned for " + format_hex(address) + ".";
            state_->last_read.reset();
        } else {
            state_->last_status = read_status;
            state_->status_text = status_description(read_status, "Read failed");
            state_->last_read.reset();
        }
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::set_address_input(std::string_view text) noexcept {
    if (!valid_utf8(text) || text.size() > kMaximumFieldBytes)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->address_input == text)
            return SAO_STATUS_OK;
        state_->address_input = std::string(text);
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::set_size_input(std::string_view text) noexcept {
    if (!valid_utf8(text) || text.size() > kMaximumFieldBytes)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->size_input == text)
            return SAO_STATUS_OK;
        state_->size_input = std::string(text);
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::set_regions_page(std::size_t page_index) noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    {
        std::lock_guard lock(state_->mutex);
        const std::size_t page_count =
            state_->regions_total == 0U ? 1U : (state_->regions_total + kRegionsPerPage - 1U) / kRegionsPerPage;
        if (page_index >= page_count)
            return SAO_STATUS_ERR_NOT_FOUND;
        if (state_->regions_page_index == page_index)
            return SAO_STATUS_OK;
        state_->regions_page_index = page_index;
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::reset_session() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    {
        std::lock_guard lock(state_->mutex);
        state_->address_input.clear();
        state_->size_input.clear();
        state_->last_read.reset();
        state_->regions.clear();
        state_->regions_total = 0U;
        state_->regions_page_index = 0U;
        state_->last_status = SAO_STATUS_OK;
        state_->status_text = "Viewer session cleared; the attachment is unchanged.";
        state_->dirty = true;
    }
    return publish();
}

sao_status_t Owner::dispatch_action(std::string_view action_id, std::string_view payload_json) noexcept {
    if (!valid_text(action_id, kMaximumActionIdBytes, true))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    bool valid = false;
    Json payload = parse_payload(payload_json, valid);
    if (!valid)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;

    if (action_id == kRefreshTargetAction)
        return refresh_target();
    if (action_id == kEnumRegionsAction)
        return enum_regions();
    if (action_id == kResetSessionAction)
        return reset_session();
    if (action_id == kReadAction) {
        std::uint64_t address = 0U;
        std::size_t size = 0U;
        bool parse_failed = false;
        {
            std::lock_guard lock(state_->mutex);
            if (!parse_hex_address(state_->address_input, address) ||
                !parse_size(state_->size_input, size)) {
                state_->last_status = SAO_STATUS_ERR_INVALID_ARGUMENT;
                state_->status_text = "Enter a hex address (0x...) and a decimal size (1-65536).";
                state_->dirty = true;
                parse_failed = true;
            }
        }
        if (parse_failed)
            return publish();
        return read(address, size);
    }
    if (action_id == kAddressInputAction) {
        const auto text = payload.find("text");
        const auto value = payload.find("value");
        const auto* chosen = text != payload.end() ? &(*text) : value != payload.end() ? &(*value) : nullptr;
        if (chosen != nullptr && !chosen->is_string())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return set_address_input(chosen != nullptr ? chosen->get<std::string>() : std::string_view{});
    }
    if (action_id == kSizeInputAction) {
        const auto text = payload.find("text");
        const auto value = payload.find("value");
        const auto* chosen = text != payload.end() ? &(*text) : value != payload.end() ? &(*value) : nullptr;
        if (chosen != nullptr && !chosen->is_string())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return set_size_input(chosen != nullptr ? chosen->get<std::string>() : std::string_view{});
    }
    if (action_id == kRegionsPageAction) {
        const std::optional<std::uint64_t> page = json_unsigned(payload, "page");
        if (!page.has_value())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return set_regions_page(static_cast<std::size_t>(*page));
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
            copy.busy = state_->busy;
            copy.read_busy = state_->read_busy;
            copy.scan_busy = state_->scan_busy;
            copy.target = state_->target;
            copy.target_health = state_->target_health;
            copy.highest_seen_generation = state_->highest_seen_generation;
            copy.regions_page_index = state_->regions_page_index;
            copy.regions = state_->regions;
            copy.regions_total = state_->regions_total;
            copy.address_input = state_->address_input;
            copy.size_input = state_->size_input;
            copy.last_read = state_->last_read;
            copy.last_status = state_->last_status;
            copy.status_text = state_->status_text;
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

} // namespace sao::launcher::memory_viewer_panel
