#include "sao/ai_editor/ai_editor_settings_panel.h"

#include "sao/ui/dialog.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_sdk.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
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
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr uint32_t kRequestTimeoutMs = 5000U;
constexpr size_t kMaximumRequestBytes = 256U * 1024U;
constexpr size_t kMaximumResponseBytes = 1024U * 1024U;
constexpr size_t kMaximumActionPayloadBytes = 64U * 1024U;
constexpr size_t kMaximumInputBytes = 64U * 1024U;
constexpr size_t kMaximumSecretBytes = 16U * 1024U;
constexpr size_t kMaximumSearchBytes = 256U;
constexpr size_t kFieldsPerPage = 12U;
constexpr size_t kMaximumValidationSummary = 6U;
constexpr size_t kMaximumNodeTextBytes = 3600U;
constexpr size_t kMaximumTitleTextBytes = 180U;
constexpr size_t kMaximumPanelSpecBytes = 192U * 1024U;
constexpr int32_t kDialogCloseAdvanceMs = 1000;
constexpr std::chrono::milliseconds kSavedStatusLifetime{5000};

constexpr char kDarkThemeOverride[] =
    R"({"colors":{"APP_BG":"#10141c","APP_CARD":"#171d28","APP_BORDER":"#2b3546","APP_TEXT":"#e8edf6","APP_TEXT_2":"#c5cedd","APP_TEXT_DIM":"#8e9bad","APP_ACCENT":"#5ba7ff","APP_BLUE":"#5ba7ff","APP_GREEN":"#54c987","APP_RED":"#ef6677","APP_ORANGE":"#e8a15a","APP_GOLD":"#e7c46a"}})";

enum class FieldKind {
    Boolean,
    Integer,
    Number,
    String,
    Json,
    List,
    Enum,
    Secret,
};

enum class SavePhase {
    Idle,
    Saving,
    Saved,
    Failed,
};

enum class DialogIntent {
    None,
    EditField,
    Search,
    ConfirmClearSecret,
    ConfirmReload,
};

enum class RpcKind {
    None,
    Describe,
    LoadScope,
    LoadRawScope,
    LoadEffective,
    Save,
};

struct PageDefinition {
    std::string_view id;
    std::string_view label;
    std::string_view description;
};

constexpr std::array<PageDefinition, 14> kPages{{
    {"overview", "Overview / 概览", "连接状态、当前作用域、AI 提供商和待保存更改。"},
    {"endpoint", "AI Endpoint / AI 端点", "提供商、模型、密钥、端点和助手运行模式。"},
    {"appearance", "Appearance / 外观", "颜色主题、图标主题与编辑器视觉选项。"},
    {"editor", "Editor & Files / 编辑器与文件", "编辑、格式化、建议、悬停、保存与文件行为。"},
    {"claude", "Claude Code", "Claude Code CLI、模型、参数和权限边界。"},
    {"codex", "Codex", "Codex CLI、模型、传输方式和启动参数。"},
    {"mcp", "MCP", "MCP 工具访问、发现、信任、采样和服务器配置。"},
    {"terminal", "Terminal / 终端", "终端配置、Shell、超时、输出限制与 ConPTY。"},
    {"workspace", "Workspace / 工作区", "工作区根目录、额外根目录与自动检测。"},
    {"extensions", "Extensions / 扩展", "安装确认、诊断、发布者与贡献点策略。"},
    {"customization", "Customization / 自定义", "AGENTS/CLAUDE 指令、代理、工作流和技能发现。"},
    {"advanced", "Advanced / 高级", "采样、连接、额外请求头与请求体。"},
    {"models", "Custom Models / 自定义模型", "私有或新模型的上下文、能力与输出限制。"},
    {"permissions", "Permissions / 权限", "读、写、执行和工具权限覆盖。"},
}};

struct FieldOption {
    json value;
    std::string label;
    std::string label_zh;
};

struct FieldDependency {
    std::string key;
    json expected;
};

struct FieldMeta {
    std::string key;
    std::string page;
    std::string group;
    std::string label;
    std::string label_zh;
    std::string description;
    std::string keywords;
    FieldKind kind{FieldKind::String};
    json default_value;
    std::vector<FieldOption> options;
    std::vector<FieldDependency> dependencies;
    std::optional<double> minimum;
    std::optional<double> maximum;
    bool required{};
    bool sensitive{};
    bool object_only{};
    bool advanced{};
};

struct PendingDialog {
    DialogIntent intent{DialogIntent::None};
    std::string field_key;
};

struct RpcResponse {
    bool ok{};
    json result;
    std::string error;
    int32_t transport_status{SAO_AI_EDITOR_OK};
};

struct RpcJob {
    RpcKind kind{RpcKind::None};
    std::string method;
    json params{json::object()};
    std::string scope;
    std::string plugin_id;
    json saved_draft{json::object()};
    std::vector<std::string> saved_reset_keys;
    std::vector<std::string> saved_secret_clears;
    std::vector<std::string> saved_secret_sets;
};

struct RpcCompletion {
    RpcJob job;
    RpcResponse response;
};

struct AiEditorSettingsPanelState {
    sao_ui_compositor_handle_t compositor{};
    sao_ai_editor_launcher_t launcher{};
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    sao_ui_dialog_handle_t dialog{};
    std::mutex mutex;
    std::mutex publish_mutex;
    std::mutex rpc_mutex;
    std::condition_variable cv;
    std::vector<FieldMeta> fields;
    json persisted{json::object()};
    json draft{json::object()};
    json effective{json::object()};
    json inherited{json::object()};
    json defaults{json::object()};
    json sources{json::object()};
    std::unordered_map<std::string, std::string> secret_states;
    std::unordered_map<std::string, std::string> effective_secret_states;
    std::unordered_map<std::string, std::string> inherited_secret_states;
    std::unordered_map<std::string, json> pending_secret_values;
    std::unordered_set<std::string> pending_secret_clears;
    std::unordered_set<std::string> reset_keys;
    std::unordered_map<std::string, std::string> validation_errors;
    PendingDialog pending_dialog;
    std::string active_page{"overview"};
    std::string selected_scope{"workspace"};
    std::string selected_plugin_id;
    std::string search_query;
    std::string review_filter{"all"};
    std::string last_spec;
    std::string backend_message;
    std::string status_message;
    std::string last_error;
    size_t result_offset{};
    size_t api_calls_in_flight{};
    size_t actions_in_flight{};
    uint64_t request_counter{};
    // Publication, readiness probes, moves, and resets are guarded by mutex.
    std::future<RpcCompletion> rpc_future;
    RpcKind rpc_kind{RpcKind::None};
    bool accepting{true};
    bool action_handler_attached{};
    bool event_handler_attached{};
    bool teardown_failed{};
    bool destroy_claimed{};
    bool visible{};
    bool backend_connected{};
    bool metadata_loaded{};
    bool values_loaded{};
    bool effective_loaded{};
    bool scope_overrides_exact{};
    bool describe_pending{};
    bool load_pending{};
    bool raw_load_pending{};
    bool effective_load_pending{};
    bool save_pending{};
    bool hidden_with_dirty{};
    bool show_advanced{};
    SavePhase save_phase{SavePhase::Idle};
    std::chrono::steady_clock::time_point saved_at{};
    std::chrono::steady_clock::time_point last_dialog_tick{};
#if defined(SAO_AI_EDITOR_TESTING)
    bool test_fail_unregister_once{};
    bool test_fail_restore_action{};
    bool test_fail_restore_event{};
#endif
};

std::mutex& registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<sao_ai_editor_settings_panel_t, std::unique_ptr<AiEditorSettingsPanelState>>&
registry() {
    static std::unordered_map<sao_ai_editor_settings_panel_t,
                              std::unique_ptr<AiEditorSettingsPanelState>>
        storage;
    return storage;
}

sao_ai_editor_settings_panel_t allocate_handle() noexcept {
    static std::atomic<uintptr_t> next{1};
    const uintptr_t value = (next.fetch_add(1, std::memory_order_relaxed) << 4U) | 5U;
    return reinterpret_cast<sao_ai_editor_settings_panel_t>(value);
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

int32_t require_owner_thread(sao_ui_compositor_handle_t compositor) noexcept {
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

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
    });
    return value;
}

std::string trim_copy(std::string_view value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return std::string(value.substr(begin, end - begin));
}

std::string localized_text(const json& value) {
    if (value.is_string())
        return value.get<std::string>();
    if (!value.is_object())
        return {};
    for (const char* key : {"zh-CN", "zh_cn", "zh", "en", "default"}) {
        const auto found = value.find(key);
        if (found != value.end() && found->is_string() && !found->empty())
            return found->get<std::string>();
    }
    return {};
}

std::string read_text(const json& object, std::initializer_list<const char*> keys) {
    if (!object.is_object())
        return {};
    for (const char* key : keys) {
        const auto found = object.find(key);
        if (found == object.end())
            continue;
        const std::string text = localized_text(*found);
        if (!text.empty())
            return text;
    }
    return {};
}

const PageDefinition* page_definition(std::string_view id) noexcept {
    const auto found = std::ranges::find(kPages, id, &PageDefinition::id);
    return found == kPages.end() ? nullptr : &*found;
}

size_t page_index(std::string_view id) noexcept {
    const auto found = std::ranges::find(kPages, id, &PageDefinition::id);
    return found == kPages.end()
               ? static_cast<size_t>(std::distance(
                     kPages.begin(),
                     std::ranges::find(kPages, std::string_view{"advanced"}, &PageDefinition::id)))
               : static_cast<size_t>(std::distance(kPages.begin(), found));
}

std::string normalize_page(std::string page) {
    page = ascii_lower(trim_copy(page));
    if (page.empty())
        return "advanced";
    if (page == "assistant" || page == "ai" || page == "ai / endpoint" || page == "ai endpoint" ||
        page == "endpoint" || page == "provider")
        return "endpoint";
    if (page == "editor" || page == "files" || page == "editor / files" || page == "editor & files")
        return "editor";
    if (page == "claude code" || page == "claude-code" || page == "claude_code")
        return "claude";
    if (page == "mcp policy")
        return "mcp";
    if (page == "custom models" || page == "custom_models" || page == "model" ||
        page == "model overrides")
        return "models";
    if (page == "permission" || page == "security")
        return "permissions";
    if (page_definition(page) != nullptr)
        return page;
    return "advanced";
}

std::string backend_key_alias(std::string_view key) {
    static const std::unordered_map<std::string_view, std::string_view> aliases{
        {"ai.max_output_tokens", "max_tokens"},
        {"ai.timeout_seconds", "timeout"},
        {"ai.stop_sequences", "stop"},
        {"workbench.colorTheme", "color_theme"},
        {"workbench.iconTheme", "file_icon_theme"},
        {"claude.cli_command", "claude_code.cli_path"},
        {"claude.model", "claude_code.model"},
        {"claude.cli_args", "claude_code.cli_args"},
        {"claude.prefer_cli", "claude_code.prefer_cli"},
        {"claude.allow_dangerously_skip_permissions",
         "claude_code.allow_dangerously_skip_permissions"},
        {"codex.cli_command", "codex.cli_path"},
        {"mcp.tool_access", "mcp.access"},
        {"mcp.name_collisions", "mcp.collision_behavior"},
        {"mcp.discovery", "mcp.discovery_enabled"},
        {"mcp.sampling", "mcp.server_sampling"},
        {"terminal.shell", "terminal.shell_path"},
        {"terminal.args", "terminal.shell_args"},
        {"workspace.additional_roots", "workspace.roots"},
        {"extensions.diagnostics", "extensions.diagnostics_enabled"},
    };
    if (const auto found = aliases.find(key); found != aliases.end())
        return std::string(found->second);
    constexpr std::string_view kAiPrefix = "ai.";
    if (key.starts_with(kAiPrefix))
        return std::string(key.substr(kAiPrefix.size()));
    return std::string(key);
}

