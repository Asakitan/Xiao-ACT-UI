#include "sao/ai_editor/ai_editor_main_panel.h"

#include "sao/ai_editor/ai_editor_ipc.h"
#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/ai_editor/ai_editor_settings_panel.h"
#include "sao/ai_editor/gpu_hunt_central_panel.h"
#include "sao/ui/dialog.h"
#include "sao/ui/overlay_host.h"
#include "sao/ui/panel_sdk.h"
#include "sao/ui/theme.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr uint32_t kRequestTimeoutMs = 5000U;
constexpr size_t kMaximumRequestBytes = 256U * 1024U;
constexpr size_t kInitialResponseBytes = 256U * 1024U;
constexpr size_t kMaximumResponseBytes = 4U * 1024U * 1024U;
constexpr size_t kMaximumActionPayloadBytes = 64U * 1024U;
constexpr size_t kMaximumComposerBytes = 48U * 1024U;
constexpr size_t kOutputTrimBytes = 128U * 1024U;
constexpr size_t kUiTextChunkBytes = 3600U;
constexpr size_t kUiInputPreviewBytes = 1900U;
constexpr size_t kMaximumVisibleMessages = 24U;
constexpr size_t kMaximumMessageChunks = 4U;
constexpr size_t kMaximumHistoryEntries = 10U;
constexpr size_t kMaximumHistorySearchEntries = 100U;
constexpr size_t kMaximumHistoryQueryBytes = 1024U;
constexpr size_t kMaximumChoiceButtons = 6U;
constexpr size_t kMaximumWorkerQueue = 64U;
constexpr uint32_t kMaximumOrphanCancelAttempts = 2U;
constexpr size_t kMaximumEventsPerDrain = 64U;
constexpr size_t kMaximumPanelSpecBytes = 192U * 1024U;
constexpr size_t kMaximumStreamedAssistantBytes = 512U * 1024U;
constexpr size_t kMaximumRunDetailBytes = 64U * 1024U;
constexpr size_t kMaximumToolStatusBytes = 64U * 1024U;
constexpr size_t kMaximumUiNodeTextBytes = 4096U;
constexpr size_t kMaximumUiTitleBytes = 512U;
constexpr std::chrono::milliseconds kEventDrainInterval{75};
constexpr std::chrono::milliseconds kRunPollInterval{250};
constexpr std::chrono::milliseconds kBootstrapRetryInterval{2000};
constexpr int32_t kDialogCloseAdvanceMs = 1000;
constexpr int32_t kDefaultPanelWidth = 1120;
constexpr int32_t kDefaultPanelMinWidth = 720;
constexpr int32_t kWorkbenchHorizontalThreshold = 1024;
constexpr int32_t kMainMinimumWidth = 620;
constexpr int32_t kWorkbenchWidth = 340;
constexpr int32_t kWorkbenchMinimumWidth = 300;

enum class RpcTaskKind {
    Bootstrap,
    Generic,
    NewConversation,
    RefreshHistory,
    SearchHistory,
    LoadConversation,
    DeleteConversation,
    RestoreConversation,
    DuplicateConversation,
    SendMessage,
    DrainEvents,
    PollRun,
    CancelRun,
    RefreshModels,
};

enum class RunPhase {
    Idle,
    Starting,
    Running,
    Cancelling,
    Completed,
    Cancelled,
    Failed,
    Stale,
};

enum class DialogIntent {
    None,
    EditComposer,
    SearchHistory,
    ConfirmDelete,
    CopyText,
    ControlPalette,
    ControlInput,
    ControlConfirm,
};

struct ChoiceItem {
    std::string id;
    std::string label;
    std::string system_prompt;
};

struct ControlActionSpec {
    const char* group;
    const char* id;
    const char* label;
    const char* method;
    const char* defaults;
    bool read_only;
    bool destructive;
    bool advanced;
};

struct ControlToolItem {
    std::string name;
    std::string description;
    std::string permission;
    std::string permission_source;
    std::string category;
    bool confirmation_required{};
    bool read_only{true};
    json schema{json::object()};
};

struct ControlPromptItem {
    std::string id;
    std::string name;
    bool pinned{};
};

struct HistoryEntry {
    std::string id;
    std::string title;
    std::string model;
    std::string scope;
    int64_t saved_at{};
    size_t message_count{};
    std::vector<std::string> matched_fields;
    size_t match_count{};
    bool search_result{};
    bool content_available{};
    json messages{json::array()};
};

struct RpcResponse {
    bool ok{};
    bool connected{};
    json result;
    std::string raw;
    std::string error;
    int32_t status{SAO_AI_EDITOR_OK};
    uint64_t request_id{};
    int64_t elapsed_ms{};
    bool truncated{};
};

struct RpcTask {
    RpcTaskKind kind{RpcTaskKind::Generic};
    uint64_t generation{};
    std::string method;
    json params{json::object()};
    std::string conversation_id;
    std::string title;
    std::string scope;
    std::string query;
    size_t history_limit{kMaximumHistoryEntries};
    std::string prompt;
    std::string run_id;
    std::string provider_id;
    std::string model;
    std::string agent_id;
    std::string agent_system_prompt;
    std::string mode;
    std::string approval;
    std::string workflow_id;
    std::string context_mode;
    std::string control_group;
    std::string control_label;
    bool control_action{};
    json messages{json::array()};
    HistoryEntry restore_entry;
    bool canonical_reload{};
    bool orphan_cleanup{};
    uint32_t orphan_cancel_attempt{};
    bool history_operation{};
};

struct RpcStepResult {
    std::string method;
    RpcResponse response;
};

struct RpcCompletion {
    RpcTask task;
    std::vector<RpcStepResult> steps;
    bool cancelled{};
};

struct AiEditorMainPanelState {
    sao_ui_compositor_handle_t compositor{};
    sao_ai_editor_launcher_t launcher{};
    sao_ai_editor_gpu_hunt_panel_t gpu_hunt_panel{};
    sao_ai_editor_settings_panel_t settings_panel{};
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    sao_ui_dialog_handle_t dialog{};
    std::mutex mutex;
    std::mutex publish_mutex;
    std::condition_variable worker_cv;
    std::deque<RpcTask> rpc_queue;
    std::deque<RpcCompletion> completed_queue;
    std::thread worker;
    std::atomic<uint64_t> generation_atomic{1};
    std::atomic<uint64_t> request_counter{0};
    std::string output_text;
    std::string backend_status;
    std::string conversation_id;
    std::string conversation_title{"New Chat"};
    json conversation_messages{json::array()};
    std::string composer_text;
    std::string pending_user_text;
    std::vector<HistoryEntry> history;
    std::string history_query;
    size_t history_total{};
    size_t history_limit{kMaximumHistoryEntries};
    size_t conversation_messages_total{};
    std::vector<ChoiceItem> providers{{"", "Auto (Settings)", ""}};
    std::vector<ChoiceItem> models{{"", "Auto (Settings)", ""}};
    std::vector<ChoiceItem> agents{{"", "None", ""}};
    std::vector<ChoiceItem> workflows{{"", "None", ""}};
    std::string selected_provider;
    std::string selected_model;
    std::string selected_agent;
    std::string selected_mode{"agent"};
    std::string selected_approval{"default"};
    std::string selected_workflow;
    std::string selected_context{"conversation"};
    std::string selected_view{"inspector"};
    std::string active_run_id;
    std::string run_status{"idle"};
    std::string streamed_assistant_text;
    std::string thinking_status;
    std::string refusal_status;
    std::string tool_status;
    std::string usage_status;
    std::string run_error;
    std::string last_status_update;
    std::string control_palette_query;
    std::string control_group_filter{"all"};
    std::string control_status{"Idle"};
    std::string control_request_id;
    std::string control_last_method;
    std::string control_last_label;
    std::string control_last_group;
    json control_last_params{json::object()};
    json control_last_execution_params{json::object()};
    std::string control_last_tool_name;
    json control_last_tool_arguments{json::object()};
    std::string control_last_result;
    std::string control_last_error;
    std::string control_permission;
    std::string control_permission_source;
    std::string control_permission_category;
    bool control_confirmation_required{};
    std::string control_elapsed;
    bool control_cache_hit{};
    bool control_truncated{};
    bool control_last_read_only{};
    bool control_last_destructive{};
    bool control_last_advanced{};
    bool control_developer_advanced{};
    std::string pending_control_method;
    std::string pending_control_label;
    std::string pending_control_group;
    bool pending_control_read_only{};
    bool pending_control_destructive{};
    bool pending_control_advanced{};
    json pending_control_params{json::object()};
    json pending_control_execution_params{json::object()};
    std::string pending_control_tool_name;
    json pending_control_tool_arguments{json::object()};
    std::string pending_control_permission;
    std::string pending_control_permission_source;
    std::string pending_control_permission_category;
    bool pending_control_confirmation_required{};
    std::string pending_copy_text;
    std::vector<ControlToolItem> control_tools;
    std::vector<ControlPromptItem> control_prompts;
    std::string diagnostic_filter{"all"};
    bool diagnostics_auto_scroll{true};
    std::string terminal_reload_run_id;
    std::string last_spec;
    DialogIntent pending_dialog{DialogIntent::None};
    std::optional<HistoryEntry> pending_delete;
    std::optional<HistoryEntry> undo_entry;
    std::chrono::steady_clock::time_point undo_deadline{};
    std::chrono::steady_clock::time_point last_dialog_tick{};
    std::chrono::steady_clock::time_point next_event_drain{};
    std::chrono::steady_clock::time_point next_run_poll{};
    std::chrono::steady_clock::time_point next_bootstrap_attempt{};
    uint64_t conversation_generation{1};
    uint64_t cancelled_generation{};
    uint64_t starting_cancelled_generation{};
    size_t api_calls_in_flight{};
    RunPhase run_phase{RunPhase::Idle};
    bool backend_connected{};
    bool bootstrap_pending{};
    bool bootstrap_loaded{};
    bool selections_initialized{};
    bool event_drain_pending{};
    bool event_drain_unavailable{};
    bool run_poll_pending{};
    bool history_task_pending{};
    bool worker_active{};
    bool worker_stop_requested{};
    bool accepting{true};
    bool action_handler_attached{};
    bool event_handler_attached{};
    bool visible{};
    bool teardown_failed{};
    bool destroy_preflight_active{};
    bool destroy_claimed{};

    ~AiEditorMainPanelState() {
        {
            std::lock_guard lock(mutex);
            worker_stop_requested = true;
            rpc_queue.clear();
        }
        worker_cv.notify_all();
        if (worker.joinable())
            worker.join();
        if (dialog != nullptr)
            sao_ui_dialog_destroy(dialog);
    }
};

std::mutex& registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<sao_ai_editor_main_panel_t, std::unique_ptr<AiEditorMainPanelState>>&
registry() {
    static std::unordered_map<sao_ai_editor_main_panel_t, std::unique_ptr<AiEditorMainPanelState>>
        storage;
    return storage;
}

sao_ai_editor_main_panel_t allocate_handle() noexcept {
    static std::atomic<uintptr_t> next{1};
    const uintptr_t value = (next.fetch_add(1, std::memory_order_relaxed) << 4U) | 3U;
    return reinterpret_cast<sao_ai_editor_main_panel_t>(value);
}

