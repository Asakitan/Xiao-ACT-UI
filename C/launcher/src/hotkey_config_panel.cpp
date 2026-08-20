#include "hotkey_config_panel.h"

#include "hotkey_manager.h"

#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER) || \
    defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#include "sao/sdk/sao_sdk_platform_internal.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sao::launcher::hotkey {
namespace {

using Json = nlohmann::json;
constexpr std::array<int, 11> kModifierKeys{VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT};
constexpr int kFirstVirtualKey = 1;
constexpr int kLastVirtualKey = 254;

std::string status_label(PanelStatus status) {
    switch (status) {
    case PanelStatus::capturing: return "Capturing...";
    case PanelStatus::success: return "Success";
    case PanelStatus::cancelled: return "Capture cancelled";
    case PanelStatus::conflict: return "Conflict";
    case PanelStatus::system_error: return "System error";
    case PanelStatus::save_error: return "Save failed; restored";
    case PanelStatus::ready: default: return "Ready";
    }
}

std::string status_style(PanelStatus status) {
    switch (status) {
    case PanelStatus::capturing: return "warn";
    case PanelStatus::success: return "ok";
    case PanelStatus::conflict:
    case PanelStatus::system_error:
    case PanelStatus::save_error: return "bad";
    case PanelStatus::cancelled: return "muted";
    case PanelStatus::ready: default: return "muted";
    }
}

Json text_node(std::string text, std::string_view style = "value", int height = 24) {
    return Json{{"type", "text"}, {"text", std::move(text)}, {"style", style}, {"height", height}};
}

Json button_node(std::string id, std::string label, std::string action, Json payload,
                 std::string_view style = "default", bool disabled = false) {
    Json node{{"type", "button"}, {"id", std::move(id)}, {"label", std::move(label)},
              {"action", std::move(action)}, {"style", style}, {"height", 28}};
    if (!payload.is_null()) node["payload"] = std::move(payload);
    if (disabled) node["disabled"] = true;
    return node;
}

Json row_node(Json children) {
    return Json{{"type", "row"}, {"align", "left"}, {"children", std::move(children)}};
}

Json card_node(std::string title, Json children, std::string_view accent = "cyan") {
    return Json{{"type", "card"}, {"title", std::move(title)}, {"accent", accent},
                {"children", std::move(children)}};
}

std::string payload_id(std::string_view payload_json) {
    const Json payload = Json::parse(payload_json.begin(), payload_json.end(), nullptr, false);
    return payload.is_string() ? payload.get<std::string>() : std::string();
}

struct CaptureResult {
    std::uint64_t generation{};
    std::string id;
    bool cancelled{};
    bool system_error{};
    bool timed_out{};
    std::string error_message;
    std::uint32_t vk{};
    std::uint32_t modifiers{};
};

} // namespace

SaoPanelDescriptor panel_descriptor_for_testing() noexcept {
    SaoPanelDescriptor descriptor{};
    descriptor.struct_size = sizeof(SaoPanelDescriptor);
    descriptor.panel_id_utf8 = kPanelId;
    descriptor.title_utf8 = kPanelTitle;
    descriptor.anchor = SAO_UI_PANEL_ANCHOR_CENTER;
    descriptor.default_width_px = 780;
    descriptor.default_height_px = 540;
    descriptor.min_width_px = 620;
    descriptor.min_height_px = 320;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = false;
    descriptor.remember_geometry = true;
    descriptor.z_class = SAO_UI_PANEL_Z_NORMAL;
    descriptor.auto_scroll = true;
    descriptor.initial_opacity = 1.0F;
    descriptor.theme_override_json_utf8 = nullptr;
    return descriptor;
}

struct Owner::Impl final : std::enable_shared_from_this<Owner::Impl> {
    enum class RetirementState : std::uint8_t { offline, online, retiring, retry_pending, deferred };
    static std::mutex deferred_mutex;
    static std::vector<std::shared_ptr<Impl>> deferred_cleanup;

    explicit Impl(sao_ui_compositor_handle_t value) noexcept : compositor(value) {}

