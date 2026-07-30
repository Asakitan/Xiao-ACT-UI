// license_panel_internal.cpp — native activation panel implementation.
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
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace sao::launcher::license_panel {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaximumPanelSpecBytes = 64U * 1024U;
constexpr std::size_t kMaximumActionPayloadBytes = 4096U;
constexpr std::size_t kMaximumKeyBytes = 256U;
constexpr std::size_t kMaximumStatusBytes = 1024U;

constexpr char kSolidDarkCyanTheme[] =
    R"({"colors":{"APP_BG":"#0E1418","APP_CARD":"#162024","APP_BORDER":"#1E2E32","APP_TEXT":"#E0EAE6","APP_TEXT_2":"#8FA8A2","APP_TEXT_DIM":"#6E7C78","APP_ACCENT":"#2FA9B8","APP_BLUE":"#2FA9B8","APP_GREEN":"#5EAA6C","APP_RED":"#D04040","APP_ORANGE":"#D4A520","APP_GOLD":"#D4A520"}})";

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
    return std::to_string(expiry_ms / 1000U) + " (UTC s)";
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

json card_node(std::string title, json children, std::string_view accent = "cyan") {
    return json{{"type", "card"},
                {"title", bounded_text(std::move(title), 512U)},
                {"accent", accent},
                {"children", std::move(children)}};
}

std::string build_panel_spec(const Snapshot& snapshot) {
    json nodes = json::array();

    // Status card — tier / expiry / hwid.
    json status_rows = json::array();
    {
        json row = json::array();
        row.push_back(text_node("授权等级 (Tier)", "muted", 22));
        row.push_back(text_node(snapshot.activated ? snapshot.tier : "—",
                                snapshot.activated ? "accent" : "muted", 22));
        status_rows.push_back(row_node(std::move(row)));
    }
    {
        json row = json::array();
        row.push_back(text_node("到期时间 (Expiry)", "muted", 22));
        row.push_back(text_node(snapshot.activated ? format_expiry(snapshot.expiry_ms) : "—",
                                snapshot.activated ? "accent" : "muted", 22));
        status_rows.push_back(row_node(std::move(row)));
    }
    {
        json row = json::array();
        row.push_back(text_node("设备指纹 (HWID)", "muted", 22));
        row.push_back(text_node(snapshot.hwid_hex.empty() ? "—" : snapshot.hwid_hex,
                                "mono", 22));
        status_rows.push_back(row_node(std::move(row)));
    }
    {
        json row = json::array();
        row.push_back(button_node("license.copy_hwid", "Copy HWID", kCopyHwidAction,
                                  json::object(), "ghost",
                                  snapshot.hwid_hex.empty() || snapshot.busy));
        status_rows.push_back(row_node(std::move(row)));
    }
    nodes.push_back(card_node("License Status", std::move(status_rows), "cyan"));

    // Activation card — key input + activate button.
    json activate_rows = json::array();
    {
        json row = json::array();
        row.push_back(input_node("license.key_input", snapshot.license_key, kKeyInputAction));
        row.push_back(button_node("license.activate",
                                   snapshot.busy ? "Activating..." : "Activate",
                                   kActivateAction, json::object(), "primary",
                                   snapshot.busy));
        activate_rows.push_back(row_node(std::move(row)));
    }
    if (snapshot.busy) {
        activate_rows.push_back(text_node("Verifying with license server...", "muted", 22));
    }
    nodes.push_back(card_node("Activation", std::move(activate_rows), "cyan"));

    // Status / error message card.
    if (!snapshot.error_text.empty()) {
        json msg = json::array();
        msg.push_back(text_node(snapshot.error_text, "bad", 28));
        nodes.push_back(card_node("Error", std::move(msg), "bad"));
    } else if (!snapshot.status_text.empty()) {
        json msg = json::array();
        msg.push_back(text_node(snapshot.status_text, "muted", 28));
        nodes.push_back(card_node("Message", std::move(msg), "cyan"));
    }

    std::string serialized =
        json{{"version", 1}, {"title", ""}, {"nodes", std::move(nodes)}}.dump();
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
        return static_cast<sao_status_t>(SAO_STATUS_ERR_OS_CALL_FAILED);
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
        return static_cast<sao_status_t>(SAO_STATUS_ERR_OS_CALL_FAILED);
    std::uint64_t expiry_ms = 0U;
    status = sao_license_sdk_get_expiry_ms(&expiry_ms);
    if (status != SAO_OK)
        return static_cast<sao_status_t>(SAO_STATUS_ERR_OS_CALL_FAILED);
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
    const int32_t status = sao_license_sdk_refresh();
    return status == SAO_OK ? SAO_STATUS_OK
                            : static_cast<sao_status_t>(SAO_STATUS_ERR_OS_CALL_FAILED);
}

json parse_payload(std::string_view payload_json, bool& valid) {
    valid = false;
    if (payload_json.size() > kMaximumActionPayloadBytes)
        return {};
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
        if (key.empty())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (key.size() > kMaximumKeyBytes)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::string owned(key);
        const int32_t status = sao_license_client_activate(owned.c_str());
        return status == SAO_OK ? SAO_STATUS_OK
                                 : static_cast<sao_status_t>(SAO_STATUS_ERR_OS_CALL_FAILED);
    };
    operations.get_hwid = &default_get_hwid;
    operations.get_status = &default_get_status;
    operations.copy_to_clipboard = &default_copy_to_clipboard;
    operations.refresh_license = &default_refresh_license;
    return operations;
}

