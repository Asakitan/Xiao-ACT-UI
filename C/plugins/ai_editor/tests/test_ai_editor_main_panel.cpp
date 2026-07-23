// Catch2 tests for ai_editor_main_panel (SDK panel API path). Headless/null
// launcher cases cover compositor lifecycle and explicit offline errors; a
// thread-safe JSON-RPC fixture covers the asynchronous Chat + History flow,
// run.status terminal reload, Stop/run.cancel, and request selection fields.

#include <catch2/catch_test_macros.hpp>

#include "sao/ai_editor/ai_editor_main_panel.h"
#include "sao/ai_editor/ai_editor_settings_panel.h"
#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ai_editor/gpu_hunt_central_panel.h"
#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<uint32_t> ai_editor_request_calls{};
using test_json = nlohmann::json;

enum class MainPanelRpcMode {
    Disabled,
    CompleteRun,
    RunUntilCancelled,
};

struct MainPanelRpcFixture {
    std::mutex mutex;
    std::condition_variable condition;
    MainPanelRpcMode mode{MainPanelRpcMode::Disabled};
    std::vector<test_json> requests;
    std::string last_user_message;
    std::optional<test_json> duplicated_conversation;
    bool cancel_requested{};
    bool hold_history_search{};
    bool history_search_waiting{};
    bool hold_chat_run{};
    bool chat_run_waiting{};
    size_t run_cancel_failures_remaining{};
    bool omit_settings_overrides{};
    bool hold_config_load{};
    bool config_load_waiting{};
    size_t history_refresh_failures_remaining{};
};

MainPanelRpcFixture& main_panel_rpc_fixture() {
    static MainPanelRpcFixture fixture;
    return fixture;
}

test_json conversation_document(std::string id, std::string title,
                                std::string user_message, bool include_assistant) {
    test_json messages = test_json::array();
    if (!user_message.empty())
        messages.push_back({{"role", "user"}, {"content", std::move(user_message)}});
    if (include_assistant)
        messages.push_back({{"role", "assistant"}, {"content", "Fixture assistant reply"}});
    return {{"id", std::move(id)},
            {"title", std::move(title)},
            {"model", "fixture-model"},
            {"scope", "workspace"},
            {"messages", std::move(messages)}};
}

test_json settings_fixture_values() {
    return {{"files", {{"autoSave", "off"}, {"autoSaveDelay", 1000}}}};
}

test_json settings_fixture_description() {
    return {
        {"defaults", settings_fixture_values()},
        {"fields",
         test_json::array({
             {{"key", "files.autoSave"},
              {"section", "files"},
              {"label", "Auto Save"},
              {"type", "enum"},
              {"default", "off"},
              {"options",
               test_json::array({"off", "afterDelay", "onFocusChange", "onWindowChange"})}},
             {{"key", "files.autoSaveDelay"},
              {"section", "files"},
              {"label", "Auto Save Delay"},
              {"type", "integer"},
              {"default", 1000},
              {"min", 0},
              {"dependencies",
               test_json::array({{{"key", "files.autoSave"}, {"equals", "afterDelay"}}})}},
         })},
    };
}

int32_t scripted_main_panel_request(const void* request_data, uint32_t request_len,
                                    void* response_data, uint32_t response_cap,
                                    uint32_t* out_response_len) {
    auto& fixture = main_panel_rpc_fixture();
    std::unique_lock lock(fixture.mutex);
    if (fixture.mode == MainPanelRpcMode::Disabled)
        return SAO_AI_EDITOR_ERR_NOT_RUNNING;
    if (request_data == nullptr || request_len == 0 || out_response_len == nullptr)
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    const auto* first = static_cast<const uint8_t*>(request_data);
    const test_json request =
        test_json::parse(first, first + request_len, nullptr, false, false);
    if (request.is_discarded() || !request.is_object())
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    fixture.requests.push_back(request);

    const std::string method = request.value("method", std::string{});
    if (method == "conversation.search" && fixture.hold_history_search) {
        fixture.history_search_waiting = true;
        fixture.condition.notify_all();
        fixture.condition.wait(lock, [&fixture] { return !fixture.hold_history_search; });
        fixture.history_search_waiting = false;
    }
    if (method == "chat.run" && fixture.hold_chat_run) {
        fixture.chat_run_waiting = true;
        fixture.condition.notify_all();
        fixture.condition.wait(lock, [&fixture] { return !fixture.hold_chat_run; });
        fixture.chat_run_waiting = false;
    }
    if (method == "config.load" && fixture.hold_config_load) {
        fixture.config_load_waiting = true;
        fixture.condition.notify_all();
        fixture.condition.wait(lock, [&fixture] { return !fixture.hold_config_load; });
        fixture.config_load_waiting = false;
    }
    if (method == "run.cancel" && fixture.run_cancel_failures_remaining != 0) {
        --fixture.run_cancel_failures_remaining;
        return SAO_AI_EDITOR_ERR_TIMEOUT;
    }
    if ((method == "conversation.list" || method == "conversation.search") &&
        fixture.history_refresh_failures_remaining != 0) {
        --fixture.history_refresh_failures_remaining;
        return SAO_AI_EDITOR_ERR_TIMEOUT;
    }
    const test_json params = request.value("params", test_json::object());
    test_json result = test_json::object();
    if (method == "runtime.initialize") {
        result = {{"config",
                   {{"ai_editor.provider", "fixture-provider"},
                    {"ai_editor.model", "fixture-model"},
                    {"ai_editor.mode", "agent"}}}};
    } else if (method == "settings.describe") {
        result = settings_fixture_description();
    } else if (method == "settings.load") {
        const test_json values = settings_fixture_values();
        result = {{"scope", params.value("scope", std::string{"workspace"})},
                  {"settings", values},
                  {"effective", values},
                  {"inherited", settings_fixture_values()},
                  {"sources",
                   {{"files",
                     {{"autoSave", "workspace"}, {"autoSaveDelay", "workspace"}}}}}};
                if (!fixture.omit_settings_overrides)
                        result["overrides"] = values;
    } else if (method == "config.load") {
        result = {{"ai_editor", settings_fixture_values()}};
    } else if (method == "settings.save") {
        result = {{"settings", params.value("changes", test_json::object())},
                  {"overrides", params.value("changes", test_json::object())}};
    } else if (method == "providers.list") {
        result = {{"kind", "providers"},
                  {"items",
                   test_json::array({{{"id", "fixture-provider"},
                                      {"name", "Fixture Provider"},
                                      {"models", test_json::array({"fixture-model", "model-b"})}}})},
                  {"total", 1}};
    } else if (method == "models.list") {
        result = {{"providerId", "fixture-provider"},
                  {"models", test_json::array({"fixture-model", "model-b"})},
                  {"total", 2}};
    } else if (method == "agents.list_defs") {
        result = {{"items",
                   test_json::array({{{"id", "fixture-agent"},
                                      {"name", "Fixture Agent"},
                                      {"systemPrompt", "Use fixture agent instructions."}}})},
                  {"total", 1}};
    } else if (method == "workflows.list_defs") {
        result = {{"items",
                   test_json::array({{{"id", "fixture-workflow"},
                                      {"name", "Fixture Workflow"}}})},
                  {"total", 1}};
    } else if (method == "conversation.list") {
        result = test_json::array({{{"id", "history-1"},
                                    {"title", "History Fixture"},
                                    {"model", "fixture-model"},
                                    {"scope", "workspace"},
                                    {"savedAt", 1'700'000'000'000LL},
                                    {"messageCount", 2}}});
        if (fixture.duplicated_conversation.has_value()) {
            const test_json& duplicate = *fixture.duplicated_conversation;
            result.push_back({{"id", duplicate.value("id", std::string{})},
                              {"title", duplicate.value("title", std::string{})},
                              {"model", duplicate.value("model", std::string{})},
                              {"scope", duplicate.value("scope", std::string{})},
                              {"savedAt", duplicate.value("savedAt", int64_t{0})},
                              {"messageCount", duplicate.value("messageCount", size_t{0})}});
        }
    } else if (method == "conversation.search") {
        const std::string query = params.value("query", std::string{});
        test_json results =
            test_json::array({{{"id", "history-1"},
                               {"title", "History Fixture"},
                               {"scope", "workspace"},
                               {"savedAt", 1'700'000'000'000LL},
                               {"matchedFields", test_json::array({"title", "message"})},
                               {"matchCount", 3}}});
        if (fixture.duplicated_conversation.has_value()) {
            const test_json& duplicate = *fixture.duplicated_conversation;
            results.push_back({{"id", duplicate.value("id", std::string{})},
                               {"title", duplicate.value("title", std::string{})},
                               {"scope", duplicate.value("scope", std::string{})},
                               {"savedAt", duplicate.value("savedAt", int64_t{0})},
                               {"matchedFields", test_json::array({"message"})},
                               {"matchCount", 1}});
        }
        const size_t total = results.size();
        result = {{"query", query}, {"results", std::move(results)}, {"total", total}};
    } else if (method == "conversation.create") {
        result = conversation_document("conversation-1", params.value("title", "New Chat"), {},
                                       false);
    } else if (method == "conversation.append") {
        const test_json message = params.value("message", test_json::object());
        fixture.last_user_message = message.value("content", std::string{});
        result = conversation_document(params.value("id", "conversation-1"), "Fixture Chat",
                                       fixture.last_user_message, false);
    } else if (method == "chat.run") {
        fixture.cancel_requested = false;
        result = {{"accepted", true}, {"runId", "run-1"}, {"status", "running"}};
    } else if (method == "run.status") {
        if (fixture.mode == MainPanelRpcMode::RunUntilCancelled &&
            !fixture.cancel_requested) {
            result = {{"runId", "run-1"}, {"status", "running"}};
        } else {
            result = {{"runId", "run-1"},
                      {"status", fixture.cancel_requested ? "cancelled" : "completed"},
                      {"result",
                       {{"content", "Fixture assistant reply"},
                        {"metrics",
                         {{"promptTokens", 4},
                          {"completionTokens", 3},
                          {"totalMs", 12.0}}}}},
                      {"error", ""}};
        }
    } else if (method == "run.cancel") {
        fixture.cancel_requested = true;
        result = {{"runId", "run-1"}, {"cancelRequested", true}};
    } else if (method == "conversation.get") {
        const std::string id = params.value("id", std::string{});
        if (fixture.duplicated_conversation.has_value() &&
            id == fixture.duplicated_conversation->value("id", std::string{})) {
            result = *fixture.duplicated_conversation;
        } else {
        result = id == "history-1"
                     ? conversation_document(id, "History Fixture", "Historical prompt", true)
                     : conversation_document(id, "Fixture Chat", fixture.last_user_message, true);
        }
    } else if (method == "conversation.delete") {
        result = {{"id", params.value("id", std::string{})}, {"deleted", true}};
    } else if (method == "conversation.duplicate") {
        result = conversation_document("history-copy-1", "History Fixture (copy)",
                                       "Historical prompt", true);
        result["savedAt"] = 1'700'000'100'000LL;
        result["messageCount"] = 2;
        result["sourceId"] = params.value("sourceId", std::string{});
        result["duplicatedAt"] = 1'700'000'100'000LL;
        fixture.duplicated_conversation = result;
    } else if (method == "ping" || method == "hello") {
        result = {{"ok", true}};
    } else {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }

    const std::string response =
        test_json{{"jsonrpc", "2.0"}, {"id", request.value("id", 0)}, {"result", result}}.dump();
    *out_response_len = static_cast<uint32_t>(response.size());
    if (response_data == nullptr || response_cap < response.size())
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    std::memcpy(response_data, response.data(), response.size());
    return SAO_AI_EDITOR_OK;
}

} // namespace

