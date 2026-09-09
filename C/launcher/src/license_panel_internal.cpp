// license_panel_internal.cpp - native activation panel implementation.
//
// Mirrors the removed Python webview license_panel.html:
//   - license key text input (monospace)
//   - HWID display + copy button
//   - Activate button (calls sao_license_client_activate)
//   - progress state during the call
//   - tier + expiry on success, clear error on failure
//
// Follows the process_selector_panel pattern (descriptor register, action
// callback, event callback, body_set_spec, take_offline). Activation runs on
// a worker thread so the compositor owner thread never blocks on the network.

#include "license_panel_internal.h"

#include "sao/license/client/client_public.h"
#include "sao/license/sdk/license_sdk.h"
#include "sao/license/sdk/license_status.h"
#include "sao/license/sdk/license_types.h"
#include "sao/core/status.h"
#include "sao_core/sao_status.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_sdk.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <ctime>
#include <cstdio>
#include <string_view>
#include <thread>
#include <utility>

namespace sao::launcher::license_panel {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaximumPanelSpecBytes = 64U * 1024U;
constexpr std::size_t kMaximumActionPayloadBytes = 4096U;
constexpr std::size_t kMaximumActionIdBytes = 64U;
constexpr std::size_t kMaximumKeyBytes = 256U;
constexpr std::size_t kMaximumStatusBytes = 1024U;

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

std::string status_description(int32_t status, std::string_view prefix) {
    std::string description(prefix);
    if (!description.empty())
        description.append(": ");
    description.append(sao_status_str(static_cast<sao_status_t>(status)));
    description.append(" (");
    description.append(std::to_string(status));
    description.push_back(')');
    return bounded_text(std::move(description), kMaximumStatusBytes);
}

std::string license_status_description(int32_t status) {
    switch (status) {
    case SAO_OK:
        return "OK";
    case SAO_ERR_INVALID_ARGUMENT:
        return "Invalid activation key";
    case SAO_ERR_NOT_INITIALIZED:
        return "License client not initialized";
    case SAO_ERR_OS_CALL_FAILED:
        return "Operating-system call failed";
    case SAO_LICENSE_ERR_NETWORK:
        return "Network error: cannot reach the license server";
    case SAO_LICENSE_ERR_SERVER_REJECTED:
        return "The license server rejected the activation code";
    case SAO_LICENSE_ERR_RATE_LIMITED:
        return "Too many attempts; please wait and retry";
    case SAO_LICENSE_ERR_SIGNATURE_INVALID:
        return "Server response signature invalid; activation aborted";
    case SAO_LICENSE_ERR_HWID_MISMATCH:
        return "Hardware identity changed; reactivation required";
    case SAO_LICENSE_ERR_TOKEN_EXPIRED:
        return "License token expired";
    case SAO_LICENSE_ERR_TOKEN_REVOKED:
        return "License token revoked";
    case SAO_LICENSE_ERR_ANTI_DEBUG_TRIGGERED:
        return "Anti-debug check triggered; activation blocked";
    case SAO_LICENSE_ERR_INTEGRITY_BAD:
        return "Integrity check failed; activation blocked";
    case SAO_LICENSE_ERR_WHITEBOX_UNAVAILABLE:
        return "Crypto subsystem unavailable";
    case SAO_LICENSE_ERR_TPM_UNAVAILABLE:
        return "TPM unavailable";
    case SAO_LICENSE_ERR_NO_TOKEN:
        return "No license token present";
    case SAO_LICENSE_ERR_NOT_INITIALIZED:
        return "License subsystem not initialized";
    default:
        return status_description(status, "Activation failed");
    }
}

std::string tier_name(sao_license_tier_t tier) {
    switch (tier) {
    case SAO_LICENSE_TIER_FREE:
        return "free";
    case SAO_LICENSE_TIER_PAID:
        return "pro";
    case SAO_LICENSE_TIER_INTERNAL:
        return "team";
    default:
        return "unknown";
    }
}

std::string format_expiry(std::uint64_t expiry_ms) {
    if (expiry_ms == 0U)
        return "perpetual";
    const std::time_t seconds = static_cast<std::time_t>(expiry_ms / 1000U);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    const auto now_ms = static_cast<std::uint64_t>(std::time(nullptr)) * 1000U;
    const std::uint64_t remaining_days = expiry_ms > now_ms
        ? (expiry_ms - now_ms + 86'400'000U - 1U) / 86'400'000U : 0U;
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d UTC (%llu days remaining)",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                  static_cast<unsigned long long>(remaining_days));
    return buffer;
}

json text_node(std::string text, std::string_view style = "value", int height = 24) {
    return json{{"type", "text"},
                {"text", bounded_text(std::move(text), 4096U)},
                {"style", style},
                {"height", height}};
}

json button_node(std::string id, std::string label, std::string action, json payload,
                 std::string_view style = "default", bool disabled = false) {
    json node{{"type", "button"},
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

json input_node(std::string id, std::string value, std::string action,
                std::string_view style = "mono") {
    return json{{"type", "input"},
               {"id", std::move(id)},
               {"value", std::move(value)},
               {"action", std::move(action)},
               {"style", style},
               {"height", 32}};
}

json row_node(json children) {
    return json{{"type", "row"}, {"align", "left"}, {"children", std::move(children)}};
}

json badge_node(std::string text, std::string_view style = "muted") {
    return json{{"type", "badge"}, {"text", bounded_text(std::move(text), 256U)},
                {"style", style}, {"height", 22}};
}

json card_node(std::string title, json children, std::string_view accent = "cyan") {
    return json{{"type", "card"}, {"title", bounded_text(std::move(title), 512U)},
                {"accent", accent}, {"children", std::move(children)}};
}

json section_node(std::string title, json children, std::string_view accent = "cyan") {
    return json{{"type", "section"}, {"title", bounded_text(std::move(title), 512U)},
                {"accent", accent}, {"children", std::move(children)}};
}

json dock_document(json nodes, std::string content_id, int min_width = 520) {
    if (!nodes.is_array() || nodes.empty())
        return json{{"version", 1}, {"title", ""}, {"layout", "dock"}, {"nodes", std::move(nodes)}};
    json top = std::move(nodes.front());
    nodes.erase(nodes.begin());
    top["dock"] = "top";
    json content{{"type", "section"},
                 {"id", std::move(content_id)},
                 {"container", true},
                 {"layout", "vertical"},
                 {"width", 0},
                 {"min_width", min_width},
                 {"weight", 1.0F},
                 {"dock", "fill"},
                 {"scroll", {{"axis", "vertical"}, {"bar", "auto"}, {"wheel", true}}},
                 {"children", std::move(nodes)}};
    return json{{"version", 1},
                {"title", ""},
                {"layout", "dock"},
                {"nodes", json::array({std::move(top), std::move(content)})}};
}

json status_strip_node(std::string label, std::string message, std::string_view accent) {
    return card_node(
        "状态",
        json::array({row_node(json::array(
            {badge_node(std::move(label), accent), text_node(std::move(message), accent, 24)}))}),
        accent);
}

std::string tier_display(std::string_view tier) {
    if (tier == "free") return "Free / 免费";
    if (tier == "pro") return "Pro / 专业";
    if (tier == "team") return "Team / 团队";
    return "Unknown / 未知";
}

std::string_view license_accent(const Snapshot& snapshot) {
    if (snapshot.busy) return "gold";
    if (!snapshot.error_text.empty() || snapshot.last_status != SAO_STATUS_OK) return "danger";
    return snapshot.activated ? "ok" : "cyan";
}

std::string_view expiry_accent(const Snapshot& snapshot) {
    if (!snapshot.activated) return "muted";
    if (snapshot.expiry_ms == 0U) return "ok";
    const auto now_ms = static_cast<std::uint64_t>(std::time(nullptr)) * 1000U;
    return snapshot.expiry_ms > now_ms ? "gold" : "danger";
}
std::string build_panel_spec(const Snapshot& snapshot) {
    const std::string_view accent = license_accent(snapshot);
    json nodes = json::array();
    const std::string label = snapshot.busy ? "Busy / 处理中" :
                              (!snapshot.error_text.empty() || snapshot.last_status != SAO_STATUS_OK)
                                  ? "Error / 错误" : snapshot.activated ? "Active / 已激活" : "Ready / 就绪";
    const std::string message = snapshot.busy ? "Verifying license / 正在验证授权" :
                                !snapshot.error_text.empty() ? snapshot.error_text :
                                snapshot.status_text.empty() ? "License status / 授权状态" : snapshot.status_text;
    nodes.push_back(status_strip_node(label, message, accent));
    json status = json::array();
    {
        json row = json::array();
        row.push_back(text_node("Tier / 等级", "muted", 22));
        row.push_back(badge_node(snapshot.activated ? tier_display(snapshot.tier) : "Inactive / 未激活",
                                 snapshot.activated ? "cyan" : "muted"));
        status.push_back(row_node(std::move(row)));
    }
    {
        json row = json::array();
        row.push_back(text_node("Expiry / 到期", "muted", 22));
        row.push_back(badge_node(snapshot.activated ? format_expiry(snapshot.expiry_ms) : "—",
                                 expiry_accent(snapshot)));
        status.push_back(row_node(std::move(row)));
    }
    {
        json row = json::array();
        row.push_back(text_node("HWID / 设备指纹", "muted", 22));
        row.push_back(text_node(snapshot.hwid_hex.empty() ? "—" : snapshot.hwid_hex,
                                "mono", 22));
        status.push_back(row_node(std::move(row)));
    }
    {
        json row = json::array();
        row.push_back(button_node("license.copy_hwid", "复制 HWID", kCopyHwidAction, json::object(),
                                  "ghost", snapshot.hwid_hex.empty() || snapshot.busy));
        row.push_back(button_node("license.refresh", "刷新", kRefreshAction, json::object(),
                                  "default", snapshot.busy));
        status.push_back(row_node(std::move(row)));
    }
    nodes.push_back(card_node("授权与设备 / License", std::move(status), accent));

    json activation = json::array();
    activation.push_back(text_node("Activation key / 激活码", "muted", 22));
    activation.push_back(text_node("已有激活码可在下方输入；也可以选择使用免费版。", "muted", 34));
    activation.push_back(input_node("license.key_input", snapshot.license_key, kKeyInputAction));
    json activation_actions = json::array();
    activation_actions.push_back(button_node("license.activate", snapshot.busy ? "激活中…" : "激活",
                                             kActivateAction, json::object(), "primary",
                                             snapshot.busy));
    activation_actions.push_back(button_node("license.skip", "使用免费版", kSkipAction,
                                             json::object(), "ghost", snapshot.busy));
    activation.push_back(row_node(std::move(activation_actions)));
    if (snapshot.busy)
        activation.push_back(text_node("Verifying with license server... / 正在验证授权服务器...", "warn", 22));
    nodes.push_back(
        card_node("激活 / Activation", std::move(activation), snapshot.busy ? "gold" : "cyan"));
    if (!snapshot.error_text.empty()) {
        json msg = json::array();
        msg.push_back(text_node(snapshot.error_text, "danger", 28));
        msg.push_back(button_node("license.error-retry", "重试", kRefreshAction, json::object(),
                                  "primary", snapshot.busy));
        nodes.push_back(section_node("Error / 错误", std::move(msg), "danger"));
    } else if (!snapshot.status_text.empty()) {
        json msg = json::array();
        msg.push_back(text_node(snapshot.status_text, "muted", 28));
        nodes.push_back(section_node("Message / 消息", std::move(msg), snapshot.activated ? "ok" : "cyan"));
    }
    std::string serialized = dock_document(std::move(nodes), "license-content").dump();
    if (serialized.size() <= kMaximumPanelSpecBytes)
        return serialized;
    json compact = json::array();
    compact.push_back(text_node("Panel spec exceeded budget.", "bad", 48));
    return json{{"version", 1}, {"title", ""}, {"nodes", std::move(compact)}}.dump();
}
sao_status_t default_get_hwid(std::string& out_hex) {
    std::array<std::uint8_t, 32> hwid{};
    const int32_t status = sao_license_sdk_get_hwid(hwid.data());
    if (status != SAO_OK)
        return static_cast<sao_status_t>(status);
    constexpr char digits[] = "0123456789abcdef";
    out_hex.resize(hwid.size() * 2U);
    for (std::size_t index = 0; index < hwid.size(); ++index) {
        out_hex[index * 2U] = digits[hwid[index] >> 4U];
        out_hex[index * 2U + 1U] = digits[hwid[index] & 0x0fU];
    }
    return SAO_STATUS_OK;
}

sao_status_t default_get_status(std::string& out_tier, std::uint64_t& out_expiry_ms) {
    sao_license_tier_t tier = SAO_LICENSE_TIER_UNKNOWN;
    int32_t status = sao_license_sdk_get_tier(&tier);
    if (status != SAO_OK)
        return static_cast<sao_status_t>(status);
    std::uint64_t expiry_ms = 0U;
    status = sao_license_sdk_get_expiry_ms(&expiry_ms);
    if (status != SAO_OK)
        return static_cast<sao_status_t>(status);
    out_tier = tier_name(tier);
    out_expiry_ms = expiry_ms;
    return SAO_STATUS_OK;
}

sao_status_t default_copy_to_clipboard(std::string_view text) {
    if (text.empty())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!OpenClipboard(nullptr))
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    struct ClipboardGuard {
        ~ClipboardGuard() { CloseClipboard(); }
    } guard;
    if (!EmptyClipboard())
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    const std::size_t byte_count = text.size() + 1U;
    HANDLE handle = GlobalAlloc(GMEM_MOVEABLE, byte_count);
    if (handle == nullptr)
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    void* locked = GlobalLock(handle);
    if (locked == nullptr) {
        GlobalFree(handle);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    std::memcpy(locked, text.data(), text.size());
    static_cast<char*>(locked)[text.size()] = '\0';
    GlobalUnlock(handle);
    if (SetClipboardData(CF_TEXT, handle) == nullptr) {
        GlobalFree(handle);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return SAO_STATUS_OK;
}

sao_status_t default_refresh_license() {
    return static_cast<sao_status_t>(sao_license_sdk_refresh());
}

json parse_payload(std::string_view payload_json, bool& valid) {
    valid = false;
    if (payload_json.size() > kMaximumActionPayloadBytes ||
        payload_json.find('\0') != std::string_view::npos || !valid_utf8(payload_json)) {
        return {};
    }
    if (payload_json.empty()) {
        valid = true;
        return json::object();
    }
    json payload = json::parse(payload_json.begin(), payload_json.end(), nullptr, false, false);
    valid = !payload.is_discarded() && payload.is_object();
    return valid ? std::move(payload) : json{};
}

} // namespace

bool Operations::complete() const noexcept {
    return activate && get_hwid && get_status && copy_to_clipboard && refresh_license;
}

Operations make_default_operations() {
    Operations operations{};
    operations.activate = [](std::string_view key) -> sao_status_t {
        if (!valid_text(key, kMaximumKeyBytes, true))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::string owned(key);
        return static_cast<sao_status_t>(sao_license_client_activate(owned.c_str()));
    };
    operations.get_hwid = &default_get_hwid;
    operations.get_status = &default_get_status;
    operations.copy_to_clipboard = &default_copy_to_clipboard;
    operations.refresh_license = &default_refresh_license;
    return operations;
}

struct Owner::State {
    static std::mutex deferred_mutex;
    static std::vector<std::unique_ptr<State>> deferred_cleanup;
    explicit State(sao_ui_compositor_handle_t borrowed_compositor, Operations value)
        : compositor(borrowed_compositor), operations(std::move(value)) {
        if (compositor == nullptr || !operations.complete()) {
            startup_status = SAO_STATUS_ERR_INVALID_ARGUMENT;
            accepting = false;
        }
    }

    Owner* owner{};
    sao_status_t fail_next_unregister_status{SAO_STATUS_OK};
    sao_status_t fail_next_action_restore_status{SAO_STATUS_OK};
    sao_status_t fail_next_event_restore_status{SAO_STATUS_OK};
    sao_ui_compositor_handle_t compositor{};
    Operations operations;
    mutable std::mutex mutex;
    sao_status_t startup_status{SAO_STATUS_OK};
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    std::size_t operations_in_flight{};
    std::size_t callbacks_in_flight{};
    std::condition_variable callback_cv;
    bool visible{};
    bool accepting{true};
    bool creating{};
    bool retiring{};
    bool action_handler_attached{};
    bool event_handler_attached{};
    bool busy{};
    bool activated{};
    sao_status_t last_status{SAO_STATUS_OK};
    std::string status_text;
    std::string error_text;
    std::string hwid_hex_;
    std::string license_key;
    std::string tier;
    std::uint64_t expiry_ms{};
    std::string rendered_spec_json;
    bool publish_pending{true};
    std::atomic<bool> activation_running{false};
    std::atomic<bool> activation_cancel_requested{false};
    std::condition_variable activation_cv;
    std::thread activation_thread;
    std::thread status_thread;
    std::atomic<bool> status_running{false};
    std::atomic<bool> status_cancel_requested{false};
    std::condition_variable status_cv;
};

std::mutex Owner::deferred_mutex_;
std::vector<std::unique_ptr<Owner::State>>* Owner::deferred_cleanup_ =
    new std::vector<std::unique_ptr<Owner::State>>();

void Owner::stop_background_threads(State& state) noexcept {
    state.status_cancel_requested.store(true);
    state.activation_cancel_requested.store(true);
    Operations::CancelActivation cancel_activation;
    {
        std::lock_guard lock(state.mutex);
        cancel_activation = state.operations.cancel_activation;
    }
    if (cancel_activation) {
        try {
            cancel_activation();
        } catch (...) {
        }
    }
    if (state.status_running.load()) {
        std::unique_lock lock(state.mutex);
        state.status_cv.wait(lock, [&state] {
            return !state.status_running.load();
        });
    }
    if (state.activation_running.load()) {
        std::unique_lock lock(state.mutex);
        state.activation_cv.wait(lock, [&state] {
            return !state.activation_running.load();
        });
    }
    if (state.status_thread.joinable())
        state.status_thread.join();
    if (state.activation_thread.joinable())
        state.activation_thread.join();
}
void Owner::defer_state(std::unique_ptr<State> state) noexcept {
    if (state == nullptr)
        return;
    Owner::stop_background_threads(*state);
    {
        std::unique_lock lock(state->mutex);
        state->accepting = false;
        state->retiring = true;
        state->callback_cv.wait(lock, [&state] {
            return state->callbacks_in_flight == 0U;
        });
        state->owner = nullptr;
        state->retiring = false;
    }
    std::lock_guard lock(deferred_mutex_);
    deferred_cleanup_->push_back(std::move(state));
}

void Owner::drain_deferred_cleanup() noexcept {
    std::vector<std::unique_ptr<State>> pending;
    {
        std::lock_guard lock(deferred_mutex_);
        pending.swap(*deferred_cleanup_);
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
            deferred_cleanup_->push_back(std::move(state));
    }
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
    : Owner(compositor, make_default_operations()) {}

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

void Owner::fail_next_unregister_for_testing(sao_status_t status) noexcept { if (state_) { std::lock_guard lock(state_->mutex); state_->fail_next_unregister_status = status; } }

void Owner::fail_next_handler_restore_for_testing(sao_status_t action_status, sao_status_t event_status) noexcept { if (state_) { std::lock_guard lock(state_->mutex); state_->fail_next_action_restore_status = action_status; state_->fail_next_event_restore_status = event_status; } }

sao_status_t Owner::require_owner_thread() const noexcept {
    if (!state_ || state_->compositor == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    return sao_ui_compositor_require_owner_thread(state_->compositor);
}

sao_status_t Owner::begin_operation() noexcept {
    const sao_status_t owner_status = require_owner_thread();
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
    std::lock_guard lock(state_->mutex);
    if (state_->startup_status != SAO_STATUS_OK)
        return state_->startup_status;
    if (!state_->accepting || state_->retiring)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
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

void SAO_UI_CALL Owner::panel_action_callback(const char* action_id_utf8,
                                              const std::uint8_t* payload_json_utf8,
                                              std::size_t payload_len,
                                              void* user_data) noexcept {
    auto* state = static_cast<State*>(user_data);
    Owner* owner = nullptr;
    if (state != nullptr) {
        std::lock_guard lock(state->mutex);
        if (state->startup_status == SAO_STATUS_OK && state->accepting &&
            !state->retiring && state->owner != nullptr) {
            ++state->callbacks_in_flight;
            owner = state->owner;
        }
    }
    if (owner == nullptr || (payload_json_utf8 == nullptr && payload_len != 0U))
        return;
    struct CallbackGuard {
        State* state;
        ~CallbackGuard() {
            std::lock_guard lock(state->mutex);
            if (state->callbacks_in_flight != 0U)
                --state->callbacks_in_flight;
            state->callback_cv.notify_all();
        }
    } callback{state};
    const auto action = bounded_c_text(action_id_utf8, kMaximumActionIdBytes);
    if (!action.has_value()) return;
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
    Owner* owner = nullptr;
    if (state != nullptr) {
        std::lock_guard lock(state->mutex);
        if (state->startup_status == SAO_STATUS_OK && state->accepting &&
            !state->retiring && state->owner != nullptr) {
            ++state->callbacks_in_flight;
            owner = state->owner;
        }
    }
    if (owner == nullptr) return;
    struct CallbackGuard {
        State* state;
        ~CallbackGuard() {
            std::lock_guard lock(state->mutex);
            if (state->callbacks_in_flight != 0U)
                --state->callbacks_in_flight;
            state->callback_cv.notify_all();
        }
    } callback{state};
    try {
        owner->handle_panel_event(event_kind);
    } catch (...) {
    }
}

void Owner::drain_deferred_cleanup_for_owner() noexcept { drain_deferred_cleanup(); }

void Owner::drain_deferred_cleanup_for_testing() noexcept { drain_deferred_cleanup_for_owner(); }

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
        if (state_->startup_status != SAO_STATUS_OK)
            return state_->startup_status;
        if (state_->panel != nullptr)
            return SAO_STATUS_OK;
        if (!state_->retiring)
            state_->accepting = true;
        if (!state_->accepting || state_->retiring || state_->creating)
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
    descriptor.default_width_px = 520;
    descriptor.default_height_px = 520;
    descriptor.min_width_px = 420;
    descriptor.min_height_px = 400;
    descriptor.max_width_px = 900;
    descriptor.max_height_px = 800;
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
            if (!state_->publish_pending && !state_->rendered_spec_json.empty())
                return SAO_STATUS_OK;
            body = state_->body;
            state_->publish_pending = false;
            view.panel_created = true;
            view.visible = state_->visible;
            view.busy = state_->busy;
            view.last_status = state_->last_status;
            view.status_text = state_->status_text;
            view.error_text = state_->error_text;
            view.hwid_hex = state_->hwid_hex_;
            view.license_key = state_->license_key;
            view.tier = state_->tier;
            view.expiry_ms = state_->expiry_ms;
            view.activated = state_->activated;
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
            state_->publish_pending = true;
        return status;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

template <typename StateT>
void start_status_refresh(StateT& state) noexcept {
    if (state.status_running.exchange(true))
        return;
    state.status_cancel_requested.store(false);
    if (state.status_thread.joinable())
        state.status_thread.join();
    try {
        state.status_thread = std::thread([&state] {
            std::string tier;
            std::uint64_t expiry_ms = 0U;
            sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
            try {
                Operations::GetStatus get_status;
                Operations::GetHwid get_hwid;
                Operations::RefreshLicense refresh_license;
                {
                    std::lock_guard lock(state.mutex);
                    get_status = state.operations.get_status;
                    get_hwid = state.operations.get_hwid;
                    refresh_license = state.operations.refresh_license;
                }
                if (!state.status_cancel_requested.load())
                    status = refresh_license();
                std::string hwid;
                if (status == SAO_STATUS_OK && !state.status_cancel_requested.load())
                    status = get_hwid(hwid);
                if (status == SAO_STATUS_OK && !state.status_cancel_requested.load())
                    status = get_status(tier, expiry_ms);
                std::lock_guard lock(state.mutex);
                if (!state.status_cancel_requested.load()) {
                    state.last_status = status;
                    if (status == SAO_STATUS_OK && valid_text(tier, 64U, true)) {
                        state.hwid_hex_ = std::move(hwid);
                        state.tier = std::move(tier);
                        state.expiry_ms = expiry_ms;
                        state.activated = true;
                        state.status_text = "Status refreshed.";
                        state.error_text.clear();
                    } else {
                        state.status_text = "Showing last known status";
                        state.error_text = license_status_description(static_cast<std::int32_t>(status));
                    }
                    state.busy = false;
                    state.publish_pending = true;
                } else {
                    state.busy = false;
                    state.last_status = SAO_STATUS_ERR_CANCELLED;
                    state.publish_pending = true;
                }
                state.status_running.store(false);
            } catch (...) {
                std::lock_guard lock(state.mutex);
                if (!state.status_cancel_requested.load()) {
                    state.last_status = SAO_STATUS_ERR_UNKNOWN;
                    state.status_text = "Showing last known status";
                    state.error_text = "License status refresh failed";
                    state.busy = false;
                    state.publish_pending = true;
                } else {
                    state.busy = false;
                    state.last_status = SAO_STATUS_ERR_CANCELLED;
                    state.publish_pending = true;
                }
                state.status_running.store(false);
            }
            state.status_cv.notify_all();
        });
    } catch (...) {
        {
            std::lock_guard lock(state.mutex);
            state.status_running.store(false);
            state.busy = false;
            state.last_status = SAO_STATUS_ERR_OS_CALL_FAILED;
            state.status_text = "Showing last known status";
            state.error_text = "License status refresh failed";
            state.publish_pending = true;
        }
        state.status_cv.notify_all();
    }
}

sao_status_t Owner::open() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;

    Operations::GetHwid get_hwid;
    bool needs_hwid = false;
    {
        std::lock_guard lock(state_->mutex);
        needs_hwid = state_->hwid_hex_.empty();
        get_hwid = state_->operations.get_hwid;
    }
    if (needs_hwid) {
        try {
            std::string hwid_hex;
            const sao_status_t hwid_status = get_hwid(hwid_hex);
            if (hwid_status == SAO_STATUS_OK && valid_text(hwid_hex, 128U, true)) {
                std::lock_guard lock(state_->mutex);
                state_->hwid_hex_ = std::move(hwid_hex);
                state_->publish_pending = true;
            }
        } catch (...) {
        }
    }

    const sao_status_t publish_status = publish();
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
    bool needs_status = false;
    {
        std::lock_guard lock(state_->mutex);
        needs_status = !state_->activated && !state_->status_running.load();
        if (needs_status) {
            state_->busy = true;
            state_->status_text = "Loading license status...";
            state_->error_text.clear();
            state_->publish_pending = true;
        }
    }
    if (needs_status) {
        start_status_refresh(*state_);
        if (publish_status == SAO_STATUS_OK)
            return publish();
    }
    return publish_status;
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
    const sao_status_t hide_status = sao_ui_panel_hide(panel);
    if (hide_status != SAO_STATUS_OK)
        return hide_status;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->panel == panel)
            state_->visible = false;
    }
    return SAO_STATUS_OK;
}

sao_status_t Owner::service_ui() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    bool publish_needed = false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->panel == nullptr || state_->body == nullptr)
            return SAO_STATUS_OK;
        publish_needed = state_->publish_pending;
    }
    if (publish_needed)
        return publish();
    return SAO_STATUS_OK;
}