std::string stable_token(std::string_view value) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::string clamp_utf8_bytes(std::string value, size_t maximum) {
    if (value.size() <= maximum)
        return value;
    if (maximum <= 3U)
        return value.substr(0, maximum);
    size_t end = maximum - 3U;
    while (end > 0U && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    value.resize(end);
    value.append("...");
    return value;
}

json text_node(std::string text, std::string_view style = "value", int32_t height = 22) {
    return json{{"type", "text"},
                {"text", clamp_utf8_bytes(std::move(text), kMaximumNodeTextBytes)},
                {"style", style},
                {"height", height}};
}

json badge_node(std::string text, std::string_view style = "muted") {
    return json{{"type", "badge"},
                {"text", clamp_utf8_bytes(std::move(text), kMaximumTitleTextBytes)},
                {"style", style},
                {"height", 22}};
}

json button_node(std::string id, std::string label, std::string action,
                 json payload = json::object(), std::string_view style = "default",
                 bool disabled = false) {
    json node{{"type", "button"},
              {"id", std::move(id)},
              {"label", clamp_utf8_bytes(std::move(label), kMaximumTitleTextBytes)},
              {"action", std::move(action)},
              {"style", style},
              {"height", 28}};
    if (!payload.empty())
        node["payload"] = std::move(payload);
    if (disabled)
        node["disabled"] = true;
    return node;
}

json row_node(json children, std::string_view align = "left") {
    return json{{"type", "row"}, {"align", align}, {"children", std::move(children)}};
}

json card_node(std::string title, json children, std::string_view accent = "cyan") {
    return json{{"type", "card"},
                {"title", clamp_utf8_bytes(std::move(title), kMaximumTitleTextBytes)},
                {"accent", accent},
                {"children", std::move(children)}};
}

json section_node(std::string title, json children) {
    return json{{"type", "section"},
                {"title", clamp_utf8_bytes(std::move(title), kMaximumTitleTextBytes)},
                {"children", std::move(children)}};
}

void append_button_rows(json& nodes, json buttons, size_t columns) {
    for (size_t begin = 0; begin < buttons.size(); begin += columns) {
        json row = json::array();
        const size_t end = std::min(buttons.size(), begin + columns);
        for (size_t index = begin; index < end; ++index)
            row.push_back(std::move(buttons[index]));
        nodes.push_back(row_node(std::move(row)));
    }
}

FieldOption option(json value, std::string label, std::string label_zh = {}) {
    return FieldOption{std::move(value), std::move(label), std::move(label_zh)};
}

FieldMeta field(std::string key, std::string page, std::string label, std::string label_zh,
                std::string description, FieldKind kind, json default_value = json(),
                std::vector<FieldOption> options = {}, std::optional<double> minimum = std::nullopt,
                std::optional<double> maximum = std::nullopt, std::string group = {}) {
    FieldMeta result;
    result.key = std::move(key);
    result.page = std::move(page);
    result.group = std::move(group);
    result.label = std::move(label);
    result.label_zh = std::move(label_zh);
    result.description = std::move(description);
    result.kind = kind;
    result.default_value = std::move(default_value);
    result.options = std::move(options);
    result.minimum = minimum;
    result.maximum = maximum;
    result.sensitive = result.kind == FieldKind::Secret;
    return result;
}

std::vector<FieldMeta> field_hints() {
    std::vector<FieldMeta> fields;
    fields.reserve(70);
    fields.push_back(field("ai.provider", "endpoint", "Provider", "提供商",
                           "Select the service used by the main assistant.", FieldKind::Enum,
                           "openai",
                           {option("openai", "OpenAI"), option("anthropic", "Anthropic"),
                            option("deepseek", "DeepSeek"), option("ollama", "Ollama"),
                            option("custom", "Custom", "自定义"), option("gemini", "Gemini")},
                           {}, {}, "Endpoint"));
    fields.push_back(field("ai.api_key", "endpoint", "API Key", "API 密钥",
                           "Protected credential for the active provider.", FieldKind::Secret,
                           json(), {}, {}, {}, "Endpoint"));
    for (const auto& [provider, label] :
         std::array<std::pair<std::string_view, std::string_view>, 6>{{
             {"openai", "OpenAI"},
             {"anthropic", "Anthropic"},
             {"deepseek", "DeepSeek"},
             {"ollama", "Ollama"},
             {"custom", "Custom"},
             {"gemini", "Gemini"},
         }}) {
        fields.push_back(field("provider_keys." + std::string(provider), "endpoint",
                               std::string(label) + " API Key", std::string(label) + " API 密钥",
                               "Protected provider-specific credential.", FieldKind::Secret, json(),
                               {}, {}, {}, "Provider Keys / 提供商密钥"));
    }
    fields.push_back(field("ai.base_url", "endpoint", "Base URL", "基础 URL",
                           "Leave empty to use the provider default endpoint.", FieldKind::String,
                           "", {}, {}, {}, "Endpoint"));
    fields.push_back(field("ai.model", "endpoint", "Model", "模型",
                           "Model name used for new assistant requests.", FieldKind::String, "", {},
                           {}, {}, "Model"));
    fields.push_back(field("ai.temperature", "endpoint", "Temperature", "温度",
                           "Sampling randomness from 0 to 2.", FieldKind::Number, 0.7, {}, 0.0, 2.0,
                           "Model"));
    fields.push_back(field("ai.max_output_tokens", "endpoint", "Max Output Tokens",
                           "最大输出 Token", "Maximum generated tokens for the active model.",
                           FieldKind::Integer, 4096, {}, 1.0, 1048576.0, "Model"));
    fields.push_back(field("ai.system_prompt", "endpoint", "System Prompt", "系统提示词",
                           "Base instruction prepended to assistant requests.", FieldKind::String,
                           "", {}, {}, {}, "Assistant"));
    fields.push_back(field("ai.mode", "endpoint", "Mode", "模式",
                           "Ask is read-only, Plan confirms writes, Agent is autonomous.",
                           FieldKind::Enum, "agent",
                           {option("ask", "Ask", "询问"), option("plan", "Plan", "规划"),
                            option("agent", "Agent", "代理")},
                           {}, {}, "Assistant"));

    fields.push_back(field("workbench.colorTheme", "appearance", "Color Theme", "颜色主题",
                           "Active workbench color theme.", FieldKind::String, "SAO Dark"));
    fields.push_back(field("workbench.iconTheme", "appearance", "File Icon Theme", "文件图标主题",
                           "Icon theme used by Explorer and extension views.", FieldKind::String,
                           ""));

    fields.push_back(field("editor.defaultFormatter", "editor", "Default Formatter", "默认格式化器",
                           "Extension identifier used by format commands.", FieldKind::String, "",
                           {}, {}, {}, "Editor"));
    fields.push_back(field("editor.tabSize", "editor", "Tab Size", "制表宽度",
                           "Number of spaces represented by one indentation level.",
                           FieldKind::Integer, 4, {}, 1.0, 16.0, "Editor"));
    fields.push_back(field("editor.insertSpaces", "editor", "Insert Spaces", "插入空格",
                           "Use spaces instead of tab characters.", FieldKind::Boolean, true, {},
                           {}, {}, "Editor"));
    fields.push_back(field("editor.wordSeparators", "editor", "Word Separators", "单词分隔符",
                           "Characters used by word selection and navigation.", FieldKind::String,
                           "", {}, {}, {}, "Editor"));
    fields.push_back(field("editor.quickSuggestions", "editor", "Quick Suggestions", "快速建议",
                           "Per-context suggestion policy as JSON.", FieldKind::Json,
                           json::object(), {}, {}, {}, "Suggestions"));
    fields.push_back(field("editor.quickSuggestionsDelay", "editor", "Quick Suggestions Delay",
                           "快速建议延迟", "Delay before automatic suggestions are requested.",
                           FieldKind::Integer, 10, {}, 0.0, 10000.0, "Suggestions"));
    fields.push_back(field("editor.hover.enabled", "editor", "Hover", "悬停提示",
                           "Controls hover information display.", FieldKind::Enum, "on",
                           {option("on", "On", "开启"), option("off", "Off", "关闭"),
                            option("onKeyboardModifier", "On keyboard modifier", "按修饰键开启")},
                           {}, {}, "Editor"));
    fields.push_back(field("editor.hover.delay", "editor", "Hover Delay", "悬停延迟",
                           "Milliseconds before hover content appears.", FieldKind::Integer, 300,
                           {}, 0.0, 10000.0, "Editor"));
    fields.push_back(field("editor.hover.hidingDelay", "editor", "Hover Hiding Delay",
                           "悬停隐藏延迟", "Milliseconds before hover content is hidden.",
                           FieldKind::Integer, 300, {}, 0.0, 10000.0, "Hover / 悬停"));
    fields.push_back(field("editor.hover.sticky", "editor", "Sticky Hover", "固定悬停框",
                           "Keep hover content visible while moving the pointer into it.",
                           FieldKind::Boolean, true, {}, {}, {}, "Hover / 悬停"));
    fields.push_back(field("editor.hover.above", "editor", "Prefer Hover Above", "优先在上方显示",
                           "Prefer showing hover content above the line.", FieldKind::Boolean, true,
                           {}, {}, {}, "Hover / 悬停"));
    fields.push_back(field("editor.minimap.enabled", "editor", "Minimap", "缩略图",
                           "Show the editor minimap.", FieldKind::Boolean, true, {}, {}, {},
                           "Editor"));
    fields.push_back(field("editor.minimap.size", "editor", "Minimap Size", "缩略图尺寸",
                           "Controls how the minimap uses the available height.", FieldKind::Enum,
                           "proportional",
                           {option("proportional", "Proportional", "按比例"),
                            option("fill", "Fill", "填满"), option("fit", "Fit", "适应")},
                           {}, {}, "Minimap / 缩略图"));
    fields.push_back(field("editor.minimap.side", "editor", "Minimap Side", "缩略图位置",
                           "Place the minimap on the left or right side.", FieldKind::Enum, "right",
                           {option("left", "Left", "左侧"), option("right", "Right", "右侧")}, {},
                           {}, "Minimap / 缩略图"));
    fields.push_back(field(
        "editor.minimap.showSlider", "editor", "Minimap Slider", "缩略图滑块",
        "Controls when the minimap slider is shown.", FieldKind::Enum, "mouseover",
        {option("always", "Always", "始终"), option("mouseover", "On mouse over", "鼠标悬停时")},
        {}, {}, "Minimap / 缩略图"));
    fields.push_back(field("editor.minimap.renderCharacters", "editor", "Render Characters",
                           "绘制字符", "Render character glyphs instead of color blocks.",
                           FieldKind::Boolean, true, {}, {}, {}, "Minimap / 缩略图"));
    fields.push_back(field("editor.minimap.maxColumn", "editor", "Minimap Max Column",
                           "缩略图最大列", "Maximum editor column represented in the minimap.",
                           FieldKind::Integer, 120, {}, 1.0, 10000.0, "Minimap / 缩略图"));
    fields.push_back(field("editor.minimap.scale", "editor", "Minimap Scale", "缩略图缩放",
                           "Pixel scale used by the minimap.", FieldKind::Integer, 1, {}, 1.0, 3.0,
                           "Minimap / 缩略图"));
    fields.push_back(
        field("editor.renderWhitespace", "editor", "Render Whitespace", "显示空白字符",
              "Controls which whitespace characters are visible.", FieldKind::Enum, "selection",
              {option("none", "None", "不显示"), option("boundary", "Boundary", "边界"),
               option("selection", "Selection", "选区"), option("trailing", "Trailing", "行尾"),
               option("all", "All", "全部")},
              {}, {}, "Editor"));
    fields.push_back(field("editor.rulers", "editor", "Rulers", "标尺",
                           "Column rulers as a JSON array.", FieldKind::Json, json::array(), {}, {},
                           {}, "Editor"));
    fields.push_back(field("editor.formatOnSave", "editor", "Format On Save", "保存时格式化",
                           "Run the active formatter before explicit saves.", FieldKind::Boolean,
                           false, {}, {}, {}, "Save"));
    fields.push_back(
        field("files.autoSave", "editor", "Auto Save", "自动保存",
              "Select when dirty files are saved automatically.", FieldKind::Enum, "off",
              {option("off", "Off", "关闭"), option("afterDelay", "After delay", "延迟后"),
               option("onFocusChange", "On focus change", "焦点变化时"),
               option("onWindowChange", "On window change", "窗口变化时")},
              {}, {}, "Files"));
    fields.push_back(field("files.autoSaveDelay", "editor", "Auto Save Delay", "自动保存延迟",
                           "Delay in milliseconds for afterDelay auto-save.", FieldKind::Integer,
                           1000, {}, 0.0, 600000.0, "Files"));
    fields.push_back(field("files.trimTrailingWhitespace", "editor", "Trim Trailing Whitespace",
                           "移除行尾空格", "Remove trailing whitespace during saves.",
                           FieldKind::Boolean, false, {}, {}, {}, "Files"));
    fields.push_back(field("files.insertFinalNewline", "editor", "Insert Final Newline",
                           "插入末尾换行", "Ensure saved text files end with a newline.",
                           FieldKind::Boolean, false, {}, {}, {}, "Files"));
    fields.push_back(field("files.trimFinalNewlines", "editor", "Trim Final Newlines",
                           "移除多余末尾换行", "Remove extra final newlines during saves.",
                           FieldKind::Boolean, false, {}, {}, {}, "Files"));

    fields.push_back(field("claude.cli_command", "claude", "CLI Command", "CLI 命令",
                           "Leave empty for Claude CLI auto-discovery.", FieldKind::String, ""));
    fields.push_back(field("claude.model", "claude", "Model", "模型",
                           "Claude model override; empty uses provider discovery.",
                           FieldKind::String, ""));
    fields.push_back(field("claude.cli_args", "claude", "CLI Arguments", "CLI 参数",
                           "One non-secret command-line argument per line.", FieldKind::List,
                           json::array()));
    fields.push_back(field("claude.prefer_cli", "claude", "Prefer CLI", "优先使用 CLI",
                           "Keep the local Claude CLI path ready.", FieldKind::Boolean, false));
    fields.push_back(
        field("claude.allow_dangerously_skip_permissions", "claude", "Allow Skip Permissions Flag",
              "允许跳过权限标志",
              "Controls whether external launches may include the skip-permissions flag.",
              FieldKind::Boolean, false));

    fields.push_back(field("codex.cli_command", "codex", "CLI Command", "CLI 命令",
                           "Leave empty for Codex CLI auto-discovery.", FieldKind::String, ""));
    fields.push_back(field("codex.model", "codex", "Model", "模型", "Codex model override.",
                           FieldKind::String, ""));
    fields.push_back(
        field("codex.transport", "codex", "Transport", "传输方式",
              "Protocol used for Codex requests.", FieldKind::Enum, "responses",
              {option("chat_completions", "Chat Completions"), option("responses", "Responses"),
               option("cli", "CLI preference", "CLI 偏好")}));
    fields.push_back(field("codex.cli_args", "codex", "CLI Arguments", "CLI 参数",
                           "One non-secret command-line argument per line.", FieldKind::List,
                           json::array()));

    fields.push_back(field("mcp.tool_access", "mcp", "Tool Access", "工具访问",
                           "Default policy for tools exposed by MCP servers.", FieldKind::Enum,
                           "prompt",
                           {option("prompt", "Prompt before use", "使用前确认"),
                            option("read_only", "Read-only allowed", "允许只读"),
                            option("allow", "Allow configured tools", "允许已配置工具"),
                            option("disabled", "Disabled", "禁用")}));
    fields.push_back(field("mcp.name_collisions", "mcp", "Name Collisions", "名称冲突",
                           "How duplicate MCP tool names are resolved.", FieldKind::Enum, "error",
                           {option("first", "Use first registered", "使用最早注册"),
                            option("last", "Use latest registered", "使用最新注册"),
                            option("error", "Treat as error", "视为错误")}));
    fields.push_back(field("mcp.discovery", "mcp", "Discover Servers", "发现服务器",
                           "Read configured MCP server entries.", FieldKind::Boolean, true));
    fields.push_back(field("mcp.workspace_trusted", "mcp", "Trust Workspace MCP", "信任工作区 MCP",
                           "Allow workspace manifests to launch MCP commands.", FieldKind::Boolean,
                           false));
    fields.push_back(field("mcp.autostart", "mcp", "Autostart Servers", "自动启动服务器",
                           "Start configured MCP servers with the editor.", FieldKind::Boolean,
                           false));
    fields.push_back(field("mcp.sampling", "mcp", "Allow Server Sampling", "允许服务器采样",
                           "Permit trusted MCP servers to request model sampling.",
                           FieldKind::Boolean, false));
    fields.push_back(field("mcp.servers", "mcp", "MCP Servers", "MCP 服务器",
                           "Server definitions as a JSON object or array.", FieldKind::Json,
                           json::object()));

    fields.push_back(field("terminal.profile", "terminal", "Profile Name", "配置名称",
                           "Human-readable terminal profile name.", FieldKind::String,
                           "PowerShell"));
    fields.push_back(field("terminal.shell", "terminal", "Shell Command", "Shell 命令",
                           "Leave empty for the platform default shell.", FieldKind::String, ""));
    fields.push_back(field("terminal.timeout", "terminal", "Timeout (seconds)", "超时（秒）",
                           "Maximum command runtime before cancellation.", FieldKind::Integer, 30,
                           {}, 1.0, 600.0));
    fields.push_back(field("terminal.output_limit", "terminal", "Output Limit", "输出限制",
                           "Maximum captured terminal output bytes.", FieldKind::Integer, 8000, {},
                           1000.0, 1048576.0));
    fields.push_back(field("terminal.args", "terminal", "Shell Arguments", "Shell 参数",
                           "One non-secret shell argument per line.", FieldKind::List,
                           json::array()));
    fields.push_back(field("terminal.use_pty", "terminal", "Use ConPTY", "使用 ConPTY",
                           "Expose a real pseudo-terminal to interactive programs.",
                           FieldKind::Boolean, true));

    fields.push_back(field("workspace.root", "workspace", "Workspace Root", "工作区根目录",
                           "Primary workspace root; empty enables auto-detection.",
                           FieldKind::String, ""));
    fields.push_back(field("workspace.additional_roots", "workspace", "Additional Roots",
                           "额外根目录", "Fallback workspace roots, one per line.", FieldKind::List,
                           json::array()));
    fields.push_back(field("workspace.auto_detect", "workspace", "Auto Detect", "自动检测",
                           "Resolve a workspace from the active file and repository.",
                           FieldKind::Boolean, true));
    fields.push_back(field("workspace.remember_last", "workspace", "Remember Last Workspace",
                           "记住上次工作区", "Reuse the last resolved workspace when appropriate.",
                           FieldKind::Boolean, true));

    fields.push_back(field(
        "extensions.confirm_install", "extensions", "Confirm Extension Installs", "确认扩展安装",
        "Require a user confirmation before install or uninstall.", FieldKind::Boolean, true));
    fields.push_back(field("extensions.diagnostics", "extensions", "Extension Host Diagnostics",
                           "扩展宿主诊断", "Enable verbose extension-host diagnostics.",
                           FieldKind::Boolean, false));
    fields.push_back(field("extensions.allowed_publishers", "extensions", "Allowed Publishers",
                           "允许的发布者", "Publisher allow-list, one identifier per line.",
                           FieldKind::List, json::array()));
    fields.push_back(field("extensions.blocked_publishers", "extensions", "Blocked Publishers",
                           "阻止的发布者", "Publisher deny-list, one identifier per line.",
                           FieldKind::List, json::array()));
    fields.push_back(field(
        "extensions.enabled_contributions", "extensions", "Enabled Contributions", "启用的贡献点",
        "VS Code contribution types loaded by the runtime.", FieldKind::List, json::array()));

    fields.push_back(field("customization.use_agent_md", "customization", "Read AGENTS.md",
                           "读取 AGENTS.md", "Include workspace AGENTS.md instruction files.",
                           FieldKind::Boolean, true));
    fields.push_back(field(
        "customization.use_claude_md", "customization", "Read CLAUDE.md", "读取 CLAUDE.md",
        "Include Claude-style project memory when explicitly enabled.", FieldKind::Boolean, false));
    fields.push_back(
        field("customization.instructions_locations", "customization", "Instruction Locations",
              "指令位置", "Relative instruction discovery paths.", FieldKind::List, json::array()));
    fields.push_back(field("customization.agent_locations", "customization", "Agent Locations",
                           "代理位置", "Relative custom agent discovery paths.", FieldKind::List,
                           json::array()));
    fields.push_back(field("customization.workflow_locations", "customization",
                           "Workflow Locations", "工作流位置", "Relative workflow discovery paths.",
                           FieldKind::List, json::array()));
    fields.push_back(field("customization.skill_locations", "customization", "Skill Locations",
                           "技能位置", "Relative skill discovery paths.", FieldKind::List,
                           json::array()));

    fields.push_back(field("ai.top_p", "advanced", "Top P", "Top P",
                           "Nucleus sampling probability.", FieldKind::Number, 1.0, {}, 0.0, 1.0,
                           "Sampling"));
    fields.push_back(field("ai.frequency_penalty", "advanced", "Frequency Penalty", "频率惩罚",
                           "Penalize tokens already present in the response.", FieldKind::Number,
                           0.0, {}, -2.0, 2.0, "Sampling"));
    fields.push_back(field("ai.presence_penalty", "advanced", "Presence Penalty", "存在惩罚",
                           "Penalize topics already introduced.", FieldKind::Number, 0.0, {}, -2.0,
                           2.0, "Sampling"));
    fields.push_back(field("ai.timeout_seconds", "advanced", "Timeout (seconds)", "超时（秒）",
                           "Provider request deadline.", FieldKind::Integer, 180, {}, 10.0, 600.0,
                           "Connection"));
    fields.push_back(field("ai.stop_sequences", "advanced", "Stop Sequences", "停止序列",
                           "One stop sequence per line.", FieldKind::List, json::array(), {}, {},
                           {}, "Sampling"));
    fields.push_back(field("ai.max_input_tokens", "advanced", "Max Input Tokens", "最大输入 Token",
                           "Zero uses the model default context window.", FieldKind::Integer, 0, {},
                           0.0, 2097152.0, "Limits"));
    fields.push_back(field("max_output_tokens", "advanced", "Max Output Tokens Override",
                           "最大输出 Token 覆盖", "Zero uses the model output limit.",
                           FieldKind::Integer, 0, {}, 0.0, 2097152.0, "Limits"));
    fields.push_back(field("ai.extra_headers", "advanced", "Extra Headers", "额外请求头",
                           "Additional provider headers as JSON.", FieldKind::Json, json::object(),
                           {}, {}, {}, "Connection"));
    fields.push_back(field("ai.extra_body", "advanced", "Extra Body", "额外请求体",
                           "Additional provider request properties as JSON.", FieldKind::Json,
                           json::object(), {}, {}, {}, "Connection"));

    fields.push_back(field("ai.custom_models", "models", "Custom Models", "自定义模型",
                           "Model context windows and capability overrides as JSON.",
                           FieldKind::Json, json::object()));

    fields.push_back(
        field("approval", "permissions", "Approval Policy", "审批策略",
              "Default approval policy for tool execution.", FieldKind::Enum, "default",
              {option("default", "Default", "默认"), option("bypass", "Bypass", "跳过确认"),
               option("autopilot", "Autopilot", "自动执行")}));

    fields.push_back(
        field("permissions.read", "permissions", "Read Permission", "读取权限",
              "Default policy for read-only tools.", FieldKind::Enum, "allowed",
              {option("allowed", "Allowed", "允许"), option("confirm", "Confirm", "确认"),
               option("disabled", "Disabled", "禁用")}));
    fields.push_back(
        field("permissions.write", "permissions", "Write Permission", "写入权限",
              "Default policy for workspace mutations.", FieldKind::Enum, "confirm",
              {option("allowed", "Allowed", "允许"), option("confirm", "Confirm", "确认"),
               option("disabled", "Disabled", "禁用")}));
    fields.push_back(
        field("permissions.execute", "permissions", "Execute Permission", "执行权限",
              "Default policy for commands and tools.", FieldKind::Enum, "confirm",
              {option("allowed", "Allowed", "允许"), option("confirm", "Confirm", "确认"),
               option("disabled", "Disabled", "禁用")}));
    fields.push_back(field("permissions.tools", "permissions", "Tool Overrides", "工具权限覆盖",
                           "Per-tool permission overrides as JSON.", FieldKind::Json,
                           json::object()));
    std::vector<FieldMeta> normalized;
    normalized.reserve(fields.size());
    std::unordered_set<std::string> seen;
    for (auto& item : fields) {
        item.key = backend_key_alias(item.key);
        if (item.key == "extra_headers" || item.key == "extra_body")
            item.sensitive = true;
        if (seen.insert(item.key).second)
            normalized.push_back(std::move(item));
    }
    return normalized;
}

FieldKind parse_kind(const json& field_value) {
    const std::string raw =
        ascii_lower(read_text(field_value, {"type", "kind", "valueType", "value_type", "format"}));
    if (field_value.value("writeOnly", false) || raw == "secret" || raw == "password")
        return FieldKind::Secret;
    if (raw == "boolean" || raw == "bool")
        return FieldKind::Boolean;
    if (raw == "integer" || raw == "int")
        return FieldKind::Integer;
    if (raw == "number" || raw == "float" || raw == "double")
        return FieldKind::Number;
    if (raw == "json" || raw == "object" || raw == "map")
        return FieldKind::Json;
    if (raw == "array" || raw == "list" || raw == "string_list")
        return FieldKind::List;
    if (raw == "enum" || raw == "select" || field_value.contains("enum") ||
        field_value.contains("options"))
        return FieldKind::Enum;
    return FieldKind::String;
}

std::vector<FieldOption> parse_options(const json& value) {
    const json* options = nullptr;
    if (const auto options_found = value.find("options");
        options_found != value.end() && options_found->is_array())
        options = &*options_found;
    else if (const auto enum_found = value.find("enum");
             enum_found != value.end() && enum_found->is_array())
        options = &*enum_found;
    std::vector<FieldOption> result;
    if (options == nullptr)
        return result;
    result.reserve(options->size());
    for (const auto& entry : *options) {
        if (entry.is_object()) {
            json option_value;
            if (const auto value_found = entry.find("value"); value_found != entry.end())
                option_value = *value_found;
            else if (const auto id_found = entry.find("id"); id_found != entry.end())
                option_value = *id_found;
            else
                continue;
            std::string label = read_text(entry, {"label", "title", "name"});
            if (label.empty())
                label = option_value.is_string() ? option_value.get<std::string>()
                                                 : option_value.dump();
            result.push_back(option(std::move(option_value), std::move(label),
                                    read_text(entry, {"labelZh", "label_zh", "titleZh"})));
        } else if (entry.is_primitive()) {
            const std::string label = entry.is_string() ? entry.get<std::string>() : entry.dump();
            result.push_back(option(entry, label));
        }
    }
    return result;
}

std::vector<FieldDependency> parse_dependencies(const json& value) {
    const json* dependencies = nullptr;
    for (const char* key : {"dependencies", "dependency", "enabledWhen", "enabled_when"}) {
        const auto found = value.find(key);
        if (found != value.end()) {
            dependencies = &*found;
            break;
        }
    }
    std::vector<FieldDependency> result;
    if (dependencies == nullptr)
        return result;

    const auto append = [&](std::string key, const json& condition) {
        key = trim_copy(key);
        if (key.empty() || key.size() > 512U)
            return;
        json expected = condition;
        if (condition.is_object()) {
            for (const char* expected_key : {"equals", "value", "is"}) {
                const auto found = condition.find(expected_key);
                if (found != condition.end()) {
                    expected = *found;
                    break;
                }
            }
        }
        if (!expected.is_discarded())
            result.push_back({std::move(key), std::move(expected)});
    };

    if (dependencies->is_array()) {
        for (const auto& condition : *dependencies) {
            if (!condition.is_object())
                continue;
            const std::string key = read_text(condition, {"key", "path", "setting"});
            append(key, condition);
        }
    } else if (dependencies->is_object()) {
        const std::string key = read_text(*dependencies, {"key", "path", "setting"});
        if (!key.empty()) {
            append(key, *dependencies);
        } else {
            for (const auto& [dependency_key, condition] : dependencies->items())
                append(dependency_key, condition);
        }
    }
    return result;
}

std::optional<FieldMeta> parse_described_field(const json& value, std::string page_hint,
                                               std::string key_hint = {}) {
    if (!value.is_object())
        return std::nullopt;
    std::string key = read_text(value, {"key", "id", "path", "name"});
    if (key.empty())
        key = std::move(key_hint);
    if (key.empty() || key.size() > 512U)
        return std::nullopt;
    std::string page = read_text(value, {"page", "section", "category"});
    if (page.empty())
        page = std::move(page_hint);
    FieldMeta result;
    result.key = std::move(key);
    result.page = normalize_page(std::move(page));
    result.group = read_text(value, {"group", "subsection"});
    result.label = read_text(value, {"label", "title", "displayName"});
    result.label_zh = read_text(value, {"labelZh", "label_zh", "titleZh"});
    if (result.label.empty())
        result.label = result.key;
    result.description =
        read_text(value, {"description", "help", "summary", "markdownDescription"});
    if (const auto keywords = value.find("keywords"); keywords != value.end()) {
        if (keywords->is_string()) {
            result.keywords = keywords->get<std::string>();
        } else if (keywords->is_array()) {
            for (const auto& keyword : *keywords) {
                if (!keyword.is_string())
                    continue;
                if (!result.keywords.empty())
                    result.keywords.push_back(' ');
                result.keywords.append(keyword.get<std::string>());
            }
        }
    }
    const std::string raw_kind =
        ascii_lower(read_text(value, {"type", "kind", "valueType", "value_type", "format"}));
    result.kind = parse_kind(value);
    result.object_only = raw_kind == "object" || raw_kind == "map" || raw_kind == "json";
    if (const auto default_value = value.find("default"); default_value != value.end())
        result.default_value = *default_value;
    result.options = parse_options(value);
    result.dependencies = parse_dependencies(value);
    for (const char* key_name : {"minimum", "min"}) {
        const auto found = value.find(key_name);
        if (found != value.end() && found->is_number()) {
            result.minimum = found->get<double>();
            break;
        }
    }
    for (const char* key_name : {"maximum", "max"}) {
        const auto found = value.find(key_name);
        if (found != value.end() && found->is_number()) {
            result.maximum = found->get<double>();
            break;
        }
    }
    result.required = value.value("required", false);
    result.sensitive = value.value("secret", false) || value.value("sensitive", false) ||
                       value.value("writeOnly", false) || result.kind == FieldKind::Secret;
    result.advanced = value.value("advanced", false) || result.page == "advanced";
    if (!result.options.empty() && result.kind == FieldKind::String)
        result.kind = FieldKind::Enum;
    return result;
}

std::string title_from_key_part(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8U);
    bool previous_lower = false;
    for (const unsigned char ch : value) {
        if (ch == '_' || ch == '-') {
            if (!result.empty() && result.back() != ' ')
                result.push_back(' ');
            previous_lower = false;
            continue;
        }
        if (ch >= 'A' && ch <= 'Z' && previous_lower)
            result.push_back(' ');
        result.push_back(static_cast<char>(ch));
        previous_lower = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    }
    bool capitalize = true;
    for (char& ch : result) {
        if (capitalize && ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - ('a' - 'A'));
        capitalize = ch == ' ';
    }
    return result;
}

