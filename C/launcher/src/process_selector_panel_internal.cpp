#include "process_selector_panel_internal.h"

#include "sao/core/process.h"
#include "sao/rt_io/proxy.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_sdk.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sao::launcher::process_selector_panel {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumActionPayloadBytes = 16U * 1024U;
constexpr std::size_t kMaximumStatusBytes = 1024U;
constexpr std::size_t kMaximumPanelSpecBytes = 256U * 1024U;
constexpr int kEnumerationAttempts = 3;

// This is a solid token-only theme. The panel does not create a fisheye layer,
// install a custom render callback, or depend on the fisheye subsystem.
constexpr char kSolidDarkCyanTheme[] =
    R"({"colors":{"APP_BG":"#090f16","APP_CARD":"#111b25","APP_BORDER":"#21475a","APP_TEXT":"#e7f7fb","APP_TEXT_2":"#b8d7df","APP_TEXT_DIM":"#6f929d","APP_ACCENT":"#25d7f2","APP_BLUE":"#25d7f2","APP_GREEN":"#49d6a0","APP_RED":"#ff6f7f","APP_ORANGE":"#f5a85b","APP_GOLD":"#e6c76a","OVERLAY_BG":"#090f16"}})";

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
            record.start_time_100ns == 0U || record.image_path_utf8.empty()) {
            continue;
        }
        if (record.base_name_utf8.empty())
            record.base_name_utf8 = base_name_from_path(record.image_path_utf8);
        if (record.base_name_utf8.empty())
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
                 std::string_view style = "default") {
    Json node{{"type", "button"},
              {"id", std::move(id)},
              {"label", std::move(label)},
              {"action", std::move(action)},
              {"style", style},
              {"height", 30}};
    if (!payload.empty())
        node["payload"] = std::move(payload);
    return node;
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

std::string build_panel_spec(const Snapshot& snapshot) {
    Json nodes = Json::array();

    Json controls = Json::array();
    controls.push_back(
        text_node("Enumerates only when this panel opens or Refresh is pressed.", "muted", 28));
    Json actions = Json::array();
    actions.push_back(
        button_node("process.refresh", "Refresh", kRefreshAction, Json::object(), "primary"));
    actions.push_back(button_node("process.filter.all", "Show all", kFilterAction,
                                  {{"mode", "all"}},
                                  snapshot.filter == FilterMode::all ? "primary" : "ghost"));
    actions.push_back(
        button_node("process.filter.game", "Likely games", kFilterAction, {{"mode", "likely_game"}},
                    snapshot.filter == FilterMode::likely_game ? "primary" : "ghost"));
    controls.push_back(row_node(std::move(actions)));
    controls.push_back(text_node("Showing " + std::to_string(snapshot.visible_processes.size()) +
                                     " of " + std::to_string(snapshot.all_processes.size()) +
                                     " queryable processes.",
                                 "accent", 26));
    nodes.push_back(card_node("Discovery", std::move(controls), "cyan"));

    if (snapshot.attached_process.has_value()) {
        const ProcessRecord& process = *snapshot.attached_process;
        Json attached = Json::array();
        attached.push_back(text_node("Attached PID " + std::to_string(process.pid) + " · " +
                                         process.base_name_utf8,
                                     "accent", 30));
        attached.push_back(text_node(process.image_path_utf8, "mono", 28));
        nodes.push_back(card_node("Attached", std::move(attached), "ok"));
    }

    if (!snapshot.status_text.empty()) {
        const std::string_view style = snapshot.last_status == SAO_STATUS_OK ? "muted" : "bad";
        Json status = Json::array();
        status.push_back(text_node(snapshot.status_text, style, 34));
        nodes.push_back(card_node("Status", std::move(status),
                                  snapshot.last_status == SAO_STATUS_OK ? "cyan" : "bad"));
    }

    if (snapshot.visible_processes.empty()) {
        Json empty = Json::array();
        empty.push_back(
            text_node(snapshot.filter == FilterMode::likely_game
                          ? "No likely game processes matched. Switch to Show all or Refresh."
                          : "No queryable processes are available. Press Refresh to try again.",
                      "muted", 44));
        nodes.push_back(card_node("Processes", std::move(empty), "cyan"));
    } else {
        for (const ProcessRecord& process : snapshot.visible_processes) {
            Json details = Json::array();
            details.push_back(text_node(process.image_path_utf8, "mono", 30));
            details.push_back(text_node("PID " + std::to_string(process.pid) + " · Parent " +
                                            std::to_string(process.parent_pid) + " · Start " +
                                            std::to_string(process.start_time_100ns),
                                        "muted", 28));
            Json attach_actions = Json::array();
            attach_actions.push_back(button_node(
                "process.attach." + std::to_string(process.pid), "Select / Attach", kAttachAction,
                {{"pid", process.pid}, {"start_time_100ns", process.start_time_100ns}}, "primary"));
            details.push_back(row_node(std::move(attach_actions)));
            nodes.push_back(
                card_node(process.base_name_utf8 + " · PID " + std::to_string(process.pid),
                          std::move(details), is_likely_game_process(process) ? "ok" : "cyan"));
        }
    }

    std::string serialized =
        Json{{"version", 1}, {"title", ""}, {"nodes", std::move(nodes)}}.dump();
    if (serialized.size() <= kMaximumPanelSpecBytes)
        return serialized;

    Json compact = Json::array();
    compact.push_back(text_node(
        "The process list exceeded the panel budget. Use Likely games to narrow it.", "bad", 48));
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
        candidate.image_path_utf8 = image_path.data();
        candidate.base_name_utf8 = base_name_from_path(candidate.image_path_utf8);
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
    if (payload_json.size() > kMaximumActionPayloadBytes)
        return {};
    if (payload_json.empty()) {
        valid = true;
        return Json::object();
    }
    Json payload = Json::parse(payload_json.begin(), payload_json.end(), nullptr, false, false);
    valid = !payload.is_discarded() && payload.is_object();
    return valid ? std::move(payload) : Json{};
}

void SAO_UI_CALL panel_action_callback(const char* action_id_utf8,
                                       const std::uint8_t* payload_json_utf8,
                                       std::size_t payload_len, void* user_data) {
    auto* owner = static_cast<Owner*>(user_data);
    if (owner == nullptr || action_id_utf8 == nullptr ||
        (payload_json_utf8 == nullptr && payload_len != 0U)) {
        return;
    }
    const std::string_view payload(
        payload_json_utf8 == nullptr ? "" : reinterpret_cast<const char*>(payload_json_utf8),
        payload_len);
    (void)owner->dispatch_action(action_id_utf8, payload);
}

void SAO_UI_CALL panel_event_callback(std::int32_t event_kind, void* user_data) {
    auto* owner = static_cast<Owner*>(user_data);
    if (owner != nullptr && event_kind == SAO_UI_PANEL_EVENT_CLOSE)
        (void)owner->close();
}

} // namespace

