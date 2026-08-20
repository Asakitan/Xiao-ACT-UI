#include "settings_config_panel.h"

#include "settings_owner_internal.h"
#include "settings_profiles.h"
#include "settings_theme_internal.h"

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER) || defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#define SAO_SETTINGS_PANEL_UI 1
#include "sao/ui/panel.h"
#include "sao/ui/panel_sdk.h"
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
#include "sao/sdk/sao_sdk_platform_internal.h"
#endif
#include "sao/ui/theme.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sao::launcher::settings {
namespace {
using Json = nlohmann::ordered_json;

constexpr std::size_t kMaximumActionBytes = 4096U;
constexpr std::size_t kMaximumSpecBytes = 256U * 1024U;
constexpr std::array<std::string_view, 5> kSections{
    "Overview", "Appearance", "Behavior", "Audio", "Advanced"};

struct PanelState final {
    std::mutex mutex;
    settings_owner::SettingsOwner* owner{};
#if defined(SAO_SETTINGS_PANEL_UI)
    sao_ui_compositor_handle_t compositor{};
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
#endif
    bool accepting{true};
    bool creating{};
    bool retiring{};
    bool action_attached{};
    bool event_attached{};
    std::size_t operations{};
    std::size_t callbacks{};
    bool visible{};
    sao_status_t last_status{SAO_STATUS_OK};
    sao_status_t fail_next_unregister_status{SAO_STATUS_OK};
    sao_status_t fail_next_action_restore_status{SAO_STATUS_OK};
    sao_status_t fail_next_event_restore_status{SAO_STATUS_OK};
    std::string status_text{"Ready"};
    std::string rendered_spec;
};

PanelState& state() {
    static PanelState value;
    return value;
}

std::string bounded(std::string value, std::size_t limit) {
    if (value.size() <= limit)
        return value;
    if (limit <= 3U)
        return value.substr(0, limit);
    value.resize(limit - 3U);
    value += "...";
    return value;
}

std::string status_text(sao_status_t status, std::string_view prefix) {
    std::string result(prefix);
    if (!result.empty())
        result += ": ";
    result += sao_status_str(status);
    result += " (";
    result += std::to_string(status);
    result += ")";
    return bounded(std::move(result), 1024U);
}

std::string wide_to_utf8(const std::wstring& value) {
#if defined(_WIN32)
    if (value.empty())
        return {};
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                               static_cast<int>(value.size()), nullptr, 0,
                                               nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), result.data(), required,
                              nullptr, nullptr) != required)
        return {};
    return result;
#else
    return std::string(value.begin(), value.end());
#endif
}

Json text_node(std::string text, std::string_view style = "value", int height = 24) {
    return Json{{"type", "text"}, {"text", bounded(std::move(text), 4096U)},
                {"style", style}, {"height", height}};
}

Json button_node(std::string id, std::string label, std::string action, Json payload,
                 std::string_view style = "default", bool active = false, bool disabled = false) {
    Json node{{"type", "button"}, {"id", std::move(id)}, {"label", std::move(label)},
              {"action", std::move(action)}, {"style", style}, {"active", active},
              {"height", 30}};
    if (!payload.empty())
        node["payload"] = std::move(payload);
    if (disabled)
        node["disabled"] = true;
    return node;
}

Json row_node(Json children) {
    return Json{{"type", "row"}, {"align", "left"}, {"children", std::move(children)}};
}

Json section_node(std::string title, Json children, std::string_view accent = "cyan") {
    return Json{{"type", "section"}, {"title", std::move(title)}, {"accent", accent},
                {"children", std::move(children)}};
}

bool key_contains(std::string_view key, std::string_view part) {
    return key.find(part) != std::string_view::npos;
}

bool is_audio_key(std::string_view key) {
    return key_contains(key, "sound") || key_contains(key, "audio") ||
           key_contains(key, "volume") || key_contains(key, "tts");
}

bool is_numeric_control(std::string_view key) {
    return key == "sound_volume" || key == "master_volume" || key == "audio_volume" ||
           key == "tts_volume" || key == "volume";
}