FieldKind kind_from_default(const json& value) {
    if (value.is_boolean())
        return FieldKind::Boolean;
    if (value.is_number_integer() || value.is_number_unsigned())
        return FieldKind::Integer;
    if (value.is_number_float())
        return FieldKind::Number;
    if (value.is_array())
        return FieldKind::List;
    if (value.is_object())
        return FieldKind::Json;
    return FieldKind::String;
}

void apply_field_hint(FieldMeta& item, const FieldMeta& hint) {
    if (item.page == "advanced" && hint.page != "advanced")
        item.page = hint.page;
    if (item.label == item.key || item.label.empty())
        item.label = hint.label;
    if (item.label_zh.empty())
        item.label_zh = hint.label_zh;
    if (item.description.empty())
        item.description = hint.description;
    if (item.group.empty())
        item.group = hint.group;
    if (item.options.empty()) {
        item.options = hint.options;
    } else {
        for (auto& described_option : item.options) {
            const auto found = std::ranges::find_if(
                hint.options, [&described_option](const FieldOption& candidate) {
                    return candidate.value == described_option.value;
                });
            if (found == hint.options.end())
                continue;
            if (described_option.label.empty() ||
                described_option.label == described_option.value.dump()) {
                described_option.label = found->label;
            }
            if (described_option.label_zh.empty())
                described_option.label_zh = found->label_zh;
        }
    }
    if (item.dependencies.empty())
        item.dependencies = hint.dependencies;
    if (!item.minimum.has_value())
        item.minimum = hint.minimum;
    if (!item.maximum.has_value())
        item.maximum = hint.maximum;
    if (!item.sensitive && hint.sensitive)
        item.sensitive = true;
    if (!item.advanced && hint.advanced)
        item.advanced = true;
    if (item.kind == FieldKind::String && hint.kind == FieldKind::Enum && !hint.options.empty())
        item.kind = FieldKind::Enum;
}

void append_object_leaf_fields(const FieldMeta& parent, const json& value,
                               std::string_view relative_prefix, std::vector<FieldMeta>& out) {
    if (!value.is_object())
        return;
    for (auto entry = value.begin(); entry != value.end(); ++entry) {
        const std::string relative = relative_prefix.empty()
                                         ? entry.key()
                                         : std::string(relative_prefix) + "." + entry.key();
        if (entry.value().is_object() && !entry.value().empty()) {
            append_object_leaf_fields(parent, entry.value(), relative, out);
            continue;
        }
        FieldMeta leaf;
        leaf.key = parent.key + "." + relative;
        leaf.page = parent.page;
        leaf.group =
            parent.label_zh.empty() ? parent.label : parent.label + " / " + parent.label_zh;
        leaf.label = title_from_key_part(entry.key());
        leaf.description = parent.description;
        leaf.keywords = parent.keywords;
        leaf.kind = kind_from_default(entry.value());
        leaf.default_value = entry.value();
        leaf.required = false;
        leaf.sensitive = parent.sensitive;
        leaf.advanced = parent.advanced;
        leaf.dependencies = parent.dependencies;
        out.push_back(std::move(leaf));
    }
}

void append_permission_leaf_fields(const FieldMeta& parent, std::vector<FieldMeta>& out) {
    const auto permission_options = std::vector<FieldOption>{
        option("allowed", "Allowed", "允许"), option("confirm", "Confirm", "确认"),
        option("disabled", "Disabled", "禁用")};
    for (const auto& [suffix, label, label_zh, description] : std::array<
             std::tuple<std::string_view, std::string_view, std::string_view, std::string_view>, 3>{
             {
                 {"read", "Read Permission", "读取权限", "Default policy for read-only tools."},
                 {"write", "Write Permission", "写入权限", "Default policy for workspace changes."},
                 {"execute", "Execute Permission", "执行权限", "Default policy for commands."},
             }}) {
        FieldMeta leaf;
        leaf.key = parent.key + "." + std::string(suffix);
        leaf.page = parent.page;
        leaf.group = "Permission Categories / 权限类别";
        leaf.label = std::string(label);
        leaf.label_zh = std::string(label_zh);
        leaf.description = std::string(description);
        leaf.kind = FieldKind::Enum;
        leaf.options = permission_options;
        leaf.advanced = parent.advanced;
        leaf.dependencies = parent.dependencies;
        out.push_back(std::move(leaf));
    }
    FieldMeta tools;
    tools.key = parent.key + ".tools";
    tools.page = parent.page;
    tools.group = "Tool Overrides / 工具覆盖";
    tools.label = "Tool Overrides";
    tools.label_zh = "工具权限覆盖";
    tools.description = "Per-tool permission overrides as JSON.";
    tools.kind = FieldKind::Json;
    tools.advanced = parent.advanced;
    tools.dependencies = parent.dependencies;
    out.push_back(std::move(tools));
}

void collect_schema_properties(const json& properties, std::string_view prefix,
                               std::string page_hint, std::vector<FieldMeta>& out) {
    if (!properties.is_object())
        return;
    for (auto item = properties.begin(); item != properties.end(); ++item) {
        if (!item.value().is_object())
            continue;
        const std::string key =
            prefix.empty() ? item.key() : std::string(prefix) + "." + item.key();
        std::string page = read_text(item.value(), {"page", "section", "category"});
        if (page.empty())
            page = page_hint;
        const auto nested = item.value().find("properties");
        if (nested != item.value().end() && nested->is_object() && !nested->empty()) {
            collect_schema_properties(*nested, key, page, out);
            continue;
        }
        if (auto parsed = parse_described_field(item.value(), page, key))
            out.push_back(std::move(*parsed));
    }
}

void collect_described_fields(const json& describe, std::vector<FieldMeta>& out) {
    if (!describe.is_object())
        return;
    if (const auto sections = describe.find("sections");
        sections != describe.end() && sections->is_array()) {
        for (const auto& section : *sections) {
            if (!section.is_object())
                continue;
            const std::string page = read_text(section, {"id", "page", "key", "title", "label"});
            const auto fields = section.find("fields");
            if (fields == section.end())
                continue;
            if (fields->is_array()) {
                for (const auto& item : *fields) {
                    if (auto parsed = parse_described_field(item, page))
                        out.push_back(std::move(*parsed));
                }
            } else if (fields->is_object()) {
                for (auto item = fields->begin(); item != fields->end(); ++item) {
                    if (auto parsed = parse_described_field(item.value(), page, item.key()))
                        out.push_back(std::move(*parsed));
                }
            }
        }
    }
    if (const auto fields = describe.find("fields"); fields != describe.end()) {
        if (fields->is_array()) {
            for (const auto& item : *fields) {
                if (auto parsed = parse_described_field(item, {}))
                    out.push_back(std::move(*parsed));
            }
        } else if (fields->is_object()) {
            for (auto item = fields->begin(); item != fields->end(); ++item) {
                if (auto parsed = parse_described_field(item.value(), {}, item.key()))
                    out.push_back(std::move(*parsed));
            }
        }
    }
    const json* schema = nullptr;
    if (const auto found = describe.find("schema"); found != describe.end())
        schema = &*found;
    if (schema != nullptr && schema->is_object()) {
        const json* properties = schema;
        if (const auto found = schema->find("properties");
            found != schema->end() && found->is_object())
            properties = &*found;
        if (properties->is_object()) {
            collect_schema_properties(*properties, {}, {}, out);
        }
    }
}

const json* lookup_value(const json& root, std::string_view key);
void set_value(json& root, std::string_view key, json value);
json object_from(const json& result, std::initializer_list<const char*> keys);

void apply_field_default(FieldMeta& item, const json& described_defaults, const FieldMeta* hint) {
    if (!item.default_value.is_null())
        return;
    if (const json* value = lookup_value(described_defaults, item.key); value != nullptr) {
        item.default_value = *value;
        return;
    }
    if (hint != nullptr && !hint->default_value.is_null())
        item.default_value = hint->default_value;
}

void merge_described_fields(AiEditorSettingsPanelState& state, const json& describe) {
    std::vector<FieldMeta> described;
    collect_described_fields(describe, described);
    if (described.empty())
        return;

    const std::vector<FieldMeta> hints = field_hints();
    std::unordered_map<std::string, const FieldMeta*> hints_by_key;
    for (const auto& hint : hints) {
        hints_by_key.try_emplace(hint.key, &hint);
        hints_by_key.try_emplace(backend_key_alias(hint.key), &hint);
    }
    const json described_defaults =
        object_from(describe, {"defaults", "defaultValues", "default_values"});

    std::vector<FieldMeta> expanded;
    expanded.reserve(described.size() * 2U);
    std::unordered_set<std::string> described_keys;
    for (auto& item : described) {
        if (!described_keys.insert(item.key).second)
            continue;
        const FieldMeta* hint = nullptr;
        if (const auto found = hints_by_key.find(item.key); found != hints_by_key.end()) {
            hint = found->second;
            apply_field_hint(item, *found->second);
        }
        apply_field_default(item, described_defaults, hint);

        if (item.key == "permissions" && item.kind == FieldKind::Json) {
            append_permission_leaf_fields(item, expanded);
            continue;
        }
        if (item.kind == FieldKind::Json && !item.sensitive && item.default_value.is_object() &&
            !item.default_value.empty()) {
            const size_t before = expanded.size();
            append_object_leaf_fields(item, item.default_value, {}, expanded);
            if (expanded.size() != before)
                continue;
        }
        expanded.push_back(std::move(item));
    }

    if (const auto presets = describe.find("providerPresets");
        presets != describe.end() && presets->is_object()) {
        for (auto preset = presets->begin(); preset != presets->end(); ++preset) {
            const auto provider =
                std::ranges::find(expanded, std::string{"provider"}, &FieldMeta::key);
            if (provider != expanded.end() &&
                std::ranges::none_of(provider->options, [&preset](const FieldOption& item) {
                    return item.value.is_string() && item.value.get<std::string>() == preset.key();
                })) {
                provider->options.push_back(
                    option(preset.key(), title_from_key_part(preset.key())));
            }
            const std::string key = "provider_keys." + preset.key();
            if (!described_keys.insert(key).second)
                continue;
            FieldMeta provider_key;
            provider_key.key = key;
            provider_key.page = "endpoint";
            provider_key.group = "Provider Keys / 提供商密钥";
            provider_key.label = title_from_key_part(preset.key()) + " API Key";
            provider_key.description = "Protected provider-specific credential.";
            provider_key.kind = FieldKind::Secret;
            provider_key.sensitive = true;
            if (const auto found = hints_by_key.find(key); found != hints_by_key.end())
                apply_field_hint(provider_key, *found->second);
            expanded.push_back(std::move(provider_key));
        }
    }

    std::unordered_set<std::string> final_keys;
    std::vector<FieldMeta> final_fields;
    final_fields.reserve(expanded.size());
    for (auto& item : expanded) {
        if (!final_keys.insert(item.key).second)
            continue;
        const FieldMeta* hint = nullptr;
        if (const auto found = hints_by_key.find(item.key); found != hints_by_key.end()) {
            hint = found->second;
            apply_field_hint(item, *found->second);
        }
        apply_field_default(item, described_defaults, hint);
        final_fields.push_back(std::move(item));
    }

    std::ranges::stable_sort(final_fields, [](const FieldMeta& left, const FieldMeta& right) {
        const size_t left_page = page_index(left.page);
        const size_t right_page = page_index(right.page);
        if (left_page != right_page)
            return left_page < right_page;
        if (left.group != right.group)
            return left.group < right.group;
        return left.label < right.label;
    });
    json defaults_with_hints = described_defaults;
    for (const auto& item : final_fields) {
        if (lookup_value(defaults_with_hints, item.key) == nullptr &&
            !item.default_value.is_null()) {
            set_value(defaults_with_hints, item.key, item.default_value);
        }
    }
    state.fields = std::move(final_fields);
    state.defaults = std::move(defaults_with_hints);
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
    const auto direct = root.find(std::string(key));
    if (direct != root.end())
        return &*direct;
    const std::vector<std::string> parts = split_key(key);
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

void set_value(json& root, std::string_view key, json value) {
    if (!root.is_object())
        root = json::object();
    const std::string direct_key(key);
    if (root.contains(direct_key)) {
        root[direct_key] = std::move(value);
        return;
    }
    const std::vector<std::string> parts = split_key(key);
    if (parts.empty()) {
        root[direct_key] = std::move(value);
        return;
    }
    json* current = &root;
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
        json& child = (*current)[parts[index]];
        if (!child.is_object())
            child = json::object();
        current = &child;
    }
    (*current)[parts.back()] = std::move(value);
}

void merge_object_patch(json& destination, const json& patch) {
    if (!destination.is_object())
        destination = json::object();
    if (!patch.is_object())
        return;
    for (const auto& [key, value] : patch.items()) {
        if (destination.contains(key) && destination[key].is_object() && value.is_object())
            merge_object_patch(destination[key], value);
        else
            destination[key] = value;
    }
}

bool erase_value(json& root, std::string_view key) {
    if (!root.is_object())
        return false;
    const std::string direct_key(key);
    if (root.erase(direct_key) != 0)
        return true;
    const std::vector<std::string> parts = split_key(key);
    if (parts.empty())
        return false;
    json* current = &root;
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
        const auto found = current->find(parts[index]);
        if (found == current->end() || !found->is_object())
            return false;
        current = &*found;
    }
    return current->erase(parts.back()) != 0;
}

json object_from(const json& result, std::initializer_list<const char*> keys) {
    if (!result.is_object())
        return json::object();
    for (const char* key : keys) {
        const auto found = result.find(key);
        if (found != result.end() && found->is_object())
            return *found;
    }
    return json::object();
}

json settings_values_from(const json& result) {
    json values = object_from(result, {"values", "settings", "config", "draft", "data"});
    if (!values.empty() || !result.is_object())
        return values;
    const bool looks_like_envelope = result.contains("ok") || result.contains("scope") ||
                                     result.contains("secretStates") ||
                                     result.contains("defaults") || result.contains("sources") ||
                                     result.contains("effective") || result.contains("overrides");
    return looks_like_envelope ? json::object() : result;
}

json scope_overrides_from(const json& result, bool& exact) {
    json overrides = object_from(result, {"overrides", "scopeOverrides", "scope_overrides",
                                          "scopeValues", "scope_values", "selectedScopeValues",
                                          "selected_scope_values", "rawSettings", "raw_settings"});
    exact = !overrides.empty() || result.contains("overrides") ||
            result.contains("scopeOverrides") || result.contains("scope_overrides") ||
            result.contains("scopeValues") || result.contains("scope_values") ||
            result.contains("selectedScopeValues") || result.contains("selected_scope_values") ||
            result.contains("rawSettings") || result.contains("raw_settings");
    return exact ? overrides : settings_values_from(result);
}

json effective_values_from(const json& result) {
    json effective = object_from(result, {"effective", "effectiveValues", "effective_values",
                                          "merged", "mergedValues", "merged_values", "resolved",
                                          "resolvedValues", "resolved_values"});
    if (!effective.empty() || result.contains("effective") || result.contains("effectiveValues") ||
        result.contains("effective_values") || result.contains("merged") ||
        result.contains("mergedValues") || result.contains("merged_values") ||
        result.contains("resolved") || result.contains("resolvedValues") ||
        result.contains("resolved_values")) {
        return effective;
    }
    const std::string scope = ascii_lower(read_text(result, {"scope"}));
    return scope == "merged" ? settings_values_from(result) : json::object();
}

json inherited_values_from(const json& result) {
    return object_from(result, {"inherited", "inheritedValues", "inherited_values", "parentValues",
                                "parent_values"});
}

json sources_from(const json& result) {
    return object_from(result, {"sources", "valueSources", "value_sources", "provenance"});
}

std::string status_from_json(const json& value) {
    if (value.is_boolean())
        return value.get<bool>() ? "set" : "unset";
    if (value.is_string()) {
        const std::string status = ascii_lower(value.get<std::string>());
        if (status == "set" || status == "configured" || status == "present" || status == "true" ||
            status == "yes" || status == "已设置")
            return "set";
        if (status == "clear" || status == "cleared" || status == "pending_clear")
            return "clear";
        return status.empty() ? "unset" : "set";
    }
    if (value.is_object()) {
        for (const char* key : {"status", "state", "configured", "isSet", "set"}) {
            const auto found = value.find(key);
            if (found != value.end())
                return status_from_json(*found);
        }
    }
    return "unset";
}

bool is_secret_field(const FieldMeta& field) noexcept {
    return field.sensitive;
}

json described_values_from(const AiEditorSettingsPanelState& state, const json& values,
                           bool include_sensitive = false) {
    json filtered = json::object();
    for (const auto& field : state.fields) {
        if (!include_sensitive && is_secret_field(field))
            continue;
        if (const json* value = lookup_value(values, field.key); value != nullptr)
            set_value(filtered, field.key, *value);
    }
    return filtered;
}

bool state_dirty(const AiEditorSettingsPanelState& state) {
    return state.persisted != state.draft || !state.reset_keys.empty() ||
           !state.pending_secret_values.empty() || !state.pending_secret_clears.empty();
}

bool followup_load_active(const AiEditorSettingsPanelState& state) noexcept {
    return state.raw_load_pending || state.effective_load_pending ||
           state.rpc_kind == RpcKind::LoadRawScope ||
           state.rpc_kind == RpcKind::LoadEffective;
}

const FieldMeta* find_field(const AiEditorSettingsPanelState& state,
                            std::string_view key) noexcept {
    const auto found = std::ranges::find(state.fields, key, &FieldMeta::key);
    return found == state.fields.end() ? nullptr : &*found;
}

std::string option_label(const FieldMeta& field, const json& value) {
    const auto found = std::ranges::find_if(
        field.options, [&value](const FieldOption& item) { return item.value == value; });
    if (found == field.options.end())
        return value.is_string() ? value.get<std::string>() : value.dump();
    if (!found->label_zh.empty())
        return found->label + " / " + found->label_zh;
    return found->label;
}

std::string compact_text(std::string value, size_t maximum = 180U) {
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    if (value.size() > maximum) {
        value.resize(maximum);
        value.append("...");
    }
    return value;
}

std::string secret_display(const AiEditorSettingsPanelState& state, const FieldMeta& field) {
    if (state.pending_secret_clears.contains(field.key))
        return "待清除 / pending clear";
    if (state.pending_secret_values.contains(field.key))
        return "待保存 / pending save";
    const auto found = state.secret_states.find(field.key);
    if (found != state.secret_states.end() && found->second == "set")
        return "已设置 / configured";
    return "未设置 / not configured";
}

std::string display_json_value(const FieldMeta& field, const json* value) {
    if (value == nullptr || value->is_null())
        return "未设置 / unset";
    if (field.kind == FieldKind::Boolean && value->is_boolean())
        return value->get<bool>() ? "开启 / Enabled" : "关闭 / Disabled";
    if (field.kind == FieldKind::Enum)
        return option_label(field, *value);
    if (value->is_string())
        return value->get<std::string>().empty() ? "空 / empty"
                                                 : compact_text(value->get<std::string>());
    if (value->is_array())
        return std::to_string(value->size()) + " items · " + compact_text(value->dump());
    if (value->is_object())
        return std::to_string(value->size()) + " keys · " + compact_text(value->dump());
    return compact_text(value->dump());
}