    struct CallbackLease final {
        explicit CallbackLease(Impl* value) noexcept : state(value) {
            if (state == nullptr) return;
            std::lock_guard lock(state->mutex);
            if (!state->accepting || state->retirement != RetirementState::online) return;
            ++state->callbacks_in_flight;
            active = true;
        }
        ~CallbackLease() {
            if (!active) return;
            std::lock_guard lock(state->mutex);
            --state->callbacks_in_flight;
        }
        explicit operator bool() const noexcept { return active; }
        Impl* state{};
        bool active{};
    };



    static void SAO_UI_CALL action_callback(const char* action, const std::uint8_t* payload,
                                            std::size_t length, void* user_data) {
        auto* state = static_cast<Impl*>(user_data);
        CallbackLease lease(state);
        if (!lease || action == nullptr || (payload == nullptr && length != 0U)) return;
        const std::string_view json(payload == nullptr ? "" : reinterpret_cast<const char*>(payload),
                                    payload == nullptr ? 0U : length);
        (void)state->dispatch(action, json);
    }

    static void SAO_UI_CALL event_callback(std::int32_t event_kind, void* user_data) {
        auto* state = static_cast<Impl*>(user_data);
        CallbackLease lease(state);
        if (lease) (void)state->dispatch_event(event_kind);
    }

    sao_status_t require_owner_thread() const noexcept {
        return compositor == nullptr ? SAO_STATUS_ERR_INVALID_ARGUMENT
                                      : sao_ui_compositor_require_owner_thread(compositor);
    }

    short key_state(int vk, const CaptureHooks& hooks) const noexcept {
        if (hooks.get_async_key_state) return hooks.get_async_key_state(vk);
#if defined(_WIN32)
        return GetAsyncKeyState(vk);
#else
        (void)vk;
        return 0;
#endif
    }

    std::uint32_t modifiers(const CaptureHooks& hooks) const noexcept {
        const auto down = [&](int vk) { return (key_state(vk, hooks) & 0x8000) != 0; };
        std::uint32_t value = 0;
        if (down(VK_CONTROL) || down(VK_LCONTROL) || down(VK_RCONTROL)) value |= MOD_CONTROL;
        if (down(VK_MENU) || down(VK_LMENU) || down(VK_RMENU)) value |= MOD_ALT;
        if (down(VK_SHIFT) || down(VK_LSHIFT) || down(VK_RSHIFT)) value |= MOD_SHIFT;
        if (down(VK_LWIN) || down(VK_RWIN)) value |= MOD_WIN;
        return value;
    }