extern "C" int32_t SAO_AI_EDITOR_CALL sao_ai_editor_request(sao_ai_editor_launcher_t,
                                                            const void* request_data,
                                                            uint32_t request_len,
                                                            void* response_data,
                                                            uint32_t response_cap,
                                                            uint32_t* out_response_len,
                                                            uint32_t) {
    ai_editor_request_calls.fetch_add(1, std::memory_order_relaxed);
    return scripted_main_panel_request(request_data, request_len, response_data, response_cap,
                                       out_response_len);
}

namespace {

class ScopedMainPanelRpc final {
  public:
    explicit ScopedMainPanelRpc(MainPanelRpcMode mode) {
        auto& fixture = main_panel_rpc_fixture();
        std::lock_guard lock(fixture.mutex);
        fixture.mode = mode;
        fixture.requests.clear();
        fixture.last_user_message.clear();
        fixture.duplicated_conversation.reset();
        fixture.cancel_requested = false;
        fixture.hold_history_search = false;
        fixture.history_search_waiting = false;
        fixture.hold_chat_run = false;
        fixture.chat_run_waiting = false;
        fixture.run_cancel_failures_remaining = 0;
        fixture.omit_settings_overrides = false;
        fixture.hold_config_load = false;
        fixture.config_load_waiting = false;
        fixture.history_refresh_failures_remaining = 0;
    }

    ~ScopedMainPanelRpc() {
        auto& fixture = main_panel_rpc_fixture();
        {
            std::lock_guard lock(fixture.mutex);
            fixture.mode = MainPanelRpcMode::Disabled;
            fixture.requests.clear();
            fixture.last_user_message.clear();
            fixture.duplicated_conversation.reset();
            fixture.cancel_requested = false;
            fixture.hold_history_search = false;
            fixture.history_search_waiting = false;
            fixture.hold_chat_run = false;
            fixture.chat_run_waiting = false;
            fixture.run_cancel_failures_remaining = 0;
            fixture.omit_settings_overrides = false;
            fixture.hold_config_load = false;
            fixture.config_load_waiting = false;
            fixture.history_refresh_failures_remaining = 0;
        }
        fixture.condition.notify_all();
    }
};

class ScopedHistorySearchHold final {
  public:
    ScopedHistorySearchHold() {
        auto& fixture = main_panel_rpc_fixture();
        std::lock_guard lock(fixture.mutex);
        fixture.hold_history_search = true;
        fixture.history_search_waiting = false;
    }

    ~ScopedHistorySearchHold() { release(); }

    bool wait_until_blocked() {
        auto& fixture = main_panel_rpc_fixture();
        std::unique_lock lock(fixture.mutex);
        return fixture.condition.wait_for(
            lock, std::chrono::seconds(2),
            [&fixture] { return fixture.history_search_waiting; });
    }

    void release() {
        if (released_)
            return;
        auto& fixture = main_panel_rpc_fixture();
        {
            std::lock_guard lock(fixture.mutex);
            fixture.hold_history_search = false;
        }
        released_ = true;
        fixture.condition.notify_all();
    }

  private:
    bool released_{};
};

class ScopedChatRunHold final {
  public:
    ScopedChatRunHold() {
        auto& fixture = main_panel_rpc_fixture();
        std::lock_guard lock(fixture.mutex);
        fixture.hold_chat_run = true;
        fixture.chat_run_waiting = false;
    }

    ~ScopedChatRunHold() { release(); }

    bool wait_until_blocked() {
        auto& fixture = main_panel_rpc_fixture();
        std::unique_lock lock(fixture.mutex);
        return fixture.condition.wait_for(
            lock, std::chrono::seconds(2),
            [&fixture] { return fixture.chat_run_waiting; });
    }

    void fail_cancel_attempts(size_t attempts) {
        auto& fixture = main_panel_rpc_fixture();
        std::lock_guard lock(fixture.mutex);
        fixture.run_cancel_failures_remaining = attempts;
    }

    void release() {
        if (released_)
            return;
        auto& fixture = main_panel_rpc_fixture();
        {
            std::lock_guard lock(fixture.mutex);
            fixture.hold_chat_run = false;
        }
        released_ = true;
        fixture.condition.notify_all();
    }

  private:
    bool released_{};
};

class ScopedConfigLoadHold final {
  public:
    ScopedConfigLoadHold() {
        auto& fixture = main_panel_rpc_fixture();
        std::lock_guard lock(fixture.mutex);
        fixture.omit_settings_overrides = true;
        fixture.hold_config_load = true;
        fixture.config_load_waiting = false;
    }

    ~ScopedConfigLoadHold() { release(); }

    bool waiting() const {
        auto& fixture = main_panel_rpc_fixture();
        std::lock_guard lock(fixture.mutex);
        return fixture.config_load_waiting;
    }

    void release() {
        if (released_)
            return;
        auto& fixture = main_panel_rpc_fixture();
        {
            std::lock_guard lock(fixture.mutex);
            fixture.hold_config_load = false;
        }
        released_ = true;
        fixture.condition.notify_all();
    }

  private:
    bool released_{};
};

size_t request_count(std::string_view method) {
    auto& fixture = main_panel_rpc_fixture();
    std::lock_guard lock(fixture.mutex);
    return static_cast<size_t>(std::ranges::count_if(
        fixture.requests, [&](const test_json& request) {
            return request.value("method", std::string{}) == method;
        }));
}

void fail_next_history_refresh() {
    auto& fixture = main_panel_rpc_fixture();
    std::lock_guard lock(fixture.mutex);
    fixture.history_refresh_failures_remaining = 1;
}

std::optional<test_json> latest_request(std::string_view method) {
    auto& fixture = main_panel_rpc_fixture();
    std::lock_guard lock(fixture.mutex);
    for (auto request = fixture.requests.rbegin(); request != fixture.requests.rend(); ++request) {
        if (request->value("method", std::string{}) == method)
            return *request;
    }
    return std::nullopt;
}

bool pump_until_request(sao_ai_editor_main_panel_t panel, std::string_view method,
                        size_t minimum_count = 1, int attempts = 2000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (sao_ai_editor_main_panel_tick(panel) != SAO_AI_EDITOR_OK)
            return false;
        if (request_count(method) >= minimum_count)
            return true;
        Sleep(1);
    }
    return false;
}