std::string source_text(const json* source) {
    if (source == nullptr)
        return {};
    if (source->is_string())
        return source->get<std::string>();
    if (source->is_object())
        return read_text(*source, {"$scope", "scope", "source", "label", "origin"});
    return {};
}

bool selected_scope_has_override(const AiEditorSettingsPanelState& state, const FieldMeta& field) {
    if (state.reset_keys.contains(field.key))
        return false;
    const json* draft_value = lookup_value(state.draft, field.key);
    const json* persisted_value = lookup_value(state.persisted, field.key);
    if (draft_value != nullptr &&
        (persisted_value == nullptr || *draft_value != *persisted_value)) {
        return true;
    }
    if (state.scope_overrides_exact)
        return draft_value != nullptr;
    const std::string source = ascii_lower(source_text(lookup_value(state.sources, field.key)));
    if (!source.empty()) {
        if (state.selected_scope == "plugin")
            return source == "plugin" || source == ascii_lower(state.selected_plugin_id) ||
                   source == "plugin:" + ascii_lower(state.selected_plugin_id);
        return source == state.selected_scope;
    }
    return state.selected_scope == "system" && lookup_value(state.draft, field.key) != nullptr;
}

const json* effective_value(const AiEditorSettingsPanelState& state, const FieldMeta& field) {
    if (state.reset_keys.contains(field.key)) {
        if (const json* inherited_value = lookup_value(state.inherited, field.key);
            inherited_value != nullptr) {
            return inherited_value;
        }
        return state.selected_scope == "system" && !field.default_value.is_null()
                   ? &field.default_value
                   : nullptr;
    }
    {
        if (const json* draft_value = lookup_value(state.draft, field.key); draft_value != nullptr)
            return draft_value;
    }
    if (const json* value = lookup_value(state.effective, field.key); value != nullptr)
        return value;
    if (const json* value = lookup_value(state.inherited, field.key); value != nullptr)
        return value;
    return field.default_value.is_null() ? nullptr : &field.default_value;
}

const json* dependency_value(const AiEditorSettingsPanelState& state,
                             std::string_view key) {
    if (const FieldMeta* dependency = find_field(state, key); dependency != nullptr)
        return effective_value(state, *dependency);
    for (const json* root : {&state.draft, &state.effective, &state.inherited, &state.defaults}) {
        if (const json* value = lookup_value(*root, key); value != nullptr)
            return value;
    }
    return nullptr;
}

bool field_dependencies_satisfied(const AiEditorSettingsPanelState& state,
                                  const FieldMeta& field) {
    return std::ranges::all_of(field.dependencies, [&](const FieldDependency& dependency) {
        const json* current = dependency_value(state, dependency.key);
        return current != nullptr && *current == dependency.expected;
    });
}

std::string dependency_display(const AiEditorSettingsPanelState& state,
                               const FieldMeta& field) {
    std::string result;
    for (const auto& dependency : field.dependencies) {
        if (!result.empty())
            result.append("; ");
        result.append(dependency.key).append(" = ");
        result.append(dependency.expected.is_string()
                          ? dependency.expected.get<std::string>()
                          : dependency.expected.dump());
        const json* current = dependency_value(state, dependency.key);
        result.append(" (current: ");
        result.append(current == nullptr
                          ? "unset"
                          : current->is_string() ? current->get<std::string>() : current->dump());
        result.push_back(')');
    }
    return result;
}

std::string override_display(const AiEditorSettingsPanelState& state, const FieldMeta& field) {
    if (state.reset_keys.contains(field.key))
        return "待重置（保存后继承） / reset queued";
    if (is_secret_field(field))
        return secret_display(state, field);
    if (!selected_scope_has_override(state, field))
        return !state.scope_overrides_exact && lookup_value(state.draft, field.key) != nullptr
                   ? "作用域快照（旧后端未区分 raw override）: " +
                         display_json_value(field, lookup_value(state.draft, field.key))
                   : "无覆盖 / no override";
    return display_json_value(field, lookup_value(state.draft, field.key));
}

std::string effective_display(const AiEditorSettingsPanelState& state, const FieldMeta& field) {
    if (is_secret_field(field)) {
        if (state.pending_secret_values.contains(field.key))
            return "保存后使用新受保护值 / pending protected value";
        if (state.pending_secret_clears.contains(field.key)) {
            const auto inherited = state.inherited_secret_states.find(field.key);
            return inherited != state.inherited_secret_states.end() && inherited->second == "set"
                       ? "保存后继承受保护值 / inherited protected value"
                       : "保存后未配置 / not configured";
        }
        const auto effective = state.effective_secret_states.find(field.key);
        if (effective != state.effective_secret_states.end())
            return effective->second == "set" ? "已配置 / configured" : "未配置 / unset";
        return secret_display(state, field);
    }
    if (state.reset_keys.contains(field.key) && state.selected_scope != "system" &&
        lookup_value(state.inherited, field.key) == nullptr) {
        return "保存后由父级作用域重新解析 / resolves from parent after Apply";
    }
    return display_json_value(field, effective_value(state, field));
}

std::string inheritance_display(const AiEditorSettingsPanelState& state, const FieldMeta& field) {
    if (state.reset_keys.contains(field.key))
        return "保存后恢复继承 / inherits after Apply";
    if (selected_scope_has_override(state, field))
        return "当前作用域覆盖 / overridden in selected scope";
    const std::string source = source_text(lookup_value(state.sources, field.key));
    if (!source.empty())
        return "继承自 " + source + " / inherited from " + source;
    if (state.selected_scope == "system")
        return "系统值或 schema 默认 / system or schema default";
    if (state.scope_overrides_exact)
        return "继承 / inherited";
    return "继承状态未由旧后端标注 / inheritance source unavailable";
}

std::string value_for_dialog(const AiEditorSettingsPanelState& state, const FieldMeta& field) {
    if (field.kind == FieldKind::Secret)
        return {};
    const json* value = lookup_value(state.draft, field.key);
    if (state.reset_keys.contains(field.key) || value == nullptr || value->is_null())
        value = effective_value(state, field);
    if (value == nullptr)
        return {};
    if (value->is_string())
        return value->get<std::string>();
    if (field.kind == FieldKind::List && value->is_array()) {
        std::string text;
        for (const auto& item : *value) {
            if (!text.empty())
                text.push_back('\n');
            text.append(item.is_string() ? item.get<std::string>() : item.dump());
        }
        return text;
    }
    return value->dump(2);
}

std::string field_search_text(const FieldMeta& field) {
    std::string text = field.label + " " + field.label_zh + " " + field.key + " " +
                       field.description + " " + field.keywords + " " + field.group;
    if (const auto* page = page_definition(field.page); page != nullptr) {
        text.append(" ");
        text.append(page->label);
        text.append(" ");
        text.append(page->description);
    }
    for (const auto& item : field.options) {
        text.append(" ");
        text.append(item.label);
        text.append(" ");
        text.append(item.label_zh);
    }
    return ascii_lower(std::move(text));
}

bool field_is_advanced(const FieldMeta& field) noexcept {
    return field.advanced || field.page == "advanced";
}

bool field_has_pending_change(const AiEditorSettingsPanelState& state,
                              const FieldMeta& field) {
    if (state.reset_keys.contains(field.key) ||
        state.pending_secret_values.contains(field.key) ||
        state.pending_secret_clears.contains(field.key)) {
        return true;
    }
    const json* draft = lookup_value(state.draft, field.key);
    const json* persisted = lookup_value(state.persisted, field.key);
    if (draft == nullptr || persisted == nullptr)
        return draft != persisted;
    return *draft != *persisted;
}

bool field_matches_review_filter(const AiEditorSettingsPanelState& state,
                                 const FieldMeta& field, std::string_view filter) {
    if (filter == "modified")
        return field_has_pending_change(state, field);
    if (filter == "overrides")
        return selected_scope_has_override(state, field);
    if (filter == "errors")
        return state.validation_errors.contains(field.key);
    return true;
}

size_t review_filter_count(const AiEditorSettingsPanelState& state, std::string_view filter) {
    return static_cast<size_t>(std::ranges::count_if(state.fields, [&](const FieldMeta& field) {
        return field_matches_review_filter(state, field, filter);
    }));
}

std::vector<const FieldMeta*> visible_fields(const AiEditorSettingsPanelState& state) {
    std::vector<const FieldMeta*> result;
    const std::string query = ascii_lower(trim_copy(state.search_query));
    for (const auto& field : state.fields) {
        if (!field_matches_review_filter(state, field, state.review_filter))
            continue;
        if (field_is_advanced(field) && !state.show_advanced && query.empty() &&
            state.active_page != "advanced") {
            continue;
        }
        if (query.empty()) {
            if (state.review_filter != "all" || field.page == state.active_page)
                result.push_back(&field);
        } else if (field_search_text(field).find(query) != std::string::npos) {
            result.push_back(&field);
        }
    }
    return result;
}

std::string summary_value(const AiEditorSettingsPanelState& state,
                          std::initializer_list<const char*> keys, std::string fallback) {
    for (const char* key : keys) {
        for (const json* root :
             {&state.draft, &state.effective, &state.inherited, &state.defaults}) {
            if (const json* value = lookup_value(*root, key); value != nullptr) {
                if (value->is_string() && !value->get<std::string>().empty())
                    return value->get<std::string>();
                if (!value->is_null())
                    return compact_text(value->dump(), 80U);
            }
        }
    }
    return fallback;
}

std::string save_phase_text(SavePhase phase) {
    switch (phase) {
    case SavePhase::Idle:
        return "idle";
    case SavePhase::Saving:
        return "saving";
    case SavePhase::Saved:
        return "saved";
    case SavePhase::Failed:
        return "failed";
    }
    return "idle";
}

std::string save_phase_style(SavePhase phase) {
    switch (phase) {
    case SavePhase::Saving:
        return "warn";
    case SavePhase::Saved:
        return "ok";
    case SavePhase::Failed:
        return "bad";
    case SavePhase::Idle:
        return "muted";
    }
    return "muted";
}

std::string rpc_kind_text(RpcKind kind) {
    switch (kind) {
    case RpcKind::Describe:
        return "loading schema";
    case RpcKind::LoadScope:
        return "loading scope";
    case RpcKind::LoadRawScope:
        return "loading raw overrides";
    case RpcKind::LoadEffective:
        return "loading effective";
    case RpcKind::Save:
        return "saving";
    case RpcKind::None:
        return "idle";
    }
    return "idle";
}

std::string scope_label(std::string_view scope) {
    if (scope == "system")
        return "System / 系统";
    if (scope == "plugin")
        return "Plugin / 插件";
    return "Workspace / 工作区";
}

json build_overview(const AiEditorSettingsPanelState& state) {
    json children = json::array();
    json connection = json::array();
    connection.push_back(text_node("Backend / 后端", "title", 24));
    connection.push_back(
        badge_node(state.backend_connected ? "Connected / 已连接" : "Offline / 离线",
                   state.backend_connected ? "ok" : "warn"));
    connection.push_back(text_node(state.backend_message.empty()
                                       ? (state.backend_connected
                                              ? "Named-pipe JSON-RPC is ready."
                                              : "后端未连接；草稿仍会保留，连接后可 Reload/Apply。")
                                       : state.backend_message,
                                   state.backend_connected ? "muted" : "warn", 34));
    children.push_back(
        card_node("Backend", std::move(connection), state.backend_connected ? "ok" : "warn"));

    json assistant = json::array();
    assistant.push_back(text_node("Active AI / 当前 AI", "title", 24));
    assistant.push_back(text_node(
        "Provider: " + summary_value(state, {"ai.provider", "ai_editor.provider", "provider"},
                                     "not configured"),
        "value"));
    assistant.push_back(
        text_node("Model: " + summary_value(state, {"ai.model", "ai_editor.model", "model"},
                                            "not configured"),
                  "value"));
    assistant.push_back(text_node(
        "Mode: " + summary_value(state, {"ai.mode", "ai_editor.mode", "mode"}, "agent"), "value"));
    json assistant_actions = json::array();
    assistant_actions.push_back(button_node("overview.endpoint", "AI Endpoint", "nav.page",
                                            {{"page", "endpoint"}}, "primary"));
    assistant_actions.push_back(
        button_node("overview.permissions", "Permissions", "nav.page", {{"page", "permissions"}}));
    assistant.push_back(row_node(std::move(assistant_actions)));
    children.push_back(card_node("Active AI", std::move(assistant), "cyan"));

    size_t configured_secrets = 0;
    for (const auto& [key, status] : state.secret_states) {
        (void)key;
        if (status == "set")
            ++configured_secrets;
    }
    json draft = json::array();
    draft.push_back(text_node("Draft / 草稿", "title", 24));
    draft.push_back(badge_node(state_dirty(state) ? "Dirty / 有未保存更改" : "Clean / 已同步",
                               state_dirty(state) ? "warn" : "ok"));
    draft.push_back(text_node("Scope: " + scope_label(state.selected_scope) +
                                  " · Save state: " + save_phase_text(state.save_phase),
                              "value"));
    draft.push_back(text_node(
        "Secrets configured: " + std::to_string(configured_secrets) +
            " · pending secret updates: " + std::to_string(state.pending_secret_values.size()) +
            " · pending clears: " + std::to_string(state.pending_secret_clears.size()),
        "muted"));
    if (state.hidden_with_dirty)
        draft.push_back(text_node(
            "提示：面板上次关闭时存在脏草稿；内容未丢失，请 Apply 或 Discard。", "warn", 34));
    children.push_back(card_node("Draft", std::move(draft), state_dirty(state) ? "warn" : "ok"));

    json quick = json::array();
    quick.push_back(text_node("Quick Settings / 快速设置", "title", 24));
    json quick_buttons = json::array();
    quick_buttons.push_back(
        button_node("overview.editor", "Editor & Files", "nav.page", {{"page", "editor"}}));
    quick_buttons.push_back(button_node("overview.mcp", "MCP", "nav.page", {{"page", "mcp"}}));
    quick_buttons.push_back(
        button_node("overview.advanced", "Advanced", "nav.page", {{"page", "advanced"}}));
    quick_buttons.push_back(
        button_node("overview.models", "Custom Models", "nav.page", {{"page", "models"}}));
    quick.push_back(row_node(std::move(quick_buttons)));
    children.push_back(card_node("Quick Settings", std::move(quick), "gold"));
    return section_node("Overview", std::move(children));
}

json build_field_card(const AiEditorSettingsPanelState& state, const FieldMeta& field) {
    json children = json::array();
    std::string title = field.label;
    if (!field.label_zh.empty() && field.label_zh != field.label)
        title.append(" / ").append(field.label_zh);
    children.push_back(text_node(std::move(title), "title", 24));
    children.push_back(text_node("Key: " + field.key, "mono", 22));
    json state_badges = json::array();
    const bool has_override = selected_scope_has_override(state, field);
    const bool dependencies_met = field_dependencies_satisfied(state, field);
    state_badges.push_back(badge_node(has_override ? "Override / 当前作用域覆盖"
                                                   : "Inherited / 继承",
                                      has_override ? "accent" : "muted"));
    if (field_has_pending_change(state, field))
        state_badges.push_back(badge_node("Modified / 已修改", "warn"));
    if (field_is_advanced(field))
        state_badges.push_back(badge_node("Advanced / 高级", "muted"));
    if (is_secret_field(field))
        state_badges.push_back(badge_node("Protected / 受保护", "ok"));
    if (!dependencies_met)
        state_badges.push_back(badge_node("Unavailable / 条件未满足", "warn"));
    children.push_back(row_node(std::move(state_badges)));
    const auto validation = state.validation_errors.find(field.key);
    children.push_back(text_node("Effective / 生效值: " + effective_display(state, field),
                                 validation == state.validation_errors.end() ? "value" : "bad",
                                 30));
    children.push_back(
        text_node("Selected scope override / 当前作用域覆盖: " + override_display(state, field),
                  "muted", 30));
    children.push_back(text_node("Inheritance / 继承: " + inheritance_display(state, field),
                                 has_override ? "accent" : "muted", 30));
    if (!field.description.empty())
        children.push_back(text_node(field.description, "muted", 38));
    if (!field.dependencies.empty()) {
        children.push_back(text_node(
            (dependencies_met ? "Condition met / 条件已满足: "
                              : "Available when / 满足以下条件后可用: ") +
                dependency_display(state, field),
            dependencies_met ? "accent" : "warn", 38));
    }
    if (validation != state.validation_errors.end())
        children.push_back(text_node("Validation: " + validation->second, "bad", 34));

    json actions = json::array();
    const std::string token = stable_token(field.key);
    const bool interaction_locked = !state.values_loaded || state.save_phase == SavePhase::Saving ||
                                    state.rpc_kind != RpcKind::None || state.save_pending ||
                                    followup_load_active(state) || !dependencies_met;
    if (field.kind == FieldKind::Boolean) {
        actions.push_back(button_node("field." + token + ".toggle", "Toggle / 切换", "field.toggle",
                                      {{"key", field.key}}, "primary", interaction_locked));
    } else if (field.kind == FieldKind::Enum && !field.options.empty()) {
        const json* current = effective_value(state, field);
        const size_t maximum_options = std::min<size_t>(field.options.size(), 10U);
        for (size_t index = 0; index < maximum_options; ++index) {
            const FieldOption& item = field.options[index];
            std::string label = item.label;
            if (!item.label_zh.empty())
                label.append(" / ").append(item.label_zh);
            const bool active = current != nullptr && *current == item.value;
            actions.push_back(button_node("field." + token + ".option." + std::to_string(index),
                                          std::move(label), "field.select",
                                          {{"key", field.key}, {"value", item.value}},
                                          active ? "primary" : "default", interaction_locked));
        }
        if (field.options.size() > maximum_options) {
            actions.push_back(button_node("field." + token + ".edit", "Select... / 选择...",
                                          "field.edit", {{"key", field.key}}, "default",
                                          interaction_locked));
        }
    } else {
        actions.push_back(button_node(
            "field." + token + ".edit",
            is_secret_field(field) ? "Set Protected Value / 设置受保护值" : "Edit / 编辑",
            "field.edit", {{"key", field.key}}, "primary", interaction_locked));
    }
    if (is_secret_field(field)) {
        actions.push_back(button_node("field." + token + ".clear", "Clear / 清除", "field.clear",
                                      {{"key", field.key}}, "danger", interaction_locked));
    }
    actions.push_back(button_node("field." + token + ".reset", "Reset / 重置", "field.reset",
                                  {{"key", field.key}}, "ghost", interaction_locked));
    children.push_back(row_node(std::move(actions)));
    return card_node(field.key, std::move(children),
                     validation == state.validation_errors.end() ? "cyan" : "bad");
}