    void capture_worker(std::uint64_t generation, std::string id, CaptureHooks hooks,
                        std::stop_token stop_token) {
        std::array<bool, kLastVirtualKey + 1> baseline{};
        for (int vk = kFirstVirtualKey; vk <= kLastVirtualKey; ++vk) {
            if (std::find(kModifierKeys.begin(), kModifierKeys.end(), vk) == kModifierKeys.end())
                baseline[static_cast<std::size_t>(vk)] = (key_state(vk, hooks) & 0x8000) != 0;
        }
        CaptureResult result{generation, std::move(id)};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!stop_token.stop_requested()) {
            for (int vk = kFirstVirtualKey; vk <= kLastVirtualKey; ++vk) {
                if (std::find(kModifierKeys.begin(), kModifierKeys.end(), vk) != kModifierKeys.end())
                    continue;
                const bool down = (key_state(vk, hooks) & 0x8000) != 0;
                if (!down || baseline[static_cast<std::size_t>(vk)]) {
                    baseline[static_cast<std::size_t>(vk)] = down;
                    continue;
                }
                baseline[static_cast<std::size_t>(vk)] = true;
                result.cancelled = vk == VK_ESCAPE;
                if (!result.cancelled) {
                    result.vk = static_cast<std::uint32_t>(vk);
                    result.modifiers = modifiers(hooks);
                }
                publish_capture(std::move(result));
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result.timed_out = true;
                publish_capture(std::move(result));
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void publish_capture(CaptureResult result) {
        CaptureHooks hooks;
        void* wake_window = nullptr;
        {
            std::lock_guard lock(mutex);
            if (result.generation != capture_generation || panel == nullptr || retirement != RetirementState::online)
                return;
            pending_capture = std::move(result);
            capture_running = false;
            hooks = capture_hooks;
            wake_window = owner_wake_window;
        }

        bool wake_posted = false;
        if (hooks.post_owner_wake) {
            wake_posted = hooks.post_owner_wake();
        } else {
#if defined(_WIN32)
            wake_posted = wake_window != nullptr &&
                          PostMessageW(static_cast<HWND>(wake_window),
                                       kCaptureCompletionMessage, 0, 0) != FALSE;
#else
            (void)wake_window;
#endif
        }
        if (!wake_posted) {
            std::lock_guard lock(mutex);
            if (pending_capture.has_value() &&
                pending_capture->generation == capture_generation) {
                pending_capture->system_error = true;
                pending_capture->error_message = "Capture completion wake failed";
            }
            capture_running = false;
        }
    }

    void complete_capture(const CaptureResult& result) {
        {
            std::lock_guard lock(mutex);
            if (result.generation != capture_generation || panel == nullptr || retirement != RetirementState::online) return;
        }
        if (result.timed_out) {
            set_status(result.id, PanelStatus::cancelled, "Capture timed out");
        } else if (result.system_error) {
            set_status(result.id, PanelStatus::system_error,
                       result.error_message.empty() ? "Capture completion failed"
                                                    : result.error_message);
        } else if (result.cancelled) {
            set_status(result.id, PanelStatus::cancelled, "Capture cancelled");
        } else {
            std::string reason;
            const RebindResult result_code = rebind_live(result.id, result.vk, result.modifiers, &reason);
            if (result_code == RebindResult::success)
                set_status(result.id, PanelStatus::success, "Saved");
            else if (result_code == RebindResult::conflict)
                set_status(result.id, PanelStatus::conflict, "Conflict with " + reason);
            else if (result_code == RebindResult::save_error)
                set_status(result.id, PanelStatus::save_error, reason);
            else
                set_status(result.id, PanelStatus::system_error, reason);
        }
        (void)refresh_now();
    }

    void set_status(const std::string& id, PanelStatus status, std::string message) {
        std::lock_guard lock(mutex);
        row_status[id] = status;
        status_message = std::move(message);
    }

    void stop_capture() noexcept {
        std::jthread old;
        {
            std::lock_guard lock(mutex);
            ++capture_generation;
            capture_running = false;
            pending_capture.reset();
            capturing_binding_id.clear();
            if (capture_thread.joinable()) {
                capture_thread.request_stop();
                old = std::move(capture_thread);
            }
        }
        if (old.joinable()) old.join();
    }

    sao_status_t start_capture(const std::string& id) noexcept {
        if (!query_binding(id).has_value()) return SAO_STATUS_ERR_NOT_FOUND;
        std::string previous_id;
        { std::lock_guard lock(mutex); previous_id = capturing_binding_id; }
        stop_capture();
        CaptureHooks hooks;
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex);
            if (panel == nullptr || retirement != RetirementState::online) return SAO_STATUS_ERR_NOT_INITIALIZED;
            generation = ++capture_generation;
            hooks = capture_hooks;
            pending_capture.reset();
            if (!previous_id.empty() && previous_id != id)
                row_status[previous_id] = PanelStatus::ready;
            capturing_binding_id = id;
            capture_running = true;
            row_status[id] = PanelStatus::capturing;
            status_message = "Press a key; Esc cancels";
        }
        try {
            auto self = shared_from_this();
            std::lock_guard lock(mutex);
            capture_thread = std::jthread([self, generation, id, hooks](std::stop_token token) mutable {
                self->capture_worker(generation, id, std::move(hooks), token);
            });
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                capture_running = false;
                capturing_binding_id.clear();
            }
            set_status(id, PanelStatus::system_error, "Capture worker could not start");
            return SAO_STATUS_ERR_UNKNOWN;
        }
        return refresh_now();
    }

    sao_status_t ensure_panel() noexcept {
        const SaoPanelDescriptor descriptor = panel_descriptor_for_testing();
        sao_ui_panel_handle_t created_panel = nullptr;
        sao_ui_panel_body_handle_t created_body = nullptr;
        sao_status_t status = sao_ui_panel_register(compositor, &descriptor, &created_panel, &created_body);
        if (status != SAO_STATUS_OK) return status;
        status = sao_ui_panel_set_action_handler(created_panel, &action_callback, this);
        if (status == SAO_STATUS_OK) status = sao_ui_panel_set_event_handler(created_panel, &event_callback, this);
        if (status != SAO_STATUS_OK) {
            (void)sao_ui_panel_set_action_handler(created_panel, nullptr, nullptr);
            (void)sao_ui_panel_set_event_handler(created_panel, nullptr, nullptr);
            (void)sao_ui_panel_unregister(created_panel);
            return status;
        }
        std::lock_guard lock(mutex);
        panel = created_panel;
        body = created_body;
        accepting = true;
        action_handler_attached = true;
        event_handler_attached = true;
        retirement = RetirementState::online;
        cleanup_pending = false;
        creating = false;
        return SAO_STATUS_OK;
    }

    sao_status_t open() noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK) return owner_status;
        sao_ui_panel_handle_t existing = nullptr;
        {
            std::lock_guard lock(mutex);
            if (creating || retirement == RetirementState::retiring || retirement == RetirementState::retry_pending || cleanup_pending) return SAO_UI_PANEL_STATUS_ERR_BUSY;
            existing = panel;
            if (existing == nullptr) creating = true;
        }
        if (existing == nullptr) {
            const sao_status_t status = ensure_panel();
            if (status != SAO_STATUS_OK) {
                std::lock_guard lock(mutex);
                creating = false;
                return status;
            }
            existing = panel_handle();
        }
        const sao_status_t refresh_status = refresh_now();
        if (refresh_status != SAO_STATUS_OK) return refresh_status;
        sao_status_t status = sao_ui_panel_show(existing);
        if (status == SAO_STATUS_OK) status = sao_ui_panel_bring_to_front(existing);
        return status;
    }

    sao_status_t close() noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK) return owner_status;
        stop_capture();
        const auto target = panel_handle();
        return target == nullptr ? SAO_STATUS_OK : sao_ui_panel_hide(target);
    }

    sao_status_t take_offline() noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK) return owner_status;
        stop_capture();
        sao_ui_panel_handle_t target = nullptr;
        {
            std::lock_guard lock(mutex);
            if (panel == nullptr) return SAO_STATUS_OK;
            if (retirement == RetirementState::retiring || callbacks_in_flight != 0U) return SAO_UI_PANEL_STATUS_ERR_BUSY;
            retirement = RetirementState::retiring;
            accepting = false;
            target = panel;
        }
        bool action_detached = false;
        bool event_detached = false;
        sao_status_t status = sao_ui_panel_set_action_handler(target, nullptr, nullptr);
        if (status == SAO_STATUS_OK) {
            action_detached = true;
            status = sao_ui_panel_set_event_handler(target, nullptr, nullptr);
        }
        if (status == SAO_STATUS_OK) {
            event_detached = true;
            sao_status_t injected = std::exchange(fail_next_unregister_status, SAO_STATUS_OK);
            status = injected == SAO_STATUS_OK ? sao_ui_panel_unregister(target) : injected;
        }
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(mutex);
            panel = nullptr;
            body = nullptr;
            accepting = true;
            cleanup_pending = false;
            retirement = RetirementState::offline;
            return SAO_STATUS_OK;
        }
        sao_status_t rollback_status = SAO_STATUS_OK;
        if (event_detached) {
            const sao_status_t injected = std::exchange(fail_next_event_restore_status, SAO_STATUS_OK);
            rollback_status = injected == SAO_STATUS_OK ? sao_ui_panel_set_event_handler(target, &event_callback, this) : injected;
        }
        if (action_detached) {
            const sao_status_t injected = std::exchange(fail_next_action_restore_status, SAO_STATUS_OK);
            const sao_status_t restore_status = injected == SAO_STATUS_OK ? sao_ui_panel_set_action_handler(target, &action_callback, this) : injected;
            if (rollback_status == SAO_STATUS_OK)
                rollback_status = restore_status;
        }
        {
            std::lock_guard lock(mutex);
            retirement = RetirementState::retry_pending;
            cleanup_pending = true;
            accepting = false;
        }
        return rollback_status == SAO_STATUS_OK ? status : rollback_status;
    }

    void fail_next_unregister(sao_status_t status) noexcept { std::lock_guard lock(mutex); fail_next_unregister_status = status; }
    void fail_next_handler_restore(sao_status_t action_status, sao_status_t event_status) noexcept { std::lock_guard lock(mutex); fail_next_action_restore_status = action_status; fail_next_event_restore_status = event_status; }
    static void defer_cleanup(std::shared_ptr<Impl> state) noexcept { if (state == nullptr) return; { std::lock_guard lock(state->mutex); state->accepting = false; state->cleanup_pending = true; state->retirement = RetirementState::deferred; } std::lock_guard lock(deferred_mutex); deferred_cleanup.push_back(std::move(state)); }
    static void drain_deferred_cleanup() noexcept { std::vector<std::shared_ptr<Impl>> pending; { std::lock_guard lock(deferred_mutex); pending.swap(deferred_cleanup); } std::vector<std::shared_ptr<Impl>> retry; for (auto& state : pending) { if (state->take_offline() != SAO_STATUS_OK) retry.push_back(std::move(state)); } if (!retry.empty()) { std::lock_guard lock(deferred_mutex); for (auto& state : retry) deferred_cleanup.push_back(std::move(state)); } }

    sao_status_t set_owner_wake_window(void* window) noexcept {
        std::lock_guard lock(mutex);
        owner_wake_window = window;
        return SAO_STATUS_OK;
    }

    sao_status_t drain_capture_for_owner() noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK) return owner_status;
        std::optional<CaptureResult> result;
        {
            std::lock_guard lock(mutex);
            result = std::move(pending_capture);
            pending_capture.reset();
        }
        if (result.has_value()) complete_capture(*result);
        return SAO_STATUS_OK;
    }

    sao_status_t refresh_now() noexcept {
        sao_ui_panel_body_handle_t target_body = nullptr;
        std::unordered_map<std::string, PanelStatus> statuses;
        std::string message;
        {
            std::lock_guard lock(mutex);
            target_body = body;
            statuses = row_status;
            message = status_message;
        }
        if (target_body == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
        Json nodes = Json::array();
        if (!message.empty()) {
            Json status_nodes = Json::array({text_node(message, "accent", 28)});
            if (capture_running)
                status_nodes.push_back(button_node("capture.cancel", "Cancel capture", "hotkey.capture.cancel", Json::object(), "ghost"));
            nodes.push_back(card_node("Status", std::move(status_nodes)));
        }
        Json rows = Json::array();
        for (const auto& binding : snapshot()) {
            const auto found = statuses.find(binding.id);
            const PanelStatus status = found == statuses.end() ? PanelStatus::ready : found->second;
            rows.push_back(row_node(Json::array({
                text_node(binding.description, "value", 28),
                text_node(binding.id, "mono", 28),
                text_node(format_combo_utf8(binding.vk, binding.modifiers), "accent", 28),
                Json{{"type", "badge"}, {"text", status_label(status)}, {"style", status_style(status)}, {"height", 22}},
                button_node("capture." + binding.id, "Capture", kCaptureAction, Json(binding.id), "primary",
                             capture_running && capturing_binding_id != binding.id),
                button_node("reset." + binding.id, "Reset", kResetAction, Json(binding.id), "ghost", capture_running),
            })));
        }
        nodes.push_back(card_node("Bindings", std::move(rows)));
        const std::string spec = Json{{"version", 1}, {"title", ""}, {"surface", "solid"}, {"nodes", std::move(nodes)}}.dump();
        return sao_ui_panel_body_set_spec(target_body, reinterpret_cast<const std::uint8_t*>(spec.data()), spec.size());
    }

    sao_status_t dispatch(std::string_view action, std::string_view payload_json) noexcept {
        const std::string id = payload_id(payload_json);
        if (id.empty()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (action == "hotkey.capture.cancel") {
            std::string old_id;
            { std::lock_guard lock(mutex); old_id = capturing_binding_id; }
            stop_capture();
            if (!old_id.empty()) set_status(old_id, PanelStatus::cancelled, "Capture cancelled");
            return refresh_now();
        }
        if (action == kCaptureAction) return start_capture(id);
        if (action == kResetAction) {
            stop_capture();
            std::string reason;
            const RebindResult result = reset_to_default(id, &reason);
            if (result == RebindResult::success) set_status(id, PanelStatus::success, "Reset and saved");
            else if (result == RebindResult::conflict) set_status(id, PanelStatus::conflict, "Conflict with " + reason);
            else if (result == RebindResult::save_error) set_status(id, PanelStatus::save_error, reason);
            else set_status(id, PanelStatus::system_error, reason);
            return refresh_now();
        }
        return SAO_STATUS_ERR_NOT_FOUND;
    }

    sao_status_t dispatch_event(std::int32_t event_kind) noexcept {
        return event_kind == SAO_UI_PANEL_EVENT_CLOSE ? close() : SAO_STATUS_OK;
    }

    sao_status_t dispatch_action_for_testing(std::string_view action, std::string_view payload) noexcept {
        const auto owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK) return owner_status;
        CallbackLease lease(this);
        return lease ? dispatch(action, payload) : SAO_UI_PANEL_STATUS_ERR_BUSY;
    }

    sao_status_t dispatch_event_for_testing(std::int32_t event_kind) noexcept {
        const auto owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK) return owner_status;
        CallbackLease lease(this);
        return lease ? dispatch_event(event_kind) : SAO_UI_PANEL_STATUS_ERR_BUSY;
    }

    sao_ui_panel_handle_t panel_handle() const noexcept {
        std::lock_guard lock(mutex);
        return panel;
    }

    bool is_capturing() const noexcept {
        std::lock_guard lock(mutex);
        return capture_running;
    }

    void preserve_for_non_owner_destruction() noexcept {
        std::lock_guard lock(mutex);
        accepting = false;
        cleanup_pending = true;
        retirement = RetirementState::deferred;
    }
    sao_status_t set_capture_hooks(CaptureHooks hooks) noexcept {
        std::lock_guard lock(mutex);
        capture_hooks = std::move(hooks);
        return SAO_STATUS_OK;
    }

    sao_ui_compositor_handle_t compositor{};
    mutable std::mutex mutex;
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    std::unordered_map<std::string, PanelStatus> row_status;
    std::string status_message;
    CaptureHooks capture_hooks;
    std::jthread capture_thread;
    std::optional<CaptureResult> pending_capture;
    std::uint64_t capture_generation{};
    std::string capturing_binding_id;
    void* owner_wake_window{};
    std::size_t callbacks_in_flight{};
    bool capture_running{};
    sao_status_t fail_next_unregister_status{SAO_STATUS_OK};
    sao_status_t fail_next_action_restore_status{SAO_STATUS_OK};
    sao_status_t fail_next_event_restore_status{SAO_STATUS_OK};
    bool accepting{};
    bool creating{};
    bool action_handler_attached{};
    bool event_handler_attached{};
    bool cleanup_pending{};
    RetirementState retirement{RetirementState::offline};
};