int section_index(std::string_view key, const Json& value) {
    if (key == "panel_themes" || key_contains(key, "theme") || key_contains(key, "appearance") ||
        key_contains(key, "display"))
        return 1;
    if (is_audio_key(key))
        return 3;
    if (value.is_boolean())
        return 2;
    if (value.is_object() || value.is_array())
        return 4;
    return 0;
}

std::string summary(const Json& value) {
    if (value.is_string())
        return "\"" + bounded(value.get<std::string>(), 240U) + "\" — configure in the settings file.";
    if (value.is_object())
        return "object (" + std::to_string(value.size()) +
               " keys) — configure in the settings file or a dedicated panel.";
    if (value.is_array())
        return "array (" + std::to_string(value.size()) +
               " items) — configure in the settings file or a dedicated panel.";
    if (value.is_null())
        return "null — configure in the settings file.";
    return bounded(value.dump(), 320U) + " — configure in the settings file or a dedicated panel.";
}

std::string value_label(const Json& value) {
    if (value.is_boolean())
        return value.get<bool>() ? "ON" : "OFF";
    if (value.is_string())
        return bounded(value.get<std::string>(), 160U);
    return bounded(value.dump(), 160U);
}

Json build_theme_row(const settings_theme::PanelTheme theme) {
    const bool light = theme == settings_theme::PanelTheme::light;
    Json children = Json::array();
    children.push_back(text_node("Process theme", "value", 28));
    children.push_back(button_node("settings.theme.dark", "Dark", kSettingsActionTheme,
                                  {{"theme", "dark"}}, light ? "ghost" : "primary", !light));
    children.push_back(button_node("settings.theme.light", "Light", kSettingsActionTheme,
                                  {{"theme", "light"}}, light ? "primary" : "ghost", light));
    return row_node(std::move(children));
}

std::string make_spec(const Json& snapshot, std::string_view status, sao_status_t status_code,
                      bool dirty, std::string_view path) {
    std::array<Json, 5> section_children;
    for (auto& children : section_children)
        children = Json::array();

    Json overview = Json::array();
    overview.push_back(text_node("Native settings compositor panel", "accent", 28));
    overview.push_back(text_node(std::string(status), status_code == SAO_STATUS_OK ? "muted" : "bad", 28));
    overview.push_back(text_node(dirty ? "Unsaved changes" : "Saved", dirty ? "warn" : "ok", 24));
    if (!path.empty())
        overview.push_back(text_node("Path: " + std::string(path), "mono", 24));
    section_children[0] = std::move(overview);

    settings_theme::PanelTheme theme = settings_theme::PanelTheme::dark;
    if (const auto panel_themes = snapshot.find("panel_themes"); panel_themes != snapshot.end() &&
        panel_themes->is_object()) {
        const auto act = panel_themes->find("act");
        if (act != panel_themes->end() && act->is_string() && act->get<std::string>() == "light")
            theme = settings_theme::PanelTheme::light;
    }
    section_children[1].push_back(build_theme_row(theme));

    if (snapshot.is_object()) {
        for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
            const std::string key = it.key();
            const Json& value = it.value();
            if (key == "panel_themes")
                continue;
            const int index = section_index(key, value);
            if (value.is_boolean()) {
                section_children[index].push_back(row_node(Json::array({
                    button_node("settings.bool." + key,
                                key + ": " + value_label(value), kSettingsActionToggle,
                                {{"key", key}}, value.get<bool>() ? "primary" : "ghost",
                                value.get<bool>()),
                })));
            } else if (is_numeric_control(key) && value.is_number()) {
                const double number = value.get<double>();
                Json controls = Json::array();
                controls.push_back(text_node(key + ": " + std::to_string(number), "value", 28));
                controls.push_back(button_node("settings.num.down." + key, "−", kSettingsActionNumericAdjust,
                                              {{"key", key}, {"delta", -5}}, "ghost"));
                controls.push_back(button_node("settings.num.up." + key, "+", kSettingsActionNumericAdjust,
                                              {{"key", key}, {"delta", 5}}, "ghost"));
                section_children[index].push_back(row_node(std::move(controls)));
            } else {
                section_children[index].push_back(text_node(key + ": " + summary(value), "muted", 32));
            }
        }
    }

    Json profiles = Json::array();
    profiles.push_back(row_node(Json::array({
        text_node("Profiles", "accent", 28),
        button_node("settings.profile.quick_backup", "Quick Backup", kSettingsActionProfileQuickBackup,
                    Json::object(), "primary"),
    })));
    const auto names = list_profiles();
    if (names.empty()) {
        profiles.push_back(text_node("No saved profiles.", "muted", 28));
    } else {
        for (const auto& name : names) {
            Json controls = Json::array();
            controls.push_back(text_node(name, "value", 28));
            controls.push_back(button_node("settings.profile.load." + name, "Load", kSettingsActionProfileLoad,
                                          {{"name", name}}, "primary"));
            controls.push_back(button_node("settings.profile.delete." + name, "Delete",
                                          kSettingsActionProfileDelete, {{"name", name}}, "danger"));
            profiles.push_back(row_node(std::move(controls)));
        }
    }
    section_children[0].push_back(std::move(profiles));

    Json actions = Json::array();
    actions.push_back(button_node("settings.refresh", "Refresh", kSettingsActionRefresh,
                                  Json::object(), "ghost"));
    actions.push_back(button_node("settings.close", "Close", kSettingsActionClose,
                                  Json::object(), "ghost"));
    section_children[0].push_back(row_node(std::move(actions)));

    Json nodes = Json::array();
    for (std::size_t index = 0; index < kSections.size(); ++index) {
        if (!section_children[index].empty())
            nodes.push_back(section_node(std::string(kSections[index]), std::move(section_children[index])));
    }
    Json result{{"version", 1}, {"title", ""}, {"nodes", std::move(nodes)}};
    return result.dump();
}