int32_t destroy_main_panel_when_idle(sao_ai_editor_main_panel_t panel, int attempts = 2000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const int32_t tick_status = sao_ai_editor_main_panel_tick(panel);
        if (tick_status != SAO_AI_EDITOR_OK)
            return tick_status;
        const int32_t destroy_status = sao_ai_editor_main_panel_try_destroy(panel);
        if (destroy_status != SAO_AI_EDITOR_ERR_BUSY)
            return destroy_status;
        Sleep(1);
    }
    return SAO_AI_EDITOR_ERR_BUSY;
}

int32_t dispatch_json_action(sao_ai_editor_main_panel_t panel, const char* action,
                             const test_json& payload) {
    const std::string encoded = payload.dump();
    return sao_ai_editor_main_panel_dispatch_action_for_testing(
        panel, action, reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
}

    int32_t dispatch_settings_json_action(sao_ai_editor_settings_panel_t panel, const char* action,
                          const test_json& payload) {
        const std::string encoded = payload.dump();
        return sao_ai_editor_settings_panel_dispatch_action_for_testing(
        panel, action, reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
    }

const test_json* find_action_node(const test_json& value, std::string_view action) {
    if (value.is_object()) {
        if (value.value("action", std::string{}) == action)
            return &value;
        for (const auto& [key, child] : value.items()) {
            (void)key;
            if (const test_json* found = find_action_node(child, action); found != nullptr)
                return found;
        }
    } else if (value.is_array()) {
        for (const auto& child : value) {
            if (const test_json* found = find_action_node(child, action); found != nullptr)
                return found;
        }
    }
    return nullptr;
}

sao_ui_compositor_handle_t make_headless_compositor() {
    sao_ui_compositor_handle_t compositor = nullptr;
    const sao_status_t status =
        sao_ui_compositor_create(nullptr, nullptr, &compositor);
    if (status != SAO_STATUS_OK)
        return nullptr;
    return compositor;
}

std::string snapshot_output(sao_ai_editor_main_panel_t panel) {
    size_t needed = 0;
    (void)sao_ai_editor_main_panel_snapshot_output_for_testing(
        panel, nullptr, 0, &needed);
    if (needed == 0)
        return {};
    std::string buffer(needed, '\0');
    (void)sao_ai_editor_main_panel_snapshot_output_for_testing(
        panel, buffer.data(), buffer.size(), &needed);
    return buffer;
}

bool pump_until_output_contains(sao_ai_editor_main_panel_t panel, std::string_view text,
                                int attempts = 2000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (sao_ai_editor_main_panel_tick(panel) != SAO_AI_EDITOR_OK)
            return false;
        if (snapshot_output(panel).find(text) != std::string::npos)
            return true;
        Sleep(1);
    }
    return false;
}

test_json snapshot_main_panel(sao_ai_editor_main_panel_t panel) {
    size_t needed = 0;
    REQUIRE(sao_ai_editor_main_panel_snapshot_json_for_testing(panel, nullptr, 0, &needed) ==
            SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
    std::string buffer(needed, '\0');
    REQUIRE(sao_ai_editor_main_panel_snapshot_json_for_testing(panel, buffer.data(), buffer.size(),
                                                               &needed) == SAO_AI_EDITOR_OK);
    return test_json::parse(buffer);
}

bool pump_until_history_state(sao_ai_editor_main_panel_t panel, std::string_view query,
                              size_t total, std::string_view conversation_id = {},
                              int attempts = 2000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (sao_ai_editor_main_panel_tick(panel) != SAO_AI_EDITOR_OK)
            return false;
        const test_json snapshot = snapshot_main_panel(panel);
        if (!snapshot.value("historyTaskPending", true) &&
            snapshot.value("historyQuery", std::string{}) == query &&
            snapshot.value("historyTotal", size_t{0}) == total &&
            (conversation_id.empty() ||
             snapshot.value("conversationId", std::string{}) == conversation_id)) {
            return true;
        }
        Sleep(1);
    }
    return false;
}

size_t compositor_layer_count(sao_ui_compositor_handle_t compositor) {
    size_t count = 0;
    REQUIRE(sao_ui_compositor_list_layers(compositor, nullptr, 0, &count) == SAO_STATUS_OK);
    return count;
}

sao_ui_panel_handle_t find_main_runtime_panel(
    sao_ui_compositor_handle_t compositor) {
    sao_ui_panel_handle_t runtime_panel = nullptr;
    REQUIRE(sao_ui_panel_find_by_id(compositor, SAO_AI_EDITOR_MAIN_PANEL_ID,
                                    &runtime_panel) == SAO_STATUS_OK);
    REQUIRE(runtime_panel != nullptr);
    return runtime_panel;
}

sao_ui_panel_handle_t find_settings_runtime_panel(
    sao_ui_compositor_handle_t compositor) {
    sao_ui_panel_handle_t runtime_panel = nullptr;
    REQUIRE(sao_ui_panel_find_by_id(compositor, SAO_AI_EDITOR_SETTINGS_PANEL_ID,
                                    &runtime_panel) == SAO_STATUS_OK);
    REQUIRE(runtime_panel != nullptr);
    return runtime_panel;
}

SaoPanelState runtime_panel_state(sao_ui_panel_handle_t panel) {
    SaoPanelState state{};
    REQUIRE(sao_ui_panel_get_state(panel, &state) == SAO_STATUS_OK);
    return state;
}

BOOL CALLBACK collect_process_windows(HWND window, LPARAM parameter) {
    auto& windows = *reinterpret_cast<std::vector<std::uintptr_t>*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == GetCurrentProcessId())
        windows.push_back(reinterpret_cast<std::uintptr_t>(window));
    return TRUE;
}

std::vector<std::uintptr_t> top_level_windows() {
    std::vector<std::uintptr_t> windows;
    REQUIRE(EnumWindows(&collect_process_windows, reinterpret_cast<LPARAM>(&windows)));
    std::ranges::sort(windows);
    return windows;
}

bool wait_for_settings_owner_preflight(int32_t target, int attempts = 2000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (sao_ai_editor_settings_panel_test_owner_preflight_waiting_target() == target)
            return true;
        Sleep(1);
    }
    return false;
}

bool wait_for_main_owner_preflight(int32_t target, int attempts = 2000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (sao_ai_editor_main_panel_test_owner_preflight_waiting_target() == target)
            return true;
        Sleep(1);
    }
    return false;
}

bool wait_for_flag(const std::atomic<bool>& flag, int attempts = 2000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (flag.load(std::memory_order_acquire))
            return true;
        Sleep(1);
    }
    return false;
}

test_json snapshot_settings(sao_ai_editor_settings_panel_t panel) {
    size_t needed = 0;
    REQUIRE(sao_ai_editor_settings_panel_snapshot_json_for_testing(panel, nullptr, 0, &needed) ==
            SAO_AI_EDITOR_OK);
    std::string buffer(needed, '\0');
    REQUIRE(sao_ai_editor_settings_panel_snapshot_json_for_testing(
                panel, buffer.data(), buffer.size(), &needed) == SAO_AI_EDITOR_OK);
    return test_json::parse(buffer);
}

bool pump_settings_until_idle(sao_ai_editor_settings_panel_t panel, int attempts = 2000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (sao_ai_editor_settings_panel_tick(panel) != SAO_AI_EDITOR_OK)
            return false;
        if (snapshot_settings(panel).value("rpc", std::string{}) == "idle")
            return true;
        Sleep(1);
    }
    return false;
}

bool pump_settings_until_loaded(sao_ai_editor_settings_panel_t panel, int attempts = 2000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (sao_ai_editor_settings_panel_tick(panel) != SAO_AI_EDITOR_OK)
            return false;
        const test_json snapshot = snapshot_settings(panel);
        if (snapshot.value("metadataLoaded", false) && snapshot.value("valuesLoaded", false) &&
            snapshot.value("rpc", std::string{}) == "idle") {
            return true;
        }
        Sleep(1);
    }
    return false;
}

bool pump_settings_until_config_load_waiting(sao_ai_editor_settings_panel_t panel,
                                             const ScopedConfigLoadHold& hold,
                                             int attempts = 2000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (sao_ai_editor_settings_panel_tick(panel) != SAO_AI_EDITOR_OK)
            return false;
        if (hold.waiting())
            return true;
        Sleep(1);
    }
    return false;
}

} // namespace