struct Owner::State {
    State(sao_ui_compositor_handle_t borrowed_compositor, Operations value)
        : compositor(borrowed_compositor), operations(std::move(value)) {}

    sao_ui_compositor_handle_t compositor{};
    Operations operations;
    mutable std::mutex mutex;
    std::atomic<bool> panel_initializing{};
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    bool visible{};
    bool destroying{};
    FilterMode filter{FilterMode::all};
    sao_status_t last_status{SAO_STATUS_OK};
    std::string status_text{"Not refreshed yet."};
    std::vector<ProcessRecord> processes;
    std::optional<ProcessRecord> attached_process;
    std::string rendered_spec_json;
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
    : state_(std::make_unique<State>(compositor, std::move(operations))) {
    if (state_->operations.current_process_id == 0U)
        state_->operations.current_process_id = GetCurrentProcessId();
}

Owner::~Owner() noexcept {
    if (!state_)
        return;
    sao_ui_panel_handle_t panel = nullptr;
    {
        std::lock_guard lock(state_->mutex);
        state_->destroying = true;
        panel = state_->panel;
    }
    if (panel != nullptr) {
        (void)sao_ui_panel_set_event_handler(panel, nullptr, nullptr);
        (void)sao_ui_panel_set_action_handler(panel, nullptr, nullptr);
        (void)sao_ui_panel_unregister(panel);
    }
    {
        std::lock_guard lock(state_->mutex);
        state_->panel = nullptr;
        state_->body = nullptr;
        state_->visible = false;
    }
}

sao_status_t Owner::require_owner_thread() const noexcept {
    if (!state_ || state_->compositor == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return sao_ui_compositor_require_owner_thread(state_->compositor);
}

sao_status_t Owner::ensure_panel() noexcept {
    const sao_status_t owner_status = require_owner_thread();
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->destroying)
            return SAO_STATUS_ERR_CANCELLED;
        if (state_->panel != nullptr && state_->body != nullptr)
            return SAO_STATUS_OK;
    }