int32_t map_ui_status(sao_status_t status) noexcept {
    switch (status) {
    case SAO_STATUS_OK:
        return SAO_AI_EDITOR_OK;
    case SAO_STATUS_ERR_INVALID_ARGUMENT:
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    case SAO_STATUS_ERR_NOT_INITIALIZED:
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    case SAO_STATUS_ERR_HANDLE_INVALID:
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    case SAO_STATUS_ERR_BUFFER_TOO_SMALL:
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    case SAO_STATUS_ERR_ALREADY_EXISTS:
        return SAO_AI_EDITOR_ERR_ALREADY_RUNNING;
    case SAO_STATUS_ERR_NOT_FOUND:
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    case SAO_STATUS_ERR_ACCESS_DENIED:
        return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
    case SAO_STATUS_ERR_CANCELLED:
        return SAO_AI_EDITOR_ERR_CANCELLED;
    case SAO_UI_PANEL_STATUS_ERR_BUSY:
        return SAO_AI_EDITOR_ERR_BUSY;
    default:
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

int32_t require_owner_thread(
    sao_ui_compositor_handle_t compositor) noexcept {
    return map_ui_status(sao_ui_compositor_require_owner_thread(compositor));
}

#if defined(SAO_AI_EDITOR_TESTING)
constexpr int32_t kTestPauseApiLeasePreflight = 1;
constexpr int32_t kTestPauseDestroyPreflight = 2;
std::atomic<int32_t> g_test_owner_preflight_pause_target{};
std::atomic<int32_t> g_test_owner_preflight_waiting_target{};
std::atomic<bool> g_test_destroy_registry_probe_completed{};

void pause_owner_preflight_for_testing(int32_t target) noexcept {
    if (g_test_owner_preflight_pause_target.load(std::memory_order_acquire) != target)
        return;
    g_test_owner_preflight_waiting_target.store(target, std::memory_order_release);
    while (g_test_owner_preflight_pause_target.load(std::memory_order_acquire) == target)
        std::this_thread::yield();
    g_test_owner_preflight_waiting_target.store(0, std::memory_order_release);
}

void note_destroy_registry_probe_completed_for_testing() noexcept {
    if (g_test_owner_preflight_pause_target.load(std::memory_order_acquire) ==
            kTestPauseDestroyPreflight &&
        g_test_owner_preflight_waiting_target.load(std::memory_order_acquire) ==
            kTestPauseDestroyPreflight) {
        g_test_destroy_registry_probe_completed.store(true, std::memory_order_release);
    }
}
#endif

std::string clamp_utf8_bytes(std::string value, size_t maximum) {
    if (value.size() <= maximum)
        return value;
    if (maximum <= 3U)
        return value.substr(0, maximum);
    size_t end = maximum - 3U;
    while (end > 0U && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U)
        --end;
    value.resize(end);
    value.append("...");
    return value;
}

json text_node(std::string text, std::string_view style = "value", int32_t height = 22) {
    return json{{"type", "text"},
                {"text", clamp_utf8_bytes(std::move(text), kMaximumUiNodeTextBytes)},
                {"style", style},
                {"height", height}};
}

json button_node(std::string id, std::string label, std::string action,
                 json payload = json::object(), std::string_view style = "default",
                 bool disabled = false) {
    json node{{"type", "button"},
              {"id", std::move(id)},
              {"label", clamp_utf8_bytes(std::move(label), kMaximumUiTitleBytes)},
              {"action", std::move(action)},
              {"style", style},
              {"height", 28}};
    if (!payload.empty())
        node["payload"] = std::move(payload);
    if (disabled)
        node["disabled"] = true;
    return node;
}

json input_node(std::string id, std::string value, std::string action, bool disabled,
                bool multiline = false) {
    if (!multiline && value.size() > kUiInputPreviewBytes) {
        size_t begin = value.size() - kUiInputPreviewBytes;
        while (begin < value.size() &&
               (static_cast<unsigned char>(value[begin]) & 0xc0U) == 0x80U) {
            ++begin;
        }
        value = "... " + value.substr(begin);
    }
    json node{{"type", "input"},
              {"id", std::move(id)},
              {"value", std::move(value)},
              {"input_type", multiline ? "multiline" : "text"},
              {"action", std::move(action)},
              {"height", multiline ? 128 : 42}};
    if (multiline) {
        node["multiline"] = true;
        node["accepts_newlines"] = true;
    }
    if (disabled)
        node["disabled"] = true;
    return node;
}

json row_node(json children, std::string_view align = "left") {
    return json{{"type", "row"}, {"align", align}, {"children", std::move(children)}};
}

json badge_node(std::string text, std::string_view style = "muted") {
    return json{{"type", "badge"},
                {"text", clamp_utf8_bytes(std::move(text), kMaximumUiTitleBytes)},
                {"style", style},
                {"height", 22}};
}

json card_node(std::string title, json children, std::string_view accent = "cyan") {
    return json{{"type", "card"},
                {"title", clamp_utf8_bytes(std::move(title), kMaximumUiTitleBytes)},
                {"accent", accent},
                {"children", std::move(children)}};
}

json section_node(std::string title, json children) {
    return json{{"type", "section"},
                {"title", clamp_utf8_bytes(std::move(title), kMaximumUiTitleBytes)},
                {"children", std::move(children)}};
}

json container_node(std::string id, json children, std::string_view layout,
                    int32_t width, int32_t min_width, float weight,
                    int32_t fallback_min_width) {
    return json{{"type", "section"},
                {"id", std::move(id)},
                {"container", true},
                {"layout", layout},
                {"orientation", layout},
                {"width", width},
                {"min_width", min_width},
                {"weight", weight},
                {"fallback", {{"layout", "vertical"},
                               {"min_width", fallback_min_width},
                               {"threshold_width", fallback_min_width}}},
                {"children", std::move(children)}};
}

std::string trim_copy(std::string_view value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
        --end;
    return std::string(value.substr(begin, end - begin));
}

std::string redact_control_text(std::string text);

std::string compact_text(std::string value, size_t maximum = 180U) {
    value = redact_control_text(std::move(value));
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    if (value.size() > maximum) {
        size_t end = maximum;
        while (end > 0 && end < value.size() &&
               (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) {
            --end;
        }
        value.resize(end);
        value.append("...");
    }
    return value;
}

bool valid_view(std::string_view value) {
    return value == "inspector" || value == "control" || value == "history" ||
           value == "diagnostics" || value == "platform";
}

std::string view_label(std::string_view value) {
    if (value == "control")
        return "Control Center";
    if (value == "history")
        return "History";
    if (value == "diagnostics")
        return "Diagnostics";
    if (value == "platform")
        return "Platform Tools";
    return "Inspector";
}

json host_status_badge(const AiEditorMainPanelState& state) {
    const sao_ui_overlay_host_handle_t host = sao_ui_compositor_host(state.compositor);
    if (host == nullptr)
        return badge_node("Headless", "muted");

    SaoOverlayHostState host_state{};
    if (sao_ui_overlay_host_get_state(host, &host_state) != SAO_STATUS_OK)
        return badge_node("Host state unavailable", "warn");
    if (host_state.capture_excluded)
        return badge_node("Capture protected", "ok");
    return badge_node("Capture visible / monitor fallback", "warn");
}
void append_bounded_text(std::string& destination, std::string_view text, size_t maximum) {
    if (text.empty() || maximum == 0U)
        return;
    if (text.size() >= maximum) {
        destination = clamp_utf8_bytes(std::string(text.substr(text.size() - maximum)), maximum);
        return;
    }
    if (destination.size() > maximum - text.size()) {
        size_t begin = destination.size() - (maximum - text.size());
        while (begin < destination.size() &&
               (static_cast<unsigned char>(destination[begin]) & 0xc0U) == 0x80U) {
            ++begin;
        }
        destination.erase(0, begin);
    }
    destination.append(text);
}

std::vector<std::string> split_utf8_chunks(std::string_view text, size_t maximum,
                                           size_t maximum_chunks) {
    std::vector<std::string> chunks;
    if (maximum == 0 || maximum_chunks == 0)
        return chunks;
    size_t begin = 0;
    while (begin < text.size() && chunks.size() < maximum_chunks) {
        size_t end = std::min(text.size(), begin + maximum);
        while (end > begin && end < text.size() &&
               (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) {
            --end;
        }
        if (end == begin)
            end = std::min(text.size(), begin + maximum);
        chunks.emplace_back(text.substr(begin, end - begin));
        begin = end;
    }
    if (begin < text.size() && !chunks.empty())
        chunks.back().append("\n... [message truncated]");
    return chunks;
}

void append_text_chunks(json& nodes, std::string_view text, std::string_view style,
                        int32_t height, size_t maximum_chunks = kMaximumMessageChunks) {
    for (auto& chunk : split_utf8_chunks(text, kUiTextChunkBytes, maximum_chunks))
        nodes.push_back(text_node(std::move(chunk), style, height));
}

std::string tail_text(std::string_view text, size_t maximum) {
    if (text.size() <= maximum)
        return std::string(text);
    size_t begin = text.size() - maximum;
    while (begin < text.size() &&
           (static_cast<unsigned char>(text[begin]) & 0xc0U) == 0x80U) {
        ++begin;
    }
    return "...\n" + std::string(text.substr(begin));
}

std::string format_iso_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t as_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    (void)::gmtime_s(&tm, &as_time_t);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", 1900 + tm.tm_year,
                  1 + tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

std::string format_saved_at(int64_t milliseconds) {
    if (milliseconds <= 0)
        return "unknown time";
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm tm{};
    if (::gmtime_s(&tm, &seconds) != 0)
        return "unknown time";
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02dZ", 1900 + tm.tm_year,
                  1 + tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return buffer;
}

void trim_output(std::string& output) noexcept {
    if (output.size() <= kOutputTrimBytes)
        return;
    const size_t drop = output.size() - kOutputTrimBytes;
    output.erase(0, drop);
    const size_t newline = output.find('\n');
    if (newline != std::string::npos && newline + 1 < output.size())
        output.erase(0, newline + 1);
}

std::string redact_control_text(std::string text);
void redact_control_json(json& value);
void clear_pending_control_locked(AiEditorMainPanelState& state);
std::string control_redacted_result(const RpcResponse& response);

void append_output_line(AiEditorMainPanelState& state, std::string line) {
    line = redact_control_text(std::move(line));
    std::lock_guard lock(state.mutex);
    if (!state.output_text.empty() && state.output_text.back() != '\n')
        state.output_text.push_back('\n');
    state.output_text.append(std::move(line));
    trim_output(state.output_text);
}

std::vector<std::string> split_key(std::string_view key) {
    std::vector<std::string> parts;
    size_t begin = 0;
    while (begin <= key.size()) {
        const size_t end = key.find('.', begin);
        const size_t count = end == std::string_view::npos ? key.size() - begin : end - begin;
        if (count == 0)
            return {};
        parts.emplace_back(key.substr(begin, count));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return parts;
}

const json* lookup_value(const json& root, std::string_view key) {
    if (!root.is_object())
        return nullptr;
    if (const auto direct = root.find(std::string(key)); direct != root.end())
        return &*direct;
    const auto parts = split_key(key);
    if (parts.empty())
        return nullptr;
    const json* current = &root;
    for (const auto& part : parts) {
        if (!current->is_object())
            return nullptr;
        const auto found = current->find(part);
        if (found == current->end())
            return nullptr;
        current = &*found;
    }
    return current;
}

std::string summary_value(const json& config, std::initializer_list<const char*> keys,
                          std::string fallback) {
    for (const char* key : keys) {
        const json* value = lookup_value(config, key);
        if (value == nullptr || value->is_null())
            continue;
        if (value->is_string() && !value->get<std::string>().empty())
            return value->get<std::string>();
        if (!value->is_string())
            return value->dump();
    }
    return fallback;
}

std::string normalize_approval(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (value == "default" || value == "bypass" || value == "autopilot")
        return value;
    return "default";
}

std::string approval_label(std::string_view approval) {
    if (approval == "bypass")
        return "Bypass / 跳过审批";
    if (approval == "autopilot")
        return "Autopilot / 自动驾驶";
    return "Default / 默认审批";
}

std::string approval_description(std::string_view approval) {
    if (approval == "bypass")
        return "All tool calls are auto-approved / 所有工具调用自动批准。";
    if (approval == "autopilot")
        return "Autonomously iterates from start to finish (preview) / 全自动迭代到完成（预览）。";
    return "Use the normal confirmation policy / 使用常规确认策略。";
}

std::string prompt_shortcut_text(std::string_view shortcut) {
    if (shortcut == "explain")
        return "Explain this code and point out the important control flow.";
    if (shortcut == "fix")
        return "Find the likely bug and propose a minimal fix.";
    if (shortcut == "tests")
        return "Add or update focused tests for this change.";
    if (shortcut == "review")
        return "Review this change for regressions and missing coverage.";
    return {};
}

std::string transport_message(int32_t status) {
    switch (status) {
    case SAO_AI_EDITOR_ERR_NOT_RUNNING:
        return "backend is not running";
    case SAO_AI_EDITOR_ERR_IPC_CONNECT_FAIL:
        return "named-pipe connection failed";
    case SAO_AI_EDITOR_ERR_IPC_TIMEOUT:
    case SAO_AI_EDITOR_ERR_TIMEOUT:
        return "request timed out";
    case SAO_AI_EDITOR_ERR_IPC_CLOSED:
        return "named-pipe connection closed";
    case SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL:
        return "response exceeded the bounded UI buffer";
    default:
        return "transport error " + std::to_string(status);
    }
}

RpcResponse request_backend(sao_ai_editor_launcher_t launcher,
                            std::atomic<uint64_t>& request_counter,
                            std::string_view method,
                            const json& params = json::object()) {
    RpcResponse response;
    const auto request_started = std::chrono::steady_clock::now();
    struct ResponseMetrics {
        RpcResponse& response;
        std::chrono::steady_clock::time_point started;
        ~ResponseMetrics() {
            response.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
        }
    } response_metrics{response, request_started};
    if (launcher == nullptr) {
        response.status = SAO_AI_EDITOR_ERR_NOT_RUNNING;
        response.error = "backend not attached";
        return response;
    }
    const uint64_t id = request_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    response.request_id = id;
    const json request{
        {"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}, {"params", params}};
    const std::string request_body = request.dump();
    if (request_body.size() > kMaximumRequestBytes ||
        request_body.size() > std::numeric_limits<uint32_t>::max()) {
        response.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        response.error = "request exceeds the 256 KiB main-panel limit";
        return response;
    }

    std::vector<char> response_buffer(kInitialResponseBytes);
    uint32_t response_len = 0;
    response.status = sao_ai_editor_request(
        launcher, request_body.data(), static_cast<uint32_t>(request_body.size()),
        response_buffer.data(), static_cast<uint32_t>(response_buffer.size()), &response_len,
        kRequestTimeoutMs);
    if (response.status == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        if (response_len == 0 || response_len > kMaximumResponseBytes ||
            response_len > std::numeric_limits<uint32_t>::max()) {
            response.status = SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
            response.error = "response exceeds the 4 MiB main-panel limit";
            return response;
        }
        response_buffer.resize(response_len);
        uint32_t retry_len = 0;
        response.status = sao_ai_editor_request(
            launcher, nullptr, 0, response_buffer.data(),
            static_cast<uint32_t>(response_buffer.size()), &retry_len, kRequestTimeoutMs);
        response_len = retry_len;
    }
    if (response.status != SAO_AI_EDITOR_OK) {
        response.error = transport_message(response.status);
        return response;
    }
    if (response_len > response_buffer.size()) {
        response.status = SAO_AI_EDITOR_ERR_PROTOCOL;
        response.error = "backend returned an invalid response length";
        return response;
    }
    response.connected = true;
    response.truncated = response_buffer.size() >= kMaximumResponseBytes;
    response.raw.assign(response_buffer.data(), response_buffer.data() + response_len);
    const json document = json::parse(response_buffer.data(), response_buffer.data() + response_len,
                                      nullptr, false, false);
    if (document.is_discarded() || !document.is_object() ||
        document.value("jsonrpc", std::string{}) != "2.0") {
        response.status = SAO_AI_EDITOR_ERR_PROTOCOL;
        response.error = "backend response is not valid JSON-RPC 2.0";
        return response;
    }
    if (const auto error = document.find("error"); error != document.end() && error->is_object()) {
        response.status = SAO_AI_EDITOR_ERR_PROTOCOL;
        response.error = error->value("message", "backend rejected the request");
        if (const auto data = error->find("data"); data != error->end())
            response.error.append(" · ").append(data->dump());
        return response;
    }
    const auto result = document.find("result");
    if (result == document.end()) {
        response.status = SAO_AI_EDITOR_ERR_PROTOCOL;
        response.error = "backend response has no result";
        return response;
    }
    response.result = *result;
    response.ok = true;
    return response;
}

RpcStepResult rpc_step(AiEditorMainPanelState& state, std::string method,
                       json params = json::object()) {
    RpcStepResult step;
    step.method = std::move(method);
    step.response = request_backend(state.launcher, state.request_counter, step.method, params);
    return step;
}

bool task_cancelled(AiEditorMainPanelState& state, const RpcTask& task) {
    std::lock_guard lock(state.mutex);
    return state.worker_stop_requested ||
           (task.generation != 0U &&
            (task.generation != state.conversation_generation ||
             task.generation <= state.cancelled_generation));
}

std::string accepted_run_id(const RpcCompletion& completion) {
    const auto found = std::ranges::find_if(
        completion.steps,
        [](const RpcStepResult& step) { return step.method == "chat.run"; });
    if (found == completion.steps.end() || !found->response.ok ||
        !found->response.result.is_object()) {
        return {};
    }
    return found->response.result.value("runId", std::string{});
}

bool completion_has_successful_step(const RpcCompletion& completion, std::string_view method) {
    return std::ranges::any_of(completion.steps, [&](const RpcStepResult& step) {
        return step.method == method && step.response.ok;
    });
}

bool queue_orphan_cancel_locked(AiEditorMainPanelState& state, uint64_t generation,
                                std::string run_id, uint32_t attempt = 1U) {
    if (run_id.empty() || state.worker_stop_requested || !state.accepting ||
        state.rpc_queue.size() >= kMaximumWorkerQueue || attempt == 0U ||
        attempt > kMaximumOrphanCancelAttempts)
        return false;
    const bool already_queued = std::ranges::any_of(state.rpc_queue, [&](const RpcTask& queued) {
        return queued.kind == RpcTaskKind::CancelRun && queued.orphan_cleanup &&
               queued.run_id == run_id;
    });
    if (already_queued)
        return true;
    RpcTask cancel;
    cancel.kind = RpcTaskKind::CancelRun;
    cancel.generation = generation;
    cancel.run_id = std::move(run_id);
    cancel.orphan_cleanup = true;
    cancel.orphan_cancel_attempt = attempt;
    state.rpc_queue.push_front(std::move(cancel));
    return true;
}

RpcCompletion execute_task(AiEditorMainPanelState& state, RpcTask task) {
    RpcCompletion completion;
    completion.task = std::move(task);
    const auto append = [&](std::string method, json params = json::object()) -> bool {
        completion.steps.push_back(rpc_step(state, std::move(method), std::move(params)));
        return completion.steps.back().response.ok;
    };

    switch (completion.task.kind) {
    case RpcTaskKind::Bootstrap:
        if (!append("runtime.initialize"))
            break;
        (void)append("providers.list");
        (void)append("agents.list_defs");
        (void)append("workflows.list_defs");
        (void)append("conversation.list", {{"scope", "all"}, {"limit", completion.task.history_limit}});
        (void)append("conversation.stats", {{"scope", "all"}});
        break;
    case RpcTaskKind::Generic:
        (void)append(completion.task.method, completion.task.params);
        break;
    case RpcTaskKind::NewConversation:
        if (task_cancelled(state, completion.task)) {
            completion.cancelled = true;
            break;
        }
        (void)append("conversation.create",
                     {{"title", completion.task.title},
                      {"model", completion.task.model},
                      {"scope", "workspace"}});
        if (!completion.steps.empty() && completion.steps.back().response.ok &&
            completion.steps.back().response.result.is_object()) {
            completion.task.conversation_id =
                completion.steps.back().response.result.value("id", std::string{});
        }
        completion.cancelled = task_cancelled(state, completion.task);
        break;
    case RpcTaskKind::RefreshHistory:
        (void)append("conversation.list",
                     {{"scope", "all"}, {"limit", completion.task.history_limit}});
        (void)append("conversation.stats", {{"scope", "all"}});
        break;
    case RpcTaskKind::SearchHistory:
        (void)append("conversation.search", {{"query", completion.task.query},
                                             {"scope", "all"},
                                             {"limit", completion.task.history_limit}});
        break;
    case RpcTaskKind::LoadConversation:
        (void)append("conversation.get", {{"id", completion.task.conversation_id}});
        break;
    case RpcTaskKind::DeleteConversation:
        if (append("conversation.get", {{"id", completion.task.conversation_id}})) {
            const json& document = completion.steps.back().response.result;
            if (document.is_object()) {
                const auto messages = document.find("messages");
                if (messages != document.end() && messages->is_array()) {
                    completion.task.restore_entry.messages = *messages;
                    completion.task.restore_entry.content_available = true;
                }
                completion.task.restore_entry.message_count = document.value(
                    "messageCount", completion.task.restore_entry.messages.is_array()
                                          ? completion.task.restore_entry.messages.size()
                                          : completion.task.restore_entry.message_count);
            }
        }
        if (append("conversation.delete", {{"id", completion.task.conversation_id}})) {
            if (completion.task.query.empty()) {
            (void)append("conversation.list",
                         {{"scope", "all"}, {"limit", completion.task.history_limit}});
        } else {
                (void)append("conversation.search", {{"query", completion.task.query},
                                                     {"scope", "all"},
                                                     {"limit", completion.task.history_limit}});
            }
        }
        break;
    case RpcTaskKind::DuplicateConversation: {
        json params{{"sourceId", completion.task.conversation_id}};
        if (!completion.task.title.empty())
            params["title"] = completion.task.title;
        if (!completion.task.scope.empty())
            params["scope"] = completion.task.scope;
        if (append("conversation.duplicate", std::move(params))) {
            if (completion.task.query.empty()) {
                (void)append("conversation.list",
                             {{"scope", "all"}, {"limit", completion.task.history_limit}});
            } else {
                (void)append("conversation.search", {{"query", completion.task.query},
                                                     {"scope", "all"},
                                                     {"limit", completion.task.history_limit}});
            }
        }
        break;
    }
    case RpcTaskKind::RestoreConversation: {
        const HistoryEntry& entry = completion.task.restore_entry;
        if (!entry.content_available || !entry.messages.is_array()) {
            RpcResponse response;
            response.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            response.error = "Undo content is unavailable; no empty conversation was created.";
            completion.steps.push_back({"conversation.restore", std::move(response)});
            break;
        }
        if (!append("conversation.create",
                    {{"title", entry.title},
                     {"model", entry.model},
                     {"scope", entry.scope == "system" ? "system" : "workspace"}}))
            break;
        const std::string restored_id =
            completion.steps.back().response.result.value("id", std::string{});
        if (restored_id.empty())
            break;
        bool append_complete = true;
        for (const auto& message : entry.messages) {
            if (!append("conversation.append", {{"id", restored_id}, {"message", message}})) {
                append_complete = false;
                break;
            }
        }
        if (!append_complete) {
            (void)append("conversation.delete", {{"id", restored_id}});
            break;
        }
        (void)append("conversation.list",
                     {{"scope", "all"}, {"limit", completion.task.history_limit}});
        (void)append("conversation.stats", {{"scope", "all"}});
        break;
    }
    case RpcTaskKind::SendMessage: {
        if (task_cancelled(state, completion.task)) {
            completion.cancelled = true;
            break;
        }
        std::string conversation_id = completion.task.conversation_id;
        json messages = completion.task.messages.is_array() ? completion.task.messages
                                                            : json::array();
        if (conversation_id.empty()) {
            if (!append("conversation.create",
                        {{"title", completion.task.title},
                         {"model", completion.task.model},
                         {"scope", "workspace"}})) {
                break;
            }
            conversation_id = completion.steps.back().response.result.value("id", std::string{});
            if (conversation_id.empty())
                break;
            completion.task.conversation_id = conversation_id;
            if (task_cancelled(state, completion.task)) {
                completion.cancelled = true;
                break;
            }
        }
        if (!append("conversation.append",
                    {{"id", conversation_id},
                     {"message", {{"role", "user"}, {"content", completion.task.prompt}}}})) {
            break;
        }
        const json& appended = completion.steps.back().response.result;
        if (appended.is_object() && appended.contains("messages") && appended["messages"].is_array())
            messages = appended["messages"];
        else
            messages.push_back({{"role", "user"}, {"content", completion.task.prompt}});
        completion.task.conversation_id = conversation_id;
        if (task_cancelled(state, completion.task)) {
            completion.cancelled = true;
            break;
        }
        if (completion.task.context_mode == "current_turn") {
            messages = json::array({{{"role", "user"}, {"content", completion.task.prompt}}});
        }
        if (!completion.task.agent_system_prompt.empty()) {
            messages.insert(messages.begin(),
                            json{{"role", "system"},
                                 {"content", completion.task.agent_system_prompt}});
        }
        json params{{"conversationId", conversation_id},
                    {"messages", std::move(messages)},
                    {"mode", completion.task.mode},
                    {"stream", true}};
        if (!completion.task.provider_id.empty())
            params["providerId"] = completion.task.provider_id;
        if (!completion.task.model.empty())
            params["model"] = completion.task.model;
        params["approval"] = normalize_approval(completion.task.approval);
        if (!completion.task.agent_id.empty())
            params["agentId"] = completion.task.agent_id;
        if (!completion.task.workflow_id.empty())
            params["workflowId"] = completion.task.workflow_id;
        params["context"] = completion.task.context_mode;
        (void)append("chat.run", std::move(params));
        completion.task.conversation_id = std::move(conversation_id);
        if (task_cancelled(state, completion.task)) {
            completion.cancelled = true;
            const RpcResponse& run_response = completion.steps.back().response;
            const std::string run_id = run_response.ok && run_response.result.is_object()
                                           ? run_response.result.value("runId", std::string{})
                                           : std::string{};
            if (!run_id.empty()) {
                completion.task.run_id = run_id;
                (void)append("run.cancel", {{"runId", run_id}});
            }
        }
        break;
    }
    case RpcTaskKind::DrainEvents:
        (void)append("events.drain", {{"limit", kMaximumEventsPerDrain}});
        break;
    case RpcTaskKind::PollRun:
        (void)append("run.status", {{"runId", completion.task.run_id}});
        break;
    case RpcTaskKind::CancelRun:
        (void)append("run.cancel", {{"runId", completion.task.run_id}});
        break;
    case RpcTaskKind::RefreshModels: {
        json params = json::object();
        if (!completion.task.provider_id.empty())
            params["providerId"] = completion.task.provider_id;
        (void)append("models.list", std::move(params));
        break;
    }
    }
    return completion;
}

void rpc_worker_main(AiEditorMainPanelState* state) {
    for (;;) {
        RpcTask task;
        {
            std::unique_lock lock(state->mutex);
            state->worker_cv.wait(lock, [&] {
                return state->worker_stop_requested || !state->rpc_queue.empty();
            });
            if (state->worker_stop_requested && state->rpc_queue.empty())
                break;
            task = std::move(state->rpc_queue.front());
            state->rpc_queue.pop_front();
            state->worker_active = true;
        }
        RpcCompletion completion;
        try {
            completion = execute_task(*state, task);
        } catch (const std::exception& error) {
            completion.task = std::move(task);
            RpcResponse response;
            response.status = SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            response.error = std::string("worker exception: ") + error.what();
            completion.steps.push_back({"worker", std::move(response)});
        } catch (...) {
            completion.task = std::move(task);
            RpcResponse response;
            response.status = SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            response.error = "worker exception";
            completion.steps.push_back({"worker", std::move(response)});
        }
        {
            std::lock_guard lock(state->mutex);
            const bool orphan_cancel = completion.task.kind == RpcTaskKind::CancelRun &&
                                       completion.task.orphan_cleanup;
            if (orphan_cancel) {
                completion.task.generation = 0U;
                if (!completion_has_successful_step(completion, "run.cancel") &&
                    completion.task.orphan_cancel_attempt < kMaximumOrphanCancelAttempts &&
                    queue_orphan_cancel_locked(
                        *state, 0U, completion.task.run_id,
                        completion.task.orphan_cancel_attempt + 1U)) {
                    completion.cancelled = true;
                }
            } else if (!completion.cancelled && completion.task.generation != 0U &&
                       (completion.task.generation != state->conversation_generation ||
                        completion.task.generation <= state->cancelled_generation)) {
                completion.cancelled = true;
            }
            if (completion.cancelled && completion.task.kind == RpcTaskKind::SendMessage &&
                !completion_has_successful_step(completion, "run.cancel")) {
                completion.task.run_id = accepted_run_id(completion);
                if (!queue_orphan_cancel_locked(*state, completion.task.generation,
                                                completion.task.run_id)) {
                    state->run_error = "Failed to queue orphan run cancellation.";
                }
            }
            state->worker_active = false;
            if (!state->worker_stop_requested)
                state->completed_queue.push_back(std::move(completion));
        }
        state->worker_cv.notify_all();
    }
    {
        std::lock_guard lock(state->mutex);
        state->worker_active = false;
    }
    state->worker_cv.notify_all();
}

bool queue_rpc_task(AiEditorMainPanelState& state, RpcTask task, std::string* error = nullptr) {
    {
        std::lock_guard lock(state.mutex);
        if (!state.accepting || state.worker_stop_requested) {
            if (error != nullptr)
                *error = "main panel is shutting down";
            return false;
        }
        if (state.launcher == nullptr) {
            if (error != nullptr)
                *error = "backend not attached";
            return false;
        }
        if (state.rpc_queue.size() >= kMaximumWorkerQueue) {
            if (error != nullptr)
                *error = "backend request queue is full";
            return false;
        }
        state.rpc_queue.push_back(std::move(task));
    }
    state.worker_cv.notify_one();
    return true;
}

void add_choice(std::vector<ChoiceItem>& choices, ChoiceItem item) {
    if (item.id.empty() || item.id.size() > 256U)
        return;
    const auto found = std::ranges::find(choices, item.id, &ChoiceItem::id);
    if (found == choices.end())
        choices.push_back(std::move(item));
}

std::string choice_label(const std::vector<ChoiceItem>& choices, std::string_view id,
                         std::string fallback) {
    const auto found = std::ranges::find(choices, id, &ChoiceItem::id);
    return found == choices.end() ? std::move(fallback) : found->label;
}

std::string selected_agent_prompt(const AiEditorMainPanelState& state) {
    const auto found = std::ranges::find(state.agents, state.selected_agent, &ChoiceItem::id);
    return found == state.agents.end() ? std::string{} : found->system_prompt;
}

std::string title_from_prompt(std::string_view prompt) {
    const size_t line_end = prompt.find_first_of("\r\n");
    std::string title = trim_copy(prompt.substr(0, line_end));
    if (title.empty())
        return "New Chat " + format_iso_timestamp();
    return compact_text(std::move(title), 72U);
}

std::string run_phase_style(RunPhase phase) {
    switch (phase) {
    case RunPhase::Running:
    case RunPhase::Starting:
    case RunPhase::Cancelling:
        return "warn";
    case RunPhase::Completed:
        return "ok";
    case RunPhase::Failed:
        return "bad";
    case RunPhase::Cancelled:
    case RunPhase::Stale:
        return "muted";
    case RunPhase::Idle:
        return "accent";
    }
    return "muted";
}

bool run_is_active(RunPhase phase) {
    return phase == RunPhase::Starting || phase == RunPhase::Running ||
           phase == RunPhase::Cancelling;
}

std::string message_content(const json& message) {
    if (!message.is_object() || !message.contains("content"))
        return "[invalid message]";
    const json& content = message["content"];
    if (content.is_string())
        return content.get<std::string>();
    return content.dump();
}

std::string metrics_summary(const json& metrics) {
    if (!metrics.is_object() || metrics.empty())
        return {};
    std::vector<std::string> fields;
    const auto append_number = [&](const char* key, std::string label) {
        const auto found = metrics.find(key);
        if (found == metrics.end() || !found->is_number())
            return;
        std::ostringstream stream;
        stream << std::move(label) << '=';
        if (found->is_number_float())
            stream << std::fixed << std::setprecision(3) << found->get<double>();
        else
            stream << found->dump();
        fields.push_back(stream.str());
    };
    append_number("promptTokens", "prompt");
    append_number("completionTokens", "completion");
    append_number("totalMs", "totalMs");
    append_number("ttfMs", "ttfMs");
    append_number("tokensPerSecond", "tok/s");
    append_number("costUsd", "costUsd");
    std::string summary;
    for (const auto& field : fields) {
        if (!summary.empty())
            summary.append(" · ");
        summary.append(field);
    }
    return summary;
}

std::vector<HistoryEntry> parse_history(const json& value, size_t maximum_entries) {
    std::vector<HistoryEntry> entries;
    const json* source = &value;
    if (value.is_object()) {
        const auto items = value.find("items");
        if (items == value.end() || !items->is_array())
            return entries;
        source = &*items;
    }
    if (!source->is_array())
        return entries;
    for (const auto& item : *source) {
        if (!item.is_object())
            continue;
        HistoryEntry entry;
        entry.id = item.value("id", std::string{});
        if (entry.id.empty() || entry.id.size() > 1024U)
            continue;
        entry.title = compact_text(item.value("title", std::string{"Untitled"}), 500U);
        entry.model = compact_text(item.value("model", std::string{}), 256U);
        entry.scope = compact_text(item.value("scope", std::string{"workspace"}), 64U);
        entry.saved_at = item.value("savedAt", int64_t{0});
        entry.message_count = item.value("messageCount", size_t{0});
        entries.push_back(std::move(entry));
        if (entries.size() >= maximum_entries)
            break;
    }
    return entries;
}

size_t history_total_from_steps(const std::vector<RpcStepResult>& steps, size_t fallback) {
    for (const auto& step : steps) {
        if (step.method == "conversation.stats" && step.response.ok &&
            step.response.result.is_object())
            return step.response.result.value("total", fallback);
        if ((step.method == "conversation.search" || step.method == "conversation.list") &&
            step.response.ok && step.response.result.is_object() &&
            step.response.result.contains("total"))
            return step.response.result.value("total", fallback);
    }
    return fallback;
}

std::vector<HistoryEntry> parse_history_search_results(const json& value, size_t maximum_entries) {
    std::vector<HistoryEntry> entries;
    if (!value.is_object())
        return entries;
    const json results = value.value("results", json::array());
    if (!results.is_array())
        return entries;
    for (const auto& item : results) {
        if (!item.is_object())
            continue;
        HistoryEntry entry;
        entry.id = item.value("id", std::string{});
        if (entry.id.empty() || entry.id.size() > 1024U)
            continue;
        entry.title = compact_text(item.value("title", std::string{"Untitled"}), 500U);
        entry.scope = compact_text(item.value("scope", std::string{"workspace"}), 64U);
        entry.saved_at = item.value("savedAt", int64_t{0});
        entry.match_count = item.value("matchCount", size_t{0});
        entry.search_result = true;
        const json matched_fields = item.value("matchedFields", json::array());
        if (matched_fields.is_array()) {
            for (const auto& field : matched_fields) {
                if (!field.is_string())
                    continue;
                const std::string label = compact_text(field.get<std::string>(), 64U);
                if (!label.empty())
                    entry.matched_fields.push_back(label);
            }
        }
        entries.push_back(std::move(entry));
        if (entries.size() >= maximum_entries)
            break;
    }
    return entries;
}

void apply_conversation_document(AiEditorMainPanelState& state, const json& document) {
    if (!document.is_object())
        return;
    const std::string conversation_id = document.value("id", std::string{});
    if (conversation_id.empty() || conversation_id.size() > 1024U)
        return;
    state.conversation_id = conversation_id;
    state.conversation_title =
        compact_text(document.value("title", std::string{"Untitled"}), 500U);
    const json messages = document.value("messages", json::array());
    state.conversation_messages = messages.is_array() ? messages : json::array();
    state.conversation_messages_total = document.value(
        "messagesTotal", document.value("messageCount", state.conversation_messages.size()));
}

void apply_choice_items(std::vector<ChoiceItem>& target, const json& items,
                        std::string_view fallback_label) {
    if (!items.is_array())
        return;
    for (const auto& item : items) {
        if (!item.is_object())
            continue;
        ChoiceItem choice;
        choice.id = item.value("id", std::string{});
        choice.label = compact_text(item.value("name", item.value("label", choice.id)), 160U);
        if (choice.label.empty())
            choice.label = std::string(fallback_label);
        choice.system_prompt = item.value(
            "systemPrompt", item.value("system_prompt", item.value("prompt", std::string{})));
        add_choice(target, std::move(choice));
    }
}

void update_backend_status(AiEditorMainPanelState& state, std::string_view method,
                           const RpcResponse& response) {
    state.backend_connected = response.connected;
    state.last_status_update = format_iso_timestamp();
    if (response.ok) {
        state.backend_status = "Connected · last request: " + std::string(method);
    } else if (response.connected) {
        state.backend_status = "Connected · request error: " + compact_text(response.error, 800U);
    } else {
        state.backend_status = "Offline / 离线 · " + compact_text(response.error, 800U);
    }
}

void record_completion_output(AiEditorMainPanelState& state, const RpcStepResult& step,
                              bool include_success_payload = false) {
    if (!step.response.ok) {
        append_output_line(state, "[" + format_iso_timestamp() + "] " + step.method +
                                      " -> error: " + redact_control_text(step.response.error));
    } else if (include_success_payload) {
        append_output_line(state, "[" + format_iso_timestamp() + "] " + step.method + " -> " +
                                      compact_text(redact_control_text(step.response.raw), 1200U));
    }
}

void apply_control_completion(AiEditorMainPanelState& state, const RpcCompletion& completion) {
    if (completion.steps.empty()) {
        std::lock_guard lock(state.mutex);
        clear_pending_control_locked(state);
        return;
    }
    const RpcStepResult& step = completion.steps.back();
    const RpcResponse& response = step.response;
    const std::string result_text = response.ok ? control_redacted_result(response) : std::string{};
    {
        std::lock_guard lock(state.mutex);
        state.control_status = response.ok ? "Success" : "Failed";
        state.control_request_id = response.request_id == 0 ? std::string{} :
                                    std::to_string(response.request_id);
        state.control_last_method = completion.task.method;
        state.control_last_label = completion.task.control_label.empty()
                                       ? completion.task.method
                                       : completion.task.control_label;
        state.control_last_params = completion.task.params;
        redact_control_json(state.control_last_params);
        state.control_last_execution_params = completion.task.params;
        state.control_last_result = result_text;
        state.control_last_error = response.ok ? std::string{}
                                                : redact_control_text(compact_text(response.error, 1600U));
        if (completion.task.method == "tools.call" && completion.task.params.is_object()) {
            state.control_last_tool_name = completion.task.params.value("name", std::string{});
            state.control_last_tool_arguments = completion.task.params.value("arguments", json::object());
        }
        state.control_elapsed = std::to_string(response.elapsed_ms) + " ms";
        state.control_truncated = response.truncated ||
                                  (response.result.is_object() && response.result.value("truncated", false));
        state.control_cache_hit = response.result.is_object() && response.result.value("cacheHit", false);
        state.control_permission = response.result.is_object()
                                       ? response.result.value("permission", std::string{"-"})
                                       : "-";
        state.control_permission_source = response.result.is_object()
                                              ? response.result.value("permissionSource", response.result.value("source", std::string{"-"}))
                                              : state.control_permission_source.empty() ? "-" : state.control_permission_source;
        state.control_permission_category = response.result.is_object()
                                                ? response.result.value("category", state.control_permission_category)
                                                : state.control_permission_category;
        state.control_confirmation_required = response.result.is_object()
                                                  ? response.result.value("confirmationRequired", state.control_confirmation_required)
                                                  : state.control_confirmation_required;
        if (response.ok && step.method == "tools.list" && response.result.is_object()) {
            state.control_tools.clear();
            const json tools = response.result.value("tools", json::array());
            if (tools.is_array()) {
                for (const auto& item : tools) {
                    if (!item.is_object())
                        continue;
                    ControlToolItem tool;
                    tool.name = item.value("name", std::string{});
                    if (tool.name.empty())
                        continue;
                    tool.description = item.value("description", std::string{});
                    tool.permission = item.value("permission", std::string{"unknown"});
                    tool.permission_source = item.value("permissionSource", item.value("source", std::string{"unknown"}));
                    tool.category = item.value("category", item.value("permissionCategory", std::string{}));
                    tool.confirmation_required = item.value("confirmationRequired", false);
                    tool.read_only = item.value("readOnly", true);
                    tool.schema = item.value("parameters", item.value("inputSchema", json::object()));
                    state.control_tools.push_back(std::move(tool));
                }
            }
        }
        if (response.ok && step.method == "prompts.list_defs" && response.result.is_object()) {
            state.control_prompts.clear();
            const json items = response.result.value("items", json::array());
            if (items.is_array()) {
                for (const auto& item : items) {
                    if (!item.is_object())
                        continue;
                    ControlPromptItem prompt;
                    prompt.id = item.value("id", std::string{});
                    prompt.name = item.value("name", prompt.id);
                    prompt.pinned = item.value("pinned", false);
                    if (!prompt.id.empty())
                        state.control_prompts.push_back(std::move(prompt));
                }
            }
        }
    }
    if (response.ok) {
        append_output_line(state, "[" + format_iso_timestamp() + "] control " + step.method +
                                      " -> Success · request " +
                                      (response.request_id == 0 ? std::string{"-"} : std::to_string(response.request_id)) +
                                      " · " + std::to_string(response.elapsed_ms) + " ms");
    } else {
        append_output_line(state, "[" + format_iso_timestamp() + "] control " + step.method +
                                      " -> Failed · request " +
                                      (response.request_id == 0 ? std::string{"-"} : std::to_string(response.request_id)) +
                                      " · " + redact_control_text(response.error));
    }
    {
        std::lock_guard lock(state.mutex);
        clear_pending_control_locked(state);
    }
}
void finish_run_from_status(AiEditorMainPanelState& state, const json& status) {
    const std::string run_status = status.value("status", std::string{"failed"});
    state.run_status = compact_text(run_status, 128U);
    const json result = status.value("result", json::object());
    const std::string content = result.value("content", std::string{});
    if (!content.empty())
        state.streamed_assistant_text = clamp_utf8_bytes(content, kMaximumStreamedAssistantBytes);
    state.usage_status = metrics_summary(result.value("metrics", json::object()));
    if (result.contains("thinking"))
        state.thinking_status = clamp_utf8_bytes(
            message_content({{"content", result["thinking"]}}), kMaximumRunDetailBytes);
    else if (result.contains("reasoning"))
        state.thinking_status = clamp_utf8_bytes(
            message_content({{"content", result["reasoning"]}}), kMaximumRunDetailBytes);
    if (result.contains("refusal"))
        state.refusal_status = clamp_utf8_bytes(
            message_content({{"content", result["refusal"]}}), kMaximumRunDetailBytes);
    if (result.contains("toolCalls"))
        state.tool_status = compact_text(result["toolCalls"].dump(), kMaximumToolStatusBytes);
    else if (result.contains("tool_calls"))
        state.tool_status = compact_text(result["tool_calls"].dump(), kMaximumToolStatusBytes);
    state.run_error = compact_text(status.value("error", std::string{}), 1200U);
    state.run_poll_pending = false;
    if (run_status == "completed")
        state.run_phase = RunPhase::Completed;
    else if (run_status == "cancelled")
        state.run_phase = RunPhase::Cancelled;
    else if (run_status == "stale")
        state.run_phase = RunPhase::Stale;
    else
        state.run_phase = RunPhase::Failed;
}

void queue_conversation_reload(AiEditorMainPanelState& state) {
    RpcTask reload;
    {
        std::lock_guard lock(state.mutex);
        reload.kind = RpcTaskKind::LoadConversation;
        reload.generation = state.conversation_generation;
        reload.conversation_id = state.conversation_id;
        reload.canonical_reload = true;
    }
    std::string error;
    if (!reload.conversation_id.empty() && !queue_rpc_task(state, std::move(reload), &error))
        append_output_line(state, "[warn] canonical conversation reload not queued: " + error);
}

void queue_terminal_conversation_reload(AiEditorMainPanelState& state, std::string run_id) {
    if (run_id.empty())
        return;
    {
        std::lock_guard lock(state.mutex);
        if (state.terminal_reload_run_id == run_id || state.conversation_id.empty())
            return;
        state.terminal_reload_run_id = run_id;
    }
    RpcTask reload;
    {
        std::lock_guard lock(state.mutex);
        reload.kind = RpcTaskKind::LoadConversation;
        reload.generation = state.conversation_generation;
        reload.conversation_id = state.conversation_id;
        reload.run_id = run_id;
        reload.canonical_reload = true;
    }
    std::string error;
    if (queue_rpc_task(state, std::move(reload), &error))
        return;
    {
        std::lock_guard lock(state.mutex);
        if (state.terminal_reload_run_id == run_id)
            state.terminal_reload_run_id.clear();
    }
    append_output_line(state, "[warn] canonical conversation reload not queued: " + error);
}

const json* event_params(const json& envelope) {
    if (!envelope.is_object() || envelope.value("method", std::string{}) != "sao.event")
        return nullptr;
    const auto params = envelope.find("params");
    return params != envelope.end() && params->is_object() ? &*params : nullptr;
}

std::string event_run_id(const json& params) {
    std::string run_id = params.value("runId", std::string{});
    if (!run_id.empty())
        return run_id;
    const json payload = params.value("payload", json::object());
    return payload.is_object() ? payload.value("runId", std::string{}) : std::string{};
}

std::string event_tool_progress(const json& payload) {
    if (!payload.is_object())
        return compact_text(payload.dump(), 1200U);
    for (const char* key : {"toolCalls", "tool_calls", "tool_calls_final", "partial"}) {
        const auto found = payload.find(key);
        if (found != payload.end() && !found->empty())
            return compact_text(found->dump(), 4000U);
    }
    const std::string type = payload.value("type", std::string{});
    if (type != "tool_delta" && type != "tool_call" && type != "tool_calls_final" &&
        !payload.contains("tool") && !payload.contains("name")) {
        return {};
    }
    std::string progress = payload.value("name", payload.value("tool", type));
    const std::string arguments =
        payload.value("arguments", payload.value("content", std::string{}));
    if (!arguments.empty()) {
        if (!progress.empty())
            progress.append(": ");
        progress.append(arguments);
    }
    return progress.empty() ? compact_text(payload.dump(), 4000U)
                            : compact_text(std::move(progress), 4000U);
}

bool apply_run_event(AiEditorMainPanelState& state, const json& envelope,
                     std::string& terminal_run_id) {
    const json* params = event_params(envelope);
    if (params == nullptr)
        return false;
    const std::string run_id = event_run_id(*params);
    if (run_id.empty() || run_id != state.active_run_id)
        return false;
    const std::string event_name = params->value("event", std::string{});
    const json payload = params->value("payload", json::object());
    const bool terminal_event = event_name == "run.completed" ||
                                event_name == "run.cancelled" ||
                                event_name == "run.failed" || event_name == "run.stale";
    if (!run_is_active(state.run_phase))
        return false;

    if (event_name == "run.started") {
        state.run_phase = RunPhase::Running;
        state.run_status = "running";
        return true;
    }
    if (event_name == "chat.delta") {
        if (!payload.is_object())
            return false;
        const std::string delta_type = payload.value("type", std::string{});
        const bool aggregate_snapshot = delta_type == "message_delta";
        const auto merge_string = [&](const char* key, std::string& destination,
                                      size_t maximum) -> bool {
            const auto found = payload.find(key);
            if (found == payload.end() || !found->is_string() || found->empty())
                return false;
            if (aggregate_snapshot) {
                destination = clamp_utf8_bytes(found->get<std::string>(), maximum);
            } else {
                append_bounded_text(destination, found->get_ref<const std::string&>(), maximum);
            }
            return true;
        };
        (void)merge_string("content", state.streamed_assistant_text,
                           kMaximumStreamedAssistantBytes);
        if (!merge_string("thinking", state.thinking_status, kMaximumRunDetailBytes))
            (void)merge_string("reasoning", state.thinking_status, kMaximumRunDetailBytes);
        (void)merge_string("refusal", state.refusal_status, kMaximumRunDetailBytes);
        if (payload.contains("usage") && payload["usage"].is_object()) {
            state.usage_status = metrics_summary(payload["usage"]);
            if (state.usage_status.empty())
                state.usage_status = compact_text(payload["usage"].dump(), 1200U);
        }
        const std::string tool_progress = event_tool_progress(payload);
        if (!tool_progress.empty()) {
            if (aggregate_snapshot || delta_type == "tool_calls_final") {
                state.tool_status = clamp_utf8_bytes(tool_progress, kMaximumToolStatusBytes);
            } else {
                if (!state.tool_status.empty())
                    append_bounded_text(state.tool_status, "\n", kMaximumToolStatusBytes);
                append_bounded_text(state.tool_status, tool_progress, kMaximumToolStatusBytes);
            }
        }
        state.run_phase = state.run_phase == RunPhase::Cancelling ? RunPhase::Cancelling
                                                                  : RunPhase::Running;
        state.run_status = state.run_phase == RunPhase::Cancelling ? "cancelling" : "running";
        return true;
    }
    if (event_name == "chat.metrics") {
        state.usage_status = metrics_summary(payload);
        if (state.usage_status.empty())
            state.usage_status = compact_text(payload.dump(), 1200U);
        return true;
    }
    if (!terminal_event)
        return false;

    state.run_poll_pending = false;
    state.event_drain_pending = false;
    if (event_name == "run.completed") {
        state.run_phase = RunPhase::Completed;
        state.run_status = "completed";
        if (payload.is_object()) {
            const std::string content = payload.value("content", std::string{});
            if (!content.empty())
                state.streamed_assistant_text =
                    clamp_utf8_bytes(content, kMaximumStreamedAssistantBytes);
            if (payload.contains("thinking"))
                state.thinking_status = clamp_utf8_bytes(
                    message_content({{"content", payload["thinking"]}}),
                    kMaximumRunDetailBytes);
            else if (payload.contains("reasoning"))
                state.thinking_status = clamp_utf8_bytes(
                    message_content({{"content", payload["reasoning"]}}),
                    kMaximumRunDetailBytes);
            if (payload.contains("refusal"))
                state.refusal_status = clamp_utf8_bytes(
                    message_content({{"content", payload["refusal"]}}),
                    kMaximumRunDetailBytes);
            if (payload.contains("metrics"))
                state.usage_status = metrics_summary(payload["metrics"]);
            const std::string tool_progress = event_tool_progress(payload);
            if (!tool_progress.empty())
                state.tool_status = clamp_utf8_bytes(tool_progress, kMaximumToolStatusBytes);
        }
        state.run_error.clear();
    } else if (event_name == "run.cancelled") {
        state.run_phase = RunPhase::Cancelled;
        state.run_status = "cancelled";
        state.run_error.clear();
    } else if (event_name == "run.stale") {
        state.run_phase = RunPhase::Stale;
        state.run_status = "stale";
        state.run_error = payload.is_object()
                              ? compact_text(payload.value("reason", std::string{}), 1200U)
                              : std::string{};
    } else {
        state.run_phase = RunPhase::Failed;
        state.run_status = "failed";
        if (payload.is_object()) {
            state.run_error = compact_text(
                payload.value("message", payload.value("error", std::string{})), 1200U);
            if (state.run_error.empty() && payload.contains("details"))
                state.run_error = compact_text(payload["details"].dump(), 1200U);
        } else {
            state.run_error = compact_text(payload.dump(), 1200U);
        }
    }
    terminal_run_id = run_id;
    return true;
}

void apply_completion(AiEditorMainPanelState& state, RpcCompletion completion) {
    if (completion.task.history_operation) {
        std::lock_guard lock(state.mutex);
        state.history_task_pending = false;
    }
    bool reconcile_cancelled_start = false;
    if (completion.cancelled) {
        std::lock_guard lock(state.mutex);
        reconcile_cancelled_start =
            (completion.task.kind == RpcTaskKind::SendMessage ||
             completion.task.kind == RpcTaskKind::NewConversation) &&
            completion.task.generation == state.starting_cancelled_generation &&
            state.run_phase == RunPhase::Cancelled;
        if (!reconcile_cancelled_start)
            return;
        state.starting_cancelled_generation = 0;
        if (!completion.task.conversation_id.empty()) {
            state.conversation_id = completion.task.conversation_id;
            state.conversation_title = compact_text(completion.task.title, 500U);
        }
        state.pending_user_text.clear();
        state.active_run_id.clear();
        state.event_drain_pending = false;
        state.run_poll_pending = false;
        state.run_phase = RunPhase::Cancelled;
        state.run_status = "cancelled before start completed";
    }
    if (completion.cancelled) {
        if (reconcile_cancelled_start && !completion.task.conversation_id.empty())
            queue_conversation_reload(state);
        return;
    }
    if (completion.steps.empty())
        return;
    const bool generation_sensitive =
        completion.task.kind == RpcTaskKind::NewConversation ||
        completion.task.kind == RpcTaskKind::LoadConversation ||
        completion.task.kind == RpcTaskKind::DuplicateConversation ||
        completion.task.kind == RpcTaskKind::SendMessage ||
        completion.task.kind == RpcTaskKind::PollRun ||
        (completion.task.kind == RpcTaskKind::CancelRun && !completion.task.orphan_cleanup);
    {
        std::lock_guard lock(state.mutex);
        if (generation_sensitive && completion.task.generation != state.conversation_generation)
            return;
    }

    const RpcStepResult& last = completion.steps.back();
    {
        std::lock_guard lock(state.mutex);
        update_backend_status(state, last.method, last.response);
    }

    switch (completion.task.kind) {
    case RpcTaskKind::Bootstrap: {
        const RpcResponse& initialized = completion.steps.front().response;
        if (!initialized.ok) {
            record_completion_output(state, completion.steps.front());
            std::lock_guard lock(state.mutex);
            state.bootstrap_pending = false;
            state.next_bootstrap_attempt =
                std::chrono::steady_clock::now() + kBootstrapRetryInterval;
            return;
        }
        bool metadata_complete = true;
        for (size_t index = 1; index < completion.steps.size(); ++index) {
            if (!completion.steps[index].response.ok) {
                metadata_complete = false;
                record_completion_output(state, completion.steps[index]);
            }
        }
        std::lock_guard lock(state.mutex);
        const json config = initialized.result.value("config", json::object());
        const std::string configured_provider = summary_value(
            config, {"ai_editor.provider", "ai.provider", "provider"}, std::string{});
        const std::string configured_model = summary_value(
            config, {"ai_editor.model", "ai.model", "model"}, std::string{});
        const std::string configured_mode = summary_value(
            config, {"ai_editor.mode", "ai.mode", "mode"}, "agent");
        const std::string configured_approval = normalize_approval(summary_value(
            config, {"ai_editor.approval", "ai.approval", "approval"}, "default"));
        if (!configured_provider.empty())
            add_choice(state.providers, {configured_provider, configured_provider, {}});
        if (!configured_model.empty())
            add_choice(state.models, {configured_model, configured_model, {}});
        for (size_t index = 1; index < completion.steps.size(); ++index) {
            const auto& step = completion.steps[index];
            if (!step.response.ok)
                continue;
            if (step.method == "providers.list")
                apply_choice_items(state.providers,
                                   step.response.result.value("items", json::array()), "Provider");
            else if (step.method == "agents.list_defs")
                apply_choice_items(state.agents,
                                   step.response.result.value("items", json::array()), "Agent");
            else if (step.method == "workflows.list_defs")
                apply_choice_items(state.workflows,
                                   step.response.result.value("items", json::array()), "Workflow");
            else if (step.method == "conversation.list" && state.history_query.empty()) {
                state.history = parse_history(step.response.result, state.history_limit);
                state.history_total = history_total_from_steps(completion.steps, state.history.size());
            }
        }
        if (!state.selections_initialized) {
            state.selected_provider = configured_provider;
            state.selected_model = configured_model;
            if (configured_mode == "ask" || configured_mode == "plan" ||
                configured_mode == "agent") {
                state.selected_mode = configured_mode;
            }
            state.selected_approval = configured_approval;
            state.selections_initialized = true;
        }
        state.bootstrap_loaded = true;
        state.bootstrap_pending = false;
        state.backend_connected = true;
        state.backend_status = metadata_complete
                       ? "Connected · chat metadata and history loaded"
                       : "Connected · some chat metadata requests failed; see output";
        break;
    }
    case RpcTaskKind::Generic:
        if (completion.task.control_action)
            apply_control_completion(state, completion);
        else
            record_completion_output(state, last, true);
        break;
    case RpcTaskKind::NewConversation:
        if (!last.response.ok || !last.response.result.is_object() ||
            last.response.result.value("id", std::string{}).empty()) {
            record_completion_output(state, last);
            std::lock_guard lock(state.mutex);
            state.run_phase = RunPhase::Failed;
            state.run_status = "create error";
            state.run_error = last.response.ok
                                  ? "conversation.create returned no conversation id"
                                  : compact_text(last.response.error, 1200U);
            break;
        }
        {
            std::lock_guard lock(state.mutex);
            apply_conversation_document(state, last.response.result);
            state.run_phase = RunPhase::Idle;
            state.run_status = "idle";
            state.run_error.clear();
            state.streamed_assistant_text.clear();
            state.refusal_status.clear();
            state.pending_user_text.clear();
        }
        append_output_line(state, "[" + format_iso_timestamp() + "] created conversation " +
                                      last.response.result.value("id", std::string{}));
        break;
    case RpcTaskKind::RefreshHistory:
        if (!last.response.ok) {
            record_completion_output(state, last);
            std::lock_guard lock(state.mutex);
            state.run_error = "History refresh failed: " +
                              compact_text(last.response.error, 1000U);
        } else {
            std::lock_guard lock(state.mutex);
            state.history = parse_history(last.response.result, state.history_limit);
            state.history_query.clear();
            state.history_total = history_total_from_steps(completion.steps, state.history.size());
            state.run_error.clear();
        }
        break;
    case RpcTaskKind::SearchHistory:
        if (!last.response.ok || !last.response.result.is_object()) {
            record_completion_output(state, last);
            std::lock_guard lock(state.mutex);
            state.run_error = "History search failed: " +
                              (last.response.ok
                                   ? std::string{"conversation.search returned invalid results"}
                                   : compact_text(last.response.error, 1000U));
            break;
        }
        {
            std::lock_guard lock(state.mutex);
            state.history_query =
                clamp_utf8_bytes(last.response.result.value("query", completion.task.query),
                                 kMaximumHistoryQueryBytes);
            state.history = parse_history_search_results(last.response.result, state.history_limit);
            state.history_total = last.response.result.value("total", state.history.size());
            state.run_error.clear();
        }
        break;
    case RpcTaskKind::LoadConversation:
        if (!last.response.ok || !last.response.result.is_object() ||
            last.response.result.value("id", std::string{}).empty()) {
            record_completion_output(state, last);
            std::lock_guard lock(state.mutex);
            const std::string detail = last.response.ok
                                           ? "conversation.get returned no conversation id"
                                           : compact_text(last.response.error, 1100U);
            if (completion.task.canonical_reload) {
                state.run_error = "Canonical conversation reload failed: " + detail;
            } else {
                state.run_status = "load error";
                state.run_error = detail;
            }
            break;
        }
        {
            std::lock_guard lock(state.mutex);
            apply_conversation_document(state, last.response.result);
            state.pending_user_text.clear();
            state.streamed_assistant_text.clear();
            if (state.run_status == "loading history") {
                state.run_status = "idle";
                state.run_phase = RunPhase::Idle;
            }
        }
        break;
    case RpcTaskKind::DeleteConversation: {
        for (const auto& step : completion.steps)
            if (!step.response.ok)
                record_completion_output(state, step);
        const auto delete_step = std::find_if(
            completion.steps.begin(), completion.steps.end(),
            [](const auto& step) { return step.method == "conversation.delete"; });
        const bool deleted = delete_step != completion.steps.end() && delete_step->response.ok;
        const std::string delete_error = delete_step == completion.steps.end()
                                             ? "missing conversation.delete response"
                                             : compact_text(delete_step->response.error, 1100U);
        std::lock_guard lock(state.mutex);
        if (!deleted) {
            state.run_error = "Conversation delete failed: " + delete_error;
            break;
        }
        state.undo_entry = completion.task.restore_entry;
        state.undo_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        if (state.conversation_id == completion.task.conversation_id) {
            state.conversation_id.clear();
            state.conversation_title = "New Chat";
            state.conversation_messages = json::array();
            state.conversation_messages_total = 0;
            state.pending_user_text.clear();
            state.streamed_assistant_text.clear();
        }
        if (completion.steps.size() > 1 && completion.steps.back().response.ok) {
            if (completion.steps.back().method == "conversation.search") {
                state.history =
                    parse_history_search_results(completion.steps.back().response.result, state.history_limit);
                state.history_total =
                    completion.steps.back().response.result.value("total", state.history.size());
            } else {
                state.history = parse_history(completion.steps.back().response.result, state.history_limit);
                state.history_total = history_total_from_steps(completion.steps, state.history.size());
            }
            state.run_error.clear();
        } else {
            const size_t removed = std::erase_if(state.history, [&](const HistoryEntry& entry) {
                return entry.id == completion.task.conversation_id;
            });
            if (removed != 0 && state.history_total != 0)
                --state.history_total;
            const std::string detail = completion.steps.size() > 1
                                           ? compact_text(completion.steps.back().response.error,
                                                          900U)
                                           : "history refresh was not reached";
            state.run_error = "Conversation deleted, but history refresh failed: " + detail;
        }
        break;
    }
    case RpcTaskKind::DuplicateConversation: {
        const auto duplicate =
            std::ranges::find_if(completion.steps, [](const RpcStepResult& step) {
                return step.method == "conversation.duplicate";
            });
        if (duplicate == completion.steps.end() || !duplicate->response.ok ||
            !duplicate->response.result.is_object() ||
            duplicate->response.result.value("id", std::string{}).empty()) {
            if (duplicate != completion.steps.end() && !duplicate->response.ok)
                record_completion_output(state, *duplicate);
            std::lock_guard lock(state.mutex);
            state.run_status = "duplicate error";
            state.run_error =
                duplicate == completion.steps.end() ? "conversation.duplicate was not reached"
                : duplicate->response.ok ? "conversation.duplicate returned no conversation id"
                                         : compact_text(duplicate->response.error, 1200U);
            break;
        }
        for (const auto& step : completion.steps)
            if (!step.response.ok)
                record_completion_output(state, step);
        {
            std::lock_guard lock(state.mutex);
            apply_conversation_document(state, duplicate->response.result);
            state.pending_user_text.clear();
            state.streamed_assistant_text.clear();
            state.thinking_status.clear();
            state.refusal_status.clear();
            state.tool_status.clear();
            state.usage_status.clear();
            state.active_run_id.clear();
            state.event_drain_pending = false;
            state.run_poll_pending = false;
            state.terminal_reload_run_id.clear();
            state.run_phase = RunPhase::Idle;
            state.run_status = "idle";
            if (completion.steps.size() > 1 && completion.steps.back().response.ok) {
                if (completion.steps.back().method == "conversation.search") {
                    state.history =
                        parse_history_search_results(completion.steps.back().response.result, state.history_limit);
                    state.history_total = completion.steps.back().response.result.value(
                        "total", state.history.size());
                } else {
                    state.history = parse_history(completion.steps.back().response.result, state.history_limit);
                    state.history_total = history_total_from_steps(completion.steps, state.history.size());
                }
                state.run_error.clear();
            } else {
                if (state.history_query.empty()) {
                    HistoryEntry entry;
                    entry.id = duplicate->response.result.value("id", std::string{});
                    entry.title = compact_text(
                        duplicate->response.result.value("title", std::string{"Untitled"}),
                        500U);
                    entry.model = compact_text(
                        duplicate->response.result.value("model", std::string{}), 256U);
                    entry.scope = compact_text(
                        duplicate->response.result.value("scope", std::string{"workspace"}),
                        64U);
                    entry.saved_at =
                        duplicate->response.result.value("savedAt", int64_t{0});
                    entry.message_count =
                        duplicate->response.result.value("messageCount", size_t{0});
                    if (std::ranges::find(state.history, entry.id, &HistoryEntry::id) ==
                        state.history.end()) {
                        state.history.insert(state.history.begin(), std::move(entry));
                        if (state.history.size() > kMaximumHistoryEntries)
                            state.history.resize(kMaximumHistoryEntries);
                        if (state.history_total < std::numeric_limits<size_t>::max())
                            ++state.history_total;
                    }
                }
                const std::string detail = completion.steps.size() > 1
                                               ? compact_text(
                                                     completion.steps.back().response.error, 900U)
                                               : "history refresh was not reached";
                state.run_error = "Conversation duplicated, but history refresh failed: " +
                                  detail + ". Use Refresh History to retry.";
            }
        }
        append_output_line(state, "[" + format_iso_timestamp() + "] duplicated conversation " +
                                      completion.task.conversation_id + " -> " +
                                      duplicate->response.result.value("id", std::string{}));
        break;
    }
    case RpcTaskKind::RestoreConversation: {
        const auto created = std::ranges::find_if(completion.steps, [](const RpcStepResult& step) {
            return step.method == "conversation.create";
        });
        if (created == completion.steps.end() || !created->response.ok ||
            !created->response.result.is_object() ||
            created->response.result.value("id", std::string{}).empty()) {
            std::lock_guard lock(state.mutex);
            state.run_error = "Undo restore failed: conversation.create returned no usable id.";
            break;
        }
        const auto failed_append =
            std::ranges::find_if(completion.steps, [](const RpcStepResult& step) {
                return step.method == "conversation.append" && !step.response.ok;
            });
        if (failed_append != completion.steps.end()) {
            for (const auto& step : completion.steps) {
                if (!step.response.ok)
                    record_completion_output(state, step);
            }
            std::lock_guard lock(state.mutex);
            state.run_error = "Undo restore failed while appending messages: " +
                              compact_text(failed_append->response.error, 1000U);
            break;
        }
        HistoryEntry restored = completion.task.restore_entry;
        restored.id = created->response.result.value("id", std::string{});
        if (restored.content_available)
            restored.message_count = restored.messages.is_array() ? restored.messages.size() : 0U;
        restored.saved_at = created->response.result.value("savedAt", int64_t{0});
        {
            std::lock_guard lock(state.mutex);
            const auto found = std::ranges::find(
                state.history, completion.task.restore_entry.id, &HistoryEntry::id);
            if (found != state.history.end())
                *found = restored;
            else
                state.history.insert(state.history.begin(), restored);
            state.history_total =
                history_total_from_steps(completion.steps, state.history.size());
            state.conversation_id = restored.id;
            state.conversation_title = restored.title;
            state.conversation_messages = restored.messages;
            state.conversation_messages_total = restored.content_available
                                                   ? restored.messages.size()
                                                   : restored.message_count;
            state.run_error.clear();
        }
        break;
    }
    case RpcTaskKind::SendMessage: {
        const auto chat_step = std::ranges::find_if(
            completion.steps, [](const RpcStepResult& step) { return step.method == "chat.run"; });
        if (chat_step == completion.steps.end() || !chat_step->response.ok) {
            for (const auto& step : completion.steps)
                if (!step.response.ok)
                    record_completion_output(state, step);
            const bool append_succeeded = std::ranges::any_of(
                completion.steps, [](const RpcStepResult& step) {
                    return step.method == "conversation.append" && step.response.ok;
                });
            const bool conversation_available = !completion.task.conversation_id.empty();
            {
                std::lock_guard lock(state.mutex);
                state.run_phase = RunPhase::Failed;
                state.run_status = "failed";
                state.run_error = chat_step == completion.steps.end()
                                      ? "chat.run was not reached"
                                      : compact_text(chat_step->response.error, 1200U);
                if (conversation_available) {
                    state.conversation_id = completion.task.conversation_id;
                    state.conversation_title = compact_text(completion.task.title, 500U);
                }
                if (!append_succeeded) {
                    state.composer_text = completion.task.prompt;
                }
                state.pending_user_text.clear();
            }
            if (append_succeeded)
                queue_conversation_reload(state);
            break;
        }
        const std::string run_id = chat_step->response.result.value("runId", std::string{});
        if (!chat_step->response.result.value("accepted", false) || run_id.empty()) {
            {
                std::lock_guard lock(state.mutex);
                state.run_phase = RunPhase::Failed;
                state.run_status = "failed";
                state.run_error = "chat.run did not accept the request or return a run id";
                state.conversation_id = completion.task.conversation_id;
                state.conversation_title = compact_text(completion.task.title, 500U);
                state.pending_user_text.clear();
            }
            queue_conversation_reload(state);
            break;
        }
        {
            std::lock_guard lock(state.mutex);
            state.conversation_id = completion.task.conversation_id;
            state.conversation_title = compact_text(completion.task.title, 500U);
            state.active_run_id = run_id;
            state.run_status = chat_step->response.result.value("status", std::string{"running"});
            state.run_phase = RunPhase::Running;
            state.event_drain_pending = false;
            state.event_drain_unavailable = false;
            state.next_event_drain = std::chrono::steady_clock::now();
            state.run_poll_pending = false;
            state.next_run_poll = std::chrono::steady_clock::now();
            state.terminal_reload_run_id.clear();
        }
        append_output_line(state, "[" + format_iso_timestamp() + "] chat.run accepted: " +
                                      chat_step->response.result.value("runId", std::string{}));
        break;
    }
    case RpcTaskKind::DrainEvents: {
        if (!last.response.ok) {
            bool unavailable = false;
            {
                std::lock_guard lock(state.mutex);
                state.event_drain_pending = false;
                state.next_event_drain = std::chrono::steady_clock::now() + kEventDrainInterval;
                unavailable = last.response.status == SAO_AI_EDITOR_ERR_NOT_FOUND ||
                              last.response.error.find("not found") != std::string::npos ||
                              last.response.error.find("unknown method") != std::string::npos;
                state.event_drain_unavailable = unavailable;
                if (unavailable) {
                    state.backend_connected = true;
                    state.backend_status =
                        "Connected · events.drain unavailable; using run.status fallback";
                }
            }
            if (!unavailable)
                record_completion_output(state, last);
            break;
        }

        json events = json::array();
        if (last.response.result.is_object())
            events = last.response.result.value("events", json::array());
        else if (last.response.result.is_array())
            events = last.response.result;
        std::string terminal_run_id;
        {
            std::lock_guard lock(state.mutex);
            state.event_drain_pending = false;
            state.event_drain_unavailable = false;
            state.next_event_drain = std::chrono::steady_clock::now() + kEventDrainInterval;
            if (events.is_array()) {
                const size_t count = std::min(events.size(), kMaximumEventsPerDrain);
                for (size_t index = 0; index < count; ++index) {
                    json envelope = events[index];
                    if (envelope.is_string()) {
                        envelope = json::parse(envelope.get_ref<const std::string&>(), nullptr,
                                               false, false);
                    }
                    if (envelope.is_discarded())
                        continue;
                    (void)apply_run_event(state, envelope, terminal_run_id);
                }
            }
        }
        if (!terminal_run_id.empty())
            queue_terminal_conversation_reload(state, std::move(terminal_run_id));
        break;
    }
    case RpcTaskKind::PollRun: {
        if (!last.response.ok) {
            record_completion_output(state, last);
            std::lock_guard lock(state.mutex);
            if (state.active_run_id == completion.task.run_id) {
                state.run_poll_pending = false;
                state.next_run_poll = std::chrono::steady_clock::now() + kRunPollInterval;
            }
            break;
        }
        bool terminal = false;
        {
            std::lock_guard lock(state.mutex);
            const std::string result_run_id =
                last.response.result.value("runId", completion.task.run_id);
            if (completion.task.run_id.empty() || result_run_id != completion.task.run_id ||
                state.active_run_id != completion.task.run_id ||
                !run_is_active(state.run_phase)) {
                return;
            }
            state.run_poll_pending = false;
            const std::string status = last.response.result.value("status", std::string{});
            if (status == "running" || status == "pending") {
                state.run_status = status;
                state.run_phase = state.run_phase == RunPhase::Cancelling
                                      ? RunPhase::Cancelling
                                      : RunPhase::Running;
                state.next_run_poll = std::chrono::steady_clock::now() + kRunPollInterval;
                break;
            }
            finish_run_from_status(state, last.response.result);
            terminal = true;
        }
        if (terminal)
            queue_terminal_conversation_reload(state, completion.task.run_id);
        break;
    }
    case RpcTaskKind::CancelRun:
        if (completion.task.orphan_cleanup) {
            if (!last.response.ok)
                record_completion_output(state, last);
            break;
        }
        if (!last.response.ok) {
            record_completion_output(state, last);
            std::lock_guard lock(state.mutex);
            if (run_is_active(state.run_phase)) {
                state.run_phase = RunPhase::Running;
                state.run_status = "running";
                state.run_error = "Cancel request failed: " +
                                  compact_text(last.response.error, 1100U);
                state.run_poll_pending = false;
                state.next_run_poll = std::chrono::steady_clock::now() + kRunPollInterval;
            }
        } else {
            std::lock_guard lock(state.mutex);
            if (run_is_active(state.run_phase)) {
                state.run_phase = RunPhase::Cancelling;
                state.run_status = "cancelling";
                state.next_run_poll = std::chrono::steady_clock::now();
            }
        }
        break;
    case RpcTaskKind::RefreshModels:
        if (!last.response.ok) {
            record_completion_output(state, last);
            break;
        }
        {
            std::lock_guard lock(state.mutex);
            std::vector<ChoiceItem> models{{"", "Auto (Settings)", ""}};
            const json items = last.response.result.value("models", json::array());
            if (items.is_array()) {
                for (const auto& item : items) {
                    if (item.is_string())
                        add_choice(models, {item.get<std::string>(), item.get<std::string>(), {}});
                    else if (item.is_object())
                        add_choice(models,
                                   {item.value("id", item.value("name", std::string{})),
                                    item.value("name", item.value("id", std::string{})), {}});
                }
            }
            if (!state.selected_model.empty())
                add_choice(models, {state.selected_model, state.selected_model, {}});
            state.models = std::move(models);
        }
        break;
    }
}

void drain_completions(AiEditorMainPanelState& state) {
    std::deque<RpcCompletion> completions;
    {
        std::lock_guard lock(state.mutex);
        completions.swap(state.completed_queue);
    }
    while (!completions.empty()) {
        apply_completion(state, std::move(completions.front()));
        completions.pop_front();
    }
}

const std::vector<ControlActionSpec>& control_action_catalog() {
    static const std::vector<ControlActionSpec> actions{
        {"Conversations", "conversation.list", "List", "conversation.list", R"({"scope":"all","limit":50})", true, false, false},
        {"Conversations", "conversation.get", "Get", "conversation.get", R"({"id":""})", true, false, false},
        {"Conversations", "conversation.search", "Search", "conversation.search", R"({"query":"","scope":"all","limit":50})", true, false, false},
        {"Conversations", "conversation.pin", "Pin", "conversation.pin", R"({"id":""})", false, false, false},
        {"Conversations", "conversation.unpin", "Unpin", "conversation.unpin", R"({"id":""})", false, false, false},
        {"Conversations", "conversation.tag", "Tag", "conversation.tag", R"({"id":"","tags":[]})", false, false, false},
        {"Conversations", "conversation.untag", "Untag", "conversation.untag", R"({"id":"","tags":[]})", false, false, false},
        {"Conversations", "conversation.find_by_tag", "Find by tag", "conversation.find_by_tag", R"({"tags":[],"scope":"all","limit":50,"matchAll":false})", true, false, false},
        {"Conversations", "conversation.list_tags", "List tags", "conversation.list_tags", R"({"scope":"all"})", true, false, false},
        {"Conversations", "conversation.branch", "Branch", "conversation.branch", R"({"sourceId":"","messageIndex":0,"title":"","scope":"workspace"})", false, true, false},
        {"Conversations", "conversation.merge", "Merge", "conversation.merge", R"({"sourceIds":[],"title":"","scope":"workspace","orderBy":"explicit"})", false, true, false},
        {"Conversations", "conversation.split", "Split", "conversation.split", R"({"sourceId":"","messageIndex":0,"scope":"workspace","keepOriginal":false})", false, true, false},
        {"Conversations", "conversation.compact", "Compact", "conversation.compact", R"({"id":"","keepLast":6,"compactStrategy":"replace"})", false, true, false},
        {"Conversations", "conversation.export", "Export", "conversation.export", R"({"scope":"all"})", true, false, false},
        {"Conversations", "conversation.import", "Import", "conversation.import", R"({"payload":{},"scope":"workspace","overwrite":false})", false, true, false},
        {"Conversations", "conversation.delete", "Delete", "conversation.delete", R"({"id":""})", false, true, false},
        {"Workflows", "workflows.reload", "Reload", "workflows.reload", R"({})", true, false, false},
        {"Workflows", "workflows.list_defs", "List", "workflows.list_defs", R"({"scope":"all"})", true, false, false},
        {"Workflows", "workflows.get_def", "Get", "workflows.get_def", R"({"id":"","scope":"all"})", true, false, false},
        {"Workflows", "workflows.save_def", "Save", "workflows.save_def", R"({"scope":"workspace","workflow":{}})", false, true, false},
        {"Workflows", "workflows.delete_def", "Delete", "workflows.delete_def", R"({"id":"","scope":"workspace"})", false, true, false},
        {"Workflows", "workflow.dry_run", "Dry run", "workflow.dry_run", R"({"id":"","input":{}})", true, false, false},
        {"Workflows", "workflows.run", "Run", "workflows.run", R"({"id":"","input":{}})", false, true, false},
        {"Workflows", "workflows.status", "Status", "workflows.status", R"({"executionId":""})", true, false, false},
        {"Workflows", "workflows.pause", "Pause", "workflows.pause", R"({"executionId":""})", false, true, false},
        {"Workflows", "workflows.resume", "Resume", "workflows.resume", R"({"executionId":""})", false, true, false},
        {"Workflows", "workflows.cancel", "Cancel", "workflows.cancel", R"({"executionId":""})", false, true, false},
        {"Workflows", "workflow.retry", "Retry", "workflow.retry", R"({"id":"","scope":"all"})", false, true, false},
        {"Workflows", "workflow.list_executions", "Execution list", "workflow.list_executions", R"({"scope":"all","limit":50})", true, false, false},
        {"Workflows", "workflow.get_execution", "Execution get", "workflow.get_execution", R"({"id":"","scope":"all"})", true, false, false},
        {"Workflows", "workflow.delete_execution", "Execution delete", "workflow.delete_execution", R"({"id":"","scope":"all"})", false, true, false},
        {"Workflows", "workflow.skip_step", "Skip step", "workflow.skip_step", R"({"executionId":"","reason":""})", false, true, false},
        {"Workflows", "workflow.replace_variable", "Replace variable", "workflow.replace_variable", R"({"executionId":"","name":"","value":""})", false, true, false},
        {"Workflows", "workflow.snapshot_variables", "Snapshot variables", "workflow.snapshot_variables", R"({"executionId":""})", true, false, false},
        {"Agents & Prompts", "agents.list_defs", "Agents list", "agents.list_defs", R"({"scope":"all"})", true, false, false},
        {"Agents & Prompts", "agents.get_def", "Agent get", "agents.get_def", R"({"id":"","scope":"all"})", true, false, false},
        {"Agents & Prompts", "agents.save_def", "Agent save", "agents.save_def", R"({"scope":"workspace","agent":{}})", false, true, false},
        {"Agents & Prompts", "agents.delete_def", "Agent delete", "agents.delete_def", R"({"id":"","scope":"workspace"})", false, true, false},
        {"Agents & Prompts", "agents.invoke", "Invoke agent", "agents.invoke", R"({"id":"","message":""})", false, true, false},
        {"Agents & Prompts", "agents.recommend", "Recommend", "agents.recommend", R"({"query":"","topK":3})", true, false, false},
        {"Agents & Prompts", "agents.import", "Import agents", "agents.import", R"({"payload":{},"scope":"workspace","overwrite":false})", false, true, false},
        {"Agents & Prompts", "agents.export", "Export agents", "agents.export", R"({"scope":"all"})", true, false, false},
        {"Agents & Prompts", "agents.invoke_with_mcp", "Invoke with MCP", "agents.invoke_with_mcp", R"({"id":"","message":"","mcpServers":[],"extraTools":[]})", false, true, false},
        {"Agents & Prompts", "agents.batch_invoke", "Batch invoke (Developer Advanced)", "agents.batch_invoke", R"({"agents":[]})", false, true, true},
        {"Agents & Prompts", "prompts.list_defs", "Prompts list", "prompts.list_defs", R"({"scope":"all"})", true, false, false},
        {"Agents & Prompts", "prompts.get_def", "Prompt get", "prompts.get_def", R"({"id":"","scope":"all"})", true, false, false},
        {"Agents & Prompts", "prompts.save_def", "Prompt save", "prompts.save_def", R"({"scope":"workspace","prompt":{}})", false, true, false},
        {"Agents & Prompts", "prompts.delete_def", "Prompt delete", "prompts.delete_def", R"({"id":"","scope":"workspace"})", false, true, false},
        {"Agents & Prompts", "prompts.render", "Render prompt", "prompts.render", R"({"id":"","arguments":{}})", true, false, false},
        {"Agents & Prompts", "prompts.render_batch", "Render batch", "prompts.render_batch", R"({"items":[]})", true, false, false},
        {"Agents & Prompts", "prompts.list_tags", "Prompt tags", "prompts.list_tags", R"({"scope":"all"})", true, false, false},
        {"Agents & Prompts", "prompt.pin", "Pin prompt", "prompt.pin", R"({"id":"","scope":"workspace"})", false, false, false},
        {"Agents & Prompts", "prompt.unpin", "Unpin prompt", "prompt.unpin", R"({"id":"","scope":"workspace"})", false, false, false},
        {"Tools", "tools.list", "List tools", "tools.list", R"({})", true, false, false},
        {"Tools", "tools.call", "Call tool", "tools.call", R"({"mode":"agent","name":"","arguments":{}})", false, true, false},
        {"Tools", "tools.register", "Register tool (Developer Advanced)", "tools.register", R"({"name":"","description":"","parameters":{"type":"object","properties":{}},"readOnly":true})", false, true, true},
        {"Tools", "tools.unregister", "Unregister tool (Developer Advanced)", "tools.unregister", R"({"name":""})", false, true, true},
        {"Tools", "tools.register_alias", "Register alias (Developer Advanced)", "tools.register_alias", R"({"alias":"","target":""})", false, true, true},
        {"Tools", "tools.unregister_alias", "Unregister alias (Developer Advanced)", "tools.unregister_alias", R"({"alias":""})", false, true, true},
        {"Tools", "tools.register_hook", "Register hook (Developer Advanced)", "tools.register_hook", R"({"id":"","phase":"before","emitEvent":""})", false, true, true},
        {"Tools", "tools.unregister_hook", "Unregister hook (Developer Advanced)", "tools.unregister_hook", R"({"id":""})", false, true, true},
        {"Tools", "tools.cache_stats", "Cache stats (Developer Advanced)", "tools.cache_stats", R"({})", true, false, true},
        {"Tools", "tools.cache_clear", "Clear cache (Developer Advanced)", "tools.cache_clear", R"({})", false, true, true},
        {"Tools", "tools.telemetry_stats", "Telemetry stats (Developer Advanced)", "tools.telemetry_stats", R"({})", true, false, true},
        {"Tools", "tools.telemetry_clear", "Clear telemetry (Developer Advanced)", "tools.telemetry_clear", R"({})", false, true, true},
        {"Providers & Usage", "providers.list", "Providers list", "providers.list", R"({"scope":"all"})", true, false, false},
        {"Providers & Usage", "providers.configure", "Configure provider (API key uses Settings)", "providers.configure", R"({"scope":"workspace","provider":{"id":""}})", false, true, false},
        {"Providers & Usage", "models.list", "Models list", "models.list", R"({"providerId":""})", true, false, false},
        {"Providers & Usage", "chat.set_pricing", "Set pricing", "chat.set_pricing", R"({"providerId":"","model":"","pricing":{}})", false, true, false},
        {"Providers & Usage", "chat.get_pricing", "Get pricing", "chat.get_pricing", R"({"providerId":"","model":""})", true, false, false},
        {"Providers & Usage", "chat.list_pricing", "List pricing", "chat.list_pricing", R"({})", true, false, false},
        {"Providers & Usage", "chat.cost_stats", "Cost stats", "chat.cost_stats", R"({})", true, false, false},
        {"Runtime & Extensions", "runtime.initialize", "Runtime status", "runtime.initialize", R"({})", true, false, false},
        {"Runtime & Extensions", "config.load", "Runtime config", "config.load", R"({})", true, false, false},
        {"Runtime & Extensions", "scopes.list", "Scopes", "scopes.list", R"({})", true, false, false},
        {"Runtime & Extensions", "permission.get", "Permission policy", "permission.get", R"({})", true, false, false},
        {"Runtime & Extensions", "extensions.list", "Extensions list", "extensions.list", R"({})", true, false, false},
        {"Runtime & Extensions", "extensions.activate", "Activate extension", "extensions.activate", R"({"extensionId":""})", false, true, false},
        {"Runtime & Extensions", "extensions.deactivate", "Deactivate extension", "extensions.deactivate", R"({"extensionId":""})", false, true, false},
        {"Runtime & Extensions", "extensions.execute_command", "Execute extension command", "extensions.execute_command", R"({"command":"","arguments":[]})", false, true, false},
        {"Runtime & Extensions", "commands.execute", "Execute command", "vscode.commands.executeCommand", R"({"command":"","arguments":[]})", false, true, false},
    };
    return actions;
}

const ControlActionSpec* find_control_action(std::string_view id_or_method) {
    const auto& actions = control_action_catalog();
    const auto found = std::ranges::find_if(actions, [&](const ControlActionSpec& action) {
        return id_or_method == action.id || id_or_method == action.method;
    });
    return found == actions.end() ? nullptr : &*found;
}

const ControlToolItem* find_control_tool(const AiEditorMainPanelState& state,
                                         std::string_view name) {
    const auto found = std::ranges::find(state.control_tools, name, &ControlToolItem::name);
    return found == state.control_tools.end() ? nullptr : &*found;
}

std::string normalized_control_key(std::string_view key) {
    std::string normalized;
    normalized.reserve(key.size());
    for (const unsigned char character : key) {
        if (std::isalnum(character) != 0)
            normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

bool is_sensitive_control_key(std::string_view key) {
    const std::string normalized = normalized_control_key(key);
    return normalized.find("apikey") != std::string::npos ||
           normalized.find("token") != std::string::npos ||
           normalized.find("secret") != std::string::npos ||
           normalized.find("password") != std::string::npos ||
           normalized.find("authorization") != std::string::npos ||
           normalized.find("credential") != std::string::npos;
}

void redact_control_json(json& value) {
    if (value.is_array()) {
        for (auto& item : value)
            redact_control_json(item);
        return;
    }
    if (!value.is_object())
        return;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (is_sensitive_control_key(it.key())) {
            it.value() = "[redacted]";
        } else {
            redact_control_json(it.value());
        }
    }
}

std::string redact_control_text(std::string text) {
    const json parsed = json::parse(text, nullptr, false, false);
    if (!parsed.is_discarded() && (parsed.is_object() || parsed.is_array())) {
        json redacted = parsed;
        redact_control_json(redacted);
        return redacted.dump();
    }

    std::string lowered = text;
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    for (const std::string_view marker : {"apikey", "api_key", "api-key", "token", "secret",
                                          "password", "authorization", "credential"}) {
        size_t search_from = 0;
        while ((search_from = lowered.find(marker, search_from)) != std::string::npos) {
            const size_t colon = lowered.find(':', search_from + marker.size());
            if (colon == std::string::npos)
                break;
            size_t value_begin = colon + 1;
            while (value_begin < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[value_begin])) != 0)
                ++value_begin;
            if (value_begin >= text.size())
                break;
            size_t value_end = value_begin;
            if (text[value_begin] == '"') {
                value_end = value_begin + 1;
                while (value_end < text.size()) {
                    if (text[value_end] == '"' && text[value_end - 1] != '\\') {
                        ++value_end;
                        break;
                    }
                    ++value_end;
                }
            } else {
                while (value_end < text.size() && text[value_end] != ',' &&
                       text[value_end] != '}' && text[value_end] != ']' &&
                       text[value_end] != '\n' && text[value_end] != '\r')
                    ++value_end;
            }
            if (value_end <= value_begin)
                break;
            text.replace(value_begin, value_end - value_begin, "\"[redacted]\"");
            lowered = text;
            std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            search_from = value_begin + 12U;
        }
    }
    return text;
}

std::string control_default_json_text(const json& value) {
    json display = value;
    redact_control_json(display);
    return clamp_utf8_bytes(display.dump(), kMaximumActionPayloadBytes);
}

void clear_pending_control_locked(AiEditorMainPanelState& state) {
    state.pending_control_method.clear();
    state.pending_control_label.clear();
    state.pending_control_group.clear();
    state.pending_control_read_only = false;
    state.pending_control_destructive = false;
    state.pending_control_advanced = false;
    state.pending_control_params = json::object();
    state.pending_control_execution_params = json::object();
    state.pending_control_tool_name.clear();
    state.pending_control_tool_arguments = json::object();
    state.pending_control_permission.clear();
    state.pending_control_permission_source.clear();
    state.pending_control_permission_category.clear();
    state.pending_control_confirmation_required = false;
    if (state.pending_dialog == DialogIntent::ControlInput ||
        state.pending_dialog == DialogIntent::ControlConfirm)
        state.pending_dialog = DialogIntent::None;
}

bool control_requires_confirmation(std::string_view permission, std::string_view category,
                                   bool confirmation_required, bool read_only,
                                   bool destructive, bool advanced) {
    std::string normalized_permission(permission);
    std::string normalized_category(category);
    std::ranges::transform(normalized_permission, normalized_permission.begin(),
                           [](unsigned char character) {
                               return static_cast<char>(std::tolower(character));
                           });
    std::ranges::transform(normalized_category, normalized_category.begin(),
                           [](unsigned char character) {
                               return static_cast<char>(std::tolower(character));
                           });
    return confirmation_required || destructive || advanced || !read_only ||
           normalized_permission == "write" || normalized_permission == "execute" ||
           normalized_category == "write" || normalized_category == "execute";
}

json control_minimal_schema_value(const json& schema) {
    if (!schema.is_object())
        return json::object();
    if (schema.contains("default"))
        return schema["default"];
    const std::string type = schema.value("type", std::string{"string"});
    if (type == "boolean")
        return false;
    if (type == "integer" || type == "number")
        return 0;
    if (type == "array")
        return json::array();
    if (type == "object")
        return json::object();
    return "";
}

json control_tool_arguments(const json& schema) {
    json arguments = json::object();
    if (!schema.is_object())
        return arguments;
    const auto properties = schema.find("properties");
    if (properties == schema.end() || !properties->is_object())
        return arguments;
    for (auto it = properties->begin(); it != properties->end(); ++it)
        arguments[it.key()] = control_minimal_schema_value(it.value());
    return arguments;
}

std::string control_redacted_result(const RpcResponse& response) {
    json value = response.result;
    redact_control_json(value);
    return clamp_utf8_bytes(value.dump(2), 48U * 1024U);
}

bool control_matches(const AiEditorMainPanelState& state, const ControlActionSpec& action) {
    if (action.advanced && !state.control_developer_advanced)
        return false;
    if (state.control_group_filter != "all" && state.control_group_filter != action.group)
        return false;
    if (state.control_palette_query.empty())
        return true;
    std::string haystack = std::string(action.group) + " " + action.label + " " + action.method;
    std::string query = state.control_palette_query;
    std::ranges::transform(haystack, haystack.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::ranges::transform(query, query.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return haystack.find(query) != std::string::npos;
}

std::string control_group_key(std::string_view group) {
    std::string key(group);
    std::ranges::transform(key, key.begin(), [](unsigned char character) {
        if (std::isalnum(character) != 0)
            return static_cast<char>(std::tolower(character));
        return '-';
    });
    return key;
}
json control_center_section(const AiEditorMainPanelState& state, bool launcher_bound) {
    json children = json::array();
    children.push_back(text_node(
        "Discover and run supported user-facing APIs. Internal codec/IPC/create/destroy/test seams are intentionally omitted.",
        "muted", 42));
    json toolbar = json::array();
    toolbar.push_back(button_node("control.palette.open", "Search API / action", "control.palette",
                                  {}, "primary", !launcher_bound));
    toolbar.push_back(button_node(
        "control.advanced.toggle",
        state.control_developer_advanced ? "Developer Advanced: ON" : "Developer Advanced: OFF",
        "control.advanced.toggle", {}, state.control_developer_advanced ? "warn" : "ghost"));
    toolbar.push_back(button_node("control.retry", "Retry", "control.retry", {}, "default",
                                  state.control_last_method.empty() || !launcher_bound));
    toolbar.push_back(button_node("control.copy", "Copy result", "control.copy", {}, "ghost",
                                  state.control_last_result.empty()));
    children.push_back(row_node(std::move(toolbar)));
    children.push_back(row_node(json::array({
        badge_node("Status: " + state.control_status,
                   state.control_status == "Success" ? "ok" : state.control_status == "Failed" ? "bad" : "warn"),
        badge_node(state.control_request_id.empty() ? "Request: -" : "Request: " + state.control_request_id,
                   state.control_request_id.empty() ? "muted" : "accent"),
        badge_node("Group: " + state.control_group_filter, "muted"),
        badge_node(state.control_developer_advanced ? "Advanced enabled" : "User surface",
                   state.control_developer_advanced ? "warn" : "muted"),
    })));
    json groups = json::array();
    groups.push_back(button_node("control.group.all", "All", "control.group", {{"group", "all"}},
                                 state.control_group_filter == "all" ? "primary" : "ghost"));
    for (const char* group : {"Conversations", "Workflows", "Agents & Prompts", "Tools",
                              "Providers & Usage", "Runtime & Extensions"}) {
        groups.push_back(button_node("control.group." + control_group_key(group), group,
                                     "control.group", {{"group", group}},
                                     state.control_group_filter == group ? "primary" : "ghost"));
    }
    children.push_back(row_node(std::move(groups)));
    if (!state.control_palette_query.empty())
        children.push_back(text_node("Search: " + state.control_palette_query, "accent", 28));
    for (const char* group : {"Conversations", "Workflows", "Agents & Prompts", "Tools",
                              "Providers & Usage", "Runtime & Extensions"}) {
        json buttons = json::array();
        size_t visible = 0;
        for (const auto& action : control_action_catalog()) {
            if (std::string_view(action.group) != group || !control_matches(state, action))
                continue;
            buttons.push_back(button_node(
                "control.invoke." + std::string(action.id), action.label, "control.open",
                {{"id", action.id}, {"method", action.method}, {"label", action.label}, {"group", action.group},
                 {"default", action.defaults}, {"readOnly", action.read_only},
                 {"destructive", action.destructive}, {"advanced", action.advanced}},
                action.destructive ? "danger" : action.read_only ? "default" : "primary", !launcher_bound));
            ++visible;
        }
        if (visible != 0U) {
            buttons.push_back(text_node(std::to_string(visible) + " available actions", "muted", 24));
            children.push_back(card_node(group, json::array({row_node(std::move(buttons))}),
                                         std::string_view(group) == "Tools" ? "gold" : "cyan"));
        }
    }
    if (!state.control_tools.empty() &&
        (state.control_group_filter == "all" || state.control_group_filter == "Tools")) {
        json tools = json::array();
        for (const auto& tool : state.control_tools) {
            const json defaults = control_tool_arguments(tool.schema);
            tools.push_back(card_node(
                compact_text(tool.name, 180U),
                json::array({text_node(compact_text(tool.description, 500U), "muted", 30),
                             row_node(json::array({
                                 badge_node(tool.read_only ? "readOnly" : "write/execute",
                                            tool.read_only ? "ok" : "warn"),
                                 badge_node("permission: " + tool.permission,
                                            tool.permission == "allowed" ? "ok" : "warn"),
                                 badge_node("category: " + (tool.category.empty() ? "unknown" : tool.category),
                                            tool.category == "read" ? "ok" : "warn"),
                                 badge_node("source: " + tool.permission_source, "muted"),
                                 badge_node(tool.confirmation_required ? "confirmation required" : "confirmation: policy",
                                            tool.confirmation_required ? "warn" : "muted"),
                                 button_node("control.tool." + tool.name, "Call", "control.tool.call",
                                             {{"name", tool.name}, {"readOnly", tool.read_only},
                                              {"permission", tool.permission},
                                              {"permissionSource", tool.permission_source},
                                              {"category", tool.category},
                                              {"confirmationRequired", tool.confirmation_required},
                                              {"schema", tool.schema}, {"arguments", defaults}},
                                             tool.read_only ? "default" : "danger", !launcher_bound),
                             }))}),
                tool.read_only ? "cyan" : "gold"));
        }
        children.push_back(section_node("Discovered Tools", std::move(tools)));
    }
    if (!state.agents.empty() &&
        (state.control_group_filter == "all" || state.control_group_filter == "Agents & Prompts")) {
        json agents = json::array();
        for (const auto& agent : state.agents) {
            if (agent.id.empty())
                continue;
            agents.push_back(row_node(json::array({
                badge_node(compact_text(agent.label, 180U), "accent"),
                button_node("control.use.agent." + agent.id, "Use in Composer", "control.use_agent",
                            {{"id", agent.id}}, "default"),
            })));
        }
        children.push_back(card_node("Agents available in Composer", std::move(agents), "cyan"));
    }
    if (!state.control_prompts.empty() &&
        (state.control_group_filter == "all" || state.control_group_filter == "Agents & Prompts")) {
        json prompts = json::array();
        for (const auto& prompt : state.control_prompts) {
            prompts.push_back(row_node(json::array({
                badge_node(compact_text(prompt.name.empty() ? prompt.id : prompt.name, 180U),
                           prompt.pinned ? "accent" : "muted"),
                button_node("control.use.prompt." + prompt.id, "Use in Composer", "control.use_prompt",
                            {{"id", prompt.id}}, "default"),
            })));
        }
        children.push_back(card_node("Prompts available in Composer", std::move(prompts), "gold"));
    }
    if (state.control_group_filter == "all" || state.control_group_filter == "Runtime & Extensions") {
        children.push_back(card_node(
            "Runtime lifecycle",
            json::array({text_node("Runtime config/status use runtime.initialize and config.load. Restart/stop remain launcher-owned lifecycle controls and are shown without a dangerous RPC call.", "muted", 44),
                         row_node(json::array({
                             button_node("control.runtime.restart", "Restart (launcher-owned)", "control.unavailable", {}, "ghost", true),
                             button_node("control.runtime.stop", "Stop (launcher-owned)", "control.unavailable", {}, "ghost", true),
                         }))}),
            "gold"));
    }
    json feedback = json::array();
    if (state.control_last_method.empty()) {
        feedback.push_back(text_node("Run an action to see request id, permission, cache, timing, and result details.",
                                     "muted", 38));
    } else {
        feedback.push_back(text_node(state.control_last_label + " · " + state.control_last_method, "title", 28));
        feedback.push_back(text_node(
            "permission=" + state.control_permission + " · category=" +
                (state.control_permission_category.empty() ? "-" : state.control_permission_category) +
                " · source=" + state.control_permission_source +
                " · confirmationRequired=" + (state.control_confirmation_required ? "true" : "false") +
                " · cacheHit=" + (state.control_cache_hit ? "true" : "false") +
                " · elapsed=" + state.control_elapsed + " · truncated=" + (state.control_truncated ? "true" : "false"),
            "muted", 30));
        if (!state.control_last_error.empty())
            feedback.push_back(text_node(state.control_last_error, "bad", 42));
        if (!state.control_last_result.empty())
            append_text_chunks(feedback, state.control_last_result, "mono", 80, 8);
    }
    children.push_back(card_node("Control Center feedback", std::move(feedback),
                                 state.control_status == "Failed" ? "bad" : "cyan"));
    return section_node("Control Center / API Explorer", std::move(children));
}
json choice_card(std::string title, std::string action, const std::vector<ChoiceItem>& choices,
                 std::string_view selected, std::string prefix, bool disabled) {
    json children = json::array();
    children.push_back(text_node(std::move(title), "title", 24));
    json buttons = json::array();
    const size_t visible = std::min(choices.size(), kMaximumChoiceButtons);
    for (size_t index = 0; index < visible; ++index) {
        const auto& item = choices[index];
        buttons.push_back(button_node(prefix + "." + std::to_string(index), item.label, action,
                                      {{"value", item.id}},
                                      item.id == selected ? "primary" : "default", disabled));
    }
    children.push_back(row_node(std::move(buttons)));
    return card_node(prefix, std::move(children), "cyan");
}

json choice_row(std::string title, std::string action, const std::vector<ChoiceItem>& choices,
                std::string_view selected, std::string prefix, bool disabled) {
    json children = json::array();
    children.push_back(text_node(std::move(title), "muted", 22));
    const size_t visible = std::min(choices.size(), kMaximumChoiceButtons);
    for (size_t index = 0; index < visible; ++index) {
        const auto& item = choices[index];
        children.push_back(button_node(prefix + "." + std::to_string(index), item.label, action,
                                       {{"value", item.id}},
                                       item.id == selected ? "primary" : "ghost", disabled));
    }
    return row_node(std::move(children));
}

std::string build_panel_spec(const AiEditorMainPanelState& state, bool launcher_bound) {
    json nodes = json::array();
    const bool history_actions_disabled =
        !launcher_bound || run_is_active(state.run_phase) || state.history_task_pending;
    const bool selectors_disabled = run_is_active(state.run_phase);
    const std::string current_view = valid_view(state.selected_view) ? state.selected_view : "inspector";

    json app_bar = json::array();
    app_bar.push_back(text_node("AI Editor", "title", 30));
    json app_badges = json::array();
    app_badges.push_back(
        badge_node(state.backend_connected ? "Backend connected" : "Backend offline",
                   state.backend_connected ? "ok" : "warn"));
    app_badges.push_back(
        badge_node("Run: " + state.run_status, run_phase_style(state.run_phase)));
    app_badges.push_back(badge_node(
        state.conversation_id.empty() ? "Unsaved conversation" : "Conversation persisted",
        state.conversation_id.empty() ? "muted" : "accent"));
    app_badges.push_back(host_status_badge(state));
    app_bar.push_back(row_node(std::move(app_badges)));
    app_bar.push_back(text_node(state.conversation_title, "value", 24));
    app_bar.push_back(text_node(
        state.backend_status.empty()
            ? (launcher_bound ? "Waiting for asynchronous runtime initialization..."
                              : "Offline / 离线: backend not attached; local UI remains usable.")
            : state.backend_status,
        state.backend_connected ? "muted" : "warn", 30));
    json status_actions = json::array();
    const bool has_status_error = !state.run_error.empty() || !state.backend_connected;
    status_actions.push_back(text_node("Status: " + (state.bootstrap_pending ? "Loading" : has_status_error ? (state.backend_connected ? "Error" : "Offline") : state.run_status),
                                           has_status_error ? "bad" : state.bootstrap_pending ? "warn" : "muted", 30));
    if (!state.last_status_update.empty())
        status_actions.push_back(text_node("Updated " + state.last_status_update, "muted", 24));
    if (!state.backend_connected || state.bootstrap_pending || !state.run_error.empty())
        status_actions.push_back(button_node("status.retry", "Retry", "diagnostics.refresh", json::object(), "default", !launcher_bound));
    if (run_is_active(state.run_phase))
        status_actions.push_back(button_node("status.cancel", "Cancel", "chat.stop", json::object(), "danger"));
    app_bar.push_back(row_node(std::move(status_actions)));
    SaoUiThemeId active_ui_theme = SAO_UI_THEME_DARK;
    if (sao_ui_theme_get_active_id(&active_ui_theme) != SAO_STATUS_OK)
        active_ui_theme = SAO_UI_THEME_DARK;
    json app_actions = json::array();
    app_actions.push_back(button_node(
        "chat.new", "New Chat", "chat.new", json::object(), "primary",
        !launcher_bound || state.run_phase == RunPhase::Running ||
            state.run_phase == RunPhase::Cancelling || state.history_task_pending));
    app_actions.push_back(button_node("history.refresh", "Refresh History", "history.refresh",
                                      json::object(), "default", history_actions_disabled));
    app_actions.push_back(button_node("settings.open", "Settings", "settings.open"));
    app_actions.push_back(button_node("gpu.hunt", "GPU Hunt", "gpu.hunt"));
    app_actions.push_back(button_node("diagnostics.refresh", "Diagnostics",
                                      "diagnostics.refresh", json::object(), "ghost",
                                      !launcher_bound));
    app_bar.push_back(row_node(std::move(app_actions)));
    json theme_actions = json::array();
    theme_actions.push_back(text_node("Color Theme · session", "muted", 22));
    theme_actions.push_back(button_node(
        "theme.dark", "Dark", "theme.select", {{"value", "dark"}},
        active_ui_theme == SAO_UI_THEME_DARK ? "primary" : "ghost"));
    theme_actions.push_back(button_node(
        "theme.light", "Light", "theme.select", {{"value", "light"}},
        active_ui_theme == SAO_UI_THEME_LIGHT ? "primary" : "ghost"));
    app_bar.push_back(row_node(std::move(theme_actions)));
    json app_bar_node = card_node("AI Editor", std::move(app_bar), "muted");
    nodes.push_back(std::move(app_bar_node));

    json transcript = json::array();
    transcript.push_back(text_node("Transcript", "title", 26));
    const size_t transcript_total = state.conversation_messages_total == 0
                                        ? state.conversation_messages.size()
                                        : state.conversation_messages_total;
    transcript.push_back(text_node(
        "Showing last " + std::to_string(std::min(kMaximumVisibleMessages, transcript_total)) +
            " of " + std::to_string(transcript_total) + " messages",
        "muted", 24));
    if (state.conversation_messages.empty() && state.pending_user_text.empty() &&
        state.streamed_assistant_text.empty()) {
        transcript.push_back(text_node("No messages yet. Start with a prompt in Composer.",
                                       "muted", 42));
    } else {
        size_t begin = 0;
        if (state.conversation_messages.size() > kMaximumVisibleMessages)
            begin = state.conversation_messages.size() - kMaximumVisibleMessages;
        for (size_t index = begin; index < state.conversation_messages.size(); ++index) {
            const auto& message = state.conversation_messages[index];
            const std::string role =
                compact_text(message.value("role", std::string{"message"}), 64U);
            json message_nodes = json::array();
            append_text_chunks(message_nodes, message_content(message),
                               role == "assistant" ? "value" : "mono", 90);
            transcript.push_back(card_node(role, std::move(message_nodes),
                                           role == "assistant" ? "cyan" : "gold"));
        }
        if (!state.pending_user_text.empty()) {
            json pending = json::array();
            append_text_chunks(pending, state.pending_user_text, "mono", 90);
            pending.push_back(badge_node("Pending backend append", "warn"));
            transcript.push_back(card_node("user", std::move(pending), "gold"));
        }
        if (!state.streamed_assistant_text.empty()) {
            json assistant = json::array();
            append_text_chunks(assistant, state.streamed_assistant_text, "value", 90);
            assistant.push_back(badge_node(
                run_is_active(state.run_phase)
                    ? "Live event-stream preview"
                    : "Terminal preview; canonical history reload is reconciled once",
                "accent"));
            transcript.push_back(card_node("assistant", std::move(assistant), "cyan"));
        }
    }
    json transcript_panel = card_node("Transcript", std::move(transcript), "cyan");

    json inspector = json::array();
    inspector.push_back(text_node("Inspector", "title", 26));
    json inspector_badges = json::array();
    inspector_badges.push_back(badge_node("Status: " + state.run_status,
                                          run_phase_style(state.run_phase)));
    inspector_badges.push_back(badge_node("Approval: " + approval_label(state.selected_approval),
                                          state.selected_approval == "default" ? "muted"
                                                                               : "warn"));
    inspector_badges.push_back(badge_node(
        state.active_run_id.empty()
            ? "No active run"
            : "Run ID: " + compact_text(state.active_run_id, 1000U),
        state.active_run_id.empty() ? "muted" : "accent"));
    inspector.push_back(row_node(std::move(inspector_badges)));

    inspector.push_back(choice_row(
        "Provider: " + choice_label(state.providers, state.selected_provider, "Auto (Settings)"),
        "select.provider", state.providers, state.selected_provider, "provider",
        selectors_disabled));
    inspector.push_back(choice_row(
        "Model: " + choice_label(state.models, state.selected_model, "Auto (Settings)"),
        "select.model", state.models, state.selected_model, "model", selectors_disabled));
    inspector.push_back(choice_row(
        "Agent: " + choice_label(state.agents, state.selected_agent, "None"), "select.agent",
        state.agents, state.selected_agent, "agent", selectors_disabled));
    inspector.push_back(choice_row(
        "Workflow: " + choice_label(state.workflows, state.selected_workflow, "None"),
        "select.workflow", state.workflows, state.selected_workflow, "workflow",
        selectors_disabled));

    json mode_controls = json::array();
    mode_controls.push_back(text_node("Mode", "muted", 22));
    for (const std::string_view value : {"ask", "plan", "agent"}) {
        mode_controls.push_back(button_node(
            "mode." + std::string(value), std::string(value), "select.mode", {{"value", value}},
            state.selected_mode == value ? "primary" : "ghost", selectors_disabled));
    }
    inspector.push_back(row_node(std::move(mode_controls)));

    json approval_controls = json::array();
    approval_controls.push_back(text_node("Approval", "muted", 22));
    for (const std::string_view value : {"default", "bypass", "autopilot"}) {
        approval_controls.push_back(button_node(
            "approval." + std::string(value),
            value == "default" ? "Default" : value == "bypass" ? "Bypass" : "Autopilot",
            "select.approval", {{"value", value}},
            state.selected_approval == value ? "primary" : "ghost", selectors_disabled));
    }
    inspector.push_back(row_node(std::move(approval_controls)));
    inspector.push_back(text_node(approval_description(state.selected_approval), "muted", 30));

    json context_controls = json::array();
    context_controls.push_back(text_node("Context", "muted", 22));
    context_controls.push_back(button_node(
        "context.conversation", "Conversation", "select.context", {{"value", "conversation"}},
        state.selected_context == "conversation" ? "primary" : "ghost", selectors_disabled));
    context_controls.push_back(button_node(
        "context.current", "Current Turn", "select.context", {{"value", "current_turn"}},
        state.selected_context == "current_turn" ? "primary" : "ghost", selectors_disabled));
    inspector.push_back(row_node(std::move(context_controls)));

    json state_components = json::array();
    state_components.push_back(row_node(json::array({
        badge_node(state.thinking_status.empty() ? "Thinking: idle" : "Thinking: active",
                   state.thinking_status.empty() ? "muted" : "accent"),
        badge_node(state.tool_status.empty() ? "Tools: idle" : "Tools: active",
                   state.tool_status.empty() ? "muted" : "accent"),
        badge_node(state.usage_status.empty() ? "Usage: pending" : "Usage: ready",
                   state.usage_status.empty() ? "muted" : "ok"),
        badge_node(state.run_error.empty() ? "Error: none" : "Error: active",
                   state.run_error.empty() ? "ok" : "bad"),
    })));
    if (!state.thinking_status.empty())
        state_components.push_back(text_node("Thinking: " + state.thinking_status, "accent", 34));
    if (!state.refusal_status.empty())
        state_components.push_back(text_node("Refusal: " + state.refusal_status, "warn", 34));
    if (!state.tool_status.empty())
        state_components.push_back(text_node("Tools: " + state.tool_status, "mono", 40));
    if (!state.usage_status.empty())
        state_components.push_back(text_node("Usage: " + state.usage_status, "value", 32));
    if (!state.run_error.empty())
        state_components.push_back(text_node("Error: " + state.run_error, "bad", 42));
    inspector.push_back(card_node("State Components", std::move(state_components),
                                  state.run_phase == RunPhase::Failed ? "bad" : "cyan"));

    json platform_help = json::array();
    platform_help.push_back(text_node(
        "helperStatus — helper liveness, loaded engines, active target, and driver readiness.",
        "muted", 40));
    platform_help.push_back(text_node(
        "engineSelect — switch the active memory engine after confirmation.", "muted", 40));
    platform_help.push_back(text_node(
        "memoryRead — read a capped PID virtual-memory range without confirmation.", "muted",
        40));
    platform_help.push_back(text_node(
        "driverList — inspect driver assets, readiness, engine availability, and VT presence.",
        "muted", 40));
    platform_help.push_back(text_node(
        "hidSend — submit one mouse or keyboard event after confirmation.", "muted", 40));
    platform_help.push_back(text_node(
        "vtStatus — reports VT_ABSENT until driver_vt is installed.", "muted", 40));
    json platform_actions = json::array();
    platform_actions.push_back(button_node(
        "platform.mcp_management.open", "MCP Management", "platform.mcp_management.open",
        json::object(), "primary", !launcher_bound));
    platform_actions.push_back(button_node(
        "platform.kernel_map.open", "Kernel Map", "platform.kernel_map.open", json::object(),
        "default", !launcher_bound));
    platform_help.push_back(row_node(std::move(platform_actions)));
    json platform_section = section_node("Platform Tools", std::move(platform_help));

    json diagnostics = json::array();
    json diagnostic_actions = json::array();
    diagnostic_actions.push_back(button_node("ai.ping", "Ping", "ai.ping", json::object(),
                                             "default", !launcher_bound));
    diagnostic_actions.push_back(button_node("ai.hello", "Hello", "ai.hello", json::object(),
                                             "default", !launcher_bound));
    diagnostic_actions.push_back(button_node("diagnostics.copy", "Copy", "diagnostics.copy",
                                             json::object(), "ghost", state.output_text.empty()));
    diagnostic_actions.push_back(button_node("output.clear", "Clear", "output.clear"));
    diagnostic_actions.push_back(button_node("diagnostics.filter",
                                             state.diagnostic_filter == "errors" ? "All logs" : "Only errors",
                                             "diagnostics.filter",
                                             {{"value", state.diagnostic_filter == "errors" ? "all" : "errors"}},
                                             "default"));
    diagnostic_actions.push_back(button_node("diagnostics.auto_scroll",
                                             state.diagnostics_auto_scroll ? "Auto-scroll: ON" : "Auto-scroll: OFF",
                                             "diagnostics.auto_scroll", json::object(), "ghost"));
    diagnostics.push_back(row_node(std::move(diagnostic_actions)));
    std::string diagnostic_text = state.output_text;
    if (state.diagnostics_auto_scroll)
        diagnostic_text = tail_text(diagnostic_text, kUiTextChunkBytes * 2U);
    std::istringstream diagnostic_lines(diagnostic_text);
    std::string diagnostic_line;
    size_t diagnostic_count = 0;
    while (std::getline(diagnostic_lines, diagnostic_line) && diagnostic_count++ < 80U) {
        const bool is_error = diagnostic_line.find("error") != std::string::npos ||
                              diagnostic_line.find("Error") != std::string::npos;
        if (state.diagnostic_filter == "errors" && !is_error)
            continue;
        diagnostics.push_back(text_node(diagnostic_line, is_error ? "bad" : "mono", 30));
    }
    if (diagnostics.size() == 1U)
        diagnostics.push_back(text_node("No diagnostic output yet.", "muted", 34));
    json diagnostics_section = section_node("Diagnostics", std::move(diagnostics));
    json inspector_panel = section_node("Inspector", std::move(inspector));

    json history = json::array();
    history.push_back(text_node("History", "title", 26));
    json history_actions = json::array();
    history_actions.push_back(button_node("history.search", "Search...", "history.search",
                                          json::object(), "primary", history_actions_disabled));
    history_actions.push_back(button_node("history.clear_search", "Clear Search",
                                          "history.clear_search", json::object(), "ghost",
                                          history_actions_disabled || state.history_query.empty()));
    history_actions.push_back(button_node("history.refresh.section", "Refresh", "history.refresh",
                                          json::object(), "default", history_actions_disabled));
    const bool undo_available = state.undo_entry.has_value() &&
                                std::chrono::steady_clock::now() < state.undo_deadline;
    if (undo_available) {
        const bool undo_content_available = state.undo_entry->content_available;
        history_actions.push_back(button_node(
            "history.undo", undo_content_available ? "Undo" : "Undo (content unavailable)",
            "history.undo", json::object(), "primary",
            history_actions_disabled || !undo_content_available));
    }
    json history_status = json::array();
    const size_t visible_begin = state.history.empty() ? 0U : 1U;
    const size_t visible_end = state.history.size();
    history_status.push_back(badge_node(
        "Showing " + std::to_string(visible_begin) + "–" + std::to_string(visible_end) +
            " of " + std::to_string(state.history_total),
        state.history_total == 0 ? "muted" : "accent"));
    if (state.history_query.empty()) {
        history_status.push_back(badge_node("Conversations: " + std::to_string(state.history_total), "muted"));
    } else {
        history_status.push_back(
            badge_node("Query: " + compact_text(state.history_query, 420U), "accent"));
        history_status.push_back(badge_node("Results: " + std::to_string(state.history_total) +
                                                " · showing " +
                                                std::to_string(state.history.size()),
                                            "ok"));
    }
    if (state.history_task_pending)
        history_status.push_back(badge_node("History request in progress", "warn"));
    if (state.history_limit >= 500U)
        history_status.push_back(badge_node("History limit: 500", "warn"));
    if (undo_available && !state.undo_entry->content_available)
        history_status.push_back(badge_node("Undo: content unavailable", "warn"));
    if (state.history.size() < state.history_total)
        history_actions.push_back(button_node(
            "history.load_more", state.history_limit >= 500U ? "Load more (limit 500)" : "Load more",
            "history.load_more", json::object(), "ghost",
            history_actions_disabled || state.history_limit >= 500U));
    history.push_back(row_node(std::move(history_actions)));
    history.push_back(row_node(std::move(history_status)));
    if (state.history.empty()) {
        history.push_back(text_node(
            !launcher_bound               ? "History is unavailable while the backend is detached."
            : state.history_query.empty() ? "No history loaded yet. Use Refresh History."
                                         : "No conversations matched the current search query.",
            "muted", 34));
    } else {
        for (size_t index = 0; index < state.history.size(); ++index) {
            const auto& entry = state.history[index];
            json summary = json::array();
            summary.push_back(text_node(compact_text(entry.title, 160U), "value", 26));
            if (entry.search_result) {
                summary.push_back(
                    text_node(entry.scope + " · " + format_saved_at(entry.saved_at), "muted", 30));
                json matches = json::array();
                for (const auto& field : entry.matched_fields)
                    matches.push_back(badge_node("Matched: " + field, "accent"));
                matches.push_back(
                    badge_node("Matches: " + std::to_string(entry.match_count), "ok"));
                summary.push_back(row_node(std::move(matches)));
            } else {
                summary.push_back(text_node(
                    std::to_string(entry.message_count) + " messages · " + entry.scope + " · " +
                        (entry.model.empty() ? "auto model" : entry.model) + " · " +
                        format_saved_at(entry.saved_at),
                    "muted", 30));
            }
            json actions = json::array();
            actions.push_back(button_node("history.load." + std::to_string(index), "Load",
                                          "history.load", {{"id", entry.id}}, "primary",
                                          history_actions_disabled));
            actions.push_back(button_node("history.delete." + std::to_string(index), "Delete",
                                          "history.delete", {{"id", entry.id}}, "danger",
                                          history_actions_disabled));
            actions.push_back(button_node("history.duplicate." + std::to_string(index),
                                          "Duplicate / 复制", "history.duplicate",
                                          {{"id", entry.id}}, "default", history_actions_disabled));
            summary.push_back(row_node(std::move(actions)));
            history.push_back(card_node(compact_text(entry.id, 500U), std::move(summary),
                                        entry.id == state.conversation_id ? "ok" : "cyan"));
        }
    }
    json history_drawer = section_node("History", std::move(history));

    json composer = json::array();
    composer.push_back(text_node("Composer", "title", 24));
    json shortcut_actions = json::array();
    shortcut_actions.push_back(button_node("prompt.explain", "Explain", "composer.shortcut",
                                           {{"value", "explain"}}, "ghost",
                                           selectors_disabled));
    shortcut_actions.push_back(button_node("prompt.fix", "Fix", "composer.shortcut",
                                           {{"value", "fix"}}, "ghost",
                                           selectors_disabled));
    shortcut_actions.push_back(button_node("prompt.tests", "Tests", "composer.shortcut",
                                           {{"value", "tests"}}, "ghost",
                                           selectors_disabled));
    shortcut_actions.push_back(button_node("prompt.review", "Review", "composer.shortcut",
                                           {{"value", "review"}}, "ghost",
                                           selectors_disabled));
    composer.push_back(row_node(std::move(shortcut_actions)));
    composer.push_back(input_node("chat.composer", state.composer_text, "composer.edit",
                                  selectors_disabled, true));
    composer.push_back(text_node(
        state.composer_text.empty()
            ? "Multiline composer ready. Enter submits through Send; line breaks are preserved."
            : std::to_string(state.composer_text.size()) + " UTF-8 bytes ready to send.",
        state.composer_text.empty() ? "muted" : "accent", 30));
    json composer_actions = json::array();
    composer_actions.push_back(button_node("composer.edit.button", "Edit", "composer.edit",
                                           json::object(), "default", selectors_disabled));
    composer_actions.push_back(button_node(
        "chat.send", "Send", "chat.send", json::object(), "primary",
        !launcher_bound || state.composer_text.empty() || selectors_disabled ||
            state.history_task_pending));
    composer_actions.push_back(button_node("chat.stop", "Stop", "chat.stop", json::object(),
                                           "danger", !run_is_active(state.run_phase)));
    composer_actions.push_back(button_node("composer.clear", "Clear", "composer.clear",
                                           json::object(), "ghost", state.composer_text.empty()));
    composer.push_back(row_node(std::move(composer_actions)));
    json composer_panel = card_node("Composer", std::move(composer), "gold");

    json main_section = section_node(
        "Main", json::array({std::move(transcript_panel), std::move(composer_panel)}));
    main_section["width"] = 0;
    main_section["min_width"] = kMainMinimumWidth;
    main_section["weight"] = 1.0;

    const std::pair<std::string_view, std::string_view> sidebar_views[] = {
        {"inspector", "Inspector"},
        {"control", "Control"},
        {"history", "History"},
        {"diagnostics", "Logs"},
        {"platform", "Tools"},
    };
    json primary_view_buttons = json::array();
    json secondary_view_buttons = json::array();
    size_t view_index = 0;
    for (const auto& view : sidebar_views) {
        json button = button_node(
            "view.select." + std::string(view.first), std::string(view.second), "view.select",
            {{"view", view.first}}, current_view == view.first ? "primary" : "ghost");
        (view_index++ < 3U ? primary_view_buttons : secondary_view_buttons)
            .push_back(std::move(button));
    }
    json view_navigation = json::array();
    view_navigation.push_back(text_node("Sidebar", "title", 24));
    view_navigation.push_back(row_node(std::move(primary_view_buttons)));
    view_navigation.push_back(row_node(std::move(secondary_view_buttons)));
    view_navigation.push_back(text_node("View: " + view_label(current_view), "muted", 24));

    json active_secondary_view;
    if (current_view == "control")
        active_secondary_view = control_center_section(state, launcher_bound);
    else if (current_view == "history")
        active_secondary_view = std::move(history_drawer);
    else if (current_view == "diagnostics")
        active_secondary_view = std::move(diagnostics_section);
    else if (current_view == "platform")
        active_secondary_view = std::move(platform_section);
    else
        active_secondary_view = std::move(inspector_panel);

    json secondary_children = json::array();
    secondary_children.push_back(card_node("Workbench", std::move(view_navigation), "gold"));
    secondary_children.push_back(std::move(active_secondary_view));
    json secondary_section = section_node("Secondary", std::move(secondary_children));
    secondary_section["width"] = kWorkbenchWidth;
    secondary_section["min_width"] = kWorkbenchMinimumWidth;
    secondary_section["weight"] = 0.0;
    json columns = container_node(
        "ai-editor-columns",
        json::array({std::move(main_section), std::move(secondary_section)}),
        "horizontal", 0, kMainMinimumWidth, 1.0F, kMainMinimumWidth);
    columns["fallback"]["threshold_width"] = kWorkbenchHorizontalThreshold;
    nodes.push_back(std::move(columns));

    std::string serialized =
        json{{"version", 1}, {"title", ""}, {"nodes", std::move(nodes)}}.dump();
    if (serialized.size() <= kMaximumPanelSpecBytes)
        return serialized;

    json compact_nodes = json::array();
    compact_nodes.push_back(text_node("AI Editor", "title", 30));
    compact_nodes.push_back(text_node(
        "The full panel exceeded the UI spec budget and was compacted. Canonical conversation "
        "data remains intact; use History after this run to load another conversation.",
        "warn", 42));
    json compact_status = json::array();
    compact_status.push_back(badge_node("Run: " + state.run_status,
                                        run_phase_style(state.run_phase)));
    compact_status.push_back(badge_node(
        state.backend_connected ? "Backend connected" : "Backend offline",
        state.backend_connected ? "ok" : "warn"));
    compact_status.push_back(badge_node("Approval: " + approval_label(state.selected_approval),
                                        state.selected_approval == "default" ? "muted"
                                                                             : "warn"));
    compact_status.push_back(text_node("Conversation: " + state.conversation_title, "value", 28));
    if (!state.active_run_id.empty())
        compact_status.push_back(text_node("Run ID: " + state.active_run_id, "mono", 24));
    if (!state.streamed_assistant_text.empty())
        compact_status.push_back(text_node(
            "Assistant: " + tail_text(state.streamed_assistant_text, kUiTextChunkBytes), "value",
            110));
    if (!state.thinking_status.empty())
        compact_status.push_back(text_node(
            "Thinking: " + tail_text(state.thinking_status, kUiTextChunkBytes), "accent", 80));
    if (!state.refusal_status.empty())
        compact_status.push_back(text_node(
            "Refusal: " + tail_text(state.refusal_status, kUiTextChunkBytes), "warn", 60));
    if (!state.tool_status.empty())
        compact_status.push_back(text_node(
            "Tools: " + tail_text(state.tool_status, kUiTextChunkBytes), "mono", 60));
    if (!state.usage_status.empty())
        compact_status.push_back(text_node("Usage: " + state.usage_status, "value", 32));
    if (!state.run_error.empty())
        compact_status.push_back(text_node("Error: " + state.run_error, "bad", 42));
    compact_nodes.push_back(card_node("Status", std::move(compact_status),
                                      state.run_phase == RunPhase::Failed ? "bad" : "cyan"));

    json compact_primary_view_buttons = json::array();
    json compact_secondary_view_buttons = json::array();
    view_index = 0;
    for (const auto& view : sidebar_views) {
        json button = button_node(
            "compact.view.select." + std::string(view.first), std::string(view.second),
            "view.select", {{"view", view.first}}, current_view == view.first ? "primary" : "ghost");
        (view_index++ < 3U ? compact_primary_view_buttons : compact_secondary_view_buttons)
            .push_back(std::move(button));
    }
    compact_nodes.push_back(card_node(
        "Sidebar: " + view_label(current_view),
        json::array({row_node(std::move(compact_primary_view_buttons)),
                     row_node(std::move(compact_secondary_view_buttons))}),
        "gold"));

    json compact_transcript = json::array();
    compact_transcript.push_back(text_node("Transcript", "title", 26));
    const size_t compact_transcript_total = state.conversation_messages_total == 0
                                                ? state.conversation_messages.size()
                                                : state.conversation_messages_total;
    compact_transcript.push_back(text_node(
        "Showing last " + std::to_string(std::min<size_t>(4U, state.conversation_messages.size())) +
            " of " + std::to_string(compact_transcript_total) + " messages", "muted", 24));
    if (state.conversation_messages.empty() && state.pending_user_text.empty() &&
        state.streamed_assistant_text.empty()) {
        compact_transcript.push_back(text_node("No messages yet. Start with a prompt in Composer.",
                                               "muted", 42));
    } else {
        const size_t begin = state.conversation_messages.size() > 4U
                                 ? state.conversation_messages.size() - 4U
                                 : 0U;
        for (size_t index = begin; index < state.conversation_messages.size(); ++index) {
            const auto& message = state.conversation_messages[index];
            const std::string role = compact_text(message.value("role", std::string{"message"}), 64U);
            append_text_chunks(compact_transcript, message_content(message),
                               role == "assistant" ? "value" : "mono", 76, 2);
        }
        if (!state.pending_user_text.empty())
            append_text_chunks(compact_transcript, state.pending_user_text, "mono", 76, 2);
        if (!state.streamed_assistant_text.empty())
            append_text_chunks(compact_transcript, state.streamed_assistant_text, "value", 76, 2);
    }
    compact_nodes.push_back(card_node("Transcript", std::move(compact_transcript), "cyan"));

    json compact_composer = json::array();
    json compact_shortcuts = json::array();
    compact_shortcuts.push_back(button_node("compact.prompt.explain", "Explain",
                                            "composer.shortcut", {{"value", "explain"}},
                                            "ghost", selectors_disabled));
    compact_shortcuts.push_back(button_node("compact.prompt.fix", "Fix", "composer.shortcut",
                                            {{"value", "fix"}}, "ghost",
                                            selectors_disabled));
    compact_shortcuts.push_back(button_node("compact.prompt.tests", "Tests",
                                            "composer.shortcut", {{"value", "tests"}},
                                            "ghost", selectors_disabled));
    compact_shortcuts.push_back(button_node("compact.prompt.review", "Review",
                                            "composer.shortcut", {{"value", "review"}},
                                            "ghost", selectors_disabled));
    compact_composer.push_back(row_node(std::move(compact_shortcuts)));
    compact_composer.push_back(input_node("compact.chat.composer", state.composer_text,
                                          "composer.edit", selectors_disabled));
    json compact_actions = json::array();
    compact_actions.push_back(button_node("compact.chat.new", "New Chat", "chat.new",
                                          json::object(), "default",
                                          !launcher_bound ||
                                              state.run_phase == RunPhase::Running ||
                                              state.run_phase == RunPhase::Cancelling ||
                                              state.history_task_pending));
    compact_actions.push_back(button_node("compact.composer.edit", "Edit Composer",
                                          "composer.edit", json::object(), "default",
                                          selectors_disabled));
    compact_actions.push_back(button_node(
        "compact.chat.send", "Send", "chat.send", json::object(), "primary",
        !launcher_bound || state.composer_text.empty() || selectors_disabled ||
            state.history_task_pending));
    compact_actions.push_back(button_node("compact.chat.stop", "Stop", "chat.stop",
                                          json::object(), "danger",
                                          !run_is_active(state.run_phase)));
    compact_actions.push_back(button_node("compact.history.refresh", "Refresh History",
                                          "history.refresh", json::object(), "ghost",
                                          history_actions_disabled));
    compact_actions.push_back(button_node("compact.history.search", "Search...", "history.search",
                                          json::object(), "ghost", history_actions_disabled));
    compact_actions.push_back(button_node("compact.history.clear_search", "Clear Search",
                                          "history.clear_search", json::object(), "ghost",
                                          history_actions_disabled || state.history_query.empty()));
    compact_composer.push_back(row_node(std::move(compact_actions)));
    compact_nodes.push_back(card_node("Composer", std::move(compact_composer), "gold"));
    return json{{"version", 1}, {"title", ""}, {"nodes", std::move(compact_nodes)}}.dump();
}

sao_status_t refresh_body(AiEditorMainPanelState& state, bool force) {
    std::lock_guard publish_lock(state.publish_mutex);
    const bool launcher_bound = state.launcher != nullptr;
    std::string spec;
    {
        std::lock_guard lock(state.mutex);
        spec = build_panel_spec(state, launcher_bound);
        if (!force && spec == state.last_spec)
            return SAO_STATUS_OK;
    }
    const sao_status_t status = sao_ui_panel_body_set_spec(
        state.body, reinterpret_cast<const uint8_t*>(spec.data()), spec.size());
    if (status != SAO_STATUS_OK)
        return status;
    std::lock_guard lock(state.mutex);
    state.last_spec = std::move(spec);
    return SAO_STATUS_OK;
}

void show_settings_panel(AiEditorMainPanelState& state) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        sao_ai_editor_settings_panel_t panel = nullptr;
        {
            std::lock_guard lock(state.mutex);
            panel = state.settings_panel;
        }
        if (panel == nullptr) {
            sao_ai_editor_settings_panel_t created = nullptr;
            const int32_t create_status =
                sao_ai_editor_settings_panel_create(state.compositor, state.launcher, &created);
            if (create_status != SAO_AI_EDITOR_OK || created == nullptr) {
                append_output_line(state, "[error] Settings panel create failed: " +
                                              std::to_string(create_status));
                return;
            }
            {
                std::lock_guard lock(state.mutex);
                if (state.settings_panel == nullptr)
                    state.settings_panel = created;
                panel = state.settings_panel;
            }
            if (panel != created) {
                const int32_t retire_status = sao_ai_editor_settings_panel_try_destroy(created);
                if (retire_status != SAO_AI_EDITOR_OK &&
                    retire_status != SAO_AI_EDITOR_ERR_HANDLE_INVALID) {
                    append_output_line(state, "[warn] duplicate Settings panel retire deferred: " +
                                                  std::to_string(retire_status));
                }
            }
        }
        const int32_t show_status = sao_ai_editor_settings_panel_show(panel);
        if (show_status == SAO_AI_EDITOR_OK)
            return;
        if (show_status != SAO_AI_EDITOR_ERR_HANDLE_INVALID || attempt != 0) {
            append_output_line(state,
                               "[error] Settings panel show failed: " +
                                   std::to_string(show_status));
            return;
        }
        std::lock_guard lock(state.mutex);
        if (state.settings_panel == panel)
            state.settings_panel = nullptr;
    }
}

void show_gpu_hunt_panel(AiEditorMainPanelState& state) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        sao_ai_editor_gpu_hunt_panel_t panel = nullptr;
        {
            std::lock_guard lock(state.mutex);
            panel = state.gpu_hunt_panel;
        }
        if (panel == nullptr) {
            sao_ai_editor_gpu_hunt_panel_t created = nullptr;
            const int32_t create_status =
                sao_ai_editor_gpu_hunt_panel_create(state.compositor, &created);
            if (create_status != SAO_AI_EDITOR_OK || created == nullptr) {
                append_output_line(state, "[error] GPU Hunt panel create failed: " +
                                              std::to_string(create_status));
                return;
            }
            {
                std::lock_guard lock(state.mutex);
                if (state.gpu_hunt_panel == nullptr)
                    state.gpu_hunt_panel = created;
                panel = state.gpu_hunt_panel;
            }
            if (panel != created)
                (void)sao_ai_editor_gpu_hunt_panel_try_destroy(created);
        }
        const int32_t show_status = sao_ai_editor_gpu_hunt_panel_show(panel);
        if (show_status == SAO_AI_EDITOR_OK)
            return;
        if (show_status != SAO_AI_EDITOR_ERR_HANDLE_INVALID || attempt != 0) {
            append_output_line(state,
                               "[error] GPU Hunt panel show failed: " +
                                   std::to_string(show_status));
            return;
        }
        std::lock_guard lock(state.mutex);
        if (state.gpu_hunt_panel == panel)
            state.gpu_hunt_panel = nullptr;
    }
}

sao_status_t ensure_dialog(AiEditorMainPanelState& state) {
    if (state.dialog != nullptr)
        return SAO_STATUS_OK;
    return sao_ui_dialog_create(state.compositor, nullptr, &state.dialog);
}

bool dialog_visible(AiEditorMainPanelState& state) {
    if (state.dialog == nullptr)
        return false;
    bool visible = false;
    return sao_ui_dialog_is_visible(state.dialog, &visible) == SAO_STATUS_OK && visible;
}

void search_history(AiEditorMainPanelState& state, std::string query);
void delete_history_conversation(AiEditorMainPanelState& state, std::string id);
int32_t hide_active_dialog(AiEditorMainPanelState& state);
std::string payload_string(const json& payload, std::string_view key);
bool enqueue_or_report(AiEditorMainPanelState& state, RpcTask task, std::string_view method);
void queue_generic_action(AiEditorMainPanelState& state, std::string method,
                          json params = json::object());
void show_control_action_dialog(AiEditorMainPanelState& state, const json& payload);
void show_control_palette_dialog(AiEditorMainPanelState& state);
void show_control_confirm_dialog(AiEditorMainPanelState& state);
void queue_control_request(AiEditorMainPanelState& state, std::string method,
                           std::string label, std::string group, json params,
                           bool read_only, bool destructive, bool advanced);
void SAO_UI_CALL composer_dialog_result(SaoUiDialogButton pressed, const char* input_text_utf8,
                                        size_t input_text_len, void* user_data);
void show_control_confirm_dialog(AiEditorMainPanelState& state) {
    if (dialog_visible(state)) {
        append_output_line(state, "[warn] finish or dismiss the current Control Center dialog first");
        return;
    }
    if (ensure_dialog(state) != SAO_STATUS_OK) {
        std::lock_guard lock(state.mutex);
        clear_pending_control_locked(state);
        return;
    }
    std::string message;
    {
        std::lock_guard lock(state.mutex);
        message = "Confirm " + state.pending_control_method +
                  "? This action can change persisted state or execute code.";
        state.pending_dialog = DialogIntent::ControlConfirm;
    }
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_ASK;
    spec.title_utf8 = "Control Center confirmation";
    spec.message_utf8 = message.c_str();
    spec.width = 680;
    spec.height = 300;
    spec.draggable = true;
    spec.dismiss_on_esc = true;
    if (sao_ui_dialog_show(state.dialog, &spec, &composer_dialog_result, &state) != SAO_STATUS_OK) {
        std::lock_guard lock(state.mutex);
        clear_pending_control_locked(state);
    }
}

void show_control_action_dialog(AiEditorMainPanelState& state, const json& payload) {
    const std::string requested_id = payload_string(payload, "id");
    const std::string requested_method = payload_string(payload, "method");
    const ControlActionSpec* canonical_by_id =
        requested_id.empty() ? nullptr : find_control_action(requested_id);
    const ControlActionSpec* canonical_by_method =
        requested_method.empty() ? nullptr : find_control_action(requested_method);
    if ((!requested_id.empty() && canonical_by_id == nullptr) ||
        (!requested_method.empty() && canonical_by_method == nullptr) ||
        (canonical_by_id != nullptr && canonical_by_method != nullptr &&
         canonical_by_id != canonical_by_method)) {
        append_output_line(state, "[error] Control Center rejected a non-canonical action.");
        return;
    }
    const ControlActionSpec* canonical =
        canonical_by_id != nullptr ? canonical_by_id : canonical_by_method;
    if (canonical == nullptr) {
        append_output_line(state, "[error] Control Center rejected a non-canonical action.");
        return;
    }
    {
        std::lock_guard lock(state.mutex);
        if (canonical->advanced && !state.control_developer_advanced) {
            state.run_error = "Developer Advanced is required for this Control Center action.";
            return;
        }
    }
    if (dialog_visible(state)) {
        append_output_line(state, "[warn] finish or dismiss the current Control Center dialog first");
        return;
    }
    if (canonical->method == std::string_view{"providers.configure"}) {
        append_output_line(state, "[control] provider secrets remain in Settings / secret dialog");
        show_settings_panel(state);
        return;
    }
    if (ensure_dialog(state) != SAO_STATUS_OK) {
        append_output_line(state, "[error] Control Center dialog create failed");
        return;
    }

    json display_params = json::parse(canonical->defaults, nullptr, false, false);
    if (display_params.is_discarded() || !display_params.is_object())
        display_params = json::object();
    json execution_params = display_params;
    const bool retry_payload = payload.contains("executionParams");
    if (retry_payload) {
        const json retry_display = json::parse(payload.value("default", std::string{"{}"}),
                                               nullptr, false, false);
        if (!retry_display.is_discarded() && retry_display.is_object())
            display_params = retry_display;
        if (payload["executionParams"].is_object())
            execution_params = payload["executionParams"];
    }

    bool read_only = canonical->read_only;
    bool destructive = canonical->destructive;
    const bool advanced = canonical->advanced;
    std::string tool_name;
    ControlToolItem tool;
    bool has_tool = false;
    {
        std::lock_guard lock(state.mutex);
        if (canonical->method == std::string_view{"tools.call"}) {
            tool_name = payload_string(payload, "name");
            if (tool_name.empty() && execution_params.is_object())
                tool_name = execution_params.value("name", std::string{});
            const auto found = find_control_tool(state, tool_name);
            if (found == nullptr) {
                state.run_error = "Control Center rejected an unknown tool.";
                return;
            }
            tool = *found;
            has_tool = true;
        }
    }
    if (has_tool) {
        json arguments = display_params.value("arguments", json::object());
        if (!retry_payload)
            arguments = payload.value("arguments", arguments);
        if (!arguments.is_object())
            arguments = json::object();
        display_params = {{"mode", "agent"}, {"name", tool.name}, {"arguments", arguments}};
        if (!execution_params.is_object())
            execution_params = display_params;
        execution_params["mode"] = "agent";
        execution_params["name"] = tool.name;
        if (!execution_params.contains("arguments") || !execution_params["arguments"].is_object())
            execution_params["arguments"] = arguments;
        read_only = tool.read_only;
        destructive = !tool.read_only;
    }

    const std::string default_text = control_default_json_text(display_params);
    {
        std::lock_guard lock(state.mutex);
        state.pending_control_method = canonical->method;
        state.pending_control_label = canonical->label;
        state.pending_control_group = canonical->group;
        state.pending_control_params = display_params;
        state.pending_control_execution_params = execution_params;
        state.pending_control_read_only = read_only;
        state.pending_control_destructive = destructive;
        state.pending_control_advanced = advanced;
        state.pending_control_tool_name = tool_name;
        state.pending_control_tool_arguments = execution_params.value("arguments", json::object());
        state.pending_control_permission = has_tool ? tool.permission : std::string{};
        state.pending_control_permission_source = has_tool ? tool.permission_source : std::string{};
        state.pending_control_permission_category = has_tool ? tool.category : std::string{};
        state.pending_control_confirmation_required = has_tool && tool.confirmation_required;
        state.pending_dialog = DialogIntent::ControlInput;
    }
    const std::string dialog_title = canonical->label;
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_INPUT;
    spec.title_utf8 = dialog_title.c_str();
    spec.message_utf8 = "Edit the compact JSON parameters. Sensitive fields are redacted in display and logs.";
    spec.input_prompt_utf8 = "Parameters JSON";
    spec.input_default_utf8 = default_text.c_str();
    spec.input_max_length = static_cast<int32_t>(kMaximumActionPayloadBytes);
    spec.width = 760;
    spec.height = 480;
    spec.draggable = true;
    spec.dismiss_on_esc = true;
    if (sao_ui_dialog_show(state.dialog, &spec, &composer_dialog_result, &state) != SAO_STATUS_OK) {
        std::lock_guard lock(state.mutex);
        clear_pending_control_locked(state);
    }
}

void show_copy_text_dialog(AiEditorMainPanelState& state, std::string title, std::string text) {
    if (dialog_visible(state)) {
        append_output_line(state, "[warn] finish or dismiss the current dialog first");
        return;
    }
    if (ensure_dialog(state) != SAO_STATUS_OK) {
        append_output_line(state, "[warn] clipboard bridge unavailable; select text manually from the panel");
        return;
    }
    text = clamp_utf8_bytes(std::move(text), kOutputTrimBytes);
    {
        std::lock_guard lock(state.mutex);
        state.pending_copy_text = text;
        state.pending_dialog = DialogIntent::CopyText;
    }
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_INPUT;
    spec.title_utf8 = title.c_str();
    spec.message_utf8 = "Clipboard bridge unavailable. Select the text below and copy it manually.";
    spec.input_prompt_utf8 = "Text";
    spec.input_default_utf8 = text.c_str();
    spec.input_max_length = static_cast<int32_t>(kOutputTrimBytes);
    spec.width = 760;
    spec.height = 520;
    spec.draggable = true;
    spec.dismiss_on_esc = true;
    if (sao_ui_dialog_show(state.dialog, &spec, &composer_dialog_result, &state) != SAO_STATUS_OK) {
        std::lock_guard lock(state.mutex);
        state.pending_copy_text.clear();
        state.pending_dialog = DialogIntent::None;
    }
}

void show_control_palette_dialog(AiEditorMainPanelState& state) {
    if (dialog_visible(state)) {
        append_output_line(state, "[warn] finish or dismiss the current Control Center dialog first");
        return;
    }
    if (ensure_dialog(state) != SAO_STATUS_OK)
        return;
    std::string query;
    {
        std::lock_guard lock(state.mutex);
        query = state.control_palette_query;
        state.pending_dialog = DialogIntent::ControlPalette;
    }
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_INPUT;
    spec.title_utf8 = "Search API / action";
    spec.message_utf8 = "Enter a method, label, or group. Results stay filtered in Control Center.";
    spec.input_prompt_utf8 = "Query";
    spec.input_default_utf8 = query.c_str();
    spec.input_max_length = 512;
    spec.width = 640;
    spec.height = 280;
    spec.draggable = true;
    spec.dismiss_on_esc = true;
    if (sao_ui_dialog_show(state.dialog, &spec, &composer_dialog_result, &state) != SAO_STATUS_OK) {
        std::lock_guard lock(state.mutex);
        state.pending_dialog = DialogIntent::None;
    }
}

int32_t hide_active_dialog(AiEditorMainPanelState& state) {
    sao_ui_dialog_handle_t dialog = nullptr;
    {
        std::lock_guard lock(state.mutex);
        dialog = state.dialog;
    }
    if (dialog == nullptr)
        return SAO_AI_EDITOR_OK;
    bool visible = false;
    const sao_status_t visible_status = sao_ui_dialog_is_visible(dialog, &visible);
    if (visible_status != SAO_STATUS_OK)
        return map_ui_status(visible_status);
    if (!visible)
        return SAO_AI_EDITOR_OK;
    const sao_status_t hide_status = sao_ui_dialog_hide(dialog);
    if (hide_status != SAO_STATUS_OK && hide_status != SAO_STATUS_ERR_NOT_INITIALIZED)
        return map_ui_status(hide_status);
    const sao_status_t tick_status = sao_ui_dialog_tick(dialog, kDialogCloseAdvanceMs);
    if (tick_status != SAO_STATUS_OK && tick_status != SAO_STATUS_ERR_NOT_INITIALIZED)
        return map_ui_status(tick_status);
    return SAO_AI_EDITOR_OK;
}

std::string payload_string(const json& payload, std::string_view key) {
    const auto found = payload.find(std::string(key));
    if (found == payload.end() || !found->is_string())
        return {};
    return found->get<std::string>();
}

bool enqueue_or_report(AiEditorMainPanelState& state, RpcTask task, std::string_view method) {
    std::string error;
    if (queue_rpc_task(state, std::move(task), &error))
        return true;
    append_output_line(state, "[error] " + error + "; cannot send \"" + std::string(method) +
                                  "\"");
    return false;
}

void queue_generic_action(AiEditorMainPanelState& state, std::string method, json params) {
    RpcTask task;
    task.kind = RpcTaskKind::Generic;
    task.method = method;
    task.params = std::move(params);
    (void)enqueue_or_report(state, std::move(task), method);
}

void queue_control_request(AiEditorMainPanelState& state, std::string method,
                           std::string label, std::string group, json params,
                           bool read_only, bool destructive, bool advanced) {
    (void)label;
    (void)group;
    (void)read_only;
    (void)destructive;
    (void)advanced;
    const ControlActionSpec* canonical = find_control_action(method);
    if (canonical == nullptr) {
        append_output_line(state, "[error] Control Center rejected non-canonical method: " + method);
        std::lock_guard lock(state.mutex);
        clear_pending_control_locked(state);
        return;
    }

    ControlToolItem tool;
    bool has_tool = false;
    {
        std::lock_guard lock(state.mutex);
        if (canonical->advanced && !state.control_developer_advanced) {
            state.run_error = "Developer Advanced is required for this Control Center action.";
            clear_pending_control_locked(state);
            return;
        }
        if (canonical->method == std::string_view{"tools.call"}) {
            const std::string name = params.is_object() ? params.value("name", std::string{}) : std::string{};
            const auto found = find_control_tool(state, name);
            if (found == nullptr) {
                state.run_error = "Control Center rejected an unknown tool.";
                clear_pending_control_locked(state);
                return;
            }
            tool = *found;
            has_tool = true;
        }
    }

    json execution_params = params.is_object() ? std::move(params) : json::object();
    bool effective_read_only = canonical->read_only;
    bool effective_destructive = canonical->destructive;
    const bool effective_advanced = canonical->advanced;
    std::string permission;
    std::string permission_source;
    std::string permission_category;
    bool confirmation_required = false;
    std::string tool_name;
    json tool_arguments = json::object();
    if (has_tool) {
        effective_read_only = tool.read_only;
        effective_destructive = !tool.read_only;
        permission = tool.permission;
        permission_source = tool.permission_source;
        permission_category = tool.category;
        confirmation_required = tool.confirmation_required;
        tool_name = tool.name;
        tool_arguments = execution_params.value("arguments", json::object());
        if (!tool_arguments.is_object())
            tool_arguments = json::object();
        execution_params["mode"] = "agent";
        execution_params["name"] = tool.name;
        execution_params["arguments"] = tool_arguments;
    }
    const bool requires_confirmation = control_requires_confirmation(
        permission, permission_category, confirmation_required, effective_read_only,
        effective_destructive, effective_advanced);
    if (has_tool && requires_confirmation && !tool_arguments.value("confirmed", false)) {
        append_output_line(state, "[warn] tool call requires confirmation before execution");
        std::lock_guard lock(state.mutex);
        clear_pending_control_locked(state);
        return;
    }

    RpcTask task;
    task.kind = RpcTaskKind::Generic;
    task.method = canonical->method;
    task.params = execution_params;
    task.control_action = true;
    task.control_group = canonical->group;
    task.control_label = canonical->label;
    {
        std::lock_guard lock(state.mutex);
        state.control_status = "Pending";
        state.control_last_method = task.method;
        state.control_last_label = task.control_label;
        state.control_last_group = task.control_group;
        state.control_last_params = execution_params;
        redact_control_json(state.control_last_params);
        state.control_last_execution_params = execution_params;
        state.control_last_tool_name = std::move(tool_name);
        state.control_last_tool_arguments = std::move(tool_arguments);
        state.control_last_read_only = effective_read_only;
        state.control_last_destructive = effective_destructive;
        state.control_last_advanced = effective_advanced;
        state.control_permission = std::move(permission);
        state.control_permission_source = std::move(permission_source);
        state.control_permission_category = std::move(permission_category);
        state.control_confirmation_required = confirmation_required;
        state.control_last_result.clear();
        state.control_last_error.clear();
        state.control_elapsed.clear();
        state.control_cache_hit = false;
        state.control_truncated = false;
    }
    std::string error;
    if (!queue_rpc_task(state, std::move(task), &error)) {
        {
            std::lock_guard lock(state.mutex);
            state.control_status = "Failed";
            state.control_last_error = redact_control_text(error);
            clear_pending_control_locked(state);
        }
        append_output_line(state, "[error] control request not queued: " + error);
    }
}


void show_history_delete_dialog(AiEditorMainPanelState& state, HistoryEntry entry) {
    if (dialog_visible(state)) {
        append_output_line(state, "[warn] finish or dismiss the current dialog first");
        return;
    }
    const sao_status_t create_status = ensure_dialog(state);
    if (create_status != SAO_STATUS_OK) {
        append_output_line(state, "[error] history delete dialog create failed: " +
                                      std::to_string(create_status));
        return;
    }
    const std::string title = "Delete conversation? / 删除会话？";
    const std::string message = entry.search_result
                                    ? "Delete \"" + compact_text(entry.title, 180U) +
                                          "\"? The canonical conversation will be checked before deletion. You can Undo for 5 seconds."
                                    : "Delete \"" + compact_text(entry.title, 180U) +
                                          "\" with " + std::to_string(entry.message_count) +
                                          " messages? You can Undo for 5 seconds.";
    {
        std::lock_guard lock(state.mutex);
        state.pending_delete = std::move(entry);
        state.pending_dialog = DialogIntent::ConfirmDelete;
    }
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_ASK;
    spec.title_utf8 = title.c_str();
    spec.message_utf8 = message.c_str();
    spec.width = 620;
    spec.height = 300;
    spec.draggable = true;
    spec.dismiss_on_esc = true;
    if (sao_ui_dialog_show(state.dialog, &spec, &composer_dialog_result, &state) != SAO_STATUS_OK) {
        std::lock_guard lock(state.mutex);
        state.pending_dialog = DialogIntent::None;
        state.pending_delete.reset();
    }
}

void show_composer_dialog(AiEditorMainPanelState& state) {
    if (dialog_visible(state)) {
        append_output_line(state, "[warn] finish or dismiss the current composer dialog first");
        return;
    }
    const sao_status_t create_status = ensure_dialog(state);
    if (create_status != SAO_STATUS_OK) {
        append_output_line(state, "[error] composer dialog create failed: " +
                                      std::to_string(create_status));
        return;
    }
    std::string default_value;
    {
        std::lock_guard lock(state.mutex);
        default_value = state.composer_text;
        state.pending_dialog = DialogIntent::EditComposer;
    }
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_INPUT;
    spec.title_utf8 = "AI Editor Composer";
    spec.message_utf8 = "Enter the user message to send to the selected provider and model.";
    spec.input_prompt_utf8 = "Message";
    spec.input_default_utf8 = default_value.c_str();
    spec.input_max_length = static_cast<int32_t>(kMaximumComposerBytes);
    spec.width = 720;
    spec.height = 380;
    spec.draggable = true;
    spec.dismiss_on_esc = true;
    const sao_status_t show_status =
        sao_ui_dialog_show(state.dialog, &spec, &composer_dialog_result, &state);
    if (show_status != SAO_STATUS_OK) {
        {
            std::lock_guard lock(state.mutex);
            state.pending_dialog = DialogIntent::None;
        }
        append_output_line(state, "[error] composer dialog show failed: " +
                                      std::to_string(show_status));
    }
}

void show_history_search_dialog(AiEditorMainPanelState& state) {
    {
        std::lock_guard lock(state.mutex);
        if (run_is_active(state.run_phase)) {
            state.run_error = "Stop the active run before searching history.";
            return;
        }
        if (state.history_task_pending) {
            state.run_error = "Wait for the current history request to finish.";
            return;
        }
    }
    if (dialog_visible(state)) {
        append_output_line(state, "[warn] finish or dismiss the current input dialog first");
        return;
    }
    const sao_status_t create_status = ensure_dialog(state);
    if (create_status != SAO_STATUS_OK) {
        append_output_line(state, "[error] history search dialog create failed: " +
                                      std::to_string(create_status));
        return;
    }
    std::string default_value;
    {
        std::lock_guard lock(state.mutex);
        default_value = state.history_query;
        state.pending_dialog = DialogIntent::SearchHistory;
    }
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_INPUT;
    spec.title_utf8 = "Search History";
    spec.message_utf8 =
        "Search conversation titles and messages across all history scopes. Empty input clears "
        "the search.";
    spec.input_prompt_utf8 = "Query";
    spec.input_default_utf8 = default_value.c_str();
    spec.input_max_length = static_cast<int32_t>(kMaximumHistoryQueryBytes);
    spec.width = 640;
    spec.height = 300;
    spec.draggable = true;
    spec.dismiss_on_esc = true;
    const sao_status_t show_status =
        sao_ui_dialog_show(state.dialog, &spec, &composer_dialog_result, &state);
    if (show_status != SAO_STATUS_OK) {
        {
            std::lock_guard lock(state.mutex);
            state.pending_dialog = DialogIntent::None;
        }
        append_output_line(state, "[error] history search dialog show failed: " +
                                      std::to_string(show_status));
    }
}

int32_t tick_dialog(AiEditorMainPanelState& state) {
    sao_ui_dialog_handle_t dialog = nullptr;
    int32_t elapsed_ms = 16;
    {
        std::lock_guard lock(state.mutex);
        dialog = state.dialog;
        const auto now = std::chrono::steady_clock::now();
        if (state.last_dialog_tick.time_since_epoch().count() != 0) {
            elapsed_ms = static_cast<int32_t>(std::clamp<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last_dialog_tick)
                    .count(),
                0, 100));
        }
        state.last_dialog_tick = now;
    }
    if (dialog == nullptr)
        return SAO_AI_EDITOR_OK;
    const sao_status_t status = sao_ui_dialog_tick(dialog, elapsed_ms);
    if (status == SAO_STATUS_OK || status == SAO_STATUS_ERR_NOT_INITIALIZED)
        return SAO_AI_EDITOR_OK;
    return map_ui_status(status);
}

void SAO_UI_CALL composer_dialog_result(SaoUiDialogButton pressed, const char* input_text_utf8,
                                        size_t input_text_len, void* user_data) {
    auto* state = static_cast<AiEditorMainPanelState*>(user_data);
    if (state == nullptr || require_owner_thread(state->compositor) != SAO_AI_EDITOR_OK)
        return;
    DialogIntent intent = DialogIntent::None;
    std::string input;
    std::string delete_id;
    std::string control_method;
    std::string control_label;
    std::string control_group;
    json control_params = json::object();
    json control_execution_params = json::object();
    bool control_read_only = false;
    bool control_destructive = false;
    bool control_advanced = false;
    bool ask_control_confirmation = false;
    bool execute_control = false;
    const bool accepted = pressed == SAO_UI_DIALOG_BTN_OK || pressed == SAO_UI_DIALOG_BTN_YES;
    {
        std::lock_guard lock(state->mutex);
        intent = state->pending_dialog;
        state->pending_dialog = DialogIntent::None;
        if (intent == DialogIntent::CopyText) {
            state->pending_copy_text.clear();
            return;
        }
        if (intent == DialogIntent::ControlPalette) {
            if (!accepted || !state->accepting)
                return;
            if (input_text_utf8 == nullptr && input_text_len != 0) {
                state->run_error = "Control Center search returned an invalid buffer.";
                return;
            }
            state->control_palette_query = trim_copy(
                input_text_utf8 == nullptr ? std::string_view{} :
                                             std::string_view(input_text_utf8, input_text_len));
            return;
        }
        if (intent == DialogIntent::ControlInput || intent == DialogIntent::ControlConfirm) {
            if (!accepted || !state->accepting) {
                clear_pending_control_locked(*state);
                return;
            }
            if (input_text_utf8 == nullptr && input_text_len != 0) {
                state->run_error = "Control Center dialog returned an invalid buffer.";
                clear_pending_control_locked(*state);
                return;
            }
            if (intent == DialogIntent::ControlInput) {
                input = input_text_utf8 == nullptr ? std::string{} :
                                                     std::string(input_text_utf8, input_text_len);
                control_params = json::parse(input, nullptr, false, false);
                if (control_params.is_discarded() || !control_params.is_object()) {
                    state->run_error = "Control Center parameters must be a JSON object.";
                    clear_pending_control_locked(*state);
                    return;
                }
                const bool preserve_protected_execution =
                    input == control_default_json_text(state->pending_control_params);
                state->pending_control_params = control_params;
                if (!preserve_protected_execution)
                    state->pending_control_execution_params = control_params;
                ask_control_confirmation = control_requires_confirmation(
                    state->pending_control_permission, state->pending_control_permission_category,
                    state->pending_control_confirmation_required, state->pending_control_read_only,
                    state->pending_control_destructive, state->pending_control_advanced);
            }
            control_method = state->pending_control_method;
            control_label = state->pending_control_label;
            control_group = state->pending_control_group;
            control_execution_params = state->pending_control_execution_params;
            control_read_only = state->pending_control_read_only;
            control_destructive = state->pending_control_destructive;
            control_advanced = state->pending_control_advanced;
            if (intent == DialogIntent::ControlConfirm && control_method == "tools.call") {
                if (!control_execution_params.is_object())
                    control_execution_params = json::object();
                json arguments = control_execution_params.value("arguments", json::object());
                if (!arguments.is_object())
                    arguments = json::object();
                arguments["confirmed"] = true;
                control_execution_params["arguments"] = std::move(arguments);
            }
            if (control_method.empty()) {
                state->run_error = "Control Center request is missing a canonical method.";
                clear_pending_control_locked(*state);
                return;
            }
            ask_control_confirmation = intent == DialogIntent::ControlInput && ask_control_confirmation;
            execute_control = !ask_control_confirmation;
            if (execute_control)
                clear_pending_control_locked(*state);
            else
                state->pending_dialog = DialogIntent::ControlConfirm;
        } else {
            if (intent == DialogIntent::ConfirmDelete) {
                if (accepted && state->accepting && state->pending_delete.has_value())
                    delete_id = state->pending_delete->id;
                state->pending_delete.reset();
                if (delete_id.empty())
                    return;
            } else {
                state->pending_delete.reset();
                if (!state->accepting || !accepted)
                    return;
            }
            if (input_text_utf8 == nullptr && input_text_len != 0) {
                state->run_error = "Input dialog returned an invalid buffer.";
                return;
            }
            input = input_text_utf8 == nullptr ? std::string{} :
                                                 std::string(input_text_utf8, input_text_len);
            if (intent == DialogIntent::EditComposer) {
                if (input_text_len > kMaximumComposerBytes) {
                    state->run_error = "Composer text exceeds the 48 KiB limit.";
                    return;
                }
                state->composer_text = std::move(input);
                state->run_error.clear();
                return;
            }
            if (intent != DialogIntent::SearchHistory)
                return;
            if (input_text_len > kMaximumHistoryQueryBytes) {
                state->run_error = "History search query exceeds the 1 KiB limit.";
                return;
            }
        }
    }
    if (ask_control_confirmation) {
        (void)hide_active_dialog(*state);
        show_control_confirm_dialog(*state);
        return;
    }
    if (execute_control) {
        queue_control_request(*state, std::move(control_method), std::move(control_label),
                              std::move(control_group), std::move(control_execution_params),
                              control_read_only, control_destructive, control_advanced);
        return;
    }
    if (!delete_id.empty()) {
        delete_history_conversation(*state, std::move(delete_id));
        return;
    }
    search_history(*state, std::move(input));
}

uint64_t cancel_starting_generation_locked(AiEditorMainPanelState& state,
                                           bool reconcile_conversation) {
    const uint64_t cancelled_generation = state.conversation_generation;
    state.cancelled_generation = std::max(state.cancelled_generation, cancelled_generation);
    state.starting_cancelled_generation =
        reconcile_conversation ? cancelled_generation : uint64_t{0};
    std::erase_if(state.rpc_queue, [&](const RpcTask& queued) {
        return queued.generation == cancelled_generation;
    });
    for (auto& completion : state.completed_queue) {
        if (completion.task.generation != cancelled_generation)
            continue;
        completion.cancelled = true;
        if (completion.task.kind == RpcTaskKind::SendMessage &&
            !completion_has_successful_step(completion, "run.cancel")) {
            completion.task.run_id = accepted_run_id(completion);
            if (!queue_orphan_cancel_locked(state, cancelled_generation,
                                            completion.task.run_id)) {
                state.run_error = "Failed to queue orphan run cancellation.";
            }
        }
    }
    state.conversation_generation =
        state.generation_atomic.fetch_add(1, std::memory_order_relaxed) + 1;
    state.active_run_id.clear();
    state.event_drain_pending = false;
    state.run_poll_pending = false;
    state.terminal_reload_run_id.clear();
    state.run_phase = RunPhase::Cancelled;
    state.run_status = "cancelled while starting";
    return cancelled_generation;
}

void queue_orphan_run_cancel(AiEditorMainPanelState& state, uint64_t generation,
                             std::string run_id) {
    if (run_id.empty())
        return;
    RpcTask cancel;
    cancel.kind = RpcTaskKind::CancelRun;
    cancel.generation = generation;
    cancel.run_id = std::move(run_id);
    cancel.orphan_cleanup = true;
    cancel.orphan_cancel_attempt = 1U;
    std::string error;
    if (!queue_rpc_task(state, std::move(cancel), &error))
        append_output_line(state, "[warn] orphan run.cancel not queued: " + error);
}

void begin_new_conversation(AiEditorMainPanelState& state) {
    RpcTask task;
    uint64_t cancelled_generation = 0;
    std::string orphan_run_id;
    {
        std::lock_guard lock(state.mutex);
        if (state.run_phase == RunPhase::Running || state.run_phase == RunPhase::Cancelling) {
            state.run_error = "Stop the active run before starting a new conversation.";
            return;
        }
        if (state.history_task_pending) {
            state.run_error = "Wait for the current history request before starting a new chat.";
            return;
        }
        if (state.run_phase == RunPhase::Starting) {
            orphan_run_id = state.active_run_id;
            cancelled_generation = cancel_starting_generation_locked(state, false);
        } else {
            state.conversation_generation =
                state.generation_atomic.fetch_add(1, std::memory_order_relaxed) + 1;
            state.starting_cancelled_generation = 0;
        }
        task.kind = RpcTaskKind::NewConversation;
        task.generation = state.conversation_generation;
        task.title = "New Chat " + format_iso_timestamp();
        task.model = state.selected_model;
        state.conversation_id.clear();
        state.conversation_title = "New Chat";
        state.conversation_messages = json::array();
        state.composer_text.clear();
        state.pending_user_text.clear();
        state.active_run_id.clear();
        state.run_status = "creating conversation";
        state.run_phase = RunPhase::Starting;
        state.streamed_assistant_text.clear();
        state.thinking_status.clear();
        state.refusal_status.clear();
        state.tool_status.clear();
        state.usage_status.clear();
        state.event_drain_pending = false;
        state.run_poll_pending = false;
        state.terminal_reload_run_id.clear();
        state.run_error.clear();
    }
    queue_orphan_run_cancel(state, cancelled_generation, std::move(orphan_run_id));
    if (!enqueue_or_report(state, std::move(task), "conversation.create")) {
        std::lock_guard lock(state.mutex);
        state.run_status = "idle";
        state.run_phase = RunPhase::Idle;
    }
}

bool claim_history_operation(AiEditorMainPanelState& state, RpcTask& task) {
    std::lock_guard lock(state.mutex);
    if (run_is_active(state.run_phase)) {
        state.run_error = "Stop the active run before changing the history view.";
        return false;
    }
    if (state.history_task_pending) {
        state.run_error = "Wait for the current history request to finish.";
        return false;
    }
    state.history_task_pending = true;
    state.run_error.clear();
    task.history_operation = true;
    return true;
}

bool enqueue_history_operation(AiEditorMainPanelState& state, RpcTask task,
                               std::string_view method) {
    if (enqueue_or_report(state, std::move(task), method))
        return true;
    std::lock_guard lock(state.mutex);
    state.history_task_pending = false;
    return false;
}

void queue_history_refresh(AiEditorMainPanelState& state) {
    RpcTask task;
    {
        std::lock_guard lock(state.mutex);
        task.query = state.history_query;
    }
    task.kind = task.query.empty() ? RpcTaskKind::RefreshHistory : RpcTaskKind::SearchHistory;
    {
        std::lock_guard lock(state.mutex);
        task.history_limit = state.history_limit;
    }
    if (!claim_history_operation(state, task))
        return;
    const std::string_view method =
        task.query.empty() ? "conversation.list" : "conversation.search";
    (void)enqueue_history_operation(state, std::move(task), method);
}

void search_history(AiEditorMainPanelState& state, std::string query) {
    query = trim_copy(query);
    if (query.size() > kMaximumHistoryQueryBytes) {
        append_output_line(state, "[error] history search query exceeds the 1 KiB limit");
        return;
    }
    RpcTask task;
    task.query = std::move(query);
    task.kind = task.query.empty() ? RpcTaskKind::RefreshHistory : RpcTaskKind::SearchHistory;
    {
        std::lock_guard lock(state.mutex);
        task.history_limit = state.history_limit;
    }
    if (!claim_history_operation(state, task))
        return;
    const std::string_view method =
        task.query.empty() ? "conversation.list" : "conversation.search";
    (void)enqueue_history_operation(state, std::move(task), method);
}

void load_history_conversation(AiEditorMainPanelState& state, std::string id) {
    if (id.empty()) {
        append_output_line(state, "[error] history.load requires a conversation id");
        return;
    }
    RpcTask task;
    task.kind = RpcTaskKind::LoadConversation;
    if (!claim_history_operation(state, task))
        return;
    {
        std::lock_guard lock(state.mutex);
        state.conversation_generation =
            state.generation_atomic.fetch_add(1, std::memory_order_relaxed) + 1;
        state.starting_cancelled_generation = 0;
        task.generation = state.conversation_generation;
        task.conversation_id = std::move(id);
        state.conversation_id = task.conversation_id;
        state.conversation_title = "Loading...";
        state.conversation_messages = json::array();
        state.pending_user_text.clear();
        state.streamed_assistant_text.clear();
        state.thinking_status.clear();
        state.refusal_status.clear();
        state.tool_status.clear();
        state.usage_status.clear();
        state.active_run_id.clear();
        state.event_drain_pending = false;
        state.run_poll_pending = false;
        state.terminal_reload_run_id.clear();
        state.run_phase = RunPhase::Idle;
        state.run_status = "loading history";
        state.run_error.clear();
    }
    if (!enqueue_history_operation(state, std::move(task), "conversation.get")) {
        std::lock_guard lock(state.mutex);
        state.run_status = "load error";
    }
}

void delete_history_conversation(AiEditorMainPanelState& state, std::string id) {
    if (id.empty()) {
        append_output_line(state, "[error] history.delete requires a conversation id");
        return;
    }
    RpcTask task;
    task.kind = RpcTaskKind::DeleteConversation;
    if (!claim_history_operation(state, task))
        return;
    {
        std::lock_guard lock(state.mutex);
        task.conversation_id = id;
        task.query = state.history_query;
        task.history_limit = state.history_limit;
        const auto found = std::ranges::find(state.history, id, &HistoryEntry::id);
        if (found != state.history.end())
            task.restore_entry = *found;
    }
    (void)enqueue_history_operation(state, std::move(task), "conversation.delete");
}

void duplicate_history_conversation(AiEditorMainPanelState& state, std::string id,
                                    std::string title, std::string scope) {
    if (id.empty()) {
        append_output_line(state, "[error] history.duplicate requires a conversation id");
        return;
    }
    RpcTask task;
    task.kind = RpcTaskKind::DuplicateConversation;
    if (!claim_history_operation(state, task))
        return;
    {
        std::lock_guard lock(state.mutex);
        state.conversation_generation =
            state.generation_atomic.fetch_add(1, std::memory_order_relaxed) + 1;
        state.starting_cancelled_generation = 0;
        task.generation = state.conversation_generation;
        task.conversation_id = std::move(id);
        task.title = std::move(title);
        task.scope = std::move(scope);
        task.query = state.history_query;
    }
    (void)enqueue_history_operation(state, std::move(task), "conversation.duplicate");
}

void send_composer(AiEditorMainPanelState& state) {
    RpcTask task;
    std::string prompt;
    {
        std::lock_guard lock(state.mutex);
        if (run_is_active(state.run_phase)) {
            state.run_error = "A run is already active. Stop it before sending another message.";
            return;
        }
        if (state.history_task_pending) {
            state.run_error = "Wait for the current history request before sending.";
            return;
        }
        if (trim_copy(state.composer_text).empty()) {
            state.run_error = "Composer is empty.";
            return;
        }
        prompt = state.composer_text;
        task.kind = RpcTaskKind::SendMessage;
        task.generation = state.conversation_generation;
        task.conversation_id = state.conversation_id;
        task.title = state.conversation_id.empty() ? title_from_prompt(prompt)
                                                   : state.conversation_title;
        task.prompt = prompt;
        task.provider_id = state.selected_provider;
        task.model = state.selected_model;
        task.agent_id = state.selected_agent;
        task.agent_system_prompt = selected_agent_prompt(state);
        task.mode = state.selected_mode;
        task.approval = state.selected_approval;
        task.workflow_id = state.selected_workflow;
        task.context_mode = state.selected_context;
        task.messages = state.conversation_messages;
        state.starting_cancelled_generation = 0;
        state.composer_text.clear();
        state.pending_user_text = prompt;
        state.active_run_id.clear();
        state.run_phase = RunPhase::Starting;
        state.run_status = "starting";
        state.streamed_assistant_text.clear();
        state.thinking_status.clear();
        state.refusal_status.clear();
        state.tool_status.clear();
        state.usage_status.clear();
        state.event_drain_pending = false;
        state.run_poll_pending = false;
        state.terminal_reload_run_id.clear();
        state.run_error.clear();
    }
    if (!enqueue_or_report(state, std::move(task), "chat.run")) {
        std::lock_guard lock(state.mutex);
        state.composer_text = std::move(prompt);
        state.pending_user_text.clear();
        state.run_phase = RunPhase::Failed;
        state.run_status = "queue error";
    }
}

void stop_active_run(AiEditorMainPanelState& state) {
    RpcTask task;
    bool cancelled_starting = false;
    uint64_t cancelled_generation = 0;
    std::string orphan_run_id;
    {
        std::lock_guard lock(state.mutex);
        if (state.run_phase == RunPhase::Starting) {
            orphan_run_id = state.active_run_id;
            cancelled_generation = cancel_starting_generation_locked(state, true);
            if (!state.pending_user_text.empty())
                state.composer_text = state.pending_user_text;
            state.pending_user_text.clear();
            state.run_error.clear();
            cancelled_starting = true;
        }
        if (cancelled_starting) {
            state.worker_cv.notify_all();
        } else if (state.active_run_id.empty()) {
            state.run_error = "There is no active run to stop.";
            return;
        } else {
            task.kind = RpcTaskKind::CancelRun;
            task.generation = state.conversation_generation;
            task.run_id = state.active_run_id;
            state.run_phase = RunPhase::Cancelling;
            state.run_status = "cancelling";
        }
    }
    if (cancelled_starting) {
        queue_orphan_run_cancel(state, cancelled_generation, std::move(orphan_run_id));
        return;
    }
    if (!enqueue_or_report(state, std::move(task), "run.cancel")) {
        std::lock_guard lock(state.mutex);
        state.run_phase = RunPhase::Running;
        state.run_status = "running";
    }
}

void select_value(AiEditorMainPanelState& state, std::string_view action, std::string value) {
    if (action == "select.mode" && value != "ask" && value != "plan" && value != "agent") {
        append_output_line(state, "[error] invalid mode selection: " + value);
        return;
    }
    if (action == "select.approval" && value != "default" && value != "bypass" &&
        value != "autopilot") {
        append_output_line(state, "[error] invalid approval selection: " + value);
        return;
    }
    if (action == "select.context" && value != "conversation" && value != "current_turn") {
        append_output_line(state, "[error] invalid context selection: " + value);
        return;
    }
    bool refresh_models = false;
    {
        std::lock_guard lock(state.mutex);
        if (run_is_active(state.run_phase)) {
            state.run_error = "Selections are locked while a run is active.";
            return;
        }
        if (action == "select.provider") {
            state.selected_provider = value;
            state.selected_model.clear();
            state.models = {{"", "Auto (Settings)", ""}};
            refresh_models = !value.empty();
        } else if (action == "select.model") {
            state.selected_model = std::move(value);
        } else if (action == "select.agent") {
            state.selected_agent = std::move(value);
        } else if (action == "select.approval") {
            state.selected_approval = std::move(value);
        } else if (action == "select.mode") {
            state.selected_mode = std::move(value);
        } else if (action == "select.workflow") {
            state.selected_workflow = std::move(value);
        } else if (action == "select.context") {
            state.selected_context = std::move(value);
        }
        state.run_error.clear();
    }
    if (refresh_models) {
        RpcTask task;
        task.kind = RpcTaskKind::RefreshModels;
        {
            std::lock_guard lock(state.mutex);
            task.provider_id = state.selected_provider;
        }
        (void)enqueue_or_report(state, std::move(task), "models.list");
    }
}

void apply_composer_shortcut(AiEditorMainPanelState& state, const json& payload) {
    const std::string shortcut = payload_string(payload, "value");
    const std::string text = prompt_shortcut_text(shortcut);
    if (text.empty()) {
        append_output_line(state, "[error] invalid composer shortcut: " + shortcut);
        return;
    }

    bool run_active = false;
    bool too_large = false;
    {
        std::lock_guard lock(state.mutex);
        run_active = run_is_active(state.run_phase);
        if (!run_active) {
            std::string next = state.composer_text;
            while (!next.empty() &&
                   std::isspace(static_cast<unsigned char>(next.back())) != 0) {
                next.pop_back();
            }
            if (!next.empty())
                next.append("\n\n");
            next.append(text);
            too_large = next.size() > kMaximumComposerBytes;
            if (!too_large) {
                state.composer_text = std::move(next);
                state.run_error.clear();
            }
        }
    }
    if (run_active) {
        append_output_line(state, "[warn] composer shortcuts are locked while a run is active");
    } else if (too_large) {
        append_output_line(state, "[error] composer shortcut would exceed the 48 KiB limit");
    }
}

void SAO_UI_CALL panel_action_callback(const char* action_id_utf8, const uint8_t* payload_json_utf8,
                                       size_t action_arg_len, void* user_data) {
    auto* state = static_cast<AiEditorMainPanelState*>(user_data);
    if (state == nullptr || action_id_utf8 == nullptr)
        return;
    if (require_owner_thread(state->compositor) != SAO_AI_EDITOR_OK)
        return;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting || state->destroy_preflight_active || state->destroy_claimed)
            return;
        ++state->api_calls_in_flight;
    }
    struct LeaseGuard {
        AiEditorMainPanelState* state;
        ~LeaseGuard() {
            std::lock_guard lock(state->mutex);
            --state->api_calls_in_flight;
        }
    } guard{state};

    drain_completions(*state);

    if (action_arg_len > kMaximumActionPayloadBytes ||
        (payload_json_utf8 == nullptr && action_arg_len != 0)) {
        append_output_line(*state, "[error] invalid or oversized action payload");
        (void)refresh_body(*state, true);
        return;
    }
    json payload = json::object();
    if (action_arg_len != 0) {
        payload = json::parse(payload_json_utf8, payload_json_utf8 + action_arg_len, nullptr, false,
                              false);
        if (payload.is_discarded() || !payload.is_object()) {
            append_output_line(*state, "[error] action payload is not a JSON object");
            (void)refresh_body(*state, true);
            return;
        }
    }

    const std::string_view action = action_id_utf8;
    if (action == "control.palette") {
        show_control_palette_dialog(*state);
    } else if (action == "control.advanced.toggle") {
        std::lock_guard lock(state->mutex);
        state->control_developer_advanced = !state->control_developer_advanced;
    } else if (action == "control.group") {
        const std::string group = payload_string(payload, "group");
        std::lock_guard lock(state->mutex);
        state->control_group_filter = group.empty() ? "all" : group;
    } else if (action == "control.open") {
        const ControlActionSpec* canonical = find_control_action(
            payload_string(payload, "id").empty() ? payload_string(payload, "method")
                                                  : payload_string(payload, "id"));
        bool developer_allowed = canonical != nullptr;
        if (canonical == nullptr) {
            append_output_line(*state, "[error] Control Center rejected a non-canonical action.");
        } else {
            {
                std::lock_guard lock(state->mutex);
                developer_allowed = !canonical->advanced || state->control_developer_advanced;
                if (!developer_allowed)
                    state->run_error = "Developer Advanced is required for this Control Center action.";
            }
            if (developer_allowed)
                show_control_action_dialog(*state, payload);
        }
    } else if (action == "control.tool.call") {
        const ControlActionSpec* canonical = find_control_action("tools.call");
        bool allowed = canonical != nullptr;
        if (!allowed)
            append_output_line(*state, "[error] Control Center catalog is missing tools.call.");
        else
            show_control_action_dialog(*state, json{{"id", canonical->id}, {"method", canonical->method},
                                                    {"name", payload_string(payload, "name")},
                                                    {"arguments", payload.value("arguments", json::object())}});
    } else if (action == "control.retry") {
        std::string method;
        std::string tool_name;
        json display_params = json::object();
        json execution_params = json::object();
        json tool_arguments = json::object();
        {
            std::lock_guard lock(state->mutex);
            method = state->control_last_method;
            tool_name = state->control_last_tool_name;
            display_params = state->control_last_params;
            execution_params = state->control_last_execution_params;
            tool_arguments = state->control_last_tool_arguments;
        }
        const ControlActionSpec* canonical = find_control_action(method);
        if (canonical == nullptr) {
            append_output_line(*state, "[error] Control Center retry rejected a non-canonical method.");
        } else {
            bool developer_allowed = true;
            {
                std::lock_guard lock(state->mutex);
                developer_allowed = !canonical->advanced || state->control_developer_advanced;
            }
            if (!developer_allowed) {
                append_output_line(*state, "[error] Developer Advanced is required for this retry.");
            } else {
                show_control_action_dialog(*state, json{{"id", canonical->id},
                                                        {"method", canonical->method},
                                                        {"default", control_default_json_text(display_params)},
                                                        {"executionParams", execution_params},
                                                        {"name", tool_name},
                                                        {"arguments", tool_arguments}});
            }
        }
    } else if (action == "control.copy") {
        std::string result;
        {
            std::lock_guard lock(state->mutex);
            result = state->control_last_result;
        }
        show_copy_text_dialog(*state, "Copy Control Center result", std::move(result));
    } else if (action == "control.use_agent") {
        const std::string id = payload_string(payload, "id");
        {
            std::lock_guard lock(state->mutex);
            state->selected_agent = id;
            state->run_error.clear();
        }
        append_output_line(*state, "[control] agent selected for Composer: " + id);
    } else if (action == "control.use_prompt") {
        const std::string id = payload_string(payload, "id");
        {
            std::lock_guard lock(state->mutex);
            state->composer_text = "Use prompt " + id + " in the next Composer request.";
            state->run_error.clear();
        }
        append_output_line(*state, "[control] prompt selected for Composer: " + id);
    } else if (action == "control.unavailable") {
        append_output_line(*state, "[warn] lifecycle control is owned by the launcher and is not an RPC action");
    } else if (action == "output.clear") {
        std::lock_guard lock(state->mutex);
        state->output_text.clear();
    } else if (action == "diagnostics.copy") {
        std::string output;
        {
            std::lock_guard lock(state->mutex);
            output = state->output_text;
        }
        show_copy_text_dialog(*state, "Copy diagnostics", std::move(output));
    } else if (action == "diagnostics.filter") {
        const std::string value = payload_string(payload, "value");
        std::lock_guard lock(state->mutex);
        state->diagnostic_filter = value == "errors" ? "errors" : "all";
    } else if (action == "diagnostics.auto_scroll") {
        std::lock_guard lock(state->mutex);
        state->diagnostics_auto_scroll = !state->diagnostics_auto_scroll;
    } else if (action == "ai.ping") {
        queue_generic_action(*state, "ping");
    } else if (action == "ai.hello") {
        queue_generic_action(*state, "hello");
    } else if (action == "chat.new") {
        begin_new_conversation(*state);
    } else if (action == "chat.send") {
        send_composer(*state);
    } else if (action == "chat.stop") {
        stop_active_run(*state);
    } else if (action == "composer.edit") {
        show_composer_dialog(*state);
    } else if (action == "composer.changed") {
        std::string value = payload_string(payload, "text");
        if (value.empty() && payload.contains("value"))
            value = payload_string(payload, "value");
        if (value.size() > kMaximumComposerBytes) {
            append_output_line(*state, "[error] composer text exceeds the 48 KiB limit");
        } else {
            std::lock_guard lock(state->mutex);
            state->composer_text = std::move(value);
            state->run_error.clear();
        }
    } else if (action == "composer.shortcut") {
        apply_composer_shortcut(*state, payload);
    } else if (action == "composer.clear") {
        std::lock_guard lock(state->mutex);
        state->composer_text.clear();
    } else if (action == "history.list" || action == "history.refresh") {
        queue_history_refresh(*state);
    } else if (action == "history.search") {
        if (payload.contains("query"))
            search_history(*state, payload_string(payload, "query"));
        else
            show_history_search_dialog(*state);
    } else if (action == "history.clear_search") {
        search_history(*state, {});
    } else if (action == "history.load") {
        load_history_conversation(*state, payload_string(payload, "id"));
    } else if (action == "history.delete") {
        delete_history_conversation(*state, payload_string(payload, "id"));
    } else if (action == "history.load_more") {
        bool can_load_more = false;
        {
            std::lock_guard lock(state->mutex);
            can_load_more = state->history_limit < 500U;
            if (can_load_more)
                state->history_limit = std::min<size_t>(state->history_limit + kMaximumHistoryEntries, 500U);
        }
        if (can_load_more)
            queue_history_refresh(*state);
    } else if (action == "history.undo") {
        HistoryEntry entry;
        bool restore = false;
        {
            std::lock_guard lock(state->mutex);
            if (state->undo_entry.has_value() && state->undo_entry->content_available &&
                std::chrono::steady_clock::now() < state->undo_deadline) {
                entry = *state->undo_entry;
                state->undo_entry.reset();
                restore = true;
            } else if (state->undo_entry.has_value() && !state->undo_entry->content_available) {
                state->run_error = "Undo content is unavailable; no empty conversation was created.";
            }
        }
        if (restore) {
            RpcTask task;
            task.kind = RpcTaskKind::RestoreConversation;
            task.restore_entry = std::move(entry);
            {
                std::lock_guard lock(state->mutex);
                task.history_limit = state->history_limit;
                task.query = state->history_query;
            }
            if (!claim_history_operation(*state, task) ||
                !enqueue_history_operation(*state, std::move(task), "conversation.create")) {
                std::lock_guard lock(state->mutex);
                state->run_error = "Undo restore could not be queued.";
            }
        }
    } else if (action == "history.duplicate") {
        duplicate_history_conversation(*state, payload_string(payload, "id"),
                                       payload_string(payload, "title"),
                                       payload_string(payload, "scope"));
    } else if (action == "theme.select") {
        const std::string value = payload_string(payload, "value");
        if (payload.size() != 1U || (value != "dark" && value != "light")) {
            append_output_line(*state,
                               "[error] theme.select requires dark or light.");
        } else {
            const SaoUiThemeId theme =
                value == "light" ? SAO_UI_THEME_LIGHT : SAO_UI_THEME_DARK;
            const sao_status_t theme_status = sao_ui_theme_set_active_id(theme);
            if (theme_status != SAO_STATUS_OK) {
                append_output_line(*state, "[error] theme.select failed: " +
                                               std::to_string(theme_status));
            }
        }
    } else if (action == "settings.open") {
        show_settings_panel(*state);
    } else if (action == "gpu.hunt") {
        show_gpu_hunt_panel(*state);
    } else if (action == "platform.mcp_management.open") {
        RpcTask task;
        task.kind = RpcTaskKind::Generic;
        task.method = "vscode.window.revealWebviewPanel";
        task.params = {{"panelId", "mcp-management-builtin"},
                       {"viewColumn", 1},
                       {"preserveFocus", false}};
        (void)enqueue_or_report(*state, std::move(task), "vscode.window.revealWebviewPanel");
    } else if (action == "platform.kernel_map.open") {
        RpcTask task;
        task.kind = RpcTaskKind::Generic;
        task.method = "vscode.window.revealWebviewPanel";
        task.params = {{"panelId", "kernel-map-builtin"},
                       {"viewColumn", 1},
                       {"preserveFocus", false}};
        (void)enqueue_or_report(*state, std::move(task), "vscode.window.revealWebviewPanel");
    } else if (action == "diagnostics.refresh") {
        RpcTask task;
        task.kind = RpcTaskKind::Bootstrap;
        bool should_queue = false;
        {
            std::lock_guard lock(state->mutex);
            if (!state->bootstrap_pending) {
                state->bootstrap_pending = true;
                state->next_bootstrap_attempt = std::chrono::steady_clock::time_point{};
                should_queue = true;
            }
        }
        if (should_queue &&
            !enqueue_or_report(*state, std::move(task), "runtime.initialize")) {
            std::lock_guard lock(state->mutex);
            state->bootstrap_pending = false;
            state->next_bootstrap_attempt =
                std::chrono::steady_clock::now() + kBootstrapRetryInterval;
        }
    } else if (action == "view.select") {
        const auto view_field = payload.find("view");
        if (payload.size() != 1U || view_field == payload.end() || !view_field->is_string()) {
            append_output_line(*state, "[error] view.select rejected payload: expected exactly one string field named view.");
        } else {
            const std::string requested_view = view_field->get<std::string>();
            if (!valid_view(requested_view)) {
                append_output_line(*state, "[error] view.select rejected unknown sidebar view: " + requested_view);
            } else {
                std::lock_guard lock(state->mutex);
                state->selected_view = requested_view;
            }
        }
    } else if (action.starts_with("select.")) {
        select_value(*state, action, payload_string(payload, "value"));
    } else {
        append_output_line(*state, "[warn] unknown action id: " + std::string(action));
    }
    (void)refresh_body(*state, true);
}