sao_status_t Owner::take_offline() noexcept {
    if (!state_)
        return SAO_STATUS_OK;
    const sao_status_t owner_status = require_owner_thread();
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
    stop_background_threads(*state_);
    sao_ui_panel_handle_t panel = nullptr;
    bool had_action_handler = false;
    bool had_event_handler = false;
    {
        std::unique_lock lock(state_->mutex);
        if (state_->panel == nullptr)
            return SAO_STATUS_OK;
        if (state_->creating || state_->retiring || state_->operations_in_flight != 0U)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        if (state_->body == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        state_->retiring = true;
        state_->accepting = false;
        state_->callback_cv.wait(lock, [this] {
            return state_->callbacks_in_flight == 0U;
        });
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
        state_->panel = nullptr;
        state_->body = nullptr;
        state_->visible = false;
        state_->busy = false;
        state_->action_handler_attached = false;
        state_->event_handler_attached = false;
        state_->accepting = false;
        state_->retiring = false;
        state_->rendered_spec_json.clear();
        state_->publish_pending = true;
        return SAO_STATUS_OK;
    }

    bool rollback_ok = true;
    if (had_action_handler && !action_attached) {
        const sao_status_t injected = std::exchange(state_->fail_next_action_restore_status, SAO_STATUS_OK);
        const sao_status_t restore_status = injected == SAO_STATUS_OK ?
            sao_ui_panel_set_action_handler(panel, &Owner::panel_action_callback, state_.get()) : injected;
        action_attached = restore_status == SAO_STATUS_OK;
        rollback_ok = rollback_ok && action_attached;
    }
    if (had_event_handler && !event_attached) {
        const sao_status_t injected = std::exchange(state_->fail_next_event_restore_status, SAO_STATUS_OK);
        const sao_status_t restore_status = injected == SAO_STATUS_OK ?
            sao_ui_panel_set_event_handler(panel, &Owner::panel_event_callback, state_.get()) : injected;
        event_attached = restore_status == SAO_STATUS_OK;
        rollback_ok = rollback_ok && event_attached;
    }
    {
        std::lock_guard lock(state_->mutex);
        state_->action_handler_attached = action_attached;
        state_->event_handler_attached = event_attached;
        state_->accepting = rollback_ok && action_attached == had_action_handler &&
                            event_attached == had_event_handler;
        state_->retiring = false;
    }
    return rollback_ok ? status : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
}

sao_status_t Owner::dispatch_action(std::string_view action_id,
                                     std::string_view payload_json) noexcept {
    if (!state_ || !valid_text(action_id, kMaximumActionIdBytes, true))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->panel == nullptr)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
    }

    try {
        bool valid_payload = false;
        json payload = parse_payload(payload_json, valid_payload);
        if (!valid_payload)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;

        if (action_id == kKeyInputAction) {
            const auto value = payload.find("value");
            const auto text = payload.find("text");
            const json* source = value != payload.end() && value->is_string()
                                     ? &*value
                                     : text != payload.end() && text->is_string() ? &*text
                                                                                  : nullptr;
            if (source == nullptr)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            std::string key = source->get<std::string>();
            if (!valid_text(key, kMaximumKeyBytes, false))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            {
                std::lock_guard lock(state_->mutex);
                state_->license_key = std::move(key);
                state_->last_status = SAO_STATUS_OK;
                state_->publish_pending = true;
            }
            return publish();
        }

        if (action_id == kCopyHwidAction) {
            std::string hwid_hex;
            Operations::CopyToClipboard copy_to_clipboard;
            {
                std::lock_guard lock(state_->mutex);
                hwid_hex = state_->hwid_hex_;
                copy_to_clipboard = state_->operations.copy_to_clipboard;
            }
            if (hwid_hex.empty()) {
                {
                    std::lock_guard lock(state_->mutex);
                    state_->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
                    state_->error_text = "HWID not available yet.";
                    state_->status_text.clear();
                    state_->publish_pending = true;
                }
                const sao_status_t publish_status = publish();
                return publish_status == SAO_STATUS_OK ? SAO_STATUS_ERR_NOT_INITIALIZED
                                                       : publish_status;
            }
            const sao_status_t clip_status = copy_to_clipboard(hwid_hex);
            {
                std::lock_guard lock(state_->mutex);
                state_->last_status = clip_status;
                if (clip_status == SAO_STATUS_OK) {
                    state_->status_text = "HWID copied to clipboard.";
                    state_->error_text.clear();
                } else {
                    state_->error_text = status_description(clip_status, "Copy HWID failed");
                    state_->status_text.clear();
                }
                state_->publish_pending = true;
            }
            const sao_status_t publish_status = publish();
            return publish_status == SAO_STATUS_OK ? clip_status : publish_status;
        }

        if (action_id == kRefreshAction) {
            {
                std::lock_guard lock(state_->mutex);
                if (state_->status_running.load() || state_->activation_running.load())
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                state_->busy = true;
                state_->status_text = "Refreshing license status...";
                state_->error_text.clear();
                state_->publish_pending = true;
            }
            start_status_refresh(*state_);
            return publish();
        }

        if (action_id == kActivateAction || action_id == kSkipAction) {
            std::string key;
            {
                std::lock_guard lock(state_->mutex);
                if (state_->activation_running.load() || state_->status_running.load())
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                key = state_->license_key;
            }
            sao_status_t action_status = SAO_STATUS_OK;
            if (action_id == kSkipAction) {
                std::lock_guard lock(state_->mutex);
                state_->activated = true;
                state_->tier = "free";
                state_->expiry_ms = 0U;
                state_->status_text = "Skipped - using free tier.";
                state_->error_text.clear();
                state_->last_status = SAO_STATUS_OK;
                state_->publish_pending = true;
            } else if (key.empty()) {
                std::lock_guard lock(state_->mutex);
                state_->error_text = "Please enter an activation key.";
                state_->status_text.clear();
                state_->last_status = SAO_STATUS_ERR_INVALID_ARGUMENT;
                state_->publish_pending = true;
                action_status = SAO_STATUS_ERR_INVALID_ARGUMENT;
            } else {
                action_status = run_activation(std::move(key));
            }
            const sao_status_t publish_status = publish();
            return publish_status == SAO_STATUS_OK ? action_status : publish_status;
        }

        return SAO_STATUS_ERR_NOT_FOUND;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t Owner::dispatch_event_for_testing(std::int32_t event_kind) noexcept {
    const sao_status_t owner_status = require_owner_thread();
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->accepting || state_->retiring)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        ++state_->callbacks_in_flight;
    }
    struct CallbackGuard {
        State* state;
        ~CallbackGuard() {
            std::lock_guard lock(state->mutex);
            if (state->callbacks_in_flight != 0U)
                --state->callbacks_in_flight;
            state->callback_cv.notify_all();
        }
    } callback{state_.get()};
    handle_panel_event(event_kind);
    return SAO_STATUS_OK;
}