std::mutex Owner::Impl::deferred_mutex;
std::vector<std::shared_ptr<Owner::Impl>> Owner::Impl::deferred_cleanup;

std::string format_combo_utf8(std::uint32_t vk, std::uint32_t modifiers) {
    std::string out;
#if defined(_WIN32)
    if (modifiers & MOD_CONTROL) out += "Ctrl+";
    if (modifiers & MOD_ALT) out += "Alt+";
    if (modifiers & MOD_SHIFT) out += "Shift+";
    if (modifiers & MOD_WIN) out += "Win+";
#endif
    if (vk >= 'A' && vk <= 'Z') out += static_cast<char>(vk);
    else if (vk >= '0' && vk <= '9') out += static_cast<char>(vk);
    else if (vk == VK_HOME) out += "Home";
    else if (vk == VK_INSERT) out += "Insert";
    else if (vk == VK_DELETE) out += "Delete";
    else if (vk == VK_ESCAPE) out += "Esc";
    else if (vk == VK_SPACE) out += "Space";
    else if (vk == VK_RETURN) out += "Enter";
    else if (vk >= VK_F1 && vk <= VK_F24) out += "F" + std::to_string(vk - VK_F1 + 1);
    else {
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "VK(0x%X)", vk);
        out += buffer;
    }
    return out;
}