Json parse_payload(std::string_view payload, bool& valid) {
    valid = false;
    if (payload.size() > kMaximumActionBytes)
        return {};
    if (payload.empty()) {
        valid = true;
        return Json::object();
    }
    Json result = Json::parse(payload.begin(), payload.end(), nullptr, false, false);
    valid = !result.is_discarded() && result.is_object();
    return valid ? result : Json{};
}

bool payload_string(const Json& payload, std::string_view key, std::string& out) {
    const auto value = payload.find(std::string(key));
    if (value == payload.end() || !value->is_string())
        return false;
    out = value->get<std::string>();
    return !out.empty();
}

#if defined(SAO_SETTINGS_PANEL_UI)
sao_ui_compositor_handle_t borrowed_compositor() noexcept {
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    void* raw = nullptr;
    if (sao_sdk_platform_get_ui_compositor(&raw) != SAO_SDK_OK)
        return nullptr;
    return static_cast<sao_ui_compositor_handle_t>(raw);
#else
    std::lock_guard lock(state().mutex);
    return state().compositor;
#endif
}

sao_status_t require_owner_thread() noexcept {
    sao_ui_compositor_handle_t compositor = nullptr;
    {
        std::lock_guard lock(state().mutex);
        compositor = state().compositor;
    }
    return compositor == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED
                                 : sao_ui_compositor_require_owner_thread(compositor);
}

class OperationGuard final {
  public:
    OperationGuard() noexcept {
        if (require_owner_thread() != SAO_STATUS_OK)
            return;
        std::lock_guard lock(state().mutex);
        if (!state().accepting || state().retiring)
            return;
        ++state().operations;
        acquired_ = true;
    }
    ~OperationGuard() {
        if (!acquired_)
            return;
        std::lock_guard lock(state().mutex);
        --state().operations;
    }
    explicit operator bool() const noexcept { return acquired_; }
  private:
    bool acquired_{};
};

bool begin_callback() noexcept {
    std::lock_guard lock(state().mutex);
    if (!state().accepting || state().retiring)
        return false;
    ++state().callbacks;
    return true;
}

void end_callback() noexcept {
    std::lock_guard lock(state().mutex);
    if (state().callbacks != 0U)
        --state().callbacks;
}

sao_status_t publish() noexcept;
sao_status_t dispatch_action_impl(std::string_view action, std::string_view payload) noexcept;

