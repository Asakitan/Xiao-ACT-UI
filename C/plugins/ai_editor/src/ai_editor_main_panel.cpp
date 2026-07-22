#include "sao/ai_editor/ai_editor_main_panel.h"

#include "sao/ai_editor/ai_editor_ipc.h"
#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/ui/panel_sdk.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

using json = nlohmann::json;

namespace {

constexpr uint32_t kRequestTimeoutMs = 5000U;
constexpr uint32_t kRequestBufferBytes = 64U * 1024U;
constexpr size_t kOutputTrimBytes = 128U * 1024U;

struct AiEditorMainPanelState {
    sao_ui_compositor_handle_t compositor{};
    sao_ai_editor_launcher_t launcher{};
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    std::mutex mutex;
    std::mutex publish_mutex;
    std::string output_text;
    std::string last_spec;
    uint64_t request_counter{};
    size_t api_calls_in_flight{};
    bool accepting{true};
};

std::mutex& registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<sao_ai_editor_main_panel_t, std::unique_ptr<AiEditorMainPanelState>>&
registry() {
    static std::unordered_map<sao_ai_editor_main_panel_t,
                              std::unique_ptr<AiEditorMainPanelState>>
        storage;
    return storage;
}

sao_ai_editor_main_panel_t allocate_handle() noexcept {
    static std::atomic<uintptr_t> next{1};
    const uintptr_t value =
        (next.fetch_add(1, std::memory_order_relaxed) << 4U) | 3U;
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

json text_node(std::string text, std::string_view style = "value",
               int32_t height = 22) {
    return json{{"type", "text"},
                {"text", std::move(text)},
                {"style", style},
                {"height", height}};
}

json button_node(std::string id, std::string label, std::string action,
                 bool disabled = false) {
    json node{{"type", "button"},
              {"id", std::move(id)},
              {"label", std::move(label)},
              {"action", std::move(action)},
              {"height", 28}};
    if (disabled)
        node["disabled"] = true;
    return node;
}

json row_node(json children) {
    return json{{"type", "row"}, {"children", std::move(children)}};
}

std::string build_panel_spec(const AiEditorMainPanelState& state,
                             bool launcher_bound) {
    json nodes = json::array();
    nodes.push_back(text_node("AI Editor", "title", 28));
    nodes.push_back(text_node(launcher_bound
                                  ? "Backend: connected (named-pipe IPC)"
                                  : "Backend: not attached (headless UI test)",
                              launcher_bound ? "subtitle" : "muted", 20));

    nodes.push_back(text_node("Output", "title", 24));
    if (state.output_text.empty()) {
        nodes.push_back(
            text_node("(no responses yet — click Send Ping / Send Hello)",
                      "muted", 22));
    } else {
        nodes.push_back(text_node(state.output_text, "mono", 480));
    }

    nodes.push_back(text_node("Actions", "title", 24));
    json actions = json::array();
    actions.push_back(
        button_node("ai.ping", "Send Ping", "ai.ping", !launcher_bound));
    actions.push_back(
        button_node("ai.hello", "Send Hello", "ai.hello", !launcher_bound));
    actions.push_back(button_node("output.clear", "Clear output",
                                   "output.clear"));
    nodes.push_back(row_node(std::move(actions)));

    json spec = json{{"panel_id", SAO_AI_EDITOR_MAIN_PANEL_ID},
                     {"root", json{{"type", "column"},
                                    {"children", std::move(nodes)}}}};
    return spec.dump();
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
        state.body, reinterpret_cast<const uint8_t*>(spec.data()),
        spec.size());
    if (status != SAO_STATUS_OK)
        return status;
    std::lock_guard lock(state.mutex);
    state.last_spec = std::move(spec);
    return SAO_STATUS_OK;
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

void append_output_line(AiEditorMainPanelState& state, std::string line) {
    std::lock_guard lock(state.mutex);
    if (!state.output_text.empty() && state.output_text.back() != '\n')
        state.output_text.push_back('\n');
    state.output_text.append(std::move(line));
    trim_output(state.output_text);
}

std::string format_iso_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t as_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    // Windows: gmtime_s writes into caller-supplied tm.
    (void)::gmtime_s(&tm, &as_time_t);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer),
                  "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  1900 + tm.tm_year, 1 + tm.tm_mon, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

void send_backend_request(AiEditorMainPanelState& state,
                          std::string_view method) {
    if (state.launcher == nullptr) {
        append_output_line(state,
                            "[error] backend not attached; cannot send \"" +
                                std::string(method) + "\"");
        return;
    }
    uint64_t id = 0;
    {
        std::lock_guard lock(state.mutex);
        id = ++state.request_counter;
    }
    const json request{{"jsonrpc", "2.0"},
                       {"id", id},
                       {"method", std::string(method)},
                       {"params", json::object()}};
    const std::string request_body = request.dump();

    std::string response_buffer(kRequestBufferBytes, '\0');
    uint32_t response_len = 0;
    const int32_t rc = sao_ai_editor_request(
        state.launcher,
        request_body.data(),
        static_cast<uint32_t>(request_body.size()),
        response_buffer.data(),
        static_cast<uint32_t>(response_buffer.size()),
        &response_len,
        kRequestTimeoutMs);
    if (rc != SAO_AI_EDITOR_OK) {
        append_output_line(state,
                            "[" + format_iso_timestamp() + "] " +
                                std::string(method) + " -> error " +
                                std::to_string(rc));
        return;
    }
    response_buffer.resize(response_len);
    append_output_line(state, "[" + format_iso_timestamp() + "] " +
                                   std::string(method) + " -> " +
                                   std::move(response_buffer));
}

void SAO_UI_CALL panel_action_callback(const char* action_id_utf8,
                                        const uint8_t* /*payload_json_utf8*/,
                                        size_t /*action_arg_len*/,
                                        void* user_data) {
    auto* state = static_cast<AiEditorMainPanelState*>(user_data);
    if (state == nullptr || action_id_utf8 == nullptr)
        return;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting)
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