std::string build_panel_spec(const AiEditorSettingsPanelState& state) {
    json nodes = json::array();
    const bool dirty = state_dirty(state);
    const bool synchronization_locked = state.rpc_kind != RpcKind::None || state.save_pending ||
                                        followup_load_active(state);
    nodes.push_back(text_node(dirty ? "AI Editor Settings *" : "AI Editor Settings",
                              dirty ? "warn" : "title", 30));
    nodes.push_back(text_node("Scope priority: Plugin > Workspace > System. Only the selected "
                              "scope is written; higher scopes override lower scopes.",
                              "muted", 34));

    json status_row = json::array();
    status_row.push_back(
        badge_node(state.backend_connected ? "Backend connected" : "Backend offline",
                   state.backend_connected ? "ok" : "warn"));
    status_row.push_back(badge_node(dirty ? "Dirty draft" : "Clean", dirty ? "warn" : "ok"));
    status_row.push_back(badge_node("Scope: " + scope_label(state.selected_scope), "accent"));
    status_row.push_back(badge_node("State: " + save_phase_text(state.save_phase),
                                    save_phase_style(state.save_phase)));
    status_row.push_back(badge_node("Sync: " + rpc_kind_text(state.rpc_kind),
                                    state.rpc_kind == RpcKind::None ? "muted" : "warn"));
    nodes.push_back(row_node(std::move(status_row)));

    json scope_children = json::array();
    scope_children.push_back(text_node("Scope / 作用域", "title", 24));
    json scope_buttons = json::array();
    for (const std::string_view scope : {"system", "workspace", "plugin"}) {
        std::string label = scope_label(scope);
        const bool plugin_unavailable = scope == "plugin" && state.selected_plugin_id.empty();
        if (scope == "plugin" && !state.selected_plugin_id.empty())
            label.append(": ").append(state.selected_plugin_id);
        scope_buttons.push_back(button_node(
            "scope." + std::string(scope), std::move(label), "scope.select", {{"scope", scope}},
            state.selected_scope == scope ? "primary" : "default",
            plugin_unavailable || synchronization_locked));
    }
    scope_children.push_back(row_node(std::move(scope_buttons)));
    scope_children.push_back(text_node(
        dirty ? "存在未保存更改：请先 Apply 或 Discard，再切换作用域。"
              : "System provides defaults; Workspace overrides System; Plugin overrides both.",
        dirty ? "warn" : "muted", 32));
    nodes.push_back(card_node("Scope", std::move(scope_children), dirty ? "warn" : "cyan"));

    json search_children = json::array();
    search_children.push_back(text_node("Search / 搜索", "title", 24));
    search_children.push_back(
        text_node(state.search_query.empty()
                      ? "Search label (中文/English), stable key, description and keywords."
                      : "Query: " + state.search_query,
                  state.search_query.empty() ? "muted" : "accent", 30));
    json search_actions = json::array();
    search_actions.push_back(button_node("search.open", "Search... / 搜索...", "search.open",
                                         json::object(), "primary"));
    search_actions.push_back(button_node("search.clear", "Clear Search / 清除搜索", "search.clear",
                                         json::object(), "ghost", state.search_query.empty()));
    search_children.push_back(row_node(std::move(search_actions)));
    search_children.push_back(text_node(
        "Review filters / 审核过滤：快速定位修改、作用域覆盖和验证错误。", "muted", 28));
    json review_actions = json::array();
    for (const std::string_view filter : {"all", "modified", "overrides", "errors"}) {
        const std::string label =
            filter == "all"          ? "All / 全部"
            : filter == "modified"   ? "Modified / 已修改"
            : filter == "overrides"  ? "Overrides / 覆盖"
                                      : "Errors / 错误";
        review_actions.push_back(button_node(
            "review." + std::string(filter),
            label + " (" + std::to_string(review_filter_count(state, filter)) + ")",
            "filter.review", {{"value", filter}},
            state.review_filter == filter ? "primary" : "default"));
    }
    search_children.push_back(row_node(std::move(review_actions)));
    json advanced_actions = json::array();
    advanced_actions.push_back(button_node(
        "advanced.toggle",
        state.show_advanced ? "Hide advanced details / 隐藏高级项"
                            : "Show advanced details / 显示高级项",
        "filter.advanced", {{"value", state.show_advanced ? "hide" : "show"}},
        state.show_advanced ? "primary" : "ghost"));
    advanced_actions.push_back(badge_node(
        std::to_string(static_cast<size_t>(std::ranges::count_if(
            state.fields, [](const FieldMeta& field) { return field_is_advanced(field); }))) +
            " advanced fields / 高级字段",
        "muted"));
    search_children.push_back(row_node(std::move(advanced_actions)));
    nodes.push_back(card_node("Search", std::move(search_children), "cyan"));

    json nav_buttons = json::array();
    for (const auto& page : kPages) {
        nav_buttons.push_back(button_node(
            "nav." + std::string(page.id), std::string(page.label), "nav.page", {{"page", page.id}},
            state.search_query.empty() && state.active_page == page.id ? "primary" : "default"));
    }
    json navigation = json::array();
    navigation.push_back(text_node("Categories / 分类", "title", 24));
    append_button_rows(navigation, std::move(nav_buttons), 4);
    nodes.push_back(card_node("Categories", std::move(navigation), "gold"));

    if (!state.last_error.empty())
        nodes.push_back(text_node("Error: " + state.last_error, "bad", 42));
    if (!state.status_message.empty())
        nodes.push_back(text_node(state.status_message,
                                  state.save_phase == SavePhase::Failed ? "bad" : "accent", 36));
    if (!state.validation_errors.empty()) {
        json errors = json::array();
        errors.push_back(text_node("Validation errors / 验证错误: " +
                                       std::to_string(state.validation_errors.size()),
                                   "bad", 26));
        size_t shown = 0;
        for (const auto& [key, message] : state.validation_errors) {
            if (shown++ >= kMaximumValidationSummary)
                break;
            errors.push_back(text_node(key + ": " + message, "bad", 28));
        }
        nodes.push_back(card_node("Validation", std::move(errors), "bad"));
    }

    if (!state.metadata_loaded && state.launcher != nullptr) {
        json loading = json::array();
        loading.push_back(text_node("Settings schema / 设置结构", "title", 24));
        loading.push_back(text_node("settings.describe is loading in the background. / "
                                    "正在后台加载 settings.describe。",
                                    "muted", 34));
        nodes.push_back(card_node("Schema", std::move(loading), "warn"));
    } else if (state.search_query.empty() && state.review_filter == "all" &&
               state.active_page == "overview") {
        nodes.push_back(build_overview(state));
    } else {
        std::vector<const FieldMeta*> fields = visible_fields(state);
        const bool searching = !state.search_query.empty();
        const bool filtering = state.review_filter != "all";
        const std::string content_title =
            searching  ? "Search Results / 搜索结果"
            : filtering ? "Review Results / 审核结果"
                      : (page_definition(state.active_page) != nullptr
                             ? std::string(page_definition(state.active_page)->label)
                             : "Settings");
        nodes.push_back(text_node(content_title, "title", 28));
        if (!searching && !filtering) {
            if (const auto* page = page_definition(state.active_page); page != nullptr)
                nodes.push_back(text_node(std::string(page->description), "muted", 34));
        } else {
            const std::string reason = searching ? " matching fields" : " reviewed fields";
            nodes.push_back(text_node(std::to_string(fields.size()) + reason +
                                          " across all categories.",
                                      "muted", 28));
        }
        if (fields.empty()) {
            json empty = json::array();
            empty.push_back(text_node("No settings match / 没有匹配的设置", "warn", 28));
            empty.push_back(text_node("Try another Chinese/English label, stable key, description "
                                      "keyword, or clear search.",
                                      "muted", 38));
            json empty_actions = json::array();
            empty_actions.push_back(button_node("empty.clear", "Clear Search", "search.clear",
                                                json::object(), "primary",
                                                state.search_query.empty()));
            empty_actions.push_back(
                button_node("empty.overview", "Overview", "nav.page", {{"page", "overview"}}));
            empty.push_back(row_node(std::move(empty_actions)));
            nodes.push_back(card_node("Empty", std::move(empty), "warn"));
        } else {
            const size_t offset = std::min(
                state.result_offset, ((fields.size() - 1U) / kFieldsPerPage) * kFieldsPerPage);
            const size_t end = std::min(fields.size(), offset + kFieldsPerPage);
            std::string current_group;
            for (size_t index = offset; index < end; ++index) {
                const FieldMeta& item = *fields[index];
                if (searching || filtering) {
                    const auto* page = page_definition(item.page);
                    nodes.push_back(text_node(
                        page == nullptr ? item.page : std::string(page->label), "subtitle", 24));
                } else if (!item.group.empty() && item.group != current_group) {
                    current_group = item.group;
                    nodes.push_back(text_node(current_group, "subtitle", 24));
                }
                nodes.push_back(build_field_card(state, item));
            }
            if (fields.size() > kFieldsPerPage) {
                json paging = json::array();
                paging.push_back(button_node("results.prev", "Previous / 上一页", "results.prev",
                                             json::object(), "default", offset == 0));
                paging.push_back(text_node("Fields " + std::to_string(offset + 1U) + "-" +
                                               std::to_string(end) + " of " +
                                               std::to_string(fields.size()),
                                           "muted", 24));
                paging.push_back(button_node("results.next", "Next / 下一页", "results.next",
                                             json::object(), "default", end >= fields.size()));
                nodes.push_back(row_node(std::move(paging), "center"));
            }
        }
    }

    json footer = json::array();
    footer.push_back(text_node(dirty ? "Unsaved changes are kept if the panel is hidden."
                                     : "No pending changes.",
                               dirty ? "warn" : "muted", 28));
    json footer_actions = json::array();
    footer_actions.push_back(
        button_node("footer.apply", "Apply / 应用", "draft.apply", json::object(), "primary",
                    !dirty || !state.values_loaded || state.save_phase == SavePhase::Saving ||
                        synchronization_locked));
    footer_actions.push_back(button_node("footer.discard", "Discard / 丢弃", "draft.discard",
                                         json::object(), "danger",
                                         !dirty || state.save_phase == SavePhase::Saving ||
                                             synchronization_locked));
    footer_actions.push_back(button_node("footer.reload", "Reload / 重新加载", "draft.reload",
                                         json::object(), "default",
                                         state.save_phase == SavePhase::Saving ||
                                             synchronization_locked));
    footer_actions.push_back(button_node("footer.reset_section", "Reset Section / 重置本页",
                                         "draft.reset_section", json::object(), "ghost",
                                         state.active_page == "overview" ||
                                             !state.search_query.empty() ||
                                             state.review_filter != "all" ||
                                             !state.values_loaded ||
                                             state.save_phase == SavePhase::Saving ||
                                             synchronization_locked));
    footer.push_back(row_node(std::move(footer_actions)));
    nodes.push_back(card_node("Actions", std::move(footer), dirty ? "warn" : "ok"));

    json document{{"version", 1}, {"title", ""}, {"nodes", std::move(nodes)}};
    std::string serialized = document.dump();
    if (serialized.size() <= kMaximumPanelSpecBytes)
        return serialized;

    json compact_nodes = json::array();
    compact_nodes.push_back(text_node("AI Editor Settings", "title", 30));
    compact_nodes.push_back(text_node(
        "This page exceeded the UI spec budget and was compacted. Narrow the search or use "
        "pagination. / 当前页面超过 UI spec 预算，已切换紧凑视图；请缩小搜索范围或翻页。",
        "warn", 42));
    compact_nodes.push_back(text_node("Page: " + state.active_page +
                                          " · query: " + compact_text(state.search_query, 120U),
                                      "muted", 28));
    json compact_nav = json::array();
    for (const auto& page : kPages) {
        compact_nav.push_back(button_node("compact.nav." + std::string(page.id),
                                          std::string(page.label), "nav.page", {{"page", page.id}},
                                          state.active_page == page.id ? "primary" : "default"));
    }
    json compact_navigation = json::array();
    compact_navigation.push_back(text_node("Categories / 分类", "title", 24));
    append_button_rows(compact_navigation, std::move(compact_nav), 4);
    compact_nodes.push_back(card_node("Categories", std::move(compact_navigation), "gold"));
    return json{{"version", 1}, {"title", ""}, {"nodes", std::move(compact_nodes)}}.dump();
}

