#include "sao/ai_editor/ai_editor_native.h"

#include "ai_editor_settings.h"
#include "chat_provider_router.h"
#include "kernel_map_commands.h"
#include "kernel_map_panel_provider.h"
#include "kernel_map_tools.h"
#include "mcp_management_panel_provider.h"
#include "native_runtime_internal.h"
#include "plugin_contributions.h"
#include "sao/ai_editor/kernel_map_bridge.h"
#include "sha256_helper.h"
#include "vt_bridge.h"
#include "vt_commands.h"
#include "vt_tools.h"

#include <commdlg.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sao::ai_editor::native {
namespace {

constexpr uint32_t kDefaultEventDrainLimit = 32U;
constexpr uint32_t kMaximumEventDrainLimit = 64U;
constexpr size_t kMaximumEventDrainBytes = 768U * 1024U;
constexpr size_t kMaximumWebviewHtmlBytes = 1024U * 1024U;

struct McpNotificationGate final {
    std::mutex mutex;
    std::condition_variable ready;
    bool accepting = true;
    uint32_t inflight = 0;
};
std::mutex mcp_notification_gates_mutex;
std::unordered_map<NativeRuntime*, std::shared_ptr<McpNotificationGate>> mcp_notification_gates;
std::shared_ptr<McpNotificationGate> get_mcp_gate(NativeRuntime* runtime) {
    std::lock_guard<std::mutex> lock(mcp_notification_gates_mutex);
    const auto found = mcp_notification_gates.find(runtime);
    return found == mcp_notification_gates.end() ? nullptr : found->second;
}
void stop_mcp_notifications(NativeRuntime* runtime, SaoAiEditorMcpClient* client) {
    const auto gate = get_mcp_gate(runtime);
    if (gate) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->accepting = false;
    }
    if (client)
        (void)sao_ai_editor_mcp_client_set_notification_forwarder(client, nullptr, nullptr);
    if (gate) {
        std::unique_lock<std::mutex> lock(gate->mutex);
        gate->ready.wait(lock, [&] { return gate->inflight == 0; });
    }
    std::lock_guard<std::mutex> lock(mcp_notification_gates_mutex);
    mcp_notification_gates.erase(runtime);
}

void runtime_trace_stage(const wchar_t* stage, int32_t status = 0) {
    wchar_t enabled[2]{};
    if (stage == nullptr ||
        GetEnvironmentVariableW(L"SAO_AI_EDITOR_TRACE", enabled,
                                static_cast<DWORD>(std::size(enabled))) == 0u ||
        enabled[0] != L'1') {
        return;
    }
    wchar_t directory[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", directory,
                                static_cast<DWORD>(std::size(directory))) == 0u) {
        return;
    }
    const auto directory_path = std::filesystem::path(directory) / L"SAOAuto";
    std::error_code error;
    std::filesystem::create_directories(directory_path, error);
    if (error) {
        return;
    }
    const auto path = directory_path / L"ai-editor-trace.log";
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == nullptr || file == INVALID_HANDLE_VALUE) {
        return;
    }
    wchar_t line[1024]{};
    const int length = _snwprintf_s(line, std::size(line), _TRUNCATE,
                                    L"[pid=%lu tid=%lu] runtime.%ls status=%d\r\n",
                                    static_cast<unsigned long>(GetCurrentProcessId()),
                                    static_cast<unsigned long>(GetCurrentThreadId()), stage,
                                    static_cast<int>(status));
    if (length > 0) {
        DWORD written = 0;
        (void)WriteFile(file, line, static_cast<DWORD>(length * sizeof(wchar_t)), &written,
                        nullptr);
        OutputDebugStringW(line);
    }
    CloseHandle(file);
}

bool valid_sha256_hex(std::string_view value) noexcept {
    if (value.size() != 64U)
        return false;
    return std::ranges::all_of(value, [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    });
}

enum class RuntimeHandleState : uint8_t {
    live,
    retired,
};

std::mutex runtime_handle_registry_mutex;
std::unordered_map<SaoAiEditorRuntime*, RuntimeHandleState> runtime_handle_registry;

bool publish_runtime_handle(SaoAiEditorRuntime* handle) {
    if (handle == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(runtime_handle_registry_mutex);
    return runtime_handle_registry.emplace(handle, RuntimeHandleState::live).second;
}

bool runtime_handle_is_live(SaoAiEditorRuntime* handle) noexcept {
    if (handle == nullptr) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(runtime_handle_registry_mutex);
        const auto found = runtime_handle_registry.find(handle);
        return found != runtime_handle_registry.end() && found->second == RuntimeHandleState::live;
    } catch (...) {
        return false;
    }
}

bool retire_runtime_handle(SaoAiEditorRuntime* handle) noexcept {
    if (handle == nullptr) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(runtime_handle_registry_mutex);
        const auto found = runtime_handle_registry.find(handle);
        if (found == runtime_handle_registry.end() ||
            found->second == RuntimeHandleState::retired) {
            return false;
        }
        found->second = RuntimeHandleState::retired;
        return true;
    } catch (...) {
        return false;
    }
}

std::string pick_kernel_driver_file() {
    std::vector<wchar_t> path(32768u, L'\0');
    constexpr wchar_t filter[] = L"Driver images (*.sys)\0*.sys\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrDefExt = L"sys";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (::GetOpenFileNameW(&dialog) == FALSE) {
        return {};
    }
    return wide_to_utf8(path.data());
}

std::string status_message(int32_t status) {
    switch (status) {
    case SAO_AI_EDITOR_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL:
        return "buffer too small";
    case SAO_AI_EDITOR_ERR_HANDLE_INVALID:
        return "invalid handle";
    case SAO_AI_EDITOR_ERR_TIMEOUT:
        return "timeout";
    case SAO_AI_EDITOR_ERR_NOT_FOUND:
        return "not found";
    case SAO_AI_EDITOR_ERR_PERMISSION_DENIED:
        return "permission denied";
    case SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED:
        return "confirmation required";
    case SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION:
        return "workspace boundary violation";
    case SAO_AI_EDITOR_ERR_CANCELLED:
        return "cancelled";
    case SAO_AI_EDITOR_ERR_HTTP:
        return "HTTP request failed";
    case SAO_AI_EDITOR_ERR_PROTOCOL:
        return "protocol error";
    case SAO_AI_EDITOR_ERR_BUSY:
        return "busy";
    default:
        return "operation failed";
    }
}
std::string trim_copy(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
                          return std::isspace(c) != 0;
                      }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string normalized_secret_key(std::string_view key) {
    std::string normalized;
    normalized.reserve(key.size());
    for (const unsigned char character : key) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return normalized;
}

bool secret_field(std::string_view key) {
    const std::string normalized = normalized_secret_key(key);
    return normalized.find("secret") != std::string::npos ||
           normalized.find("token") != std::string::npos ||
           normalized.find("password") != std::string::npos ||
           normalized.find("apikey") != std::string::npos ||
           normalized.find("authorization") != std::string::npos ||
           normalized.find("cookie") != std::string::npos;
}
Json redact_secret_fields(const Json& value) {
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value)
            out.push_back(redact_secret_fields(item));
        return out;
    }
    if (!value.is_object())
        return value;
    Json out = Json::object();
    for (const auto& [key, item] : value.items())
        out[key] = secret_field(key) ? Json{"<redacted>"} : redact_secret_fields(item);
    return out;
}

// Locate the on-disk workflow history directory for a scope.  Anchored to
// ScopeStore::history_root("<scope>") so we stay in sync with the
// chat_history layout convention — the sibling directory naming means new
// scope roots (system_root_ / workspace_scope_root_) get picked up
// automatically without exposing those private paths.
std::filesystem::path workflow_history_root(const ScopeStore& scopes, std::string_view scope) {
    const std::filesystem::path chat_history = scopes.history_root(scope);
    if (chat_history.empty()) {
        return {};
    }
    return chat_history.parent_path() / L"workflow_history";
}

int rpc_code(int32_t status) {
    switch (status) {
    case SAO_AI_EDITOR_ERR_INVALID_ARGUMENT:
        return -32602;
    case SAO_AI_EDITOR_ERR_NOT_FOUND:
        return -32004;
    case SAO_AI_EDITOR_ERR_PERMISSION_DENIED:
        return -32003;
    case SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED:
        return -32002;
    case SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION:
        return -32001;
    case SAO_AI_EDITOR_ERR_CANCELLED:
        return -32800;
    default:
        return -32000;
    }
}

std::string new_run_id() {
    static std::atomic<uint64_t> sequence{0};
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return "run-" + std::to_string(ticks) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::string environment_value(std::string_view name) {
    if (name.empty() || !valid_utf8(name)) {
        return {};
    }
    const std::wstring wide_name = utf8_to_wide(name);
    const DWORD required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (required == 0 || required > 64U * 1024U) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD written =
        GetEnvironmentVariableW(wide_name.c_str(), value.data(), static_cast<DWORD>(value.size()));
    if (written == 0 || written >= value.size()) {
        return {};
    }
    value.resize(written);
    return wide_to_utf8(value);
}

std::optional<std::filesystem::path> current_executable_path() {
    std::wstring buffer(32768u, L'\0');
    const DWORD written =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0u || written >= buffer.size()) {
        return std::nullopt;
    }
    buffer.resize(written);
    return std::filesystem::path(buffer);
}

bool equal_filename(std::wstring left, std::wstring right) {
    std::transform(left.begin(), left.end(), left.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    std::transform(right.begin(), right.end(), right.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return left == right;
}

bool is_production_ai_editor_process() {
    const auto executable = current_executable_path();
    return executable.has_value() &&
           equal_filename(executable->filename().native(), L"SaoAiEditor.exe");
}

std::string resolve_panel_assets(std::string_view system_root, std::string_view panel_directory) {
    std::vector<std::filesystem::path> candidates;
    if (!system_root.empty()) {
        candidates.emplace_back(std::filesystem::path(utf8_to_wide(system_root)) / L"assets" /
                                L"ai_editor" / utf8_to_wide(panel_directory));
    }
    if (const auto executable = current_executable_path(); executable.has_value()) {
        candidates.emplace_back(executable->parent_path() / L"assets" / L"ai_editor" /
                                utf8_to_wide(panel_directory));
    }
    std::error_code error;
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_directory(candidate, error) && !error) {
            return wide_to_utf8(candidate.native());
        }
        error.clear();
    }
    return {};
}

std::string mode_of(const Json& params) {
    std::string mode = params.value("mode", "agent");
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return mode;
}

bool supported_mode(std::string_view mode) {
    return mode == "agent" || mode == "ask" || mode == "plan";
}

int32_t parse_event_drain_limit(const Json& params, uint32_t& limit) {
    limit = kDefaultEventDrainLimit;
    const auto found = params.find("limit");
    if (found == params.end()) {
        return SAO_AI_EDITOR_OK;
    }
    if (!found->is_number_integer()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    uint64_t requested = 0;
    if (found->is_number_unsigned()) {
        requested = found->get<uint64_t>();
    } else {
        const int64_t signed_requested = found->get<int64_t>();
        if (signed_requested <= 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        requested = static_cast<uint64_t>(signed_requested);
    }
    if (requested == 0 || requested > kMaximumEventDrainLimit) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    limit = static_cast<uint32_t>(requested);
    return SAO_AI_EDITOR_OK;
}

std::string approval_of(const Json& params) {
    std::string approval = params.value("approval", "default");
    std::transform(approval.begin(), approval.end(), approval.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return approval;
}

bool supported_approval(std::string_view approval) {
    return approval == "default" || approval == "bypass" || approval == "autopilot";
}

bool auto_approves(std::string_view approval) {
    return approval == "bypass" || approval == "autopilot";
}

std::string permission_value(const Json& value) {
    if (!value.is_string()) {
        return {};
    }
    std::string permission = value.get<std::string>();
    std::transform(
        permission.begin(), permission.end(), permission.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return permission == "allowed" || permission == "confirm" || permission == "disabled"
               ? permission
               : std::string{};
}

int mode_restriction(std::string_view mode) {
    if (mode == "agent") {
        return 0;
    }
    if (mode == "plan") {
        return 1;
    }
    return mode == "ask" ? 2 : -1;
}

std::string stricter_mode(std::string_view left, std::string_view right) {
    return mode_restriction(left) >= mode_restriction(right) ? std::string(left)
                                                             : std::string(right);
}

std::string stricter_approval(std::string_view settings_approval,
                              std::string_view request_approval) {
    if (settings_approval == "default" || request_approval == "default") {
        return "default";
    }
    return std::string(request_approval);
}

int permission_restriction(std::string_view permission) {
    if (permission == "allowed") {
        return 0;
    }
    if (permission == "confirm") {
        return 1;
    }
    return permission == "disabled" ? 2 : -1;
}

std::string default_permission(std::string_view mode, std::string_view category) {
    if (category == "read") {
        return "allowed";
    }
    if (category != "write" && category != "execute") {
        return {};
    }
    if (mode == "ask") {
        return "disabled";
    }
    return mode == "plan" ? "confirm" : "allowed";
}

int32_t validate_permissions(const Json& permissions) {
    if (!permissions.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    for (const std::string_view category : {"read", "write", "execute"}) {
        const std::string key(category);
        if (permissions.contains(key) && permission_value(permissions[key]).empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }
    if (!permissions.contains("tools")) {
        return SAO_AI_EDITOR_OK;
    }
    if (!permissions["tools"].is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    for (const auto& [_name, value] : permissions["tools"].items()) {
        if (permission_value(value).empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }
    return SAO_AI_EDITOR_OK;
}

struct RuntimePolicySnapshot final {
    std::string settings_mode;
    std::string request_mode;
    std::string effective_mode;
    std::string settings_approval;
    std::string request_approval;
    std::string effective_approval;
    Json settings_permissions = Json::object();
    Json request_permissions = Json::object();
    bool request_permission_layer{};
};

int32_t prepare_runtime_policy(const ScopeStore& scopes, const Json& request,
                               RuntimePolicySnapshot& policy) {
    if (!request.is_object() || (request.contains("mode") && !request["mode"].is_string()) ||
        (request.contains("approval") && !request["approval"].is_string()) ||
        (request.contains("permissions") && !request["permissions"].is_object())) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }

    Json settings;
    const int32_t settings_status =
        AiEditorSettings::prepare_runtime_request(scopes, Json::object(), settings);
    if (settings_status != SAO_AI_EDITOR_OK) {
        return settings_status;
    }

    policy.settings_mode = mode_of(settings);
    policy.request_mode = request.contains("mode") ? mode_of(request) : policy.settings_mode;
    policy.settings_approval = approval_of(settings);
    policy.request_approval =
        request.contains("approval") ? approval_of(request) : policy.settings_approval;
    if (!supported_mode(policy.settings_mode) || !supported_mode(policy.request_mode) ||
        !supported_approval(policy.settings_approval) ||
        !supported_approval(policy.request_approval)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }

    policy.settings_permissions = settings.value("permissions", Json::object());
    policy.request_permissions = request.value("permissions", Json::object());
    int32_t status = validate_permissions(policy.settings_permissions);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    status = validate_permissions(policy.request_permissions);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }

    policy.effective_mode = stricter_mode(policy.settings_mode, policy.request_mode);
    policy.effective_approval =
        stricter_approval(policy.settings_approval, policy.request_approval);
    policy.request_permission_layer = request.contains("mode") || request.contains("permissions");
    return SAO_AI_EDITOR_OK;
}

struct PermissionResolution final {
    std::string permission;
    std::string source;
};

int32_t permission_entry(const Json& source, std::string_view name, bool& found,
                         std::string& permission) {
    found = false;
    permission.clear();
    if (!source.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string key(name);
    if (!source.contains(key)) {
        return SAO_AI_EDITOR_OK;
    }
    found = true;
    permission = permission_value(source[key]);
    return permission.empty() ? SAO_AI_EDITOR_ERR_INVALID_ARGUMENT : SAO_AI_EDITOR_OK;
}

int32_t resolve_permission_layer(const Json& permissions, std::string_view mode,
                                 std::string_view requested_name, std::string_view canonical_name,
                                 std::string_view category, std::string_view layer,
                                 PermissionResolution& resolution) {
    const Json* tool_permissions = nullptr;
    if (permissions.contains("tools")) {
        if (!permissions["tools"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        tool_permissions = &permissions["tools"];
    }
    const auto resolve_name = [&](std::string_view name, std::string_view source_name,
                                  PermissionResolution& output) -> int32_t {
        bool found = false;
        std::string permission;
        int32_t status = permission_entry(permissions, name, found, permission);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        if (!found && tool_permissions != nullptr) {
            status = permission_entry(*tool_permissions, name, found, permission);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        if (found) {
            output.permission = std::move(permission);
            output.source = std::string(layer) + "." + std::string(source_name);
        }
        return SAO_AI_EDITOR_OK;
    };

    int32_t status = resolve_name(requested_name, "tool", resolution);
    if (status != SAO_AI_EDITOR_OK || !resolution.permission.empty()) {
        return status;
    }
    if (canonical_name != requested_name) {
        status = resolve_name(canonical_name, "canonical", resolution);
        if (status != SAO_AI_EDITOR_OK || !resolution.permission.empty()) {
            return status;
        }
    }

    bool found = false;
    std::string permission;
    status = permission_entry(permissions, category, found, permission);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    if (found) {
        resolution.permission = std::move(permission);
        resolution.source = std::string(layer) + ".category";
        return SAO_AI_EDITOR_OK;
    }

    resolution.permission = default_permission(mode, category);
    resolution.source = std::string(layer) + ".mode";
    return resolution.permission.empty() ? SAO_AI_EDITOR_ERR_INVALID_ARGUMENT : SAO_AI_EDITOR_OK;
}

int32_t tool_permission_category(const Json* descriptor, std::string_view canonical_name,
                                 std::string& category) {
    category.clear();
    if (descriptor == nullptr || !descriptor->is_object()) {
        return SAO_AI_EDITOR_OK;
    }
    if (descriptor->contains("category")) {
        if (!(*descriptor)["category"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        category = (*descriptor)["category"].get<std::string>();
        std::transform(
            category.begin(), category.end(), category.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        if (category == "read" || category == "write" || category == "execute") {
            return SAO_AI_EDITOR_OK;
        }
        category.clear();
    }
    if (!descriptor->contains("readOnly") || !(*descriptor)["readOnly"].is_boolean()) {
        return SAO_AI_EDITOR_OK;
    }
    if ((*descriptor)["readOnly"].get<bool>()) {
        category = "read";
    } else {
        category = canonical_name == "editFile" ? "write" : "execute";
    }
    return SAO_AI_EDITOR_OK;
}

int32_t resolve_tool_permission(const RuntimePolicySnapshot& policy,
                                std::string_view requested_name, std::string_view canonical_name,
                                const Json* descriptor, PermissionResolution& resolution,
                                std::string& category) {
    int32_t status = tool_permission_category(descriptor, canonical_name, category);
    if (status != SAO_AI_EDITOR_OK || category.empty()) {
        return status;
    }

    PermissionResolution settings_resolution;
    status =
        resolve_permission_layer(policy.settings_permissions, policy.settings_mode, requested_name,
                                 canonical_name, category, "settings", settings_resolution);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    resolution = settings_resolution;

    if (policy.request_permission_layer) {
        PermissionResolution request_resolution;
        status = resolve_permission_layer(policy.request_permissions, policy.request_mode,
                                          requested_name, canonical_name, category, "request",
                                          request_resolution);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        const int settings_restriction = permission_restriction(settings_resolution.permission);
        const int request_restriction = permission_restriction(request_resolution.permission);
        if (request_restriction > settings_restriction ||
            (request_restriction == settings_restriction &&
             request_resolution.source.find(".mode") == std::string::npos)) {
            resolution = std::move(request_resolution);
        }
    }
    return SAO_AI_EDITOR_OK;
}

const Json* find_tool_descriptor(const Json& tools, std::string_view name) {
    if (!tools.is_array()) {
        return nullptr;
    }
    for (const auto& tool : tools) {
        if (tool.is_object() && tool.value("name", std::string{}) == name) {
            return &tool;
        }
    }
    return nullptr;
}

struct ToolExecutionPlan final {
    std::string mode;
    Json arguments = Json::object();
    PermissionResolution permission;
    std::string category;
};

int32_t prepare_tool_execution(const RuntimePolicySnapshot& policy, std::string_view requested_name,
                               std::string_view resolved_name, const Json* descriptor,
                               const Json& arguments, ToolExecutionPlan& plan) {
    if (!arguments.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    plan.mode = policy.effective_mode;
    plan.arguments = arguments;
    const bool explicit_confirmation_required =
        descriptor != nullptr && descriptor->is_object() &&
        descriptor->value("explicitConfirmationRequired", false);
    const bool confirmed = plan.arguments.contains("confirmed") &&
                           plan.arguments["confirmed"].is_boolean() &&
                           plan.arguments["confirmed"].get<bool>();
    if (explicit_confirmation_required && !confirmed) {
        return SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED;
    }
    const int32_t status = resolve_tool_permission(policy, requested_name, resolved_name,
                                                   descriptor, plan.permission, plan.category);
    if (status != SAO_AI_EDITOR_OK || plan.permission.permission.empty()) {
        return status;
    }
    if (plan.permission.permission == "disabled") {
        return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
    }
    if (plan.permission.permission == "allowed") {
        plan.mode = "agent";
        return SAO_AI_EDITOR_OK;
    }

    plan.mode = "plan";
    if (!confirmed && !auto_approves(policy.effective_approval)) {
        return SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED;
    }
    if (!confirmed) {
        plan.arguments["confirmed"] = true;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t apply_tool_permissions(const RuntimePolicySnapshot& policy, Json& tools) {
    if (!tools.is_array()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    for (auto& tool : tools) {
        if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string name = tool["name"].get<std::string>();
        const std::string alias_of = tool.value("aliasOf", name);
        PermissionResolution resolution;
        std::string category;
        const int32_t status =
            resolve_tool_permission(policy, name, alias_of, &tool, resolution, category);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        if (resolution.permission.empty()) {
            continue;
        }
        tool["permission"] = resolution.permission;
        tool["permissionSource"] = resolution.source;
        tool["permissionCategory"] = category;
    }
    return SAO_AI_EDITOR_OK;
}

Json reported_permission_overrides(const RuntimePolicySnapshot& policy) {
    if (!policy.request_permission_layer || policy.request_permissions.empty()) {
        return policy.settings_permissions;
    }
    if (policy.settings_permissions.empty()) {
        return policy.request_permissions;
    }
    return Json{{"settings", policy.settings_permissions}, {"request", policy.request_permissions}};
}

struct SecretSnapshot final {
    bool present{};
    std::string value;
};

int32_t snapshot_secret(SecretStore& secrets, std::string_view key, SecretSnapshot& snapshot) {
    snapshot = {};
    const int32_t status = secrets.get(key, snapshot.value);
    if (status == SAO_AI_EDITOR_OK) {
        snapshot.present = true;
        return SAO_AI_EDITOR_OK;
    }
    return status == SAO_AI_EDITOR_ERR_NOT_FOUND ? SAO_AI_EDITOR_OK : status;
}

int32_t write_secret(SecretStore& secrets, std::string_view key, std::string_view value) {
    if (!value.empty()) {
        return secrets.set(key, value);
    }
    const int32_t status = secrets.erase(key);
    return status == SAO_AI_EDITOR_ERR_NOT_FOUND ? SAO_AI_EDITOR_OK : status;
}

int32_t restore_secret(SecretStore& secrets, std::string_view key, const SecretSnapshot& snapshot) {
    if (snapshot.present) {
        return secrets.set(key, snapshot.value);
    }
    const int32_t status = secrets.erase(key);
    return status == SAO_AI_EDITOR_ERR_NOT_FOUND ? SAO_AI_EDITOR_OK : status;
}

bool scope_exists(const ScopeStore& scopes, std::string_view scope, std::string_view plugin_id) {
    const Json described = scopes.describe_scopes();
    if (!described.is_array()) {
        return false;
    }
    for (const auto& entry : described) {
        if (!entry.is_object() || entry.value("scope", std::string{}) != scope) {
            continue;
        }
        if (scope != "plugin" || entry.value("pluginId", std::string{}) == plugin_id) {
            return true;
        }
    }
    return false;
}

int32_t apply_workflow_approval_policy(const Json& params, WorkflowDefinition& definition) {
    const std::string approval = approval_of(params);
    if (!supported_approval(approval)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (auto_approves(approval)) {
        for (auto& step : definition.steps) {
            step.requires_confirmation = false;
        }
    }
    return SAO_AI_EDITOR_OK;
}

void copy_chat_option(Json& body, const Json& params, std::string_view output_key,
                      std::initializer_list<std::string_view> input_keys) {
    for (const auto input_key : input_keys) {
        const std::string key(input_key);
        if (params.contains(key)) {
            body[std::string(output_key)] = params[key];
            return;
        }
    }
}

int32_t build_openai_chat_body(const Json& params, std::string_view model, Json messages,
                               bool stream, Json& body) {
    if (model.empty() || !messages.is_array() || messages.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    body = Json{{"model", model}, {"messages", std::move(messages)}, {"stream", stream}};
    copy_chat_option(body, params, "temperature", {"temperature"});
    copy_chat_option(body, params, "max_tokens", {"max_tokens", "maxTokens"});
    copy_chat_option(body, params, "top_p", {"top_p", "topP"});
    copy_chat_option(body, params, "top_k", {"top_k", "topK"});
    copy_chat_option(body, params, "frequency_penalty", {"frequency_penalty", "frequencyPenalty"});
    copy_chat_option(body, params, "presence_penalty", {"presence_penalty", "presencePenalty"});
    copy_chat_option(body, params, "stop", {"stop"});
    copy_chat_option(body, params, "seed", {"seed"});
    copy_chat_option(body, params, "logit_bias", {"logit_bias", "logitBias"});
    copy_chat_option(body, params, "logprobs", {"logprobs"});
    copy_chat_option(body, params, "top_logprobs", {"top_logprobs", "topLogprobs"});
    copy_chat_option(body, params, "n", {"n"});
    copy_chat_option(body, params, "user", {"user"});
    copy_chat_option(body, params, "tools", {"tools"});
    copy_chat_option(body, params, "tool_choice", {"tool_choice", "toolChoice"});
    copy_chat_option(body, params, "response_format", {"response_format", "responseFormat"});
    copy_chat_option(body, params, "parallel_tool_calls",
                     {"parallel_tool_calls", "parallelToolCalls"});
    return SAO_AI_EDITOR_OK;
}

Json registry_summary(const Json& registry, std::string_view kind) {
    return Json{{"kind", kind},
                {"items", registry.is_array() ? registry : Json::array()},
                {"total", registry.is_array() ? registry.size() : 0U}};
}

bool parse_scope_key(std::string_view key, std::string& scope, std::string& plugin_id) {
    plugin_id.clear();
    if (key == "system" || key == "workspace") {
        scope = key;
        return true;
    }
    constexpr std::string_view prefix = "plugin:";
    if (key.starts_with(prefix)) {
        plugin_id = std::string(key.substr(prefix.size()));
        if (valid_simple_id(plugin_id)) {
            scope = "plugin";
            return true;
        }
    }
    return false;
}

// Convert a WebviewPanelState into the JSON shape the Node-side extension
// shim + tests consume.  Kept here rather than on the struct because the
// registry header intentionally has no nlohmann::json dependency beyond
// the opaque `Json extras` bag.
Json panel_state_to_json(const WebviewPanelState& state) {
    bool active = state.visible;
    const auto active_value = state.options.extras.find("active");
    if (active_value != state.options.extras.end() && active_value->is_boolean())
        active = active_value->get<bool>();
    return Json{
        {"panelId", state.panel_id},
        {"viewType", state.view_type},
        {"title", state.title},
        {"active", active},
        {"visible", state.visible},
        {"viewColumn", state.options.view_column},
        {"disposed", state.disposed},
        {"htmlLength", static_cast<int64_t>(state.html.size())},
        {"createdMs", state.created_ms},
        {"lastRevealMs", state.last_reveal_ms},
        {"lastPostMs", state.last_post_ms},
        {"messageSeq", static_cast<int64_t>(state.message_seq)},
        {"options", Json{{"enableScripts", state.options.enable_scripts},
                         {"retainContextWhenHidden", state.options.retain_context_when_hidden},
                         {"viewColumn", state.options.view_column},
                         {"extras", state.options.extras}}},
        {"initialState", state.initial_state}};
}

} // namespace

NativeRuntime::NativeRuntime(RuntimeOptions options)
    : options_(std::move(options)), conversations_(scopes_), vt_bridge_(nullptr),
      tools_(scopes_, options.maximum_file_bytes, options.maximum_search_results),
      maximum_event_queue_(std::clamp(options.maximum_event_queue, 8U, 4096U)) {
    // Register the three built-in tool-result compressors + hand the registry
    // pointer to the tool registry.  Filters are shared_ptrs so the registry
    // can snapshot the vector without holding its mutex on every dispatch;
    // set_filter_registry takes a raw pointer because the tool registry lives
    // strictly within this runtime and never outlives it.  Order matters only
    // for the fired-id list — the LLM sees them in the order they compressed.
    filter_registry_.register_filter(make_list_files_folder_filter());
    filter_registry_.register_filter(make_search_files_collapse_filter());
    filter_registry_.register_filter(make_read_file_truncate_filter());
    tools_.set_filter_registry(&filter_registry_);
}

NativeRuntime::~NativeRuntime() {
    // Let the deferred built-in MCP registration reach a terminal state before
    // closing the client it writes into. The registration worker never outlives
    // `this`; joining here also serializes builtin_mcp_* field publication.
    if (builtin_mcp_worker_.joinable()) {
        builtin_mcp_worker_.join();
    }
    // Detach the shared extapi surface first so in-flight extapi publishes
    // stop resolving this runtime before member teardown starts.
    extapi::detach_runtime(this);
    stop_mcp_notifications(this, mcp_client_.get());
    if (mcp_client_ != nullptr)
        (void)sao_ai_editor_mcp_client_close(mcp_client_.get(), nullptr);
    int32_t first_panel_error = SAO_AI_EDITOR_OK;
    auto unregister_panel = [&](auto& panel) {
        if (panel == nullptr) {
            return;
        }
        const int32_t status = panel->unregister_from_runtime(webview_panels_);
        if (status != SAO_AI_EDITOR_OK && status != SAO_AI_EDITOR_ERR_NOT_FOUND &&
            first_panel_error == SAO_AI_EDITOR_OK) {
            first_panel_error = status;
        }
        if (status == SAO_AI_EDITOR_OK || status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
            panel.reset();
        }
    };
    std::vector<std::shared_ptr<WorkflowExecution>> workflows;
    {
        std::lock_guard<std::mutex> lock(workflow_mutex_);
        workflows.reserve(workflow_executions_.size());
        for (const auto& [id, execution] : workflow_executions_) {
            (void)id;
            workflows.push_back(execution);
        }
    }
    for (const auto& execution : workflows) {
        execution->request_cancel();
    }

    std::vector<std::shared_ptr<RunState>> runs;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        runs.reserve(runs_.size());
        for (const auto& [id, run] : runs_) {
            (void)id;
            runs.push_back(run);
            run->cancellation->cancel();
        }
    }

    for (const auto& execution : workflows) {
        execution->join();
    }
    for (const auto& run : runs) {
        if (run->worker.joinable()) {
            run->worker.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(workflow_mutex_);
        workflow_executions_.clear();
    }

    if (extension_host_ != nullptr) {
        const int32_t vt_command_status =
            sao::ai_editor::vt::unregister_vt_commands(*extension_host_);
        if (vt_command_status != SAO_AI_EDITOR_OK &&
            vt_command_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
            sao::ai_editor::vt::abandon_vt_commands(*extension_host_);
        }
    }
    if (vt_bridge_ != nullptr) {
        const int32_t vt_status = sao::ai_editor::vt::unregister_vt_tools(tools_, vt_bridge_);
        if (vt_status == SAO_AI_EDITOR_OK || vt_status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
            vt_bridge_.reset();
        } else {
            sao::ai_editor::vt::abandon_vt_tools(tools_);
        }
    }
    mcp_client_.reset();

    if (extension_host_ != nullptr) {
        (void)sao::ai_editor::kernel_map::unregister_kernel_map_commands(*extension_host_);
        sao::ai_editor::kernel_map::abandon_kernel_map_commands(*extension_host_);
        extension_host_.reset();
    }
    const auto kernel_map_bridge = sao::ai_editor::kernel_map::shared_bridge_handle();
    (void)sao::ai_editor::kernel_map::unregister_kernel_map_tools(tools_, kernel_map_bridge);
    sao::ai_editor::kernel_map::abandon_kernel_map_tools(tools_);

    if (kernel_map_bridge_owner_) {
        std::vector<uint64_t> residual_kernel_maps;
        kernel_map_teardown_status_ =
            sao::ai_editor::kernel_map::release_shared_bridge_owner(residual_kernel_maps);
        kernel_map_bridge_owner_ = false;
        if (kernel_map_teardown_status_ != SAO_AI_EDITOR_OK &&
            kernel_map_teardown_status_ != SAO_AI_EDITOR_ERR_NOT_INITIALIZED &&
            kernel_map_teardown_status_ != SAO_AI_EDITOR_ERR_NOT_FOUND) {
            emit("kernelMap.teardownFailed", Json{{"status", kernel_map_teardown_status_},
                                                  {"residualBases", residual_kernel_maps.size()}});
        }
    }

    set_webview_post_message_handler({});
    unregister_panel(mcp_management_panel_);
    unregister_panel(kernel_map_panel_);
    (void)first_panel_error;

    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        stopping_ = true;
    }
    event_ready_.notify_all();
}

void NativeRuntime::set_webview_post_message_handler(WebviewPostMessageHandler handler) {
    std::lock_guard<std::mutex> guard(webview_bridge_mutex_);
    webview_post_message_handler_ = std::move(handler);
}

std::optional<WebviewPanelState> NativeRuntime::active_webview_panel() const {
    return webview_panels_.active_panel();
}

int32_t NativeRuntime::initialize() {
    runtime_trace_stage(L"initialize.begin");
    auto cleanup_vt = [&]() -> int32_t {
        if (extension_host_ != nullptr) {
            const int32_t command_status =
                sao::ai_editor::vt::unregister_vt_commands(*extension_host_);
            if (command_status != SAO_AI_EDITOR_OK && command_status != SAO_AI_EDITOR_ERR_NOT_FOUND)
                return command_status;
        }
        if (vt_bridge_ != nullptr) {
            const int32_t tool_status = sao::ai_editor::vt::unregister_vt_tools(tools_, vt_bridge_);
            if (tool_status == SAO_AI_EDITOR_OK || tool_status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
                vt_bridge_.reset();
            } else {
                return tool_status;
            }
        }
        return SAO_AI_EDITOR_OK;
    };
    const int32_t vt_cleanup_status = cleanup_vt();
    if (vt_cleanup_status != SAO_AI_EDITOR_OK && vt_cleanup_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return vt_cleanup_status;
    }
    const auto kernel_map_bridge = sao::ai_editor::kernel_map::shared_bridge_handle();
    auto cleanup_kernel_map = [&] {
        int32_t first_error = SAO_AI_EDITOR_OK;
        auto unregister_panel = [&](auto& panel) {
            if (panel == nullptr) {
                return;
            }
            const int32_t status = panel->unregister_from_runtime(webview_panels_);
            if (status != SAO_AI_EDITOR_OK && status != SAO_AI_EDITOR_ERR_NOT_FOUND &&
                first_error == SAO_AI_EDITOR_OK) {
                first_error = status;
            }
            if (status == SAO_AI_EDITOR_OK || status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
                panel.reset();
            }
        };
        unregister_panel(mcp_management_panel_);
        unregister_panel(kernel_map_panel_);
        if (extension_host_ != nullptr) {
            const int32_t status =
                sao::ai_editor::kernel_map::unregister_kernel_map_commands(*extension_host_);
            if (status != SAO_AI_EDITOR_OK && status != SAO_AI_EDITOR_ERR_NOT_FOUND &&
                first_error == SAO_AI_EDITOR_OK) {
                first_error = status;
            }
            if (status == SAO_AI_EDITOR_OK || status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
                extension_host_.reset();
            }
        }
        const int32_t tools_status =
            sao::ai_editor::kernel_map::unregister_kernel_map_tools(tools_, kernel_map_bridge);
        if (tools_status != SAO_AI_EDITOR_OK && tools_status != SAO_AI_EDITOR_ERR_NOT_FOUND &&
            first_error == SAO_AI_EDITOR_OK) {
            first_error = tools_status;
        }
        return first_error;
    };
    const int32_t cleanup_status = cleanup_kernel_map();
    if (cleanup_status != SAO_AI_EDITOR_OK && cleanup_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return cleanup_status;
    }
    const int32_t status = scopes_.initialize(options_.workspace_root, options_.system_root,
                                              options_.plugin_roots_json);
    runtime_trace_stage(L"scopes.initialize", status);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    secrets_ = std::make_unique<SecretStore>(scopes_.secret_vault_path());
    SaoAiEditorMcpClient* mcp_raw = nullptr;
    runtime_trace_stage(L"mcp_client.create.begin");
    if (sao_ai_editor_mcp_client_create(&mcp_raw) != SAO_AI_EDITOR_OK) {
        runtime_trace_stage(L"mcp_client.create.failed", SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    runtime_trace_stage(L"mcp_client.create.done");
    mcp_client_.reset(mcp_raw);
    mcp_notification_gates[this] = std::make_shared<McpNotificationGate>();
    // Forward every MCP notification (tools/list_changed, prompts/list_changed,
    // resources/list_changed, resources/updated, notifications/message, ...)
    // straight into the runtime event queue so the UI can observe changes as
    // sao.event / mcp.notification without having to poll the MCP APIs.
    sao_ai_editor_mcp_client_set_notification_forwarder(
        mcp_client_.get(), this, &NativeRuntime::mcp_notification_trampoline);
    runtime_trace_stage(L"mcp_client.forwarder.done");
    runtime_trace_stage(L"mcp_client.builtin_register.defer");
    builtin_mcp_registration_status_.store(SAO_AI_EDITOR_ERR_BUSY,
                                           std::memory_order_release);
    try {
        builtin_mcp_worker_ = std::thread([this] {
            runtime_trace_stage(L"mcp_client.builtin_register.begin");
            const int32_t registration_status = register_builtin_mcp_server();
            builtin_mcp_registration_status_.store(registration_status,
                                                   std::memory_order_release);
            runtime_trace_stage(L"mcp_client.builtin_register.done", registration_status);
            emit("mcp.serverRegistered",
                 Json{{"name", "kernel_map"}, {"status", registration_status}});
        });
    } catch (...) {
        builtin_mcp_registration_status_.store(SAO_AI_EDITOR_ERR_OS_CALL_FAILED,
                                               std::memory_order_release);
        runtime_trace_stage(L"mcp_client.builtin_register.failed",
                            SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
    }
    runtime_trace_stage(L"plugin_contributions.begin");
    register_plugin_manifest_contributions();
    runtime_trace_stage(L"plugin_contributions.done");
    auth_flow_ = std::make_unique<AuthDeviceFlow>(secrets_.get());
    extension_host_ = std::make_unique<ExtensionHost>(*this);
    runtime_trace_stage(L"extension_host.created");
    // Bind this runtime's roots + pointer into the shared extapi surface so
    // both dispatch doors (Node extension host and the standalone shim C
    // ABI) resolve the same workspace, vault and registries.
    extapi::configure(options_.workspace_root, options_.system_root,
                      options_.plugin_roots_json);
    extapi::attach_runtime(this);
    runtime_trace_stage(L"extapi.attached");
    if (!kernel_map_bridge_owner_) {
        runtime_trace_stage(L"kernel_map.acquire.begin");
        if (!sao::ai_editor::kernel_map::acquire_shared_bridge_owner()) {
            runtime_trace_stage(L"kernel_map.acquire.failed", SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        kernel_map_bridge_owner_ = true;
        runtime_trace_stage(L"kernel_map.acquire.done");
    }
    // kernel_map wiring: surface the four native kernelMap.* tool descriptors
    // on tools/list and register the sao.kernelMap.* command handlers.  Both
    // registrations are idempotent (see kernel_map_tools.cpp /
    // kernel_map_commands.cpp) so re-initialising the runtime does not
    // duplicate entries.  The Bridge is a process-wide singleton; sharing
    // one instance between the tool + command paths keeps a single wire
    // channel serialising the operator's requests.
    int32_t registration_status =
        sao::ai_editor::kernel_map::register_kernel_map_tools(tools_, kernel_map_bridge);
    runtime_trace_stage(L"kernel_map.tools", registration_status);
    if (registration_status != SAO_AI_EDITOR_OK) {
        const int32_t rollback_status = cleanup_kernel_map();
        return rollback_status != SAO_AI_EDITOR_OK && rollback_status != SAO_AI_EDITOR_ERR_NOT_FOUND
                   ? rollback_status
                   : registration_status;
    }
    registration_status = sao::ai_editor::kernel_map::register_kernel_map_commands(
        *extension_host_, kernel_map_bridge);
    runtime_trace_stage(L"kernel_map.commands", registration_status);
    if (registration_status != SAO_AI_EDITOR_OK) {
        const int32_t rollback_status = cleanup_kernel_map();
        return rollback_status != SAO_AI_EDITOR_OK && rollback_status != SAO_AI_EDITOR_ERR_NOT_FOUND
                   ? rollback_status
                   : registration_status;
    }
    // Operator-facing kernel map dashboard.  Constructs the panel
    // provider and registers it in the webview panel registry so the
    // sidebar surfaces the built-in view.  Registration is idempotent;
    // when the on-disk assets are missing the provider falls back to a
    // built-in stub HTML that still exercises the message dispatch.
    kernel_map_panel_ = std::make_unique<KernelMapPanelProvider>();
    kernel_map_panel_->install_file_picker(&pick_kernel_driver_file);

    registration_status = kernel_map_panel_->register_with_runtime(
        webview_panels_, resolve_panel_assets(options_.system_root, "kernel_map_panel"));
    runtime_trace_stage(L"kernel_map.panel", registration_status);
    if (registration_status != SAO_AI_EDITOR_OK) {
        const int32_t rollback_status = cleanup_kernel_map();
        return rollback_status != SAO_AI_EDITOR_OK && rollback_status != SAO_AI_EDITOR_ERR_NOT_FOUND
                   ? rollback_status
                   : registration_status;
    }
    mcp_management_panel_ = std::make_unique<McpManagementPanelProvider>();
    mcp_management_panel_->install_snapshot_provider([this] { return mcp_management_snapshot(); });
    mcp_management_panel_->install_kernel_map_navigator([this] {
        WebviewPanelState state;
        const int32_t reveal_status =
            webview_panels_.reveal(std::string{kKernelMapPanelId}, 1, false, state);
        if (reveal_status == SAO_AI_EDITOR_OK) {
            emit("vscode.window.webviewPanel.revealed", panel_state_to_json(state));
            return true;
        }
        return false;
    });
    registration_status = mcp_management_panel_->register_with_runtime(
        webview_panels_, resolve_panel_assets(options_.system_root, "mcp_management_panel"));
    runtime_trace_stage(L"mcp_management.panel", registration_status);
    if (registration_status != SAO_AI_EDITOR_OK) {
        const int32_t rollback_status = cleanup_kernel_map();
        return rollback_status != SAO_AI_EDITOR_OK && rollback_status != SAO_AI_EDITOR_ERR_NOT_FOUND
                   ? rollback_status
                   : registration_status;
    }
    vt_bridge_ = std::make_shared<sao::ai_editor::vt::Bridge>();
    registration_status = sao::ai_editor::vt::register_vt_tools(tools_, vt_bridge_);
    runtime_trace_stage(L"vt.tools", registration_status);
    if (registration_status != SAO_AI_EDITOR_OK) {
        const int32_t vt_rollback_status = cleanup_vt();
        if (vt_rollback_status != SAO_AI_EDITOR_OK &&
            vt_rollback_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
            return vt_rollback_status;
        }
        const int32_t kernel_map_rollback_status = cleanup_kernel_map();
        return kernel_map_rollback_status != SAO_AI_EDITOR_OK &&
                       kernel_map_rollback_status != SAO_AI_EDITOR_ERR_NOT_FOUND
                   ? kernel_map_rollback_status
                   : registration_status;
    }
    registration_status = sao::ai_editor::vt::register_vt_commands(*extension_host_, vt_bridge_);
    runtime_trace_stage(L"vt.commands", registration_status);
    if (registration_status != SAO_AI_EDITOR_OK) {
        const int32_t vt_rollback_status = cleanup_vt();
        if (vt_rollback_status != SAO_AI_EDITOR_OK &&
            vt_rollback_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
            return vt_rollback_status;
        }
        const int32_t kernel_map_rollback_status = cleanup_kernel_map();
        return kernel_map_rollback_status != SAO_AI_EDITOR_OK &&
                       kernel_map_rollback_status != SAO_AI_EDITOR_ERR_NOT_FOUND
                   ? kernel_map_rollback_status
                   : registration_status;
    }
    runtime_trace_stage(L"initialize.end", SAO_AI_EDITOR_OK);
    return SAO_AI_EDITOR_OK;
}

void NativeRuntime::register_plugin_manifest_contributions() {
    if (mcp_client_ == nullptr) {
        return;
    }
    manifest_chat_providers_ = Json::array();
    plugin_mcp_diagnostics_.clear();
    plugin_mcp_registered_count_ = 0;
    Json roots = Json::object();
    if (!options_.plugin_roots_json.empty()) {
        try {
            roots = Json::parse(options_.plugin_roots_json, nullptr, false);
        } catch (...) {
            roots = Json::object();
        }
    }
    PluginContributions contributions;
    scan_plugin_contributions(roots, contributions);
    plugin_mcp_diagnostics_ = std::move(contributions.diagnostics);
    manifest_chat_providers_ = std::move(contributions.chat_providers);
    Json settings;
    const int32_t settings_status = [&] {
        std::lock_guard<std::mutex> lock(store_mutex_);
        return scopes_.load_merged_config(settings);
    }();
    if (settings_status != SAO_AI_EDITOR_OK) {
        if (plugin_mcp_diagnostics_.size() < kMaximumPluginContributionDiagnostics) {
            plugin_mcp_diagnostics_.push_back(
                "skipped plugin MCP autostart: settings unavailable (status " +
                std::to_string(settings_status) + ")");
        }
        return;
    }
    if (!plugin_mcp_autostart_enabled(settings.value("mcp", Json::object()))) {
        return;
    }
    plugin_mcp_registered_count_ = register_plugin_mcp_servers(
        mcp_client_.get(), contributions.mcp_servers, plugin_mcp_diagnostics_);
}

int32_t NativeRuntime::register_builtin_mcp_server() {
    if (mcp_client_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    if (!is_production_ai_editor_process()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const auto executable = current_executable_path();
    if (!executable.has_value()) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    const std::filesystem::path server_path =
        executable->parent_path() / L"SaoKernelMapMcpServer.exe";
    std::error_code error;
    if (!std::filesystem::is_regular_file(server_path, error) || error) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    std::string server_path_utf8;
    {
        std::lock_guard<std::mutex> guard(builtin_mcp_mutex_);
        builtin_mcp_server_path_ = wide_to_utf8(server_path.native());
        server_path_utf8 = builtin_mcp_server_path_;
    }
    const Json config{{"name", "kernel_map"},
                      {"transport", "stdio"},
                      {"command", server_path_utf8},
                      {"cwd", wide_to_utf8(server_path.parent_path().native())},
                      {"startupMs", 2000u}};
    const std::string serialized = dump_json(config);
    return sao_ai_editor_mcp_client_register(mcp_client_.get(), serialized.data(),
                                             static_cast<std::uint32_t>(serialized.size()));
}

Json NativeRuntime::mcp_management_snapshot() {
    Json servers;
    Json tools;
    Json prompts;
    Json resources;
    const int32_t servers_status = dispatch_mcp("mcp.list_servers", Json::object(), servers);
    const int32_t tools_status = dispatch_mcp("mcp.list_tools", Json::object(), tools);
    const int32_t prompts_status = dispatch_mcp("mcp.list_prompts", Json::object(), prompts);
    const int32_t resources_status = dispatch_mcp("mcp.list_resources", Json::object(), resources);
    const Json server_items =
        servers.is_object() ? servers.value("items", Json::array()) : Json::array();
    const Json tool_items = tools.is_object() ? tools.value("items", Json::array()) : Json::array();
    const Json prompt_items =
        prompts.is_object() ? prompts.value("items", Json::array()) : Json::array();
    const Json resource_items =
        resources.is_object() ? resources.value("items", Json::array()) : Json::array();
    const int32_t builtin_status = builtin_mcp_registration_status_.load();
    std::string builtin_path;
    {
        std::lock_guard<std::mutex> guard(builtin_mcp_mutex_);
        builtin_path = builtin_mcp_server_path_;
    }
    return Json{{"registration",
                 {{"name", "kernel_map"},
                  {"status", builtin_status},
                  {"path", builtin_path}}},
                {"plugin",
                 {{"mcp_registered", plugin_mcp_registered_count_},
                  {"chat_providers", manifest_chat_providers_},
                  {"diagnostics", plugin_mcp_diagnostics_}}},
                {"servers_status", servers_status},
                {"tools_status", tools_status},
                {"prompts_status", prompts_status},
                {"resources_status", resources_status},
                {"servers", server_items},
                {"tools", tool_items},
                {"prompts", prompt_items},
                {"resources", resource_items}};
}
void SAO_AI_EDITOR_CALL NativeRuntime::mcp_notification_trampoline(void* user,
                                                                   const char* json_utf8,
                                                                   uint32_t json_len) {
    if (user == nullptr || json_utf8 == nullptr || json_len == 0) {
        return;
    }
    try {
        auto* self = static_cast<NativeRuntime*>(user);
        const auto gate = get_mcp_gate(self);
        if (gate == nullptr)
            return;
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            if (!gate->accepting)
                return;
            ++gate->inflight;
        }
        struct GateRelease final {
            std::shared_ptr<McpNotificationGate> gate;
            ~GateRelease() {
                std::lock_guard<std::mutex> lock(gate->mutex);
                if (gate->inflight > 0)
                    --gate->inflight;
                if (!gate->accepting && gate->inflight == 0)
                    gate->ready.notify_all();
            }
        } release{gate};
        Json envelope = Json::parse(json_utf8, json_utf8 + json_len, nullptr, false);
        if (envelope.is_discarded() || !envelope.is_object()) {
            return;
        }
        const std::string server_name = envelope.value("server", std::string{});
        Json notification = envelope.value("notification", Json::object());
        std::string method = notification.value("method", std::string{});
        if (method.empty()) {
            return;
        }
        Json payload =
            redact_secret_fields(Json{{"server", server_name},
                                      {"method", std::move(method)},
                                      {"params", notification.value("params", Json::object())}});
        self->emit("mcp.notification", payload);
        static std::atomic<uint64_t> panel_notification_sequence{1};
        if (self->mcp_management_panel_ != nullptr &&
            self->mcp_management_panel_->is_registered()) {
            WebviewPanelState state;
            if (self->webview_panels_.note_post_message(std::string{kMcpManagementPanelId},
                                                        state) == SAO_AI_EDITOR_OK) {
                const uint64_t sequence =
                    panel_notification_sequence.fetch_add(1, std::memory_order_relaxed);
                Json page_message{{"status", "event"},
                                  {"cmd", "notification"},
                                  {"requestId", "mcp-notification-" + std::to_string(sequence)},
                                  {"payload", payload}};
                Json page_result;
                (void)self->dispatch_webview_message_to_page(
                    Json{{"panelId", kMcpManagementPanelId},
                         {"messageSeq", static_cast<int64_t>(state.message_seq)},
                         {"message", std::move(page_message)}},
                    page_result);
            }
        }
    } catch (...) {
        // Never propagate exceptions across the C API boundary.
    }
}

Json NativeRuntime::permission_policy(std::string_view mode) {
    if (!supported_mode(mode)) {
        return Json();
    }
    const bool ask = mode == "ask";
    const bool plan = mode == "plan";
    return Json{{"mode", mode},
                {"read", "allowed"},
                {"write", ask    ? "disabled"
                          : plan ? "confirm"
                                 : "allowed"},
                {"execute", ask    ? "disabled"
                            : plan ? "confirm"
                                   : "allowed"}};
}

void NativeRuntime::emit(std::string_view event_name, const Json& payload,
                         std::string_view run_id) {
    Json params{{"event", event_name}, {"payload", payload}};
    if (!run_id.empty()) {
        params["runId"] = run_id;
    }
    Json notification{{"jsonrpc", "2.0"},
                      {"method", "sao.event"},
                      {"params", std::move(params)},
                      {"sao", {{"protocolVersion", SAO_AI_EDITOR_PROTOCOL_VERSION}}}};
    std::string text = dump_json(notification);
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (stopping_) {
            return;
        }
        if (events_.size() >= maximum_event_queue_) {
            events_.pop_front();
            ++dropped_events_;
        }
        events_.push_back(std::move(text));
    }
    event_ready_.notify_one();
}

int32_t NativeRuntime::next_event(uint32_t timeout_ms, std::string& event_json) {
    std::unique_lock<std::mutex> lock(event_mutex_);
    const auto ready = [&] { return stopping_ || !events_.empty(); };
    if (timeout_ms == 0) {
        if (!ready()) {
            return SAO_AI_EDITOR_ERR_TIMEOUT;
        }
    } else if (!event_ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
        return SAO_AI_EDITOR_ERR_TIMEOUT;
    }
    if (events_.empty()) {
        return SAO_AI_EDITOR_ERR_CANCELLED;
    }
    event_json = std::move(events_.front());
    events_.pop_front();
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::dispatch(const Json& request, Json& response) {
    if (!request.is_object() || request.value("jsonrpc", "") != "2.0" ||
        !request.contains("method") || !request["method"].is_string()) {
        response = rpc_error(nullptr, -32600, "invalid JSON-RPC request");
        return SAO_AI_EDITOR_OK;
    }
    const Json id = request.value("id", Json(nullptr));
    const Json params = request.value("params", Json::object());
    if (!params.is_object()) {
        response = rpc_error(id, -32602, "params must be an object");
        return SAO_AI_EDITOR_OK;
    }
    Json result;
    const int32_t status = invoke(request["method"].get_ref<const std::string&>(), params, result);
    if (status == SAO_AI_EDITOR_ERR_NOT_FOUND &&
        request["method"].get_ref<const std::string&>().find('.') != std::string::npos) {
        response = rpc_error(id, -32601, "method or item not found", Json{{"status", status}});
    } else if (status != SAO_AI_EDITOR_OK) {
        const std::string message =
            result.is_object() && result.contains("reason") && result["reason"].is_string()
                ? result["reason"].get<std::string>()
                : status_message(status);
        response =
            rpc_error(id, rpc_code(status), message, Json{{"status", status}, {"details", result}});
    } else {
        response = rpc_result(id, std::move(result));
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::invoke(std::string_view method, const Json& params, Json& result) {
    if (method == "events.drain") {
        uint32_t limit = 0;
        const int32_t limit_status = parse_event_drain_limit(params, limit);
        if (limit_status != SAO_AI_EDITOR_OK) {
            result = Json{{"parameter", "limit"},
                          {"minimum", 1},
                          {"maximum", kMaximumEventDrainLimit},
                          {"default", kDefaultEventDrainLimit}};
            return limit_status;
        }

        Json drained = Json::array();
        size_t selected = 0;
        size_t selected_bytes = 0;
        std::lock_guard<std::mutex> lock(event_mutex_);
        for (auto event = events_.cbegin(); event != events_.cend() && selected < limit; ++event) {
            const size_t event_bytes = event->size();
            if (event_bytes > kMaximumEventDrainBytes) {
                if (selected == 0) {
                    result = Json{{"maximumBatchBytes", kMaximumEventDrainBytes},
                                  {"nextEventBytes", event_bytes}};
                    return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
                }
                break;
            }
            if (selected_bytes > kMaximumEventDrainBytes - event_bytes) {
                break;
            }
            Json notification = Json::parse(*event, nullptr, false);
            if (notification.is_discarded()) {
                result =
                    Json{{"message", "queued event is not valid JSON"}, {"eventIndex", selected}};
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
            drained.push_back(std::move(notification));
            selected_bytes += event_bytes;
            ++selected;
        }

        const size_t remaining = events_.size() - selected;
        const bool closed = stopping_ && remaining == 0;
        result = Json{{"events", std::move(drained)},
                      {"count", selected},
                      {"remaining", remaining},
                      {"dropped", dropped_events_},
                      {"hasMore", remaining != 0},
                      {"stopping", stopping_},
                      {"closed", closed},
                      {"limit", limit}};
        for (size_t index = 0; index < selected; ++index) {
            events_.pop_front();
        }
        dropped_events_ = 0;
        return SAO_AI_EDITOR_OK;
    }
    if (method.starts_with("sao.vt.")) {
        if (vt_bridge_ == nullptr) {
            result = Json{{"ok", false},
                          {"available", false},
                          {"statusCode", SAO_AI_EDITOR_ERR_NOT_INITIALIZED},
                          {"reason", "not_initialized"},
                          {"unknown", false},
                          {"partial", false}};
            return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        }
        return sao::ai_editor::vt::dispatch_vt_command(*vt_bridge_, method, params, result);
    }
    if (method == "runtime.initialize") {
        Json config;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            int32_t status = scopes_.load_merged_config(config);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        result = Json{{"nativeAbiVersion", SAO_AI_EDITOR_NATIVE_ABI_VERSION},
                      {"protocolVersion", SAO_AI_EDITOR_PROTOCOL_VERSION},
                      {"workspaceRoot", wide_to_utf8(scopes_.workspace_root().native())},
                      {"scopes", scopes_.describe_scopes()},
                      {"config", std::move(config)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "scopes.list") {
        result = scopes_.describe_scopes();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "settings.describe") {
        result = AiEditorSettings::describe(scopes_);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "settings.load") {
        if (secrets_ == nullptr) {
            return SAO_AI_EDITOR_ERR_CONFIG_MISSING;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return AiEditorSettings::load(scopes_, *secrets_, params, result);
    }
    if (method == "settings.save") {
        if (secrets_ == nullptr) {
            return SAO_AI_EDITOR_ERR_CONFIG_MISSING;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return AiEditorSettings::save(scopes_, *secrets_, params, result);
    }
    if (method == "config.load") {
        std::lock_guard<std::mutex> lock(store_mutex_);
        if (params.contains("scope")) {
            if (!params["scope"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            std::string scope;
            std::string plugin_id;
            if (!parse_scope_key(params["scope"].get<std::string>(), scope, plugin_id)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            return scopes_.load_scope_config(scope, plugin_id, result);
        }
        return scopes_.load_merged_config(result);
    }
    if (method == "config.save") {
        if (!params.contains("scope") || !params["scope"].is_string() ||
            !params.contains("config") || !params["config"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::string scope;
        std::string plugin_id;
        if (!parse_scope_key(params["scope"].get<std::string>(), scope, plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        const int32_t status = scopes_.save_scope_config(scope, plugin_id, params["config"]);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"scope", params["scope"]}};
        }
        return status;
    }
    if (method == "permission.get") {
        RuntimePolicySnapshot policy;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status = prepare_runtime_policy(scopes_, params, policy);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        result = permission_policy(policy.effective_mode);
        if (result.is_null()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        for (const std::string_view category : {"read", "write", "execute"}) {
            PermissionResolution resolution;
            const int32_t status =
                resolve_permission_layer(policy.settings_permissions, policy.settings_mode, {}, {},
                                         category, "settings", resolution);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            if (policy.request_permission_layer) {
                PermissionResolution request_resolution;
                const int32_t request_status =
                    resolve_permission_layer(policy.request_permissions, policy.request_mode, {},
                                             {}, category, "request", request_resolution);
                if (request_status != SAO_AI_EDITOR_OK) {
                    return request_status;
                }
                if (permission_restriction(request_resolution.permission) >
                    permission_restriction(resolution.permission)) {
                    resolution = std::move(request_resolution);
                }
            }
            result[std::string(category)] = resolution.permission;
        }
        result["settingsMode"] = policy.settings_mode;
        result["requestedMode"] = policy.request_mode;
        result["approval"] = policy.effective_approval;
        result["settingsApproval"] = policy.settings_approval;
        result["requestedApproval"] = policy.request_approval;
        result["overrides"] = reported_permission_overrides(policy);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "conversation.create") {
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.create(params.value("title", "Untitled"), params.value("model", ""),
                                     params.value("scope", "workspace"), result);
    }
    if (method == "conversation.append") {
        if (!params.contains("id") || !params["id"].is_string() || !params.contains("message")) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.append(params["id"].get<std::string>(), params["message"], result);
    }
    if (method == "conversation.get") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.get(params["id"].get<std::string>(), result);
    }
    if (method == "conversation.list") {
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.list(params.value("scope", "all"), params.value("limit", 100U),
                                   result);
    }
    if (method == "conversation.stats") {
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.stats(params.value("scope", "all"), result);
    }
    if (method == "conversation.search") {
        if (!params.contains("query") || !params["query"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.search(params["query"].get<std::string>(),
                                     params.value("scope", "all"), params.value("limit", 20U),
                                     result);
    }
    if (method == "conversation.delete") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.remove(params["id"].get<std::string>(), result);
    }
    if (method == "conversation.pin" || method == "conversation.unpin" ||
        method == "conversation.set_pinned") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Method-aware pinned resolution:
        //   * conversation.pin        → pinned defaults to true (payload may
        //                               still force pinned:false)
        //   * conversation.unpin      → always pinned:false; ignore payload
        //   * conversation.set_pinned → payload must supply pinned:bool
        bool pinned = false;
        if (method == "conversation.unpin") {
            pinned = false;
        } else if (method == "conversation.pin") {
            pinned = params.value("pinned", true);
        } else {
            if (!params.contains("pinned") || !params["pinned"].is_boolean()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            pinned = params["pinned"].get<bool>();
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.set_pinned(params["id"].get<std::string>(), pinned, result);
    }
    if (method == "conversation.tag" || method == "conversation.untag") {
        // Both endpoints funnel into set_tags(add, remove) so we can keep the
        // "add and subtract in one call" primitive on the store while still
        // exposing the two verbs the UI wants.  Payload requires id + tags[]
        // for either method; anything else falls through to INVALID_ARGUMENT.
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (!params.contains("tags") || !params["tags"].is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::string> incoming;
        incoming.reserve(params["tags"].size());
        for (const auto& tag : params["tags"]) {
            if (!tag.is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            incoming.push_back(tag.get<std::string>());
        }
        const std::vector<std::string> add_tags =
            method == "conversation.tag" ? incoming : std::vector<std::string>{};
        const std::vector<std::string> remove_tags =
            method == "conversation.untag" ? incoming : std::vector<std::string>{};
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.set_tags(params["id"].get<std::string>(), add_tags, remove_tags,
                                       result);
    }
    if (method == "conversation.find_by_tag") {
        if (!params.contains("tags") || !params["tags"].is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::string> query_tags;
        query_tags.reserve(params["tags"].size());
        for (const auto& tag : params["tags"]) {
            if (!tag.is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            query_tags.push_back(tag.get<std::string>());
        }
        const std::string scope = params.value("scope", std::string{"all"});
        const uint32_t limit = params.value("limit", 50U);
        const bool match_all = params.value("matchAll", false);
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.find_by_tag(query_tags, scope, limit, match_all, result);
    }
    if (method == "conversation.list_tags") {
        const std::string scope = params.value("scope", std::string{"all"});
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.list_tags_stats(scope, result);
    }
    if (method == "conversation.duplicate") {
        if (!params.contains("sourceId") || !params["sourceId"].is_string() ||
            params["sourceId"].get_ref<const std::string&>().empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (params.contains("title") && !params["title"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (params.contains("scope") && !params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string source_id = params["sourceId"].get<std::string>();
        const std::string title =
            params.contains("title") ? params["title"].get<std::string>() : std::string{};
        const bool has_scope = params.contains("scope");
        const std::string scope = has_scope ? params["scope"].get<std::string>() : std::string{};
        if (has_scope && scope != "workspace" && scope != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.duplicate(source_id, title, scope, result);
    }
    if (method == "conversation.branch") {
        if (!params.contains("sourceId") || !params["sourceId"].is_string() ||
            !params.contains("messageIndex") || !params["messageIndex"].is_number_integer()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string source_id = params["sourceId"].get<std::string>();
        const int64_t message_index = params["messageIndex"].get<int64_t>();
        const std::string title = params.value("title", std::string{});
        const std::string scope = params.value("scope", std::string{"workspace"});
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.branch(source_id, message_index, title, scope, result);
    }
    if (method == "conversation.merge") {
        if (!params.contains("sourceIds") || !params["sourceIds"].is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::string> source_ids;
        source_ids.reserve(params["sourceIds"].size());
        for (const auto& item : params["sourceIds"]) {
            if (!item.is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            source_ids.push_back(item.get<std::string>());
        }
        const std::string title = params.value("title", std::string{});
        const std::string scope = params.value("scope", std::string{"workspace"});
        // "explicit" (default) preserves the caller's sourceIds order;
        // "savedAt" sorts by each source's savedAt ascending before concat.
        const std::string order_by = params.value("orderBy", std::string{"explicit"});
        if (order_by != "explicit" && order_by != "savedAt") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const Json separator = params.contains("separator") ? params["separator"] : Json(nullptr);
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.merge(source_ids, title, scope, separator, order_by == "savedAt",
                                    result);
    }
    if (method == "conversation.split") {
        if (!params.contains("sourceId") || !params["sourceId"].is_string() ||
            !params.contains("messageIndex") || !params["messageIndex"].is_number_integer()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const int64_t message_index_signed = params["messageIndex"].get<int64_t>();
        if (message_index_signed < 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string source_id = params["sourceId"].get<std::string>();
        const std::string scope = params.value("scope", std::string{"workspace"});
        const bool keep_original = params.value("keepOriginal", false);
        // titles is optional; when present it must be a 2-element string
        // array [before, after].  Missing / partial entries fall back to
        // the store's default naming.
        std::string title_before;
        std::string title_after;
        if (params.contains("titles")) {
            if (!params["titles"].is_array() || params["titles"].size() > 2) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            const auto& titles = params["titles"];
            if (titles.size() >= 1 && titles[0].is_string()) {
                title_before = titles[0].get<std::string>();
            } else if (titles.size() >= 1 && !titles[0].is_null()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (titles.size() >= 2 && titles[1].is_string()) {
                title_after = titles[1].get<std::string>();
            } else if (titles.size() >= 2 && !titles[1].is_null()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.split(source_id, static_cast<size_t>(message_index_signed),
                                    title_before, title_after, scope, keep_original, result);
    }
    if (method == "conversation.compact") {
        // Fold the head of a long transcript into a single summary produced
        // by an LLM.  Params:
        //   id           - required conversation id.
        //   keepLast     - number of tail messages to keep intact (default 6).
        //   provider     - LLM provider config forwarded verbatim to
        //                  run_chat_sync (same shape as chat.run).
        //   model        - model id (defaults resolved by the provider).
        //   summaryPrompt (optional) - system message driving the summary;
        //                  a fixed default ships when omitted so callers can
        //                  cheaply "just compact this thing" without
        //                  configuring prompts.
        //   compactStrategy - "replace" (default) drops the early tail and
        //                     substitutes one system summary + keepLast
        //                     recent; "prepend" retains everything and only
        //                     glues the summary at the head.
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string conversation_id = params["id"].get<std::string>();
        // Accept both integer and floating-point keepLast so JS callers
        // whose JSON serialisers turn `6` into `6.0` still hit the happy
        // path.  Signed compare catches negatives before the size_t cast.
        int64_t keep_last_signed = 6;
        if (params.contains("keepLast")) {
            if (!params["keepLast"].is_number()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            keep_last_signed = params["keepLast"].get<int64_t>();
        }
        if (keep_last_signed <= 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const size_t keep_last = static_cast<size_t>(keep_last_signed);
        const std::string strategy = params.value("compactStrategy", std::string{"replace"});
        if (strategy != "replace" && strategy != "prepend") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Load a snapshot of the conversation up front.  We deliberately
        // release store_mutex_ before the LLM call because run_chat_sync
        // performs a blocking HTTP round-trip and holding the store lock
        // through it would starve every other conversation.* method.
        Json conversation;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t get_status = conversations_.get(conversation_id, conversation);
            if (get_status != SAO_AI_EDITOR_OK) {
                return get_status;
            }
        }
        if (!conversation.contains("messages") || !conversation["messages"].is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const Json& all_messages = conversation["messages"];
        const size_t original_count = all_messages.size();
        // Nothing to summarise - short-circuit the LLM entirely.  We still
        // route through compact() so the response shape is identical to a
        // "did work" call; the store's noop path emits summary:"" and the
        // savedAt is left untouched.
        if (original_count <= keep_last) {
            std::lock_guard<std::mutex> lock(store_mutex_);
            return conversations_.compact(conversation_id, keep_last, std::string_view{}, strategy,
                                          result);
        }
        const size_t drop_count = original_count - keep_last;
        // Roll the early messages into a single "transcript" string.  Using
        // one user turn (rather than replaying the historic roles) keeps
        // the summary prompt cheap and avoids accidentally letting the
        // model treat old assistant text as authoritative context.
        std::string transcript;
        for (size_t index = 0; index < drop_count; ++index) {
            const auto& message = all_messages[index];
            if (!message.is_object()) {
                continue;
            }
            const std::string role = message.value("role", std::string{"user"});
            std::string content;
            if (message.contains("content") && message["content"].is_string()) {
                content = message["content"].get<std::string>();
            } else if (message.contains("content")) {
                // Non-string content (arrays / objects for tool calls) -
                // dump as JSON so the model still sees something rather
                // than a silent empty line.
                content = message["content"].dump();
            }
            transcript += role;
            transcript += ": ";
            transcript += content;
            transcript += "\n";
        }
        const std::string default_summary_prompt =
            "Please summarize the following conversation history "
            "concisely. Preserve important facts, decisions, and open "
            "questions. Output only the summary paragraph, no preamble.";
        const std::string summary_prompt = params.value("summaryPrompt", default_summary_prompt);
        Json summary_messages = Json::array();
        summary_messages.push_back(Json{{"role", "system"}, {"content", summary_prompt}});
        summary_messages.push_back(Json{{"role", "user"}, {"content", transcript}});
        Json chat_params = Json::object();
        if (params.contains("provider")) {
            chat_params["provider"] = params["provider"];
        }
        if (params.contains("model") && params["model"].is_string()) {
            chat_params["model"] = params["model"];
        }
        chat_params["messages"] = std::move(summary_messages);
        // Forward the standard timeout/retry knobs so callers can crank
        // both down when they know the summary should be quick.
        for (const std::string_view field : {"temperature", "max_tokens", "retry"}) {
            const std::string key(field);
            if (params.contains(field)) {
                chat_params[key] = params[field];
            }
        }
        if (params.contains("timeoutMs")) {
            chat_params["timeoutMs"] = params["timeoutMs"];
        }
        const uint32_t timeout_ms = params.value("timeoutMs", 60'000U);
        std::string summary_text;
        const int32_t chat_status = run_chat_sync(chat_params, timeout_ms, summary_text);
        if (chat_status != SAO_AI_EDITOR_OK) {
            // Conversation on disk is untouched - the LLM call happened
            // *before* the store rewrite, so callers can safely retry.
            return chat_status;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.compact(conversation_id, keep_last, summary_text, strategy, result);
    }
    if (method == "conversation.export") {
        const bool has_id = params.contains("id");
        const bool has_scope = params.contains("scope");
        if (has_id == has_scope) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (has_id) {
            if (!params["id"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            Json conversation;
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status =
                conversations_.get(params["id"].get<std::string>(), conversation);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            result = Json{{"format", "sao-conversation/1"},
                          {"conversation", std::move(conversation)},
                          {"exportedAt", now}};
            // Envelope integrity: hash the payload minus the sha256 field
            // itself.  Import will strip and recompute, so the digest must
            // be stamped last so it covers every stable field above.
            result["sha256"] = sha256_envelope_hex(result);
            return SAO_AI_EDITOR_OK;
        }
        if (!params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        // conversations_.export_all() stamps its own sha256 — no post-hash
        // needed here.
        return conversations_.export_all(params["scope"].get<std::string>(), result);
    }
    if (method == "conversation.import") {
        if (!params.contains("payload") || !params["payload"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string target_scope = params.value("scope", std::string{"workspace"});
        if (target_scope != "workspace" && target_scope != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const bool overwrite = params.value("overwrite", false);
        const Json& payload = params["payload"];
        // Envelope integrity: if the payload carries a `sha256` field, verify
        // it against a recomputation over the payload with the field stripped.
        // Missing digest is permitted for backward compatibility with pre-
        // checksum exports (R4 shipped without it).
        if (payload.contains("sha256")) {
            if (!payload["sha256"].is_string())
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            const std::string claimed = payload["sha256"].get<std::string>();
            if (!valid_sha256_hex(claimed))
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            const std::string recomputed = sha256_envelope_hex(payload);
            if (recomputed.empty() || claimed != recomputed) {
                result = Json{{"message", "envelope sha256 mismatch"},
                              {"expected", claimed},
                              {"actual", recomputed}};
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
        }
        const std::string format = payload.value("format", std::string{});
        std::vector<Json> incoming;
        if (format == "sao-conversation/1") {
            if (!payload.contains("conversation") || !payload["conversation"].is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            incoming.push_back(payload["conversation"]);
        } else if (format == "sao-conversations/1") {
            if (!payload.contains("conversations") || !payload["conversations"].is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            for (const auto& item : payload["conversations"]) {
                if (!item.is_object()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                incoming.push_back(item);
            }
        } else {
            result = Json{{"message", "unknown export format"}, {"format", format}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (incoming.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        // Validate ids and detect conflicts up front so overwrite=false can
        // reject atomically without a partial import.
        Json conflicts = Json::array();
        for (const auto& conversation : incoming) {
            const std::string candidate_id = conversation.value("id", std::string{});
            if (candidate_id.empty()) {
                continue;
            }
            if (!valid_simple_id(candidate_id)) {
                result = Json{{"message", "invalid conversation id"}, {"id", candidate_id}};
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            Json existing_doc;
            const int32_t get_status = conversations_.get(candidate_id, existing_doc);
            if (get_status == SAO_AI_EDITOR_OK) {
                conflicts.push_back(candidate_id);
            }
        }
        if (!conflicts.empty() && !overwrite) {
            result = Json{{"imported", 0},
                          {"conflicts", std::move(conflicts)},
                          {"assignedIds", Json::array()}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json assigned_ids = Json::array();
        size_t imported = 0;
        for (const auto& conversation : incoming) {
            std::string assigned_id;
            const int32_t import_status = conversations_.import_conversation(
                conversation, target_scope, overwrite, assigned_id);
            if (import_status != SAO_AI_EDITOR_OK) {
                result = Json{{"imported", imported},
                              {"conflicts", std::move(conflicts)},
                              {"assignedIds", std::move(assigned_ids)}};
                return import_status;
            }
            assigned_ids.push_back(assigned_id);
            ++imported;
        }
        result = Json{{"imported", imported},
                      {"conflicts", std::move(conflicts)},
                      {"assignedIds", std::move(assigned_ids)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.export") {
        const bool has_id = params.contains("id");
        const bool has_scope = params.contains("scope");
        if (has_id == has_scope) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        if (has_id) {
            if (!params["id"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            WorkflowDefinition definition;
            if (!workflow_registry_.get(params["id"].get<std::string>(), definition)) {
                return SAO_AI_EDITOR_ERR_NOT_FOUND;
            }
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            result = Json{{"format", "sao-workflow/1"},
                          {"workflow", definition.to_json()},
                          {"exportedAt", now}};
            // Envelope integrity — see conversation.export for the invariant.
            result["sha256"] = sha256_envelope_hex(result);
            return SAO_AI_EDITOR_OK;
        }
        if (!params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // workflow_registry_.export_all() stamps its own sha256.
        return workflow_registry_.export_all(params["scope"].get<std::string>(), result);
    }
    if (method == "workflow.import") {
        if (!params.contains("payload") || !params["payload"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string scope_key = params.value("scope", std::string{"workspace"});
        std::string scope;
        std::string plugin_id;
        if (!parse_scope_key(scope_key, scope, plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const bool overwrite = params.value("overwrite", false);
        const Json& payload = params["payload"];
        // Envelope integrity guard — see conversation.import for the invariant.
        // Missing digest is tolerated for backward compatibility with R6-era
        // exports; a mismatched digest short-circuits with a protocol error
        // so tampered payloads cannot poison the registry.
        if (payload.contains("sha256")) {
            if (!payload["sha256"].is_string())
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            const std::string claimed = payload["sha256"].get<std::string>();
            if (!valid_sha256_hex(claimed))
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            const std::string recomputed = sha256_envelope_hex(payload);
            if (recomputed.empty() || claimed != recomputed) {
                result = Json{{"message", "envelope sha256 mismatch"},
                              {"expected", claimed},
                              {"actual", recomputed}};
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
        }
        const std::string format = payload.value("format", std::string{});
        std::vector<Json> incoming;
        if (format == "sao-workflow/1") {
            if (!payload.contains("workflow") || !payload["workflow"].is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            incoming.push_back(payload["workflow"]);
        } else if (format == "sao-workflows/1") {
            if (!payload.contains("workflows") || !payload["workflows"].is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            for (const auto& item : payload["workflows"]) {
                if (!item.is_object()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                incoming.push_back(item);
            }
        } else {
            result = Json{{"message", "unknown export format"}, {"format", format}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (incoming.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        workflow_registry_.reload(scopes_);
        // Two-pass conflict detection so overwrite=false rejects atomically
        // instead of half-importing a batch.  Built-ins are always fatal —
        // even with overwrite=true — because letting a user replace a
        // built-in id would either shadow it after reload or masquerade a
        // user workflow as compile-time.
        Json conflicts = Json::array();
        for (const auto& workflow : incoming) {
            const std::string candidate_id = workflow.value("id", std::string{});
            if (candidate_id.empty() || !valid_simple_id(candidate_id)) {
                result = Json{{"message", "invalid workflow id"}, {"id", candidate_id}};
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (workflow_registry_.is_builtin(candidate_id)) {
                result = Json{{"imported", 0},
                              {"conflicts", Json::array({candidate_id})},
                              {"assignedIds", Json::array()},
                              {"message", "cannot overwrite builtin"}};
                return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
            }
            WorkflowDefinition existing;
            if (workflow_registry_.get(candidate_id, existing) && !existing.builtin) {
                conflicts.push_back(candidate_id);
            }
        }
        if (!conflicts.empty() && !overwrite) {
            result = Json{{"imported", 0},
                          {"conflicts", std::move(conflicts)},
                          {"assignedIds", Json::array()}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json assigned_ids = Json::array();
        size_t imported = 0;
        for (const auto& workflow : incoming) {
            std::string assigned_id;
            const int32_t import_status = workflow_registry_.import_workflow(
                workflow, scope, plugin_id, overwrite, scopes_, assigned_id);
            if (import_status != SAO_AI_EDITOR_OK) {
                result = Json{{"imported", imported},
                              {"conflicts", std::move(conflicts)},
                              {"assignedIds", std::move(assigned_ids)}};
                return import_status;
            }
            assigned_ids.push_back(assigned_id);
            ++imported;
        }
        result = Json{{"imported", imported},
                      {"conflicts", std::move(conflicts)},
                      {"assignedIds", std::move(assigned_ids)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.list_executions") {
        const std::string scope_key = params.value("scope", std::string{"all"});
        if (scope_key != "all" && scope_key != "workspace" && scope_key != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        uint32_t limit = params.value("limit", 50U);
        limit = std::clamp<uint32_t>(limit, 1U, 500U);
        std::string workflow_filter;
        if (params.contains("workflowId")) {
            if (!params["workflowId"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            workflow_filter = params["workflowId"].get<std::string>();
        }
        // Collect from both scopes when scope=="all"; individual scope
        // requests only search the matching root.  Missing directories are
        // silently treated as empty by enumerate_history.
        std::vector<std::string_view> selected;
        if (scope_key == "all") {
            selected = {"workspace", "system"};
        } else {
            selected = {std::string_view(scope_key)};
        }
        std::vector<Json> combined;
        for (const auto scope_view : selected) {
            const std::filesystem::path root = workflow_history_root(scopes_, scope_view);
            std::vector<Json> summaries;
            const int32_t enumerate_status =
                WorkflowExecution::enumerate_history(root, workflow_filter, summaries);
            if (enumerate_status != SAO_AI_EDITOR_OK) {
                return enumerate_status;
            }
            for (auto& summary : summaries) {
                summary["scope"] = std::string(scope_view);
                combined.push_back(std::move(summary));
            }
        }
        std::sort(combined.begin(), combined.end(), [](const Json& left, const Json& right) {
            return left.value("completedAt", int64_t{0}) > right.value("completedAt", int64_t{0});
        });
        Json items = Json::array();
        for (size_t index = 0; index < combined.size() && index < static_cast<size_t>(limit);
             ++index) {
            items.push_back(std::move(combined[index]));
        }
        result = Json{{"items", std::move(items)}, {"total", combined.size()}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.get_execution") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string execution_id = params["id"].get<std::string>();
        if (execution_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Try both roots (workspace first, then system).  Callers can pass
        // an explicit scope to skip the second lookup, but the default
        // scans everywhere so the API mirrors conversation.get semantics.
        const std::string scope_key = params.value("scope", std::string{"all"});
        if (scope_key != "all" && scope_key != "workspace" && scope_key != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::filesystem::path> candidates;
        if (scope_key == "workspace" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "workspace"));
        }
        if (scope_key == "system" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "system"));
        }
        for (const auto& root : candidates) {
            Json record;
            const int32_t status =
                WorkflowExecution::load_history_record(root, execution_id, record);
            if (status == SAO_AI_EDITOR_OK) {
                result = std::move(record);
                return SAO_AI_EDITOR_OK;
            }
            if (status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                return status;
            }
        }
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (method == "workflow.delete_execution") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string execution_id = params["id"].get<std::string>();
        if (execution_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string scope_key = params.value("scope", std::string{"all"});
        if (scope_key != "all" && scope_key != "workspace" && scope_key != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::filesystem::path> candidates;
        if (scope_key == "workspace" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "workspace"));
        }
        if (scope_key == "system" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "system"));
        }
        bool deleted = false;
        for (const auto& root : candidates) {
            const int32_t status = WorkflowExecution::delete_history_record(root, execution_id);
            if (status == SAO_AI_EDITOR_OK) {
                deleted = true;
                continue;
            }
            if (status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                return status;
            }
        }
        if (!deleted) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        result = Json{{"ok", true}, {"id", execution_id}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.skip_step" || method == "workflow.replace_variable" ||
        method == "workflow.snapshot_variables") {
        // Runtime workflow-control surface — each op resolves the target
        // execution from workflow_executions_, then delegates to the
        // matching WorkflowExecution primitive.  A single validation
        // block handles the shared "executionId" arg so the individual
        // branches stay focussed on their unique params.  Lives here in
        // invoke() (not dispatch_workflow) because the "workflow." (no s)
        // methods route through the top-level dispatcher — the plural
        // "workflows." prefix is what feeds dispatch_workflow.
        if (!params.contains("executionId") || !params["executionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string execution_id = params["executionId"].get<std::string>();
        if (execution_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::shared_ptr<WorkflowExecution> execution;
        {
            std::lock_guard<std::mutex> guard(workflow_mutex_);
            const auto found = workflow_executions_.find(execution_id);
            if (found != workflow_executions_.end()) {
                execution = found->second;
            }
        }
        if (!execution) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (method == "workflow.skip_step") {
            const std::string reason = params.value("reason", std::string{});
            const int32_t skip_status = execution->request_skip(reason);
            if (skip_status != SAO_AI_EDITOR_OK) {
                return skip_status;
            }
            result = Json{{"ok", true}, {"executionId", execution_id}};
            if (!reason.empty()) {
                result["reason"] = reason;
            }
            return SAO_AI_EDITOR_OK;
        }
        if (method == "workflow.replace_variable") {
            if (!params.contains("name") || !params["name"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (!params.contains("value") || !params["value"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            const std::string name = params["name"].get<std::string>();
            const std::string value = params["value"].get<std::string>();
            const int32_t set_status = execution->set_variable(name, value);
            if (set_status != SAO_AI_EDITOR_OK) {
                return set_status;
            }
            result =
                Json{{"ok", true}, {"name", name}, {"value", value}, {"executionId", execution_id}};
            return SAO_AI_EDITOR_OK;
        }
        // workflow.snapshot_variables
        result =
            Json{{"executionId", execution_id}, {"variables", execution->variables_snapshot()}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.retry") {
        // Resume a previously-failed execution starting at the failed
        // step (or an explicit `fromStep` override).  Original variables
        // from the persisted record are preserved by default so downstream
        // steps see the successful outputs of earlier ones.
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string original_id = params["id"].get<std::string>();
        if (original_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string scope_key = params.value("scope", std::string{"all"});
        if (scope_key != "all" && scope_key != "workspace" && scope_key != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Locate the persisted record.  Mirror the workspace-first scan
        // pattern that workflow.get_execution uses so the two APIs stay
        // consistent for callers who don't pin a scope.
        std::vector<std::filesystem::path> candidates;
        if (scope_key == "workspace" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "workspace"));
        }
        if (scope_key == "system" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "system"));
        }
        Json record;
        bool found_record = false;
        for (const auto& root : candidates) {
            const int32_t load_status =
                WorkflowExecution::load_history_record(root, original_id, record);
            if (load_status == SAO_AI_EDITOR_OK) {
                found_record = true;
                break;
            }
            if (load_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                return load_status;
            }
        }
        if (!found_record) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        // Only failed executions are eligible — completed runs have no
        // work left, cancelled runs are intentionally aborted, and
        // running/pending records would race with an already-live worker.
        if (record.value("status", std::string{}) != "failed") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string workflow_id = record.value("workflowId", std::string{});
        if (workflow_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Refresh the registry snapshot the same way workflows.run does so
        // an out-of-band edit to the definition takes effect on retry.
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        WorkflowDefinition definition;
        if (!workflow_registry_.get(workflow_id, definition)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (definition.steps.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Determine the target step index.  Callers may override with
        // `fromStep`; otherwise scan stepResults for the first
        // status=="failed" entry, falling back to record.currentStep when
        // the persisted stepResults array is missing (older schemas).
        size_t from_step = definition.steps.size();
        if (params.contains("fromStep") && !params["fromStep"].is_null()) {
            if (!params["fromStep"].is_number_integer() &&
                !params["fromStep"].is_number_unsigned()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            const int64_t requested = params["fromStep"].get<int64_t>();
            if (requested < 0) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            from_step = static_cast<size_t>(requested);
        } else {
            bool found_failed = false;
            if (record.contains("stepResults") && record["stepResults"].is_array()) {
                for (const auto& step_result : record["stepResults"]) {
                    if (!step_result.is_object()) {
                        continue;
                    }
                    if (step_result.value("status", std::string{}) == "failed" &&
                        step_result.contains("stepIndex")) {
                        const int64_t idx = step_result["stepIndex"].get<int64_t>();
                        if (idx < 0) {
                            continue;
                        }
                        from_step = static_cast<size_t>(idx);
                        found_failed = true;
                        break;
                    }
                }
            }
            if (!found_failed) {
                const int64_t current = record.value("currentStep", int64_t{0});
                from_step = current < 0 ? 0 : static_cast<size_t>(current);
            }
        }
        if (from_step >= definition.steps.size()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Resolve the (possibly-new) provider before we build the
        // execution so an invalid provider payload doesn't leak a
        // half-registered execution into workflow_executions_.
        Json prepared_params;
        Json provider;
        std::string api_key;
        const int32_t provider_status =
            resolve_provider(params, prepared_params, provider, api_key);
        if (provider_status != SAO_AI_EDITOR_OK) {
            return provider_status;
        }
        if (!api_key.empty()) {
            provider["apiKey"] = api_key;
        }
        const std::string model =
            prepared_params.value("model", provider.value("model", std::string{}));
        ProviderRoute route = normalise_provider(provider, model);
        if (model.empty() || route.endpoint.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        provider["endpoint"] = route.endpoint;
        provider["model"] = model;
        provider["type"] = route.type;
        const uint32_t chat_timeout = prepared_params.value("timeoutMs", 60'000U);
        const int32_t approval_status = apply_workflow_approval_policy(prepared_params, definition);
        if (approval_status != SAO_AI_EDITOR_OK) {
            return approval_status;
        }
        const bool keep_variables = params.value("keepVariables", true);
        // Seed the initial input from the persisted record so
        // interpolations like {{input}} still resolve when
        // keepVariables=false.  The ctor then re-derives variables_
        // from this payload; keepVariables=true overrides variables_
        // wholesale after construction via seed_retry_state.
        Json initial_input = Json::object();
        if (record.contains("variables") && record["variables"].is_object() &&
            record["variables"].contains("input")) {
            initial_input["input"] = record["variables"]["input"];
        }
        std::unordered_map<std::string, std::string> preserved;
        if (keep_variables && record.contains("variables") && record["variables"].is_object()) {
            for (const auto& [key, value] : record["variables"].items()) {
                if (value.is_string()) {
                    preserved[key] = value.get<std::string>();
                } else {
                    preserved[key] = value.dump();
                }
            }
        }
        static std::atomic<uint64_t> retry_execution_counter{0};
        const std::string new_id =
            "wf-" + std::to_string(GetTickCount64()) + "-retry-" +
            std::to_string(retry_execution_counter.fetch_add(1, std::memory_order_relaxed));
        auto execution = std::make_shared<WorkflowExecution>(new_id, std::move(definition),
                                                             std::move(initial_input));
        execution->seed_retry_state(from_step, std::move(preserved), original_id);
        {
            std::lock_guard<std::mutex> guard(workflow_mutex_);
            workflow_executions_[execution->id()] = execution;
        }
        const std::filesystem::path history_dir = workflow_history_root(scopes_, "workspace");
        execution->start(*this, std::move(provider), model, chat_timeout, history_dir);
        result = Json{{"executionId", execution->id()},
                      {"retryOf", original_id},
                      {"fromStep", static_cast<int64_t>(from_step)},
                      {"status", "running"}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.dry_run") {
        // Preview each step's interpolated prompt without hitting the LLM.
        // Reuses WorkflowExecution::interpolate_with so the substitution
        // rules stay 1:1 with a real workflows.run — callers can rely on
        // this to eyeball {{var}} bindings, group batching, and
        // confirmation gates before spending tokens.
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        WorkflowDefinition definition;
        if (!workflow_registry_.get(params["id"].get<std::string>(), definition)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        // Seed variables the same way WorkflowExecution's ctor does — start
        // from `input` (falling back to inputs[input]), fold every key
        // from `inputs`, then overlay `simulateOutputs` last so the caller
        // can pretend earlier steps already ran.  Non-string values are
        // JSON-encoded to match ctor semantics.
        std::unordered_map<std::string, std::string> variables;
        auto absorb_object = [&variables](const Json& source) {
            if (!source.is_object()) {
                return;
            }
            for (const auto& [key, value] : source.items()) {
                if (value.is_string()) {
                    variables[key] = value.get<std::string>();
                } else {
                    variables[key] = value.dump();
                }
            }
        };
        if (params.contains("input")) {
            const Json& input = params["input"];
            if (input.is_string()) {
                variables["input"] = input.get<std::string>();
            } else if (input.is_object()) {
                absorb_object(input);
            } else if (!input.is_null()) {
                variables["input"] = input.dump();
            }
        }
        if (params.contains("inputs")) {
            absorb_object(params["inputs"]);
        }
        if (variables.find("input") == variables.end()) {
            variables["input"] = "";
        }
        if (params.contains("simulateOutputs")) {
            absorb_object(params["simulateOutputs"]);
        }
        // Track the initial variable set separately so the response can
        // list "estimatedVariables" in the order they'd appear during a
        // real run: initial seed keys first, then each step's output_var
        // as it fires.
        std::vector<std::string> estimated_variables;
        std::unordered_map<std::string, size_t> estimated_index;
        auto record_var = [&](const std::string& name) {
            if (name.empty()) {
                return;
            }
            if (estimated_index.find(name) != estimated_index.end()) {
                return;
            }
            estimated_index.emplace(name, estimated_variables.size());
            estimated_variables.push_back(name);
        };
        // Seed keys are recorded in a stable order — `input` always
        // first, then everything else sorted for determinism.
        record_var("input");
        std::vector<std::string> seed_keys;
        for (const auto& [key, value] : variables) {
            (void)value;
            if (key != "input") {
                seed_keys.push_back(key);
            }
        }
        std::sort(seed_keys.begin(), seed_keys.end());
        for (const auto& key : seed_keys) {
            record_var(key);
        }
        // Walk the definition once, collecting per-step preview info and
        // sliding a window across contiguous same-group runs to identify
        // parallel batches.
        const auto& steps = definition.steps;
        Json preview = Json::array();
        Json groups = Json::array();
        std::vector<size_t> batch_end_of(steps.size(), 0);
        std::vector<size_t> batch_start_of(steps.size(), 0);
        for (size_t index = 0; index < steps.size();) {
            size_t batch_end = index + 1;
            const std::string& group_id = steps[index].group;
            if (!group_id.empty()) {
                while (batch_end < steps.size() && steps[batch_end].group == group_id) {
                    ++batch_end;
                }
            }
            const size_t batch_size = batch_end - index;
            const bool parallel = batch_size > 1;
            groups.push_back(Json{{"groupId", group_id},
                                  {"startStep", static_cast<int64_t>(index)},
                                  {"endStep", static_cast<int64_t>(batch_end - 1)},
                                  {"parallel", parallel}});
            for (size_t k = 0; k < batch_size; ++k) {
                batch_start_of[index + k] = index;
                batch_end_of[index + k] = batch_end;
            }
            index = batch_end;
        }
        // Second pass: render each step against the current variables map,
        // then commit a synthesised output_var so subsequent steps see the
        // placeholder in {{var}} interpolations.  Parallel batches share
        // one snapshot for rendering (matching run_loop's pre-batch
        // snapshot semantics), then commit together at the batch boundary
        // — otherwise a group's later steps would see earlier steps' fake
        // output which never happens at runtime.
        for (size_t index = 0; index < steps.size();) {
            const size_t batch_start = index;
            const size_t batch_end = batch_end_of[index];
            const size_t batch_size = batch_end - batch_start;
            const std::string& group_id = steps[batch_start].group;
            const std::unordered_map<std::string, std::string> snapshot = variables;
            for (size_t k = 0; k < batch_size; ++k) {
                const size_t step_index = batch_start + k;
                const WorkflowStep& step = steps[step_index];
                const std::string rendered =
                    WorkflowExecution::interpolate_with(step.prompt, snapshot);
                Json will_parallel = Json::array();
                for (size_t j = 0; j < batch_size; ++j) {
                    if (batch_start + j == step_index) {
                        continue;
                    }
                    will_parallel.push_back(static_cast<int64_t>(batch_start + j));
                }
                preview.push_back(Json{{"stepIndex", static_cast<int64_t>(step_index)},
                                       {"label", step.label},
                                       {"agent", step.agent},
                                       {"outputVar", step.output_var},
                                       {"group", group_id},
                                       {"requiresConfirmation", step.requires_confirmation},
                                       {"renderedPrompt", rendered},
                                       {"willBeParallelWith", std::move(will_parallel)}});
            }
            // Commit synthesised outputs after the whole batch renders so
            // the next batch's snapshot picks them up.  simulateOutputs
            // wins over the placeholder — the caller's provided value
            // remains authoritative across the whole preview.
            for (size_t k = 0; k < batch_size; ++k) {
                const WorkflowStep& step = steps[batch_start + k];
                if (step.output_var.empty()) {
                    continue;
                }
                record_var(step.output_var);
                if (variables.find(step.output_var) != variables.end()) {
                    // A caller-provided simulateOutputs (or a matching
                    // inputs key) trumps the synthetic placeholder.  Keep
                    // whatever's already there so downstream steps see
                    // that exact value.
                    continue;
                }
                std::string placeholder = "<simulated: ";
                placeholder += step.label.empty() ? step.output_var : step.label;
                placeholder += ">";
                variables[step.output_var] = std::move(placeholder);
            }
            index = batch_end;
        }
        Json estimated = Json::array();
        for (const auto& name : estimated_variables) {
            estimated.push_back(name);
        }
        result = Json{{"workflowId", definition.id},
                      {"workflowName", definition.name},
                      {"totalSteps", static_cast<int64_t>(steps.size())},
                      {"groups", std::move(groups)},
                      {"preview", std::move(preview)},
                      {"estimatedVariables", std::move(estimated)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.list" || method == "workflows.list" || method == "providers.list") {
        const std::string kind(method.substr(0, method.find('.')));
        Json registry;
        std::lock_guard<std::mutex> lock(store_mutex_);
        const int32_t status = scopes_.load_registry(kind, Json::array(), registry);
        if (status == SAO_AI_EDITOR_OK) {
            result = registry_summary(registry, kind);
        }
        return status;
    }
    if (method == "agents.save" || method == "workflows.save") {
        const std::string kind(method.substr(0, method.find('.')));
        if (!params.contains("scope") || !params["scope"].is_string() || !params.contains("item") ||
            !params["item"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string item_id = params["item"].value("id", "");
        std::string scope;
        std::string plugin_id;
        if (!valid_simple_id(item_id) ||
            !parse_scope_key(params["scope"].get<std::string>(), scope, plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        const int32_t status =
            scopes_.save_registry_item(kind, scope, plugin_id, item_id, params["item"]);
        if (status == SAO_AI_EDITOR_OK) {
            result = params["item"];
            result["scope"] = params["scope"];
        }
        return status;
    }
    if (method == "providers.configure") {
        if (!params.contains("scope") || !params["scope"].is_string() ||
            !params.contains("provider") || !params["provider"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json provider = params["provider"];
        const std::string id = provider.value("id", "");
        if (!valid_simple_id(id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::string scope;
        std::string plugin_id;
        if (!parse_scope_key(params["scope"].get<std::string>(), scope, plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }

        const bool changes_secret = provider.contains("apiKey");
        std::string secret_value;
        if (changes_secret) {
            if (!provider["apiKey"].is_string() || secrets_ == nullptr) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            secret_value = provider["apiKey"].get<std::string>();
            provider.erase("apiKey");
        }

        std::lock_guard<std::mutex> lock(store_mutex_);
        if (!scope_exists(scopes_, scope, plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string secret_key = "provider/" + id + "/apiKey";
        SecretSnapshot previous_secret;
        if (changes_secret) {
            const int32_t snapshot_status = snapshot_secret(*secrets_, secret_key, previous_secret);
            if (snapshot_status != SAO_AI_EDITOR_OK) {
                return snapshot_status;
            }
            const int32_t secret_status = write_secret(*secrets_, secret_key, secret_value);
            if (secret_status != SAO_AI_EDITOR_OK) {
                return secret_status;
            }
        }

        const int32_t status =
            scopes_.save_registry_item("providers", scope, plugin_id, id, provider);
        if (status != SAO_AI_EDITOR_OK) {
            if (!changes_secret) {
                return status;
            }
            const int32_t rollback_status = restore_secret(*secrets_, secret_key, previous_secret);
            result = Json{{"operation", "providers.configure"},
                          {"registryStatus", status},
                          {"secretRollbackStatus", rollback_status},
                          {"secretRestored", rollback_status == SAO_AI_EDITOR_OK}};
            return rollback_status == SAO_AI_EDITOR_OK ? status : SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        result = provider;
        result["scope"] = params["scope"];
        return SAO_AI_EDITOR_OK;
    }
    if (method == "models.list") {
        Json prepared_params;
        Json provider;
        std::string secret;
        const int32_t status = resolve_provider(params, prepared_params, provider, secret);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        Json models = provider.value("models", Json::array());
        if (!models.is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        result =
            Json{{"providerId", provider.value("id", "inline")}, {"models", std::move(models)}};
        result["total"] = result["models"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools.list") {
        RuntimePolicySnapshot policy;
        Json tools;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status = prepare_runtime_policy(scopes_, params, policy);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            tools = tools_.describe(policy.effective_mode);
        }
        const int32_t permission_status = apply_tool_permissions(policy, tools);
        if (permission_status != SAO_AI_EDITOR_OK) {
            return permission_status;
        }
        result = Json{{"mode", policy.effective_mode},
                      {"settingsMode", policy.settings_mode},
                      {"requestedMode", policy.request_mode},
                      {"approval", policy.effective_approval},
                      {"overrides", reported_permission_overrides(policy)},
                      {"tools", std::move(tools)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools.call") {
        RuntimePolicySnapshot policy;
        Json tool_descriptors;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status = prepare_runtime_policy(scopes_, params, policy);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            tool_descriptors = tools_.describe(policy.effective_mode);
        }
        if (!params.contains("name") || !params["name"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const Json arguments = params.value("arguments", Json::object());
        const std::string requested_name = params["name"].get<std::string>();
        // Resolve aliases *before* firing hooks so the audit payload sees the
        // canonical tool name (matches the value passed to tool_filter).  The
        // descriptor and policy are snapshots, so filesystem I/O below never
        // runs while store_mutex_ is held.
        const std::string resolved_name = tools_.resolve_alias(requested_name);
        const Json* descriptor = find_tool_descriptor(tool_descriptors, requested_name);
        if (descriptor == nullptr && resolved_name != requested_name) {
            descriptor = find_tool_descriptor(tool_descriptors, resolved_name);
        }
        ToolExecutionPlan execution;
        const int32_t policy_status = prepare_tool_execution(policy, requested_name, resolved_name,
                                                             descriptor, arguments, execution);
        if (policy_status != SAO_AI_EDITOR_OK) {
            if (policy_status == SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED) {
                result = Json{{"confirmationRequired", true},
                              {"tool", requested_name},
                              {"permissionSource", execution.permission.source},
                              {"permissionCategory", execution.category}};
            }
            const auto before_hooks = tools_.snapshot_hooks("before", resolved_name);
            for (const auto& hook : before_hooks) {
                Json payload{{"hookId", hook.id},
                             {"phase", "before"},
                             {"tool", resolved_name},
                             {"arguments", arguments}};
                if (resolved_name != requested_name) {
                    payload["requestedTool"] = requested_name;
                }
                emit(hook.emit_event, payload);
            }
            tool_monitor_.record_invocation(resolved_name);
            tool_monitor_.record_error(resolved_name);
            const auto error_hooks = tools_.snapshot_hooks("error", resolved_name);
            for (const auto& hook : error_hooks) {
                Json payload{{"hookId", hook.id},       {"phase", "error"},
                             {"tool", resolved_name},   {"arguments", arguments},
                             {"status", policy_status}, {"durationMs", 0}};
                if (resolved_name != requested_name) {
                    payload["requestedTool"] = requested_name;
                }
                if (policy_status == SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED) {
                    payload["errorResult"] = result;
                }
                emit(hook.emit_event, payload);
            }
            return policy_status;
        }
        // Before-phase hooks fire even when the tool ends up returning
        // NOT_FOUND — the payload includes the tool name that failed to
        // dispatch, which is exactly what an audit sink wants to record.
        const auto before_hooks = tools_.snapshot_hooks("before", resolved_name);
        for (const auto& hook : before_hooks) {
            Json payload{{"hookId", hook.id},
                         {"phase", "before"},
                         {"tool", resolved_name},
                         {"arguments", execution.arguments}};
            if (resolved_name != requested_name) {
                payload["requestedTool"] = requested_name;
            }
            emit(hook.emit_event, payload);
        }
        const auto call_start = std::chrono::steady_clock::now();
        // Telemetry: every dispatched tools.call counts as an invocation
        // regardless of cache outcome — matches VSCode's outputMonitor
        // where the "asked for it" event is what the LLM proxy records.
        // record_result() below fires on both hit + miss.
        tool_monitor_.record_invocation(resolved_name);
        // Session-memory dedup: mutation-aware invalidation runs first, then
        // read-only calls try the cache and short-circuit on hit.  observe()
        // is a no-op for read-only tools; lookup() returns nullopt for
        // uncached tools (mutations, custom).  See tool_result_cache.h.
        tool_cache_.observe(resolved_name, execution.arguments);
        if (auto hit = tool_cache_.lookup(resolved_name, execution.arguments)) {
            result = hit->result;
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count();
            const int64_t age_ms = now_ms - hit->timestamp_ms;
            if (result.is_object()) {
                result["cacheHit"] = true;
                result["cacheAgeMs"] = age_ms;
            }
            Json hit_payload{{"tool", resolved_name},
                             {"arguments", execution.arguments},
                             {"cacheAgeMs", age_ms}};
            if (resolved_name != requested_name) {
                hit_payload["requestedTool"] = requested_name;
            }
            emit("tools.cache.hit", hit_payload);
            // Telemetry (cache-hit path): duration is 0 by contract so the
            // monitor's avg_duration_ms excludes it — cache-hit latency is
            // essentially "one hash lookup" and folding it into the mean
            // would hide the true cost of the miss path.
            tool_monitor_.record_result(resolved_name, result, /*duration_ms=*/0,
                                        /*cache_hit=*/true);
            // Cache hits still fire the after-phase hook so audit sinks see a
            // canonical (before, after) pair even for served-from-memory calls.
            const auto after_hooks = tools_.snapshot_hooks("after", resolved_name);
            for (const auto& hook : after_hooks) {
                Json payload{{"hookId", hook.id},     {"phase", "after"},
                             {"tool", resolved_name}, {"arguments", execution.arguments},
                             {"result", result},      {"durationMs", 0},
                             {"cacheHit", true}};
                if (resolved_name != requested_name) {
                    payload["requestedTool"] = requested_name;
                }
                emit(hook.emit_event, payload);
            }
            return SAO_AI_EDITOR_OK;
        }
        const int32_t status =
            tools_.execute(execution.mode, requested_name, execution.arguments, result);
        const auto call_end = std::chrono::steady_clock::now();
        const auto duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(call_end - call_start).count();
        // Record successful executions into the session-memory cache.  We
        // deliberately skip failed calls (permission denied, boundary
        // violation, schema failure) so retries do not return the error
        // payload as if it were the ground truth.
        if (status == SAO_AI_EDITOR_OK) {
            tool_cache_.record(resolved_name, execution.arguments, result);
            tool_monitor_.record_result(resolved_name, result, duration_ms,
                                        /*cache_hit=*/false);
        } else {
            // Error-path telemetry: bump error_count only.  We deliberately
            // skip record_result here so failed calls do not contaminate the
            // avg_duration_ms + total_bytes_returned aggregates.
            tool_monitor_.record_error(resolved_name);
        }
        // After / error hooks reflect the outcome.  We treat every non-OK
        // status as an error phase so validation failures + permission
        // denials also surface — audits typically care about *any* non-happy
        // path.  The result payload is included for after; for error we
        // include the numeric status code the JSON-RPC caller would see so
        // downstream sinks do not need to keep a parallel status table.
        if (status == SAO_AI_EDITOR_OK) {
            const auto after_hooks = tools_.snapshot_hooks("after", resolved_name);
            for (const auto& hook : after_hooks) {
                Json payload{{"hookId", hook.id},     {"phase", "after"},
                             {"tool", resolved_name}, {"arguments", execution.arguments},
                             {"result", result},      {"durationMs", duration_ms}};
                if (resolved_name != requested_name) {
                    payload["requestedTool"] = requested_name;
                }
                emit(hook.emit_event, payload);
            }
        } else {
            const auto error_hooks = tools_.snapshot_hooks("error", resolved_name);
            for (const auto& hook : error_hooks) {
                Json payload{{"hookId", hook.id},     {"phase", "error"},
                             {"tool", resolved_name}, {"arguments", execution.arguments},
                             {"status", status},      {"durationMs", duration_ms}};
                if (resolved_name != requested_name) {
                    payload["requestedTool"] = requested_name;
                }
                if (!result.is_null()) {
                    payload["errorResult"] = result;
                }
                emit(hook.emit_event, payload);
            }
        }
        return status;
    }
    if (method == "tools.register") {
        if (!params.contains("name") || !params["name"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // description is optional, parameters is optional (defaults to a
        // {type:"object", properties:{}} schema inside the registry).
        const std::string description = params.value("description", std::string{});
        const Json parameters = params.value("parameters", Json(nullptr));
        // Custom tools are read-only unless the caller opts in — matches
        // built-in defaults (readFile/listFiles/searchFiles are read-only).
        const bool read_only = params.value("readOnly", true);
        const std::string name = params["name"].get<std::string>();
        const int32_t status = tools_.register_custom(name, description, parameters, read_only);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"name", name}};
        }
        return status;
    }
    if (method == "tools.unregister") {
        if (!params.contains("name") || !params["name"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string name = params["name"].get<std::string>();
        const int32_t status = tools_.unregister_custom(name);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"name", name}};
        }
        return status;
    }
    if (method == "tools.register_alias") {
        // alias + target are both required strings.  Anything else is
        // INVALID_ARGUMENT — the registry itself also validates but rejecting
        // here keeps the JSON-RPC error boundary crisp.
        if (!params.contains("alias") || !params["alias"].is_string() ||
            !params.contains("target") || !params["target"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string alias = params["alias"].get<std::string>();
        const std::string target = params["target"].get<std::string>();
        const int32_t status = tools_.register_alias(alias, target);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"alias", alias}, {"target", target}};
        }
        return status;
    }
    if (method == "tools.unregister_alias") {
        if (!params.contains("alias") || !params["alias"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string alias = params["alias"].get<std::string>();
        const int32_t status = tools_.unregister_alias(alias);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"alias", alias}};
        }
        return status;
    }
    if (method == "tools.register_hook") {
        // id + phase + emitEvent are required.  `toolFilter` is optional — null
        // or missing means "match every tool".  When present it must be an
        // array of strings (mixed / non-string entries are rejected up-front so
        // downstream matching does not need to filter them out per call).
        if (!params.contains("id") || !params["id"].is_string() || !params.contains("phase") ||
            !params["phase"].is_string() || !params.contains("emitEvent") ||
            !params["emitEvent"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::string> filter;
        if (params.contains("toolFilter") && !params["toolFilter"].is_null()) {
            const auto& raw = params["toolFilter"];
            if (!raw.is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            filter.reserve(raw.size());
            for (const auto& entry : raw) {
                if (!entry.is_string()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                filter.push_back(entry.get<std::string>());
            }
        }
        const std::string id = params["id"].get<std::string>();
        const std::string phase = params["phase"].get<std::string>();
        const std::string emit_event = params["emitEvent"].get<std::string>();
        const int32_t status = tools_.register_hook(id, phase, filter, emit_event);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"id", id}, {"phase", phase}, {"emitEvent", emit_event}};
        }
        return status;
    }
    if (method == "tools.unregister_hook") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string id = params["id"].get<std::string>();
        const int32_t status = tools_.unregister_hook(id);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"id", id}};
        }
        return status;
    }
    if (method == "tools.cache_stats") {
        // Session-memory cache introspection — used by the debug console
        // + tests to confirm hit/miss/invalidation accounting.
        result = tool_cache_.stats();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools.cache_clear") {
        // Wipe every cached entry + zero the counters.  No parameters — the
        // cache is per-runtime so scoping is implicit.  We deliberately do
        // NOT touch tool_monitor_ here — the telemetry ledger is a long-run
        // view of tool usage that shouldn't reset just because the dedup
        // cache was flushed.  Use `tools.telemetry_clear` for that.
        tool_cache_.clear();
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools.telemetry_stats") {
        // Per-tool execution counters (invocations, bytes returned, cache
        // hit ratio, avg duration, error count).  See
        // tool_execution_monitor.h for the full schema.
        result = tool_monitor_.stats();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools.telemetry_clear") {
        // Wipe every monitor counter.  Deliberately independent from
        // tools.cache_clear so ops can zero the cache without losing
        // invocation history and vice-versa.
        tool_monitor_.clear();
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "chat.run") {
        // Render an optional `promptId` + `promptArguments` into the
        // messages array before delegating to start_chat.  Doing it here
        // (rather than inside start_chat) keeps chat.run_with_mcp free to
        // apply the prompt exactly once via its own path.
        Json forwarded = params;
        const int32_t prompt_status = apply_prompt_source(forwarded);
        if (prompt_status != SAO_AI_EDITOR_OK) {
            return prompt_status;
        }
        forwarded.erase("promptId");
        forwarded.erase("promptArguments");
        return start_chat(forwarded, result);
    }
    if (method == "chat.run_with_mcp") {
        return start_chat_with_mcp(params, result);
    }
    if (method == "chat.dispatch_tool_calls") {
        return dispatch_tool_calls(params, result);
    }
    if (method == "chat.set_pricing") {
        return set_pricing(params, result);
    }
    if (method == "chat.get_pricing") {
        return get_pricing(params, result);
    }
    if (method == "chat.list_pricing") {
        return list_pricing(params, result);
    }
    if (method == "chat.cost_stats") {
        return cost_stats(params, result);
    }
    if (method == "run.cancel") {
        return cancel_run(params, result);
    }
    if (method == "run.status") {
        return run_status(params, result);
    }
    if (method.starts_with("mcp.")) {
        return dispatch_mcp(method, params, result);
    }
    if (method.starts_with("workflows.")) {
        return dispatch_workflow(method, params, result);
    }
    if (method.starts_with("auth.")) {
        return dispatch_auth(method, params, result);
    }
    if (method.starts_with("extensions.")) {
        return dispatch_extension(method, params, result);
    }
    // vscode.* / sao.host.* — normally surfaced by the Node-side extension
    // shim invoking back through dispatch_extension_call.  We also expose
    // them on the JSON-RPC dispatch surface so tests + local tools can
    // exercise the same handlers without spinning up a Node worker.
    if (method.starts_with("vscode.") || method.starts_with("sao.host.")) {
        return dispatch_extension_call(method, params, result);
    }
    if (method == "agents.list_defs" || method == "agents.get_def" || method == "agents.save_def" ||
        method == "agents.delete_def" || method == "agents.invoke" ||
        method == "agents.recommend" || method == "agents.export" || method == "agents.import") {
        return dispatch_agent(method, params, result);
    }
    if (method == "agents.invoke_with_mcp") {
        return agent_invoke_with_mcp(params, result);
    }
    if (method == "agents.batch_invoke") {
        return batch_invoke_agents(params, result);
    }
    if (method == "prompts.list_defs" || method == "prompts.get_def" ||
        method == "prompts.save_def" || method == "prompts.delete_def" ||
        method == "prompts.render" || method == "prompts.render_batch" ||
        method == "prompts.list_tags" || method == "prompt.pin" || method == "prompt.unpin") {
        return dispatch_prompt(method, params, result);
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::dispatch_agent(std::string_view method, const Json& params, Json& result) {
    if (method == "agents.list_defs") {
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            agent_registry_.reload(scopes_);
        }
        Json items = Json::array();
        for (const auto& agent : agent_registry_.list()) {
            items.push_back(agent.to_json());
        }
        result = Json{{"items", std::move(items)}};
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.recommend") {
        // Rule-based recommender.  The registry does the scoring; this branch
        // just marshals params → typed args, applies the include* filter mask
        // (an all-false include set short-circuits to an empty response so
        // we don't rely on the "0 == include everything" fallback), and
        // reloads the registry so any market presets / on-disk edits land
        // before we score.
        if (!params.contains("query") || !params["query"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string query = params["query"].get<std::string>();
        const int top_k = params.value("topK", 3);
        const bool include_builtin = params.value("includeBuiltin", true);
        const bool include_market = params.value("includeMarket", true);
        const bool include_user = params.value("includeUser", true);
        uint32_t scope_flags = 0U;
        if (include_builtin) {
            scope_flags |= kRecommendIncludeBuiltin;
        }
        if (include_market) {
            scope_flags |= kRecommendIncludeMarket;
        }
        if (include_user) {
            scope_flags |= kRecommendIncludeUser;
        }
        const bool all_disabled = !include_builtin && !include_market && !include_user;
        std::vector<std::string> boost_tags;
        if (params.contains("boostTags") && params["boostTags"].is_array()) {
            for (const auto& tag : params["boostTags"]) {
                if (tag.is_string()) {
                    boost_tags.push_back(tag.get<std::string>());
                }
            }
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            agent_registry_.reload(scopes_);
        }
        if (all_disabled) {
            result = Json{{"query", query}, {"recommendations", Json::array()}, {"total", 0}};
            return SAO_AI_EDITOR_OK;
        }
        return agent_registry_.recommend(query, top_k, scope_flags, boost_tags, result);
    }
    if (method == "agents.get_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            agent_registry_.reload(scopes_);
        }
        AgentDefinition agent;
        if (!agent_registry_.get(params["id"].get<std::string>(), agent)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        result = agent.to_json();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.save_def") {
        if (!params.contains("agent") || !params["agent"].is_object() ||
            !params.contains("scope") || !params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        AgentDefinition agent = AgentDefinition::from_json(params["agent"]);
        const std::string scope_key = params["scope"].get<std::string>();
        std::string scope;
        std::string plugin_id;
        if (scope_key == "system" || scope_key == "workspace") {
            scope = scope_key;
        } else if (scope_key.rfind("plugin:", 0) == 0) {
            scope = "plugin";
            plugin_id = scope_key.substr(7);
            if (!valid_simple_id(plugin_id)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        } else {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status = agent_registry_.save(agent, scopes_, scope, plugin_id);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = agent.to_json();
        result["scope"] = scope_key;
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.delete_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status =
            agent_registry_.remove(params["id"].get<std::string>(), scopes_,
                                   params.value("scope", "workspace"), std::string{});
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"ok", true}, {"id", params["id"]}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.export") {
        // Mirrors workflow.export: `id` and `scope` are mutually exclusive.
        // Passing neither or both is a caller error rather than a "return
        // everything" convenience — the client already has agents.list_defs
        // for browsing.
        const bool has_id = params.contains("id");
        const bool has_scope = params.contains("scope");
        if (has_id == has_scope) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            agent_registry_.reload(scopes_);
        }
        if (has_id) {
            if (!params["id"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            AgentDefinition agent;
            if (!agent_registry_.get(params["id"].get<std::string>(), agent)) {
                return SAO_AI_EDITOR_ERR_NOT_FOUND;
            }
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            result =
                Json{{"format", "sao-agent/1"}, {"agent", agent.to_json()}, {"exportedAt", now}};
            // Envelope integrity — see conversation.export for the invariant.
            result["sha256"] = sha256_envelope_hex(result);
            return SAO_AI_EDITOR_OK;
        }
        if (!params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // agent_registry_.export_all() stamps its own sha256.
        return agent_registry_.export_all(params["scope"].get<std::string>(), result);
    }
    if (method == "agents.import") {
        if (!params.contains("payload") || !params["payload"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Accept the same scope key vocabulary as agents.save_def: bare
        // "workspace"/"system" or "plugin:<id>".  Default to workspace so
        // callers that omit `scope` land somewhere reasonable.
        const std::string scope_key = params.value("scope", std::string{"workspace"});
        std::string scope;
        std::string plugin_id;
        if (!parse_scope_key(scope_key, scope, plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const bool overwrite = params.value("overwrite", false);
        const Json& payload = params["payload"];
        // Envelope integrity guard — see conversation.import for the invariant.
        // Older R13-era exports predate the checksum so a missing digest is
        // silently accepted; a present-but-wrong digest surfaces PROTOCOL.
        if (payload.contains("sha256")) {
            if (!payload["sha256"].is_string())
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            const std::string claimed = payload["sha256"].get<std::string>();
            if (!valid_sha256_hex(claimed))
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            const std::string recomputed = sha256_envelope_hex(payload);
            if (recomputed.empty() || claimed != recomputed) {
                result = Json{{"message", "envelope sha256 mismatch"},
                              {"expected", claimed},
                              {"actual", recomputed}};
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
        }
        const std::string format = payload.value("format", std::string{});
        std::vector<Json> incoming;
        if (format == "sao-agent/1") {
            if (!payload.contains("agent") || !payload["agent"].is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            incoming.push_back(payload["agent"]);
        } else if (format == "sao-agents/1") {
            if (!payload.contains("agents") || !payload["agents"].is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            for (const auto& item : payload["agents"]) {
                if (!item.is_object()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                incoming.push_back(item);
            }
        } else {
            result = Json{{"message", "unknown export format"}, {"format", format}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (incoming.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        agent_registry_.reload(scopes_);
        // Two-pass conflict detection so overwrite=false rejects atomically
        // instead of half-importing a batch.  Built-ins are always fatal —
        // even with overwrite=true — because letting a user replace a
        // built-in id would either shadow it after reload or masquerade a
        // user agent as compile-time.
        Json conflicts = Json::array();
        for (const auto& agent : incoming) {
            const std::string candidate_id = agent.value("id", std::string{});
            if (candidate_id.empty() || !valid_simple_id(candidate_id)) {
                result = Json{{"message", "invalid agent id"}, {"id", candidate_id}};
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (agent_registry_.is_builtin(candidate_id)) {
                result = Json{{"imported", 0},
                              {"conflicts", Json::array({candidate_id})},
                              {"assignedIds", Json::array()},
                              {"message", "cannot overwrite builtin"}};
                return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
            }
            AgentDefinition existing;
            if (agent_registry_.get(candidate_id, existing) && !existing.builtin) {
                conflicts.push_back(candidate_id);
            }
        }
        if (!conflicts.empty() && !overwrite) {
            result = Json{{"imported", 0},
                          {"conflicts", std::move(conflicts)},
                          {"assignedIds", Json::array()}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json assigned_ids = Json::array();
        size_t imported = 0;
        for (const auto& agent : incoming) {
            std::string assigned_id;
            const int32_t import_status = agent_registry_.import_agent(
                agent, scope, plugin_id, overwrite, scopes_, assigned_id);
            if (import_status != SAO_AI_EDITOR_OK) {
                result = Json{{"imported", imported},
                              {"conflicts", std::move(conflicts)},
                              {"assignedIds", std::move(assigned_ids)}};
                return import_status;
            }
            assigned_ids.push_back(assigned_id);
            ++imported;
        }
        result = Json{{"imported", imported},
                      {"conflicts", std::move(conflicts)},
                      {"assignedIds", std::move(assigned_ids)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.invoke") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string message = params.value("message", std::string{});
        if (message.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            agent_registry_.reload(scopes_);
        }
        AgentDefinition agent;
        if (!agent_registry_.get(params["id"].get<std::string>(), agent)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        // conversationId is authoritative when provided: we load the stored
        // history from disk and ignore the `history` param.  If it's absent
        // and createConversation=true, we lazily create a fresh workspace
        // conversation and return its id so the caller can keep appending.
        std::string conversation_id = params.value("conversationId", std::string{});
        const bool create_conversation = params.value("createConversation", false);
        Json history_from_store = Json::array();
        if (!conversation_id.empty()) {
            Json conversation_doc;
            int32_t get_status = SAO_AI_EDITOR_OK;
            {
                std::lock_guard<std::mutex> guard(store_mutex_);
                get_status = conversations_.get(conversation_id, conversation_doc);
            }
            if (get_status != SAO_AI_EDITOR_OK) {
                return get_status;
            }
            history_from_store = conversation_doc.value("messages", Json::array());
        } else if (create_conversation) {
            Json created;
            const std::string title = params.value("conversationTitle", agent.name);
            const std::string convo_model = params.value("model", agent.model);
            int32_t create_status = SAO_AI_EDITOR_OK;
            {
                std::lock_guard<std::mutex> guard(store_mutex_);
                create_status = conversations_.create(title, convo_model, "workspace", created);
            }
            if (create_status != SAO_AI_EDITOR_OK) {
                return create_status;
            }
            conversation_id = created.value("id", std::string{});
        }
        // When a conversationId is in play, the persisted history overrides
        // any caller-supplied `history` array.  Without one, we fall back to
        // the legacy single-shot behaviour that still respects `history`.
        const Json history =
            !conversation_id.empty() ? history_from_store : params.value("history", Json::array());
        Json messages = agent_registry_.build_chat_messages(agent, message, history);
        const std::string effective_model = params.value("model", agent.model);
        Json chat_params = params;
        chat_params.erase("id");
        chat_params.erase("message");
        chat_params.erase("history");
        chat_params.erase("conversationId");
        chat_params.erase("createConversation");
        chat_params.erase("conversationTitle");
        chat_params["messages"] = std::move(messages);
        if (!effective_model.empty()) {
            chat_params["model"] = effective_model;
        }
        // Prompt library integration: an agents.invoke_with_prompt caller
        // passes promptId (+ optional promptArguments) alongside the agent
        // id.  Render before start_chat sees the payload so the library
        // prompt lands as a proper message.  Strip the fields afterwards so
        // downstream code can't double-apply them.
        const int32_t prompt_status = apply_prompt_source(chat_params);
        if (prompt_status != SAO_AI_EDITOR_OK) {
            return prompt_status;
        }
        chat_params.erase("promptId");
        chat_params.erase("promptArguments");
        const uint32_t timeout_ms = params.value("timeoutMs", 60'000U);
        const bool stream_requested = params.value("stream", false);
        std::string content;
        std::function<void(const Json&)> on_delta;
        if (stream_requested) {
            const std::string agent_id = agent.id;
            on_delta = [this, agent_id](const Json& event) {
                Json payload{{"agentId", agent_id}, {"event", event}};
                if (event.value("type", "") == "delta" && event.contains("content") &&
                    event["content"].is_string()) {
                    payload["content"] = event["content"];
                }
                emit("agent.delta", payload);
            };
        }
        const int32_t status = run_chat_sync(chat_params, timeout_ms, content, std::move(on_delta));
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        // Persist both the user turn and the assistant reply so the next
        // agents.invoke on this conversation sees the full transcript.
        if (!conversation_id.empty()) {
            Json append_result;
            std::lock_guard<std::mutex> guard(store_mutex_);
            conversations_.append(conversation_id, Json{{"role", "user"}, {"content", message}},
                                  append_result);
            conversations_.append(conversation_id,
                                  Json{{"role", "assistant"}, {"content", content}}, append_result);
        }
        result = Json{{"agentId", agent.id},
                      {"agentName", agent.name},
                      {"content", std::move(content)},
                      {"model", effective_model}};
        if (!conversation_id.empty()) {
            result["conversationId"] = conversation_id;
        }
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::batch_invoke_agents(const Json& params, Json& result) {
    if (!params.contains("agents") || !params["agents"].is_array() || params["agents"].empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json& agents_array = params["agents"];
    // Default fields let callers avoid repeating provider/model/message
    // per-agent when a common configuration applies.  Per-agent entries
    // override these with the usual JSON "value" semantics (empty string ==
    // missing).
    const Json default_provider = params.value("provider", Json::object());
    const Json top_default_provider = params.value("defaultProvider", default_provider);
    const std::string default_model =
        params.value("defaultModel", params.value("model", std::string{}));
    const std::string default_message = params.value("defaultMessage", std::string{});
    const uint32_t default_timeout_ms = params.value("timeoutMs", 60'000U);
    const std::string conversation_id = params.value("conversationId", std::string{});
    // concurrency policy: clamp caller value to [1, 16].  Default 4 matches
    // the doc contract; upper bound 16 keeps the WinHTTP session pool from
    // exploding under pathological input.
    int concurrency = params.value("concurrency", 4);
    if (concurrency < 1) {
        concurrency = 1;
    }
    if (concurrency > 16) {
        concurrency = 16;
    }
    const size_t total = agents_array.size();
    const size_t worker_count = std::min<size_t>(static_cast<size_t>(concurrency), total);

    // Refresh the registry once up front so every worker sees a consistent
    // snapshot without re-taking store_mutex_ per agent (registry is otherwise
    // treated as read-only from worker threads).
    {
        std::lock_guard<std::mutex> guard(store_mutex_);
        agent_registry_.reload(scopes_);
    }

    struct Slot final {
        std::string agent_id;
        std::string agent_name;
        std::string message;
        std::string model;
        std::string content;
        std::string error;
        int32_t status = SAO_AI_EDITOR_OK;
        bool resolved = false; // agent_id -> definition resolved OK
        uint64_t duration_ms = 0;
        Json params_snapshot = Json::object();
    };
    std::vector<Slot> slots(total);

    // Phase 1 (single-threaded): resolve every agent's definition and build
    // the run_chat_sync parameter blob.  Doing this before dispatching
    // workers keeps the JSON parsing on the caller thread and lets us
    // surface "agent not found" / "provider missing" as per-slot failures
    // without touching the WinHTTP path.
    for (size_t i = 0; i < total; ++i) {
        Slot& slot = slots[i];
        const Json& entry = agents_array[i];
        if (!entry.is_object()) {
            slot.agent_id = "";
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "agent entry must be an object";
            continue;
        }
        slot.agent_id = entry.value("id", std::string{});
        if (slot.agent_id.empty()) {
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "agent id missing";
            continue;
        }
        AgentDefinition agent;
        if (!agent_registry_.get(slot.agent_id, agent)) {
            slot.status = SAO_AI_EDITOR_ERR_NOT_FOUND;
            slot.error = "agent not found";
            continue;
        }
        slot.agent_name = agent.name;
        // Per-agent message overrides defaultMessage; both empty -> failure.
        slot.message = entry.value("message", default_message);
        if (slot.message.empty()) {
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "message missing";
            continue;
        }
        // Provider: per-agent > top-level defaultProvider > empty.  Model:
        // per-agent > defaultModel > agent.model.
        Json provider = entry.contains("provider") && entry["provider"].is_object()
                            ? entry["provider"]
                            : top_default_provider;
        std::string model =
            entry.value("model", default_model.empty() ? agent.model : default_model);
        slot.model = model;
        // Build the messages array from the agent's system prompt + user
        // turn.  Unlike agents.invoke, batch mode never consults conversation
        // history for the outbound request — every agent gets a clean slate
        // per its own message.  We still write successful turns back to
        // conversation storage afterwards for callers that pass
        // conversationId.
        Json messages = agent_registry_.build_chat_messages(agent, slot.message, Json::array());
        Json chat_params = Json::object();
        if (provider.is_object() && !provider.empty()) {
            chat_params["provider"] = std::move(provider);
        }
        if (!model.empty()) {
            chat_params["model"] = model;
        }
        if (entry.contains("timeoutMs")) {
            chat_params["timeoutMs"] = entry["timeoutMs"];
        } else if (params.contains("timeoutMs")) {
            chat_params["timeoutMs"] = params["timeoutMs"];
        }
        chat_params["messages"] = std::move(messages);
        // Forward a small allowlist of chat.run knobs so callers can pass
        // temperature / max_tokens once via defaults or per-agent overrides.
        for (const std::string_view field :
             {"temperature", "max_tokens", "response_format", "stream"}) {
            const std::string key(field);
            if (entry.contains(field)) {
                chat_params[key] = entry[field];
            } else if (params.contains(field)) {
                chat_params[key] = params[field];
            }
        }
        slot.params_snapshot = std::move(chat_params);
        slot.resolved = true;
    }

    // Phase 2: launch worker threads that only touch their own slot.  Use
    // an atomic counter as a lock-free work queue so `concurrency` acts as
    // an in-flight cap without per-task locking.  Slots that failed
    // resolution in phase 1 are skipped instantly.
    const auto batch_start = std::chrono::steady_clock::now();
    std::atomic<size_t> next_index{0};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&, i_capture = w] {
            (void)i_capture;
            while (true) {
                const size_t idx = next_index.fetch_add(1, std::memory_order_acq_rel);
                if (idx >= total) {
                    break;
                }
                Slot& slot = slots[idx];
                if (!slot.resolved) {
                    continue; // phase 1 already recorded status/error
                }
                const auto step_start = std::chrono::steady_clock::now();
                try {
                    std::string content;
                    const int32_t status =
                        run_chat_sync(slot.params_snapshot, default_timeout_ms, content);
                    slot.status = status;
                    if (status == SAO_AI_EDITOR_OK) {
                        slot.content = std::move(content);
                    } else {
                        slot.error = status_message(status);
                    }
                } catch (const std::exception& ex) {
                    slot.status = SAO_AI_EDITOR_ERR_HTTP;
                    slot.error = std::string("exception: ") + ex.what();
                } catch (...) {
                    slot.status = SAO_AI_EDITOR_ERR_HTTP;
                    slot.error = "unknown exception";
                }
                const auto step_end = std::chrono::steady_clock::now();
                slot.duration_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(step_end - step_start)
                        .count());
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto batch_end = std::chrono::steady_clock::now();
    const uint64_t total_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(batch_end - batch_start).count());

    // Phase 3: assemble results in original order.  Conversation appends
    // happen here (single-threaded) so the transcript ordering is
    // deterministic regardless of which worker finished first.  Successful
    // agent replies are prefixed with the agent name so downstream readers
    // can attribute lines back to their source.
    Json results = Json::array();
    size_t success = 0;
    size_t failure = 0;
    for (size_t i = 0; i < total; ++i) {
        const Slot& slot = slots[i];
        Json entry_result{{"agentId", slot.agent_id}, {"durationMs", slot.duration_ms}};
        if (!slot.agent_name.empty()) {
            entry_result["agentName"] = slot.agent_name;
        }
        if (!slot.model.empty()) {
            entry_result["model"] = slot.model;
        }
        if (slot.status == SAO_AI_EDITOR_OK && slot.resolved) {
            entry_result["status"] = "completed";
            entry_result["content"] = slot.content;
            ++success;
            if (!conversation_id.empty()) {
                std::lock_guard<std::mutex> guard(store_mutex_);
                Json append_result;
                conversations_.append(
                    conversation_id,
                    Json{{"role", "user"}, {"content", slot.message}, {"agentId", slot.agent_id}},
                    append_result);
                const std::string prefix = !slot.agent_name.empty() ? "[" + slot.agent_name + "] "
                                                                    : "[" + slot.agent_id + "] ";
                conversations_.append(conversation_id,
                                      Json{{"role", "assistant"},
                                           {"content", prefix + slot.content},
                                           {"agentId", slot.agent_id}},
                                      append_result);
            }
        } else {
            entry_result["status"] = "failed";
            entry_result["error"] = slot.error.empty() ? status_message(slot.status) : slot.error;
            ++failure;
        }
        results.push_back(std::move(entry_result));
    }
    result = Json{{"results", std::move(results)},
                  {"successCount", success},
                  {"failureCount", failure},
                  {"totalMs", total_ms}};
    if (!conversation_id.empty()) {
        result["conversationId"] = conversation_id;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::dispatch_tool_calls(const Json& params, Json& result) {
    RuntimePolicySnapshot policy;
    Json tool_descriptors;
    {
        std::lock_guard<std::mutex> lock(store_mutex_);
        const int32_t status = prepare_runtime_policy(scopes_, params, policy);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        tool_descriptors = tools_.describe(policy.effective_mode);
    }
    if (!params.contains("toolCalls") || !params["toolCalls"].is_array() ||
        params["toolCalls"].empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json& tool_calls = params["toolCalls"];
    // Concurrency policy mirrors agents.batch_invoke: clamp [1, 16] with a
    // default of 4.  A tool dispatch batch is usually smaller than an agent
    // batch (an LLM rarely emits >5 tool_calls in one turn), but the same
    // upper bound stops a hostile prompt from spawning hundreds of threads.
    int concurrency = params.value("concurrency", 4);
    if (concurrency < 1) {
        concurrency = 1;
    }
    if (concurrency > 16) {
        concurrency = 16;
    }
    const size_t total = tool_calls.size();
    const size_t worker_count = std::min<size_t>(static_cast<size_t>(concurrency), total);

    struct Slot final {
        std::string id;   // caller-supplied opaque id (echoed back)
        std::string name; // tool name (empty on validation failure)
        std::string resolved_name;
        std::string execution_mode;
        Json arguments = Json::object();
        Json result_payload = Json::object();
        std::string error;
        int32_t status = SAO_AI_EDITOR_OK;
        bool ready = false; // arguments parsed / name checked OK
        uint64_t duration_ms = 0;
    };
    std::vector<Slot> slots(total);

    // Phase 1 (single-threaded): validate id/name/arguments shape and, when
    // arguments is a JSON string (as LLMs emit them), parse it eagerly so
    // worker threads never touch the raw string form.  Any per-call failure
    // is recorded on the slot and skipped by the worker — peers still run.
    for (size_t i = 0; i < total; ++i) {
        Slot& slot = slots[i];
        const Json& entry = tool_calls[i];
        if (!entry.is_object()) {
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "tool_call entry must be an object";
            continue;
        }
        // id is optional in the OpenAI wire format (`call_...`) but we echo
        // it back verbatim so callers can correlate the response array with
        // whichever id scheme the LLM used.  Missing id -> empty string.
        if (entry.contains("id") && entry["id"].is_string()) {
            slot.id = entry["id"].get<std::string>();
        }
        if (!entry.contains("name") || !entry["name"].is_string() ||
            entry["name"].get<std::string>().empty()) {
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "name missing or not a non-empty string";
            continue;
        }
        slot.name = entry["name"].get<std::string>();
        // arguments may be:
        //   - absent  -> default to {}
        //   - object  -> forwarded as-is
        //   - string  -> Json::parse'd; failure records "invalid arguments JSON"
        // Anything else (array/number/etc.) is rejected as malformed.
        if (!entry.contains("arguments")) {
            slot.arguments = Json::object();
        } else {
            const Json& raw = entry["arguments"];
            if (raw.is_object()) {
                slot.arguments = raw;
            } else if (raw.is_string()) {
                const std::string text = raw.get<std::string>();
                if (text.empty()) {
                    slot.arguments = Json::object();
                } else {
                    try {
                        Json parsed = Json::parse(text);
                        if (!parsed.is_object()) {
                            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                            slot.error = "invalid arguments JSON";
                            continue;
                        }
                        slot.arguments = std::move(parsed);
                    } catch (const std::exception&) {
                        slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                        slot.error = "invalid arguments JSON";
                        continue;
                    }
                }
            } else {
                slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                slot.error = "arguments must be an object or JSON string";
                continue;
            }
        }
        slot.resolved_name = tools_.resolve_alias(slot.name);
        const Json* descriptor = find_tool_descriptor(tool_descriptors, slot.name);
        if (descriptor == nullptr && slot.resolved_name != slot.name) {
            descriptor = find_tool_descriptor(tool_descriptors, slot.resolved_name);
        }
        ToolExecutionPlan execution;
        const int32_t policy_status = prepare_tool_execution(policy, slot.name, slot.resolved_name,
                                                             descriptor, slot.arguments, execution);
        if (policy_status != SAO_AI_EDITOR_OK) {
            slot.status = policy_status;
            slot.error = status_message(policy_status);
            if (policy_status == SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED) {
                slot.result_payload = Json{{"confirmationRequired", true},
                                           {"tool", slot.name},
                                           {"permissionSource", execution.permission.source},
                                           {"permissionCategory", execution.category}};
            }
            continue;
        }
        slot.execution_mode = std::move(execution.mode);
        slot.arguments = std::move(execution.arguments);
        slot.ready = true;
    }

    // Phase 2: worker pool.  Every worker grabs the next unclaimed slot via
    // an atomic counter (same lock-free pattern as batch_invoke_agents) and
    // runs tools_.execute directly.  Unlike the dispatch()-level tools.call
    // branch, we intentionally do NOT hold store_mutex_ during execution —
    // NativeToolRegistry::execute is const + self-synchronising, and the
    // whole point of the batch API is to run calls concurrently.  Holding
    // store_mutex_ here would serialise every worker back onto one thread
    // and defeat the concurrency knob.
    const auto batch_start = std::chrono::steady_clock::now();
    std::atomic<size_t> next_index{0};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&] {
            while (true) {
                const size_t idx = next_index.fetch_add(1, std::memory_order_acq_rel);
                if (idx >= total) {
                    break;
                }
                Slot& slot = slots[idx];
                if (!slot.ready) {
                    continue; // phase 1 already recorded status/error
                }
                const auto step_start = std::chrono::steady_clock::now();
                try {
                    Json tool_result;
                    const int32_t status =
                        tools_.execute(slot.execution_mode, slot.name, slot.arguments, tool_result);
                    slot.status = status;
                    if (status == SAO_AI_EDITOR_OK) {
                        slot.result_payload = std::move(tool_result);
                    } else {
                        // Propagate whatever `details` the tool registry
                        // returned (e.g. validationErrors) into the error
                        // path so callers still get structured feedback.
                        slot.result_payload = std::move(tool_result);
                        slot.error = status_message(status);
                    }
                } catch (const std::exception& ex) {
                    slot.status = SAO_AI_EDITOR_ERR_HTTP;
                    slot.error = std::string("exception: ") + ex.what();
                } catch (...) {
                    slot.status = SAO_AI_EDITOR_ERR_HTTP;
                    slot.error = "unknown exception";
                }
                const auto step_end = std::chrono::steady_clock::now();
                slot.duration_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(step_end - step_start)
                        .count());
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto batch_end = std::chrono::steady_clock::now();
    const uint64_t total_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(batch_end - batch_start).count());

    // Phase 3: assemble the response in original input order.  Runs on the
    // caller thread so no synchronisation is needed even though the workers
    // may have completed out of order.
    Json results = Json::array();
    size_t success = 0;
    size_t failure = 0;
    for (size_t i = 0; i < total; ++i) {
        const Slot& slot = slots[i];
        Json entry_result{{"id", slot.id}, {"name", slot.name}, {"durationMs", slot.duration_ms}};
        if (slot.status == SAO_AI_EDITOR_OK && slot.ready) {
            entry_result["status"] = "completed";
            entry_result["result"] = slot.result_payload;
            ++success;
        } else {
            entry_result["status"] = "failed";
            entry_result["error"] = slot.error.empty() ? status_message(slot.status) : slot.error;
            // Surface tool-registry details (validationErrors etc.) even on
            // failure so LLM-facing UI can highlight the offending field.
            if (slot.result_payload.is_object() && !slot.result_payload.empty()) {
                entry_result["details"] = slot.result_payload;
            }
            ++failure;
        }
        results.push_back(std::move(entry_result));
    }
    result = Json{{"results", std::move(results)},
                  {"mode", policy.effective_mode},
                  {"settingsMode", policy.settings_mode},
                  {"requestedMode", policy.request_mode},
                  {"approval", policy.effective_approval},
                  {"successCount", success},
                  {"failureCount", failure},
                  {"totalMs", total_ms}};
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::dispatch_prompt(std::string_view method, const Json& params, Json& result) {
    if (method == "prompts.list_defs") {
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        // Optional tag filter: OR semantics — a prompt survives if it has
        // at least one tag in `params.tags`.  Missing / empty / non-array
        // falls back to the full list (backward compatible).
        std::vector<std::string> tag_filter;
        if (params.is_object() && params.contains("tags") && params["tags"].is_array()) {
            for (const auto& item : params["tags"]) {
                if (item.is_string()) {
                    const std::string tag = item.get<std::string>();
                    if (!tag.empty()) {
                        tag_filter.push_back(tag);
                    }
                }
            }
        }
        Json items = Json::array();
        const auto prompts = tag_filter.empty() ? prompt_registry_.list()
                                                : prompt_registry_.list_by_tags(tag_filter);
        for (const auto& prompt : prompts) {
            items.push_back(prompt.to_json());
        }
        result = Json{{"items", std::move(items)}};
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.list_tags") {
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        prompt_registry_.list_all_tags(result);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.render_batch") {
        if (!params.is_object() || !params.contains("items") || !params["items"].is_array() ||
            params["items"].empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // onMissing controls whether an unknown prompt id aborts the batch
        // ("fail" — INVALID_ARGUMENT so callers get a clear surface) or
        // just records a per-item not_found (skip).  Same shape as MCP
        // batch tool-call semantics elsewhere in the runtime.
        const std::string on_missing = params.value("onMissing", std::string{"fail"});
        if (on_missing != "fail" && on_missing != "skip") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        Json results = Json::array();
        size_t success = 0;
        size_t failure = 0;
        for (const auto& entry : params["items"]) {
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            const std::string id = entry["id"].get<std::string>();
            Json arguments = entry.value("arguments", Json::object());
            if (!arguments.is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            PromptDefinition prompt;
            if (!prompt_registry_.get(id, prompt)) {
                if (on_missing == "fail") {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                results.push_back(
                    Json{{"id", id}, {"status", "not_found"}, {"error", "prompt id not found"}});
                ++failure;
                continue;
            }
            results.push_back(
                Json{{"id", prompt.id}, {"content", prompt.render(arguments)}, {"status", "ok"}});
            ++success;
        }
        result = Json{
            {"results", std::move(results)}, {"successCount", success}, {"failureCount", failure}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.get_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        PromptDefinition prompt;
        if (!prompt_registry_.get(params["id"].get<std::string>(), prompt)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        result = prompt.to_json();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.save_def") {
        if (!params.contains("prompt") || !params["prompt"].is_object() ||
            !params.contains("scope") || !params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        PromptDefinition prompt = PromptDefinition::from_json(params["prompt"]);
        const std::string scope_key = params["scope"].get<std::string>();
        std::string scope;
        std::string plugin_id;
        if (scope_key == "system" || scope_key == "workspace") {
            scope = scope_key;
        } else if (scope_key.rfind("plugin:", 0) == 0) {
            scope = "plugin";
            plugin_id = scope_key.substr(7);
            if (!valid_simple_id(plugin_id)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        } else {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status = prompt_registry_.save(prompt, scopes_, scope, plugin_id);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = prompt.to_json();
        result["scope"] = scope_key;
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.delete_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status =
            prompt_registry_.remove(params["id"].get<std::string>(), scopes_,
                                    params.value("scope", "workspace"), std::string{});
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"ok", true}, {"id", params["id"]}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.render") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        PromptDefinition prompt;
        if (!prompt_registry_.get(params["id"].get<std::string>(), prompt)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        const Json arguments = params.value("arguments", Json::object());
        if (!arguments.is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        result = Json{{"id", prompt.id}, {"content", prompt.render(arguments)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompt.pin" || method == "prompt.unpin") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Reload before mutating so the registry reflects any external
        // edits before we consult the entry's scope key.  set_pinned()
        // is scope-aware and reuses save() to persist the toggled flag,
        // so no additional bookkeeping is required here.
        std::lock_guard<std::mutex> guard(store_mutex_);
        prompt_registry_.reload(scopes_);
        const bool pinned = (method == "prompt.pin");
        return prompt_registry_.set_pinned(params["id"].get<std::string>(), pinned, scopes_,
                                           result);
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::apply_prompt_source(Json& params) {
    if (!params.is_object() || !params.contains("promptId")) {
        return SAO_AI_EDITOR_OK;
    }
    const Json& id_field = params["promptId"];
    // Absent / null / empty-string promptId is a no-op — callers may send
    // the field unconditionally.  The runtime still strips it from the
    // forwarded params in the caller.
    if (!id_field.is_string() || id_field.get<std::string>().empty()) {
        return SAO_AI_EDITOR_OK;
    }
    const std::string id = id_field.get<std::string>();
    Json arguments = Json::object();
    if (params.contains("promptArguments")) {
        if (!params["promptArguments"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        arguments = params["promptArguments"];
    }
    {
        std::lock_guard<std::mutex> guard(store_mutex_);
        prompt_registry_.reload(scopes_);
    }
    PromptDefinition prompt;
    if (!prompt_registry_.get(id, prompt)) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const std::string rendered = prompt.render(arguments);
    // Match apply_system_prompt_source: prepend as system when the caller
    // has no system message yet; otherwise queue as a user turn so the
    // existing system message stays authoritative.
    const bool caller_has_system = params.contains("messages") && params["messages"].is_array() &&
                                   !params["messages"].empty() &&
                                   params["messages"][0].is_object() &&
                                   params["messages"][0].value("role", "") == "system";
    const std::string role = caller_has_system ? "user" : "system";
    Json prepended = Json::array();
    prepended.push_back(Json{{"role", role}, {"content", rendered}});
    Json existing = params.value("messages", Json::array());
    if (!existing.is_array()) {
        existing = Json::array();
    }
    for (const auto& message : existing) {
        prepended.push_back(message);
    }
    params["messages"] = std::move(prepended);
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::dispatch_extension(std::string_view method, const Json& params,
                                          Json& result) {
    if (extension_host_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    if (method == "extensions.configure_host") {
        return extension_host_->configure(params);
    }
    if (method == "extensions.list") {
        return extension_host_->list_extensions(result);
    }
    if (method == "extensions.register") {
        return extension_host_->register_extension(params, result);
    }
    if (method == "extensions.unregister") {
        if (!params.contains("extensionId") || !params["extensionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return extension_host_->unregister_extension(params["extensionId"].get<std::string>(),
                                                     result);
    }
    if (method == "extensions.activate") {
        if (!params.contains("extensionId") || !params["extensionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string extension_id = params["extensionId"].get<std::string>();
        const int32_t status = extension_host_->activate(
            extension_id, params.value("timeoutMs", 15000U), result);
        if (status == SAO_AI_EDITOR_OK) {
            extapi::publish("extensionHost",
                            Json{{"op", "activated"}, {"extensionId", extension_id}});
        }
        return status;
    }
    if (method == "extensions.deactivate") {
        if (!params.contains("extensionId") || !params["extensionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string extension_id = params["extensionId"].get<std::string>();
        const int32_t status = extension_host_->deactivate(extension_id, result);
        if (status == SAO_AI_EDITOR_OK) {
            extapi::publish("extensionHost",
                            Json{{"op", "deactivated"}, {"extensionId", extension_id}});
        }
        return status;
    }
    if (method == "extensions.execute_command") {
        if (!params.contains("command") || !params["command"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return extension_host_->execute_command(params["command"].get<std::string>(),
                                                params.value("arguments", Json::array()),
                                                params.value("timeoutMs", 15000U), result);
    }
    if (method == "extensions.snapshot") {
        result = extension_host_->snapshot();
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::dispatch_extension_call(std::string_view method, const Json& params,
                                               Json& result) {
    // vscode.workspace.* — file surface goes through the built-in tools
    // registry so writes still respect the workspace boundary + permission
    // model.
    if (method == "vscode.workspace.readTextDocument") {
        const Json arguments{{"path", params.value("path", std::string{})},
                             {"startLine", params.value("startLine", 0)},
                             {"endLine", params.value("endLine", 0)}};
        return tools_.execute("agent", "readFile", arguments, result);
    }
    if (method == "vscode.workspace.writeTextDocument") {
        Json arguments = params;
        arguments.erase("confirmed");
        Json policy_params = Json::object();
        for (const char* key : {"mode", "approval", "permissions"})
            if (params.contains(key))
                policy_params[key] = params[key];
        RuntimePolicySnapshot policy;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status = prepare_runtime_policy(scopes_, policy_params, policy);
            if (status != SAO_AI_EDITOR_OK)
                return status;
        }
        const Json descriptor{{"name", "editFile"}, {"readOnly", false}};
        ToolExecutionPlan plan;
        const int32_t permission_status =
            prepare_tool_execution(policy, "editFile", "editFile", &descriptor, arguments, plan);
        if (permission_status != SAO_AI_EDITOR_OK)
            return permission_status;
        return tools_.execute(plan.mode, "editFile", plan.arguments, result);
    }
    if (method == "vscode.workspace.findFiles") {
        const Json arguments{{"path", params.value("path", std::string{"."})},
                             {"pattern", params.value("pattern", std::string{"*"})},
                             {"recursive", params.value("recursive", true)},
                             {"limit", params.value("limit", 200)}};
        return tools_.execute("agent", "listFiles", arguments, result);
    }
    if (method == "vscode.workspace.textSearch") {
        const Json arguments{{"query", params.value("query", std::string{})},
                             {"path", params.value("path", std::string{"."})},
                             {"pattern", params.value("pattern", std::string{"*"})},
                             {"regex", params.value("regex", false)},
                             {"caseSensitive", params.value("caseSensitive", false)},
                             {"limit", params.value("limit", 200)}};
        return tools_.execute("agent", "searchFiles", arguments, result);
    }
    if (method == "vscode.workspace.workspaceFolders") {
        // Real multi-root surface — the primary root plus every persisted
        // extra folder (see vscode.workspace.updateWorkspaceFolders).
        return extapi::dispatch(this, method, params, result);
    }
    if (method == "vscode.workspace.getConfiguration") {
        Json merged;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status = scopes_.load_merged_config(merged);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        const std::string section = params.value("section", std::string{});
        if (section.empty()) {
            result = std::move(merged);
        } else {
            const auto found = merged.find(section);
            result = found == merged.end() ? Json::object() : *found;
        }
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.workspace.updateConfiguration") {
        if (!params.contains("section") || !params["section"].is_string() ||
            !params.contains("value")) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json policy_params = Json::object();
        for (const char* key : {"mode", "approval", "permissions"})
            if (params.contains(key))
                policy_params[key] = params[key];
        RuntimePolicySnapshot policy;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status = prepare_runtime_policy(scopes_, policy_params, policy);
            if (status != SAO_AI_EDITOR_OK)
                return status;
        }
        const Json descriptor{
            {"name", "updateConfiguration"}, {"category", "write"}, {"readOnly", false}};
        ToolExecutionPlan plan;
        Json arguments = params;
        arguments.erase("confirmed");
        const int32_t permission_status = prepare_tool_execution(
            policy, "updateConfiguration", "updateConfiguration", &descriptor, arguments, plan);
        if (permission_status != SAO_AI_EDITOR_OK)
            return permission_status;
        std::lock_guard<std::mutex> lock(store_mutex_);
        Json existing;
        (void)scopes_.load_scope_config("workspace", "", existing);
        existing[params["section"].get<std::string>()] = params["value"];
        const int32_t save_status =
            scopes_.save_scope_config("workspace", "", existing);
        if (save_status == SAO_AI_EDITOR_OK) {
            extapi::publish("configChanged",
                            Json{{"section", params["section"].get<std::string>()},
                                 {"scope", "workspace"},
                                 {"value", params["value"]}});
        }
        return save_status;
    }
    // vscode.window.* — surface messages/inputs as native events so the
    // host UI can render them.
    if (method == "vscode.window.showInformationMessage" ||
        method == "vscode.window.showWarningMessage" ||
        method == "vscode.window.showErrorMessage") {
        const std::string_view kind = method.substr(std::string_view("vscode.window.show").size());
        emit(std::string("vscode.window.") + std::string(kind),
             Json{{"message", params.value("message", std::string{})},
                  {"actions", params.value("actions", Json::array())}});
        result = Json(nullptr);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.showQuickPick" || method == "vscode.window.showInputBox") {
        emit(std::string("vscode.window.") +
                 std::string(method.substr(std::string_view("vscode.window.").size())),
             params);
        // No UI attached — fail closed with null.
        result = Json(nullptr);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.createOutputChannel") {
        // Real channel store with a bounded rolling buffer (see extapi).
        return extapi::dispatch(this, method, params, result);
    }
    if (method == "vscode.window.appendOutput") {
        // Append into the extapi channel store; the historical
        // vscode.window.output event emission happens inside extapi so
        // both doors stay consistent.
        return extapi::dispatch(this, method, params, result);
    }
    // vscode.window.createWebviewPanel — register a fresh WebviewPanel
    // record and emit an event so the host UI (WebView2 bridge or Tk
    // fallback shim) can materialise a window.  Returns a JSON snapshot
    // of the new panel; the id is deterministic within a runtime instance.
    if (method == "vscode.window.createWebviewPanel") {
        const std::string view_type = params.value("viewType", std::string{});
        if (view_type.empty()) {
            result = Json{{"message", "viewType is required"}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WebviewPanelOptions options;
        if (params.contains("options") && params["options"].is_object()) {
            const Json& opts = params["options"];
            options.enable_scripts = opts.value("enableScripts", true);
            options.retain_context_when_hidden = opts.value("retainContextWhenHidden", false);
            options.view_column = opts.value("viewColumn", 1);
            options.extras = opts.value("extras", Json::object());
        }
        WebviewPanelState state;
        const int32_t status =
            webview_panels_.create(params.value("panelId", std::string{}), view_type,
                                   params.value("title", std::string{}), options, state);
        if (status != SAO_AI_EDITOR_OK) {
            result = Json{{"message", "createWebviewPanel failed"}};
            return status;
        }
        emit("vscode.window.webviewPanel.created", panel_state_to_json(state));
        result = panel_state_to_json(state);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.revealWebviewPanel") {
        const std::string panel_id = params.value("panelId", std::string{});
        if (panel_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WebviewPanelState state;
        const int32_t status = webview_panels_.reveal(panel_id, params.value("viewColumn", 1),
                                                      params.value("preserveFocus", false), state);
        if (status != SAO_AI_EDITOR_OK) {
            result = Json{{"message", "reveal failed"}, {"panelId", panel_id}};
            return status;
        }
        emit("vscode.window.webviewPanel.revealed", panel_state_to_json(state));
        result = panel_state_to_json(state);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.disposeWebviewPanel") {
        const std::string panel_id = params.value("panelId", std::string{});
        if (panel_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WebviewPanelState state;
        const int32_t status = webview_panels_.dispose(panel_id, state);
        if (status != SAO_AI_EDITOR_OK) {
            result = Json{{"message", "dispose failed"}, {"panelId", panel_id}};
            return status;
        }
        emit("vscode.window.webviewPanel.disposed", panel_state_to_json(state));
        result = panel_state_to_json(state);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.webview.postMessage") {
        const std::string panel_id = params.value("panelId", std::string{});
        const std::string view_id = params.value("viewId", std::string{});
        if (panel_id.empty() && view_id.empty()) {
            result = Json{{"message", "panelId or viewId is required"}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (!panel_id.empty()) {
            WebviewPanelState state;
            const int32_t status = webview_panels_.note_post_message(panel_id, state);
            if (status != SAO_AI_EDITOR_OK) {
                emit("vscode.window.webviewPanel.postFailed",
                     Json{{"panelId", panel_id},
                          {"status", status},
                          {"reason", status == SAO_AI_EDITOR_ERR_NOT_FOUND ? "unknown panel"
                                                                           : "panel disposed"}});
                result = Json{
                    {"accepted", false}, {"message", "postMessage failed"}, {"panelId", panel_id}};
                return status;
            }
            Json bridge_params = params;
            bridge_params["messageSeq"] = static_cast<int64_t>(state.message_seq);
            const int32_t bridge_status = dispatch_webview_message_to_page(bridge_params, result);
            if (bridge_status != SAO_AI_EDITOR_OK) {
                emit("vscode.window.webviewPanel.postFailed",
                     Json{{"panelId", panel_id},
                          {"status", bridge_status},
                          {"reason", "webview bridge is not ready"}});
                result["accepted"] = false;
                return bridge_status;
            }
            emit("vscode.webview.postMessage", params);
            result = Json{{"accepted", true}, {"panelId", panel_id}, {"viewId", view_id}};
            return SAO_AI_EDITOR_OK;
        }
        result = Json{{"accepted", false},
                      {"message", "webview panel is not registered"},
                      {"panelId", panel_id},
                      {"viewId", view_id}};
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (method == "vscode.window.postMessageToWebview") {
        const std::string panel_id = params.value("panelId", std::string{});
        if (panel_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WebviewPanelState state;
        const int32_t status = webview_panels_.note_post_message(panel_id, state);
        if (status != SAO_AI_EDITOR_OK) {
            emit("vscode.window.webviewPanel.postFailed",
                 Json{{"panelId", panel_id},
                      {"status", status},
                      {"reason", status == SAO_AI_EDITOR_ERR_NOT_FOUND ? "unknown panel"
                                                                       : "panel disposed"}});
            result = Json{{"message", "postMessage failed"}, {"panelId", panel_id}};
            return status;
        }
        const Json payload = params.value("message", Json());
        NativePanelProvider* native_provider = nullptr;
        if (kernel_map_panel_ != nullptr && panel_id == kernel_map_panel_->provider_panel_id()) {
            native_provider = kernel_map_panel_.get();
        } else if (mcp_management_panel_ != nullptr &&
                   panel_id == mcp_management_panel_->provider_panel_id()) {
            native_provider = mcp_management_panel_.get();
        }
        if (native_provider != nullptr) {
            Json provider_reply;
            const int32_t provider_status =
                native_provider->handle_message(payload, provider_reply);
            if (provider_status != SAO_AI_EDITOR_OK) {
                result =
                    Json{{"accepted", false}, {"panelId", panel_id}, {"status", provider_status}};
                return provider_status;
            }
            if (provider_reply.value("status", std::string{}) == "forward") {
                const std::string forward_method = provider_reply.value("method", std::string{});
                const Json routed_params = provider_reply.value("params", Json::object());
                Json routed_result;
                const int32_t routed_status =
                    dispatch_mcp(forward_method, routed_params, routed_result);
                Json routed_reply{
                    {"cmd", payload.value("cmd", payload.value("command", std::string{}))},
                    {"status", routed_status == SAO_AI_EDITOR_OK ? "ok" : "error"}};
                if (payload.contains("requestId"))
                    routed_reply["requestId"] = payload["requestId"];
                if (payload.contains("generation"))
                    routed_reply["generation"] = payload["generation"];
                if (routed_status == SAO_AI_EDITOR_OK) {
                    routed_reply["payload"] = std::move(routed_result);
                } else {
                    routed_reply["reason"] = routed_result.is_object() &&
                                                     routed_result.contains("reason") &&
                                                     routed_result["reason"].is_string()
                                                 ? routed_result["reason"].get<std::string>()
                                                 : status_message(routed_status);
                    if (!routed_result.is_null() && !routed_result.empty())
                        routed_reply["payload"] = std::move(routed_result);
                }
                provider_reply = std::move(routed_reply);
            }
            Json page_result;
            const int32_t page_status = dispatch_webview_message_to_page(
                Json{{"panelId", panel_id},
                     {"messageSeq", static_cast<int64_t>(state.message_seq)},
                     {"message", provider_reply}},
                page_result);
            if (page_status != SAO_AI_EDITOR_OK) {
                result = std::move(page_result);
                return page_status;
            }
            emit("vscode.window.webviewPanel.postMessage",
                 Json{{"panelId", panel_id},
                      {"messageSeq", static_cast<int64_t>(state.message_seq)},
                      {"message", provider_reply}});
            result = Json{{"panelId", panel_id},
                          {"messageSeq", static_cast<int64_t>(state.message_seq)},
                          {"accepted", true},
                          {"nativeProvider", true}};
            return SAO_AI_EDITOR_OK;
        }
        Json bridge_result;
        const int32_t bridge_status = dispatch_webview_message_to_extension(params, bridge_result);
        if (bridge_status != SAO_AI_EDITOR_OK) {
            emit("vscode.window.webviewPanel.postFailed",
                 Json{{"panelId", panel_id},
                      {"status", bridge_status},
                      {"reason", "extension host is not ready"}});
            result = Json{{"accepted", false},
                          {"message", "extension host is not ready"},
                          {"panelId", panel_id},
                          {"bridgeStatus", bridge_status}};
            return bridge_status;
        }
        if (!bridge_result.value("ok", false)) {
            result = Json{{"accepted", false},
                          {"message", "webview message was not delivered"},
                          {"panelId", panel_id},
                          {"bridgeResult", bridge_result}};
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        emit("vscode.window.webviewPanel.postMessage",
             Json{{"panelId", panel_id},
                  {"messageSeq", static_cast<int64_t>(state.message_seq)},
                  {"message", payload}});
        result = Json{{"panelId", panel_id},
                      {"messageSeq", static_cast<int64_t>(state.message_seq)},
                      {"accepted", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.setWebviewHtml") {
        const std::string panel_id = params.value("panelId", std::string{});
        const auto html = params.find("html");
        if (panel_id.empty() || html == params.end() || !html->is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (html->get_ref<const std::string&>().size() > kMaximumWebviewHtmlBytes) {
            result = Json{{"message", "webview HTML exceeds the native limit"},
                          {"panelId", panel_id},
                          {"maximumBytes", kMaximumWebviewHtmlBytes}};
            return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
        }
        WebviewPanelState state;
        const int32_t status =
            webview_panels_.set_html(panel_id, html->get_ref<const std::string&>(), state);
        if (status != SAO_AI_EDITOR_OK) {
            result = Json{{"message", "setWebviewHtml failed"}, {"panelId", panel_id}};
            return status;
        }
        emit("vscode.window.webviewPanel.htmlChanged",
             Json{{"panelId", panel_id}, {"htmlLength", static_cast<int64_t>(state.html.size())}});
        result = panel_state_to_json(state);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.getWebviewPanel") {
        const std::string requested = params.value("panelId", std::string{});
        if (requested.empty())
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        std::optional<WebviewPanelState> panel = webview_panels_.snapshot(requested);
        if (!panel) {
            for (const auto& candidate : webview_panels_.list_alive()) {
                if (candidate.view_type == requested) {
                    panel = candidate;
                    break;
                }
            }
        }
        if (!panel || panel->disposed)
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        if (panel->html.size() > kMaximumWebviewHtmlBytes)
            return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
        result = panel_state_to_json(*panel);
        result["html"] = panel->html;
        result["state"] = panel->initial_state;
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.setWebviewPanelViewState") {
        const std::string panel_id = params.value("panelId", std::string{});
        if (panel_id.empty() || !params.contains("active") || !params["active"].is_boolean() ||
            !params.contains("visible") || !params["visible"].is_boolean()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WebviewPanelState state;
        const int32_t status = webview_panels_.set_view_state(
            panel_id, params["active"].get<bool>(), params["visible"].get<bool>(),
            params.value("viewColumn", 1), state);
        if (status != SAO_AI_EDITOR_OK)
            return status;
        emit("vscode.window.webviewPanel.viewStateChanged", panel_state_to_json(state));
        result = panel_state_to_json(state);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.setWebviewState") {
        const std::string panel_id = params.value("panelId", std::string{});
        if (panel_id.empty() || !params.contains("state"))
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        WebviewPanelState state;
        const int32_t status = webview_panels_.set_state(panel_id, params["state"], state);
        if (status != SAO_AI_EDITOR_OK)
            return status;
        result = panel_state_to_json(state);
        result["state"] = state.initial_state;
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.listWebviewPanels") {
        Json array = Json::array();
        for (const auto& panel : webview_panels_.list_alive(WebviewPanelOwner::extension_host)) {
            array.push_back(panel_state_to_json(panel));
        }
        result = Json{{"panels", std::move(array)},
                      {"totalCreated", static_cast<int64_t>(webview_panels_.total_created(
                                           WebviewPanelOwner::extension_host))}};
        return SAO_AI_EDITOR_OK;
    }
    // vscode.commands.executeCommand loops back through the extension host
    // if a Node-side command was registered.
    if (method == "vscode.commands.executeCommand") {
        if (extension_host_ == nullptr) {
            return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        }
        return extension_host_->execute_command(params.value("command", std::string{}),
                                                params.value("arguments", Json::array()),
                                                params.value("timeoutMs", 15000U), result);
    }
    // vscode.languages.* — merged provider + language-configuration
    // registry (see extapi).
    if (method == "vscode.languages.getLanguages") {
        return extapi::dispatch(this, method, params, result);
    }
    // sao.host.* — direct pass-through to native runtime methods so the
    // extension shim can lean on SAO's own JSON-RPC surface without going
    // through the parent process.
    if (method == "sao.host.dispatch") {
        Json request{{"jsonrpc", "2.0"},
                     {"id", 1},
                     {"method", params.value("method", std::string{})},
                     {"params", params.value("params", Json::object())}};
        Json response;
        const int32_t status = dispatch(request, response);
        if (status != SAO_AI_EDITOR_OK) {
            result = Json{{"message", "dispatch failed"}};
            return status;
        }
        if (response.contains("error")) {
            result = response["error"];
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        result = response.value("result", Json::object());
        return SAO_AI_EDITOR_OK;
    }
    if (method == "sao.host.log") {
        emit("host.log", params);
        result = Json(nullptr);
        return SAO_AI_EDITOR_OK;
    }
    // Remaining vscode.* / sao.extapi.* calls land on the shared extapi
    // surface — clipboard, secrets, status bar, output channels,
    // language providers, diagnostics, documents, watchers, workspace
    // folders, fs.*, extensions, lm, chat, authentication, debug/tasks
    // registries.  Anything extapi does not implement returns
    // NOT_FOUND here and surfaces to the shim as a truthful failure.
    if (method.rfind("vscode.", 0) == 0 ||
        method.rfind("sao.extapi.", 0) == 0) {
        return extapi::dispatch(this, method, params, result);
    }
    result = Json{{"message", "unsupported extension method"}};
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::dispatch_webview_message_to_extension(const Json& params, Json& result) {
    if (extension_host_ == nullptr) {
        result = Json{{"accepted", false}, {"message", "extension host is not initialized"}};
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const int32_t status = extension_host_->post_webview_message(params, result);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    return result.value("ok", false) ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_IPC_CLOSED;
}

int32_t NativeRuntime::dispatch_webview_message_to_page(const Json& params, Json& result) {
    const std::string panel_id = params.value("panelId", std::string{});
    const std::string view_id = params.value("viewId", std::string{});
    if (panel_id.empty() && view_id.empty()) {
        result = Json{{"accepted", false}, {"message", "panelId or viewId is required"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    WebviewPostMessageHandler handler;
    {
        std::lock_guard<std::mutex> guard(webview_bridge_mutex_);
        handler = webview_post_message_handler_;
    }
    if (!handler || panel_id.empty()) {
        result = Json{{"accepted", false},
                      {"message", "webview bridge is not ready"},
                      {"panelId", panel_id},
                      {"viewId", view_id}};
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    const uint64_t message_seq = static_cast<uint64_t>(params.value("messageSeq", int64_t{0}));
    const bool accepted = handler(panel_id, message_seq, params.value("message", Json()));
    result = Json{{"accepted", accepted}, {"panelId", panel_id}, {"viewId", view_id}};
    return accepted ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_IPC_CLOSED;
}

int32_t NativeRuntime::dispatch_auth(std::string_view method, const Json& params, Json& result) {
    if (auth_flow_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    if (method == "auth.begin_device_flow") {
        DeviceFlowState state;
        const int32_t status = auth_flow_->begin(params, state);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"flowId", state.flow_id},
                      {"providerId", state.provider_id},
                      {"userCode", state.user_code},
                      {"verificationUri", state.verification_uri},
                      {"verificationUriComplete", state.verification_uri_complete},
                      {"intervalSeconds", state.interval_seconds},
                      {"expiresAtUnixMs", state.expires_at_unix_ms},
                      {"status", state.status}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "auth.poll_device_flow") {
        if (!params.contains("flowId") || !params["flowId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->poll(params["flowId"].get<std::string>(), result);
    }
    if (method == "auth.status_device_flow") {
        if (!params.contains("flowId") || !params["flowId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->status(params["flowId"].get<std::string>(), result);
    }
    if (method == "auth.cancel_device_flow") {
        if (!params.contains("flowId") || !params["flowId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->cancel(params["flowId"].get<std::string>(), result);
    }
    if (method == "auth.store_token") {
        if (!params.contains("providerId") || !params["providerId"].is_string() ||
            !params.contains("token") || !params["token"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->store_token(params["providerId"].get<std::string>(), params["token"],
                                       result);
    }
    if (method == "auth.load_token") {
        if (!params.contains("providerId") || !params["providerId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->load_token(params["providerId"].get<std::string>(), result);
    }
    if (method == "auth.revoke_token") {
        if (!params.contains("providerId") || !params["providerId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->revoke(params["providerId"].get<std::string>(), result);
    }
    if (method == "auth.refresh_token") {
        if (!params.contains("providerId") || !params["providerId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->refresh(params["providerId"].get<std::string>(), result);
    }
    if (method == "auth.get_access_token") {
        if (!params.contains("providerId") || !params["providerId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const int64_t leeway = params.value("expiryLeewaySeconds", int64_t{60});
        return auth_flow_->get_access_token(params["providerId"].get<std::string>(), leeway,
                                            result);
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::run_chat_sync(const Json& params, uint32_t timeout_ms,
                                     std::string& out_content,
                                     std::function<void(const Json&)> on_delta) {
    Json prepared_params;
    Json provider;
    std::string api_key;
    int32_t status = resolve_provider(params, prepared_params, provider, api_key);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const std::string model =
        prepared_params.value("model", provider.value("model", std::string{}));
    if (model.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json messages = prepared_params.value("messages", Json::array());
    if (!messages.is_array() || messages.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Force stream=true whenever the caller supplied an on_delta callback so
    // upper layers (workflow / agents.invoke) can observe intermediate tokens
    // without having to explicitly set stream in params.
    const bool stream = prepared_params.value("stream", false) || static_cast<bool>(on_delta);
    Json body;
    const int32_t body_status =
        build_openai_chat_body(prepared_params, model, std::move(messages), stream, body);
    if (body_status != SAO_AI_EDITOR_OK) {
        return body_status;
    }
    Json provider_with_key = provider;
    if (!api_key.empty()) {
        provider_with_key["apiKey"] = api_key;
    }
    ProviderRoute route = normalise_provider(provider_with_key, model);
    if (route.endpoint.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    ProviderRequest provider_request;
    const int32_t build_status = build_provider_request(route, body, provider_request);
    if (build_status != SAO_AI_EDITOR_OK) {
        return build_status;
    }
    HttpChatRequest request{};
    request.endpoint = provider_request.endpoint;
    request.api_key = route.api_key;
    request.authorization = provider_request.authorization;
    request.extra_headers = provider_request.extra_headers;
    request.request_json = provider_request.body_json;
    request.provider_type = route.type;
    request.timeout_ms = prepared_params.value("timeoutMs", timeout_ms);
    request.stream = stream;
    // Retry policy precedence: chat.run params.retry > provider.retry >
    // default (max_attempts=3).  run_chat_sync has no runId to attach
    // chat.retry events to, so retries are silent from the caller's view
    // but still honour the same backoff / status matrix as start_chat.
    if (prepared_params.contains("retry")) {
        request.retry = RetryPolicy::from_json(prepared_params["retry"]);
    } else if (provider.contains("retry")) {
        request.retry = RetryPolicy::from_json(provider["retry"]);
    }
    ChatCancellation cancellation;
    std::string streamed;
    Json transport_result;
    const int32_t chat_status = perform_openai_chat_with_retry(
        request, cancellation,
        [&](const Json& event) {
            if (event.value("type", "") == "delta" && event.contains("content") &&
                event["content"].is_string()) {
                streamed += event["content"].get<std::string>();
                if (on_delta) {
                    on_delta(event);
                }
            } else if (on_delta) {
                // Forward non-delta events (message_delta, tool_delta, done,
                // etc.) so the caller can observe stream completion / usage.
                on_delta(event);
            }
        },
        RetryNotifyCallback{}, // no runId available at this layer
        transport_result);
    if (chat_status != SAO_AI_EDITOR_OK) {
        return chat_status;
    }
    out_content =
        streamed.empty() ? transport_result.value("content", std::string{}) : std::move(streamed);
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::dispatch_workflow(std::string_view method, const Json& params,
                                         Json& result) {
    if (method == "workflows.reload") {
        std::lock_guard<std::mutex> guard(store_mutex_);
        workflow_registry_.reload(scopes_);
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.list_defs") {
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        Json items = Json::array();
        for (const auto& definition : workflow_registry_.list()) {
            items.push_back(definition.to_json());
        }
        result = Json{{"items", std::move(items)}};
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.get_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        WorkflowDefinition definition;
        if (!workflow_registry_.get(params["id"].get<std::string>(), definition)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        result = definition.to_json();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.save_def") {
        if (!params.contains("workflow") || !params["workflow"].is_object() ||
            !params.contains("scope") || !params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WorkflowDefinition definition = WorkflowDefinition::from_json(params["workflow"]);
        const std::string scope_key = params["scope"].get<std::string>();
        std::string scope;
        std::string plugin_id;
        if (scope_key == "system" || scope_key == "workspace") {
            scope = scope_key;
        } else if (scope_key.rfind("plugin:", 0) == 0) {
            scope = "plugin";
            plugin_id = scope_key.substr(7);
            if (!valid_simple_id(plugin_id)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        } else {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status = workflow_registry_.save(definition, scopes_, scope, plugin_id);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = definition.to_json();
        result["scope"] = scope_key;
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.delete_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status =
            workflow_registry_.remove(params["id"].get<std::string>(), scopes_,
                                      params.value("scope", "workspace"), std::string{});
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"ok", true}, {"id", params["id"]}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.run") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        WorkflowDefinition definition;
        if (!workflow_registry_.get(params["id"].get<std::string>(), definition)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        Json prepared_params;
        Json provider;
        std::string api_key;
        int32_t status = resolve_provider(params, prepared_params, provider, api_key);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        if (!api_key.empty()) {
            provider["apiKey"] = api_key;
        }
        const std::string model =
            prepared_params.value("model", provider.value("model", std::string{}));
        ProviderRoute route = normalise_provider(provider, model);
        if (model.empty() || route.endpoint.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        provider["endpoint"] = route.endpoint;
        provider["model"] = model;
        provider["type"] = route.type;
        const uint32_t chat_timeout = prepared_params.value("timeoutMs", 60'000U);
        status = apply_workflow_approval_policy(prepared_params, definition);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        Json input =
            params.contains("input") ? params["input"] : params.value("inputs", Json::object());
        static std::atomic<uint64_t> execution_counter{0};
        auto execution = std::make_shared<WorkflowExecution>(
            "wf-" + std::to_string(GetTickCount64()) + "-" +
                std::to_string(execution_counter.fetch_add(1, std::memory_order_relaxed)),
            std::move(definition), std::move(input));
        {
            std::lock_guard<std::mutex> guard(workflow_mutex_);
            workflow_executions_[execution->id()] = execution;
        }
        // Persist completed runs into the workspace history root, mirroring
        // the chat_history layout so users get a durable record across
        // process restarts.  ScopeStore always has a workspace scope
        // available here (initialize() ran before dispatch), so the empty
        // path branch that disables persistence is reserved for direct
        // WorkflowExecution consumers (i.e. unit tests).
        const std::filesystem::path history_dir = workflow_history_root(scopes_, "workspace");
        execution->start(*this, std::move(provider), model, chat_timeout, history_dir);
        result = Json{{"executionId", execution->id()}, {"status", "running"}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.status") {
        if (!params.contains("executionId") || !params["executionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::shared_ptr<WorkflowExecution> execution;
        {
            std::lock_guard<std::mutex> guard(workflow_mutex_);
            const auto found = workflow_executions_.find(params["executionId"].get<std::string>());
            if (found != workflow_executions_.end()) {
                execution = found->second;
            }
        }
        if (!execution) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        result = execution->snapshot();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.pause" || method == "workflows.resume" ||
        method == "workflows.cancel" || method == "workflows.confirm") {
        if (!params.contains("executionId") || !params["executionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::shared_ptr<WorkflowExecution> execution;
        {
            std::lock_guard<std::mutex> guard(workflow_mutex_);
            const auto found = workflow_executions_.find(params["executionId"].get<std::string>());
            if (found != workflow_executions_.end()) {
                execution = found->second;
            }
        }
        if (!execution) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (method == "workflows.pause") {
            execution->request_pause();
            result = Json{{"ok", true}};
            return SAO_AI_EDITOR_OK;
        }
        if (method == "workflows.resume") {
            execution->request_resume(params.value("humanInput", std::string{}));
            result = Json{{"ok", true}};
            return SAO_AI_EDITOR_OK;
        }
        if (method == "workflows.cancel") {
            execution->request_cancel();
            result = Json{{"ok", true}};
            return SAO_AI_EDITOR_OK;
        }
        // confirm
        const int32_t confirm_status = execution->confirm(params.value("stepId", std::string{}),
                                                          params.value("approved", false),
                                                          params.value("note", std::string{}));
        if (confirm_status != SAO_AI_EDITOR_OK) {
            return confirm_status;
        }
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.gc") {
        // Reap completed executions to bound memory.
        std::lock_guard<std::mutex> guard(workflow_mutex_);
        Json removed = Json::array();
        for (auto iterator = workflow_executions_.begin();
             iterator != workflow_executions_.end();) {
            const Json snapshot = iterator->second->snapshot();
            const std::string status = snapshot.value("status", "");
            if (status == "completed" || status == "cancelled" || status == "failed") {
                iterator->second->join();
                removed.push_back(iterator->first);
                iterator = workflow_executions_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        result = Json{{"removed", std::move(removed)}};
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::resolve_provider(const Json& params, Json& prepared_params, Json& provider,
                                        std::string& api_key) {
    if (secrets_ == nullptr) {
        return SAO_AI_EDITOR_ERR_CONFIG_MISSING;
    }
    {
        std::lock_guard<std::mutex> lock(store_mutex_);
        int32_t status = AiEditorSettings::prepare_chat_request(scopes_, *secrets_, params,
                                                                prepared_params, provider, api_key);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        RuntimePolicySnapshot policy;
        status = prepare_runtime_policy(scopes_, params, policy);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        prepared_params["mode"] = policy.effective_mode;
        prepared_params["approval"] = policy.effective_approval;
    }
    if (api_key.empty() && provider.contains("apiKeyEnv") && provider["apiKeyEnv"].is_string()) {
        api_key = environment_value(provider["apiKeyEnv"].get<std::string>());
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::start_chat(const Json& params, Json& result) {
    Json prepared_params;
    Json provider;
    std::string api_key;
    int32_t status = resolve_provider(params, prepared_params, provider, api_key);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const std::string model =
        prepared_params.value("model", provider.value("model", std::string{}));
    if (model.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json messages = prepared_params.value("messages", Json::array());
    const std::string conversation_id = prepared_params.value("conversationId", "");
    if (messages.empty() && !conversation_id.empty()) {
        Json conversation;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            status = conversations_.get(conversation_id, conversation);
        }
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        messages = conversation.value("messages", Json::array());
    }
    if (!messages.is_array() || messages.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const bool stream = prepared_params.value("stream", true);
    Json body;
    const int32_t body_status =
        build_openai_chat_body(prepared_params, model, std::move(messages), stream, body);
    if (body_status != SAO_AI_EDITOR_OK) {
        return body_status;
    }
    Json provider_with_key = provider;
    if (!api_key.empty()) {
        provider_with_key["apiKey"] = api_key;
    }
    ProviderRoute route = normalise_provider(provider_with_key, model);
    if (route.endpoint.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    ProviderRequest provider_request;
    const int32_t build_status = build_provider_request(route, body, provider_request);
    if (build_status != SAO_AI_EDITOR_OK) {
        return build_status;
    }
    HttpChatRequest request{};
    request.endpoint = provider_request.endpoint;
    request.api_key = route.api_key;
    request.authorization = provider_request.authorization;
    request.extra_headers = provider_request.extra_headers;
    request.request_json = provider_request.body_json;
    request.provider_type = route.type;
    request.timeout_ms = prepared_params.value("timeoutMs", 60'000U);
    request.stream = stream;
    // Retry policy precedence: chat.run params.retry > provider.retry >
    // default (max_attempts=3).  execute_chat picks this up and emits
    // chat.retry via the RetryNotifyCallback before each backoff sleep.
    if (prepared_params.contains("retry")) {
        request.retry = RetryPolicy::from_json(prepared_params["retry"]);
    } else if (provider.contains("retry")) {
        request.retry = RetryPolicy::from_json(provider["retry"]);
    }
    // Best-effort pricing lookup: the pricing map is keyed by
    // (provider_type, model), so we consult it here — winhttp_chat itself
    // stays free of the pricing map.  Missing entry leaves
    // `request.pricing_rule` empty which perform_openai_chat interprets as
    // `pricingApplied:false, costUsd:0`.
    resolve_pricing_rule(request.provider_type, model, request);
    auto run = std::make_shared<RunState>();
    run->id = new_run_id();
    run->cancellation = std::make_shared<ChatCancellation>();
    run->provider_type = request.provider_type;
    run->model = model;
    std::shared_ptr<RunState> stale_run;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++generation_;
        run->generation = generation_;
        if (!active_run_id_.empty()) {
            const auto active = runs_.find(active_run_id_);
            if (active != runs_.end() && active->second->status == "running") {
                stale_run = active->second;
                stale_run->status = "stale";
            }
        }
        active_run_id_ = run->id;
        runs_[run->id] = run;
    }
    if (stale_run) {
        stale_run->cancellation->cancel();
        emit("run.stale", Json{{"reason", "superseded"}}, stale_run->id);
    }
    try {
        run->worker = std::thread(&NativeRuntime::execute_chat, this, run, std::move(request),
                                  conversation_id);
    } catch (...) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        runs_.erase(run->id);
        if (active_run_id_ == run->id) {
            active_run_id_.clear();
        }
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    result = Json{{"accepted", true}, {"runId", run->id}, {"status", "running"}};
    emit("run.started", Json{{"model", model}, {"stream", stream}}, run->id);
    return SAO_AI_EDITOR_OK;
}

bool NativeRuntime::is_current(const std::shared_ptr<RunState>& run) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return run->generation == generation_ && run->status == "running";
}

void NativeRuntime::execute_chat(const std::shared_ptr<RunState>& run, HttpChatRequest request,
                                 std::string conversation_id) {
    try {
        Json transport_result;
        std::string streamed_content;
        const uint32_t max_attempts = std::max<uint32_t>(request.retry.max_attempts, 1);
        const int32_t status = perform_openai_chat_with_retry(
            request, *run->cancellation,
            [&](const Json& event) {
                if (!is_current(run)) {
                    return;
                }
                if (event.value("type", "") == "delta" && event.contains("content") &&
                    event["content"].is_string()) {
                    streamed_content += event["content"].get<std::string>();
                }
                emit("chat.delta", event, run->id);
            },
            [&](uint32_t attempt, uint32_t delay_ms, std::string_view reason) {
                // Fire chat.retry *before* the sleep so subscribers can start
                // their own timers.  Emitting even when the run has been
                // superseded is safe — is_current() gates chat.delta but the
                // retry event is a runtime-wide signal and we still want it
                // visible for observability dashboards.
                emit("chat.retry",
                     Json{{"runId", run->id},
                          {"attempt", attempt},
                          {"maxAttempts", max_attempts},
                          {"delayMs", delay_ms},
                          {"reason", std::string(reason)}},
                     run->id);
            },
            transport_result);

        std::string final_state;
        bool current = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            current = run->generation == generation_ && run->status == "running";
            if (run->status == "stale") {
                final_state = "stale";
            } else if (status == SAO_AI_EDITOR_ERR_CANCELLED || run->cancellation->cancelled()) {
                final_state = "cancelled";
                run->status = final_state;
            } else if (status == SAO_AI_EDITOR_OK) {
                final_state = "completed";
                run->status = final_state;
                run->result = transport_result;
                if (!streamed_content.empty()) {
                    run->result["content"] = streamed_content;
                }
            } else {
                final_state = "failed";
                run->status = final_state;
                run->result = transport_result;
                run->error = status_message(status);
            }
            if (active_run_id_ == run->id) {
                active_run_id_.clear();
            }
        }
        if (!current && final_state == "stale") {
            return;
        }
        if (final_state == "completed" && !conversation_id.empty()) {
            Json appended;
            const std::string content = !streamed_content.empty()
                                            ? streamed_content
                                            : transport_result.value("content", "");
            if (!content.empty()) {
                std::lock_guard<std::mutex> lock(store_mutex_);
                conversations_.append(conversation_id,
                                      Json{{"role", "assistant"}, {"content", content}}, appended);
            }
        }
        if (final_state == "completed") {
            // Surface latency/token-throughput separately from the completion
            // payload so callers that only care about metrics (dashboards,
            // logging) can subscribe to chat.metrics without pulling the full
            // run.completed body.  Missing metrics (e.g. transport short-circuit)
            // simply skip the emit — never blocks run.completed.
            if (run->result.is_object() && run->result.contains("metrics") &&
                run->result["metrics"].is_object()) {
                Json metrics_payload = run->result["metrics"];
                metrics_payload["runId"] = run->id;
                // Feed the runtime-wide accumulator so chat.cost_stats picks up
                // this run.  We accumulate regardless of whether a pricing rule
                // was applied — token counts stay useful even without cost.
                accumulate_cost_stats(run->provider_type, run->model, metrics_payload);
                emit("chat.metrics", metrics_payload, run->id);
            }
            emit("run.completed", run->result, run->id);
        } else if (final_state == "cancelled") {
            emit("run.cancelled", Json::object(), run->id);
        } else if (final_state == "failed") {
            emit("run.failed",
                 Json{{"status", status}, {"message", run->error}, {"details", run->result}},
                 run->id);
        }
    } catch (...) {
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            stale = run->status == "stale";
            if (!stale) {
                run->status = "failed";
                run->error = "worker exception";
            }
            if (active_run_id_ == run->id) {
                active_run_id_.clear();
            }
        }
        if (!stale) {
            try {
                emit("run.failed",
                     Json{{"status", SAO_AI_EDITOR_ERR_PROTOCOL}, {"message", "worker exception"}},
                     run->id);
            } catch (...) {
            }
        }
    }
}

int32_t NativeRuntime::cancel_run(const Json& params, Json& result) {
    const std::string id = params.value("runId", "");
    if (!valid_simple_id(id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::shared_ptr<RunState> run;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto found = runs_.find(id);
        if (found == runs_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        run = found->second;
        if (run->status == "running") {
            run->status = "cancelling";
        }
    }
    run->cancellation->cancel();
    result = Json{{"runId", id}, {"cancelRequested", true}};
    return SAO_AI_EDITOR_OK;
}

namespace {

int32_t collect_mcp_output(int32_t query_status, uint32_t required,
                           std::function<int32_t(char*, uint32_t, uint32_t*)> drain, Json& result) {
    if (query_status == SAO_AI_EDITOR_OK && required == 0) {
        result = Json::object();
        return SAO_AI_EDITOR_OK;
    }
    if (query_status != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        return query_status;
    }
    std::string buffer(static_cast<size_t>(required) + 1U, '\0');
    uint32_t written = 0;
    const int32_t drain_status =
        drain(buffer.data(), static_cast<uint32_t>(buffer.size()), &written);
    if (drain_status != SAO_AI_EDITOR_OK) {
        return drain_status;
    }
    result = Json::parse(buffer.data(), buffer.data() + written, nullptr, false);
    if (result.is_discarded()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    return SAO_AI_EDITOR_OK;
}

// Percent-encode a UTF-8 string for use inside a URI component.  RFC 3986
// unreserved set is left untouched; every other byte is upper-hex encoded.
// This matches the "reserved-safe" behaviour required by RFC 6570's simple
// (`{name}`) template expansion.
std::string percent_encode_component(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char byte : value) {
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                                byte == '.' || byte == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(byte));
        } else {
            static const char hex[] = "0123456789ABCDEF";
            encoded.push_back('%');
            encoded.push_back(hex[(byte >> 4) & 0x0FU]);
            encoded.push_back(hex[byte & 0x0FU]);
        }
    }
    return encoded;
}

// RFC 6570 reserved-expansion: keep gen-delims / sub-delims and any existing
// pct-triplet intact so `{+path}` inside `sao://workspace/{+path}` carries
// `foo/bar` as literal `foo/bar` rather than `foo%2Fbar`.  Everything outside
// the "reserved + unreserved + pct-encoded" set is still percent-encoded.
std::string percent_encode_reserved(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                                byte == '.' || byte == '~';
        // Reserved characters kept literal (RFC 3986 gen-delims + sub-delims).
        const bool reserved = byte == ':' || byte == '/' || byte == '?' || byte == '#' ||
                              byte == '[' || byte == ']' || byte == '@' || byte == '!' ||
                              byte == '$' || byte == '&' || byte == '\'' || byte == '(' ||
                              byte == ')' || byte == '*' || byte == '+' || byte == ',' ||
                              byte == ';' || byte == '=';
        // Preserve existing pct-triplets so already-encoded input is not
        // double-encoded when it round-trips through `{+var}`.
        if (byte == '%' && index + 2 < value.size() &&
            std::isxdigit(static_cast<unsigned char>(value[index + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[index + 2]))) {
            encoded.push_back('%');
            encoded.push_back(value[index + 1]);
            encoded.push_back(value[index + 2]);
            index += 2;
            continue;
        }
        if (unreserved || reserved) {
            encoded.push_back(static_cast<char>(byte));
        } else {
            static const char hex[] = "0123456789ABCDEF";
            encoded.push_back('%');
            encoded.push_back(hex[(byte >> 4) & 0x0FU]);
            encoded.push_back(hex[byte & 0x0FU]);
        }
    }
    return encoded;
}

// Extract the string form of any JSON scalar for URI templating.  Objects /
// arrays are stringified as JSON so callers get a deterministic (if ugly)
// value instead of the C++ default "true/false" surprise.
std::string uri_variable_value(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }
    if (value.is_number_float()) {
        return std::to_string(value.get<double>());
    }
    if (value.is_null()) {
        return {};
    }
    return value.dump();
}

// Minimal RFC 6570 URI-template renderer.  Supports the simple (`{var}`) and
// reserved (`{+var}`) operators, which is what MCP resource templates use in
// practice today.  Anything else (fragment `#`, path-segment `/`, form-style
// `?`, `.` prefixed, etc.) is left as-is so callers see the raw template and
// can spot bugs in their schema — better than silently rewriting `{?q}` to
// an empty string.
std::string render_uri_template(std::string_view templ, const Json& arguments) {
    std::string output;
    output.reserve(templ.size());
    size_t index = 0;
    while (index < templ.size()) {
        const char current = templ[index];
        if (current != '{') {
            output.push_back(current);
            ++index;
            continue;
        }
        const size_t close = templ.find('}', index + 1);
        if (close == std::string_view::npos) {
            // Unmatched `{` — preserve the remainder verbatim.
            output.append(templ.substr(index));
            break;
        }
        std::string_view expr = templ.substr(index + 1, close - index - 1);
        if (expr.empty()) {
            output.append(templ.substr(index, close - index + 1));
            index = close + 1;
            continue;
        }
        bool reserved_expansion = false;
        if (expr.front() == '+') {
            reserved_expansion = true;
            expr.remove_prefix(1);
        } else if (expr.front() == '#' || expr.front() == '/' || expr.front() == '.' ||
                   expr.front() == ';' || expr.front() == '?' || expr.front() == '&') {
            // Unsupported operator — preserve verbatim.
            output.append(templ.substr(index, close - index + 1));
            index = close + 1;
            continue;
        }
        const std::string name(expr);
        std::string value;
        if (arguments.is_object() && arguments.contains(name)) {
            value = uri_variable_value(arguments[name]);
        }
        output.append(reserved_expansion ? percent_encode_reserved(value)
                                         : percent_encode_component(value));
        index = close + 1;
    }
    return output;
}

int32_t render_resource_uri(const Json& params, Json& result) {
    if (!params.contains("template") || !params["template"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string templ = params["template"].get<std::string>();
    // `arguments` is optional — an absent / non-object payload is treated as
    // "no variables set", which produces the template with `{name}` expanded
    // to empty strings.  Matches RFC 6570 §3.2.1 behaviour for undefined vars.
    Json arguments = Json::object();
    if (params.contains("arguments") && params["arguments"].is_object()) {
        arguments = params["arguments"];
    }
    result = Json{{"uri", render_uri_template(templ, arguments)}};
    return SAO_AI_EDITOR_OK;
}

// Compose the "<provider>|<model>" key used by both pricing_rules_ and
// cost_stats_.  Anything missing collapses to an empty component (never
// synthesises a placeholder) so unknown-provider / unknown-model rows
// stay disambiguated on lookup.
std::string pricing_key(std::string_view provider, std::string_view model) {
    std::string key;
    key.reserve(provider.size() + 1 + model.size());
    key.append(provider);
    key.push_back('|');
    key.append(model);
    return key;
}

// Read a number-typed field off a metrics object, falling back to zero on
// missing / non-numeric so cost_stats accumulation stays defensive
// against partial provider payloads.
int64_t metrics_int64(const Json& metrics, const char* key) {
    const auto it = metrics.find(key);
    if (it == metrics.end() || !it->is_number()) {
        return 0;
    }
    if (it->is_number_integer()) {
        return it->get<int64_t>();
    }
    return static_cast<int64_t>(it->get<double>());
}

double metrics_double(const Json& metrics, const char* key) {
    const auto it = metrics.find(key);
    if (it == metrics.end() || !it->is_number()) {
        return 0.0;
    }
    return it->get<double>();
}

} // namespace

int32_t NativeRuntime::set_pricing(const Json& params, Json& result) {
    if (!params.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string provider = params.value("provider", std::string{});
    const std::string model = params.value("model", std::string{});
    if (provider.empty() || model.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json rule = Json::object();
    // Store the rule as a plain object with the two numeric fields we
    // recognise.  Extra caller-supplied fields are dropped so downstream
    // cost math has a stable, minimal shape.  At least one of the two
    // fields must parse as a number — an empty rule is rejected so
    // set_pricing never silently registers a no-op entry.
    bool has_any = false;
    if (params.contains("promptPer1K") && params["promptPer1K"].is_number()) {
        rule["promptPer1K"] = params["promptPer1K"].get<double>();
        has_any = true;
    }
    if (params.contains("completionPer1K") && params["completionPer1K"].is_number()) {
        rule["completionPer1K"] = params["completionPer1K"].get<double>();
        has_any = true;
    }
    if (!has_any) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string key = pricing_key(provider, model);
    {
        std::lock_guard<std::mutex> lock(pricing_mutex_);
        pricing_rules_[key] = rule;
    }
    result =
        Json{{"ok", true}, {"provider", provider}, {"model", model}, {"rule", std::move(rule)}};
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::get_pricing(const Json& params, Json& result) {
    if (!params.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string provider = params.value("provider", std::string{});
    const std::string model = params.value("model", std::string{});
    if (provider.empty() || model.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string key = pricing_key(provider, model);
    Json rule;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(pricing_mutex_);
        const auto it = pricing_rules_.find(key);
        if (it != pricing_rules_.end()) {
            rule = it->second;
            found = true;
        }
    }
    result = Json{{"provider", provider}, {"model", model}, {"found", found}};
    if (found) {
        result["rule"] = std::move(rule);
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::list_pricing(const Json& /*params*/, Json& result) {
    Json items = Json::array();
    {
        std::lock_guard<std::mutex> lock(pricing_mutex_);
        for (const auto& [key, rule] : pricing_rules_) {
            const auto pipe = key.find('|');
            if (pipe == std::string::npos) {
                continue;
            }
            items.push_back(Json{
                {"provider", key.substr(0, pipe)},
                {"model", key.substr(pipe + 1)},
                {"rule", rule},
            });
        }
    }
    result = Json{{"items", std::move(items)}};
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::cost_stats(const Json& params, Json& result) {
    // `days` is accepted for forward compatibility but the current
    // in-process accumulator has no time bucketing — restarting drops the
    // history.  We still surface the value on the response so callers can
    // sanity-check what they asked for.
    int64_t days = 7;
    if (params.is_object() && params.contains("days") && params["days"].is_number_integer()) {
        days = params["days"].get<int64_t>();
    }
    uint64_t total_requests = 0;
    int64_t total_prompt = 0;
    int64_t total_completion = 0;
    double total_cost = 0.0;
    Json by_model = Json::object();
    // Roll up per-provider aggregates from the same rows so callers can
    // slice by vendor without walking `byModel`.  Kept parallel to
    // byModel/CostStatsRow so both views stay lookup-compatible.
    struct ProviderAgg {
        uint64_t requests = 0;
        int64_t prompt_tokens = 0;
        int64_t completion_tokens = 0;
        double cost_usd = 0.0;
    };
    std::unordered_map<std::string, ProviderAgg> provider_totals;
    {
        std::lock_guard<std::mutex> lock(cost_stats_mutex_);
        for (const auto& [key, row] : cost_stats_) {
            total_requests += row.requests;
            total_prompt += row.prompt_tokens;
            total_completion += row.completion_tokens;
            total_cost += row.cost_usd;
            // Public key on the response uses the model name so callers can
            // slice quickly; the provider is stashed inside so lookups can
            // still disambiguate identical model names across vendors.
            Json entry{
                {"provider", row.provider},          {"requests", row.requests},
                {"promptTokens", row.prompt_tokens}, {"completionTokens", row.completion_tokens},
                {"costUsd", row.cost_usd},
            };
            by_model[row.model] = std::move(entry);
            auto& agg = provider_totals[row.provider];
            agg.requests += row.requests;
            agg.prompt_tokens += row.prompt_tokens;
            agg.completion_tokens += row.completion_tokens;
            agg.cost_usd += row.cost_usd;
        }
    }
    Json by_provider = Json::object();
    for (const auto& [provider, agg] : provider_totals) {
        by_provider[provider] = Json{
            {"requests", agg.requests},
            {"promptTokens", agg.prompt_tokens},
            {"completionTokens", agg.completion_tokens},
            {"costUsd", agg.cost_usd},
        };
    }
    Json average = Json::object();
    if (total_requests > 0) {
        average["promptTokens"] =
            static_cast<int64_t>(total_prompt / static_cast<int64_t>(total_requests));
        average["completionTokens"] =
            static_cast<int64_t>(total_completion / static_cast<int64_t>(total_requests));
        average["costUsd"] = total_cost / static_cast<double>(total_requests);
    } else {
        average["promptTokens"] = 0;
        average["completionTokens"] = 0;
        average["costUsd"] = 0.0;
    }
    result = Json{
        {"days", days},
        {"totalRequestsSampled", total_requests},
        {"totalPromptTokens", total_prompt},
        {"totalCompletionTokens", total_completion},
        {"totalCostUsd", total_cost},
        {"byModel", std::move(by_model)},
        {"byProvider", std::move(by_provider)},
        {"averagePerRequest", std::move(average)},
    };
    return SAO_AI_EDITOR_OK;
}

void NativeRuntime::resolve_pricing_rule(std::string_view provider_type, std::string_view model,
                                         HttpChatRequest& request) const {
    if (provider_type.empty() || model.empty()) {
        return;
    }
    const std::string key = pricing_key(provider_type, model);
    std::lock_guard<std::mutex> lock(pricing_mutex_);
    const auto it = pricing_rules_.find(key);
    if (it != pricing_rules_.end()) {
        request.pricing_rule = it->second;
    }
}

void NativeRuntime::accumulate_cost_stats(std::string_view provider_type, std::string_view model,
                                          const Json& metrics) {
    if (!metrics.is_object()) {
        return;
    }
    const int64_t prompt = metrics_int64(metrics, "promptTokens");
    const int64_t completion = metrics_int64(metrics, "completionTokens");
    const double cost = metrics_double(metrics, "costUsd");
    const std::string key = pricing_key(provider_type, model);
    std::lock_guard<std::mutex> lock(cost_stats_mutex_);
    auto& row = cost_stats_[key];
    if (row.model.empty()) {
        row.provider.assign(provider_type);
        row.model.assign(model);
    }
    ++row.requests;
    row.prompt_tokens += prompt;
    row.completion_tokens += completion;
    row.cost_usd += cost;
}

int32_t NativeRuntime::dispatch_mcp(std::string_view method, const Json& params, Json& result) {
    if (mcp_client_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    auto* raw = mcp_client_.get();
    if (method == "mcp.register_server") {
        if (!params.contains("name") || !params["name"].is_string() ||
            params["name"].get<std::string>().empty())
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        const std::string transport = params.value("transport", std::string{"stdio"});
        if (transport != "stdio" && transport != "http")
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        if (transport == "stdio" &&
            (!params.contains("command") || !params["command"].is_string() ||
             params["command"].get<std::string>().empty()))
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        if (params.contains("args") && !params["args"].is_array())
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        if (!params.value("builtin", false)) {
            Json settings;
            {
                std::lock_guard<std::mutex> lock(store_mutex_);
                if (scopes_.load_merged_config(settings) != SAO_AI_EDITOR_OK)
                    return SAO_AI_EDITOR_ERR_CONFIG_MISSING;
            }
            const Json mcp = settings.value("mcp", Json::object());
            const bool confirmed = params.contains("confirmed") &&
                                   params["confirmed"].is_boolean() &&
                                   params["confirmed"].get<bool>();
            const bool workspace_trusted = mcp.value("workspace_trusted", false);
            if (!workspace_trusted) {
                result = Json{{"error", "trust-required"},
                              {"trustRequired", true},
                              {"reason", "workspace trust required"},
                              {"name", params["name"]},
                              {"transport", transport},
                              {"workspaceTrusted", false}};
                return SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED;
            }
            if (!confirmed || !mcp.value("enabled", true) ||
                mcp.value("access", std::string{"prompt"}) == "disabled") {
                result = Json{{"confirmationRequired", true},
                              {"name", params["name"]},
                              {"transport", transport},
                              {"workspaceTrusted", true}};
                return SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED;
            }
        }
        const std::string config = dump_json(params);
        const int32_t status = sao_ai_editor_mcp_client_register(
            raw, config.data(), static_cast<uint32_t>(config.size()));
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"ok", true}, {"name", params.value("name", "")}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.list_servers") {
        uint32_t required = 0;
        const int32_t query = sao_ai_editor_mcp_client_list_servers(raw, nullptr, 0, &required);
        Json items;
        const int32_t status = collect_mcp_output(
            query, required,
            [raw](char* output, uint32_t capacity, uint32_t* out_length) {
                return sao_ai_editor_mcp_client_list_servers(raw, output, capacity, out_length);
            },
            items);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = redact_secret_fields(Json{{"items", items.is_array() ? items : Json::array()}});
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.list_tools" || method == "mcp.list_prompts" ||
        method == "mcp.list_resources") {
        auto invoker =
            method == "mcp.list_tools"
                ? &sao_ai_editor_mcp_client_list_tools
                : (method == "mcp.list_prompts" ? &sao_ai_editor_mcp_client_list_prompts
                                                : &sao_ai_editor_mcp_client_list_resources);
        uint32_t required = 0;
        const int32_t query = invoker(raw, nullptr, 0, &required);
        Json items;
        const int32_t status = collect_mcp_output(
            query, required,
            [raw, invoker](char* output, uint32_t capacity, uint32_t* out_length) {
                return invoker(raw, output, capacity, out_length);
            },
            items);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = redact_secret_fields(Json{{"items", items.is_array() ? items : Json::array()}});
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.call_tool" || method == "mcp.read_resource" || method == "mcp.get_prompt") {
        if (method == "mcp.call_tool") {
            uint32_t required = 0;
            const int32_t query = sao_ai_editor_mcp_client_list_tools(raw, nullptr, 0, &required);
            Json tools;
            const int32_t list_status = collect_mcp_output(
                query, required,
                [raw](char* output, uint32_t capacity, uint32_t* out_length) {
                    return sao_ai_editor_mcp_client_list_tools(raw, output, capacity, out_length);
                },
                tools);
            if (list_status != SAO_AI_EDITOR_OK) {
                return list_status;
            }
            const std::string server = params.value("server", std::string{});
            const std::string name = params.value("name", std::string{});
            bool requires_confirm = false;
            if (tools.is_array()) {
                for (const auto& tool : tools) {
                    if (tool.value("server", std::string{}) == server &&
                        tool.value("name", std::string{}) == name) {
                        requires_confirm = tool.value("requires_confirm", false);
                        break;
                    }
                }
            }
            if (requires_confirm && !params.value("confirmed", false)) {
                result = Json{{"confirmationRequired", true}, {"server", server}, {"name", name}};
                return SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED;
            }
        }
        if (params.contains("arguments") && !params["arguments"].is_object())
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        const std::string request = dump_json(params);
        auto invoker = method == "mcp.call_tool" ? &sao_ai_editor_mcp_client_call_tool
                                                 : (method == "mcp.read_resource"
                                                        ? &sao_ai_editor_mcp_client_read_resource
                                                        : &sao_ai_editor_mcp_client_get_prompt);
        uint32_t required = 0;
        int32_t query = invoker(raw, request.data(), static_cast<uint32_t>(request.size()), nullptr,
                                0, &required);
        if (query == SAO_AI_EDITOR_OK && required == 0) {
            result = Json::object();
            return SAO_AI_EDITOR_OK;
        }
        if (query != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
            return query;
        }
        std::string buffer(static_cast<size_t>(required) + 1U, '\0');
        uint32_t written = 0;
        const int32_t drain_status =
            invoker(raw, nullptr, 0, buffer.data(), static_cast<uint32_t>(buffer.size()), &written);
        if (drain_status != SAO_AI_EDITOR_OK) {
            return drain_status;
        }
        result = Json::parse(buffer.data(), buffer.data() + written, nullptr, false);
        if (result.is_discarded()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        result = redact_secret_fields(result);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.close_server") {
        if (!params.contains("name") || !params["name"].is_string() ||
            params["name"].get<std::string>().empty())
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        const std::string name = trim_copy(params["name"].get<std::string>());
        if (name.empty())
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        const int32_t status =
            sao_ai_editor_mcp_client_close(raw, name.empty() ? nullptr : name.c_str());
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"ok", true}, {"name", name}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.list_resource_templates") {
        // MCP `resources/templates/list` aggregation.  The underlying C client
        // does not yet expose a dedicated primitive for templates — surface a
        // well-formed but empty response so callers can adopt this dispatch
        // method today and start receiving real templates once the client
        // gains the primitive.  Down-stream code should treat an empty array
        // as "no template metadata available", not "no template exists".
        (void)raw;
        result = Json{{"items", Json::array()}, {"total", 0}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.render_resource_uri") {
        // Local-only URI template rendering.  No network / MCP round-trip —
        // pure string substitution against the caller-supplied `arguments`
        // map, so an offline UI can pre-compute the URI for `mcp.read_resource`.
        (void)raw;
        return render_resource_uri(params, result);
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::collect_mcp_openai_tools(const Json& mcp_server_filter, Json& out_tools) {
    out_tools = Json::array();
    if (mcp_client_ == nullptr) {
        // No MCP registry — treat as "no MCP tools available"; callers may
        // still layer extraTools on top.
        return SAO_AI_EDITOR_OK;
    }
    // Build a normalized filter set.  Empty filter (missing / null / empty
    // array) means "include tools from every registered server".
    std::vector<std::string> filter;
    bool filter_enabled = false;
    if (mcp_server_filter.is_array()) {
        for (const auto& entry : mcp_server_filter) {
            if (entry.is_string()) {
                const std::string name = entry.get<std::string>();
                if (!name.empty()) {
                    filter.push_back(name);
                    filter_enabled = true;
                }
            }
        }
    }
    Json tools_response;
    const int32_t status = dispatch_mcp("mcp.list_tools", Json::object(), tools_response);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    if (!tools_response.is_object() || !tools_response.contains("items") ||
        !tools_response["items"].is_array()) {
        return SAO_AI_EDITOR_OK;
    }
    for (const auto& item : tools_response["items"]) {
        if (!item.is_object()) {
            continue;
        }
        const std::string server = item.value("server", std::string{});
        const std::string tool_name = item.value("name", std::string{});
        if (server.empty() || tool_name.empty()) {
            continue;
        }
        if (filter_enabled) {
            if (std::find(filter.begin(), filter.end(), server) == filter.end()) {
                continue;
            }
        }
        Json function_body{{"name", "mcp__" + server + "__" + tool_name}};
        if (item.contains("description") && item["description"].is_string()) {
            function_body["description"] = item["description"];
        }
        Json parameters;
        if (item.contains("inputSchema") && item["inputSchema"].is_object()) {
            parameters = item["inputSchema"];
        } else {
            parameters = Json{{"type", "object"}, {"properties", Json::object()}};
        }
        function_body["parameters"] = std::move(parameters);
        out_tools.push_back(Json{{"type", "function"}, {"function", std::move(function_body)}});
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::apply_system_prompt_source(Json& params) {
    if (!params.is_object() || !params.contains("systemPromptSource")) {
        return SAO_AI_EDITOR_OK;
    }
    const Json source = params["systemPromptSource"];
    // Absent / null / empty-object systemPromptSource is a no-op — callers may
    // send the field unconditionally.  The runtime still strips it from the
    // forwarded params in the caller.
    if (!source.is_object() || source.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    if (!source.contains("server") || !source["server"].is_string() || !source.contains("name") ||
        !source["name"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (mcp_client_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json request{{"server", source["server"]}, {"name", source["name"]}};
    if (source.contains("arguments") && !source["arguments"].is_object())
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    if (source.contains("arguments")) {
        request["arguments"] = source["arguments"];
    } else {
        request["arguments"] = Json::object();
    }
    if (source.contains("timeoutMs") && source["timeoutMs"].is_number_integer()) {
        request["timeoutMs"] = source["timeoutMs"];
    }
    const std::string request_json = dump_json(request);
    auto* raw = mcp_client_.get();
    uint32_t required = 0;
    int32_t query = sao_ai_editor_mcp_client_get_prompt(raw, request_json.data(),
                                                        static_cast<uint32_t>(request_json.size()),
                                                        nullptr, 0, &required);
    Json prompt_result;
    if (query == SAO_AI_EDITOR_OK && required == 0) {
        prompt_result = Json::object();
    } else if (query != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        return query;
    } else {
        std::string buffer(static_cast<size_t>(required) + 1U, '\0');
        uint32_t written = 0;
        const int32_t drain_status = sao_ai_editor_mcp_client_get_prompt(
            raw, nullptr, 0, buffer.data(), static_cast<uint32_t>(buffer.size()), &written);
        if (drain_status != SAO_AI_EDITOR_OK) {
            return drain_status;
        }
        prompt_result = Json::parse(buffer.data(), buffer.data() + written, nullptr, false);
        if (prompt_result.is_discarded() || !prompt_result.is_object()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
    }
    // Normalize the MCP prompts/get shape into OpenAI-flavour chat messages.
    // MCP content is either a plain string, a single {type:"text",text:...}
    // object, or an array of such objects; downstream chat.run expects
    // {"role","content"} with content as a string.
    Json prepended = Json::array();
    bool prompt_has_system = false;
    if (prompt_result.contains("messages") && prompt_result["messages"].is_array()) {
        for (const auto& message : prompt_result["messages"]) {
            if (message.is_object() && message.value("role", std::string{}) == "system") {
                prompt_has_system = true;
                break;
            }
        }
    }
    const bool caller_has_system = params.contains("messages") && params["messages"].is_array() &&
                                   !params["messages"].empty() &&
                                   params["messages"][0].is_object() &&
                                   params["messages"][0].value("role", "") == "system";
    // Only surface the prompt description as a system header when neither
    // the prompt messages nor the caller already provide one — otherwise
    // it duplicates the intent of the existing system message.
    if (!caller_has_system && !prompt_has_system && prompt_result.contains("description") &&
        prompt_result["description"].is_string()) {
        const std::string description = prompt_result["description"].get<std::string>();
        if (!description.empty()) {
            prepended.push_back(Json{{"role", "system"}, {"content", description}});
        }
    }
    if (prompt_result.contains("messages") && prompt_result["messages"].is_array()) {
        for (const auto& message : prompt_result["messages"]) {
            if (!message.is_object()) {
                continue;
            }
            const std::string role = message.value("role", std::string{});
            if (role.empty()) {
                continue;
            }
            std::string text;
            const auto& content = message.contains("content") ? message["content"] : Json{};
            if (content.is_string()) {
                text = content.get<std::string>();
            } else if (content.is_object() && content.value("type", "") == "text" &&
                       content.contains("text") && content["text"].is_string()) {
                text = content["text"].get<std::string>();
            } else if (content.is_array()) {
                for (const auto& part : content) {
                    if (part.is_object() && part.value("type", "") == "text" &&
                        part.contains("text") && part["text"].is_string()) {
                        if (!text.empty()) {
                            text.push_back('\n');
                        }
                        text.append(part["text"].get<std::string>());
                    }
                }
            }
            if (text.empty()) {
                continue;
            }
            prepended.push_back(Json{{"role", role}, {"content", text}});
        }
    }
    if (!prepended.empty()) {
        Json existing = params.value("messages", Json::array());
        if (!existing.is_array()) {
            existing = Json::array();
        }
        for (const auto& message : existing) {
            prepended.push_back(message);
        }
        params["messages"] = std::move(prepended);
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::start_chat_with_mcp(const Json& params, Json& result) {
    if (!params.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json mcp_tools;
    const int32_t collect_status =
        collect_mcp_openai_tools(params.value("mcpServers", Json{}), mcp_tools);
    if (collect_status != SAO_AI_EDITOR_OK) {
        return collect_status;
    }
    // Build combined tools list: MCP tools first, then user-provided
    // extraTools.  Empty combined list -> do not set params.tools so the
    // request body stays identical to chat.run without tools.
    Json combined_tools = std::move(mcp_tools);
    if (params.contains("extraTools") && params["extraTools"].is_array()) {
        for (const auto& tool : params["extraTools"]) {
            combined_tools.push_back(tool);
        }
    }
    Json forwarded = params;
    // Render systemPromptSource (MCP prompts/get) into forwarded.messages
    // before stripping the source field.  On error we surface the failure
    // instead of silently dropping the source.
    const int32_t prompt_status = apply_system_prompt_source(forwarded);
    if (prompt_status != SAO_AI_EDITOR_OK) {
        return prompt_status;
    }
    // Also honour prompt-library references so callers can compose an MCP
    // prompt source with a stored template in the same request.
    const int32_t library_status = apply_prompt_source(forwarded);
    if (library_status != SAO_AI_EDITOR_OK) {
        return library_status;
    }
    forwarded.erase("mcpServers");
    forwarded.erase("extraTools");
    forwarded.erase("systemPromptSource");
    forwarded.erase("promptId");
    forwarded.erase("promptArguments");
    if (!combined_tools.empty()) {
        forwarded["tools"] = std::move(combined_tools);
    }
    return start_chat(forwarded, result);
}

int32_t NativeRuntime::agent_invoke_with_mcp(const Json& params, Json& result) {
    if (!params.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json mcp_tools;
    const int32_t collect_status =
        collect_mcp_openai_tools(params.value("mcpServers", Json{}), mcp_tools);
    if (collect_status != SAO_AI_EDITOR_OK) {
        return collect_status;
    }
    Json combined_tools = std::move(mcp_tools);
    if (params.contains("extraTools") && params["extraTools"].is_array()) {
        for (const auto& tool : params["extraTools"]) {
            combined_tools.push_back(tool);
        }
    }
    Json forwarded = params;
    const int32_t prompt_status = apply_system_prompt_source(forwarded);
    if (prompt_status != SAO_AI_EDITOR_OK) {
        return prompt_status;
    }
    forwarded.erase("mcpServers");
    forwarded.erase("extraTools");
    forwarded.erase("systemPromptSource");
    if (!combined_tools.empty()) {
        forwarded["tools"] = std::move(combined_tools);
    }
    return dispatch_agent("agents.invoke", forwarded, result);
}

int32_t NativeRuntime::run_status(const Json& params, Json& result) {
    const std::string id = params.value("runId", "");
    if (!valid_simple_id(id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto found = runs_.find(id);
    if (found == runs_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const auto& run = found->second;
    result = Json{
        {"runId", id}, {"status", run->status}, {"result", run->result}, {"error", run->error}};
    return SAO_AI_EDITOR_OK;
}

// ----------------------------------------------------------------------------
// extapi — process-global vscode.* extension surface
// ----------------------------------------------------------------------------
//
// Shared backend for the Node extension-host door (dispatch_extension_call
// falls through here for every vscode.* / sao.extapi.* method it does not
// own) and the standalone sao_ai_editor_vscode_shim_dispatch C ABI door.
// All registries live process-global so both doors resolve identical state.
// When a NativeRuntime is attached, extapi additionally gains:
//   - the runtime's ScopeStore/SecretStore roots (configure() rebinds the
//     lazily-built standalone stores to the same directories),
//   - sao.event mirroring through emit_extapi_event(),
//   - Node extension-host round-trips through the commands.execute tunnel
//     (internal commands sao.internal.extapi.event / .invoke),
//   - the chat pipeline for vscode.lm.sendRequest on native providers.
// Runtime-bound operations return SAO_AI_EDITOR_ERR_NOT_INITIALIZED when no
// runtime is attached.

namespace extapi {

constexpr size_t kEventQueueCapacity = 1024;
constexpr uint32_t kChannelBufferBytes = 256U * 1024U;
constexpr uint32_t kDocumentBytes = 16U * 1024U * 1024U;
constexpr size_t kMaximumProviders = 512;
constexpr size_t kMaximumDocuments = 512;
constexpr size_t kMaximumWatchers = 64;
constexpr size_t kMaximumWatcherBatchEvents = 512;
constexpr size_t kMaximumExtensionsInvoke = 32;
constexpr uint32_t kNodeInvokeTimeoutMs = 15000;
constexpr uint32_t kNodeEventTimeoutMs = 250;

struct OutputChannelRecord final {
    std::string id;
    std::string name;
    bool is_log = false;
    bool visible = false;
    std::string buffer;
    uint64_t dropped_bytes = 0;
};

struct DocumentRecord final {
    std::string uri;
    std::string fs_path;  // empty for untitled buffers
    std::string language_id;
    uint64_t version = 1;
    bool dirty = false;
    bool untitled = false;
    std::string content;
};

struct WatcherRecord final {
    uint64_t id = 0;
    std::string extension_id;
    std::string glob;
    std::string base;
    bool ignore_create = false;
    bool ignore_change = false;
    bool ignore_delete = false;
};

struct ExtapiState final {
    std::mutex mutex;
    NativeRuntime* runtime = nullptr;
    std::filesystem::path workspace_root;
    std::filesystem::path system_root;
    std::string plugin_roots_json;
    bool configured = false;

    std::unique_ptr<ScopeStore> standalone_scopes;
    std::unique_ptr<SecretStore> standalone_secrets;

    std::deque<Json> event_queue;
    uint64_t event_dropped = 0;
    uint64_t sequence = 1;

    std::map<std::string, Json, std::less<>> status_items;
    std::map<std::string, OutputChannelRecord, std::less<>> channels;
    std::map<std::string, DocumentRecord, std::less<>> documents;
    std::map<std::string, Json, std::less<>> language_providers;
    std::map<std::string, Json, std::less<>> language_configs;
    // collection key "<extensionId>::<name>" -> {diagnostics: {uri: [...]}}
    std::map<std::string, Json, std::less<>> diagnostic_collections;
    std::map<std::string, Json, std::less<>> auth_providers;
    std::map<std::string, Json, std::less<>> chat_participants;
    std::map<std::string, Json, std::less<>> lm_providers;
    std::map<std::string, Json, std::less<>> task_providers;
    std::map<std::string, Json, std::less<>> debug_providers;
    std::vector<WatcherRecord> watchers;
    std::string active_document_uri;

    HANDLE watch_dir = nullptr;
    HANDLE watch_stop = nullptr;
    std::thread watch_thread;
    std::atomic<bool> watch_running{false};
};

ExtapiState& state() {
    // Leaked-on-purpose singleton: destructor-time publishes must stay safe
    // even while runtime members are torn down.
    static ExtapiState* instance = new ExtapiState();
    return *instance;
}

int64_t extapi_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::filesystem::path default_system_root() {
    const char* profile_utf8 = std::getenv("USERPROFILE");
    if (profile_utf8 != nullptr && *profile_utf8 != '\0' &&
        valid_utf8(profile_utf8)) {
        const std::wstring wide = utf8_to_wide(profile_utf8);
        if (!wide.empty()) {
            return std::filesystem::path(wide) / L".sao";
        }
    }
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    return (error ? std::filesystem::path(L".") : current) / L".sao";
}

// Workspace-root lookup; empty path until configure() ran.
std::filesystem::path workspace_root() {
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().workspace_root;
}

// Store accessors — caller must hold state().mutex.
const ScopeStore* scope_store_locked() {
    auto& s = state();
    if (s.runtime != nullptr) {
        return &s.runtime->scope_store();
    }
    if (s.standalone_scopes == nullptr && s.configured) {
        auto store = std::make_unique<ScopeStore>();
        const std::string root_utf8 = wide_to_utf8(s.workspace_root.native());
        const std::string system_utf8 = wide_to_utf8(s.system_root.native());
        if (store->initialize(root_utf8, system_utf8, s.plugin_roots_json) ==
            SAO_AI_EDITOR_OK) {
            s.standalone_scopes = std::move(store);
        }
    }
    return s.standalone_scopes.get();
}

SecretStore* secret_store_locked() {
    auto& s = state();
    if (s.runtime != nullptr && s.runtime->secrets_store() != nullptr) {
        return s.runtime->secrets_store();
    }
    if (s.standalone_secrets == nullptr && !s.system_root.empty()) {
        s.standalone_secrets = std::make_unique<SecretStore>(
            s.system_root / L"secrets" / L"ai_editor.vault.json");
    }
    return s.standalone_secrets.get();
}

NativeRuntime* bound_runtime() {
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().runtime;
}

// Generic file-uri helpers — keep "file:///e:/dir/file" with the same
// UTF-8 passthrough the JS shim uses on Uri.file()/Uri.parse().
std::string file_uri_for(const std::filesystem::path& path) {
    std::string utf8 = wide_to_utf8(path.native());
    std::replace(utf8.begin(), utf8.end(), '\\', '/');
    std::string encoded;
    encoded.reserve(utf8.size() + 8);
    for (const char character : utf8) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte == '%' || byte == ' ' || byte == '#' || byte == '?') {
            char digits[4];
            std::snprintf(digits, sizeof(digits), "%%%02X", byte);
            encoded += digits;
        } else {
            encoded.push_back(character);
        }
    }
    if (!encoded.empty() && encoded.front() != '/') {
        encoded.insert(encoded.begin(), '/');
    }
    return "file://" + encoded;
}

// Decode a `file://` uri or a plain absolute/relative path into a UTF-8
// filesystem path string suitable for resolve_bounded_path().
std::string param_fs_path(const Json& params) {
    std::string raw;
    for (const char* key : {"path", "fsPath"}) {
        if (params.contains(key) && params[key].is_string()) {
            raw = params[key].get<std::string>();
            break;
        }
    }
    if (raw.empty() && params.contains("uri")) {
        if (params["uri"].is_string()) {
            raw = params["uri"].get<std::string>();
        } else if (params["uri"].is_object() &&
                   params["uri"].contains("fsPath") &&
                   params["uri"]["fsPath"].is_string()) {
            raw = params["uri"]["fsPath"].get<std::string>();
        }
    }
    if (raw.rfind("file://", 0) == 0) {
        raw = raw.substr(7);
        const size_t slash = raw.find('/');
        if (slash != std::string::npos) {
            raw = raw.substr(slash + 1);
        }
        std::string decoded;
        decoded.reserve(raw.size());
        for (size_t index = 0; index < raw.size(); ++index) {
            if (raw[index] == '%' && index + 2 < raw.size()) {
                const auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int high = hex(raw[index + 1]);
                const int low = hex(raw[index + 2]);
                if (high >= 0 && low >= 0) {
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    index += 2;
                    continue;
                }
            }
            decoded.push_back(raw[index]);
        }
        raw = std::move(decoded);
    }
    return raw;
}

// Resolve a caller-supplied fs path/uri to a workspace-contained canonical
// path.  Both absolute (inside the root) and relative inputs are accepted,
// matching the behaviour of resolve_bounded_path.
int32_t resolve_workspace(const Json& params, bool for_write,
                          std::filesystem::path& out) {
    const std::string raw = param_fs_path(params);
    if (raw.empty() || !valid_utf8(raw)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::filesystem::path root = workspace_root();
    if (root.empty()) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    if (!resolve_bounded_path(root, raw, for_write, out)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t resolve_workspace_uri(std::string_view uri_utf8, bool for_write,
                              std::filesystem::path& out) {
    Json params;
    params["uri"] = std::string(uri_utf8);
    return resolve_workspace(params, for_write, out);
}

// Best-effort push of one extapi event into the live Node shim via the
// commands.execute tunnel (internal command sao.internal.extapi.event).
// Callback-guard contention degrades silently: the drain queue stays the
// authoritative channel.
void push_event_to_node(NativeRuntime* runtime, const Json& event) {
    if (runtime == nullptr) {
        return;
    }
    Json arguments = Json::array({event});
    Json sink;
    (void)runtime->invoke_extension_command("sao.internal.extapi.event", arguments,
                                            kNodeEventTimeoutMs, sink);
}

// Runtime-bound node round-trip for provider invocations; must be called
// WITHOUT state().mutex held (the Node callback may reenter extapi).
int32_t invoke_extension(NativeRuntime* runtime, std::string_view target,
                         const Json& body, Json& out) {
    if (runtime == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json request = body;
    request["invoke"] = target;
    Json arguments = Json::array({request});
    return runtime->invoke_extension_command("sao.internal.extapi.invoke", arguments,
                                             kNodeInvokeTimeoutMs, out);
}

void publish(std::string_view kind, const Json& payload) {
    Json event;
    try {
        event = Json{{"type", "extapi.event"},
                     {"kind", std::string(kind)},
                     {"ts", extapi_now_ms()},
                     {"payload", payload}};
    } catch (...) {
        return;
    }
    NativeRuntime* runtime;
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.event_queue.size() >= kEventQueueCapacity) {
            s.event_queue.pop_front();
            ++s.event_dropped;
        }
        s.event_queue.push_back(event);
        runtime = s.runtime;
    }
    if (runtime != nullptr) {
        runtime->emit_extapi_event(std::string("vscode.extapi.") +
                                       std::string(kind),
                                   payload);
        push_event_to_node(runtime, event);
    }
}

// --- clipboard (real Win32) ----------------------------------------------

int32_t clipboard_read(Json& result) {
    if (::OpenClipboard(nullptr) == FALSE) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    std::string text;
    HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
    if (handle != nullptr) {
        if (const wchar_t* raw =
                static_cast<const wchar_t*>(::GlobalLock(handle))) {
            text = wide_to_utf8(raw);
            ::GlobalUnlock(handle);
        }
    }
    ::CloseClipboard();
    result = Json{{"text", text}};
    return SAO_AI_EDITOR_OK;
}

int32_t clipboard_write(const Json& params, Json& result) {
    const std::string text = params.value("text", std::string{});
    if (!valid_utf8(text) || text.size() > kMaximumJsonBytes) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::wstring wide = utf8_to_wide(text);
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if (void* target = ::GlobalLock(memory)) {
        std::memcpy(target, wide.c_str(), bytes);
        ::GlobalUnlock(memory);
    } else {
        ::GlobalFree(memory);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    // Retry loop: another process may hold the clipboard briefly.
    bool opened = false;
    for (int attempt = 0; attempt < 10 && !opened; ++attempt) {
        opened = ::OpenClipboard(nullptr) == TRUE;
        if (!opened) ::Sleep(5);
    }
    if (!opened) {
        ::GlobalFree(memory);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if (::EmptyClipboard() == FALSE ||
        ::SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    // SetClipboardData owns `memory` on success.
    ::CloseClipboard();
    result = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

// --- env.openExternal (real ShellExecuteW) ---------------------------------

int32_t open_external(const Json& params, Json& result) {
    const std::string uri = params.value("uri", std::string{});
    const size_t scheme_end = uri.find("://");
    if (uri.empty() || uri.size() > 2048 || !valid_utf8(uri)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string scheme =
        scheme_end == std::string::npos ? std::string{} : uri.substr(0, scheme_end);
    const auto allowed = {"http", "https", "mailto", "file"};
    if (std::find(allowed.begin(), allowed.end(), scheme) == allowed.end()) {
        result = Json{{"ok", false}, {"error", "uri scheme is not allowed"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::wstring wide = utf8_to_wide(uri);
    const HINSTANCE handle =
        ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(handle) <= 32) {
        result = Json{{"ok", false},
                      {"error", "ShellExecuteW failed"},
                      {"code", static_cast<int64_t>(
                                   reinterpret_cast<intptr_t>(handle))}};
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    result = Json{{"ok", true}, {"uri", uri}};
    return SAO_AI_EDITOR_OK;
}

// --- secrets ---------------------------------------------------------------

std::string secret_key_for(const std::string& extension_id,
                           const std::string& key) {
    return "ext/" + extension_id + "/" + key;
}

int32_t secrets_store(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string key = params.value("key", std::string{});
    const std::string value = params.value("value", std::string{});
    if (!valid_simple_id(extension_id) || key.empty() ||
        !valid_utf8(key) || key.size() > 256 ||
        value.size() > kMaximumJsonBytes || !valid_utf8(value)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    SecretStore* store;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        store = secret_store_locked();
    }
    if (store == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const int32_t status = store->set(secret_key_for(extension_id, key), value);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    publish("secrets", Json{{"op", "set"},
                            {"extensionId", extension_id},
                            {"key", key}});
    result = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

int32_t secrets_get(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string key = params.value("key", std::string{});
    if (!valid_simple_id(extension_id) || key.empty() || !valid_utf8(key) ||
        key.size() > 256) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    SecretStore* store;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        store = secret_store_locked();
    }
    if (store == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    std::string value;
    const int32_t status = store->get(secret_key_for(extension_id, key), value);
    result = Json{{"found", status == SAO_AI_EDITOR_OK}};
    if (status == SAO_AI_EDITOR_OK) {
        result["value"] = value;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t secrets_delete(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string key = params.value("key", std::string{});
    if (!valid_simple_id(extension_id) || key.empty() || !valid_utf8(key)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    SecretStore* store;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        store = secret_store_locked();
    }
    if (store == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const int32_t status = store->erase(secret_key_for(extension_id, key));
    if (status == SAO_AI_EDITOR_OK) {
        publish("secrets", Json{{"op", "delete"},
                                {"extensionId", extension_id},
                                {"key", key}});
    }
    result = Json{{"ok", status == SAO_AI_EDITOR_OK}};
    return status;
}

// --- environmentVariableCollection ------------------------------------------
//
// Persisted per extension as registry items in directory "envvars" (scope
// "workspace" or "system" for global).  Each item keeps
// {id:ext-<sanitized>, values:{VAR:{type,value}}} where type is the VS Code
// mutator enum: 1=replace, 2=append, 3=prepend, 4=delete.

std::string env_item_id(const std::string& extension_id, std::string_view scope) {
    return "ext-" + extension_id + "." + std::string(scope);
}

int32_t env_vars_load(const ScopeStore* scopes, const std::string& extension_id,
                      std::string_view scope, Json& item) {
    Json registry;
    const int32_t status = scopes->load_registry("envvars", Json::array(), registry);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const std::string wanted = env_item_id(extension_id, scope);
    item = Json{{"id", wanted}, {"values", Json::object()}};
    if (registry.is_array()) {
        for (const auto& entry : registry) {
            if (entry.is_object() && entry.value("id", std::string{}) == wanted) {
                item = entry;
                if (!item.contains("values") || !item["values"].is_object()) {
                    item["values"] = Json::object();
                }
                break;
            }
        }
    }
    return SAO_AI_EDITOR_OK;
}

int type_of_op(const std::string& op) {
    if (op == "replace") return 1;
    if (op == "append") return 2;
    if (op == "prepend") return 3;
    if (op == "delete") return 4;
    return 0;
}

int32_t env_vars_apply(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string scope_name =
        params.value("scope", std::string{"workspace"}) == "global" ? "system" : "workspace";
    if (!valid_simple_id(extension_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const ScopeStore* scopes;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        scopes = scope_store_locked();
    }
    if (scopes == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json item;
    const int32_t load_status = env_vars_load(scopes, extension_id, scope_name, item);
    if (load_status != SAO_AI_EDITOR_OK) {
        return load_status;
    }
    Json& values = item["values"];
    Json mutations = params.value("mutations", Json::array());
    if (!mutations.is_array()) {
        mutations = Json::array({Json{{"op", params.value("op", std::string{})},
                                     {"variable", params.value("variable", std::string{})},
                                     {"value", params.value("value", std::string{})}}});
    }
    for (const auto& mutation : mutations) {
        if (!mutation.is_object()) {
            continue;
        }
        const std::string op = mutation.value("op", std::string{});
        if (op == "clear") {
            values = Json::object();
            continue;
        }
        const std::string variable = mutation.value("variable", std::string{});
        if (variable.empty() || variable.size() > 512 ||
            variable.find('=') != std::string::npos || !valid_utf8(variable)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const int type = type_of_op(op);
        if (type == 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (type == 4) {
            values[variable] = Json{{"type", 4}, {"value", std::string{}}};
        } else {
            const std::string value = mutation.value("value", std::string{});
            if (value.size() > 8192 || !valid_utf8(value)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            values[variable] = Json{{"type", type}, {"value", value}};
        }
    }
    const int32_t save_status = scopes->save_registry_item(
        "envvars", scope_name, "", env_item_id(extension_id, scope_name), item);
    if (save_status != SAO_AI_EDITOR_OK) {
        return save_status;
    }
    result = Json{{"ok", true}, {"entries", values}};
    return SAO_AI_EDITOR_OK;
}

int32_t env_vars_snapshot(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const ScopeStore* scopes;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        scopes = scope_store_locked();
    }
    if (scopes == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json registry;
    const int32_t status = scopes->load_registry("envvars", Json::array(), registry);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    Json merged = Json::object();
    if (registry.is_array()) {
        std::vector<Json> items(registry.begin(), registry.end());
        std::sort(items.begin(), items.end(), [](const Json& left, const Json& right) {
            return left.value("id", std::string{}) < right.value("id", std::string{});
        });
        for (const auto& entry : items) {
            if (!extension_id.empty() &&
                entry.value("id", std::string{})
                        .rfind("ext-" + extension_id + ".", 0) != 0) {
                continue;
            }
            const Json values = entry.value("values", Json::object());
            if (values.is_object()) {
                for (const auto& [variable, op] : values.items()) {
                    merged[variable] = op;
                }
            }
        }
    }
    result = Json{{"entries", merged}};
    return SAO_AI_EDITOR_OK;
}

int32_t apply_environment_overrides(
    std::vector<std::pair<std::string, std::string>>& environment) {
    const ScopeStore* scopes;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        scopes = scope_store_locked();
    }
    if (scopes == nullptr) {
        // No store configured → nothing declared; the ambient parent block
        // remains authoritative.
        return SAO_AI_EDITOR_OK;
    }
    Json registry;
    const int32_t status = scopes->load_registry("envvars", Json::array(), registry);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    if (!registry.is_array()) {
        return SAO_AI_EDITOR_OK;
    }
    // Deterministic application order: extension id, then variable name.
    std::vector<Json> items(registry.begin(), registry.end());
    std::sort(items.begin(), items.end(), [](const Json& left, const Json& right) {
        return left.value("id", std::string{}) < right.value("id", std::string{});
    });
    auto ci_less = [](const std::string& left, const std::string& right) {
        const std::string lhs = [&left] {
            std::string upper = left;
            for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return upper;
        }();
        const std::string rhs = [&right] {
            std::string upper = right;
            for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return upper;
        }();
        return lhs < rhs;
    };
    std::map<std::string, std::string, decltype(ci_less)> resolved(ci_less);
    for (const auto& [name, value] : environment) {
        resolved[name] = value;
    }
    const auto ambient = [](const std::string& name) -> std::string {
        const std::wstring wide = utf8_to_wide(name);
        wchar_t buffer[32767];
        const DWORD count = ::GetEnvironmentVariableW(wide.c_str(), buffer,
                                                      sizeof(buffer) / sizeof(wchar_t));
        return count == 0 || count >= sizeof(buffer) / sizeof(wchar_t)
                   ? std::string{}
                   : wide_to_utf8(std::wstring(buffer, count));
    };
    for (const auto& entry : items) {
        const Json values = entry.value("values", Json::object());
        if (!values.is_object()) {
            continue;
        }
        std::vector<std::string> variables;
        for (const auto& [variable, op] : values.items()) {
            variables.push_back(variable);
        }
        std::sort(variables.begin(), variables.end());
        for (const auto& variable : variables) {
            const Json& op = values[variable];
            const int type = op.value("type", 0);
            const std::string value = op.value("value", std::string{});
            const std::string base = resolved.count(variable)
                                         ? resolved[variable]
                                         : ambient(variable);
            switch (type) {
                case 1:
                    resolved[variable] = value;
                    break;
                case 2:
                    resolved[variable] = base.empty() ? value : base + ";" + value;
                    break;
                case 3:
                    resolved[variable] = base.empty() ? value : value + ";" + base;
                    break;
                case 4:
                    // BootOptions.environment only supports set semantics —
                    // the closest faithful behaviour is an empty value so the
                    // spawned extension host never sees a stale inherited value.
                    resolved[variable] = std::string{};
                    break;
                default:
                    break;
            }
        }
    }
    environment.clear();
    environment.reserve(resolved.size());
    for (const auto& [name, value] : resolved) {
        environment.emplace_back(name, value);
    }
    return SAO_AI_EDITOR_OK;
}

// --- ExtensionContext memento ----------------------------------------------
//
// Per-extension persisted state: registry "extstate" items named
// "<extensionId>.<scope>" holding {entries:{key:json}}.

std::string state_item_id(const std::string& extension_id,
                          const std::string& normalized_scope);

int32_t state_load(const ScopeStore* scopes, const std::string& extension_id,
                   std::string_view scope, Json& item) {
    Json registry;
    const int32_t status = scopes->load_registry("extstate", Json::array(), registry);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const std::string wanted =
        state_item_id(extension_id, std::string(scope));
    item = Json{{"id", wanted}, {"entries", Json::object()}};
    if (registry.is_array()) {
        for (const auto& entry : registry) {
            if (entry.is_object() && entry.value("id", std::string{}) == wanted) {
                item = entry;
                if (!item.contains("entries") || !item["entries"].is_object()) {
                    item["entries"] = Json::object();
                }
                break;
            }
        }
    }
    return SAO_AI_EDITOR_OK;
}

std::string normalize_state_scope(std::string_view scope) {
    if (scope == "global" || scope == "system") return "system";
    if (scope.rfind("plugin:", 0) == 0 &&
        valid_simple_id(scope.substr(7))) {
        return std::string(scope);
    }
    return "workspace";
}

// Registry-safe item id for extension state.  `plugin:` scopes contain ':'
// which is outside the valid_simple_id charset, so the item id uses the
// flattened "<extensionId>.plugin-<pluginId>" form.
std::string state_item_id(const std::string& extension_id,
                          const std::string& normalized_scope) {
    if (normalized_scope.rfind("plugin:", 0) == 0) {
        return extension_id + ".plugin-" + normalized_scope.substr(7);
    }
    return extension_id + "." + normalized_scope;
}

bool split_state_scope(const std::string& scope, std::string& out_scope,
                       std::string& out_plugin) {
    if (scope == "system" || scope == "workspace") {
        out_scope = scope;
        out_plugin.clear();
        return true;
    }
    if (scope.rfind("plugin:", 0) == 0) {
        out_scope = "plugin";
        out_plugin = scope.substr(7);
        return true;
    }
    return false;
}

int32_t state_list(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    if (!valid_simple_id(extension_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const ScopeStore* scopes;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        scopes = scope_store_locked();
    }
    if (scopes == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json item;
    const int32_t status =
        state_load(scopes, extension_id,
                   normalize_state_scope(params.value("scope", std::string{"workspace"})),
                   item);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result = Json{{"entries", item["entries"]}};
    return SAO_AI_EDITOR_OK;
}

int32_t state_update(const Json& params, Json& result, bool clear_all) {
    const std::string extension_id = params.value("extensionId", std::string{});
    if (!valid_simple_id(extension_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const ScopeStore* scopes;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        scopes = scope_store_locked();
    }
    if (scopes == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const std::string normalized =
        normalize_state_scope(params.value("scope", std::string{"workspace"}));
    std::string scope;
    std::string plugin_id;
    if (!split_state_scope(normalized, scope, plugin_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json item;
    const int32_t status = state_load(scopes, extension_id, normalized, item);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    if (clear_all) {
        item["entries"] = Json::object();
    } else {
        const std::string key = params.value("key", std::string{});
        if (key.empty() || key.size() > 1024 || !valid_utf8(key)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (params.contains("value") && !params["value"].is_null()) {
            item["entries"][key] = params["value"];
        } else {
            item["entries"].erase(key);
        }
    }
    const int32_t save_status =
        scopes->save_registry_item("extstate", scope, plugin_id,
                                   state_item_id(extension_id, normalized),
                                   item);
    if (save_status != SAO_AI_EDITOR_OK) {
        return save_status;
    }
    result = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

// --- status bar ---------------------------------------------------------------

Json status_item_json(const Json& record) {
    return record;
}

int32_t status_bar_create(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    std::string item_id = params.value("itemId", std::string{});
    if (!extension_id.empty() && !valid_simple_id(extension_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json record;
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.status_items.size() >= kMaximumProviders) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (item_id.empty()) {
            item_id = "status-" + std::to_string(s.sequence++);
        }
        if (!valid_simple_id(item_id) || s.status_items.count(item_id) != 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const int alignment = params.value("alignment", 1);
        record = Json{
            {"itemId", item_id},
            {"extensionId", extension_id},
            {"name", params.value("name", std::string{})},
            {"text", params.value("text", std::string{})},
            {"tooltip", params.value("tooltip", std::string{})},
            {"alignment", alignment == 2 ? 2 : 1},
            {"priority", params.value("priority", 0)},
            {"color", params.value("color", std::string{})},
            {"backgroundColor", params.value("backgroundColor", std::string{})},
            {"command", params.value("command", Json(nullptr))},
            {"accessibilityInformation",
             params.value("accessibilityInformation", Json(nullptr))},
            {"visible", false},
        };
        s.status_items[item_id] = record;
    }
    publish("statusBarItem", Json{{"op", "create"}, {"item", record}});
    result = record;
    return SAO_AI_EDITOR_OK;
}

int32_t status_bar_update(const Json& params, Json& result) {
    const std::string item_id = params.value("itemId", std::string{});
    if (item_id.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    auto& s = state();
    Json snapshot;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto found = s.status_items.find(item_id);
        if (found == s.status_items.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        Json& record = found->second;
        for (const char* key : {"text", "tooltip", "color", "backgroundColor", "name"}) {
            if (params.contains(key)) {
                record[key] = params[key];
            }
        }
        for (const char* key : {"command", "accessibilityInformation"}) {
            if (params.contains(key)) {
                record[key] = params[key];
            }
        }
        if (params.contains("visible") && params["visible"].is_boolean()) {
            record["visible"] = params["visible"].get<bool>();
        }
        if (params.contains("priority") && params["priority"].is_number()) {
            record["priority"] = params["priority"].get<int64_t>();
        }
        if (params.contains("alignment") && params["alignment"].is_number()) {
            const int64_t alignment = params["alignment"].get<int64_t>();
            record["alignment"] = alignment == 2 ? 2 : 1;
        }
        snapshot = record;
    }
    publish("statusBarItem",
            Json{{"op", "update"}, {"itemId", item_id}, {"item", snapshot}});
    result = snapshot;
    return SAO_AI_EDITOR_OK;
}

int32_t status_bar_dispose(const Json& params, Json& result) {
    const std::string item_id = params.value("itemId", std::string{});
    if (item_id.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    auto& s = state();
    Json record;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto found = s.status_items.find(item_id);
        if (found == s.status_items.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        record = std::move(found->second);
        s.status_items.erase(found);
    }
    publish("statusBarItem",
            Json{{"op", "dispose"}, {"itemId", item_id}, {"item", record}});
    result = Json{{"ok", true}, {"itemId", item_id}};
    return SAO_AI_EDITOR_OK;
}

// --- output channels ----------------------------------------------------------
//
// Real rolling-buffer store (256 KiB tail, drop-oldest) so the real
// vscode.window.createOutputChannel / append / show / clear surface works
// without a UI; the workbench reads via vscode.window.readOutputChannel and
// every mutation lands in the event queue as kind "outputChannel".

OutputChannelRecord* channel_lookup(const Json& params) {
    auto& s = state();
    const std::string id = params.value("channelId", std::string{});
    if (!id.empty()) {
        const auto found = s.channels.find(id);
        return found == s.channels.end() ? nullptr : &found->second;
    }
    const std::string name = params.value("name", std::string{});
    for (auto& [key, channel] : s.channels) {
        (void)key;
        if (channel.name == name) {
            return &channel;
        }
    }
    return nullptr;
}

int32_t channel_create(const Json& params, Json& result) {
    const std::string name = params.value("name", std::string{});
    if (name.empty() || name.size() > 256 || !valid_utf8(name)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::string id;
    bool is_log = false;
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (OutputChannelRecord* existing = channel_lookup(Json{{"name", name}})) {
            result = Json{{"channelId", existing->id}, {"name", existing->name}};
            return SAO_AI_EDITOR_OK;
        }
        id = "outc-" + std::to_string(s.sequence++);
        OutputChannelRecord record;
        record.id = id;
        record.name = name;
        record.is_log = params.value("log", false);
        is_log = record.is_log;
        s.channels[id] = record;
    }
    publish("outputChannel",
            Json{{"op", "create"}, {"channelId", id}, {"name", name}, {"log", is_log}});
    result = Json{{"channelId", id}, {"name", name}};
    return SAO_AI_EDITOR_OK;
}

int32_t channel_append(const Json& params, Json& result) {
    const std::string text = params.value("text", std::string{});
    if (!valid_utf8(text) || text.size() > kDocumentBytes) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    NativeRuntime* runtime;
    std::string channel_id;
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        OutputChannelRecord* channel = channel_lookup(params);
        if (channel == nullptr) {
            // Auto-create a durable channel keyed by the id/name given — the
            // append path must never drop output silently.
            const std::string fallback = params.value("channelId",
                                                      params.value("name", std::string{"default"}));
            channel = &s.channels[fallback];
            channel->id = fallback;
            channel->name = params.value("name", fallback);
        }
        channel_id = channel->id;
        if (channel->buffer.size() + text.size() > kChannelBufferBytes) {
            const size_t overflow =
                channel->buffer.size() + text.size() - kChannelBufferBytes;
            size_t cut = overflow;
            const size_t newline = channel->buffer.find('\n', overflow);
            if (newline != std::string::npos) {
                cut = newline + 1;
            }
            if (cut >= channel->buffer.size()) {
                cut = channel->buffer.size();
            }
            channel->dropped_bytes += cut;
            channel->buffer.erase(0, cut);
        }
        channel->buffer += text;
        if (s.channels.size() > 256) {
            // Bound the channel map itself (should never trigger in
            // practice; drop-oldest on map order keeps the store finite).
            s.channels.erase(s.channels.begin());
        }
        runtime = s.runtime;
    }
    publish("outputChannel", Json{{"op", "write"},
                                  {"channelId", channel_id},
                                  {"bytes", static_cast<int64_t>(text.size())}});
    if (runtime != nullptr) {
        // Legacy event name kept: existing consumers subscribe to
        // vscode.window.output for streamed output.
        runtime->emit_extapi_event(
            "vscode.window.output",
            Json{{"channelId", channel_id}, {"text", text}});
    }
    result = Json(nullptr);
    return SAO_AI_EDITOR_OK;
}

int32_t channel_op(const Json& params, Json& result) {
    const std::string op = params.value("op", std::string{});
    std::string id;
    std::string name;
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        OutputChannelRecord* channel = channel_lookup(params);
        if (channel == nullptr) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        id = channel->id;
        name = channel->name;
        if (op == "show") {
            channel->visible = true;
        } else if (op == "hide") {
            channel->visible = false;
        } else if (op == "clear") {
            channel->dropped_bytes += channel->buffer.size();
            channel->buffer.clear();
        } else if (op == "replace") {
            const std::string text = params.value("text", std::string{});
            if (!valid_utf8(text) || text.size() > kChannelBufferBytes) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            channel->buffer = text;
        } else if (op == "dispose") {
            s.channels.erase(id);
        } else {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }
    publish("outputChannel", Json{{"op", op}, {"channelId", id}, {"name", name}});
    result = Json{{"ok", true}, {"channelId", id}};
    return SAO_AI_EDITOR_OK;
}

int32_t channel_read(const Json& params, Json& result) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    OutputChannelRecord* channel = channel_lookup(params);
    if (channel == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    result = Json{{"channelId", channel->id},
                  {"name", channel->name},
                  {"content", channel->buffer},
                  {"droppedBytes", channel->dropped_bytes}};
    return SAO_AI_EDITOR_OK;
}

int32_t channel_list(Json& result) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Json items = Json::array();
    for (const auto& [id, channel] : s.channels) {
        items.push_back(Json{{"channelId", id},
                             {"name", channel.name},
                             {"bytes", static_cast<int64_t>(channel.buffer.size())},
                             {"droppedBytes", channel.dropped_bytes},
                             {"visible", channel.visible},
                             {"log", channel.is_log}});
    }
    result = Json{{"channels", items}};
    return SAO_AI_EDITOR_OK;
}

// --- language providers -------------------------------------------------------
//
// Provider registrations are real registry entries; invocation round-trips
// through the extension host (internal invoke channel) so the JS-side
// provider object actually runs.

std::map<std::string, std::string> provider_kind_methods() {
    return {{"codelens", "provideCodeLenses"},
            {"completion", "provideCompletionItems"},
            {"declaration", "provideDeclaration"},
            {"definition", "provideDefinition"},
            {"documentColors", "provideDocumentColors"},
            {"documentDropEdit", "provideDocumentDropEdits"},
            {"documentFormatting", "provideDocumentFormattingEdits"},
            {"documentRangeFormatting", "provideDocumentRangeFormattingEdits"},
            {"documentSymbol", "provideDocumentSymbols"},
            {"foldingRange", "provideFoldingRanges"},
            {"hover", "provideHover"},
            {"implementation", "provideImplementation"},
            {"inlineCompletion", "provideInlineCompletionItems"},
            {"inlineValues", "provideInlineValues"},
            {"linkedEditingRange", "provideLinkedEditingRanges"},
            {"onTypeFormatting", "provideOnTypeFormattingEdits"},
            {"references", "provideReferences"},
            {"rename", "provideRenameEdits"},
            {"selectionRange", "provideSelectionRanges"},
            {"semanticTokens", "provideDocumentSemanticTokens"},
            {"signatureHelp", "provideSignatureHelp"},
            {"typeDefinition", "provideTypeDefinition"},
            {"typeHierarchy", "prepareTypeHierarchy"},
            {"callHierarchy", "prepareCallHierarchy"},
            {"inlayHints", "provideInlayHints"}};
}

int32_t language_register_provider(NativeRuntime* runtime, const Json& params,
                                   Json& result) {
    const std::string kind = params.value("kind", std::string{});
    const auto methods = provider_kind_methods();
    if (methods.find(kind) == methods.end()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string extension_id = params.value("extensionId", std::string{});
    if (!extension_id.empty() && !valid_simple_id(extension_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::string provider_id = params.value("providerId", std::string{});
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.language_providers.size() >= kMaximumProviders) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (provider_id.empty()) {
            provider_id = "lp-" + std::to_string(s.sequence++);
        }
        if (!valid_simple_id(provider_id) ||
            s.language_providers.count(provider_id) != 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        s.language_providers[provider_id] = Json{
            {"providerId", provider_id},
            {"extensionId", extension_id},
            {"kind", kind},
            {"invoke", methods.at(kind)},
            {"selector", params.value("selector", Json(nullptr))},
            {"metadata", params.value("metadata", Json::object())}};
    }
    (void)runtime;
    publish("providers",
            Json{{"op", "register"}, {"kind", kind}, {"providerId", provider_id}});
    result = Json{{"providerId", provider_id}, {"kind", kind}};
    return SAO_AI_EDITOR_OK;
}

int32_t language_unregister_provider(const Json& params, Json& result) {
    const std::string provider_id = params.value("providerId", std::string{});
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        if (state().language_providers.erase(provider_id) == 0) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
    }
    publish("providers",
            Json{{"op", "unregister"}, {"providerId", provider_id}});
    result = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

int32_t language_list_providers(const Json& params, Json& result) {
    const std::string kind = params.value("kind", std::string{});
    std::lock_guard<std::mutex> lock(state().mutex);
    Json items = Json::array();
    for (const auto& [id, provider] : state().language_providers) {
        if (!kind.empty() && provider.value("kind", std::string{}) != kind) {
            continue;
        }
        items.push_back(provider);
    }
    result = Json{{"providers", items}};
    return SAO_AI_EDITOR_OK;
}

// selector matching: shim pre-computes `languageId`/`uri`; here we accept an
// explicit `selector` subset {language,scheme,pattern} — matches when the
// language field equals the document's languageId (or is absent).
bool provider_selector_matches(const Json& selector, const std::string& language_id,
                               const std::string& scheme) {
    if (!selector.is_object()) {
        return !selector.is_null();  // absent selector → match anything
    }
    if (selector.contains("language") && selector["language"].is_string()) {
        if (selector["language"].get<std::string>() != language_id) {
            return false;
        }
    }
    if (selector.contains("scheme") && selector["scheme"].is_string()) {
        if (selector["scheme"].get<std::string>() != scheme) {
            return false;
        }
    }
    return true;
}

int32_t language_invoke(NativeRuntime* runtime, const Json& params, Json& result) {
    const std::string kind = params.value("kind", std::string{});
    if (kind.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::vector<Json> providers;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        for (const auto& [id, provider] : state().language_providers) {
            if (provider.value("kind", std::string{}) != kind) {
                continue;
            }
            if (params.contains("providerId") && params["providerId"].is_string() &&
                provider.value("providerId", std::string{}) !=
                    params["providerId"].get<std::string>()) {
                continue;
            }
            providers.push_back(provider);
            if (providers.size() >= kMaximumExtensionsInvoke) {
                break;
            }
        }
    }
    if (providers.empty()) {
        result = Json{{"results", Json::array()}, {"total", 0}};
        return SAO_AI_EDITOR_OK;
    }
    const std::string language_id = params.value("languageId", std::string{});
    const std::string scheme = params.value("scheme", std::string{"file"});
    Json request{{"kind", kind},
                 {"uri", params.value("uri", Json(nullptr))},
                 {"languageId", language_id},
                 {"position", params.value("position", Json(nullptr))},
                 {"context", params.value("context", Json::object())}};
    if (params.contains("document") && params["document"].is_object()) {
        request["document"] = params["document"];
    }
    Json results = Json::array();
    for (const auto& provider : providers) {
        if (!provider_selector_matches(provider.value("selector", Json(nullptr)),
                                       language_id, scheme)) {
            continue;
        }
        request["providerId"] = provider.value("providerId", std::string{});
        Json out;
        const int32_t status =
            invoke_extension(runtime, "languages", request, out);
        if (status == SAO_AI_EDITOR_OK) {
            Json entry{{"providerId", provider.value("providerId", std::string{})},
                       {"status", "ok"},
                       {"result", out.value("result", out)}};
            results.push_back(std::move(entry));
        } else {
            results.push_back(Json{{"providerId",
                                    provider.value("providerId", std::string{})},
                                   {"status", "error"},
                                   {"code", status},
                                   {"message", "provider invocation failed"}});
        }
    }
    result = Json{{"results", results}, {"total", results.size()}};
    return SAO_AI_EDITOR_OK;
}

int32_t language_set_configuration(const Json& params, Json& result) {
    const std::string language = params.value("language", std::string{});
    if (language.empty() || language.size() > 64 || !valid_utf8(language) ||
        !params.contains("configuration")) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().language_configs[language] = params["configuration"];
    }
    publish("languageConfig",
            Json{{"op", "set"}, {"language", language}});
    result = Json{{"ok", true}, {"language", language}};
    return SAO_AI_EDITOR_OK;
}

// --- diagnostics --------------------------------------------------------------

std::string diagnostics_key(const std::string& extension_id,
                            const std::string& name) {
    return extension_id + "::" + name;
}

int32_t diagnostics_set(NativeRuntime* runtime, const Json& params, Json& result) {
    (void)runtime;
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string name = params.value("collection", params.value("name", std::string{}));
    if (!valid_simple_id(extension_id) || name.empty() || name.size() > 256) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string key = diagnostics_key(extension_id, name);
    const std::string uri = params.value("uri", std::string{});
    auto& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        Json& collection = s.diagnostic_collections[key];
        if (!collection.is_object()) {
            collection = Json{{"name", name},
                              {"extensionId", extension_id},
                              {"entries", Json::object()}};
        }
        if (uri.empty()) {
            // uri omitted + diagnostics array → replace whole collection;
            // uri omitted + no diagnostics → clear.
            collection["entries"] = Json::object();
        } else if (params.contains("diagnostics") &&
                   params["diagnostics"].is_array() &&
                   !params["diagnostics"].empty()) {
            collection["entries"][uri] = params["diagnostics"];
        } else {
            collection["entries"].erase(uri);
        }
    }
    publish("diagnostics", Json{{"collection", name},
                                {"extensionId", extension_id},
                                {"uri", uri},
                                {"op", uri.empty() ? "clear" : "set"},
                                {"uris",
                                 uri.empty() ? Json::array() : Json::array({uri})}});
    result = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

int32_t diagnostics_get(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string collection_name =
        params.value("collection", std::string{});
    const std::string uri = params.value("uri", std::string{});
    std::lock_guard<std::mutex> lock(state().mutex);
    Json items = Json::array();
    for (const auto& [key, collection] : state().diagnostic_collections) {
        if (!collection_name.empty() &&
            collection.value("name", std::string{}) != collection_name) {
            continue;
        }
        if (!extension_id.empty() &&
            collection.value("extensionId", std::string{}) != extension_id) {
            continue;
        }
        const Json entries = collection.value("entries", Json::object());
        if (!entries.is_object()) {
            continue;
        }
        if (!uri.empty()) {
            const auto found = entries.find(uri);
            if (found != entries.end()) {
                items.push_back(Json{{"uri", uri}, {"diagnostics", *found}});
            }
            continue;
        }
        for (const auto& [path_uri, diagnostics] : entries.items()) {
            items.push_back(Json{{"uri", path_uri}, {"diagnostics", diagnostics}});
        }
    }
    result = Json{{"items", items}, {"total", items.size()}};
    return SAO_AI_EDITOR_OK;
}

int32_t diagnostics_dispose(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string name = params.value("collection", params.value("name", std::string{}));
    const std::string key = diagnostics_key(extension_id, name);
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().diagnostic_collections.erase(key);
    }
    publish("diagnostics", Json{{"collection", name},
                                {"extensionId", extension_id},
                                {"op", "dispose"}});
    result = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

// --- text documents ----------------------------------------------------------
//
// Real document store: openTextDocument materialises a tracked copy (UTF-8,
// 16 MiB cap, workspace containment), edits apply in UTF-16 code-unit
// semantics (matching VS Code position encoding), dirty buffers persist
// only through saveTextDocument.  Untracked applyEdit edits write straight
// to disk through the same bounded writers used by tools.

std::string guess_language_id(const std::string& file_path) {
    const std::filesystem::path path(utf8_to_wide(file_path));
    const std::string ext = wide_to_utf8(path.extension().native());
    static const std::map<std::string, std::string, std::less<>> table = {
        {".js", "javascript"},  {".mjs", "javascript"}, {".cjs", "javascript"},
        {".ts", "typescript"},  {".jsx", "javascriptreact"},
        {".tsx", "typescriptreact"},
        {".py", "python"},      {".go", "go"},
        {".rs", "rust"},        {".cpp", "cpp"},
        {".cc", "cpp"},         {".h", "cpp"},
        {".hpp", "cpp"},        {".cs", "csharp"},
        {".json", "json"},      {".md", "markdown"},
        {".html", "html"},      {".css", "css"},
    };
    const auto found = table.find(ext);
    return found == table.end() ? "plaintext" : found->second;
}

Json document_to_json(const DocumentRecord& record, bool include_content) {
    Json doc{{"uri", record.uri},
             {"fsPath", record.fs_path},
             {"languageId", record.language_id},
             {"version", record.version},
             {"dirty", record.dirty},
             {"untitled", record.untitled},
             {"isClosed", false}};
    if (include_content) {
        doc["content"] = record.content;
    }
    return doc;
}

int32_t document_open(NativeRuntime* runtime, const Json& params, Json& result) {
    (void)runtime;
    const std::string explicit_uri = params.value("uri", std::string{});
    DocumentRecord record;
    if (explicit_uri.rfind("untitled:", 0) == 0 ||
        params.value("untitled", false)) {
        {
            auto& s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.documents.size() >= kMaximumDocuments) {
                return SAO_AI_EDITOR_ERR_BUSY;
            }
            record.uri = explicit_uri.empty()
                             ? "untitled://untitled-" + std::to_string(s.sequence++)
                             : explicit_uri;
            record.untitled = true;
            record.language_id =
                params.value("languageId", std::string{"plaintext"});
            record.content = params.value("content", std::string{});
            if (record.content.size() > kDocumentBytes || !valid_utf8(record.content)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            s.documents[record.uri] = record;
        }
        publish("document", Json{{"op", "open"}, {"uri", record.uri}});
        result = document_to_json(record, true);
        return SAO_AI_EDITOR_OK;
    }
    std::filesystem::path resolved;
    const int32_t path_status = resolve_workspace(params, false, resolved);
    if (path_status != SAO_AI_EDITOR_OK) {
        return path_status;
    }
    const std::string uri = file_uri_for(resolved);
    auto& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto found = s.documents.find(uri);
        if (found != s.documents.end()) {
            result = document_to_json(found->second, true);
            return SAO_AI_EDITOR_OK;
        }
        if (s.documents.size() >= kMaximumDocuments) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
    }
    std::string content;
    {
        const std::filesystem::path root = workspace_root();
        const int32_t read_status =
            read_text_file_bounded(root, resolved, kDocumentBytes, content);
        if (read_status != SAO_AI_EDITOR_OK) {
            return read_status;
        }
    }
    record.uri = uri;
    record.fs_path = wide_to_utf8(resolved.native());
    record.language_id = guess_language_id(record.fs_path);
    record.content = std::move(content);
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.documents[record.uri] = record;
    }
    publish("document", Json{{"op", "open"},
                             {"uri", record.uri},
                             {"languageId", record.language_id}});
    result = document_to_json(record, true);
    return SAO_AI_EDITOR_OK;
}

int32_t document_close(const Json& params, Json& result) {
    const std::string uri = params.value("uri", std::string{});
    if (uri.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.documents.erase(uri) == 0) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (s.active_document_uri == uri) {
            s.active_document_uri.clear();
        }
    }
    publish("document", Json{{"op", "close"}, {"uri", uri}});
    result = Json{{"ok", true}, {"uri", uri}};
    return SAO_AI_EDITOR_OK;
}

int32_t document_save(const Json& params, Json& result) {
    const std::string uri = params.value("uri", std::string{});
    if (uri.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (params.contains("content") && !params["content"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    DocumentRecord snapshot;
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto found = s.documents.find(uri);
        if (found == s.documents.end() || found->second.untitled) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        found->second.content = params.value("content", found->second.content);
        if (found->second.content.size() > kDocumentBytes ||
            !valid_utf8(found->second.content)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        snapshot = found->second;
    }
    std::filesystem::path resolved;
    const int32_t path_status = resolve_workspace_uri(uri, true, resolved);
    if (path_status != SAO_AI_EDITOR_OK) {
        return path_status;
    }
    const std::filesystem::path root = workspace_root();
    const int32_t write_status =
        write_text_atomic_bounded(root, resolved, snapshot.content);
    if (write_status != SAO_AI_EDITOR_OK) {
        return write_status;
    }
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        auto& doc = s.documents[uri];
        doc.dirty = false;
        ++doc.version;
        snapshot.version = doc.version;
    }
    publish("document", Json{{"op", "save"}, {"uri", uri}});
    result = Json{{"ok", true}, {"uri", uri}, {"version", snapshot.version}};
    return SAO_AI_EDITOR_OK;
}

int32_t document_list(Json& result) {
    std::lock_guard<std::mutex> lock(state().mutex);
    Json items = Json::array();
    for (const auto& [uri, record] : state().documents) {
        (void)uri;
        items.push_back(document_to_json(record, false));
    }
    result = Json{{"documents", items}, {"total", items.size()}};
    return SAO_AI_EDITOR_OK;
}

// Position encoding: UTF-16 code units — identical to VS Code's Range
// semantics, so a JS-side Range round-trips exactly.
int32_t utf16_offset(const std::wstring& wide, int64_t line, int64_t character,
                     size_t& offset) {
    if (line < 0 || character < 0) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    size_t cursor = 0;
    int64_t current_line = 0;
    while (current_line < line) {
        const size_t newline = wide.find(L'\n', cursor);
        if (newline == std::wstring::npos) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        cursor = newline + 1;
        ++current_line;
    }
    size_t line_end = wide.find(L'\n', cursor);
    if (line_end == std::wstring::npos) {
        line_end = wide.size();
    }
    const size_t line_length = line_end - cursor;
    offset = cursor + std::min<size_t>(static_cast<size_t>(character), line_length);
    return SAO_AI_EDITOR_OK;
}

int32_t apply_edits_to_text(const std::string& content, const Json& edits,
                            std::string& out_content, Json& changes) {
    if (!edits.is_array() || edits.size() > 4096) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::wstring wide = utf8_to_wide(content);
    std::wstring working = wide;
    struct Edit {
        size_t begin = 0;
        size_t end = 0;
        std::string replacement;
        Json range;
    };
    std::vector<Edit> resolved;
    resolved.reserve(edits.size());
    for (const auto& edit : edits) {
        if (!edit.is_object() || !edit.contains("range") ||
            !edit["range"].is_object() || !edit.contains("newText") ||
            !edit["newText"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const Json& range = edit["range"];
        const Json& start = range.value("start", Json::object());
        const Json& end = range.value("end", Json::object());
        const int64_t start_line = start.value("line", INT64_C(-1));
        const int64_t start_char = start.value("character", INT64_C(-1));
        const int64_t end_line = end.value("line", start_line);
        const int64_t end_char = end.value("character", start_char);
        Edit one;
        one.replacement = edit["newText"].get<std::string>();
        if (one.replacement.size() > kDocumentBytes ||
            !valid_utf8(one.replacement)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        one.range = range;
        const int32_t begin_status =
            utf16_offset(wide, start_line, start_char, one.begin);
        if (begin_status != SAO_AI_EDITOR_OK) {
            return begin_status;
        }
        const int32_t end_status = utf16_offset(wide, end_line, end_char, one.end);
        if (end_status != SAO_AI_EDITOR_OK) {
            return end_status;
        }
        if (one.end < one.begin) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        resolved.push_back(std::move(one));
    }
    std::sort(resolved.begin(), resolved.end(),
              [](const Edit& left, const Edit& right) {
                  return left.begin > right.begin;
              });
    changes = Json::array();
    for (const auto& edit : resolved) {
        const std::wstring replacement = utf8_to_wide(edit.replacement);
        working.replace(edit.begin, edit.end - edit.begin, replacement);
        changes.push_back(Json{{"range", edit.range},
                               {"rangeOffset", edit.begin},
                               {"rangeLength", edit.end - edit.begin},
                               {"text", edit.replacement}});
    }
    out_content = wide_to_utf8(working);
    if (out_content.size() > kDocumentBytes) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    std::reverse(changes.begin(), changes.end());  // restore document order
    return SAO_AI_EDITOR_OK;
}

int32_t document_apply_edits(const Json& params, Json& result) {
    const std::string uri = params.value("uri", std::string{});
    const Json edits = params.value("edits", Json::array());
    if (uri.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::string out_content;
    Json changes;
    DocumentRecord updated;
    bool tracked = false;
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto found = s.documents.find(uri);
        tracked = found != s.documents.end();
        if (tracked) {
            const int32_t status = apply_edits_to_text(found->second.content, edits,
                                                       out_content, changes);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            found->second.content = out_content;
            found->second.dirty = true;
            ++found->second.version;
            updated = found->second;
        }
    }
    if (!tracked) {
        std::filesystem::path resolved;
        const int32_t path_status = resolve_workspace_uri(uri, false, resolved);
        if (path_status != SAO_AI_EDITOR_OK) {
            return path_status;
        }
        const std::filesystem::path root = workspace_root();
        std::string content;
        const int32_t read_status =
            read_text_file_bounded(root, resolved, kDocumentBytes, content);
        if (read_status != SAO_AI_EDITOR_OK) {
            return read_status;
        }
        const int32_t status =
            apply_edits_to_text(content, edits, out_content, changes);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        const int32_t write_status =
            write_text_atomic_bounded(root, resolved, out_content);
        if (write_status != SAO_AI_EDITOR_OK) {
            return write_status;
        }
    }
    publish("document", Json{{"op", "change"},
                             {"uri", uri},
                             {"changes", changes},
                             {"persisted", !tracked}});
    if (tracked) {
        result = Json{{"applied", true},
                      {"uri", uri},
                      {"version", updated.version},
                      {"contentChanges", changes}};
    } else {
        result = Json{{"applied", true}, {"uri", uri}, {"persisted", true},
                      {"contentChanges", changes}};
    }
    return SAO_AI_EDITOR_OK;
}

int32_t workspace_apply_edit(const Json& params, Json& result) {
    int applied_document_edits = 0;
    int applied_file_operations = 0;
    Json failures = Json::array();
    // File operations first (VS Code applies them before document edits).
    for (const auto& operation : params.value("fileOperations", Json::array())) {
        if (!operation.is_object()) {
            continue;
        }
        const std::string op = operation.value("op", std::string{});
        int32_t status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        if (op == "create") {
            Json create_params = operation;
            std::filesystem::path target;
            status = resolve_workspace(create_params, true, target);
            if (status == SAO_AI_EDITOR_OK) {
                const bool overwrite = operation.value("overwrite", false);
                const std::string content =
                    operation.value("content", std::string{});
                if (content.size() > kDocumentBytes || !valid_utf8(content)) {
                    status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                } else if (overwrite) {
                    status = write_text_atomic_bounded(workspace_root(), target,
                                                       content);
                } else {
                    status = create_text_atomic_bounded(workspace_root(), target,
                                                        content);
                }
            }
            if (status == SAO_AI_EDITOR_OK) {
                publish("fileEvent", Json{{"op", "created"},
                                          {"uri", file_uri_for(target)},
                                          {"path",
                                           wide_to_utf8(target.native())}});
            }
        } else if (op == "rename") {
            std::filesystem::path from;
            std::filesystem::path to;
            Json from_params = operation;
            from_params["path"] = operation.value("from", std::string{});
            status = resolve_workspace(from_params, false, from);
            if (status == SAO_AI_EDITOR_OK) {
                Json to_params = operation;
                to_params["path"] = operation.value("to", std::string{});
                status = resolve_workspace(to_params, true, to);
            }
            if (status == SAO_AI_EDITOR_OK) {
                std::error_code error;
                if (operation.value("overwrite", false) &&
                    std::filesystem::exists(to, error)) {
                    std::filesystem::remove_all(to, error);
                }
                error.clear();
                std::filesystem::rename(from, to, error);
                status = error ? SAO_AI_EDITOR_ERR_OS_CALL_FAILED
                               : SAO_AI_EDITOR_OK;
            }
            if (status == SAO_AI_EDITOR_OK) {
                publish("fileEvent",
                        Json{{"op", "renamed"},
                             {"uri", file_uri_for(to)},
                             {"oldUri", file_uri_for(from)},
                             {"path", wide_to_utf8(to.native())}});
            }
        } else if (op == "delete") {
            std::filesystem::path target;
            Json delete_params = operation;
            delete_params["path"] = operation.value("uri", operation.value("from",
                                                                         std::string{}));
            status = resolve_workspace(delete_params, false, target);
            if (status == SAO_AI_EDITOR_OK) {
                std::error_code error;
                if (operation.value("recursive", false)) {
                    std::filesystem::remove_all(target, error);
                } else {
                    std::filesystem::remove(target, error);
                }
                status = error ? SAO_AI_EDITOR_ERR_OS_CALL_FAILED
                               : SAO_AI_EDITOR_OK;
            }
            if (status == SAO_AI_EDITOR_OK) {
                publish("fileEvent", Json{{"op", "deleted"},
                                          {"uri", file_uri_for(target)},
                                          {"path",
                                           wide_to_utf8(target.native())}});
            }
        }
        if (status == SAO_AI_EDITOR_OK) {
            ++applied_file_operations;
        } else {
            failures.push_back(Json{{"op", op}, {"status", status}});
        }
    }
    for (const auto& document_edit :
         params.value("documentEdits", Json::array())) {
        if (!document_edit.is_object()) {
            continue;
        }
        Json sub_params{{"uri", document_edit.value("uri", std::string{})},
                        {"edits", document_edit.value("edits", Json::array())}};
        Json sub_result;
        const int32_t status = document_apply_edits(sub_params, sub_result);
        if (status == SAO_AI_EDITOR_OK) {
            ++applied_document_edits;
        } else {
            failures.push_back(
                Json{{"uri", document_edit.value("uri", std::string{})},
                     {"status", status}});
        }
    }
    result = Json{{"applied", failures.empty()},
                  {"documentEdits", applied_document_edits},
                  {"fileOperations", applied_file_operations},
                  {"failures", failures}};
    return failures.empty() ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
}

// --- filesystem watchers -----------------------------------------------------
//
// Real ReadDirectoryChangesW pump over the workspace root (recursive).
// Watchers register per-extension glob + ignore flags; event filtering by
// glob happens shim-side where the glob grammar lives, while the drain
// queue records the raw stream.

void watcher_thread_main(std::filesystem::path root, HANDLE dir, HANDLE stop) {
    std::vector<uint8_t> buffer(64 * 1024);
    std::wstring pending_old_name;
    for (;;) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DWORD bytes = 0;
        const BOOL issued = ::ReadDirectoryChangesW(
            dir, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_CREATION,
            &bytes, &overlapped, nullptr);
        if (issued == FALSE || overlapped.hEvent == nullptr) {
            if (overlapped.hEvent != nullptr) ::CloseHandle(overlapped.hEvent);
            break;
        }
        HANDLE waits[2] = {overlapped.hEvent, stop};
        const DWORD waited =
            ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (waited == WAIT_OBJECT_0 + 1 ||
            !state().watch_running.load()) {
            ::CancelIoEx(dir, &overlapped);
            ::CloseHandle(overlapped.hEvent);
            break;
        }
        if (waited != WAIT_OBJECT_0) {
            ::CancelIoEx(dir, &overlapped);
            ::CloseHandle(overlapped.hEvent);
            break;
        }
        DWORD transferred = 0;
        const BOOL got =
            ::GetOverlappedResult(dir, &overlapped, &transferred, FALSE);
        ::CloseHandle(overlapped.hEvent);
        if (got == FALSE || transferred == 0) {
            continue;
        }
        size_t emitted = 0;
        const uint8_t* cursor = buffer.data();
        const uint8_t* end = buffer.data() + transferred;
        while (cursor < end && emitted < kMaximumWatcherBatchEvents) {
            const auto* info =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor);
            const std::wstring name(info->FileName,
                                    info->FileNameLength / sizeof(wchar_t));
            const std::filesystem::path full = root / name;
            std::string op;
            switch (info->Action) {
                case FILE_ACTION_ADDED: op = "created"; break;
                case FILE_ACTION_REMOVED: op = "deleted"; break;
                case FILE_ACTION_MODIFIED: op = "changed"; break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    pending_old_name = name;
                    op.clear();
                    break;
                case FILE_ACTION_RENAMED_NEW_NAME: op = "renamed"; break;
                default: op.clear(); break;
            }
            if (!op.empty()) {
                Json payload{{"op", op},
                             {"path", wide_to_utf8(full.native())},
                             {"uri", file_uri_for(full)}};
                if (op == "renamed" && !pending_old_name.empty()) {
                    payload["oldUri"] = file_uri_for(root / pending_old_name);
                    payload["oldPath"] =
                        wide_to_utf8((root / pending_old_name).native());
                    pending_old_name.clear();
                }
                publish("fileEvent", payload);
                ++emitted;
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            cursor += info->NextEntryOffset;
        }
    }
}

void stop_watcher_pump() {
    auto& s = state();
    std::thread thread;
    HANDLE dir = nullptr;
    HANDLE stop = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.watch_running.store(false);
        dir = s.watch_dir;
        stop = s.watch_stop;
        thread = std::move(s.watch_thread);
        s.watch_dir = nullptr;
        s.watch_stop = nullptr;
        s.watch_thread = std::thread();
    }
    if (stop != nullptr) {
        ::SetEvent(stop);
    }
    if (thread.joinable()) {
        thread.join();
    }
    if (dir != nullptr) {
        ::CloseHandle(dir);
    }
    if (stop != nullptr) {
        ::CloseHandle(stop);
    }
}

int32_t ensure_watcher_pump_locked() {
    auto& s = state();
    if (s.watch_running.load()) {
        return SAO_AI_EDITOR_OK;
    }
    if (s.workspace_root.empty()) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const std::wstring native = s.workspace_root.native();
    HANDLE dir = ::CreateFileW(native.c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS |
                                   FILE_FLAG_OVERLAPPED,
                               nullptr);
    if (dir == INVALID_HANDLE_VALUE || dir == nullptr) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop == nullptr) {
        ::CloseHandle(dir);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    s.watch_dir = dir;
    s.watch_stop = stop;
    s.watch_running.store(true);
    try {
        s.watch_thread = std::thread(watcher_thread_main, s.workspace_root,
                                     dir, stop);
    } catch (...) {
        s.watch_running.store(false);
        ::SetEvent(stop);
        ::CloseHandle(dir);
        ::CloseHandle(stop);
        s.watch_dir = nullptr;
        s.watch_stop = nullptr;
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t watcher_create(const Json& params, Json& result) {
    const Json glob = params.value("glob", Json(nullptr));
    std::string pattern;
    std::string base;
    if (glob.is_string()) {
        pattern = glob.get<std::string>();
    } else if (glob.is_object()) {
        pattern = glob.value("pattern", std::string{});
        base = glob.value("base", std::string{});
    }
    if (pattern.empty() || pattern.size() > 1024 || !valid_utf8(pattern)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    bool stop_after = false;
    std::string id_text;
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.watchers.size() >= kMaximumWatchers) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        WatcherRecord record;
        record.id = s.sequence++;
        record.extension_id = params.value("extensionId", std::string{});
        record.glob = pattern;
        record.base = base;
        record.ignore_create = params.value("ignoreCreateEvents", false);
        record.ignore_change = params.value("ignoreChangeEvents", false);
        record.ignore_delete = params.value("ignoreDeleteEvents", false);
        s.watchers.push_back(record);
        id_text = "watcher-" + std::to_string(record.id);
        const int32_t status = ensure_watcher_pump_locked();
        if (status != SAO_AI_EDITOR_OK) {
            s.watchers.pop_back();
            return status;
        }
        (void)stop_after;
    }
    result = Json{{"watcherId", id_text}, {"glob", pattern}};
    return SAO_AI_EDITOR_OK;
}

int32_t watcher_dispose(const Json& params, Json& result) {
    const std::string id_text = params.value("watcherId", std::string{});
    if (id_text.rfind("watcher-", 0) != 0) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const uint64_t id = std::strtoull(id_text.c_str() + 8, nullptr, 10);
    bool stop_pump = false;
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto found =
            std::find_if(s.watchers.begin(), s.watchers.end(),
                         [id](const WatcherRecord& record) {
                             return record.id == id;
                         });
        if (found == s.watchers.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        s.watchers.erase(found);
        stop_pump = s.watchers.empty();
    }
    if (stop_pump) {
        stop_watcher_pump();
    }
    result = Json{{"ok", true}, {"watcherId", id_text}};
    return SAO_AI_EDITOR_OK;
}

// --- workspace folders ---------------------------------------------------------
//
// Primary root stays fixed at index 0 (matches the workspace containment
// contract); extra folders persist under the extapi.workspaceFolders
// section of the workspace configuration scope.

int32_t workspace_load_extra_folders(Json& folders) {
    folders = Json::array();
    const ScopeStore* scopes;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        scopes = scope_store_locked();
    }
    if (scopes == nullptr) {
        return SAO_AI_EDITOR_OK;  // store unavailable → primaries only
    }
    Json workspace_config;
    const int32_t status =
        scopes->load_scope_config("workspace", "", workspace_config);
    if (status != SAO_AI_EDITOR_OK || !workspace_config.is_object()) {
        return SAO_AI_EDITOR_OK;
    }
    const Json stored =
        workspace_config.value("extapi.workspaceFolders", Json::array());
    if (stored.is_array()) {
        for (const auto& entry : stored) {
            if (entry.is_object() &&
                (entry.contains("uri") || entry.contains("path"))) {
                folders.push_back(Json{{"uri", entry.value("uri", std::string{})},
                                       {"path", entry.value("path", std::string{})},
                                       {"name", entry.value("name", std::string{})}});
            }
        }
    }
    return SAO_AI_EDITOR_OK;
}

int32_t workspace_save_extra_folders(const Json& folders) {
    const ScopeStore* scopes;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        scopes = scope_store_locked();
    }
    if (scopes == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json workspace_config;
    const int32_t load_status =
        scopes->load_scope_config("workspace", "", workspace_config);
    if (load_status != SAO_AI_EDITOR_OK) {
        return load_status;
    }
    if (!workspace_config.is_object()) {
        workspace_config = Json::object();
    }
    workspace_config["extapi.workspaceFolders"] = folders;
    return scopes->save_scope_config("workspace", "", workspace_config);
}

int32_t workspace_folders(Json& result) {
    const std::filesystem::path root = workspace_root();
    Json folders = Json::array();
    if (!root.empty()) {
        folders.push_back(Json{{"index", 0},
                               {"name", "workspace"},
                               {"uri", file_uri_for(root)},
                               {"fsPath", wide_to_utf8(root.native())}});
    }
    Json extras;
    const int32_t status = workspace_load_extra_folders(extras);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    for (const auto& entry : extras) {
        const std::string uri = entry.value("uri", std::string{});
        const std::string raw_path = entry.value("path", std::string{});
        std::string fs_path = raw_path;
        std::string resolved_uri = uri;
        if (fs_path.empty() && !uri.empty()) {
            Json params;
            params["uri"] = uri;
            fs_path = param_fs_path(params);
        }
        if (resolved_uri.empty() && !fs_path.empty()) {
            resolved_uri = file_uri_for(std::filesystem::path(utf8_to_wide(fs_path)));
        }
        folders.push_back(Json{{"index", static_cast<int64_t>(folders.size())},
                               {"name",
                                entry.value("name", std::filesystem::path(
                                                        utf8_to_wide(fs_path))
                                                        .filename()
                                                        .string())},
                               {"uri", resolved_uri},
                               {"fsPath", fs_path}});
    }
    result = folders;
    return SAO_AI_EDITOR_OK;
}

int32_t workspace_update_folders(const Json& params, Json& result) {
    if (!params.contains("folders") || !params["folders"].is_array()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const int64_t start = params.value("start", -1);
    const int64_t delete_count = params.value("deleteCount", 0);
    const Json folders_to_add = params["folders"];
    if (folders_to_add.size() > 64) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json extras;
    (void)workspace_load_extra_folders(extras);
    std::vector<Json> current(extras.begin(), extras.end());
    const int64_t total = 1 + static_cast<int64_t>(current.size());
    const int64_t clamped_start =
        start < 0 || start > total ? total : start;
    const int64_t clamped_delete =
        std::max<int64_t>(0, std::min(delete_count, total - clamped_start));
    if (clamped_start == 0 && clamped_delete > 0) {
        result = Json{{"applied", false},
                      {"error", "the primary workspace folder cannot be removed"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    for (const auto& addition : folders_to_add) {
        if (!addition.is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }
    std::vector<Json> additions;
    for (const auto& addition : folders_to_add) {
        const auto uri_it = addition.find("uri");
        const std::string uri =
            uri_it != addition.end() && uri_it->is_string()
                ? uri_it->get<std::string>()
                : std::string{};
        const std::string raw = addition.value("path", std::string{});
        std::string fs_path = raw;
        if (fs_path.empty() && !uri.empty()) {
            Json holder;
            holder["uri"] = uri;
            fs_path = param_fs_path(holder);
        }
        if (fs_path.empty() || !valid_utf8(fs_path)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        additions.push_back(Json{{"path", fs_path},
                                 {"uri", uri.empty() ? file_uri_for(
                                                           std::filesystem::path(
                                                               utf8_to_wide(fs_path)))
                                                     : uri},
                                 {"name",
                                  addition.value("name", std::string{})}});
    }
    const size_t erase_begin =
        static_cast<size_t>(clamped_start - 1 < 0 ? 0 : clamped_start - 1);
    const size_t erase_count = static_cast<size_t>(clamped_delete);
    std::vector<Json> removed(current.begin() +
                                  std::min(current.size(), erase_begin),
                              current.begin() +
                                  std::min(current.size(),
                                           erase_begin + erase_count));
    current.erase(current.begin() + std::min(current.size(), erase_begin),
                  current.begin() +
                      std::min(current.size(), erase_begin + erase_count));
    current.insert(current.begin() + std::min(current.size(), erase_begin),
                   additions.begin(), additions.end());
    Json stored = Json::array();
    for (const auto& folder : current) {
        stored.push_back(folder);
    }
    const int32_t save_status = workspace_save_extra_folders(stored);
    if (save_status != SAO_AI_EDITOR_OK) {
        return save_status;
    }
    for (const auto& folder : additions) {
        publish("workspaceFolder", Json{{"op", "add"}, {"folder", folder}});
    }
    for (const auto& folder : removed) {
        publish("workspaceFolder", Json{{"op", "remove"}, {"folder", folder}});
    }
    Json folders_result;
    (void)workspace_folders(folders_result);
    result = Json{{"applied", true}, {"folders", folders_result}};
    return SAO_AI_EDITOR_OK;
}

int32_t workspace_get_folder(const Json& params, Json& result) {
    const std::string raw = param_fs_path(params);
    if (raw.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::error_code error;
    std::filesystem::path candidate = std::filesystem::weakly_canonical(
        std::filesystem::path(utf8_to_wide(raw)), error);
    if (error) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json folders;
    const int32_t status = workspace_folders(folders);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::string best;
    int64_t best_index = -1;
    size_t best_length = 0;
    for (const auto& folder : folders) {
        const std::string fs_path = folder.value("fsPath", std::string{});
        std::error_code nested_error;
        const std::filesystem::path folder_path = std::filesystem::weakly_canonical(
            std::filesystem::path(utf8_to_wide(fs_path)), nested_error);
        if (nested_error) {
            continue;
        }
        const std::wstring folder_native = folder_path.native();
        const std::wstring candidate_native = candidate.native();
        const bool contains =
            candidate_native == folder_native ||
            (candidate_native.size() > folder_native.size() &&
             candidate_native.compare(0, folder_native.size(), folder_native) == 0 &&
             (candidate_native[folder_native.size()] == L'\\' ||
              candidate_native[folder_native.size()] == L'/'));
        if (contains && fs_path.size() > best_length) {
            best = fs_path;
            best_length = fs_path.size();
            best_index = folder.value("index", INT64_C(-1));
        }
    }
    if (best_index < 0) {
        result = Json(nullptr);
        return SAO_AI_EDITOR_OK;  // VS Code returns undefined
    }
    (void)best;
    for (const auto& folder : folders) {
        if (folder.value("index", INT64_C(-1)) == best_index) {
            result = folder;
            return SAO_AI_EDITOR_OK;
        }
    }
    result = Json(nullptr);
    return SAO_AI_EDITOR_OK;
}

// --- fs.* ------------------------------------------------------------------

int32_t fs_stat(const Json& params, Json& result) {
    std::filesystem::path resolved;
    const int32_t status = resolve_workspace(params, false, resolved);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::error_code error;
    const auto file_status = std::filesystem::status(resolved, error);
    if (error) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    int64_t type = 0;  // FileType.Unknown
    if (std::filesystem::is_directory(file_status)) {
        type = 2;
    } else if (std::filesystem::is_regular_file(file_status)) {
        type = 1;
    }
    if (std::filesystem::is_symlink(file_status)) {
        type |= 64;
    }
    const auto mtime = std::filesystem::last_write_time(resolved, error);
    const int64_t mtime_ms = error
        ? 0
        : std::chrono::duration_cast<std::chrono::milliseconds>(
              mtime.time_since_epoch())
              .count();
    uint64_t size = 0;
    if (type & 1) {
        size = static_cast<uint64_t>(
            std::filesystem::file_size(resolved, error));
        if (error) size = 0;
    }
    result = Json{{"type", type},
                  {"ctime", mtime_ms},
                  {"mtime", mtime_ms},
                  {"size", size}};
    return SAO_AI_EDITOR_OK;
}

int32_t fs_read_directory(const Json& params, Json& result) {
    std::filesystem::path resolved;
    const int32_t status = resolve_workspace(params, false, resolved);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::error_code error;
    std::filesystem::directory_iterator iterator(resolved, error);
    if (error) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    Json items = Json::array();
    size_t count = 0;
    for (const auto& entry : iterator) {
        if (++count > 4096) {
            break;
        }
        std::error_code type_error;
        int64_t type = 0;
        if (entry.is_symlink(type_error)) {
            type |= 64;
        }
        if (entry.is_directory(type_error)) {
            type |= 2;
        } else if (entry.is_regular_file(type_error)) {
            type |= 1;
        }
        items.push_back(Json::array(
            {wide_to_utf8(entry.path().filename().native()), type}));
    }
    result = items;
    return SAO_AI_EDITOR_OK;
}

int32_t fs_create_directory(const Json& params, Json& result) {
    std::filesystem::path resolved;
    const int32_t status = resolve_workspace(params, true, resolved);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::error_code error;
    std::filesystem::create_directories(resolved, error);
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    publish("fileEvent", Json{{"op", "created"},
                              {"uri", file_uri_for(resolved)},
                              {"path", wide_to_utf8(resolved.native())}});
    result = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

int32_t fs_delete(const Json& params, Json& result) {
    std::filesystem::path resolved;
    const int32_t status = resolve_workspace(params, false, resolved);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::error_code error;
    std::uintmax_t removed = 0;
    if (params.value("recursive", false)) {
        removed = std::filesystem::remove_all(resolved, error);
    } else {
        removed = std::filesystem::remove(resolved, error) ? 1 : 0;
    }
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    publish("fileEvent", Json{{"op", "deleted"},
                              {"uri", file_uri_for(resolved)},
                              {"path", wide_to_utf8(resolved.native())}});
    result = Json{{"ok", true}, {"removed", static_cast<int64_t>(removed)}};
    return SAO_AI_EDITOR_OK;
}

int32_t fs_rename(const Json& params, Json& result) {
    std::filesystem::path from;
    const std::string from_raw =
        params.value("source", params.value("from", std::string{}));
    if (from_raw.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json from_params;
    from_params["path"] = from_raw;
    int32_t status = resolve_workspace(from_params, false, from);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const std::string to_raw =
        params.value("target", params.value("to", std::string{}));
    Json to_params;
    to_params["path"] = to_raw;
    std::filesystem::path to;
    status = resolve_workspace(to_params, true, to);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::error_code error;
    if (params.value("overwrite", false) &&
        std::filesystem::exists(to, error)) {
        std::filesystem::remove_all(to, error);
        error.clear();
    }
    std::filesystem::rename(from, to, error);
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    publish("fileEvent", Json{{"op", "renamed"},
                              {"uri", file_uri_for(to)},
                              {"oldUri", file_uri_for(from)},
                              {"path", wide_to_utf8(to.native())}});
    result = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

int32_t fs_copy(const Json& params, Json& result) {
    std::filesystem::path from;
    Json from_params;
    from_params["path"] =
        params.value("source", params.value("from", std::string{}));
    int32_t status = resolve_workspace(from_params, false, from);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    Json to_params;
    to_params["path"] =
        params.value("target", params.value("to", std::string{}));
    std::filesystem::path to;
    status = resolve_workspace(to_params, true, to);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::error_code error;
    const auto options =
        params.value("overwrite", false)
            ? std::filesystem::copy_options::overwrite_existing
            : std::filesystem::copy_options::none;
    if (std::filesystem::is_directory(from, error)) {
        std::filesystem::copy(from, to,
                              std::filesystem::copy_options::recursive |
                                  options,
                              error);
    } else {
        std::filesystem::copy_file(from, to, options, error);
    }
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    publish("fileEvent", Json{{"op", "created"},
                              {"uri", file_uri_for(to)},
                              {"path", wide_to_utf8(to.native())}});
    result = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

// --- showTextDocument / progress ----------------------------------------------

int32_t window_show_text_document(const Json& params, Json& result) {
    const std::string uri = params.value("uri", std::string{});
    Json open_params = params;
    Json opened;
    const int32_t status = document_open(nullptr, open_params, opened);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().active_document_uri = opened.value("uri", uri);
    }
    publish("document", Json{{"op", "active"},
                             {"uri", opened.value("uri", uri)}});
    result = opened;
    result.erase("content");
    return SAO_AI_EDITOR_OK;
}

int32_t window_progress(const Json& params, Json& result) {
    const std::string phase = params.value("phase", std::string{"report"});
    publish("progress", Json{{"phase", phase},
                             {"id", params.value("progressId",
                                                 params.value("id", std::string{}))},
                             {"title", params.value("title", std::string{})},
                             {"location", params.value("location", Json(nullptr))},
                             {"cancellable", params.value("cancellable", false)},
                             {"increment", params.value("increment", Json(nullptr))},
                             {"message", params.value("message", std::string{})}});
    result = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

// --- extensions (host snapshot) --------------------------------------------------

int32_t extensions_list(NativeRuntime* runtime, Json& result) {
    if (runtime == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const Json snapshot = runtime->extension_host_snapshot();
    result = Json{{"extensions", snapshot.value("extensions", Json::array())},
                  {"total", snapshot.value("total", 0)},
                  {"nodeAlive", snapshot.value("nodeAlive", false)}};
    return SAO_AI_EDITOR_OK;
}

int32_t extensions_get(NativeRuntime* runtime, const Json& params, Json& result) {
    if (runtime == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const std::string id = params.value("id", params.value("extensionId",
                                                           std::string{}));
    const Json snapshot = runtime->extension_host_snapshot();
    for (const auto& extension :
         snapshot.value("extensions", Json::array())) {
        if (extension.is_object() &&
            extension.value("id", extension.value("extensionId", std::string{})) ==
                id) {
            result = Json{{"found", true}, {"extension", extension}};
            return SAO_AI_EDITOR_OK;
        }
    }
    result = Json{{"found", false}};
    return SAO_AI_EDITOR_OK;
}

// --- lm ------------------------------------------------------------------------
//
// selectChatModels merges the real configured providers (persisted
// `providers` registry + manifest contributions via
// NativeRuntime::chat_model_catalog) with extension-registered providers,
// so an empty catalog genuinely means "no configured provider".
// sendRequest routes to run_extension_chat for catalog ids and to the
// extension-host invoke channel for extension providers.

int32_t lm_select_models(NativeRuntime* runtime, Json& result) {
    Json models = Json::array();
    if (runtime != nullptr) {
        Json catalog = runtime->chat_model_catalog();
        if (catalog.is_array()) {
            for (auto& model : catalog) {
                model["sendRequest"] = "native";
                models.push_back(std::move(model));
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        for (const auto& [id, provider] : state().lm_providers) {
            Json entry{
                {"id", "ext/" + provider.value("providerId", std::string{})},
                {"vendor", provider.value("vendor", std::string{})},
                {"family", provider.value("vendor", std::string{})},
                {"name", provider.value("label",
                                        provider.value("vendor", std::string{}))},
                {"sendRequest", "extension"},
                {"extensionId", provider.value("extensionId", std::string{})}};
            models.push_back(std::move(entry));
        }
    }
    result = Json{{"models", models}, {"total", models.size()}};
    return SAO_AI_EDITOR_OK;
}

int32_t lm_register_provider(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string vendor = params.value("vendor", std::string{});
    if (!valid_simple_id(extension_id) || vendor.empty() ||
        vendor.size() > 128 || !valid_utf8(vendor)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::string provider_id = params.value("providerId", std::string{});
    {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (provider_id.empty()) {
            provider_id = "lm-" + std::to_string(s.sequence++);
        }
        s.lm_providers[provider_id] = Json{
            {"providerId", provider_id},
            {"extensionId", extension_id},
            {"vendor", vendor},
            {"label", params.value("label", vendor)},
            {"metadata", params.value("metadata", Json::object())}};
    }
    publish("providers",
            Json{{"op", "register"}, {"kind", "lm"}, {"providerId", provider_id}});
    result = Json{{"providerId", provider_id}};
    return SAO_AI_EDITOR_OK;
}

int32_t lm_send_request(NativeRuntime* runtime, const Json& params, Json& result) {
    const std::string model = params.value("model", std::string{});
    const Json messages = params.value("messages", Json::array());
    if (model.empty() || !messages.is_array() || messages.empty() ||
        messages.size() > 256) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (model.rfind("ext/", 0) == 0) {
        const std::string provider_id = model.substr(4);
        Json invoke_params{{"providerId", provider_id},
                           {"messages", messages},
                           {"model", model},
                           {"options",
                            params.value("options", Json::object())}};
        Json out;
        const int32_t status =
            invoke_extension(runtime, "lm.chat", invoke_params, out);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"model", model},
                      {"provider", "extension"},
                      {"response", out}};
        return SAO_AI_EDITOR_OK;
    }
    if (runtime == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    // "providerId/modelId" or bare "providerId".
    std::string provider_id = model;
    std::string model_id = params.value("modelId", std::string{});
    const size_t slash = model.find('/');
    if (slash != std::string::npos) {
        provider_id = model.substr(0, slash);
        if (model_id.empty()) {
            model_id = model.substr(slash + 1);
        }
    }
    Json chat_params{{"provider", Json{{"id", provider_id}}},
                     {"messages", messages}};
    if (!model_id.empty()) {
        chat_params["model"] = model_id;
    }
    for (const char* key : {"maxTokens", "temperature", "topP", "timeoutMs"}) {
        if (params.contains(key)) {
            chat_params[key] = params[key];
        }
    }
    std::string content;
    const int32_t status = runtime->run_extension_chat(
        chat_params, params.value("timeoutMs", 60000U), content);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result = Json{{"model", model},
                  {"provider", "native"},
                  {"content", content}};
    return SAO_AI_EDITOR_OK;
}

// --- chat participants -----------------------------------------------------------

int32_t chat_register_participant(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string participant_id =
        params.value("participantId", std::string{});
    if (!valid_simple_id(extension_id) || participant_id.empty() ||
        participant_id.size() > 128 || !valid_utf8(participant_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().chat_participants[extension_id + "/" + participant_id] = Json{
            {"participantId", participant_id},
            {"extensionId", extension_id},
            {"description", params.value("description", std::string{})},
            {"isSticky", params.value("isSticky", false)}};
    }
    publish("chat",
            Json{{"op", "registerParticipant"},
                 {"participantId", participant_id},
                 {"extensionId", extension_id}});
    result = Json{{"participantId", participant_id}};
    return SAO_AI_EDITOR_OK;
}

int32_t chat_invoke(NativeRuntime* runtime, const Json& params, Json& result) {
    const std::string participant_id =
        params.value("participantId", std::string{});
    if (participant_id.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json invoke_params{{"participantId", participant_id},
                       {"request", params.value("request", Json::object())},
                       {"references", params.value("references", Json::array())}};
    Json out;
    const int32_t status =
        invoke_extension(runtime, "chat.participant", invoke_params, out);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result = out;
    return SAO_AI_EDITOR_OK;
}

// --- authentication ---------------------------------------------------------------
//
// Session persistence rides the shared secrets vault under
// "auth-sessions/<providerId>" so sessions survive across doors.

int32_t auth_register_provider(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string provider_id = params.value("providerId", std::string{});
    const std::string label = params.value("label", std::string{});
    if (!valid_simple_id(extension_id) || !valid_simple_id(provider_id) ||
        label.empty() || label.size() > 256 || !valid_utf8(label)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().auth_providers[provider_id] = Json{
            {"providerId", provider_id},
            {"extensionId", extension_id},
            {"label", label},
            {"supportsMultipleAccounts",
             params.value("supportsMultipleAccounts", false)}};
    }
    publish("authentication", Json{{"op", "registerProvider"},
                                   {"providerId", provider_id}});
    result = Json{{"providerId", provider_id}};
    return SAO_AI_EDITOR_OK;
}

bool auth_scopes_cover(const Json& session_scopes, const Json& wanted) {
    if (!session_scopes.is_array() || !wanted.is_array()) {
        return false;
    }
    for (const auto& scope : wanted) {
        if (!scope.is_string()) {
            continue;
        }
        bool covered = false;
        for (const auto& owned : session_scopes) {
            if (owned.is_string() &&
                owned.get<std::string>() == scope.get<std::string>()) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            return false;
        }
    }
    return true;
}

int32_t auth_get_session(NativeRuntime* runtime, const Json& params,
                         Json& result) {
    const std::string provider_id = params.value("providerId", std::string{});
    const Json wanted_scopes = params.value("scopes", Json::array());
    if (!valid_simple_id(provider_id) || !wanted_scopes.is_array() ||
        wanted_scopes.size() > 64) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string secret_key = "auth-sessions/" + provider_id;
    SecretStore* store;
    Json provider;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        store = secret_store_locked();
        const auto found = state().auth_providers.find(provider_id);
        if (found != state().auth_providers.end()) {
            provider = found->second;
        }
    }
    Json sessions = Json::array();
    if (store != nullptr) {
        std::string raw;
        if (store->get(secret_key, raw) == SAO_AI_EDITOR_OK &&
            valid_utf8(raw)) {
            Json parsed = Json::parse(raw, nullptr, false);
            if (parsed.is_object() && parsed.contains("sessions") &&
                parsed["sessions"].is_array()) {
                sessions = parsed["sessions"];
            }
        }
    }
    for (const auto& session : sessions) {
        if (!session.is_object()) {
            continue;
        }
        if (auth_scopes_cover(session.value("scopes", Json::array()),
                              wanted_scopes)) {
            result = Json{{"found", true}, {"session", session}};
            return SAO_AI_EDITOR_OK;
        }
    }
    if (!provider.is_object()) {
        result = Json{{"found", false},
                      {"reason", "no authentication provider registered"}};
        return SAO_AI_EDITOR_OK;
    }
    if (!params.value("createIfNone", false) &&
        !params.value("forceNewSession", false)) {
        result = Json{{"found", false}};
        return SAO_AI_EDITOR_OK;
    }
    if (sessions.size() >= 64) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    // Round-trip into the extension's createSession handler through the
    // host invoke channel.
    Json invoke_params{{"providerId", provider_id},
                       {"scopes", wanted_scopes},
                       {"supportsMultipleAccounts",
                        provider.value("supportsMultipleAccounts", false)}};
    Json out;
    const int32_t status =
        invoke_extension(runtime, "auth.session", invoke_params, out);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    Json session = out.is_object() && out.contains("session") ? out["session"] : out;
    if (!session.is_object() || !session.contains("accessToken")) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    session["scopes"] = wanted_scopes;
    sessions.push_back(session);
    if (store != nullptr) {
        (void)store->set(secret_key,
                         Json{{"sessions", sessions}}.dump());
    }
    publish("authentication", Json{{"op", "sessionsChanged"},
                                   {"providerId", provider_id},
                                   {"added", Json::array({session.value("id", std::string{})})}});
    result = Json{{"found", true}, {"session", session}};
    return SAO_AI_EDITOR_OK;
}

// --- debug / tasks registries --------------------------------------------------------

int32_t debug_register_provider(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string type = params.value("type", std::string{});
    const std::string kind = params.value("kind", std::string{"configuration"});
    static const std::vector<std::string> kinds = {
        "configuration", "adapterFactory", "adapterDescriptorFactory",
        "trackerFactory"};
    if (!valid_simple_id(extension_id) || type.empty() || type.size() > 128 ||
        std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string key = extension_id + "::" + kind + "::" + type;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().debug_providers[key] = Json{
            {"key", key},
            {"extensionId", extension_id},
            {"type", type},
            {"kind", kind}};
    }
    publish("debug",
            Json{{"op", "registerProvider"}, {"kind", kind}, {"type", type}});
    result = Json{{"key", key}};
    return SAO_AI_EDITOR_OK;
}

int32_t tasks_register_provider(const Json& params, Json& result) {
    const std::string extension_id = params.value("extensionId", std::string{});
    const std::string type = params.value("type", std::string{});
    if (!valid_simple_id(extension_id) || type.empty() || type.size() > 128) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string key = extension_id + "::" + type;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().task_providers[key] = Json{
            {"key", key},
            {"extensionId", extension_id},
            {"type", type}};
    }
    publish("tasks", Json{{"op", "registerProvider"}, {"type", type}});
    result = Json{{"key", key}};
    return SAO_AI_EDITOR_OK;
}

int32_t tasks_fetch(NativeRuntime* runtime, const Json& params, Json& result) {
    const std::string type = params.value("type", std::string{});
    std::vector<Json> providers;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        for (const auto& [key, provider] : state().task_providers) {
            if (!type.empty() && provider.value("type", std::string{}) != type) {
                continue;
            }
            providers.push_back(provider);
            if (providers.size() >= kMaximumExtensionsInvoke) {
                break;
            }
        }
    }
    Json tasks = Json::array();
    for (const auto& provider : providers) {
        Json invoke_params{{"type", provider.value("type", std::string{})},
                           {"key", provider.value("key", std::string{})}};
        Json out;
        const int32_t status =
            invoke_extension(runtime, "tasks.fetch", invoke_params, out);
        if (status == SAO_AI_EDITOR_OK) {
            const Json listed = out.is_object()
                                    ? out.value("tasks", Json::array())
                                    : out.is_array() ? out : Json::array();
            for (const auto& task : listed) {
                if (tasks.size() >= 512) {
                    break;
                }
                tasks.push_back(task);
            }
        }
    }
    result = Json{{"tasks", tasks}, {"total", tasks.size()}};
    return SAO_AI_EDITOR_OK;
}

// --- generic registry lister ------------------------------------------------

int32_t registry_list(const std::map<std::string, Json, std::less<>>& source,
                      Json& result, std::string_view field) {
    Json items = Json::array();
    for (const auto& [key, value] : source) {
        (void)key;
        items.push_back(value);
    }
    result = Json{{std::string(field), items}, {"total", items.size()}};
    return SAO_AI_EDITOR_OK;
}

// --- dispatch -----------------------------------------------------------------

int32_t dispatch_impl(NativeRuntime* runtime, std::string_view method,
                      const Json& params, Json& result);

int32_t dispatch(NativeRuntime* runtime, std::string_view method, const Json& params,
                 Json& result) {
    try {
        return dispatch_impl(runtime, method, params, result);
    } catch (const std::exception& error) {
        result = Json{{"message", error.what()}};
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    } catch (...) {
        result = Json{{"message", "extapi dispatch failed"}};
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

int32_t dispatch_impl(NativeRuntime* runtime, std::string_view method,
                      const Json& params, Json& result) {
    // clipboard + openExternal — real Win32.
    if (method == "vscode.env.clipboard.readText") {
        return clipboard_read(result);
    }
    if (method == "vscode.env.clipboard.writeText") {
        return clipboard_write(params, result);
    }
    if (method == "vscode.env.openExternal") {
        return open_external(params, result);
    }

    // secrets — real DPAPI vault.
    if (method == "vscode.secrets.store") {
        return secrets_store(params, result);
    }
    if (method == "vscode.secrets.get") {
        return secrets_get(params, result);
    }
    if (method == "vscode.secrets.delete" || method == "vscode.secrets.remove") {
        return secrets_delete(params, result);
    }

    // environmentVariableCollection — real persisted registry.
    if (method == "vscode.env.vars.apply") {
        return env_vars_apply(params, result);
    }
    if (method == "vscode.env.vars.snapshot") {
        return env_vars_snapshot(params, result);
    }

    // ExtensionContext memento.
    if (method == "vscode.context.state.list") {
        return state_list(params, result);
    }
    if (method == "vscode.context.state.update") {
        return state_update(params, result, false);
    }
    if (method == "vscode.context.state.clear") {
        return state_update(params, result, true);
    }

    // status bar.
    if (method == "vscode.window.statusBar.create" ||
        method == "vscode.window.createStatusBarItem") {
        return status_bar_create(params, result);
    }
    if (method == "vscode.window.statusBar.update" ||
        method == "vscode.window.updateStatusBarItem") {
        return status_bar_update(params, result);
    }
    if (method == "vscode.window.statusBar.dispose" ||
        method == "vscode.window.disposeStatusBarItem") {
        return status_bar_dispose(params, result);
    }
    if (method == "vscode.window.statusBar.list" ||
        method == "vscode.window.listStatusBarItems") {
        std::lock_guard<std::mutex> lock(state().mutex);
        return registry_list(state().status_items, result, "items");
    }

    // output channels.
    if (method == "vscode.window.createOutputChannel") {
        return channel_create(params, result);
    }
    if (method == "vscode.window.appendOutput") {
        return channel_append(params, result);
    }
    if (method == "vscode.window.outputChannel") {
        return channel_op(params, result);
    }
    if (method == "vscode.window.readOutputChannel") {
        return channel_read(params, result);
    }
    if (method == "vscode.window.listOutputChannels") {
        return channel_list(result);
    }

    // documents + workspace edit.
    if (method == "vscode.workspace.openTextDocument") {
        return document_open(runtime, params, result);
    }
    if (method == "vscode.workspace.closeTextDocument") {
        return document_close(params, result);
    }
    if (method == "vscode.workspace.saveTextDocument") {
        return document_save(params, result);
    }
    if (method == "vscode.workspace.documents" ||
        method == "vscode.workspace.textDocuments") {
        return document_list(result);
    }
    if (method == "vscode.workspace.applyTextEdits") {
        return document_apply_edits(params, result);
    }
    if (method == "vscode.workspace.applyEdit") {
        return workspace_apply_edit(params, result);
    }
    if (method == "vscode.window.showTextDocument") {
        return window_show_text_document(params, result);
    }
    if (method == "vscode.window.progress") {
        return window_progress(params, result);
    }

    // file system watchers.
    if (method == "vscode.workspace.createWatcher" ||
        method == "vscode.workspace.createFileSystemWatcher") {
        return watcher_create(params, result);
    }
    if (method == "vscode.workspace.disposeWatcher") {
        return watcher_dispose(params, result);
    }
    if (method == "vscode.workspace.watchers") {
        std::lock_guard<std::mutex> lock(state().mutex);
        Json items = Json::array();
        for (const auto& watcher : state().watchers) {
            items.push_back(Json{{"watcherId",
                                  "watcher-" + std::to_string(watcher.id)},
                                 {"extensionId", watcher.extension_id},
                                 {"glob", watcher.glob},
                                 {"base", watcher.base},
                                 {"ignoreCreateEvents", watcher.ignore_create},
                                 {"ignoreChangeEvents", watcher.ignore_change},
                                 {"ignoreDeleteEvents", watcher.ignore_delete}});
        }
        result = Json{{"watchers", items}};
        return SAO_AI_EDITOR_OK;
    }

    // workspace folders.
    if (method == "vscode.workspace.workspaceFolders") {
        return workspace_folders(result);
    }
    if (method == "vscode.workspace.updateWorkspaceFolders") {
        return workspace_update_folders(params, result);
    }
    if (method == "vscode.workspace.getWorkspaceFolder") {
        return workspace_get_folder(params, result);
    }

    // fs.*
    if (method == "vscode.workspace.fs.stat") {
        return fs_stat(params, result);
    }
    if (method == "vscode.workspace.fs.readDirectory") {
        return fs_read_directory(params, result);
    }
    if (method == "vscode.workspace.fs.createDirectory") {
        return fs_create_directory(params, result);
    }
    if (method == "vscode.workspace.fs.delete") {
        return fs_delete(params, result);
    }
    if (method == "vscode.workspace.fs.rename") {
        return fs_rename(params, result);
    }
    if (method == "vscode.workspace.fs.copy") {
        return fs_copy(params, result);
    }
    if (method == "vscode.workspace.fs.isWritableFileSystem") {
        result = Json{{"scheme", "file"}, {"writable", true}};
        return SAO_AI_EDITOR_OK;
    }

    // languages.
    if (method == "vscode.languages.getLanguages") {
        std::set<std::string> languages{"plaintext", "json", "javascript",
                                        "typescript", "python",  "cpp",
                                        "csharp",    "go",   "rust",
                                        "markdown",  "html", "css"};
        {
            std::lock_guard<std::mutex> lock(state().mutex);
            for (const auto& [language, config] : state().language_configs) {
                (void)config;
                languages.insert(language);
            }
            for (const auto& [id, provider] : state().language_providers) {
                (void)id;
                const Json selector = provider.value("selector", Json(nullptr));
                if (selector.is_object() && selector.contains("language") &&
                    selector["language"].is_string()) {
                    languages.insert(selector["language"].get<std::string>());
                }
            }
        }
        result = Json::array();
        for (const auto& language : languages) {
            result.push_back(language);
        }
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.languages.registerProvider") {
        return language_register_provider(runtime, params, result);
    }
    if (method == "vscode.languages.unregisterProvider") {
        return language_unregister_provider(params, result);
    }
    if (method == "vscode.languages.listProviders") {
        return language_list_providers(params, result);
    }
    if (method == "vscode.languages.invoke") {
        return language_invoke(runtime, params, result);
    }
    if (method == "vscode.languages.setLanguageConfiguration") {
        return language_set_configuration(params, result);
    }
    if (method == "vscode.languages.getLanguageConfiguration") {
        const std::string language = params.value("language", std::string{});
        std::lock_guard<std::mutex> lock(state().mutex);
        const auto found = state().language_configs.find(language);
        result = Json{{"language", language},
                      {"configuration", found == state().language_configs.end()
                                             ? Json(nullptr)
                                             : found->second}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.languages.setDiagnostics") {
        return diagnostics_set(runtime, params, result);
    }
    if (method == "vscode.languages.getDiagnostics") {
        return diagnostics_get(params, result);
    }
    if (method == "vscode.languages.disposeDiagnostics" ||
        method == "vscode.languages.clearDiagnostics") {
        return diagnostics_dispose(params, result);
    }

    // extensions.
    if (method == "vscode.extensions.list") {
        return extensions_list(runtime, result);
    }
    if (method == "vscode.extensions.get" ||
        method == "vscode.extensions.getExtension") {
        return extensions_get(runtime, params, result);
    }

    // lm.
    if (method == "vscode.lm.selectChatModels") {
        return lm_select_models(runtime, result);
    }
    if (method == "vscode.lm.registerChatModelProvider") {
        return lm_register_provider(params, result);
    }
    if (method == "vscode.lm.unregisterChatModelProvider") {
        const std::string provider_id = params.value("providerId", std::string{});
        std::lock_guard<std::mutex> lock(state().mutex);
        state().lm_providers.erase(provider_id);
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.lm.listProviders") {
        std::lock_guard<std::mutex> lock(state().mutex);
        return registry_list(state().lm_providers, result, "providers");
    }
    if (method == "vscode.lm.sendRequest") {
        return lm_send_request(runtime, params, result);
    }

    // chat.
    if (method == "vscode.chat.registerParticipant") {
        return chat_register_participant(params, result);
    }
    if (method == "vscode.chat.unregisterParticipant") {
        const std::string extension_id = params.value("extensionId", std::string{});
        const std::string participant_id =
            params.value("participantId", std::string{});
        std::lock_guard<std::mutex> lock(state().mutex);
        state().chat_participants.erase(extension_id + "/" + participant_id);
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.chat.listParticipants") {
        std::lock_guard<std::mutex> lock(state().mutex);
        return registry_list(state().chat_participants, result, "participants");
    }
    if (method == "vscode.chat.invoke") {
        return chat_invoke(runtime, params, result);
    }

    // authentication.
    if (method == "vscode.authentication.registerProvider") {
        return auth_register_provider(params, result);
    }
    if (method == "vscode.authentication.unregisterProvider") {
        const std::string provider_id = params.value("providerId", std::string{});
        {
            std::lock_guard<std::mutex> lock(state().mutex);
            state().auth_providers.erase(provider_id);
        }
        publish("authentication",
                Json{{"op", "unregisterProvider"}, {"providerId", provider_id}});
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.authentication.listProviders") {
        std::lock_guard<std::mutex> lock(state().mutex);
        return registry_list(state().auth_providers, result, "providers");
    }
    if (method == "vscode.authentication.getSession") {
        return auth_get_session(runtime, params, result);
    }

    // debug + tasks registries.
    if (method == "vscode.debug.registerProvider") {
        return debug_register_provider(params, result);
    }
    if (method == "vscode.debug.listProviders") {
        std::lock_guard<std::mutex> lock(state().mutex);
        return registry_list(state().debug_providers, result, "providers");
    }
    if (method == "vscode.tasks.registerTaskProvider") {
        return tasks_register_provider(params, result);
    }
    if (method == "vscode.tasks.listProviders") {
        std::lock_guard<std::mutex> lock(state().mutex);
        return registry_list(state().task_providers, result, "providers");
    }
    if (method == "vscode.tasks.fetchTasks") {
        return tasks_fetch(runtime, params, result);
    }

    // extapi housekeeping.
    if (method == "sao.extapi.drain") {
        return ai_editor_extapi_drain_events(result);
    }
    if (method == "sao.extapi.describe") {
        std::lock_guard<std::mutex> lock(state().mutex);
        result = Json{{"runtimeBound", state().runtime != nullptr},
                      {"workspaceRoot", wide_to_utf8(state().workspace_root.native())},
                      {"systemRoot", wide_to_utf8(state().system_root.native())},
                      {"queuedEvents", state().event_queue.size()},
                      {"droppedEvents", state().event_dropped},
                      {"documents", state().documents.size()},
                      {"statusBarItems", state().status_items.size()},
                      {"outputChannels", state().channels.size()},
                      {"languageProviders", state().language_providers.size()},
                      {"diagnosticCollections", state().diagnostic_collections.size()},
                      {"watchers", state().watchers.size()},
                      {"watchPumpRunning", state().watch_running.load()}};
        return SAO_AI_EDITOR_OK;
    }

    result = Json{{"message", "unsupported extension method"},
                  {"method", std::string(method)}};
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

// --- lifecycle ------------------------------------------------------------------

void configure(std::string_view workspace, std::string_view system,
               std::string_view plugin_roots_json_value) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!workspace.empty()) {
        std::filesystem::path root;
        if (normalize_root(workspace, root, /*create=*/true)) {
            s.workspace_root = root;
        }
    }
    if (!system.empty()) {
        std::filesystem::path sys_root;
        if (normalize_root(system, sys_root, /*create=*/true)) {
            s.system_root = sys_root;
        }
    }
    if (s.system_root.empty()) {
        s.system_root = default_system_root();
    }
    if (!plugin_roots_json_value.empty() &&
        plugin_roots_json_value.size() <= kMaximumJsonBytes &&
        valid_utf8(plugin_roots_json_value)) {
        s.plugin_roots_json = std::string(plugin_roots_json_value);
    }
    s.configured = !s.workspace_root.empty();
    // Roots rebound — drop the lazily-built standalone stores so the next
    // access initialises against the new roots.  Runtime-bound access keeps
    // going through runtime->scope_store()/secrets_store() untouched.
    s.standalone_scopes.reset();
    s.standalone_secrets.reset();
}

void attach_runtime(NativeRuntime* runtime) noexcept {
    try {
        if (runtime == nullptr) {
            return;
        }
        configure(runtime->runtime_options().workspace_root,
                  runtime->runtime_options().system_root,
                  runtime->runtime_options().plugin_roots_json);
        std::lock_guard<std::mutex> lock(state().mutex);
        state().runtime = runtime;
    } catch (...) {
    }
}

void detach_runtime(NativeRuntime* runtime) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(state().mutex);
            if (state().runtime != runtime) {
                return;
            }
            state().runtime = nullptr;
        }
        stop_watcher_pump();
    } catch (...) {
    }
}

// --- drain ---------------------------------------------------------------------

int32_t drain(Json& out) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    out = Json::array();
    if (s.event_dropped > 0) {
        out.push_back(Json{{"type", "extapi.event"},
                           {"kind", "queue.overflow"},
                           {"ts", extapi_now_ms()},
                           {"payload",
                            Json{{"dropped", s.event_dropped}}}});
        s.event_dropped = 0;
    }
    while (!s.event_queue.empty()) {
        out.push_back(std::move(s.event_queue.front()));
        s.event_queue.pop_front();
    }
    return SAO_AI_EDITOR_OK;
}

} // namespace extapi

int32_t ai_editor_extapi_drain_events(Json& out) {
    try {
        return extapi::drain(out);
    } catch (...) {
        out = Json::array();
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

// --- NativeRuntime extapi bridges -------------------------------------------

int32_t NativeRuntime::invoke_extension_command(std::string_view command,
                                                const Json& arguments,
                                                uint32_t timeout_ms,
                                                Json& result) {
    if (extension_host_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    return extension_host_->execute_command(
        std::string(command), arguments,
        timeout_ms == 0 ? 15000 : timeout_ms, result);
}

int32_t NativeRuntime::run_extension_chat(const Json& params, uint32_t timeout_ms,
                                          std::string& out_content) {
    return run_chat_sync(params, timeout_ms == 0 ? 60000 : timeout_ms,
                         out_content);
}

Json NativeRuntime::chat_model_catalog() {
    Json models = Json::array();
    {
        Json registry;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status =
                scopes_.load_registry("providers", Json::array(), registry);
            if (status != SAO_AI_EDITOR_OK) {
                registry = Json::array();
            }
        }
        if (registry.is_array()) {
            for (const auto& provider : registry) {
                if (!provider.is_object()) {
                    continue;
                }
                const std::string provider_id =
                    provider.value("id", std::string{});
                if (provider_id.empty()) {
                    continue;
                }
                const std::string family =
                    provider.value("model", provider.value("type",
                                                           provider_id));
                const Json declared_models =
                    provider.value("models", Json::array());
                if (declared_models.is_array() && !declared_models.empty()) {
                    for (const auto& model_entry : declared_models) {
                        const std::string model_id =
                            model_entry.is_object()
                                ? model_entry.value("id", model_entry.value(
                                                              "name",
                                                              std::string{}))
                                : model_entry.is_string()
                                      ? model_entry.get<std::string>()
                                      : std::string{};
                        if (model_id.empty()) {
                            continue;
                        }
                        models.push_back(Json{
                            {"id", provider_id + "/" + model_id},
                            {"providerId", provider_id},
                            {"vendor", provider.value("type", provider_id)},
                            {"family", family},
                            {"name", model_id}});
                    }
                } else {
                    models.push_back(Json{
                        {"id", provider_id},
                        {"providerId", provider_id},
                        {"vendor", provider.value("type", provider_id)},
                        {"family", family},
                        {"name",
                         provider.value("label", provider_id)}});
                }
            }
        }
    }
    if (manifest_chat_providers_.is_array()) {
        for (const auto& provider : manifest_chat_providers_) {
            if (!provider.is_object()) {
                continue;
            }
            const std::string provider_id =
                provider.value("id", provider.value("vendor", std::string{}));
            if (provider_id.empty()) {
                continue;
            }
            models.push_back(Json{
                {"id", "manifest/" + provider_id},
                {"providerId", provider_id},
                {"vendor", provider.value("vendor", provider_id)},
                {"family", provider.value("family", provider_id)},
                {"name", provider.value("label", provider_id)},
                {"manifest", true}});
        }
    }
    return models;
}

Json NativeRuntime::extension_host_snapshot() const {
    if (extension_host_ == nullptr) {
        return Json{{"extensions", Json::array()}, {"total", 0}};
    }
    return extension_host_->snapshot();
}

} // namespace sao::ai_editor::native

namespace sao::ai_editor::native {

RuntimeLease::RuntimeLease(SaoAiEditorRuntime* handle) noexcept : handle_(handle) {
    if (!runtime_handle_is_live(handle_)) {
        handle_ = nullptr;
        return;
    }
    std::lock_guard<std::mutex> guard(handle_->lifetime_mutex);
    if (handle_->destroying || handle_->implementation == nullptr) {
        handle_ = nullptr;
        return;
    }
    ++handle_->leases;
    runtime_ = handle_->implementation.get();
}

RuntimeLease::~RuntimeLease() {
    if (handle_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(handle_->lifetime_mutex);
    if (handle_->leases > 0) {
        --handle_->leases;
    }
    if (handle_->destroying && handle_->leases == 0) {
        handle_->lifetime_ready.notify_all();
    }
}

} // namespace sao::ai_editor::native

namespace {

int32_t parse_runtime_options(const SaoAiEditorRuntimeConfig& config,
                              sao::ai_editor::native::RuntimeOptions& options) {
    using namespace sao::ai_editor::native;
    if (config.struct_size < sizeof(SaoAiEditorRuntimeConfig) ||
        config.workspace_root_utf8 == nullptr || !valid_utf8(config.workspace_root_utf8)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    options.workspace_root = config.workspace_root_utf8;
    if (config.system_root_utf8 != nullptr && config.system_root_utf8[0] != '\0') {
        if (!valid_utf8(config.system_root_utf8)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        options.system_root = config.system_root_utf8;
    } else {
        std::wstring home;
        DWORD required = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
        if (required == 0) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        home.resize(required);
        required = GetEnvironmentVariableW(L"USERPROFILE", home.data(), required);
        if (required == 0 || required >= home.size()) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        home.resize(required);
        options.system_root = wide_to_utf8((std::filesystem::path(home) / L".sao").native());
    }
    if (config.plugin_roots_json_utf8 != nullptr && config.plugin_roots_json_utf8[0] != '\0') {
        if (!valid_utf8(config.plugin_roots_json_utf8)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const Json plugins = Json::parse(config.plugin_roots_json_utf8, nullptr, false);
        if (plugins.is_discarded() || !plugins.is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        for (size_t index = 0; index < plugins.size(); ++index) {
            std::string id;
            std::string path;
            if (plugins[index].is_string()) {
                id = "plugin-" + std::to_string(index + 1);
                path = plugins[index].get<std::string>();
            } else if (plugins[index].is_object() && plugins[index].contains("path") &&
                       plugins[index]["path"].is_string()) {
                id = plugins[index].value("id", "plugin-" + std::to_string(index + 1));
                path = plugins[index]["path"].get<std::string>();
            } else {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (!valid_simple_id(id) || path.empty() || !valid_utf8(path)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        }
        options.plugin_roots_json = plugins.dump();
    }
    options.maximum_file_bytes = config.max_file_bytes;
    options.maximum_search_results = config.max_search_results;
    return SAO_AI_EDITOR_OK;
}

} // namespace

extern "C" SAO_AI_EDITOR_API uint32_t SAO_AI_EDITOR_CALL sao_ai_editor_native_abi_version(void) {
    try {
        return SAO_AI_EDITOR_NATIVE_ABI_VERSION;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_runtime_create(
    const SaoAiEditorRuntimeConfig* config, sao_ai_editor_runtime_t* out_handle) {
    try {
        if (config == nullptr || out_handle == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        *out_handle = nullptr;
        sao::ai_editor::native::RuntimeOptions options;
        const int32_t parsed = parse_runtime_options(*config, options);
        if (parsed != SAO_AI_EDITOR_OK) {
            return parsed;
        }
        auto handle = std::make_unique<SaoAiEditorRuntime>();
        handle->implementation =
            std::make_unique<sao::ai_editor::native::NativeRuntime>(std::move(options));
        const int32_t status = handle->implementation->initialize();
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        SaoAiEditorRuntime* raw_handle = handle.get();
        if (!sao::ai_editor::native::publish_runtime_handle(raw_handle)) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        *out_handle = handle.release();
        return SAO_AI_EDITOR_OK;
    } catch (const sao::ai_editor::native::Json::exception&) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_runtime_dispatch(
    sao_ai_editor_runtime_t handle, const void* request_json, uint32_t request_len,
    char* response_out, uint32_t response_cap, uint32_t* out_len) {
    try {
        sao::ai_editor::native::RuntimeLease lease(handle);
        auto* runtime = lease.get();
        if (runtime == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if ((request_json == nullptr && request_len != 0) || out_len == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(handle->dispatch_mutex);
        if (handle->pending_dispatch.empty()) {
            const auto input =
                std::string_view(static_cast<const char*>(request_json), request_len);
            if (input.empty() || input.size() > sao::ai_editor::native::kMaximumJsonBytes ||
                !sao::ai_editor::native::valid_utf8(input)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            sao::ai_editor::native::Json request =
                sao::ai_editor::native::Json::parse(input, nullptr, false);
            if (request.is_discarded()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            sao::ai_editor::native::Json response;
            const int32_t status = runtime->dispatch(request, response);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            handle->pending_dispatch = sao::ai_editor::native::dump_json(response);
        } else if (request_len != 0) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        const int32_t status = sao::ai_editor::native::copy_text_to_caller(
            handle->pending_dispatch, response_out, response_cap, out_len);
        if (status == SAO_AI_EDITOR_OK) {
            handle->pending_dispatch.clear();
        }
        return status;
    } catch (const sao::ai_editor::native::Json::exception&) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_runtime_next_event(
    sao_ai_editor_runtime_t handle, char* event_out, uint32_t event_cap, uint32_t* out_len) {
    try {
        sao::ai_editor::native::RuntimeLease lease(handle);
        auto* runtime = lease.get();
        if (runtime == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if (out_len == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(handle->event_mutex);
        if (handle->pending_event.empty()) {
            const int32_t status = runtime->next_event(0, handle->pending_event);
            if (status == SAO_AI_EDITOR_ERR_TIMEOUT) {
                *out_len = 0;
                if (event_out != nullptr && event_cap > 0) {
                    event_out[0] = '\0';
                }
                return SAO_AI_EDITOR_OK;
            }
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        const int32_t status = sao::ai_editor::native::copy_text_to_caller(
            handle->pending_event, event_out, event_cap, out_len);
        if (status == SAO_AI_EDITOR_OK) {
            handle->pending_event.clear();
        }
        return status;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_runtime_cancel(sao_ai_editor_runtime_t handle, const char* run_id_utf8) {
    try {
        sao::ai_editor::native::RuntimeLease lease(handle);
        auto* runtime = lease.get();
        if (runtime == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if (run_id_utf8 == nullptr || !sao::ai_editor::native::valid_utf8(run_id_utf8)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        sao::ai_editor::native::Json request{{"jsonrpc", "2.0"},
                                             {"id", 0},
                                             {"method", "run.cancel"},
                                             {"params", {{"runId", run_id_utf8}}}};
        sao::ai_editor::native::Json response;
        const int32_t status = runtime->dispatch(request, response);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        return response.contains("error") ? SAO_AI_EDITOR_ERR_NOT_FOUND : SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL
sao_ai_editor_runtime_destroy(sao_ai_editor_runtime_t handle) {
    try {
        if (handle == nullptr || !sao::ai_editor::native::retire_runtime_handle(handle)) {
            return;
        }
        {
            std::unique_lock<std::mutex> lock(handle->lifetime_mutex);
            handle->destroying = true;
            handle->lifetime_ready.wait(lock, [handle] { return handle->leases == 0; });
            handle->implementation.reset();
        }
    } catch (...) {
    }
}