std::string build_panel_spec_for_testing() {
    Json rows = Json::array();
    for (const auto& binding : snapshot()) {
        rows.push_back(row_node(Json::array({
            text_node(binding.description),
            text_node(binding.id, "mono"),
            text_node(format_combo_utf8(binding.vk, binding.modifiers), "accent"),
            Json{{"type", "badge"}, {"text", "Ready"}, {"style", "muted"}, {"height", 22}},
            button_node("capture." + binding.id, "Capture", kCaptureAction, Json(binding.id), "primary"),
            button_node("reset." + binding.id, "Reset", kResetAction, Json(binding.id), "ghost"),
        })));
    }
    return Json{{"version", 1}, {"title", ""}, {"surface", "solid"}, {"nodes", Json::array({card_node("Bindings", std::move(rows))})}}.dump();
}

Owner::Owner(sao_ui_compositor_handle_t compositor) noexcept {
    try { impl_ = std::make_shared<Impl>(compositor); } catch (...) {}
}

Owner::~Owner() {
    if (impl_ == nullptr) return;
    auto state = std::move(impl_);
    state->stop_capture();
    if (state->panel_handle() == nullptr) return;
    if (sao_ui_compositor_require_owner_thread(state->compositor) == SAO_STATUS_OK && state->take_offline() == SAO_STATUS_OK)
        return;
    Impl::defer_cleanup(std::move(state));
}