struct Owner::State {
    explicit State(sao_ui_compositor_handle_t borrowed_compositor, Operations value)
        : compositor(borrowed_compositor), operations(std::move(value)) {
        refresh_hwid();
    }

    void refresh_hwid() {
        std::string hwid_hex;
        const sao_status_t status = operations.get_hwid(hwid_hex);
        if (status == SAO_STATUS_OK)
            hwid_hex_ = bounded_text(std::move(hwid_hex), 128U);
    }

    sao_ui_compositor_handle_t compositor{};
    Operations operations;
    mutable std::mutex mutex;
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
    bool busy{};
    bool activated{};
    sao_status_t last_status{SAO_STATUS_OK};
    std::string status_text;
    std::string error_text;
    std::string hwid_hex_;
    std::string license_key;
    std::string tier;
    std::uint64_t expiry_ms{};
    std::atomic<bool> activation_running{false};
    std::thread activation_thread;
};

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
    : state_(std::make_unique<State>(compositor, std::move(operations))) {}

Owner::~Owner() noexcept {
    if (state_) {
        if (state_->activation_thread.joinable())
            state_->activation_thread.join();
    }
}

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

void Owner::panel_action_callback(const char* action_id_utf8,
                                   const std::uint8_t* payload_json_utf8,
                                   std::size_t payload_len, void* user_data) noexcept {
    auto* owner = static_cast<Owner*>(user_data);
    if (owner == nullptr || action_id_utf8 == nullptr ||
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
        (void)owner->dispatch_action(action_id_utf8, payload);
    } catch (...) {
    }
}

void Owner::panel_event_callback(std::int32_t event_kind, void* user_data) noexcept {
    auto* owner = static_cast<Owner*>(user_data);
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
        if (state_->panel != nullptr)
            return SAO_STATUS_OK;
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
    descriptor.theme_override_json_utf8 = kSolidDarkCyanTheme;
    descriptor.initial_opacity = 1.0F;
    descriptor.auto_scroll = true;

    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    sao_status_t status = sao_ui_panel_register(state_->compositor, &descriptor, &panel, &body);
    if (status != SAO_STATUS_OK)
        return status;

    bool action_attached = false;
    bool event_attached = false;
    status = sao_ui_panel_set_action_handler(panel, &Owner::panel_action_callback, this);
    action_attached = status == SAO_STATUS_OK;
    if (status == SAO_STATUS_OK) {
        status = sao_ui_panel_set_event_handler(panel, &Owner::panel_event_callback, this);
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
            body = state_->body;
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
        return status;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t Owner::open() noexcept {
    OperationGuard operation(*this);
    if (operation.status != SAO_STATUS_OK)
        return operation.status;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;

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
        if (state_->activation_running.load() == false && state_->busy) {
            // Activation thread finished; drain results.
            publish_needed = true;
        }
    }
    if (publish_needed)
        return publish();
    return SAO_STATUS_OK;
}

sao_status_t Owner::take_offline() noexcept {
    const sao_status_t owner_status = require_owner_thread();
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->panel == nullptr)
            return SAO_STATUS_OK;
        if (state_->operations_in_flight != 0U || state_->callbacks_in_flight != 0U ||
            state_->activation_running.load()) {
            state_->retiring = true;
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        }
        state_->retiring = true;
        panel = state_->panel;
        body = state_->body;
    }
    if (state_->activation_thread.joinable())
        state_->activation_thread.join();

    sao_status_t rollback_status = SAO_STATUS_OK;
    if (state_->event_handler_attached) {
        const sao_status_t detach_status =
            sao_ui_panel_set_event_handler(panel, nullptr, nullptr);
        if (detach_status != SAO_STATUS_OK)
            rollback_status = detach_status;
        else
            state_->event_handler_attached = false;
    }
    if (state_->action_handler_attached) {
        const sao_status_t detach_status =
            sao_ui_panel_set_action_handler(panel, nullptr, nullptr);
        if (detach_status != SAO_STATUS_OK) {
            if (rollback_status == SAO_STATUS_OK)
                rollback_status = detach_status;
        } else {
            state_->action_handler_attached = false;
        }
    }
    const sao_status_t unregister_status = sao_ui_panel_unregister(panel);
    {
        std::lock_guard lock(state_->mutex);
        state_->panel = nullptr;
        state_->body = nullptr;
        state_->accepting = false;
        state_->retiring = false;
    }
    if (unregister_status != SAO_STATUS_OK)
        return rollback_status == SAO_STATUS_OK ? unregister_status
                                                : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
    (void)body;
    return rollback_status;
}