TEST_CASE("ai_editor_main_panel create rejects null args",
          "[ai_editor][main_panel]") {
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(nullptr, nullptr, &panel) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(panel == nullptr);
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, nullptr) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("ai_editor_main_panel create + show + destroy round trip",
          "[ai_editor][main_panel]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(panel != nullptr);
    REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_hide(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_tick(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("ai_editor_main_panel foreign create fails before registration",
          "[ai_editor][main_panel][owner_thread][focused]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel =
        reinterpret_cast<sao_ai_editor_main_panel_t>(
            static_cast<uintptr_t>(0x1));
    int32_t create_status = SAO_AI_EDITOR_OK;

    std::thread foreign([&] {
        create_status =
            sao_ai_editor_main_panel_create(compositor, nullptr, &panel);
    });
    foreign.join();

    CHECK(create_status == SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK(panel == nullptr);
    CHECK(compositor_layer_count(compositor) == 0);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("ai_editor_main_panel foreign UI calls preserve state",
          "[ai_editor][main_panel][owner_thread][focused]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    const sao_ui_panel_handle_t runtime_panel =
        find_main_runtime_panel(compositor);
    REQUIRE_FALSE(runtime_panel_state(runtime_panel).visible);

    std::array<int32_t, 5> hidden_statuses{};
    std::thread hidden_foreign([&] {
        hidden_statuses[0] = sao_ai_editor_main_panel_show(panel);
        hidden_statuses[1] = sao_ai_editor_main_panel_hide(panel);
        hidden_statuses[2] = sao_ai_editor_main_panel_tick(panel);
        hidden_statuses[3] =
            sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "ai.ping", nullptr, 0);
        hidden_statuses[4] = sao_ai_editor_main_panel_try_destroy(panel);
    });
    hidden_foreign.join();
    for (const int32_t status : hidden_statuses)
        CHECK(status == SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK_FALSE(runtime_panel_state(runtime_panel).visible);
    CHECK(snapshot_output(panel).empty());
    CHECK(compositor_layer_count(compositor) == 1);

    REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(runtime_panel_state(runtime_panel).visible);
    std::array<int32_t, 3> visible_statuses{};
    std::thread visible_foreign([&] {
        visible_statuses[0] = sao_ai_editor_main_panel_hide(panel);
        visible_statuses[1] = sao_ai_editor_main_panel_tick(panel);
        visible_statuses[2] =
            sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "ai.hello", nullptr, 0);
    });
    visible_foreign.join();
    for (const int32_t status : visible_statuses)
        CHECK(status == SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK(runtime_panel_state(runtime_panel).visible);
    CHECK(snapshot_output(panel).empty());
    CHECK(compositor_layer_count(compositor) == 1);

    REQUIRE(sao_ai_editor_main_panel_hide(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("ai_editor_main_panel destroy claim pins leases and leaves registry available",
          "[ai_editor][main_panel][owner_thread][teardown][concurrency][focused]") {
    constexpr int32_t kPauseApiLeasePreflight = 1;
    constexpr int32_t kPauseDestroyPreflight = 2;
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) == SAO_AI_EDITOR_OK);
    const sao_ui_panel_handle_t runtime_panel = find_main_runtime_panel(compositor);
    REQUIRE_FALSE(runtime_panel_state(runtime_panel).visible);

    sao_ai_editor_main_panel_test_set_owner_preflight_pause(kPauseApiLeasePreflight);
    std::atomic<int32_t> pinned_foreign_status{SAO_AI_EDITOR_OK};
    std::thread pinned_foreign([&] {
        pinned_foreign_status.store(sao_ai_editor_main_panel_show(panel),
                                    std::memory_order_release);
    });
    const bool api_preflight_paused = wait_for_main_owner_preflight(kPauseApiLeasePreflight);
    std::atomic<int32_t> pinned_destroy_status{SAO_AI_EDITOR_ERR_HANDLE_INVALID};
    std::atomic<bool> pinned_destroy_completed{};
    std::optional<std::thread> pinned_destroy;
    bool pin_blocked_erase = false;
    if (api_preflight_paused) {
        pinned_destroy.emplace([&] {
            pinned_destroy_status.store(sao_ai_editor_main_panel_try_destroy(panel),
                                        std::memory_order_release);
            pinned_destroy_completed.store(true, std::memory_order_release);
        });
        pin_blocked_erase = wait_for_flag(pinned_destroy_completed);
    }
    sao_ai_editor_main_panel_test_set_owner_preflight_pause(0);
    if (pinned_destroy.has_value())
        pinned_destroy->join();
    pinned_foreign.join();

    CHECK(api_preflight_paused);
    CHECK(pin_blocked_erase);
    CHECK(pinned_destroy_status.load(std::memory_order_acquire) == SAO_AI_EDITOR_ERR_BUSY);
    CHECK(pinned_foreign_status.load(std::memory_order_acquire) ==
          SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK_FALSE(runtime_panel_state(runtime_panel).visible);
    CHECK(compositor_layer_count(compositor) == 1);

    sao_ai_editor_main_panel_test_set_owner_preflight_pause(kPauseDestroyPreflight);
    std::atomic<int32_t> foreign_destroy_status{SAO_AI_EDITOR_OK};
    std::thread foreign_destroy([&] {
        foreign_destroy_status.store(sao_ai_editor_main_panel_try_destroy(panel),
                                     std::memory_order_release);
    });
    const bool destroy_preflight_paused =
        wait_for_main_owner_preflight(kPauseDestroyPreflight);
    std::atomic<int32_t> api_during_claim_status{SAO_AI_EDITOR_OK};
    std::atomic<int32_t> destroy_during_claim_status{SAO_AI_EDITOR_OK};
    std::atomic<bool> api_during_claim_completed{};
    std::atomic<bool> destroy_during_claim_completed{};
    std::optional<std::thread> api_during_claim;
    std::optional<std::thread> destroy_during_claim;
    bool registry_available_during_preflight = false;
    bool destroy_registry_probe_completed = false;
    if (destroy_preflight_paused) {
        api_during_claim.emplace([&] {
            api_during_claim_status.store(sao_ai_editor_main_panel_show(panel),
                                          std::memory_order_release);
            api_during_claim_completed.store(true, std::memory_order_release);
        });
        destroy_during_claim.emplace([&] {
            destroy_during_claim_status.store(sao_ai_editor_main_panel_try_destroy(panel),
                                              std::memory_order_release);
            destroy_during_claim_completed.store(true, std::memory_order_release);
        });
        registry_available_during_preflight = wait_for_flag(api_during_claim_completed) &&
                                              wait_for_flag(destroy_during_claim_completed);
        destroy_registry_probe_completed =
            sao_ai_editor_main_panel_test_destroy_registry_probe_completed();
    }
    sao_ai_editor_main_panel_test_set_owner_preflight_pause(0);
    if (api_during_claim.has_value())
        api_during_claim->join();
    if (destroy_during_claim.has_value())
        destroy_during_claim->join();
    foreign_destroy.join();

    CHECK(destroy_preflight_paused);
    CHECK(registry_available_during_preflight);
    CHECK(destroy_registry_probe_completed);
    CHECK(api_during_claim_status.load(std::memory_order_acquire) == SAO_AI_EDITOR_ERR_BUSY);
    CHECK(destroy_during_claim_status.load(std::memory_order_acquire) == SAO_AI_EDITOR_ERR_BUSY);
    CHECK(foreign_destroy_status.load(std::memory_order_acquire) ==
          SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK_FALSE(runtime_panel_state(runtime_panel).visible);
    CHECK(compositor_layer_count(compositor) == 1);

    REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(runtime_panel_state(runtime_panel).visible);
    REQUIRE(sao_ai_editor_main_panel_hide(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("ai_editor_settings_panel foreign create fails before registration",
          "[ai_editor][settings_panel][owner_thread][focused]") {
    const auto windows_before = top_level_windows();
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_settings_panel_t panel =
        reinterpret_cast<sao_ai_editor_settings_panel_t>(static_cast<uintptr_t>(0x1));
    int32_t create_status = SAO_AI_EDITOR_OK;

    std::thread foreign(
        [&] { create_status = sao_ai_editor_settings_panel_create(compositor, nullptr, &panel); });
    foreign.join();

    CHECK(create_status == SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK(panel == nullptr);
    CHECK(compositor_layer_count(compositor) == 0);
    CHECK(top_level_windows() == windows_before);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("ai_editor_settings_panel foreign UI calls preserve shared layer state",
          "[ai_editor][settings_panel][owner_thread][compositor][focused]") {
    const auto windows_before = top_level_windows();
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    REQUIRE(compositor_layer_count(compositor) == 0);
    ai_editor_request_calls.store(0, std::memory_order_relaxed);
    const auto launcher = reinterpret_cast<sao_ai_editor_launcher_t>(static_cast<uintptr_t>(0x1));

    sao_ai_editor_settings_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_settings_panel_create(compositor, launcher, &panel) == SAO_AI_EDITOR_OK);
    REQUIRE(panel != nullptr);
    REQUIRE(compositor_layer_count(compositor) == 1);
    CHECK(top_level_windows() == windows_before);

    const sao_ui_panel_handle_t runtime_panel = find_settings_runtime_panel(compositor);
    REQUIRE_FALSE(runtime_panel_state(runtime_panel).visible);

    std::array<int32_t, 4> hidden_statuses{};
    std::thread hidden_foreign([&] {
        hidden_statuses[0] = sao_ai_editor_settings_panel_show(panel);
        hidden_statuses[1] = sao_ai_editor_settings_panel_hide(panel);
        hidden_statuses[2] = sao_ai_editor_settings_panel_tick(panel);
        hidden_statuses[3] = sao_ai_editor_settings_panel_try_destroy(panel);
    });
    hidden_foreign.join();
    for (const int32_t status : hidden_statuses)
        CHECK(status == SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK(ai_editor_request_calls.load(std::memory_order_relaxed) == 0);
    CHECK_FALSE(runtime_panel_state(runtime_panel).visible);
    CHECK(compositor_layer_count(compositor) == 1);
    CHECK(top_level_windows() == windows_before);

    REQUIRE(pump_settings_until_idle(panel));
    CHECK(ai_editor_request_calls.load(std::memory_order_relaxed) >= 1);

        REQUIRE(dispatch_settings_json_action(panel, "filter.review", {{"value", "modified"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(dispatch_settings_json_action(panel, "filter.advanced", {{"value", "show"}}) ==
            SAO_AI_EDITOR_OK);
        test_json review_snapshot = snapshot_settings(panel);
        CHECK(review_snapshot.value("reviewFilter", std::string{}) == "modified");
        CHECK(review_snapshot.value("showAdvanced", false));
        REQUIRE(dispatch_settings_json_action(panel, "filter.review", {{"value", "all"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(dispatch_settings_json_action(panel, "filter.advanced", {{"value", "hide"}}) ==
            SAO_AI_EDITOR_OK);
        review_snapshot = snapshot_settings(panel);
        CHECK(review_snapshot.value("reviewFilter", std::string{}) == "all");
        CHECK_FALSE(review_snapshot.value("showAdvanced", true));

    REQUIRE(sao_ai_editor_settings_panel_show(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(runtime_panel_state(runtime_panel).visible);
    ai_editor_request_calls.store(0, std::memory_order_relaxed);
    std::array<int32_t, 3> visible_statuses{};
    std::thread visible_foreign([&] {
        visible_statuses[0] = sao_ai_editor_settings_panel_hide(panel);
        visible_statuses[1] = sao_ai_editor_settings_panel_show(panel);
        visible_statuses[2] = sao_ai_editor_settings_panel_tick(panel);
    });
    visible_foreign.join();
    for (const int32_t status : visible_statuses)
        CHECK(status == SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK(ai_editor_request_calls.load(std::memory_order_relaxed) == 0);
    CHECK(runtime_panel_state(runtime_panel).visible);
    CHECK(compositor_layer_count(compositor) == 1);
    CHECK(top_level_windows() == windows_before);

    REQUIRE(pump_settings_until_idle(panel));
    CHECK(ai_editor_request_calls.load(std::memory_order_relaxed) >= 1);

    REQUIRE(sao_ai_editor_settings_panel_dispatch_action_for_testing(
                panel, "search.open", nullptr, 0) == SAO_AI_EDITOR_OK);
    const size_t layers_with_dialog = compositor_layer_count(compositor);
    REQUIRE(snapshot_settings(panel).value("dialogVisible", false));
    std::atomic<int32_t> dialog_foreign_destroy_status{SAO_AI_EDITOR_OK};
    std::thread dialog_foreign_destroy([&] {
        dialog_foreign_destroy_status.store(sao_ai_editor_settings_panel_try_destroy(panel),
                                            std::memory_order_release);
    });
    dialog_foreign_destroy.join();
    CHECK(dialog_foreign_destroy_status.load(std::memory_order_acquire) ==
          SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK(runtime_panel_state(runtime_panel).visible);
    CHECK(snapshot_settings(panel).value("dialogVisible", false));
    CHECK(compositor_layer_count(compositor) == layers_with_dialog);

    REQUIRE(sao_ai_editor_settings_panel_hide(panel) == SAO_AI_EDITOR_OK);
    REQUIRE_FALSE(runtime_panel_state(runtime_panel).visible);
    CHECK_FALSE(snapshot_settings(panel).value("dialogVisible", true));

    constexpr int32_t kPauseApiLeasePreflight = 1;
    constexpr int32_t kPauseDestroyPreflight = 2;
    sao_ai_editor_settings_panel_test_set_owner_preflight_pause(kPauseApiLeasePreflight);
    std::atomic<int32_t> pinned_foreign_status{SAO_AI_EDITOR_OK};
    std::thread pinned_foreign([&] {
        pinned_foreign_status.store(sao_ai_editor_settings_panel_show(panel),
                                    std::memory_order_release);
    });
    const bool api_preflight_paused = wait_for_settings_owner_preflight(kPauseApiLeasePreflight);
    std::atomic<int32_t> busy_status{SAO_AI_EDITOR_ERR_HANDLE_INVALID};
    std::atomic<bool> busy_completed{};
    std::optional<std::thread> busy_probe;
    bool pin_blocked_erase = false;
    if (api_preflight_paused) {
        busy_probe.emplace([&] {
            busy_status.store(sao_ai_editor_settings_panel_try_destroy(panel),
                              std::memory_order_release);
            busy_completed.store(true, std::memory_order_release);
        });
        pin_blocked_erase = wait_for_flag(busy_completed);
    }
    sao_ai_editor_settings_panel_test_set_owner_preflight_pause(0);
    if (busy_probe.has_value())
        busy_probe->join();
    pinned_foreign.join();

    CHECK(api_preflight_paused);
    CHECK(pin_blocked_erase);
    CHECK(busy_status.load(std::memory_order_acquire) == SAO_AI_EDITOR_ERR_BUSY);
    CHECK(pinned_foreign_status.load(std::memory_order_acquire) ==
          SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK_FALSE(runtime_panel_state(runtime_panel).visible);
    CHECK(compositor_layer_count(compositor) == 1);

    sao_ai_editor_settings_panel_test_set_owner_preflight_pause(kPauseDestroyPreflight);
    std::atomic<int32_t> foreign_destroy_status{SAO_AI_EDITOR_OK};
    std::thread foreign_destroy([&] {
        foreign_destroy_status.store(sao_ai_editor_settings_panel_try_destroy(panel),
                                     std::memory_order_release);
    });
    const bool destroy_preflight_paused = wait_for_settings_owner_preflight(kPauseDestroyPreflight);
    std::atomic<int32_t> api_during_claim_status{SAO_AI_EDITOR_OK};
    std::atomic<int32_t> destroy_during_claim_status{SAO_AI_EDITOR_OK};
    std::atomic<bool> api_during_claim_completed{};
    std::atomic<bool> destroy_during_claim_completed{};
    std::optional<std::thread> api_during_claim;
    std::optional<std::thread> destroy_during_claim;
    bool registry_available_during_preflight = false;
    bool destroy_registry_probe_completed = false;
    if (destroy_preflight_paused) {
        api_during_claim.emplace([&] {
            api_during_claim_status.store(sao_ai_editor_settings_panel_show(panel),
                                          std::memory_order_release);
            api_during_claim_completed.store(true, std::memory_order_release);
        });
        destroy_during_claim.emplace([&] {
            destroy_during_claim_status.store(sao_ai_editor_settings_panel_try_destroy(panel),
                                              std::memory_order_release);
            destroy_during_claim_completed.store(true, std::memory_order_release);
        });
        registry_available_during_preflight = wait_for_flag(api_during_claim_completed) &&
                                              wait_for_flag(destroy_during_claim_completed);
        destroy_registry_probe_completed =
            sao_ai_editor_settings_panel_test_destroy_registry_probe_completed();
    }
    sao_ai_editor_settings_panel_test_set_owner_preflight_pause(0);
    if (api_during_claim.has_value())
        api_during_claim->join();
    if (destroy_during_claim.has_value())
        destroy_during_claim->join();
    foreign_destroy.join();

    CHECK(destroy_preflight_paused);
    CHECK(registry_available_during_preflight);
    CHECK(destroy_registry_probe_completed);
    CHECK(api_during_claim_status.load(std::memory_order_acquire) == SAO_AI_EDITOR_ERR_BUSY);
    CHECK(destroy_during_claim_status.load(std::memory_order_acquire) == SAO_AI_EDITOR_ERR_BUSY);
    CHECK(foreign_destroy_status.load(std::memory_order_acquire) ==
          SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    CHECK_FALSE(runtime_panel_state(runtime_panel).visible);
    CHECK(compositor_layer_count(compositor) == 1);

    REQUIRE(sao_ai_editor_settings_panel_show(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(runtime_panel_state(runtime_panel).visible);
    REQUIRE(sao_ai_editor_settings_panel_hide(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_settings_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    CHECK(compositor_layer_count(compositor) == 0);
    CHECK(top_level_windows() == windows_before);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("ai_editor_settings_panel explains and enforces field dependencies",
          "[ai_editor][settings_panel][dependencies][focused]") {
    ScopedMainPanelRpc rpc(MainPanelRpcMode::CompleteRun);
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    const auto launcher = reinterpret_cast<sao_ai_editor_launcher_t>(static_cast<uintptr_t>(0x1));
    sao_ai_editor_settings_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_settings_panel_create(compositor, launcher, &panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_settings_panel_show(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(pump_settings_until_loaded(panel));

    REQUIRE(dispatch_settings_json_action(panel, "nav.page", {{"page", "editor"}}) ==
            SAO_AI_EDITOR_OK);
    test_json snapshot = snapshot_settings(panel);
    const std::string initial_spec = snapshot["spec"].dump();
    CHECK(initial_spec.find("Available when / 满足以下条件后可用") != std::string::npos);
    CHECK(initial_spec.find("files.autoSave = afterDelay (current: off)") != std::string::npos);
    const test_json* delay_edit = find_action_node(snapshot["spec"], "field.edit");
    REQUIRE(delay_edit != nullptr);
    CHECK(delay_edit->value("disabled", false));

    REQUIRE(dispatch_settings_json_action(panel, "field.edit",
                                          {{"key", "files.autoSaveDelay"}}) ==
            SAO_AI_EDITOR_OK);
    CHECK_FALSE(snapshot_settings(panel).value("dialogVisible", false));

    REQUIRE(dispatch_settings_json_action(
                panel, "field.select",
                {{"key", "files.autoSave"}, {"value", "afterDelay"}}) == SAO_AI_EDITOR_OK);
    snapshot = snapshot_settings(panel);
    const std::string enabled_spec = snapshot["spec"].dump();
    CHECK(enabled_spec.find("Condition met / 条件已满足") != std::string::npos);
    CHECK(enabled_spec.find("files.autoSave = afterDelay (current: afterDelay)") !=
          std::string::npos);
    delay_edit = find_action_node(snapshot["spec"], "field.edit");
    REQUIRE(delay_edit != nullptr);
    CHECK_FALSE(delay_edit->value("disabled", false));

    REQUIRE(dispatch_settings_json_action(panel, "field.edit", {{"key", "files.autoSaveDelay"}}) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(snapshot_settings(panel).value("dialogVisible", false));
    REQUIRE(dispatch_settings_json_action(panel, "field.select",
                                          {{"key", "files.autoSave"}, {"value", "off"}}) ==
            SAO_AI_EDITOR_OK);
    constexpr std::string_view changed_delay = "2500";
    REQUIRE(sao_ai_editor_settings_panel_submit_dialog_for_testing(
                panel, changed_delay.data(), changed_delay.size()) == SAO_AI_EDITOR_OK);
    snapshot = snapshot_settings(panel);
    CHECK(snapshot["draft"]["files"]["autoSaveDelay"] == 1000);
    CHECK(snapshot["spec"].dump().find("Setting dependency is no longer satisfied") !=
          std::string::npos);

    REQUIRE(dispatch_settings_json_action(panel, "field.select",
                                          {{"key", "files.autoSave"}, {"value", "afterDelay"}}) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(dispatch_settings_json_action(panel, "field.edit", {{"key", "files.autoSaveDelay"}}) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_settings_panel_submit_dialog_for_testing(
                panel, changed_delay.data(), changed_delay.size()) == SAO_AI_EDITOR_OK);
    snapshot = snapshot_settings(panel);
    CHECK(snapshot["draft"]["files"]["autoSaveDelay"] == 2500);

    REQUIRE(dispatch_settings_json_action(panel, "field.toggle",
                                          {{"key", "files.autoSaveDelay"}}) == SAO_AI_EDITOR_OK);
    snapshot = snapshot_settings(panel);
    CHECK(snapshot["draft"]["files"]["autoSaveDelay"] == 2500);
    CHECK(snapshot["spec"].dump().find("Setting action does not match field type") !=
          std::string::npos);

    REQUIRE(dispatch_settings_json_action(panel, "field.select",
                                          {{"key", "files.autoSave"}, {"value", "off"}}) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_settings_panel_dispatch_action_for_testing(
                panel, "draft.reset_section", nullptr, 0) == SAO_AI_EDITOR_OK);
    snapshot = snapshot_settings(panel);
    CHECK(snapshot["draft"]["files"]["autoSaveDelay"] == 2500);
    CHECK(snapshot["spec"].dump().find("Skipped 1 unavailable fields") != std::string::npos);

    REQUIRE(sao_ai_editor_settings_panel_hide(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_settings_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("ai_editor_settings_panel locks draft changes during raw follow-up loading",
          "[ai_editor][settings_panel][followup][focused]") {
    ScopedMainPanelRpc rpc(MainPanelRpcMode::CompleteRun);
    ScopedConfigLoadHold config_hold;
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    const auto launcher = reinterpret_cast<sao_ai_editor_launcher_t>(static_cast<uintptr_t>(0x1));
    sao_ai_editor_settings_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_settings_panel_create(compositor, launcher, &panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_settings_panel_show(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(pump_settings_until_config_load_waiting(panel, config_hold));

    test_json snapshot = snapshot_settings(panel);
    REQUIRE(snapshot.value("valuesLoaded", false));
    CHECK(snapshot["draft"]["files"]["autoSave"] == "off");
    REQUIRE(dispatch_settings_json_action(
                panel, "field.select",
                {{"key", "files.autoSave"}, {"value", "afterDelay"}}) == SAO_AI_EDITOR_OK);
    snapshot = snapshot_settings(panel);
    CHECK(snapshot["draft"]["files"]["autoSave"] == "off");
    CHECK(snapshot["spec"].dump().find("Settings are synchronizing") != std::string::npos);

    REQUIRE(sao_ai_editor_settings_panel_dispatch_action_for_testing(
                panel, "draft.apply", nullptr, 0) == SAO_AI_EDITOR_OK);
    CHECK(request_count("settings.save") == 0);
    config_hold.release();
    REQUIRE(pump_settings_until_loaded(panel));
    snapshot = snapshot_settings(panel);
    CHECK(snapshot["draft"]["files"]["autoSave"] == "off");
    CHECK(request_count("settings.save") == 0);

    REQUIRE(sao_ai_editor_settings_panel_hide(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_settings_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("ai_editor_main_panel destroy rejects unknown handle",
          "[ai_editor][main_panel]") {
    sao_ai_editor_main_panel_t bogus =
        reinterpret_cast<sao_ai_editor_main_panel_t>(
            static_cast<uintptr_t>(0xDEADBEEFu));
    REQUIRE(sao_ai_editor_main_panel_try_destroy(bogus) ==
            SAO_AI_EDITOR_ERR_HANDLE_INVALID);
}

TEST_CASE("ai_editor_main_panel output starts empty",
          "[ai_editor][main_panel]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(snapshot_output(panel).empty());
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("ai_editor_main_panel dispatches ai.ping with backend not attached",
          "[ai_editor][main_panel]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "ai.ping", nullptr, 0) == SAO_AI_EDITOR_OK);
    const std::string output = snapshot_output(panel);
    REQUIRE(output.find("[error] backend not attached") != std::string::npos);
    REQUIRE(output.find("ping") != std::string::npos);
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("ai_editor_main_panel dispatches output.clear",
          "[ai_editor][main_panel]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "ai.hello", nullptr, 0) == SAO_AI_EDITOR_OK);
    REQUIRE_FALSE(snapshot_output(panel).empty());
    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "output.clear", nullptr, 0) == SAO_AI_EDITOR_OK);
    REQUIRE(snapshot_output(panel).empty());
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("ai_editor_main_panel unknown action is recorded as warning",
          "[ai_editor][main_panel]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "unknown.thing", nullptr, 0) == SAO_AI_EDITOR_OK);
    const std::string output = snapshot_output(panel);
    REQUIRE(output.find("unknown action id: unknown.thing") !=
            std::string::npos);
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    sao_ui_compositor_destroy(compositor);
}

    TEST_CASE("ai_editor_main_panel async chat and history use canonical RPC flow",
          "[ai_editor][main_panel][chat][history][focused]") {
        ScopedMainPanelRpc rpc(MainPanelRpcMode::CompleteRun);
        sao_ui_compositor_handle_t compositor = make_headless_compositor();
        REQUIRE(compositor != nullptr);
        const auto launcher = reinterpret_cast<sao_ai_editor_launcher_t>(static_cast<uintptr_t>(0x1));
        sao_ai_editor_main_panel_t panel = nullptr;
        REQUIRE(sao_ai_editor_main_panel_create(compositor, launcher, &panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "conversation.list"));

        REQUIRE(dispatch_json_action(panel, "select.provider", {{"value", "fixture-provider"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(dispatch_json_action(panel, "select.model", {{"value", "model-b"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(dispatch_json_action(panel, "select.mode", {{"value", "plan"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(dispatch_json_action(panel, "select.approval", {{"value", "autopilot"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(dispatch_json_action(panel, "select.agent", {{"value", "fixture-agent"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(dispatch_json_action(panel, "select.workflow", {{"value", "fixture-workflow"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(dispatch_json_action(panel, "select.context", {{"value", "current_turn"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(dispatch_json_action(panel, "composer.changed",
                                     {{"text", "Hello from composer   "}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(dispatch_json_action(panel, "composer.shortcut", {{"value", "fix"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(panel, "chat.send", nullptr, 0) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "chat.run"));

        const auto chat_request = latest_request("chat.run");
        REQUIRE(chat_request.has_value());
        const test_json chat_params = chat_request->at("params");
        CHECK(chat_params.value("providerId", std::string{}) == "fixture-provider");
        CHECK(chat_params.value("model", std::string{}) == "model-b");
        CHECK(chat_params.value("mode", std::string{}) == "plan");
        CHECK(chat_params.value("approval", std::string{}) == "autopilot");
        CHECK(chat_params.value("agentId", std::string{}) == "fixture-agent");
        CHECK(chat_params.value("workflowId", std::string{}) == "fixture-workflow");
        CHECK(chat_params.value("context", std::string{}) == "current_turn");
        CHECK(chat_params.value("conversationId", std::string{}) == "conversation-1");
        REQUIRE(chat_params.at("messages").is_array());
        REQUIRE(chat_params.at("messages").size() == 2);
        CHECK(chat_params.at("messages").front().value("role", std::string{}) == "system");
        CHECK(chat_params.at("messages").back().value("content", std::string{}) ==
                            "Hello from composer\n\nFind the likely bug and propose a minimal fix.");

        REQUIRE(pump_until_request(panel, "run.status"));
        REQUIRE(pump_until_request(panel, "conversation.get"));
        REQUIRE(dispatch_json_action(panel, "history.load", {{"id", "history-1"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "conversation.get", 2));
        REQUIRE(pump_until_history_state(panel, "", 1, "history-1"));
        const auto history_get = latest_request("conversation.get");
        REQUIRE(history_get.has_value());
        CHECK(history_get->at("params").value("id", std::string{}) == "history-1");

        REQUIRE(dispatch_json_action(panel, "history.delete", {{"id", "history-1"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "conversation.delete"));
        const auto history_delete = latest_request("conversation.delete");
        REQUIRE(history_delete.has_value());
        CHECK(history_delete->at("params").value("id", std::string{}) == "history-1");

        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(panel, "chat.new", nullptr, 0) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "conversation.create", 2));
        REQUIRE(destroy_main_panel_when_idle(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    }

    TEST_CASE("ai_editor_main_panel History search clear and duplicate preserve the current view",
          "[ai_editor][main_panel][history][search][duplicate][focused]") {
        ScopedMainPanelRpc rpc(MainPanelRpcMode::CompleteRun);
        sao_ui_compositor_handle_t compositor = make_headless_compositor();
        REQUIRE(compositor != nullptr);
        const auto launcher = reinterpret_cast<sao_ai_editor_launcher_t>(static_cast<uintptr_t>(0x1));
        sao_ai_editor_main_panel_t panel = nullptr;
        REQUIRE(sao_ai_editor_main_panel_create(compositor, launcher, &panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_history_state(panel, "", 1));

        test_json snapshot = snapshot_main_panel(panel);
        const test_json* search_action = find_action_node(snapshot["spec"], "history.search");
        REQUIRE(search_action != nullptr);
        CHECK(search_action->value("label", std::string{}) == "Search...");
        const test_json* duplicate_action = find_action_node(snapshot["spec"], "history.duplicate");
        REQUIRE(duplicate_action != nullptr);
        CHECK(duplicate_action->value("label", std::string{}) == "Duplicate / 复制");

        REQUIRE(dispatch_json_action(panel, "composer.changed", {{"text", "must wait"}}) ==
            SAO_AI_EDITOR_OK);
        const size_t create_count_before_search = request_count("conversation.create");
        const size_t run_count_before_search = request_count("chat.run");
        ScopedHistorySearchHold search_hold;
        REQUIRE(dispatch_json_action(panel, "history.search", {{"query", "needle"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(search_hold.wait_until_blocked());
        snapshot = snapshot_main_panel(panel);
        CHECK(snapshot.value("historyTaskPending", false));
        search_action = find_action_node(snapshot["spec"], "history.search");
        REQUIRE(search_action != nullptr);
        CHECK(search_action->value("disabled", false));
        duplicate_action = find_action_node(snapshot["spec"], "history.duplicate");
        REQUIRE(duplicate_action != nullptr);
        CHECK(duplicate_action->value("disabled", false));
        const test_json* new_chat_action = find_action_node(snapshot["spec"], "chat.new");
        REQUIRE(new_chat_action != nullptr);
        CHECK(new_chat_action->value("disabled", false));
        const test_json* send_action = find_action_node(snapshot["spec"], "chat.send");
        REQUIRE(send_action != nullptr);
        CHECK(send_action->value("disabled", false));
        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                    panel, "chat.new", nullptr, 0) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                    panel, "chat.send", nullptr, 0) == SAO_AI_EDITOR_OK);
        CHECK(request_count("conversation.create") == create_count_before_search);
        CHECK(request_count("chat.run") == run_count_before_search);
        search_hold.release();
        REQUIRE(pump_until_request(panel, "conversation.search"));
        const auto search_request = latest_request("conversation.search");
        REQUIRE(search_request.has_value());
        const test_json search_params = search_request->at("params");
        CHECK(search_params.value("query", std::string{}) == "needle");
        CHECK(search_params.value("scope", std::string{}) == "all");
        CHECK(search_params.value("limit", size_t{0}) == 100);
        REQUIRE(pump_until_history_state(panel, "needle", 1));

        snapshot = snapshot_main_panel(panel);
        REQUIRE(snapshot["history"].is_array());
        REQUIRE(snapshot["history"].size() == 1);
        CHECK(snapshot["history"][0].value("id", std::string{}) == "history-1");
        CHECK(snapshot["history"][0].value("matchCount", size_t{0}) == 3);
        CHECK(snapshot["history"][0]["matchedFields"] ==
            test_json::array({"title", "message"}));
        const std::string search_spec = snapshot["spec"].dump();
        CHECK(search_spec.find("Query: needle") != std::string::npos);
        CHECK(search_spec.find("Results: 1 · showing 1") != std::string::npos);
        CHECK(search_spec.find("Matched: title") != std::string::npos);
        CHECK(search_spec.find("Matched: message") != std::string::npos);
        CHECK(search_spec.find("Matches: 3") != std::string::npos);
        const test_json* clear_action = find_action_node(snapshot["spec"], "history.clear_search");
        REQUIRE(clear_action != nullptr);
        CHECK_FALSE(clear_action->value("disabled", false));

        const size_t search_count_before_duplicate = request_count("conversation.search");
        REQUIRE(dispatch_json_action(panel, "history.duplicate", {{"id", "history-1"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "conversation.duplicate"));
        const auto duplicate_request = latest_request("conversation.duplicate");
        REQUIRE(duplicate_request.has_value());
        const test_json duplicate_params = duplicate_request->at("params");
        CHECK(duplicate_params.value("sourceId", std::string{}) == "history-1");
        CHECK_FALSE(duplicate_params.contains("title"));
        CHECK_FALSE(duplicate_params.contains("scope"));
        REQUIRE(pump_until_request(panel, "conversation.search",
                                   search_count_before_duplicate + 1));
        REQUIRE(pump_until_history_state(panel, "needle", 2, "history-copy-1"));

        snapshot = snapshot_main_panel(panel);
        CHECK(snapshot.value("conversationTitle", std::string{}) == "History Fixture (copy)");
        REQUIRE(snapshot["conversationMessages"].is_array());
        REQUIRE(snapshot["conversationMessages"].size() == 2);
        CHECK(snapshot["conversationMessages"][0].value("content", std::string{}) ==
            "Historical prompt");
        CHECK(snapshot["history"].size() == 2);
        CHECK(snapshot["spec"].dump().find("History Fixture (copy)") != std::string::npos);

        const size_t list_count_before_clear = request_count("conversation.list");
        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                    panel, "history.clear_search", nullptr, 0) == SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "conversation.list", list_count_before_clear + 1));
        REQUIRE(pump_until_history_state(panel, "", 2, "history-copy-1"));
        const auto list_request = latest_request("conversation.list");
        REQUIRE(list_request.has_value());
        CHECK(list_request->at("params").value("scope", std::string{}) == "all");
        CHECK(list_request->at("params").value("limit", size_t{0}) == 10);
        snapshot = snapshot_main_panel(panel);
        CHECK(snapshot["spec"].dump().find("Conversations: 2") != std::string::npos);

        REQUIRE(destroy_main_panel_when_idle(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    }

    TEST_CASE("ai_editor_main_panel keeps useful history state when mutation refresh fails",
          "[ai_editor][main_panel][history][duplicate][delete][refresh][focused]") {
        ScopedMainPanelRpc rpc(MainPanelRpcMode::CompleteRun);
        sao_ui_compositor_handle_t compositor = make_headless_compositor();
        REQUIRE(compositor != nullptr);
        const auto launcher = reinterpret_cast<sao_ai_editor_launcher_t>(static_cast<uintptr_t>(0x1));
        sao_ai_editor_main_panel_t panel = nullptr;
        REQUIRE(sao_ai_editor_main_panel_create(compositor, launcher, &panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_history_state(panel, "", 1));

        size_t list_count = request_count("conversation.list");
        fail_next_history_refresh();
        REQUIRE(dispatch_json_action(panel, "history.duplicate", {{"id", "history-1"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "conversation.duplicate"));
        REQUIRE(pump_until_request(panel, "conversation.list", list_count + 1));
        REQUIRE(pump_until_history_state(panel, "", 2, "history-copy-1"));
        test_json snapshot = snapshot_main_panel(panel);
        REQUIRE(snapshot["history"].size() == 2);
        CHECK(snapshot["history"][0].value("id", std::string{}) == "history-copy-1");
        CHECK(snapshot["spec"].dump().find(
                  "Conversation duplicated, but history refresh failed") != std::string::npos);

        list_count = request_count("conversation.list");
        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "history.refresh", nullptr, 0) == SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "conversation.list", list_count + 1));
        REQUIRE(pump_until_history_state(panel, "", 2, "history-copy-1"));
        snapshot = snapshot_main_panel(panel);
        CHECK(snapshot["spec"].dump().find(
              "Conversation duplicated, but history refresh failed") == std::string::npos);

        list_count = request_count("conversation.list");
        fail_next_history_refresh();
        REQUIRE(dispatch_json_action(panel, "history.delete", {{"id", "history-1"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "conversation.delete"));
        REQUIRE(pump_until_request(panel, "conversation.list", list_count + 1));
        REQUIRE(pump_until_history_state(panel, "", 1, "history-copy-1"));
        snapshot = snapshot_main_panel(panel);
        REQUIRE(snapshot["history"].size() == 1);
        CHECK(snapshot["history"][0].value("id", std::string{}) == "history-copy-1");
        CHECK(snapshot["spec"].dump().find(
                  "Conversation deleted, but history refresh failed") != std::string::npos);

        REQUIRE(destroy_main_panel_when_idle(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    }

    TEST_CASE("ai_editor_main_panel Stop requests run cancellation and terminal reload",
          "[ai_editor][main_panel][chat][cancel][focused]") {
        ScopedMainPanelRpc rpc(MainPanelRpcMode::RunUntilCancelled);
        sao_ui_compositor_handle_t compositor = make_headless_compositor();
        REQUIRE(compositor != nullptr);
        const auto launcher = reinterpret_cast<sao_ai_editor_launcher_t>(static_cast<uintptr_t>(0x1));
        sao_ai_editor_main_panel_t panel = nullptr;
        REQUIRE(sao_ai_editor_main_panel_create(compositor, launcher, &panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "conversation.list"));
        REQUIRE(dispatch_json_action(panel, "composer.changed", {{"text", "Please keep running"}}) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(panel, "chat.send", nullptr, 0) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "run.status"));
        const test_json active_snapshot = snapshot_main_panel(panel);
        const test_json* active_search = find_action_node(active_snapshot["spec"], "history.search");
        REQUIRE(active_search != nullptr);
        CHECK(active_search->value("disabled", false));
        const test_json* active_duplicate =
            find_action_node(active_snapshot["spec"], "history.duplicate");
        REQUIRE(active_duplicate != nullptr);
        CHECK(active_duplicate->value("disabled", false));
        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(panel, "chat.stop", nullptr, 0) ==
            SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_request(panel, "run.cancel"));
        const auto cancel_request = latest_request("run.cancel");
        REQUIRE(cancel_request.has_value());
        CHECK(cancel_request->at("params").value("runId", std::string{}) == "run-1");
        REQUIRE(pump_until_request(panel, "run.status", 2));
        REQUIRE(pump_until_request(panel, "conversation.get"));
        REQUIRE(destroy_main_panel_when_idle(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    }

    TEST_CASE("ai_editor_main_panel retries failed cancellation for a superseded starting run",
          "[ai_editor][main_panel][chat][cancel][retry][focused]") {
        ScopedMainPanelRpc rpc(MainPanelRpcMode::CompleteRun);
        sao_ui_compositor_handle_t compositor = make_headless_compositor();
        REQUIRE(compositor != nullptr);
        const auto launcher = reinterpret_cast<sao_ai_editor_launcher_t>(static_cast<uintptr_t>(0x1));
        sao_ai_editor_main_panel_t panel = nullptr;
        REQUIRE(sao_ai_editor_main_panel_create(compositor, launcher, &panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_history_state(panel, "", 1));

        const size_t create_count_before_send = request_count("conversation.create");
        REQUIRE(dispatch_json_action(panel, "composer.changed", {{"text", "Supersede me"}}) ==
            SAO_AI_EDITOR_OK);
        ScopedChatRunHold run_hold;
        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                    panel, "chat.send", nullptr, 0) == SAO_AI_EDITOR_OK);
        REQUIRE(run_hold.wait_until_blocked());
        REQUIRE(request_count("conversation.create") == create_count_before_send + 1);
        run_hold.fail_cancel_attempts(1);

        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                    panel, "chat.new", nullptr, 0) == SAO_AI_EDITOR_OK);
        run_hold.release();
        REQUIRE(pump_until_request(panel, "run.cancel", 2));
        CHECK(request_count("run.cancel") == 2);
        const auto cancel_request = latest_request("run.cancel");
        REQUIRE(cancel_request.has_value());
        CHECK(cancel_request->at("params").value("runId", std::string{}) == "run-1");
        REQUIRE(pump_until_request(panel, "conversation.create", create_count_before_send + 2));

        REQUIRE(destroy_main_panel_when_idle(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    }

    TEST_CASE("ai_editor_main_panel bounds orphan cancellation retries and reports final failure",
          "[ai_editor][main_panel][chat][cancel][retry][focused]") {
        ScopedMainPanelRpc rpc(MainPanelRpcMode::CompleteRun);
        sao_ui_compositor_handle_t compositor = make_headless_compositor();
        REQUIRE(compositor != nullptr);
        const auto launcher = reinterpret_cast<sao_ai_editor_launcher_t>(static_cast<uintptr_t>(0x1));
        sao_ai_editor_main_panel_t panel = nullptr;
        REQUIRE(sao_ai_editor_main_panel_create(compositor, launcher, &panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(pump_until_history_state(panel, "", 1));

        const size_t create_count_before_send = request_count("conversation.create");
        REQUIRE(dispatch_json_action(panel, "composer.changed", {{"text", "Cancel permanently"}}) ==
            SAO_AI_EDITOR_OK);
        ScopedChatRunHold run_hold;
        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                    panel, "chat.send", nullptr, 0) == SAO_AI_EDITOR_OK);
        REQUIRE(run_hold.wait_until_blocked());
        run_hold.fail_cancel_attempts(3);
        REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                    panel, "chat.new", nullptr, 0) == SAO_AI_EDITOR_OK);
        run_hold.release();

        REQUIRE(pump_until_request(panel, "run.cancel", 3));
        REQUIRE(pump_until_output_contains(panel, "run.cancel -> error: request timed out"));
        CHECK(request_count("run.cancel") == 3);
        REQUIRE(pump_until_request(panel, "conversation.create", create_count_before_send + 2));

        REQUIRE(destroy_main_panel_when_idle(panel) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    }

TEST_CASE("ai_editor_main_panel GPU Hunt action adds only shared compositor layers",
          "[ai_editor][main_panel][gpu_hunt][compositor][focused]") {
    const auto windows_before = top_level_windows();
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    REQUIRE(compositor_layer_count(compositor) == 0);

    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(compositor_layer_count(compositor) == 1);
    CHECK(top_level_windows() == windows_before);

    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "gpu.hunt", nullptr, 0) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_tick(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(compositor_layer_count(compositor) == 2);
    CHECK(top_level_windows() == windows_before);

    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "gpu.hunt", nullptr, 0) == SAO_AI_EDITOR_OK);
    REQUIRE(compositor_layer_count(compositor) == 2);
    CHECK(top_level_windows() == windows_before);

    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "settings.open", nullptr, 0) == SAO_AI_EDITOR_OK);
    REQUIRE(compositor_layer_count(compositor) == 3);
    CHECK(top_level_windows() == windows_before);

        sao_ui_panel_handle_t gpu_panel = nullptr;
        REQUIRE(sao_ui_panel_find_by_id(compositor, SAO_AI_EDITOR_GPU_HUNT_PANEL_ID, &gpu_panel) ==
            SAO_STATUS_OK);
        REQUIRE(gpu_panel != nullptr);
        REQUIRE(runtime_panel_state(gpu_panel).visible);
        const sao_ui_panel_handle_t settings_panel = find_settings_runtime_panel(compositor);
        REQUIRE(runtime_panel_state(settings_panel).visible);
        REQUIRE(sao_ai_editor_main_panel_hide(panel) == SAO_AI_EDITOR_OK);
        CHECK_FALSE(runtime_panel_state(find_main_runtime_panel(compositor)).visible);
        CHECK_FALSE(runtime_panel_state(gpu_panel).visible);
        CHECK_FALSE(runtime_panel_state(settings_panel).visible);

        std::atomic<int32_t> foreign_destroy_status{SAO_AI_EDITOR_OK};
        std::thread foreign_destroy([&] {
            foreign_destroy_status.store(sao_ai_editor_main_panel_try_destroy(panel),
                                         std::memory_order_release);
        });
        foreign_destroy.join();
        CHECK(foreign_destroy_status.load(std::memory_order_acquire) ==
              SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
        CHECK(compositor_layer_count(compositor) == 3);
        CHECK_FALSE(runtime_panel_state(find_main_runtime_panel(compositor)).visible);
        CHECK_FALSE(runtime_panel_state(gpu_panel).visible);
        CHECK_FALSE(runtime_panel_state(settings_panel).visible);

    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(compositor_layer_count(compositor) == 0);
    CHECK(top_level_windows() == windows_before);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}