sao_status_t Owner::open() noexcept { return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->open(); }
sao_status_t Owner::close() noexcept { return impl_ == nullptr ? SAO_STATUS_OK : impl_->close(); }
sao_status_t Owner::take_offline() noexcept { return impl_ == nullptr ? SAO_STATUS_OK : impl_->take_offline(); }
sao_status_t Owner::set_owner_wake_window(void* window) noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->set_owner_wake_window(window);
}
sao_status_t Owner::drain_capture_for_owner() noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->drain_capture_for_owner();
}
sao_status_t Owner::set_capture_hooks_for_testing(CaptureHooks hooks) noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->set_capture_hooks(std::move(hooks));
}
sao_status_t Owner::dispatch_action_for_testing(std::string_view action, std::string_view payload) noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->dispatch_action_for_testing(action, payload);
}
sao_status_t Owner::dispatch_event_for_testing(std::int32_t event_kind) noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->dispatch_event_for_testing(event_kind);
}
void Owner::fail_next_unregister_for_testing(sao_status_t status) noexcept { if (impl_ != nullptr) impl_->fail_next_unregister(status); }

void Owner::fail_next_handler_restore_for_testing(sao_status_t action_status, sao_status_t event_status) noexcept { if (impl_ != nullptr) impl_->fail_next_handler_restore(action_status, event_status); }

void Owner::drain_deferred_cleanup_for_owner() noexcept { Impl::drain_deferred_cleanup(); }

void Owner::drain_deferred_cleanup_for_testing() noexcept { drain_deferred_cleanup_for_owner(); }

sao_ui_panel_handle_t Owner::panel_handle() const noexcept { return impl_ == nullptr ? nullptr : impl_->panel_handle(); }
bool Owner::is_capturing() const noexcept { return impl_ != nullptr && impl_->is_capturing(); }

sao_status_t open_config_panel_status() noexcept { return SAO_STATUS_ERR_NOT_INITIALIZED; }
void open_config_panel() {}

sao_status_t open_config_panel_status(Owner* owner) noexcept {
    return owner == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : owner->open();
}

void open_config_panel(Owner* owner) {
    (void)open_config_panel_status(owner);
}

void close_config_panel() {}

void close_config_panel(Owner* owner) {
    if (owner != nullptr)
        (void)owner->close();
}

} // namespace sao::launcher::hotkey