    bool expected = false;
    if (!state_->panel_initializing.compare_exchange_strong(expected, true,
                                                            std::memory_order_acq_rel)) {
        return SAO_STATUS_ERR_CANCELLED;
    }
    struct InitializationGuard {
        std::atomic<bool>& flag;
        ~InitializationGuard() {
            flag.store(false, std::memory_order_release);
        }
    } initialization{state_->panel_initializing};

    SaoPanelDescriptor descriptor{};
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
    descriptor.theme_override_json_utf8 = kSolidDarkCyanTheme;
    descriptor.initial_opacity = 1.0F;
    descriptor.auto_scroll = true;

    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    sao_status_t status = sao_ui_panel_register(state_->compositor, &descriptor, &panel, &body);
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_panel_set_action_handler(panel, &panel_action_callback, this);
    if (status == SAO_STATUS_OK)
        status = sao_ui_panel_set_event_handler(panel, &panel_event_callback, this);
    if (status != SAO_STATUS_OK) {
        (void)sao_ui_panel_set_event_handler(panel, nullptr, nullptr);
        (void)sao_ui_panel_set_action_handler(panel, nullptr, nullptr);
        (void)sao_ui_panel_unregister(panel);
        return status;
    }

    bool accepted = false;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->destroying && state_->panel == nullptr) {
            state_->panel = panel;
            state_->body = body;
            accepted = true;
        }
    }
    if (!accepted) {
        (void)sao_ui_panel_set_event_handler(panel, nullptr, nullptr);
        (void)sao_ui_panel_set_action_handler(panel, nullptr, nullptr);
        (void)sao_ui_panel_unregister(panel);
        return SAO_STATUS_ERR_CANCELLED;
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
            body = state_->body;
            view.panel_created = true;
            view.visible = state_->visible;
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
        if (status != SAO_STATUS_OK)
            return status;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->body != body)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            state_->rendered_spec_json = std::move(spec);
        }
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t Owner::open() noexcept {
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

sao_status_t Owner::close() noexcept {
    const sao_status_t owner_status = require_owner_thread();
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
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

sao_status_t Owner::refresh() noexcept {
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;

    const auto enumerate = state_->operations.enumerate_snapshot;
    if (!enumerate) {
        {
            std::lock_guard lock(state_->mutex);
            state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
            state_->status_text = "Process enumeration operation is not configured.";
        }
        (void)publish();
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }

    try {
        std::vector<ProcessRecord> candidate;
        const sao_status_t enumerate_status = enumerate(candidate);
        if (enumerate_status != SAO_STATUS_OK) {
            {
                std::lock_guard lock(state_->mutex);
                state_->last_status = enumerate_status;
                state_->status_text = status_description(enumerate_status, "Refresh failed");
            }
            const sao_status_t publish_status = publish();
            return publish_status == SAO_STATUS_OK ? enumerate_status : publish_status;
        }
        candidate = normalize_snapshot(std::move(candidate), state_->operations.current_process_id);
        {
            std::lock_guard lock(state_->mutex);
            state_->processes = std::move(candidate);
            state_->last_status = SAO_STATUS_OK;
            state_->status_text = "Refresh complete: " + std::to_string(state_->processes.size()) +
                                  " queryable processes.";
        }
        return publish();
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t Owner::set_filter(FilterMode filter) noexcept {
    if (filter != FilterMode::all && filter != FilterMode::likely_game)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    {
        std::lock_guard lock(state_->mutex);
        state_->filter = filter;
        state_->last_status = SAO_STATUS_OK;
        state_->status_text = filter == FilterMode::all
                                  ? "Filter changed: showing all queryable processes."
                                  : "Filter changed: showing likely game processes.";
    }
    return publish();
}

sao_status_t Owner::attach(ProcessIdentity identity) noexcept {
    if (identity.pid == 0U || identity.pid == 4U || identity.start_time_100ns == 0U)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;

    bool selected_identity_exists = false;
    {
        std::lock_guard lock(state_->mutex);
        selected_identity_exists =
            std::ranges::any_of(state_->processes, [&](const ProcessRecord& process) {
                return process.identity() == identity;
            });
    }
    if (!selected_identity_exists) {
        {
            std::lock_guard lock(state_->mutex);
            state_->last_status = SAO_STATUS_ERR_NOT_FOUND;
            state_->status_text =
                "Attach rejected: selection is no longer in the current snapshot.";
        }
        (void)publish();
        return SAO_STATUS_ERR_NOT_FOUND;
    }

    const auto query_process = state_->operations.query_process;
    const auto attach_operation = state_->operations.attach;
    if (!query_process || !attach_operation)
        return SAO_STATUS_ERR_NOT_INITIALIZED;

    try {
        ProcessRecord candidate{};
        const sao_status_t query_status = query_process(identity.pid, candidate);
        if (query_status != SAO_STATUS_OK) {
            {
                std::lock_guard lock(state_->mutex);
                state_->last_status = query_status;
                state_->status_text = status_description(query_status, "Identity recheck failed");
            }
            (void)publish();
            return query_status;
        }
        std::vector<ProcessRecord> normalized = normalize_snapshot(
            std::vector<ProcessRecord>{candidate}, state_->operations.current_process_id);
        const bool process_missing = normalized.empty() || normalized.front().pid != identity.pid;
        if (process_missing || normalized.front().start_time_100ns != identity.start_time_100ns) {
            {
                std::lock_guard lock(state_->mutex);
                state_->last_status = SAO_STATUS_ERR_PROCESS_GONE;
                state_->status_text = process_missing
                                          ? "Attach rejected: process exited before attach."
                                          : "Attach rejected: PID identity changed before attach.";
            }
            (void)publish();
            return SAO_STATUS_ERR_PROCESS_GONE;
        }
        const ProcessRecord validated = std::move(normalized.front());
        {
            std::lock_guard lock(state_->mutex);
            state_->last_status = SAO_STATUS_OK;
            state_->status_text =
                "Identity verified; attaching PID " + std::to_string(identity.pid) + ".";
        }

        const sao_status_t attach_status = attach_operation(identity.pid);
        {
            std::lock_guard lock(state_->mutex);
            state_->last_status = attach_status;
            if (attach_status == SAO_STATUS_OK) {
                state_->attached_process = validated;
                state_->status_text = "Attached PID " + std::to_string(validated.pid) + " · " +
                                      validated.base_name_utf8;
            } else {
                state_->status_text = status_description(attach_status, "Attach failed");
            }
        }
        const sao_status_t publish_status = publish();
        return publish_status == SAO_STATUS_OK ? attach_status : publish_status;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t Owner::dispatch_action(std::string_view action_id,
                                    std::string_view payload_json) noexcept {
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
        if (value == "all")
            return set_filter(FilterMode::all);
        if (value == "likely_game")
            return set_filter(FilterMode::likely_game);
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
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
    return SAO_STATUS_ERR_NOT_FOUND;
}

sao_status_t Owner::snapshot(Snapshot& out) const noexcept {
    if (!state_)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    sao_ui_panel_handle_t panel = nullptr;
    try {
        Snapshot copy{};
        {
            std::lock_guard lock(state_->mutex);
            panel = state_->panel;
            copy.panel_created = panel != nullptr;
            copy.visible = state_->visible;
            copy.filter = state_->filter;
            copy.last_status = state_->last_status;
            copy.status_text = state_->status_text;
            copy.all_processes = state_->processes;
            copy.visible_processes = select_visible(copy.all_processes, copy.filter);
            copy.attached_process = state_->attached_process;
            copy.rendered_spec_json = state_->rendered_spec_json;
        }
        if (panel != nullptr) {
            SaoPanelState panel_state{};
            if (sao_ui_panel_get_state(panel, &panel_state) == SAO_STATUS_OK)
                copy.visible = panel_state.visible;
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