void SAO_UI_CALL panel_event_callback(int32_t event_kind, void* user_data) {
    auto* state = static_cast<AiEditorMainPanelState*>(user_data);
    if (state == nullptr || require_owner_thread(state->compositor) != SAO_AI_EDITOR_OK)
        return;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting || state->destroy_preflight_active || state->destroy_claimed)
            return;
        ++state->api_calls_in_flight;
    }
    struct LeaseGuard {
        AiEditorMainPanelState* state;
        ~LeaseGuard() {
            std::lock_guard lock(state->mutex);
            --state->api_calls_in_flight;
        }
    } guard{state};

    if (event_kind == SAO_UI_PANEL_EVENT_CLOSE) {
        const sao_status_t hide_status = sao_ui_panel_hide(state->panel);
        if (hide_status != SAO_STATUS_OK) {
            append_output_line(*state, "[error] AI Editor panel close failed: " +
                                          std::to_string(map_ui_status(hide_status)));
            return;
        }
    }
    if (event_kind == SAO_UI_PANEL_EVENT_SHOW || event_kind == SAO_UI_PANEL_EVENT_HIDE ||
        event_kind == SAO_UI_PANEL_EVENT_CLOSE) {
        std::lock_guard lock(state->mutex);
        state->visible = event_kind == SAO_UI_PANEL_EVENT_SHOW;
    }
}