    const std::string_view action = action_id_utf8;
    if (action == "output.clear") {
        std::lock_guard lock(state->mutex);
        state->output_text.clear();
    } else if (action == "ai.ping") {
        send_backend_request(*state, "ping");
    } else if (action == "ai.hello") {
        send_backend_request(*state, "hello");
    } else {
        append_output_line(*state,
                            "[warn] unknown action id: " + std::string(action));
    }
    (void)refresh_body(*state, true);
}

class ApiLease final {
public:
    explicit ApiLease(sao_ai_editor_main_panel_t handle) {
        std::lock_guard lock(registry_mutex());
        const auto found = registry().find(handle);
        if (found == registry().end())
            return;
        state_ = found->second.get();
        std::lock_guard state_lock(state_->mutex);
        if (!state_->accepting) {
            state_ = nullptr;
            return;
        }
        ++state_->api_calls_in_flight;
    }
    ~ApiLease() {
        if (state_ == nullptr)
            return;
        std::lock_guard lock(state_->mutex);
        --state_->api_calls_in_flight;
    }
    ApiLease(const ApiLease&) = delete;
    ApiLease& operator=(const ApiLease&) = delete;
    explicit operator bool() const noexcept { return state_ != nullptr; }
    AiEditorMainPanelState& state() noexcept { return *state_; }

private:
    AiEditorMainPanelState* state_ = nullptr;
};

}  // namespace

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_create(
    sao_ui_compositor_handle_t borrowed_compositor,
    sao_ai_editor_launcher_t borrowed_launcher,
    sao_ai_editor_main_panel_t* out_panel) {
    if (out_panel == nullptr || borrowed_compositor == nullptr)
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    *out_panel = nullptr;
    try {
        auto state = std::make_unique<AiEditorMainPanelState>();
        state->compositor = borrowed_compositor;
        state->launcher = borrowed_launcher;

        SaoPanelDescriptor descriptor{};
        descriptor.panel_id_utf8 = SAO_AI_EDITOR_MAIN_PANEL_ID;
        descriptor.title_utf8 = "AI Editor";
        descriptor.anchor = SAO_UI_PANEL_ANCHOR_CENTER;
        descriptor.default_width_px = 720;
        descriptor.default_height_px = 640;
        descriptor.min_width_px = 480;
        descriptor.min_height_px = 320;
        descriptor.movable = true;
        descriptor.resizable = true;
        descriptor.show_titlebar = true;
        descriptor.show_close_button = true;
        descriptor.visible = false;
        descriptor.auto_scroll = true;
        descriptor.z_class = SAO_UI_PANEL_Z_NORMAL;
        descriptor.initial_opacity = 1.0F;

        sao_status_t status = sao_ui_panel_register(
            borrowed_compositor, &descriptor, &state->panel, &state->body);
        if (status != SAO_STATUS_OK)
            return map_ui_status(status);
        status = sao_ui_panel_set_action_handler(
            state->panel, &panel_action_callback, state.get());
        if (status == SAO_STATUS_OK)
            status = refresh_body(*state, true);
        if (status != SAO_STATUS_OK) {
            (void)sao_ui_panel_set_action_handler(state->panel, nullptr,
                                                   nullptr);
            (void)sao_ui_panel_unregister(state->panel);
            return map_ui_status(status);
        }
        const sao_ai_editor_main_panel_t handle = allocate_handle();
        {
            std::lock_guard lock(registry_mutex());
            registry().emplace(handle, std::move(state));
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
    ApiLease lease(panel);
    if (!lease)
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    const sao_status_t status = sao_ui_panel_show(lease.state().panel);
    return map_ui_status(status);
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_hide(sao_ai_editor_main_panel_t panel) {
    ApiLease lease(panel);
    if (!lease)
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    const sao_status_t status = sao_ui_panel_hide(lease.state().panel);
    return map_ui_status(status);
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_tick(sao_ai_editor_main_panel_t panel) {
    ApiLease lease(panel);
    if (!lease)
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    return map_ui_status(refresh_body(lease.state(), false));
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_try_destroy(sao_ai_editor_main_panel_t panel) {
    std::unique_ptr<AiEditorMainPanelState> owned;
    {
        std::lock_guard registry_lock(registry_mutex());
        const auto found = registry().find(panel);
        if (found == registry().end())
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        AiEditorMainPanelState& state = *found->second;
        {
            std::lock_guard state_lock(state.mutex);
            if (state.api_calls_in_flight != 0)
                return SAO_AI_EDITOR_ERR_BUSY;
            state.accepting = false;
        }
        (void)sao_ui_panel_set_action_handler(state.panel, nullptr, nullptr);
        const sao_status_t unregister_status =
            sao_ui_panel_unregister(state.panel);
        if (unregister_status != SAO_STATUS_OK) {
            std::lock_guard state_lock(state.mutex);
            state.accepting = true;
            return map_ui_status(unregister_status);
        }
        owned = std::move(found->second);
        registry().erase(found);
    }
    // owned destroyed here (outside registry lock).
    return SAO_AI_EDITOR_OK;
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_dispatch_action_for_testing(
    sao_ai_editor_main_panel_t panel, const char* action_id_utf8,
    const uint8_t* payload_json_utf8, size_t payload_len) {
    ApiLease lease(panel);
    if (!lease || action_id_utf8 == nullptr)
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    panel_action_callback(action_id_utf8, payload_json_utf8, payload_len,
                           &lease.state());
    return SAO_AI_EDITOR_OK;
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_snapshot_output_for_testing(
    sao_ai_editor_main_panel_t panel, char* buffer_utf8, size_t buffer_cap,
    size_t* out_len) {
    ApiLease lease(panel);
    if (!lease || out_len == nullptr)
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    std::lock_guard state_lock(lease.state().mutex);
    const std::string& text = lease.state().output_text;
    *out_len = text.size();
    if (buffer_utf8 == nullptr || buffer_cap == 0)
        return text.empty() ? SAO_AI_EDITOR_OK
                            : SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    if (buffer_cap < text.size())
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buffer_utf8, text.data(), text.size());
    if (buffer_cap > text.size())
        buffer_utf8[text.size()] = '\0';
    return SAO_AI_EDITOR_OK;
}