sao_status_t Owner::dispatch_action(std::string_view action_id,
                                     std::string_view payload_json) noexcept {
    if (action_id.empty())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    bool valid_payload = false;
    json payload = parse_payload(payload_json, valid_payload);
    if (!valid_payload)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;

    // The input widget reports its current text in the payload when the user
    // commits (Enter) or the field loses focus. Mirror it into the stored key.
    if (action_id == kKeyInputAction) {
        if (payload.contains("value") && payload["value"].is_string()) {
            std::lock_guard lock(state_->mutex);
            state_->license_key = bounded_text(payload["value"].get<std::string>(),
                                                kMaximumKeyBytes);
        } else if (payload.contains("text") && payload["text"].is_string()) {
            std::lock_guard lock(state_->mutex);
            state_->license_key = bounded_text(payload["text"].get<std::string>(),
                                                kMaximumKeyBytes);
        }
        return publish();
    }

    if (action_id == kCopyHwidAction) {
        std::string hwid_hex;
        {
            std::lock_guard lock(state_->mutex);
            hwid_hex = state_->hwid_hex_;
        }
        if (hwid_hex.empty()) {
            std::lock_guard lock(state_->mutex);
            state_->error_text = "HWID not available yet.";
            state_->status_text.clear();
        }
        const sao_status_t clip_status = state_->operations.copy_to_clipboard(hwid_hex);
        {
            std::lock_guard lock(state_->mutex);
            if (clip_status == SAO_STATUS_OK) {
                state_->status_text = "HWID copied to clipboard.";
                state_->error_text.clear();
            } else {
                state_->error_text = status_description(clip_status, "Copy HWID failed");
                state_->status_text.clear();
            }
        }
        return publish();
    }

    if (action_id == kRefreshAction) {
        {
            std::lock_guard lock(state_->mutex);
            state_->refresh_hwid();
            std::string tier;
            std::uint64_t expiry_ms = 0U;
            const sao_status_t status = state_->operations.get_status(tier, expiry_ms);
            if (status == SAO_STATUS_OK) {
                state_->tier = std::move(tier);
                state_->expiry_ms = expiry_ms;
                state_->activated = true;
                state_->status_text = "Status refreshed.";
                state_->error_text.clear();
            } else {
                state_->error_text = status_description(status, "Status refresh failed");
                state_->status_text.clear();
            }
        }
        return publish();
    }

    if (action_id == kActivateAction || action_id == kSkipAction) {
        if (state_->activation_running.load())
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        std::string key;
        {
            std::lock_guard lock(state_->mutex);
            key = state_->license_key;
        }
        if (action_id == kSkipAction) {
            // Skip = accept free tier, no activation needed.
            std::lock_guard lock(state_->mutex);
            state_->activated = true;
            state_->tier = "free";
            state_->expiry_ms = 0U;
            state_->status_text = "Skipped — using free tier.";
            state_->error_text.clear();
            state_->last_status = SAO_STATUS_OK;
        } else {
            if (key.empty()) {
                std::lock_guard lock(state_->mutex);
                state_->error_text = "请输入激活码 (Please enter an activation key).";
                state_->status_text.clear();
                state_->last_status = SAO_STATUS_ERR_INVALID_ARGUMENT;
            } else {
                run_activation(std::move(key));
            }
        }
        return publish();
    }

    return SAO_STATUS_ERR_INVALID_ARGUMENT;
}

void Owner::run_activation(std::string key) noexcept {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->activation_running.load())
            return;
        state_->busy = true;
        state_->status_text = "Activating...";
        state_->error_text.clear();
        state_->activation_running.store(true);
    }
    if (state_->activation_thread.joinable())
        state_->activation_thread.join();
    state_->activation_thread = std::thread(&Owner::activation_thread_main, this,
                                             std::move(key));
}

void Owner::activation_thread_main(Owner* owner, std::string key) noexcept {
    if (owner == nullptr || owner->state_ == nullptr)
        return;
    State& state = *owner->state_;
    sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
    std::string tier;
    std::uint64_t expiry_ms = 0U;
    bool activated = false;
    try {
        status = state.operations.activate(key);
        if (status == SAO_STATUS_OK) {
            status = state.operations.get_status(tier, expiry_ms);
            if (status == SAO_STATUS_OK)
                activated = true;
        }
    } catch (...) {
        status = SAO_STATUS_ERR_UNKNOWN;
    }

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
            // Best-effort live refresh so feature flags update without restart.
            (void)state.operations.refresh_license();
        } else {
            state.error_text = license_status_description(static_cast<int32_t>(status));
            state.status_text.clear();
        }
    }
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
    return SAO_STATUS_OK;
}

} // namespace sao::launcher::license_panel