void queue_bootstrap_if_needed(AiEditorMainPanelState& state) {
    RpcTask task;
    {
        std::lock_guard lock(state.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (state.launcher == nullptr || state.bootstrap_loaded || state.bootstrap_pending ||
            !state.accepting || now < state.next_bootstrap_attempt) {
            return;
        }
        state.bootstrap_pending = true;
        task.kind = RpcTaskKind::Bootstrap;
    }
    std::string error;
    if (!queue_rpc_task(state, std::move(task), &error)) {
        {
            std::lock_guard lock(state.mutex);
            state.bootstrap_pending = false;
            state.next_bootstrap_attempt =
                std::chrono::steady_clock::now() + kBootstrapRetryInterval;
        }
        append_output_line(state, "[error] runtime initialization not queued: " + error);
    }
}

void queue_run_poll_if_due(AiEditorMainPanelState& state) {
    RpcTask task;
    {
        std::lock_guard lock(state.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (!state.accepting || state.active_run_id.empty() || state.run_poll_pending ||
            !run_is_active(state.run_phase) || now < state.next_run_poll) {
            return;
        }
        state.run_poll_pending = true;
        task.kind = RpcTaskKind::PollRun;
        task.generation = state.conversation_generation;
        task.run_id = state.active_run_id;
    }
    std::string error;
    if (!queue_rpc_task(state, std::move(task), &error)) {
        {
            std::lock_guard lock(state.mutex);
            state.run_poll_pending = false;
            state.next_run_poll = std::chrono::steady_clock::now() + kRunPollInterval;
        }
        append_output_line(state, "[warn] run.status not queued: " + error);
    }
}

void queue_event_drain_if_due(AiEditorMainPanelState& state) {
    RpcTask task;
    {
        std::lock_guard lock(state.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (!state.accepting || state.active_run_id.empty() || state.event_drain_pending ||
            state.event_drain_unavailable || !run_is_active(state.run_phase) ||
            now < state.next_event_drain) {
            return;
        }
        state.event_drain_pending = true;
        task.kind = RpcTaskKind::DrainEvents;
        task.generation = state.conversation_generation;
        task.run_id = state.active_run_id;
    }
    std::string error;
    if (!queue_rpc_task(state, std::move(task), &error)) {
        {
            std::lock_guard lock(state.mutex);
            state.event_drain_pending = false;
            state.next_event_drain = std::chrono::steady_clock::now() + kEventDrainInterval;
        }
        append_output_line(state, "[warn] events.drain not queued: " + error);
    }
}

int32_t hide_child_panels(AiEditorMainPanelState& state) {
    int32_t first_error = SAO_AI_EDITOR_OK;
    const int32_t dialog_status = hide_active_dialog(state);
    if (dialog_status != SAO_AI_EDITOR_OK)
        first_error = dialog_status;

    sao_ai_editor_settings_panel_t settings_panel = nullptr;
    sao_ai_editor_gpu_hunt_panel_t gpu_hunt_panel = nullptr;
    {
        std::lock_guard lock(state.mutex);
        settings_panel = state.settings_panel;
        gpu_hunt_panel = state.gpu_hunt_panel;
    }
    if (settings_panel != nullptr) {
        const int32_t status = sao_ai_editor_settings_panel_hide(settings_panel);
        if (status == SAO_AI_EDITOR_ERR_HANDLE_INVALID) {
            std::lock_guard lock(state.mutex);
            if (state.settings_panel == settings_panel)
                state.settings_panel = nullptr;
        } else if (status != SAO_AI_EDITOR_OK && first_error == SAO_AI_EDITOR_OK) {
            first_error = status;
        }
    }
    if (gpu_hunt_panel != nullptr) {
        const int32_t status = sao_ai_editor_gpu_hunt_panel_hide(gpu_hunt_panel);
        if (status == SAO_AI_EDITOR_ERR_HANDLE_INVALID) {
            std::lock_guard lock(state.mutex);
            if (state.gpu_hunt_panel == gpu_hunt_panel)
                state.gpu_hunt_panel = nullptr;
        } else if (status != SAO_AI_EDITOR_OK && first_error == SAO_AI_EDITOR_OK) {
            first_error = status;
        }
    }
    return first_error;
}

void release_destroy_claim(AiEditorMainPanelState& state, bool previous_accepting,
                           bool previous_teardown_failed) noexcept {
    std::lock_guard lock(state.mutex);
    state.destroy_preflight_active = false;
    state.destroy_claimed = false;
    state.accepting = previous_accepting;
    state.teardown_failed = previous_teardown_failed;
    state.worker_cv.notify_all();
}

void release_destroy_preflight(AiEditorMainPanelState& state) noexcept {
    std::lock_guard lock(state.mutex);
    state.destroy_preflight_active = false;
    state.worker_cv.notify_all();
}

int32_t restore_panel_handlers(AiEditorMainPanelState& state, int32_t original_status) {
    bool action_attached = false;
    bool event_attached = false;
    {
        std::lock_guard lock(state.mutex);
        action_attached = state.action_handler_attached;
        event_attached = state.event_handler_attached;
    }

    sao_status_t restore_status = SAO_STATUS_OK;
    if (!action_attached) {
        restore_status =
            sao_ui_panel_set_action_handler(state.panel, &panel_action_callback, &state);
        action_attached = restore_status == SAO_STATUS_OK;
    }
    if (!event_attached) {
        const sao_status_t event_status =
            sao_ui_panel_set_event_handler(state.panel, &panel_event_callback, &state);
        if (restore_status == SAO_STATUS_OK)
            restore_status = event_status;
        event_attached = event_status == SAO_STATUS_OK;
    }

    {
        std::lock_guard lock(state.mutex);
        state.action_handler_attached = action_attached;
        state.event_handler_attached = event_attached;
        state.accepting = action_attached && event_attached;
        state.teardown_failed = !state.accepting;
        state.destroy_preflight_active = false;
        state.destroy_claimed = false;
        state.worker_cv.notify_all();
    }
    return restore_status == SAO_STATUS_OK ? original_status : map_ui_status(restore_status);
}

class ApiLease final {
  public:
    explicit ApiLease(sao_ai_editor_main_panel_t handle,
                      bool owner_thread_required = false) {
        {
            std::lock_guard registry_lock(registry_mutex());
            const auto found = registry().find(handle);
            if (found == registry().end())
                return;
            state_ = found->second.get();
            std::lock_guard state_lock(state_->mutex);
            if (!state_->accepting || state_->destroy_preflight_active ||
                state_->destroy_claimed) {
                status_ = SAO_AI_EDITOR_ERR_BUSY;
                state_ = nullptr;
                return;
            }
            ++state_->api_calls_in_flight;
            acquired_ = true;
        }
        if (owner_thread_required) {
#if defined(SAO_AI_EDITOR_TESTING)
            pause_owner_preflight_for_testing(kTestPauseApiLeasePreflight);
#endif
            status_ = require_owner_thread(state_->compositor);
            if (status_ != SAO_AI_EDITOR_OK) {
                release();
                return;
            }
        }
        status_ = SAO_AI_EDITOR_OK;
    }
    ~ApiLease() {
        release();
    }
    void release() noexcept {
        if (!acquired_)
            return;
        AiEditorMainPanelState* const state = state_;
        {
            std::lock_guard lock(state->mutex);
            --state->api_calls_in_flight;
            acquired_ = false;
            state->worker_cv.notify_all();
        }
        state_ = nullptr;
    }
    ApiLease(const ApiLease&) = delete;
    ApiLease& operator=(const ApiLease&) = delete;
    explicit operator bool() const noexcept {
        return acquired_;
    }
    int32_t status() const noexcept {
        return status_;
    }
    AiEditorMainPanelState& state() noexcept {
        return *state_;
    }

  private:
    AiEditorMainPanelState* state_ = nullptr;
    bool acquired_{};
    int32_t status_{SAO_AI_EDITOR_ERR_HANDLE_INVALID};
};

} // namespace

#if defined(SAO_AI_EDITOR_TESTING)
extern "C" void SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_test_set_owner_preflight_pause(int32_t target) {
    if (target == kTestPauseDestroyPreflight)
        g_test_destroy_registry_probe_completed.store(false, std::memory_order_release);
    g_test_owner_preflight_pause_target.store(target, std::memory_order_release);
}

extern "C" int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_test_owner_preflight_waiting_target(void) {
    return g_test_owner_preflight_waiting_target.load(std::memory_order_acquire);
}

extern "C" bool SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_test_destroy_registry_probe_completed(void) {
    return g_test_destroy_registry_probe_completed.load(std::memory_order_acquire);
}
#endif

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_create(
    sao_ui_compositor_handle_t borrowed_compositor, sao_ai_editor_launcher_t borrowed_launcher,
    sao_ai_editor_main_panel_t* out_panel) {
    if (out_panel == nullptr || borrowed_compositor == nullptr)
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    *out_panel = nullptr;
    const int32_t owner_status = require_owner_thread(borrowed_compositor);
    if (owner_status != SAO_AI_EDITOR_OK)
        return owner_status;
    try {
        auto state = std::make_unique<AiEditorMainPanelState>();
        AiEditorMainPanelState* raw_state = state.get();
        state->compositor = borrowed_compositor;
        state->launcher = borrowed_launcher;
        state->backend_status = borrowed_launcher == nullptr
                                    ? "Offline / 离线: backend not attached (headless UI mode)."
                                    : "Waiting for asynchronous runtime initialization...";

        SaoPanelDescriptor descriptor{};
        descriptor.struct_size = sizeof(SaoPanelDescriptor);
        descriptor.panel_id_utf8 = SAO_AI_EDITOR_MAIN_PANEL_ID;
        descriptor.title_utf8 = "AI Editor";
        descriptor.anchor = SAO_UI_PANEL_ANCHOR_CENTER;
        descriptor.default_width_px = kDefaultPanelWidth;
        descriptor.default_height_px = 760;
        descriptor.min_width_px = kDefaultPanelMinWidth;
        descriptor.min_height_px = 520;
        descriptor.movable = true;
        descriptor.resizable = true;
        descriptor.show_titlebar = true;
        descriptor.show_close_button = true;
        descriptor.visible = false;
        descriptor.remember_geometry = true;
        descriptor.auto_scroll = true;
        descriptor.z_class = SAO_UI_PANEL_Z_NORMAL;
        descriptor.initial_opacity = 1.0F;

        sao_status_t status =
            sao_ui_panel_register(borrowed_compositor, &descriptor, &state->panel, &state->body);
        if (status != SAO_STATUS_OK)
            return map_ui_status(status);
        status = sao_ui_panel_set_action_handler(state->panel, &panel_action_callback, state.get());
        if (status == SAO_STATUS_OK)
            state->action_handler_attached = true;
        if (status == SAO_STATUS_OK) {
            status = sao_ui_panel_set_event_handler(state->panel, &panel_event_callback, state.get());
            if (status == SAO_STATUS_OK)
                state->event_handler_attached = true;
        }
        if (status == SAO_STATUS_OK)
            status = refresh_body(*state, true);
        if (status != SAO_STATUS_OK) {
            (void)sao_ui_panel_set_event_handler(state->panel, nullptr, nullptr);
            (void)sao_ui_panel_set_action_handler(state->panel, nullptr, nullptr);
            (void)sao_ui_panel_unregister(state->panel);
            return map_ui_status(status);
        }
        try {
            state->worker = std::thread(&rpc_worker_main, state.get());
        } catch (...) {
            (void)sao_ui_panel_set_event_handler(state->panel, nullptr, nullptr);
            (void)sao_ui_panel_set_action_handler(state->panel, nullptr, nullptr);
            (void)sao_ui_panel_unregister(state->panel);
            throw;
        }
        const sao_ai_editor_main_panel_t handle = allocate_handle();
        try {
            std::lock_guard lock(registry_mutex());
            const auto [slot, inserted] = registry().try_emplace(handle);
            if (!inserted)
                throw std::runtime_error("AI Editor panel handle collision");
            slot->second = std::move(state);
        } catch (...) {
            (void)sao_ui_panel_set_event_handler(raw_state->panel, nullptr, nullptr);
            (void)sao_ui_panel_set_action_handler(raw_state->panel, nullptr, nullptr);
            (void)sao_ui_panel_unregister(raw_state->panel);
            throw;
        }
        *out_panel = handle;
        return SAO_AI_EDITOR_OK;
    } catch (const std::bad_alloc&) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_show(sao_ai_editor_main_panel_t panel) {
    ApiLease lease(panel, true);
    if (!lease)
        return lease.status();
    sao_status_t status = sao_ui_panel_show(lease.state().panel);
    if (status == SAO_STATUS_OK)
        status = sao_ui_panel_bring_to_front(lease.state().panel);
    if (status == SAO_STATUS_OK) {
        {
            std::lock_guard lock(lease.state().mutex);
            lease.state().visible = true;
        }
        queue_bootstrap_if_needed(lease.state());
    }
    return map_ui_status(status);
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_hide(sao_ai_editor_main_panel_t panel) {
    ApiLease lease(panel, true);
    if (!lease)
        return lease.status();
    const int32_t child_status = hide_child_panels(lease.state());
    if (child_status != SAO_AI_EDITOR_OK)
        return child_status;
    const sao_status_t status = sao_ui_panel_hide(lease.state().panel);
    if (status == SAO_STATUS_OK) {
        std::lock_guard lock(lease.state().mutex);
        lease.state().visible = false;
    }
    return map_ui_status(status);
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_tick(sao_ai_editor_main_panel_t panel) {
    ApiLease lease(panel, true);
    if (!lease)
        return lease.status();
    const int32_t dialog_status = tick_dialog(lease.state());
    if (dialog_status != SAO_AI_EDITOR_OK)
        return dialog_status;
    drain_completions(lease.state());
    queue_bootstrap_if_needed(lease.state());
    queue_event_drain_if_due(lease.state());
    queue_run_poll_if_due(lease.state());
    const sao_status_t refresh_status = refresh_body(lease.state(), false);
    if (refresh_status != SAO_STATUS_OK)
        return map_ui_status(refresh_status);
    sao_ai_editor_settings_panel_t settings_panel = nullptr;
    sao_ai_editor_gpu_hunt_panel_t gpu_hunt_panel = nullptr;
    {
        std::lock_guard lock(lease.state().mutex);
        settings_panel = lease.state().settings_panel;
        gpu_hunt_panel = lease.state().gpu_hunt_panel;
    }
    if (settings_panel != nullptr) {
        const int32_t settings_status = sao_ai_editor_settings_panel_tick(settings_panel);
        if (settings_status == SAO_AI_EDITOR_ERR_HANDLE_INVALID) {
            std::lock_guard lock(lease.state().mutex);
            if (lease.state().settings_panel == settings_panel)
                lease.state().settings_panel = nullptr;
        } else if (settings_status != SAO_AI_EDITOR_OK) {
            return settings_status;
        }
    }
    if (gpu_hunt_panel == nullptr)
        return SAO_AI_EDITOR_OK;
    const int32_t tick_status = sao_ai_editor_gpu_hunt_panel_tick(gpu_hunt_panel);
    if (tick_status == SAO_AI_EDITOR_ERR_HANDLE_INVALID) {
        std::lock_guard lock(lease.state().mutex);
        if (lease.state().gpu_hunt_panel == gpu_hunt_panel)
            lease.state().gpu_hunt_panel = nullptr;
        return SAO_AI_EDITOR_OK;
    }
    return tick_status;
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_try_destroy(sao_ai_editor_main_panel_t panel) {
    AiEditorMainPanelState* state = nullptr;
    bool previous_accepting = false;
    bool previous_teardown_failed = false;
    std::unique_ptr<AiEditorMainPanelState> owned;
    {
        std::lock_guard registry_lock(registry_mutex());
        const auto found = registry().find(panel);
        if (found == registry().end())
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        state = found->second.get();
        std::lock_guard state_lock(state->mutex);
        if (state->destroy_preflight_active || state->destroy_claimed) {
#if defined(SAO_AI_EDITOR_TESTING)
            note_destroy_registry_probe_completed_for_testing();
#endif
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (state->api_calls_in_flight != 0 || state->worker_active ||
            !state->rpc_queue.empty() || !state->completed_queue.empty() ||
            run_is_active(state->run_phase)) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        state->destroy_preflight_active = true;
    }

#if defined(SAO_AI_EDITOR_TESTING)
    pause_owner_preflight_for_testing(kTestPauseDestroyPreflight);
#endif
    const int32_t owner_status = require_owner_thread(state->compositor);
    if (owner_status != SAO_AI_EDITOR_OK) {
        release_destroy_preflight(*state);
        return owner_status;
    }

    {
        std::lock_guard registry_lock(registry_mutex());
        const auto found = registry().find(panel);
        if (found == registry().end() || found->second.get() != state) {
            release_destroy_preflight(*state);
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        std::lock_guard state_lock(state->mutex);
        if (!state->destroy_preflight_active || state->destroy_claimed ||
            state->api_calls_in_flight != 0 || state->worker_active ||
            !state->rpc_queue.empty() || !state->completed_queue.empty() ||
            run_is_active(state->run_phase)) {
            state->destroy_preflight_active = false;
            state->worker_cv.notify_all();
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        previous_accepting = state->accepting;
        previous_teardown_failed = state->teardown_failed;
        state->destroy_preflight_active = false;
        state->destroy_claimed = true;
        state->accepting = false;
        state->teardown_failed = false;
    }

    bool action_attached = false;
    bool event_attached = false;
    {
        std::lock_guard state_lock(state->mutex);
        action_attached = state->action_handler_attached;
        event_attached = state->event_handler_attached;
    }
    if (action_attached) {
        const sao_status_t handler_status =
            sao_ui_panel_set_action_handler(state->panel, nullptr, nullptr);
        if (handler_status != SAO_STATUS_OK)
            return restore_panel_handlers(*state, map_ui_status(handler_status));
        std::lock_guard state_lock(state->mutex);
        state->action_handler_attached = false;
    }
    if (event_attached) {
        const sao_status_t handler_status =
            sao_ui_panel_set_event_handler(state->panel, nullptr, nullptr);
        if (handler_status != SAO_STATUS_OK)
            return restore_panel_handlers(*state, map_ui_status(handler_status));
        std::lock_guard state_lock(state->mutex);
        state->event_handler_attached = false;
    }

    const int32_t dialog_status = hide_active_dialog(*state);
    if (dialog_status != SAO_AI_EDITOR_OK)
        return restore_panel_handlers(*state, dialog_status);

    sao_ai_editor_settings_panel_t settings_panel = nullptr;
    sao_ai_editor_gpu_hunt_panel_t gpu_hunt_panel = nullptr;
    {
        std::lock_guard state_lock(state->mutex);
        settings_panel = state->settings_panel;
        gpu_hunt_panel = state->gpu_hunt_panel;
    }
    if (settings_panel != nullptr) {
        const int32_t settings_status = sao_ai_editor_settings_panel_try_destroy(settings_panel);
        if (settings_status != SAO_AI_EDITOR_OK &&
            settings_status != SAO_AI_EDITOR_ERR_HANDLE_INVALID) {
            return restore_panel_handlers(*state, settings_status);
        }
        std::lock_guard state_lock(state->mutex);
        if (state->settings_panel == settings_panel)
            state->settings_panel = nullptr;
    }
    if (gpu_hunt_panel != nullptr) {
        const int32_t gpu_status = sao_ai_editor_gpu_hunt_panel_try_destroy(gpu_hunt_panel);
        if (gpu_status != SAO_AI_EDITOR_OK && gpu_status != SAO_AI_EDITOR_ERR_HANDLE_INVALID)
            return restore_panel_handlers(*state, gpu_status);
        std::lock_guard state_lock(state->mutex);
        if (state->gpu_hunt_panel == gpu_hunt_panel)
            state->gpu_hunt_panel = nullptr;
    }

    const sao_status_t unregister_status = sao_ui_panel_unregister(state->panel);
    if (unregister_status != SAO_STATUS_OK)
        return restore_panel_handlers(*state, map_ui_status(unregister_status));

    {
        std::lock_guard state_lock(state->mutex);
        state->worker_stop_requested = true;
        state->worker_cv.notify_all();
    }
    bool final_claim_valid = true;
    {
        std::lock_guard registry_lock(registry_mutex());
        const auto found = registry().find(panel);
        if (found == registry().end() || found->second.get() != state) {
            final_claim_valid = false;
        } else {
            std::lock_guard state_lock(state->mutex);
            if (!state->destroy_claimed) {
                final_claim_valid = false;
            } else {
                owned = std::move(found->second);
                registry().erase(found);
            }
        }
    }
    if (!final_claim_valid) {
        release_destroy_claim(*state, previous_accepting, previous_teardown_failed);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    // owned destroyed here (outside registry lock).
    return SAO_AI_EDITOR_OK;
}

#if defined(SAO_AI_EDITOR_TESTING)
extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_dispatch_action_for_testing(sao_ai_editor_main_panel_t panel,
                                                     const char* action_id_utf8,
                                                     const uint8_t* payload_json_utf8,
                                                     size_t payload_len) {
    try {
        ApiLease lease(panel, true);
        if (!lease)
            return lease.status();
        if (action_id_utf8 == nullptr)
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        panel_action_callback(action_id_utf8, payload_json_utf8, payload_len, &lease.state());
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_snapshot_json_for_testing(
    sao_ai_editor_main_panel_t panel, char* buffer_utf8, size_t buffer_cap, size_t* out_len) {
    ApiLease lease(panel);
    if (!lease)
        return lease.status();
    if (out_len == nullptr || (buffer_utf8 == nullptr && buffer_cap != 0))
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    json snapshot;
    {
        std::lock_guard state_lock(lease.state().mutex);
        json history = json::array();
        for (const auto& entry : lease.state().history) {
            history.push_back({{"id", entry.id},
                               {"title", entry.title},
                               {"scope", entry.scope},
                               {"messageCount", entry.message_count},
                               {"matchedFields", entry.matched_fields},
                               {"matchCount", entry.match_count},
                               {"searchResult", entry.search_result}});
        }
        json spec = json::parse(lease.state().last_spec, nullptr, false, false);
        if (spec.is_discarded())
            spec = json::object();
        snapshot = {{"conversationId", lease.state().conversation_id},
                    {"conversationTitle", lease.state().conversation_title},
                    {"conversationMessages", lease.state().conversation_messages},
                    {"historyQuery", lease.state().history_query},
                    {"selectedView", lease.state().selected_view},
                    {"historyTotal", lease.state().history_total},
                    {"historyTaskPending", lease.state().history_task_pending},
                    {"visible", lease.state().visible},
                    {"eventHandlerAttached", lease.state().event_handler_attached},
                    {"history", std::move(history)},
                    {"runStatus", lease.state().run_status},
                    {"spec", std::move(spec)}};
    }
    const std::string encoded = snapshot.dump();
    *out_len = encoded.size();
    if (buffer_utf8 == nullptr || buffer_cap == 0)
        return encoded.empty() ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    if (buffer_cap < encoded.size())
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buffer_utf8, encoded.data(), encoded.size());
    if (buffer_cap > encoded.size())
        buffer_utf8[encoded.size()] = '\0';
    return SAO_AI_EDITOR_OK;
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_snapshot_output_for_testing(sao_ai_editor_main_panel_t panel,
                                                     char* buffer_utf8, size_t buffer_cap,
                                                     size_t* out_len) {
    ApiLease lease(panel);
    if (!lease)
        return lease.status();
    if (out_len == nullptr || (buffer_utf8 == nullptr && buffer_cap != 0))
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    std::lock_guard state_lock(lease.state().mutex);
    const std::string& text = lease.state().output_text;
    *out_len = text.size();
    if (buffer_utf8 == nullptr || buffer_cap == 0)
        return text.empty() ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    if (buffer_cap < text.size())
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buffer_utf8, text.data(), text.size());
    if (buffer_cap > text.size())
        buffer_utf8[text.size()] = '\0';
    return SAO_AI_EDITOR_OK;
}
#endif