sao_status_t Owner::run_activation(std::string key) noexcept {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->activation_running.load() || state_->status_running.load())
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
    }
    try {
        if (state_->activation_thread.joinable())
            state_->activation_thread.join();
        {
            std::lock_guard lock(state_->mutex);
            state_->busy = true;
            state_->status_text = "Activating...";
            state_->error_text.clear();
            state_->activation_cancel_requested.store(false);
            state_->activation_running.store(true);
            state_->publish_pending = true;
        }
        state_->activation_thread =
            std::thread(&Owner::activation_thread_main, state_.get(), std::move(key));
        return SAO_STATUS_OK;
    } catch (...) {
        std::lock_guard lock(state_->mutex);
        state_->busy = false;
        state_->activation_running.store(false);
        state_->last_status = SAO_STATUS_ERR_OS_CALL_FAILED;
        state_->status_text.clear();
        state_->error_text = "Activation worker could not start.";
        state_->publish_pending = true;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}
void Owner::activation_thread_main(State* state_ptr, std::string key) noexcept {
    if (state_ptr == nullptr)
        return;
    State& state = *state_ptr;
    sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
    std::string tier;
    std::uint64_t expiry_ms = 0U;
    bool activated = false;
    try {
        status = state.operations.activate(key);
        if (state.activation_cancel_requested.load() && status == SAO_STATUS_OK)
            status = SAO_STATUS_ERR_CANCELLED;
        if (status == SAO_STATUS_OK)
            status = state.operations.refresh_license();
        if (state.activation_cancel_requested.load() && status == SAO_STATUS_OK)
            status = SAO_STATUS_ERR_CANCELLED;
        if (status == SAO_STATUS_OK) {
            status = state.operations.get_status(tier, expiry_ms);
            if (status == SAO_STATUS_OK && valid_text(tier, 64U, true))
                activated = true;
            else if (status == SAO_STATUS_OK)
                status = SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    } catch (...) {
        status = SAO_STATUS_ERR_UNKNOWN;
    }

    if (state.activation_cancel_requested.load() && status == SAO_STATUS_OK)
        status = SAO_STATUS_ERR_CANCELLED;
    {
        std::lock_guard lock(state.mutex);
        state.busy = false;
        state.activation_running.store(false);
        state.last_status = status;
        if (activated) {
            state.activated = true;
            state.tier = std::move(tier);
            state.expiry_ms = expiry_ms;
            state.status_text = "Activation successful.";
            state.error_text.clear();
        } else {
            state.error_text = license_status_description(static_cast<std::int32_t>(status));
            state.status_text.clear();
        }
        state.publish_pending = true;
    }
    state.activation_cv.notify_all();
}

sao_status_t Owner::snapshot(Snapshot& out) const noexcept {
    if (!state_)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    std::lock_guard lock(state_->mutex);
    out.panel_created = state_->panel != nullptr;
    out.visible = state_->visible;
    out.busy = state_->busy;
    out.last_status = state_->last_status;
    out.status_text = state_->status_text;
    out.error_text = state_->error_text;
    out.hwid_hex = state_->hwid_hex_;
    out.license_key = state_->license_key;
    out.tier = state_->tier;
    out.expiry_ms = state_->expiry_ms;
    out.activated = state_->activated;
    out.rendered_spec_json = state_->rendered_spec_json;
    return SAO_STATUS_OK;
}

} // namespace sao::launcher::license_panel