sao_status_t refresh_body(AiEditorSettingsPanelState& state, bool force) {
    std::lock_guard publish_lock(state.publish_mutex);
    std::string spec;
    {
        std::lock_guard lock(state.mutex);
        spec = build_panel_spec(state);
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

std::string rpc_transport_message(int32_t status) {
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
        return "response exceeded the bounded buffer";
    default:
        return "transport error " + std::to_string(status);
    }
}

RpcResponse rpc_request(AiEditorSettingsPanelState& state, std::string_view method,
                        const json& params) {
    RpcResponse response;
    sao_ai_editor_launcher_t launcher = nullptr;
    uint64_t id = 0;
    {
        std::lock_guard lock(state.mutex);
        launcher = state.launcher;
        id = ++state.request_counter;
    }
    if (launcher == nullptr) {
        response.transport_status = SAO_AI_EDITOR_ERR_NOT_RUNNING;
        response.error = "后端未连接 / backend not attached";
        return response;
    }
    const json request{
        {"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}, {"params", params}};
    const std::string request_body = request.dump();
    if (request_body.size() > kMaximumRequestBytes ||
        request_body.size() > std::numeric_limits<uint32_t>::max()) {
        response.transport_status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        response.error = "request exceeds the 256 KiB UI limit";
        return response;
    }
    std::vector<char> response_buffer(kMaximumResponseBytes);
    uint32_t response_len = 0;
    int32_t status = SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    {
        std::lock_guard rpc_lock(state.rpc_mutex);
        status = sao_ai_editor_request(
            launcher, request_body.data(), static_cast<uint32_t>(request_body.size()),
            response_buffer.data(), static_cast<uint32_t>(response_buffer.size()), &response_len,
            kRequestTimeoutMs);
    }
    response.transport_status = status;
    if (status != SAO_AI_EDITOR_OK) {
        response.error = rpc_transport_message(status);
        return response;
    }
    if (response_len > response_buffer.size()) {
        response.transport_status = SAO_AI_EDITOR_ERR_PROTOCOL;
        response.error = "backend returned an invalid response length";
        return response;
    }
    const json document = json::parse(response_buffer.data(), response_buffer.data() + response_len,
                                      nullptr, false, false);
    if (document.is_discarded() || !document.is_object()) {
        response.transport_status = SAO_AI_EDITOR_ERR_PROTOCOL;
        response.error = "backend response is not valid JSON";
        return response;
    }
    if (document.value("jsonrpc", std::string{}) != "2.0") {
        response.transport_status = SAO_AI_EDITOR_ERR_PROTOCOL;
        response.error = "backend response is not JSON-RPC 2.0";
        return response;
    }
    if (const auto error = document.find("error"); error != document.end() && error->is_object()) {
        response.transport_status = SAO_AI_EDITOR_ERR_PROTOCOL;
        response.error = read_text(*error, {"message"});
        if (response.error.empty())
            response.error = "backend rejected the request";
        if (const auto data = error->find("data"); data != error->end())
            response.error.append(" · ").append(compact_text(data->dump(), 300U));
        return response;
    }
    const auto result = document.find("result");
    if (result == document.end()) {
        response.transport_status = SAO_AI_EDITOR_ERR_PROTOCOL;
        response.error = "backend response has no result";
        return response;
    }
    response.ok = true;
    response.result = *result;
    return response;
}

void mark_backend_failure(AiEditorSettingsPanelState& state, std::string context,
                          const RpcResponse& response) {
    std::lock_guard lock(state.mutex);
    state.backend_connected = false;
    state.backend_message = "Offline / 离线: " + response.error;
    state.last_error = std::move(context) + ": " + response.error;
}

const json* backend_secret_state(const json& states, const json& values, const FieldMeta& field) {
    if (const json* direct = lookup_value(states, field.key); direct != nullptr)
        return direct;
    std::string provider;
    if (const json* active = lookup_value(states, "activeProvider");
        active != nullptr && active->is_string())
        provider = active->get<std::string>();
    if (provider.empty()) {
        if (const json* configured = lookup_value(values, "provider");
            configured != nullptr && configured->is_string())
            provider = configured->get<std::string>();
    }
    if (field.key == "api_key" || field.key.ends_with(".api_key"))
        return lookup_value(states, "apiKey");
    constexpr std::string_view kProviderPrefix = "provider_keys.";
    if (field.key.starts_with(kProviderPrefix)) {
        const std::string key = "providerKeys." + field.key.substr(kProviderPrefix.size());
        return lookup_value(states, key);
    }
    if (provider.empty())
        return nullptr;
    if (field.key == "extra_headers" || field.key.ends_with(".extra_headers"))
        return lookup_value(states, "extraHeaders." + provider);
    if (field.key == "extra_body" || field.key.ends_with(".extra_body"))
        return lookup_value(states, "extraBody." + provider);
    return nullptr;
}

void collect_secret_states(const AiEditorSettingsPanelState& state, const json& result,
                           const json& values,
                           std::unordered_map<std::string, std::string>& target,
                           std::initializer_list<const char*> state_keys =
                               {"secretStates", "secret_states", "secrets"}) {
    json states = object_from(result, state_keys);
    for (const auto& field : state.fields) {
        if (!is_secret_field(field))
            continue;
        std::string status = "unset";
        if (const json* explicit_state = backend_secret_state(states, values, field);
            explicit_state != nullptr) {
            status = status_from_json(*explicit_state);
        } else if (const json* loaded = lookup_value(values, field.key); loaded != nullptr) {
            status = status_from_json(*loaded);
        }
        target[field.key] = status == "clear" ? "unset" : status;
    }
}

void apply_scope_secret_states(AiEditorSettingsPanelState& state, const json& result,
                               const json& values) {
    collect_secret_states(state, result, values, state.secret_states);
    for (const auto& field : state.fields) {
        if (!is_secret_field(field))
            continue;
        (void)erase_value(state.persisted, field.key);
        (void)erase_value(state.draft, field.key);
    }
}

void apply_describe_result(AiEditorSettingsPanelState& state, const RpcResponse& response) {
    if (!response.ok) {
        mark_backend_failure(state, "settings.describe failed", response);
        std::lock_guard lock(state.mutex);
        state.load_pending = false;
        state.raw_load_pending = false;
        state.effective_load_pending = false;
        return;
    }
    std::lock_guard lock(state.mutex);
    merge_described_fields(state, response.result);
    state.defaults = described_values_from(state, state.defaults);
    if (const auto scopes = response.result.find("scopes");
        scopes != response.result.end() && scopes->is_array()) {
        for (const auto& entry : *scopes) {
            if (!entry.is_object() || entry.value("scope", std::string{}) != "plugin")
                continue;
            const std::string plugin_id = entry.value("pluginId", std::string{});
            if (!plugin_id.empty()) {
                state.selected_plugin_id = plugin_id;
                break;
            }
        }
    }
    state.metadata_loaded = true;
    state.backend_connected = true;
    state.backend_message = "Connected · settings schema loaded";
    state.last_error.clear();
}

void apply_scope_load_result(AiEditorSettingsPanelState& state, const RpcJob& job,
                             const RpcResponse& response) {
    if (!response.ok) {
        mark_backend_failure(state, "settings.load failed", response);
        std::lock_guard lock(state.mutex);
        state.effective_load_pending = false;
        return;
    }
    bool exact_overrides = false;
    json overrides = scope_overrides_from(response.result, exact_overrides);
    json selected_values = settings_values_from(response.result);
    json effective = effective_values_from(response.result);
    json inherited = inherited_values_from(response.result);
    json sources = sources_from(response.result);

    std::lock_guard lock(state.mutex);
    if (job.scope != state.selected_scope || job.plugin_id != state.selected_plugin_id)
        return;
    overrides = described_values_from(state, overrides);
    json selected_described = described_values_from(state, selected_values);
    effective = described_values_from(state, effective);
    inherited = described_values_from(state, inherited);
    sources = described_values_from(state, sources, true);
    state.persisted = overrides.is_object() ? std::move(overrides) : json::object();
    state.draft = state.persisted;
    state.scope_overrides_exact = exact_overrides;
    state.sources = sources.is_object() ? std::move(sources) : json::object();
    state.inherited = inherited.is_object() ? std::move(inherited) : json::object();
    apply_scope_secret_states(state, response.result, selected_values);
    state.effective_secret_states = state.secret_states;
    state.inherited_secret_states.clear();
    collect_secret_states(state, response.result, state.inherited,
                          state.inherited_secret_states,
                          {"inheritedSecretStates", "inherited_secret_states"});
    if (effective.is_object() && (!effective.empty() || response.result.contains("effective") ||
                                  response.result.contains("effectiveValues") ||
                                  response.result.contains("effective_values"))) {
        state.effective = std::move(effective);
        state.effective_loaded = true;
        collect_secret_states(state, response.result, state.effective,
                              state.effective_secret_states,
                              {"effectiveSecretStates", "effective_secret_states",
                               "secretStates", "secret_states"});
    } else if (state.selected_scope == "system") {
        state.effective = std::move(selected_described);
        state.effective_loaded = true;
    } else {
        state.effective = json::object();
        state.effective_loaded = false;
        state.effective_load_pending = true;
    }
    state.raw_load_pending = !exact_overrides;
    state.pending_secret_values.clear();
    state.pending_secret_clears.clear();
    state.reset_keys.clear();
    state.validation_errors.clear();
    state.values_loaded = true;
    state.backend_connected = true;
    state.backend_message = "Connected · settings loaded for " + scope_label(state.selected_scope);
    state.last_error.clear();
    state.hidden_with_dirty = false;
    if (state.save_phase != SavePhase::Saved)
        state.save_phase = SavePhase::Idle;
}

void apply_raw_scope_load_result(AiEditorSettingsPanelState& state, const RpcJob& job,
                                 const RpcResponse& response) {
    if (!response.ok) {
        std::lock_guard lock(state.mutex);
        if (job.scope != state.selected_scope || job.plugin_id != state.selected_plugin_id)
            return;
        state.scope_overrides_exact = false;
        state.status_message =
            "Selected scope loaded; exact raw overrides are unavailable from this backend. / "
            "当前作用域已加载，但后端未提供精确 raw override。";
        return;
    }

    json raw_root = response.result.is_object() ? response.result : json::object();
    json overrides = object_from(raw_root, {"ai_editor"});
    std::lock_guard lock(state.mutex);
    if (job.scope != state.selected_scope || job.plugin_id != state.selected_plugin_id)
        return;
    state.persisted = described_values_from(state, overrides);
    state.draft = state.persisted;
    state.scope_overrides_exact = true;
    state.pending_secret_values.clear();
    state.pending_secret_clears.clear();
    state.reset_keys.clear();
    state.validation_errors.clear();
    state.backend_connected = true;
    state.backend_message = "Connected · exact selected-scope overrides loaded";
    state.last_error.clear();
}

void apply_effective_load_result(AiEditorSettingsPanelState& state, const RpcJob& job,
                                 const RpcResponse& response) {
    if (!response.ok) {
        std::lock_guard lock(state.mutex);
        state.effective_loaded = false;
        state.backend_connected = state.values_loaded;
        state.backend_message = "Selected scope loaded; merged effective values unavailable.";
        state.last_error = "settings.load merged failed: " + response.error;
        return;
    }
    json effective = effective_values_from(response.result);
    if (!effective.is_object() || effective.empty())
        effective = settings_values_from(response.result);
    json inherited = inherited_values_from(response.result);
    json sources = sources_from(response.result);
    std::lock_guard lock(state.mutex);
    if (job.scope != state.selected_scope || job.plugin_id != state.selected_plugin_id)
        return;
    state.effective = described_values_from(state, effective);
    inherited = described_values_from(state, inherited);
    sources = described_values_from(state, sources, true);
    if (!inherited.empty())
        state.inherited = std::move(inherited);
    if (!sources.empty())
        state.sources = std::move(sources);
    state.effective_secret_states.clear();
    collect_secret_states(state, response.result, state.effective,
                          state.effective_secret_states,
                          {"effectiveSecretStates", "effective_secret_states",
                           "secretStates", "secret_states"});
    if (!state.inherited.empty()) {
        state.inherited_secret_states.clear();
        collect_secret_states(state, response.result, state.inherited,
                              state.inherited_secret_states,
                              {"inheritedSecretStates", "inherited_secret_states"});
    }
    state.effective_loaded = true;
    state.backend_connected = true;
    state.backend_message = "Connected · selected scope and merged effective values loaded";
    state.last_error.clear();
}

bool parse_integer(std::string_view text, int64_t& out) {
    const std::string trimmed = trim_copy(text);
    if (trimmed.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(trimmed.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0')
        return false;
    out = static_cast<int64_t>(value);
    return true;
}

bool parse_number(std::string_view text, double& out) {
    const std::string trimmed = trim_copy(text);
    if (trimmed.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(trimmed.c_str(), &end);
    if (errno == ERANGE || end == nullptr || *end != '\0' || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

std::optional<std::string> validate_value(const FieldMeta& field, const json& value) {
    if (field.kind == FieldKind::Secret)
        return std::nullopt;
    if (field.required &&
        (value.is_null() || (value.is_string() && value.get<std::string>().empty())))
        return "value is required";
    double number = 0.0;
    switch (field.kind) {
    case FieldKind::Boolean:
        if (!value.is_boolean())
            return "expected a boolean";
        break;
    case FieldKind::Integer:
        if (!value.is_number_integer() && !value.is_number_unsigned())
            return "expected an integer";
        number = value.get<double>();
        break;
    case FieldKind::Number:
        if (!value.is_number())
            return "expected a number";
        number = value.get<double>();
        if (!std::isfinite(number))
            return "number must be finite";
        break;
    case FieldKind::String:
        if (!value.is_string())
            return "expected text";
        if (value.get_ref<const std::string&>().size() > kMaximumInputBytes)
            return "text exceeds the 64 KiB limit";
        break;
    case FieldKind::Json:
        if (value.is_discarded())
            return "invalid JSON";
        break;
    case FieldKind::List:
        if (!value.is_array())
            return "expected a JSON array or one item per line";
        break;
    case FieldKind::Enum:
        if (!field.options.empty() &&
            std::ranges::none_of(field.options,
                                 [&value](const FieldOption& item) { return item.value == value; }))
            return "value is not one of the supported options";
        break;
    case FieldKind::Secret:
        break;
    }
    if ((field.kind == FieldKind::Integer || field.kind == FieldKind::Number)) {
        if (field.minimum.has_value() && number < *field.minimum)
            return "value is below minimum " + std::to_string(*field.minimum);
        if (field.maximum.has_value() && number > *field.maximum)
            return "value is above maximum " + std::to_string(*field.maximum);
    }
    return std::nullopt;
}

std::optional<json> parse_dialog_value(const FieldMeta& field, std::string_view input,
                                       std::string& error) {
    if (input.size() > (is_secret_field(field) ? kMaximumSecretBytes : kMaximumInputBytes)) {
        error = is_secret_field(field) ? "protected value exceeds the 16 KiB limit"
                                       : "input exceeds the 64 KiB limit";
        return std::nullopt;
    }
    json value;
    switch (field.kind) {
    case FieldKind::Secret:
    case FieldKind::String:
        value = std::string(input);
        break;
    case FieldKind::Integer: {
        int64_t number = 0;
        if (!parse_integer(input, number)) {
            error = "enter a valid integer";
            return std::nullopt;
        }
        value = number;
        break;
    }
    case FieldKind::Number: {
        double number = 0.0;
        if (!parse_number(input, number)) {
            error = "enter a valid finite number";
            return std::nullopt;
        }
        value = number;
        break;
    }
    case FieldKind::Json: {
        value = json::parse(input.begin(), input.end(), nullptr, false, false);
        if (value.is_discarded()) {
            error = "enter valid JSON";
            return std::nullopt;
        }
        break;
    }
    case FieldKind::List: {
        value = json::parse(input.begin(), input.end(), nullptr, false, false);
        if (!value.is_array()) {
            value = json::array();
            size_t begin = 0;
            while (begin <= input.size()) {
                const size_t end = input.find('\n', begin);
                const std::string item = trim_copy(input.substr(
                    begin, end == std::string_view::npos ? input.size() - begin : end - begin));
                if (!item.empty())
                    value.push_back(item);
                if (end == std::string_view::npos)
                    break;
                begin = end + 1;
            }
        }
        break;
    }
    case FieldKind::Enum: {
        const std::string candidate = trim_copy(input);
        const auto found =
            std::ranges::find_if(field.options, [&candidate](const FieldOption& item) {
                return (item.value.is_string() && item.value.get<std::string>() == candidate) ||
                       ascii_lower(item.label) == ascii_lower(candidate) ||
                       (!item.label_zh.empty() && item.label_zh == candidate);
            });
        if (found == field.options.end()) {
            error = "enter one of the displayed options";
            return std::nullopt;
        }
        value = found->value;
        break;
    }
    case FieldKind::Boolean:
        error = "boolean fields use Toggle";
        return std::nullopt;
    }
    if (const auto validation = validate_value(field, value); validation.has_value()) {
        error = *validation;
        return std::nullopt;
    }
    return value;
}

void set_field_value(AiEditorSettingsPanelState& state, const FieldMeta& field, json value) {
    if (is_secret_field(field)) {
        if (!value.is_null() && (!value.is_string() || !value.get<std::string>().empty())) {
            state.pending_secret_values[field.key] = std::move(value);
            state.pending_secret_clears.erase(field.key);
            state.reset_keys.erase(field.key);
        }
    } else {
        set_value(state.draft, field.key, std::move(value));
        state.reset_keys.erase(field.key);
    }
    state.validation_errors.erase(field.key);
    state.save_phase = SavePhase::Idle;
    state.last_error.clear();
    state.status_message = "Draft updated: " + field.key;
}

void reset_field(AiEditorSettingsPanelState& state, const FieldMeta& field) {
    if (is_secret_field(field)) {
        state.pending_secret_values.erase(field.key);
        state.pending_secret_clears.insert(field.key);
    } else {
        (void)erase_value(state.draft, field.key);
        state.reset_keys.insert(field.key);
    }
    state.validation_errors.erase(field.key);
    state.save_phase = SavePhase::Idle;
    state.status_message = "Reset queued: " + field.key;
}

void discard_draft(AiEditorSettingsPanelState& state) {
    state.draft = state.persisted;
    state.pending_secret_values.clear();
    state.pending_secret_clears.clear();
    state.reset_keys.clear();
    state.validation_errors.clear();
    state.save_phase = SavePhase::Idle;
    state.last_error.clear();
    state.hidden_with_dirty = false;
    state.status_message = "Draft discarded / 草稿已丢弃";
}

bool validate_draft(AiEditorSettingsPanelState& state) {
    state.validation_errors.clear();
    for (const auto& field : state.fields) {
        if (state.reset_keys.contains(field.key) || is_secret_field(field))
            continue;
        const json* value = lookup_value(state.draft, field.key);
        if (value == nullptr)
            continue;
        if (const auto error = validate_value(field, *value); error.has_value())
            state.validation_errors[field.key] = *error;
    }
    for (const auto& [key, secret] : state.pending_secret_values) {
        const std::string serialized = secret.dump();
        if (secret.is_string() && secret.get_ref<const std::string&>().empty())
            state.validation_errors[key] = "secret must not be empty";
        else if (serialized.size() > kMaximumSecretBytes)
            state.validation_errors[key] = "protected value exceeds the 16 KiB limit";
    }
    return state.validation_errors.empty();
}

json build_save_params(const AiEditorSettingsPanelState& state) {
    json patch = json::object();
    for (const auto& field : state.fields) {
        if (is_secret_field(field) || state.reset_keys.contains(field.key))
            continue;
        const json* draft_value = lookup_value(state.draft, field.key);
        const json* persisted_value = lookup_value(state.persisted, field.key);
        if (draft_value != nullptr &&
            (persisted_value == nullptr || *draft_value != *persisted_value))
            set_value(patch, field.key, *draft_value);
    }

    std::vector<std::string> sorted_resets(state.reset_keys.begin(), state.reset_keys.end());
    std::ranges::sort(sorted_resets);
    json resets = json::array();
    for (const auto& key : sorted_resets)
        resets.push_back(key);

    std::vector<std::string> sorted_secret_clears(state.pending_secret_clears.begin(),
                                                  state.pending_secret_clears.end());
    std::ranges::sort(sorted_secret_clears);
    json clear_secrets = json::array();
    for (const auto& key : sorted_secret_clears)
        clear_secrets.push_back(key);
    json secret_updates = json::object();
    const auto append_secret_update = [&](std::string_view key, std::string_view action,
                                          const json* value) {
        json update{{"action", action}};
        if (value != nullptr)
            update["value"] = *value;
        if (key == "api_key" || key.ends_with(".api_key")) {
            secret_updates["apiKey"] = std::move(update);
            return;
        }
        constexpr std::string_view kProviderPrefix = "provider_keys.";
        if (key.starts_with(kProviderPrefix)) {
            secret_updates["providerKeys"][std::string(key.substr(kProviderPrefix.size()))] =
                std::move(update);
            return;
        }
        if (key == "extra_headers" || key.ends_with(".extra_headers")) {
            secret_updates["extraHeaders"] = std::move(update);
            return;
        }
        if (key == "extra_body" || key.ends_with(".extra_body")) {
            secret_updates["extraBody"] = std::move(update);
            return;
        }
        secret_updates[std::string(key)] = std::move(update);
    };
    for (const auto& [key, secret] : state.pending_secret_values) {
        append_secret_update(key, "set", &secret);
    }
    for (const auto& key : sorted_secret_clears)
        append_secret_update(key, "clear", nullptr);
    json params{{"scope", state.selected_scope},
                {"patch", patch},
                {"settings", patch},
                {"changes", patch},
                {"secretUpdates", secret_updates},
                {"secret_updates", secret_updates},
                {"resetKeys", resets},
                {"reset_keys", resets},
                {"clearSecretKeys", clear_secrets},
                {"clear_secret_keys", clear_secrets}};
    if (state.selected_scope == "plugin")
        params["pluginId"] = state.selected_plugin_id;
    return params;
}

void apply_save_success(AiEditorSettingsPanelState& state, const RpcJob& job, const json& result) {
    bool exact_overrides = false;
    json returned_overrides = scope_overrides_from(result, exact_overrides);
    state.persisted = exact_overrides && returned_overrides.is_object()
                          ? std::move(returned_overrides)
                          : job.saved_draft;
    for (const auto& key : job.saved_reset_keys)
        (void)erase_value(state.persisted, key);
    state.draft = state.persisted;
    if (exact_overrides)
        state.scope_overrides_exact = true;
    for (const auto& key : job.saved_secret_sets)
        state.secret_states[key] = "set";
    for (const auto& key : job.saved_secret_clears)
        state.secret_states[key] = "unset";
    state.pending_secret_values.clear();
    state.pending_secret_clears.clear();
    state.reset_keys.clear();
    state.validation_errors.clear();
    state.save_phase = SavePhase::Saved;
    state.saved_at = std::chrono::steady_clock::now();
    state.last_error.clear();
    state.status_message = "Saved / 已保存 · " + scope_label(state.selected_scope);
    state.hidden_with_dirty = false;

    const json returned_values = settings_values_from(result);
    if (result.contains("secretStates") || result.contains("secret_states") ||
        result.contains("secrets")) {
        collect_secret_states(state, result, returned_values, state.secret_states);
    }
    state.effective = state.selected_scope == "system" ? state.defaults : state.inherited;
    merge_object_patch(state.effective, state.persisted);
    state.effective_loaded = state.selected_scope == "system";
    state.load_pending = true;
    state.effective_load_pending = state.selected_scope != "system";
}

void apply_save_result(AiEditorSettingsPanelState& state, const RpcJob& job,
                       const RpcResponse& response) {
    std::lock_guard lock(state.mutex);
    if (job.scope != state.selected_scope || job.plugin_id != state.selected_plugin_id)
        return;
    if (!response.ok) {
        state.backend_connected = false;
        state.backend_message = "Offline / 离线: " + response.error;
        state.save_phase = SavePhase::Failed;
        state.last_error = "settings.save failed: " + response.error;
        state.status_message =
            "Save failed; draft retained and can be retried. / 保存失败，草稿已保留。";
        return;
    }
    state.backend_connected = true;
    state.backend_message = "Connected · last save succeeded";
    apply_save_success(state, job, response.result);
}

void queue_save(AiEditorSettingsPanelState& state) {
    std::lock_guard lock(state.mutex);
    if (state.save_phase == SavePhase::Saving || state.save_pending)
        return;
    if (followup_load_active(state) || state.rpc_kind != RpcKind::None) {
        state.last_error = "Settings are synchronizing; wait before applying changes. / "
                           "设置正在同步，请等待完成后再应用。";
        return;
    }
    if (!state_dirty(state)) {
        state.status_message = "No pending changes / 没有待保存更改";
        return;
    }
    if (!validate_draft(state)) {
        state.save_phase = SavePhase::Failed;
        state.last_error = "Fix validation errors before applying settings.";
        state.status_message = "Apply blocked / 应用已阻止";
        return;
    }
    state.save_pending = true;
    state.save_phase = SavePhase::Saving;
    state.status_message = "Save queued / 已进入保存队列";
    state.last_error.clear();
}

std::optional<RpcJob> take_next_rpc_job(AiEditorSettingsPanelState& state) {
    std::lock_guard lock(state.mutex);
    if (state.rpc_kind != RpcKind::None || state.launcher == nullptr)
        return std::nullopt;

    RpcJob job;
    job.scope = state.selected_scope;
    job.plugin_id = state.selected_plugin_id;
    if (state.describe_pending) {
        state.describe_pending = false;
        job.kind = RpcKind::Describe;
        job.method = "settings.describe";
        job.params = {{"scope", state.selected_scope}};
    } else if (state.save_pending) {
        state.save_pending = false;
        job.kind = RpcKind::Save;
        job.method = "settings.save";
        job.params = build_save_params(state);
        job.saved_draft = state.draft;
        job.saved_reset_keys.assign(state.reset_keys.begin(), state.reset_keys.end());
        job.saved_secret_clears.assign(state.pending_secret_clears.begin(),
                                       state.pending_secret_clears.end());
        for (const auto& [key, value] : state.pending_secret_values) {
            (void)value;
            job.saved_secret_sets.push_back(key);
        }
        std::ranges::sort(job.saved_reset_keys);
        std::ranges::sort(job.saved_secret_clears);
        std::ranges::sort(job.saved_secret_sets);
        state.status_message = "Saving in background / 正在后台保存...";
    } else if (state.load_pending) {
        state.load_pending = false;
        job.kind = RpcKind::LoadScope;
        job.method = "settings.load";
        job.params = {{"scope", state.selected_scope},
                      {"raw", true},
                      {"includeOverrides", true},
                      {"includeEffective", true},
                      {"includeSources", true}};
        if (state.selected_scope == "plugin")
            job.params["pluginId"] = state.selected_plugin_id;
    } else if (state.raw_load_pending) {
        state.raw_load_pending = false;
        job.kind = RpcKind::LoadRawScope;
        job.method = "config.load";
        job.params = {{"scope", state.selected_scope == "plugin"
                                    ? "plugin:" + state.selected_plugin_id
                                    : state.selected_scope}};
    } else if (state.effective_load_pending) {
        state.effective_load_pending = false;
        job.kind = RpcKind::LoadEffective;
        job.method = "settings.load";
        job.params = {{"scope", "merged"},
                      {"targetScope", state.selected_scope},
                      {"includeSources", true},
                      {"includeInherited", true}};
        if (state.selected_scope == "plugin")
            job.params["pluginId"] = state.selected_plugin_id;
    } else {
        return std::nullopt;
    }
    state.rpc_kind = job.kind;
    return job;
}

int32_t start_rpc_job(AiEditorSettingsPanelState& state, RpcJob job) {
    const RpcKind kind = job.kind;
    try {
        std::future<RpcCompletion> future =
            std::async(std::launch::async, [&state, job = std::move(job)]() mutable {
                RpcCompletion completion;
                completion.job = std::move(job);
                try {
                    completion.response =
                        rpc_request(state, completion.job.method, completion.job.params);
                } catch (const std::exception& error) {
                    completion.response.transport_status = SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
                    completion.response.error =
                        std::string("background RPC failed: ") + error.what();
                } catch (...) {
                    completion.response.transport_status = SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
                    completion.response.error = "background RPC failed with an internal error";
                }
                return completion;
            });
        {
            std::lock_guard lock(state.mutex);
            state.rpc_future = std::move(future);
        }
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        std::lock_guard lock(state.mutex);
        state.rpc_future = {};
        state.rpc_kind = RpcKind::None;
        if (kind == RpcKind::Describe)
            state.describe_pending = true;
        else if (kind == RpcKind::LoadScope)
            state.load_pending = true;
        else if (kind == RpcKind::LoadRawScope)
            state.raw_load_pending = true;
        else if (kind == RpcKind::LoadEffective)
            state.effective_load_pending = true;
        else if (kind == RpcKind::Save) {
            state.save_phase = SavePhase::Failed;
            state.save_pending = true;
        }
        state.last_error = "Failed to start the background settings request.";
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

int32_t poll_rpc_completion(AiEditorSettingsPanelState& state) {
    std::future<RpcCompletion> ready;
    {
        std::lock_guard lock(state.mutex);
        if (state.rpc_kind == RpcKind::None || !state.rpc_future.valid() ||
            state.rpc_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            return SAO_AI_EDITOR_OK;
        }
        ready = std::move(state.rpc_future);
        state.rpc_future = {};
        state.rpc_kind = RpcKind::None;
    }

    RpcCompletion completion;
    try {
        completion = ready.get();
    } catch (...) {
        std::lock_guard lock(state.mutex);
        state.backend_connected = false;
        state.last_error = "Background settings request completed with an internal error.";
        state.save_phase = SavePhase::Failed;
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }

    switch (completion.job.kind) {
    case RpcKind::Describe:
        apply_describe_result(state, completion.response);
        break;
    case RpcKind::LoadScope:
        apply_scope_load_result(state, completion.job, completion.response);
        break;
    case RpcKind::LoadRawScope:
        apply_raw_scope_load_result(state, completion.job, completion.response);
        break;
    case RpcKind::LoadEffective:
        apply_effective_load_result(state, completion.job, completion.response);
        break;
    case RpcKind::Save:
        apply_save_result(state, completion.job, completion.response);
        break;
    case RpcKind::None:
        break;
    }
    return SAO_AI_EDITOR_OK;
}

bool read_payload(const uint8_t* bytes, size_t length, json& out, std::string& error) {
    if (length > kMaximumActionPayloadBytes) {
        error = "action payload exceeds 64 KiB";
        return false;
    }
    if (bytes == nullptr && length != 0) {
        error = "action payload pointer is null";
        return false;
    }
    if (length == 0) {
        out = json::object();
        return true;
    }
    out = json::parse(bytes, bytes + length, nullptr, false, false);
    if (out.is_discarded() || !out.is_object()) {
        error = "action payload is not a JSON object";
        return false;
    }
    return true;
}

std::optional<std::string> payload_string(const json& payload, const char* key,
                                          size_t maximum = 512U) {
    const auto found = payload.find(key);
    if (found == payload.end() || !found->is_string())
        return std::nullopt;
    std::string value = found->get<std::string>();
    if (value.empty() || value.size() > maximum)
        return std::nullopt;
    return value;
}

class ActionLease final {
  public:
    explicit ActionLease(AiEditorSettingsPanelState* state) : state_(state) {
        if (state_ == nullptr)
            return;
        if (require_owner_thread(state_->compositor) != SAO_AI_EDITOR_OK)
            return;
        std::lock_guard lock(state_->mutex);
        if (!state_->accepting)
            return;
        ++state_->actions_in_flight;
        acquired_ = true;
    }

    ~ActionLease() {
        if (!acquired_)
            return;
        {
            std::lock_guard lock(state_->mutex);
            --state_->actions_in_flight;
            state_->cv.notify_all();
        }
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }
    AiEditorSettingsPanelState& state() const noexcept {
        return *state_;
    }

  private:
    AiEditorSettingsPanelState* state_{};
    bool acquired_{};
};

sao_status_t ensure_dialog(AiEditorSettingsPanelState& state) {
    if (state.dialog != nullptr)
        return SAO_STATUS_OK;
    return sao_ui_dialog_create(state.compositor, nullptr, &state.dialog);
}

void SAO_UI_CALL dialog_result_callback(SaoUiDialogButton pressed, const char* input_text_utf8,
                                        size_t input_text_len, void* user_data) {
    auto* state = static_cast<AiEditorSettingsPanelState*>(user_data);
    ActionLease lease(state);
    if (!lease)
        return;
    state = &lease.state();
    std::lock_guard lock(state->mutex);
    const PendingDialog pending = state->pending_dialog;
    state->pending_dialog = {};
    if (!state->accepting || pressed != SAO_UI_DIALOG_BTN_OK)
        return;
    if (input_text_utf8 == nullptr && input_text_len != 0) {
        state->last_error = "Dialog returned a null input buffer with a non-zero length.";
        return;
    }
    const std::string input =
        input_text_utf8 == nullptr ? std::string{} : std::string(input_text_utf8, input_text_len);
    if (pending.intent == DialogIntent::Search) {
        if (input.size() > kMaximumSearchBytes) {
            state->last_error = "Search query exceeds 256 bytes.";
            return;
        }
        state->search_query = trim_copy(input);
        state->result_offset = 0;
        state->status_message = state->search_query.empty()
                                    ? "Search cleared / 搜索已清除"
                                    : "Search active: " + state->search_query;
        state->last_error.clear();
        return;
    }
    if (state->rpc_kind != RpcKind::None || state->save_pending ||
        followup_load_active(*state)) {
        state->last_error = "Settings changed while the dialog was open; wait for synchronization "
                            "and try again. / 设置同步状态已变化，请等待后重试。";
        return;
    }
    if (pending.intent == DialogIntent::ConfirmReload) {
        discard_draft(*state);
        state->load_pending = true;
        state->status_message = "Reload queued / 已请求重新加载";
        return;
    }
    const FieldMeta* field = find_field(*state, pending.field_key);
    if (field == nullptr) {
        state->last_error = "Setting metadata is no longer available: " + pending.field_key;
        return;
    }
    if (!field_dependencies_satisfied(*state, *field)) {
        state->last_error =
            "Setting dependency is no longer satisfied: " + dependency_display(*state, *field);
        return;
    }
    if (pending.intent == DialogIntent::ConfirmClearSecret) {
        state->pending_secret_values.erase(field->key);
        state->pending_secret_clears.insert(field->key);
        state->reset_keys.erase(field->key);
        state->validation_errors.erase(field->key);
        state->save_phase = SavePhase::Idle;
        state->status_message = "Secret clear queued: " + field->key;
        return;
    }
    if (pending.intent != DialogIntent::EditField)
        return;
    std::string error;
    std::optional<json> value = parse_dialog_value(*field, input, error);
    if (!value.has_value()) {
        state->validation_errors[field->key] = std::move(error);
        state->last_error = "Invalid value for " + field->key;
        return;
    }
    set_field_value(*state, *field, std::move(*value));
}

bool dialog_visible(AiEditorSettingsPanelState& state) {
    if (state.dialog == nullptr)
        return false;
    bool visible = false;
    return sao_ui_dialog_is_visible(state.dialog, &visible) == SAO_STATUS_OK && visible;
}

int32_t hide_active_dialog(AiEditorSettingsPanelState& state) {
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

void show_input_dialog(AiEditorSettingsPanelState& state, DialogIntent intent,
                       const FieldMeta* field, std::string title, std::string message,
                       std::string prompt, std::string default_value, size_t maximum_length) {
    if (dialog_visible(state)) {
        std::lock_guard lock(state.mutex);
        state.last_error = "Finish or dismiss the current dialog first.";
        return;
    }
    const sao_status_t create_status = ensure_dialog(state);
    if (create_status != SAO_STATUS_OK) {
        std::lock_guard lock(state.mutex);
        state.last_error = "Dialog creation failed (status " + std::to_string(create_status) + ").";
        return;
    }
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_INPUT;
    spec.title_utf8 = title.c_str();
    spec.message_utf8 = message.c_str();
    spec.input_prompt_utf8 = prompt.c_str();
    spec.input_default_utf8 = default_value.c_str();
    spec.input_max_length = static_cast<int32_t>(
        std::min<size_t>(maximum_length, static_cast<size_t>(std::numeric_limits<int32_t>::max())));
    spec.width = 620;
    spec.height = 320;
    spec.draggable = true;
    spec.dismiss_on_esc = true;
    {
        std::lock_guard lock(state.mutex);
        state.pending_dialog.intent = intent;
        state.pending_dialog.field_key = field == nullptr ? std::string{} : field->key;
    }
    const sao_status_t status =
        sao_ui_dialog_show(state.dialog, &spec, &dialog_result_callback, &state);
    if (status != SAO_STATUS_OK) {
        std::lock_guard lock(state.mutex);
        state.pending_dialog = {};
        state.last_error = "Dialog show failed (status " + std::to_string(status) + ").";
    }
}

void show_confirm_dialog(AiEditorSettingsPanelState& state, DialogIntent intent,
                         const FieldMeta* field, std::string title, std::string message) {
    if (dialog_visible(state)) {
        std::lock_guard lock(state.mutex);
        state.last_error = "Finish or dismiss the current dialog first.";
        return;
    }
    const sao_status_t create_status = ensure_dialog(state);
    if (create_status != SAO_STATUS_OK) {
        std::lock_guard lock(state.mutex);
        state.last_error = "Dialog creation failed (status " + std::to_string(create_status) + ").";
        return;
    }
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_ASK;
    spec.title_utf8 = title.c_str();
    spec.message_utf8 = message.c_str();
    spec.width = 560;
    spec.height = 280;
    spec.draggable = true;
    spec.dismiss_on_esc = true;
    {
        std::lock_guard lock(state.mutex);
        state.pending_dialog.intent = intent;
        state.pending_dialog.field_key = field == nullptr ? std::string{} : field->key;
    }
    const sao_status_t status =
        sao_ui_dialog_show(state.dialog, &spec, &dialog_result_callback, &state);
    if (status != SAO_STATUS_OK) {
        std::lock_guard lock(state.mutex);
        state.pending_dialog = {};
        state.last_error = "Dialog show failed (status " + std::to_string(status) + ").";
    }
}

struct ApiLease final {
    explicit ApiLease(sao_ai_editor_settings_panel_t handle, bool owner_thread_required = false) {
        {
            std::lock_guard registry_lock(registry_mutex());
            const auto found = registry().find(handle);
            if (found == registry().end())
                return;
            state_ = found->second.get();
            std::lock_guard state_lock(state_->mutex);
            if (!state_->accepting || state_->destroy_claimed) {
                status_ = SAO_AI_EDITOR_ERR_BUSY;
                state_ = nullptr;
                return;
            }
            ++state_->api_calls_in_flight;
        }
        acquired_ = true;
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
        AiEditorSettingsPanelState* const state = state_;
        {
            std::lock_guard lock(state->mutex);
            --state->api_calls_in_flight;
            acquired_ = false;
            state->cv.notify_all();
        }
        state_ = nullptr;
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }
    int32_t status() const noexcept {
        return status_;
    }
    AiEditorSettingsPanelState& state() const noexcept {
        return *state_;
    }

  private:
    AiEditorSettingsPanelState* state_{};
    bool acquired_{};
    int32_t status_{SAO_AI_EDITOR_ERR_HANDLE_INVALID};
};

void dispatch_action(AiEditorSettingsPanelState& state, std::string_view action,
                     const uint8_t* payload_bytes, size_t payload_len) {
    json payload;
    std::string payload_error;
    if (!read_payload(payload_bytes, payload_len, payload, payload_error)) {
        std::lock_guard lock(state.mutex);
        state.last_error = std::move(payload_error);
        return;
    }

    if (action == "nav.page") {
        const auto page = payload_string(payload, "page", 64U);
        if (!page.has_value() || page_definition(*page) == nullptr) {
            std::lock_guard lock(state.mutex);
            state.last_error = "Unknown settings page.";
            return;
        }
        std::lock_guard lock(state.mutex);
        state.active_page = *page;
        state.search_query.clear();
        state.review_filter = "all";
        state.result_offset = 0;
        state.last_error.clear();
        return;
    }
    if (action == "search.open") {
        std::string current;
        {
            std::lock_guard lock(state.mutex);
            current = state.search_query;
        }
        show_input_dialog(state, DialogIntent::Search, nullptr, "Search Settings / 搜索设置",
                          "匹配中文/英文标签、稳定 key、说明和关键词。", "Search query",
                          std::move(current), kMaximumSearchBytes);
        return;
    }
    if (action == "search.clear") {
        std::lock_guard lock(state.mutex);
        state.search_query.clear();
        state.result_offset = 0;
        state.status_message = "Search cleared / 搜索已清除";
        state.last_error.clear();
        return;
    }
    if (action == "filter.review") {
        const auto filter = payload_string(payload, "value", 32U);
        if (!filter.has_value() ||
            (*filter != "all" && *filter != "modified" && *filter != "overrides" &&
             *filter != "errors")) {
            std::lock_guard lock(state.mutex);
            state.last_error = "Unknown settings review filter.";
            return;
        }
        std::lock_guard lock(state.mutex);
        state.review_filter = *filter;
        state.result_offset = 0;
        state.status_message = *filter == "all"
                                   ? "Review filter cleared / 审核过滤已清除"
                                   : "Review filter active: " + *filter;
        state.last_error.clear();
        return;
    }
    if (action == "filter.advanced") {
        const auto value = payload_string(payload, "value", 16U);
        if (!value.has_value() || (*value != "show" && *value != "hide")) {
            std::lock_guard lock(state.mutex);
            state.last_error = "Invalid advanced-details action.";
            return;
        }
        std::lock_guard lock(state.mutex);
        state.show_advanced = *value == "show";
        state.result_offset = 0;
        state.status_message = state.show_advanced
                                   ? "Advanced details shown / 已显示高级项"
                                   : "Advanced details hidden / 已隐藏高级项";
        state.last_error.clear();
        return;
    }
    if (action == "results.prev" || action == "results.next") {
        std::lock_guard lock(state.mutex);
        const size_t total = visible_fields(state).size();
        if (action == "results.prev") {
            state.result_offset =
                state.result_offset >= kFieldsPerPage ? state.result_offset - kFieldsPerPage : 0;
        } else if (state.result_offset + kFieldsPerPage < total) {
            state.result_offset += kFieldsPerPage;
        }
        return;
    }
    const bool mutates_draft = action == "draft.discard" || action == "draft.reload" ||
                               action == "draft.reset_section" || action.starts_with("field.");
    if (mutates_draft) {
        std::lock_guard lock(state.mutex);
        if (!state.values_loaded || state.save_phase == SavePhase::Saving ||
            state.rpc_kind != RpcKind::None || state.save_pending ||
            followup_load_active(state)) {
            state.last_error = "Settings are synchronizing; wait for the current load/save. / "
                               "设置正在同步，请等待当前加载或保存完成。";
            return;
        }
    }
    if (action == "scope.select") {
        const auto scope = payload_string(payload, "scope", 32U);
        if (!scope.has_value() ||
            (*scope != "system" && *scope != "workspace" && *scope != "plugin")) {
            std::lock_guard lock(state.mutex);
            state.last_error = "Invalid settings scope.";
            return;
        }
        std::lock_guard lock(state.mutex);
        if (*scope == state.selected_scope)
            return;
        if (state.rpc_kind != RpcKind::None || state.save_pending ||
            followup_load_active(state)) {
            state.last_error = "Scope switch blocked while settings are synchronizing. / "
                               "设置同步期间不能切换作用域。";
            return;
        }
        if (*scope == "plugin" && state.selected_plugin_id.empty()) {
            state.last_error = "No plugin settings scope is registered for this workspace. / "
                               "当前工作区没有插件作用域。";
            return;
        }
        if (state_dirty(state)) {
            state.last_error = "Scope switch blocked: Apply or Discard the dirty draft first. / "
                               "切换作用域前请先应用或丢弃草稿。";
            state.status_message = "Scope unchanged / 作用域未切换";
            return;
        }
        state.selected_scope = *scope;
        state.load_pending = true;
        state.result_offset = 0;
        state.values_loaded = false;
        state.last_error.clear();
        state.status_message = "Loading " + scope_label(*scope) + " settings...";
        return;
    }
    if (action == "draft.discard") {
        std::lock_guard lock(state.mutex);
        discard_draft(state);
        return;
    }
    if (action == "draft.reload") {
        bool dirty = false;
        {
            std::lock_guard lock(state.mutex);
            dirty = state_dirty(state);
            if (!dirty) {
                state.load_pending = true;
                state.status_message = "Reload queued / 已请求重新加载";
            }
        }
        if (dirty) {
            show_confirm_dialog(
                state, DialogIntent::ConfirmReload, nullptr,
                "Discard and Reload? / 丢弃并重新加载？",
                "Reload will discard the current dirty draft. Choose OK to continue.");
        }
        return;
    }
    if (action == "draft.reset_section") {
        std::lock_guard lock(state.mutex);
        if (state.active_page == "overview" || !state.search_query.empty() ||
            state.review_filter != "all") {
            state.last_error = "Open one category before Reset Section.";
            return;
        }
        std::vector<const FieldMeta*> resettable;
        size_t skipped = 0;
        for (const auto& item : state.fields) {
            if (item.page != state.active_page)
                continue;
            if (!field_dependencies_satisfied(state, item)) {
                ++skipped;
                continue;
            }
            resettable.push_back(&item);
        }
        for (const FieldMeta* item : resettable)
            reset_field(state, *item);
        state.status_message =
            "Reset queued for " + std::to_string(resettable.size()) + " fields in this section.";
        if (skipped != 0)
            state.status_message += " Skipped " + std::to_string(skipped) + " unavailable fields.";
        return;
    }
    if (action == "draft.apply") {
        queue_save(state);
        return;
    }

    const auto key = payload_string(payload, "key");
    if (!key.has_value()) {
        std::lock_guard lock(state.mutex);
        state.last_error = "Setting action is missing a stable key.";
        return;
    }
    const FieldMeta* field_meta = nullptr;
    {
        std::lock_guard lock(state.mutex);
        field_meta = find_field(state, *key);
        if (field_meta == nullptr) {
            state.last_error = "Unknown setting key: " + *key;
            return;
        }
        if (!field_dependencies_satisfied(state, *field_meta)) {
            state.last_error = "Setting dependency is not satisfied: " +
                               dependency_display(state, *field_meta);
            return;
        }
        if ((action == "field.toggle" && field_meta->kind != FieldKind::Boolean) ||
            (action == "field.select" && field_meta->kind != FieldKind::Enum) ||
            (action == "field.clear" && !is_secret_field(*field_meta)) ||
            (action == "field.edit" && field_meta->kind == FieldKind::Boolean)) {
            state.last_error = "Setting action does not match field type: " + field_meta->key;
            return;
        }
    }
    if (action == "field.toggle") {
        std::lock_guard lock(state.mutex);
        const json* current = effective_value(state, *field_meta);
        bool value =
            field_meta->default_value.is_boolean() ? field_meta->default_value.get<bool>() : false;
        if (current != nullptr && current->is_boolean())
            value = current->get<bool>();
        set_field_value(state, *field_meta, !value);
        return;
    }
    if (action == "field.select") {
        const auto value = payload.find("value");
        if (value == payload.end()) {
            std::lock_guard lock(state.mutex);
            state.last_error = "Selection action is missing a value.";
            return;
        }
        if (const auto error = validate_value(*field_meta, *value); error.has_value()) {
            std::lock_guard lock(state.mutex);
            state.validation_errors[field_meta->key] = *error;
            state.last_error = "Invalid selection for " + field_meta->key;
            return;
        }
        std::lock_guard lock(state.mutex);
        set_field_value(state, *field_meta, *value);
        return;
    }
    if (action == "field.reset") {
        std::lock_guard lock(state.mutex);
        reset_field(state, *field_meta);
        return;
    }
    if (action == "field.clear") {
        show_confirm_dialog(state, DialogIntent::ConfirmClearSecret, field_meta,
                            "Clear Protected Value? / 清除敏感值？",
                            "The protected value will be marked for deletion and removed only "
                            "after Apply. Its contents are never displayed.");
        return;
    }
    if (action == "field.edit") {
        std::string title = field_meta->label;
        if (!field_meta->label_zh.empty())
            title.append(" / ").append(field_meta->label_zh);
        std::string prompt;
        switch (field_meta->kind) {
        case FieldKind::Json:
            prompt = is_secret_field(*field_meta) ? "Protected JSON" : "JSON";
            break;
        case FieldKind::List:
            prompt = is_secret_field(*field_meta) ? "Protected JSON array or one item per line"
                                                  : "JSON array or one item per line";
            break;
        case FieldKind::Integer:
        case FieldKind::Number:
            prompt = "Numeric value";
            break;
        case FieldKind::Enum:
            prompt = "Option value or label";
            break;
        case FieldKind::Secret:
            prompt = "New secret (plaintext is never rendered after submission)";
            break;
        case FieldKind::String:
            prompt = "Text value";
            break;
        case FieldKind::Boolean:
            prompt = "Use Toggle";
            break;
        }
        std::string default_value;
        {
            std::lock_guard lock(state.mutex);
            default_value = value_for_dialog(state, *field_meta);
        }
        show_input_dialog(state, DialogIntent::EditField, field_meta, std::move(title),
                          field_meta->description.empty() ? "Edit the selected setting."
                                                          : field_meta->description,
                          std::move(prompt), std::move(default_value),
                          is_secret_field(*field_meta) ? kMaximumSecretBytes : kMaximumInputBytes);
        return;
    }

    std::lock_guard lock(state.mutex);
    state.last_error = "Unknown settings action: " + std::string(action);
}

void SAO_UI_CALL panel_action_callback(const char* action_id_utf8, const uint8_t* payload_json_utf8,
                                       size_t payload_len, void* user_data) {
    auto* state = static_cast<AiEditorSettingsPanelState*>(user_data);
    ActionLease lease(state);
    if (!lease || action_id_utf8 == nullptr)
        return;
    try {
        dispatch_action(*state, action_id_utf8, payload_json_utf8, payload_len);
    } catch (const std::exception& error) {
        std::lock_guard lock(state->mutex);
        state->last_error = std::string("Settings action failed: ") + error.what();
    } catch (...) {
        std::lock_guard lock(state->mutex);
        state->last_error = "Settings action failed with an internal error.";
    }
    (void)refresh_body(*state, true);
}

void SAO_UI_CALL panel_event_callback(int32_t event_kind, void* user_data) {
    auto* state = static_cast<AiEditorSettingsPanelState*>(user_data);
    ActionLease lease(state);
    if (!lease)
        return;
    bool hide_dialog = false;
    {
        std::lock_guard lock(state->mutex);
        if (event_kind == SAO_UI_PANEL_EVENT_SHOW) {
            state->visible = true;
        } else if (event_kind == SAO_UI_PANEL_EVENT_HIDE ||
                   event_kind == SAO_UI_PANEL_EVENT_CLOSE) {
            state->visible = false;
            hide_dialog = true;
            if (state_dirty(*state)) {
                state->hidden_with_dirty = true;
                state->status_message = "Panel hidden with a dirty draft; changes are retained. / "
                                        "面板已隐藏，未保存草稿仍保留。";
            }
        }
    }
    if (hide_dialog) {
        const int32_t status = hide_active_dialog(*state);
        if (status != SAO_AI_EDITOR_OK) {
            std::lock_guard error_lock(state->mutex);
            state->last_error = "Failed to hide the active settings dialog (status " +
                                std::to_string(status) + ").";
        }
    }
}

int32_t tick_dialog(AiEditorSettingsPanelState& state) {
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

void expire_saved_status(AiEditorSettingsPanelState& state) {
    std::lock_guard lock(state.mutex);
    if (state.save_phase != SavePhase::Saved || state.saved_at.time_since_epoch().count() == 0)
        return;
    if (std::chrono::steady_clock::now() - state.saved_at >= kSavedStatusLifetime) {
        state.save_phase = SavePhase::Idle;
    }
}

int32_t restore_panel_handlers(AiEditorSettingsPanelState& state, int32_t original_status) {
    bool action_attached = false;
    bool event_attached = false;
#if defined(SAO_AI_EDITOR_TESTING)
    bool fail_restore_action = false;
    bool fail_restore_event = false;
#endif
    {
        std::lock_guard lock(state.mutex);
        action_attached = state.action_handler_attached;
        event_attached = state.event_handler_attached;
#if defined(SAO_AI_EDITOR_TESTING)
        fail_restore_action = state.test_fail_restore_action;
        fail_restore_event = state.test_fail_restore_event;
#endif
    }

    sao_status_t action_status = SAO_STATUS_OK;
    sao_status_t event_status = SAO_STATUS_OK;
    if (!action_attached) {
#if defined(SAO_AI_EDITOR_TESTING)
        if (fail_restore_action) {
            action_status = SAO_STATUS_ERR_UNKNOWN;
        } else
#endif
        {
            action_status =
                sao_ui_panel_set_action_handler(state.panel, &panel_action_callback, &state);
        }
        action_attached = action_status == SAO_STATUS_OK;
    }
    if (!event_attached) {
#if defined(SAO_AI_EDITOR_TESTING)
        if (fail_restore_event) {
            event_status = SAO_STATUS_ERR_UNKNOWN;
        } else
#endif
        {
            event_status =
                sao_ui_panel_set_event_handler(state.panel, &panel_event_callback, &state);
        }
        event_attached = event_status == SAO_STATUS_OK;
    }

    std::lock_guard lock(state.mutex);
    state.action_handler_attached = action_attached;
    state.event_handler_attached = event_attached;
    state.accepting = action_attached && event_attached;
    state.teardown_failed = !state.accepting;
    state.destroy_claimed = false;
    state.cv.notify_all();
    if (state.teardown_failed) {
        state.last_error = "Settings panel teardown rollback could not restore all handlers.";
        if (action_status != SAO_STATUS_OK)
            return map_ui_status(action_status);
        if (event_status != SAO_STATUS_OK)
            return map_ui_status(event_status);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    return original_status;
}

void release_destroy_claim(AiEditorSettingsPanelState& state, bool previous_accepting,
                           bool previous_teardown_failed) noexcept {
    std::lock_guard lock(state.mutex);
    state.destroy_claimed = false;
    state.accepting = previous_accepting;
    state.teardown_failed = previous_teardown_failed;
    state.cv.notify_all();
}

} // namespace

#if defined(SAO_AI_EDITOR_TESTING)
extern "C" void
    SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_test_set_owner_preflight_pause(int32_t target) {
    if (target == kTestPauseDestroyPreflight)
        g_test_destroy_registry_probe_completed.store(false, std::memory_order_release);
    g_test_owner_preflight_pause_target.store(target, std::memory_order_release);
}

extern "C" int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_test_owner_preflight_waiting_target(void) {
    return g_test_owner_preflight_waiting_target.load(std::memory_order_acquire);
}

extern "C" bool SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_test_destroy_registry_probe_completed(void) {
    return g_test_destroy_registry_probe_completed.load(std::memory_order_acquire);
}

extern "C" int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_dispatch_action_for_testing(
    sao_ai_editor_settings_panel_t panel, const char* action_id_utf8,
    const uint8_t* payload_json_utf8, size_t payload_len) {
    if (action_id_utf8 == nullptr)
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    ApiLease lease(panel, true);
    if (!lease)
        return lease.status();
    try {
        dispatch_action(lease.state(), action_id_utf8, payload_json_utf8, payload_len);
        return map_ui_status(refresh_body(lease.state(), true));
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_submit_dialog_for_testing(
    sao_ai_editor_settings_panel_t panel, const char* input_text_utf8, size_t input_text_len) {
    if (input_text_utf8 == nullptr && input_text_len != 0)
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    ApiLease lease(panel, true);
    if (!lease)
        return lease.status();
    try {
        dialog_result_callback(SAO_UI_DIALOG_BTN_OK, input_text_utf8, input_text_len,
                               &lease.state());
        const int32_t hide_status = hide_active_dialog(lease.state());
        if (hide_status != SAO_AI_EDITOR_OK)
            return hide_status;
        return map_ui_status(refresh_body(lease.state(), true));
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_snapshot_json_for_testing(
    sao_ai_editor_settings_panel_t panel, char* buffer_utf8, size_t buffer_cap, size_t* out_len) {
    if (out_len == nullptr || (buffer_utf8 == nullptr && buffer_cap != 0))
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    ApiLease lease(panel, true);
    if (!lease)
        return lease.status();
    try {
        json snapshot;
        sao_ui_dialog_handle_t dialog = nullptr;
        {
            std::lock_guard lock(lease.state().mutex);
            const auto& state = lease.state();
            json fields = json::array();
            for (const auto& field : state.fields)
                fields.push_back(field.key);
            std::vector<std::string> reset_values;
            for (const auto& key : state.reset_keys)
                reset_values.push_back(key);
            std::ranges::sort(reset_values);
            std::vector<std::string> pending_secret_set_values;
            for (const auto& [key, value] : state.pending_secret_values) {
                (void)value;
                pending_secret_set_values.push_back(key);
            }
            std::ranges::sort(pending_secret_set_values);
            std::vector<std::string> pending_secret_clear_values;
            for (const auto& key : state.pending_secret_clears)
                pending_secret_clear_values.push_back(key);
            std::ranges::sort(pending_secret_clear_values);
            json save_params = build_save_params(state);
            save_params.erase("secretUpdates");
            save_params.erase("secret_updates");
            json spec = json::parse(state.last_spec, nullptr, false, false);
            if (spec.is_discarded())
                spec = json::object();
            snapshot = {{"fields", std::move(fields)},
                        {"persisted", state.persisted},
                        {"draft", state.draft},
                        {"effective", state.effective},
                        {"inherited", state.inherited},
                        {"sources", state.sources},
                        {"scope", state.selected_scope},
                        {"pluginId", state.selected_plugin_id},
                        {"reviewFilter", state.review_filter},
                        {"showAdvanced", state.show_advanced},
                        {"visibleFieldCount", visible_fields(state).size()},
                        {"scopeOverridesExact", state.scope_overrides_exact},
                        {"metadataLoaded", state.metadata_loaded},
                        {"valuesLoaded", state.values_loaded},
                        {"effectiveLoaded", state.effective_loaded},
                        {"rpc", rpc_kind_text(state.rpc_kind)},
                        {"savePhase", save_phase_text(state.save_phase)},
                        {"savePending", state.save_pending},
                        {"resetKeys", std::move(reset_values)},
                        {"pendingSecretSets", std::move(pending_secret_set_values)},
                        {"pendingSecretClears", std::move(pending_secret_clear_values)},
                        {"saveParams", std::move(save_params)},
                        {"accepting", state.accepting},
                        {"actionHandlerAttached", state.action_handler_attached},
                        {"eventHandlerAttached", state.event_handler_attached},
                        {"teardownFailed", state.teardown_failed},
                        {"specBytes", state.last_spec.size()},
                        {"spec", std::move(spec)}};
            dialog = state.dialog;
        }
        bool dialog_is_visible = false;
        if (dialog != nullptr &&
            sao_ui_dialog_is_visible(dialog, &dialog_is_visible) != SAO_STATUS_OK) {
            dialog_is_visible = false;
        }
        snapshot["dialogVisible"] = dialog_is_visible;
        const std::string serialized = snapshot.dump();
        *out_len = serialized.size();
        if (buffer_utf8 == nullptr || buffer_cap < serialized.size())
            return buffer_utf8 == nullptr ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
        if (!serialized.empty())
            std::memcpy(buffer_utf8, serialized.data(), serialized.size());
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_set_teardown_failures_for_testing(sao_ai_editor_settings_panel_t panel,
                                                               bool fail_unregister_once,
                                                               bool fail_restore_action,
                                                               bool fail_restore_event) {
    std::lock_guard registry_lock(registry_mutex());
    const auto found = registry().find(panel);
    if (found == registry().end())
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    std::lock_guard state_lock(found->second->mutex);
    found->second->test_fail_unregister_once = fail_unregister_once;
    found->second->test_fail_restore_action = fail_restore_action;
    found->second->test_fail_restore_event = fail_restore_event;
    return SAO_AI_EDITOR_OK;
}

extern "C" int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_get_teardown_state_for_testing(
    sao_ai_editor_settings_panel_t panel, bool* out_accepting, bool* out_action_handler_attached,
    bool* out_event_handler_attached, bool* out_teardown_failed) {
    if (out_accepting == nullptr || out_action_handler_attached == nullptr ||
        out_event_handler_attached == nullptr || out_teardown_failed == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard registry_lock(registry_mutex());
    const auto found = registry().find(panel);
    if (found == registry().end())
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    std::lock_guard state_lock(found->second->mutex);
    *out_accepting = found->second->accepting;
    *out_action_handler_attached = found->second->action_handler_attached;
    *out_event_handler_attached = found->second->event_handler_attached;
    *out_teardown_failed = found->second->teardown_failed;
    return SAO_AI_EDITOR_OK;
}
#endif

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_create(
    sao_ui_compositor_handle_t borrowed_compositor, sao_ai_editor_launcher_t borrowed_launcher,
    sao_ai_editor_settings_panel_t* out_panel) {
    if (out_panel == nullptr || borrowed_compositor == nullptr)
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    *out_panel = nullptr;
    const int32_t owner_status = require_owner_thread(borrowed_compositor);
    if (owner_status != SAO_AI_EDITOR_OK)
        return owner_status;
    try {
        auto state = std::make_unique<AiEditorSettingsPanelState>();
        state->compositor = borrowed_compositor;
        state->launcher = borrowed_launcher;
        state->describe_pending = borrowed_launcher != nullptr;
        state->load_pending = borrowed_launcher != nullptr;
        state->backend_connected = false;
        state->backend_message =
            borrowed_launcher == nullptr
                ? "Offline / 离线: backend not attached; local draft remains available."
                : "Connecting to settings backend...";

        SaoPanelDescriptor descriptor{};
        descriptor.struct_size = sizeof(SaoPanelDescriptor);
        descriptor.panel_id_utf8 = SAO_AI_EDITOR_SETTINGS_PANEL_ID;
        descriptor.title_utf8 = "AI Editor Settings";
        descriptor.anchor = SAO_UI_PANEL_ANCHOR_CENTER;
        descriptor.default_width_px = 1040;
        descriptor.default_height_px = 820;
        descriptor.min_width_px = 900;
        descriptor.min_height_px = 720;
        descriptor.movable = true;
        descriptor.resizable = true;
        descriptor.show_titlebar = true;
        descriptor.show_close_button = true;
        descriptor.visible = false;
        descriptor.remember_geometry = true;
        descriptor.modal = false;
        descriptor.overlay_style = false;
        descriptor.z_class = SAO_UI_PANEL_Z_NORMAL;
        descriptor.theme_override_json_utf8 = kDarkThemeOverride;
        descriptor.initial_opacity = 1.0F;
        descriptor.auto_scroll = true;

        sao_status_t status =
            sao_ui_panel_register(borrowed_compositor, &descriptor, &state->panel, &state->body);
        if (status != SAO_STATUS_OK)
            return map_ui_status(status);
        status = sao_ui_panel_set_action_handler(state->panel, &panel_action_callback, state.get());
        if (status == SAO_STATUS_OK)
            state->action_handler_attached = true;
        if (status == SAO_STATUS_OK) {
            status =
                sao_ui_panel_set_event_handler(state->panel, &panel_event_callback, state.get());
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

        const sao_ai_editor_settings_panel_t handle = allocate_handle();
        auto* const raw = state.get();
        try {
            std::lock_guard lock(registry_mutex());
            const auto [slot, inserted] = registry().try_emplace(handle);
            if (!inserted)
                throw std::runtime_error("AI Editor settings handle collision");
            slot->second = std::move(state);
        } catch (...) {
            (void)sao_ui_panel_set_event_handler(raw->panel, nullptr, nullptr);
            (void)sao_ui_panel_set_action_handler(raw->panel, nullptr, nullptr);
            (void)sao_ui_panel_unregister(raw->panel);
            throw;
        }
        *out_panel = handle;
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_show(sao_ai_editor_settings_panel_t panel) {
    ApiLease lease(panel, true);
    if (!lease)
        return lease.status();
    try {
        auto& state = lease.state();
        bool previous_visible = false;
        bool previous_describe_pending = false;
        bool previous_load_pending = false;
        bool previous_hidden_with_dirty = false;
        std::string previous_status_message;
        {
            std::lock_guard lock(state.mutex);
            previous_visible = state.visible;
            previous_describe_pending = state.describe_pending;
            previous_load_pending = state.load_pending;
            previous_hidden_with_dirty = state.hidden_with_dirty;
            previous_status_message = state.status_message;
        }
        const sao_status_t show_status = sao_ui_panel_show(state.panel);
        if (show_status != SAO_STATUS_OK)
            return map_ui_status(show_status);
        const sao_status_t front_status = sao_ui_panel_bring_to_front(state.panel);
        if (front_status != SAO_STATUS_OK) {
            if (!previous_visible)
                (void)sao_ui_panel_hide(state.panel);
            std::lock_guard lock(state.mutex);
            state.visible = previous_visible;
            state.describe_pending = previous_describe_pending;
            state.load_pending = previous_load_pending;
            state.hidden_with_dirty = previous_hidden_with_dirty;
            state.status_message = std::move(previous_status_message);
            return map_ui_status(front_status);
        }
        {
            std::lock_guard lock(state.mutex);
            state.visible = true;
            if (!state.backend_connected && state.launcher != nullptr) {
                state.describe_pending = !state.metadata_loaded;
                state.load_pending = true;
            }
            if (state_dirty(state)) {
                state.hidden_with_dirty = true;
                state.status_message = "Unsaved draft restored / 已恢复未保存草稿。";
            }
        }
        const sao_status_t refresh_status = refresh_body(state, true);
        if (refresh_status != SAO_STATUS_OK) {
            if (!previous_visible)
                (void)sao_ui_panel_hide(state.panel);
            std::lock_guard lock(state.mutex);
            state.visible = previous_visible;
            state.describe_pending = previous_describe_pending;
            state.load_pending = previous_load_pending;
            state.hidden_with_dirty = previous_hidden_with_dirty;
            state.status_message = std::move(previous_status_message);
            return map_ui_status(refresh_status);
        }
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_hide(sao_ai_editor_settings_panel_t panel) {
    ApiLease lease(panel, true);
    if (!lease)
        return lease.status();
    try {
        auto& state = lease.state();
        const int32_t dialog_status = hide_active_dialog(state);
        if (dialog_status != SAO_AI_EDITOR_OK)
            return dialog_status;
        const sao_status_t status = sao_ui_panel_hide(state.panel);
        if (status != SAO_STATUS_OK)
            return map_ui_status(status);
        {
            std::lock_guard lock(state.mutex);
            state.visible = false;
            if (state_dirty(state)) {
                state.hidden_with_dirty = true;
                state.status_message = "Panel hidden with a dirty draft; changes are retained. / "
                                       "面板已隐藏，草稿仍保留。";
            }
        }
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_tick(sao_ai_editor_settings_panel_t panel) {
    ApiLease lease(panel, true);
    if (!lease)
        return lease.status();
    auto& state = lease.state();
    try {
        const int32_t dialog_status = tick_dialog(state);
        if (dialog_status != SAO_AI_EDITOR_OK)
            return dialog_status;
        const int32_t completion_status = poll_rpc_completion(state);
        if (completion_status != SAO_AI_EDITOR_OK)
            return completion_status;
        if (std::optional<RpcJob> job = take_next_rpc_job(state); job.has_value()) {
            const int32_t start_status = start_rpc_job(state, std::move(*job));
            if (start_status != SAO_AI_EDITOR_OK)
                return start_status;
        }
        expire_saved_status(state);
        return map_ui_status(refresh_body(state, false));
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_try_destroy(sao_ai_editor_settings_panel_t panel) {
    if (panel == nullptr)
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    try {
        AiEditorSettingsPanelState* state = nullptr;
        bool previous_accepting = false;
        bool previous_teardown_failed = false;
        std::future<RpcCompletion> completed_rpc;
        {
            std::lock_guard registry_lock(registry_mutex());
            const auto found = registry().find(panel);
            if (found == registry().end())
                return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
            state = found->second.get();
            std::lock_guard state_lock(state->mutex);
            if (state->destroy_claimed) {
#if defined(SAO_AI_EDITOR_TESTING)
                note_destroy_registry_probe_completed_for_testing();
#endif
                return SAO_AI_EDITOR_ERR_BUSY;
            }
            if (state->api_calls_in_flight != 0 || state->actions_in_flight != 0)
                return SAO_AI_EDITOR_ERR_BUSY;
            previous_accepting = state->accepting;
            previous_teardown_failed = state->teardown_failed;
            state->destroy_claimed = true;
            state->accepting = false;
            state->teardown_failed = false;
        }

#if defined(SAO_AI_EDITOR_TESTING)
        pause_owner_preflight_for_testing(kTestPauseDestroyPreflight);
#endif
        const int32_t owner_status = require_owner_thread(state->compositor);
        if (owner_status != SAO_AI_EDITOR_OK) {
            release_destroy_claim(*state, previous_accepting, previous_teardown_failed);
            return owner_status;
        }

        bool rpc_busy = false;
        {
            std::lock_guard state_lock(state->mutex);
            if (!state->destroy_claimed)
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            rpc_busy = (state->rpc_kind != RpcKind::None && !state->rpc_future.valid()) ||
                       (state->rpc_future.valid() &&
                        state->rpc_future.wait_for(std::chrono::milliseconds(0)) !=
                            std::future_status::ready);
        }
        if (rpc_busy) {
            release_destroy_claim(*state, previous_accepting, previous_teardown_failed);
            return SAO_AI_EDITOR_ERR_BUSY;
        }

        const int32_t dialog_status = hide_active_dialog(*state);
        if (dialog_status != SAO_AI_EDITOR_OK)
            return restore_panel_handlers(*state, dialog_status);

        bool action_attached = false;
        bool event_attached = false;
        {
            std::lock_guard state_lock(state->mutex);
            action_attached = state->action_handler_attached;
            event_attached = state->event_handler_attached;
        }

        if (action_attached) {
            const sao_status_t action_status =
                sao_ui_panel_set_action_handler(state->panel, nullptr, nullptr);
            if (action_status != SAO_STATUS_OK)
                return restore_panel_handlers(*state, map_ui_status(action_status));
            std::lock_guard state_lock(state->mutex);
            state->action_handler_attached = false;
        }
        if (event_attached) {
            const sao_status_t event_status =
                sao_ui_panel_set_event_handler(state->panel, nullptr, nullptr);
            if (event_status != SAO_STATUS_OK)
                return restore_panel_handlers(*state, map_ui_status(event_status));
            std::lock_guard state_lock(state->mutex);
            state->event_handler_attached = false;
        }

        sao_status_t unregister_status = SAO_STATUS_OK;
#if defined(SAO_AI_EDITOR_TESTING)
        bool fail_unregister = false;
        {
            std::lock_guard state_lock(state->mutex);
            fail_unregister = state->test_fail_unregister_once;
            state->test_fail_unregister_once = false;
        }
        unregister_status =
            fail_unregister ? SAO_STATUS_ERR_UNKNOWN : sao_ui_panel_unregister(state->panel);
#else
        unregister_status = sao_ui_panel_unregister(state->panel);
#endif
        if (unregister_status != SAO_STATUS_OK)
            return restore_panel_handlers(*state, map_ui_status(unregister_status));

        sao_ui_dialog_handle_t dialog = nullptr;
        {
            std::lock_guard state_lock(state->mutex);
            dialog = state->dialog;
            state->dialog = nullptr;
            if (state->rpc_future.valid()) {
                completed_rpc = std::move(state->rpc_future);
                state->rpc_future = {};
            }
            state->rpc_kind = RpcKind::None;
        }
        if (dialog != nullptr)
            sao_ui_dialog_destroy(dialog);
        if (completed_rpc.valid()) {
            try {
                (void)completed_rpc.get();
            } catch (...) {
            }
        }

        std::unique_ptr<AiEditorSettingsPanelState> owned;
        {
            std::lock_guard registry_lock(registry_mutex());
            const auto found = registry().find(panel);
            if (found == registry().end() || found->second.get() != state)
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            std::lock_guard state_lock(state->mutex);
            if (!state->destroy_claimed)
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            owned = std::move(found->second);
            registry().erase(found);
        }
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}