void SAO_UI_CALL panel_action_callback(const char* action, const std::uint8_t* bytes,
                                       std::size_t length, void*) noexcept {
    if (action == nullptr || (bytes == nullptr && length != 0U) || !begin_callback())
        return;
    const std::string_view payload(bytes == nullptr ? "" : reinterpret_cast<const char*>(bytes), length);
    try { (void)dispatch_action_impl(action, payload); } catch (...) {}
    end_callback();
}

void SAO_UI_CALL panel_event_callback(std::int32_t event_kind, void*) noexcept {
    if (!begin_callback())
        return;
    if (event_kind == SAO_UI_PANEL_EVENT_CLOSE) {
        OperationGuard operation;
        if (operation) {
            sao_ui_panel_handle_t panel = nullptr;
            {
                std::lock_guard lock(state().mutex);
                panel = state().panel;
            }
            if (panel != nullptr && sao_ui_panel_hide(panel) == SAO_STATUS_OK) {
                std::lock_guard lock(state().mutex);
                state().visible = false;
            }
        }
    }
    end_callback();
}

sao_status_t ensure_panel() noexcept {
    if (require_owner_thread() != SAO_STATUS_OK)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    sao_ui_compositor_handle_t compositor = borrowed_compositor();
    if (compositor == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    {
        std::lock_guard lock(state().mutex);
        if (state().panel != nullptr && state().body != nullptr)
            return SAO_STATUS_OK;
        if (state().creating)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        if (state().panel != nullptr || state().body != nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        state().compositor = compositor;
        state().creating = true;
    }

    SaoPanelDescriptor descriptor{};
    descriptor.struct_size = sizeof(SaoPanelDescriptor);
    descriptor.panel_id_utf8 = kSettingsPanelId;
    descriptor.title_utf8 = kSettingsPanelTitle;
    descriptor.anchor = SAO_UI_PANEL_ANCHOR_CENTER;
    descriptor.default_width_px = 900;
    descriptor.default_height_px = 720;
    descriptor.min_width_px = 620;
    descriptor.min_height_px = 460;
    descriptor.max_width_px = 1400;
    descriptor.max_height_px = 1100;
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
    sao_status_t status = sao_ui_panel_register(compositor, &descriptor, &panel, &body);
    if (status == SAO_STATUS_OK)
        status = sao_ui_panel_set_action_handler(panel, &panel_action_callback, nullptr);
    if (status == SAO_STATUS_OK) {
        std::lock_guard lock(state().mutex);
        state().action_attached = true;
    }
    if (status == SAO_STATUS_OK)
        status = sao_ui_panel_set_event_handler(panel, &panel_event_callback, nullptr);
    if (status == SAO_STATUS_OK) {
        std::lock_guard lock(state().mutex);
        state().event_attached = true;
    }
    if (status != SAO_STATUS_OK) {
        if (panel != nullptr) {
            if (state().event_attached)
                (void)sao_ui_panel_set_event_handler(panel, nullptr, nullptr);
            if (state().action_attached)
                (void)sao_ui_panel_set_action_handler(panel, nullptr, nullptr);
            if (sao_ui_panel_unregister(panel) != SAO_STATUS_OK) {
                std::lock_guard lock(state().mutex);
                state().panel = panel;
                state().body = body;
                state().accepting = false;
                state().retiring = false;
            }
        }
        std::lock_guard lock(state().mutex);
        state().creating = false;
        return status;
    }
    {
        std::lock_guard lock(state().mutex);
        state().panel = panel;
        state().body = body;
        state().accepting = true;
        state().creating = false;
    }
    return SAO_STATUS_OK;
}
#endif

sao_status_t owner_snapshot(Json& out, settings_owner::SettingsOwner::Lease& owner_lease, bool& dirty, std::string& path) noexcept {
    settings_owner::SettingsOwner* owner = nullptr;
    { std::lock_guard lock(state().mutex); owner = state().owner; }
    if (owner == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    owner_lease = owner->acquire_lease();
    if (!owner_lease) return SAO_UI_PANEL_STATUS_ERR_BUSY;
    const sao_status_t status = owner_lease->snapshot(out);
    if (status != SAO_STATUS_OK) return status;
    dirty = owner_lease->dirty();
    std::wstring wide_path;
    if (owner_lease->path(wide_path) == SAO_STATUS_OK) path = wide_to_utf8(wide_path);
    return SAO_STATUS_OK;
}
sao_status_t update_status(sao_status_t status, std::string text) noexcept {
    try {
        std::lock_guard lock(state().mutex);
        state().last_status = status;
        state().status_text = std::move(text);
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

#if defined(SAO_SETTINGS_PANEL_UI)
sao_status_t publish() noexcept {
    Json snapshot;
    settings_owner::SettingsOwner::Lease owner_lease;
    bool dirty = false;
    std::string path;
    const sao_status_t snapshot_status =
        owner_snapshot(snapshot, owner_lease, dirty, path);
    std::string status;
    sao_status_t status_code = snapshot_status;
    {
        std::lock_guard lock(state().mutex);
        status = state().status_text;
        status_code = state().last_status;
        if (snapshot_status != SAO_STATUS_OK) {
            status = status_text(snapshot_status, "Snapshot failed");
            state().last_status = snapshot_status;
            state().status_text = status;
        }
    }
    if (snapshot_status != SAO_STATUS_OK)
        return snapshot_status;
    std::string spec = make_spec(snapshot, status, status_code, dirty, path);
    if (spec.size() > kMaximumSpecBytes)
        return update_status(SAO_STATUS_ERR_BUFFER_TOO_SMALL, "Settings panel exceeded its size limit");
    sao_ui_panel_body_handle_t body = nullptr;
    {
        std::lock_guard lock(state().mutex);
        body = state().body;
    }
    if (body == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    const sao_status_t status_result = sao_ui_panel_body_set_spec(
        body, reinterpret_cast<const std::uint8_t*>(spec.data()), spec.size());
    if (status_result != SAO_STATUS_OK)
        return update_status(status_result, status_text(status_result, "Panel render failed"));
    {
        std::lock_guard lock(state().mutex);
        state().rendered_spec = std::move(spec);
    }
    return SAO_STATUS_OK;
}
#endif

sao_status_t dispatch_action_impl(std::string_view action, std::string_view payload) noexcept {
    if (action.size() > 128U)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    bool valid = false;
    const Json data = parse_payload(payload, valid);
    if (!valid)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;

    if (action == kSettingsActionClose) {
        return close_for_testing();
    }
    if (action == kSettingsActionRefresh) {
#if defined(SAO_SETTINGS_PANEL_UI)
        const sao_status_t status = publish();
        return update_status(status, status == SAO_STATUS_OK ? "Refreshed" : status_text(status, "Refresh failed"));
#else
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
    }

    settings_owner::SettingsOwner::Lease owner_lease;
    Json snapshot;
    bool dirty = false;
    std::string path;
    sao_status_t status = owner_snapshot(snapshot, owner_lease, dirty, path);
    if (status != SAO_STATUS_OK)
        return update_status(status, status_text(status, "Settings unavailable"));

    if (action == kSettingsActionToggle) {
        std::string key;
        if (!payload_string(data, "key", key))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const auto value = snapshot.find(key);
        if (value == snapshot.end() || !value->is_boolean())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        status = owner_lease->set_value_and_save(key, !value->get<bool>());
        if (status == SAO_STATUS_OK)
            update_status(status, "Saved " + key);
        else
            update_status(status, status_text(status, "Save failed; change rolled back"));
#if defined(SAO_SETTINGS_PANEL_UI)
        if (state().body != nullptr)
            (void)publish();
#endif
        return status;
    }

    if (action == kSettingsActionNumericAdjust) {
        std::string key;
        if (!payload_string(data, "key", key) || !is_numeric_control(key))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const auto value = snapshot.find(key);
        const auto delta = data.find("delta");
        if (value == snapshot.end() || !value->is_number() || delta == data.end() ||
            !delta->is_number())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const double next = std::clamp(value->get<double>() + delta->get<double>(), 0.0, 100.0);
        status = owner_lease->set_value_and_save(key, next);
        update_status(status, status == SAO_STATUS_OK ? "Saved " + key
                                                       : status_text(status, "Save failed; change rolled back"));
#if defined(SAO_SETTINGS_PANEL_UI)
        if (state().body != nullptr)
            (void)publish();
#endif
        return status;
    }

    if (action == kSettingsActionTheme) {
        std::string requested;
        if (!payload_string(data, "theme", requested))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const settings_theme::PanelTheme next = requested == "light"
                                                    ? settings_theme::PanelTheme::light
                                                    : requested == "dark"
                                                          ? settings_theme::PanelTheme::dark
                                                          : static_cast<settings_theme::PanelTheme>(255);
        if (next != settings_theme::PanelTheme::light && next != settings_theme::PanelTheme::dark)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        settings_theme::PanelTheme previous{};
        status = settings_theme::read_process_theme(*owner_lease.get(), previous);
        if (status != SAO_STATUS_OK)
            return status;
#if defined(SAO_SETTINGS_PANEL_UI)
        const SaoUiThemeId runtime_previous = previous == settings_theme::PanelTheme::light
                                                  ? SAO_UI_THEME_LIGHT
                                                  : SAO_UI_THEME_DARK;
        const SaoUiThemeId runtime_next = next == settings_theme::PanelTheme::light
                                              ? SAO_UI_THEME_LIGHT
                                              : SAO_UI_THEME_DARK;
        status = sao_ui_theme_set_active_id(runtime_next);
        if (status != SAO_STATUS_OK)
            return update_status(status, status_text(status, "Theme switch failed"));
#endif
        status = settings_theme::replace_all_panel_themes(*owner_lease.get(), next);
        if (status != SAO_STATUS_OK) {
#if defined(SAO_SETTINGS_PANEL_UI)
            (void)sao_ui_theme_set_active_id(runtime_previous);
#endif
            update_status(status, status_text(status, "Theme save failed; change rolled back"));
            return status;
        }
        update_status(SAO_STATUS_OK, requested == "light" ? "Saved Light theme" : "Saved Dark theme");
#if defined(SAO_SETTINGS_PANEL_UI)
        if (state().body != nullptr)
            (void)publish();
#endif
        return SAO_STATUS_OK;
    }

    if (action == kSettingsActionProfileQuickBackup) {
        const bool saved = save_profile(std::string(kQuickBackupProfileName));
        status = saved ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
        update_status(status, saved ? "Quick Backup saved" : "Quick Backup failed");
#if defined(SAO_SETTINGS_PANEL_UI)
        if (state().body != nullptr)
            (void)publish();
#endif
        return status;
    }

    if (action == kSettingsActionProfileLoad || action == kSettingsActionProfileDelete) {
        std::string name;
        if (!payload_string(data, "name", name))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const bool changed = action == kSettingsActionProfileLoad ? load_profile(name) : delete_profile(name);
        status = changed ? SAO_STATUS_OK : SAO_STATUS_ERR_NOT_FOUND;
        update_status(status, changed ? (action == kSettingsActionProfileLoad ? "Profile loaded" : "Profile deleted")
                                      : status_text(status, "Profile action failed"));
#if defined(SAO_SETTINGS_PANEL_UI)
        if (state().body != nullptr)
            (void)publish();
#endif
        return status;
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

} // namespace

extern "C" sao_status_t sao_launcher_settings_panel_set_owner(void* owner_opaque) noexcept {
    settings_owner::SettingsOwner* previous = nullptr;
    try {
        {
            std::lock_guard lock(state().mutex);
            previous = state().owner;
            if (previous == owner_opaque)
                return SAO_STATUS_OK;
            state().owner = nullptr;
            state().accepting = false;
        }
        if (previous != nullptr)
            previous->retire_and_wait();
        auto* next = reinterpret_cast<settings_owner::SettingsOwner*>(owner_opaque);
        if (next != nullptr)
            next->resume_after_retire();
        {
            std::lock_guard lock(state().mutex);
            state().owner = next;
            state().accepting = next != nullptr;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t settings_panel_set_owner(void* owner_opaque) noexcept {
    return sao_launcher_settings_panel_set_owner(owner_opaque);
}

sao_status_t settings_panel_bind_owner(void* owner_opaque) noexcept {
    return sao_launcher_settings_panel_set_owner(owner_opaque);
}

sao_status_t settings_panel_unbind_owner(void* owner_opaque) noexcept {
    {
        std::lock_guard lock(state().mutex);
        if (owner_opaque != nullptr && state().owner != owner_opaque)
            return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    return sao_launcher_settings_panel_set_owner(nullptr);
}

sao_status_t rebind_owner_for_testing(void* owner_opaque) noexcept {
    return sao_launcher_settings_panel_set_owner(owner_opaque);
}

#if defined(SAO_SETTINGS_PANEL_UI)
void open_config_panel() {
    const sao_ui_compositor_handle_t compositor = borrowed_compositor();
    if (compositor == nullptr)
        return;
    {
        std::lock_guard lock(state().mutex);
        state().compositor = compositor;
    }
    OperationGuard operation;
    if (!operation)
        return;
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK) {
        (void)update_status(panel_status, status_text(panel_status, "Panel unavailable"));
        return;
    }
    const sao_status_t render_status = publish();
    sao_ui_panel_handle_t panel = nullptr;
    {
        std::lock_guard lock(state().mutex);
        panel = state().panel;
    }
    if (render_status != SAO_STATUS_OK || panel == nullptr)
        return;
    if (sao_ui_panel_show(panel) == SAO_STATUS_OK && sao_ui_panel_bring_to_front(panel) == SAO_STATUS_OK) {
        std::lock_guard lock(state().mutex);
        state().visible = true;
    }
}
#else
void open_config_panel() {}
#endif

sao_status_t set_compositor_for_testing(sao_ui_compositor_handle_t compositor) noexcept {
#if defined(SAO_SETTINGS_PANEL_UI)
    if (compositor == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(state().mutex);
    if (state().panel != nullptr)
        return SAO_UI_PANEL_STATUS_ERR_BUSY;
    state().compositor = compositor;
    return SAO_STATUS_OK;
#else
    (void)compositor;
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

sao_status_t dispatch_action_for_testing(std::string_view action_id,
                                         std::string_view payload_json) noexcept {
#if defined(SAO_SETTINGS_PANEL_UI)
    const sao_status_t owner_thread_status = require_owner_thread();
    if (owner_thread_status != SAO_STATUS_OK)
        return owner_thread_status;
    OperationGuard operation;
    if (!operation)
        return SAO_STATUS_ERR_CANCELLED;
    return dispatch_action_impl(action_id, payload_json);
#else
    (void)action_id;
    (void)payload_json;
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

sao_status_t close_for_testing() noexcept {
#if defined(SAO_SETTINGS_PANEL_UI)
    const sao_status_t owner_thread_status = require_owner_thread();
    if (owner_thread_status != SAO_STATUS_OK)
        return owner_thread_status;
    OperationGuard operation;
    if (!operation)
        return SAO_STATUS_ERR_CANCELLED;
    sao_ui_panel_handle_t panel = nullptr;
    {
        std::lock_guard lock(state().mutex);
        panel = state().panel;
    }
    if (panel == nullptr)
        return SAO_STATUS_OK;
    const sao_status_t status = sao_ui_panel_hide(panel);
    if (status == SAO_STATUS_OK) {
        std::lock_guard lock(state().mutex);
        state().visible = false;
    }
    return status;
#else
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

sao_status_t take_offline_for_testing() noexcept {
#if defined(SAO_SETTINGS_PANEL_UI)
    const sao_status_t owner_thread_status = require_owner_thread();
    if (owner_thread_status != SAO_STATUS_OK)
        return owner_thread_status;
    sao_ui_panel_handle_t panel = nullptr;
    bool action_attached = false;
    bool event_attached = false;
    {
        std::lock_guard lock(state().mutex);
        if (state().creating || state().operations != 0U || state().callbacks != 0U)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        if (state().panel == nullptr && state().body == nullptr)
            return SAO_STATUS_OK;
        if (state().panel == nullptr || state().body == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        state().retiring = true;
        state().accepting = false;
        panel = state().panel;
        action_attached = state().action_attached;
        event_attached = state().event_attached;
    }
    sao_status_t status = SAO_STATUS_OK;
    if (event_attached) {
        status = sao_ui_panel_set_event_handler(panel, nullptr, nullptr);
        if (status == SAO_STATUS_OK)
            event_attached = false;
    }
    if (status == SAO_STATUS_OK && action_attached) {
        status = sao_ui_panel_set_action_handler(panel, nullptr, nullptr);
        if (status == SAO_STATUS_OK)
            action_attached = false;
    }
    if (status == SAO_STATUS_OK) {
        sao_status_t injected = SAO_STATUS_OK;
        {
            std::lock_guard lock(state().mutex);
            injected = std::exchange(state().fail_next_unregister_status, SAO_STATUS_OK);
        }
        status = injected == SAO_STATUS_OK ? sao_ui_panel_unregister(panel) : injected;
    }
    if (status == SAO_STATUS_OK) {
        std::lock_guard lock(state().mutex);
        state().panel = nullptr;
        state().body = nullptr;
        state().visible = false;
        state().action_attached = false;
        state().event_attached = false;
        state().retiring = false;
        state().accepting = true;
        state().rendered_spec.clear();
        return SAO_STATUS_OK;
    }
    bool rollback_ok = true;
    if (!action_attached) {
        sao_status_t injected = SAO_STATUS_OK;
        {
            std::lock_guard lock(state().mutex);
            injected = std::exchange(state().fail_next_action_restore_status, SAO_STATUS_OK);
        }
        const sao_status_t restore_status =
            injected == SAO_STATUS_OK
                ? sao_ui_panel_set_action_handler(panel, &panel_action_callback, nullptr)
                : injected;
        action_attached = restore_status == SAO_STATUS_OK;
        rollback_ok = rollback_ok && action_attached;
    }
    if (!event_attached) {
        sao_status_t injected = SAO_STATUS_OK;
        {
            std::lock_guard lock(state().mutex);
            injected = std::exchange(state().fail_next_event_restore_status, SAO_STATUS_OK);
        }
        const sao_status_t restore_status =
            injected == SAO_STATUS_OK
                ? sao_ui_panel_set_event_handler(panel, &panel_event_callback, nullptr)
                : injected;
        event_attached = restore_status == SAO_STATUS_OK;
        rollback_ok = rollback_ok && event_attached;
    }
    {
        std::lock_guard lock(state().mutex);
        state().action_attached = action_attached;
        state().event_attached = event_attached;
        state().retiring = false;
        state().accepting = rollback_ok;
    }
    return rollback_ok ? status : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
#else
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

void drain_deferred_cleanup_for_owner() noexcept {}

void drain_deferred_cleanup_for_testing() noexcept {
    drain_deferred_cleanup_for_owner();
}

void fail_next_unregister_for_testing(sao_status_t status) noexcept {
    std::lock_guard lock(state().mutex);
    state().fail_next_unregister_status = status;
}

void fail_next_handler_restore_for_testing(sao_status_t action_status,
                                           sao_status_t event_status) noexcept {
    std::lock_guard lock(state().mutex);
    state().fail_next_action_restore_status = action_status;
    state().fail_next_event_restore_status = event_status;
}

sao_status_t snapshot_for_testing(std::string& out_json) noexcept {
    try {
        std::lock_guard lock(state().mutex);
        Json result{{"panel_id", kSettingsPanelId}, {"visible", state().visible},
                    {"last_status", state().last_status}, {"status", state().status_text},
                    {"spec", state().rendered_spec}};
        out_json = result.dump();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

std::string build_spec_for_testing(std::string_view snapshot_json_utf8) {
    try {
        const Json snapshot = Json::parse(snapshot_json_utf8.begin(), snapshot_json_utf8.end());
        if (!snapshot.is_object())
            return {};
        return make_spec(snapshot, "Test snapshot", SAO_STATUS_OK, false, {});
    } catch (...) {
        return {};
    }
}

} // namespace sao::launcher::settings