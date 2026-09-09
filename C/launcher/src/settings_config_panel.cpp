#include "settings_config_panel.h"
#include "sao/launcher/user_guide_webview.h"

#include "sao/ui/sound.h"
#include "settings_owner_internal.h"
#include "settings_profiles.h"
#include "settings_theme_internal.h"

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER) ||                                         \
    defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#define SAO_SETTINGS_PANEL_UI 1
#include "sao/ui/dialog.h"
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
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
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
constexpr std::array<std::string_view, 6> kSections{"Overview / 概览", "Appearance / 外观",
                                                    "Behavior / 行为", "Audio / 音频",
                                                    "Advanced / 高级", "Profiles / 配置"};

struct PanelState final {
    std::mutex mutex;
    settings_owner::SettingsOwner* owner{};
#if defined(SAO_SETTINGS_PANEL_UI)
    sao_ui_compositor_handle_t compositor{};
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    sao_ui_panel_body_handle_t rendered_body{};
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
    std::vector<std::string> profile_names;
    Json draft_snapshot = Json::object();
    Json committed_snapshot = Json::object();
    bool draft_initialized{};
    bool draft_dirty{};
    std::string profile_preview;
    std::size_t selected_section{};
};

PanelState& state() {
    static PanelState value;
    return value;
}

void refresh_profile_names() noexcept {
    try {
        std::vector<std::string> names = list_profiles();
        std::lock_guard lock(state().mutex);
        state().profile_names = std::move(names);
    } catch (...) {
    }
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

std::string format_number(const Json& value) {
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        return "invalid";
    std::string result = value.dump();
    const std::size_t decimal = result.find('.');
    if (decimal == std::string::npos || result.find_first_of("eE") != std::string::npos)
        return result;
    while (result.size() > decimal + 1U && result.back() == '0')
        result.pop_back();
    if (result.size() == decimal + 1U)
        result.pop_back();
    return result == "-0" ? "0" : result;
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
    const int required =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), result.data(), required, nullptr,
                              nullptr) != required)
        return {};
    return result;
#else
    return std::string(value.begin(), value.end());
#endif
}

Json text_node(std::string text, std::string_view style = "value", int height = 24) {
    return Json{{"type", "text"},
                {"text", bounded(std::move(text), 4096U)},
                {"style", style},
                {"height", height}};
}

Json badge_node(std::string text, std::string_view style = "muted") {
    return Json{{"type", "badge"},
                {"text", bounded(std::move(text), 512U)},
                {"style", style},
                {"height", 22}};
}

Json button_node(std::string id, std::string label, std::string action, Json payload,
                 std::string_view style = "default", bool active = false, bool disabled = false) {
    Json node{{"type", "button"},
              {"id", std::move(id)},
              {"label", std::move(label)},
              {"action", std::move(action)},
              {"style", style},
              {"active", active},
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
    return Json{{"type", "section"},
                {"title", std::move(title)},
                {"accent", accent},
                {"children", std::move(children)}};
}

Json card_node(std::string title, Json children, std::string_view accent = "cyan") {
    return Json{{"type", "card"},
                {"title", bounded(std::move(title), 512U)},
                {"accent", accent},
                {"children", std::move(children)}};
}

Json status_strip_node(std::string label, std::string message, std::string_view accent) {
    return card_node(
        "Status / 状态",
        Json::array({row_node(Json::array(
            {badge_node(std::move(label), accent), text_node(std::move(message), accent, 24)}))}),
        accent);
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
        return "\"" + bounded(value.get<std::string>(), 240U) +
               "\" — configure in the settings file.";
    if (value.is_object())
        return "object (" + std::to_string(value.size()) +
               " keys) — configure in the settings file or a dedicated panel.";
    if (value.is_array())
        return "array (" + std::to_string(value.size()) +
               " items) — configure in the settings file or a dedicated panel.";
    if (value.is_null())
        return "null — configure in the settings file.";
    if (value.is_number())
        return format_number(value) + " — configure in the settings file or a dedicated panel.";
    return bounded(value.dump(), 320U) + " — configure in the settings file or a dedicated panel.";
}

std::string value_label(const Json& value) {
    if (value.is_boolean())
        return value.get<bool>() ? "ON" : "OFF";
    if (value.is_string())
        return bounded(value.get<std::string>(), 160U);
    if (value.is_number())
        return format_number(value);
    return bounded(value.dump(), 160U);
}

Json build_theme_row(const settings_theme::PanelTheme theme) {
    Json children = Json::array();
    children.push_back(text_node("界面外观", "value", 28));
    children.push_back(Json{{"type", "dropdown"},
                            {"id", "settings.theme"},
                            {"action", "settings.theme.select"},
                            {"selected_id", theme == settings_theme::PanelTheme::light ? 0 : 1},
                            {"items", Json::array({
                                          {{"id", 0}, {"label", "经典浅色"}, {"value", "light"}},
                                          {{"id", 1}, {"label", "深色"}, {"value", "dark"}},
                                      })}});
    return row_node(std::move(children));
}

std::string make_spec(const Json& snapshot, std::string_view status, sao_status_t status_code,
                      bool dirty, std::string_view path, std::string_view profile_preview,
                      const std::vector<std::string>& profile_names,
                      std::size_t selected_section = 0U) {
    selected_section = std::min(selected_section, kSections.size() - 1U);
    std::array<Json, 6> section_children;
    for (auto& children : section_children)
        children = Json::array();

    const std::string_view overview_accent =
        status_code == SAO_STATUS_OK ? (dirty ? "gold" : "ok") : "danger";
    Json overview = Json::array();
    overview.push_back(status_strip_node(dirty ? "Draft / 草稿" : "Ready / 就绪",
                                         std::string(status), overview_accent));
    overview.push_back(
        text_node("Edit a draft, then apply to save. / 编辑草稿后点击应用保存。", "muted", 32));
    if (!path.empty())
        overview.push_back(text_node("Path: " + std::string(path), "mono", 24));

    settings_theme::PanelTheme theme = settings_theme::PanelTheme::light;
    if (const auto panel_themes = snapshot.find("panel_themes");
        panel_themes != snapshot.end() && panel_themes->is_object()) {
        const auto act = panel_themes->find("act");
        if (act != panel_themes->end() && act->is_string())
            (void)settings_theme::parse_panel_theme(act->get_ref<const std::string&>(), theme);
    }
    section_children[1].push_back(card_node(
        "Theme / 主题",
        Json::array(
            {build_theme_row(theme),
             text_node("浅色适合明亮环境，深色适合长时间阅读；应用后保存外观选择。", "muted", 34)}),
        theme == settings_theme::PanelTheme::light ? "gold" : "cyan"));

    bool has_sound_enabled = false;
    bool has_sound_volume = false;
    if (snapshot.is_object()) {
        for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
            const std::string key = it.key();
            const Json& value = it.value();
            if (key == "panel_themes")
                continue;
            has_sound_enabled = has_sound_enabled || key == "sound_enabled";
            has_sound_volume = has_sound_volume || key == "sound_volume";
            const int index = section_index(key, value);
            if (static_cast<std::size_t>(index) != selected_section)
                continue;
            const std::string display_key = key == "sound_enabled"  ? "界面音效"
                                            : key == "sound_volume" ? "音效音量"
                                                                    : key;
            if (value.is_boolean()) {
                section_children[index].push_back(row_node(Json::array({
                    button_node("settings.bool." + key, display_key + ": " + value_label(value),
                                kSettingsActionToggle, {{"key", key}},
                                value.get<bool>() ? "primary" : "ghost", value.get<bool>()),
                })));
            } else if (is_numeric_control(key) && value.is_number()) {
                Json controls = Json::array(
                    {text_node(display_key, "value", 28),
                     Json{{"type", "slider"},
                          {"id", "settings.volume." + key},
                          {"action", "settings.volume.set"},
                          {"payload", {{"key", key}}},
                          {"value", std::clamp(value.get<double>() / 100.0, 0.0, 1.0)}}});
                section_children[index].push_back(row_node(std::move(controls)));
            } else {
                section_children[index].push_back(
                    text_node(key + ": " + summary(value), "muted", 32));
            }
        }
    }
    if (!has_sound_enabled) {
        section_children[3].push_back(row_node(Json::array({
            button_node("settings.bool.sound_enabled", "界面音效: ON", kSettingsActionToggle,
                        {{"key", "sound_enabled"}}, "primary", true),
        })));
    }
    if (!has_sound_volume) {
        Json controls = Json::array(
            {text_node("音效音量", "value", 28), Json{{"type", "slider"},
                                                      {"id", "settings.volume.sound_volume"},
                                                      {"action", "settings.volume.set"},
                                                      {"payload", {{"key", "sound_volume"}}},
                                                      {"value", 0.7}}});
        section_children[3].push_back(row_node(std::move(controls)));
    }

    Json sound_samples = Json::array();
    constexpr std::array<const char*, 15> sound_labels = {
        "选择",      "菜单展开", "菜单关闭", "面板展开", "子菜单",
        "提示框",    "提示关闭", "SAO 欢迎", "ALO 欢迎", "LINK START",
        "NerveGear", "消息",     "系统",     "警告",     "紧急"};
    for (size_t start = 0; start < sound_labels.size(); start += 3) {
        Json row = Json::array();
        for (size_t index = start; index < std::min(start + 3, sound_labels.size()); ++index)
            row.push_back(button_node("settings.sound.preview." + std::to_string(index),
                                      sound_labels[index], "settings.sound.preview",
                                      {{"cue", index}}, "ghost"));
        sound_samples.push_back(row_node(std::move(row)));
    }
    sound_samples.push_back(text_node(
        "试听遵循界面音效总开关与音量。欢迎语仅用于开场；日常反馈采用短音。", "muted", 40));
    section_children[3].push_back(card_node("音效试听", std::move(sound_samples), "gold"));

    if (section_children[2].empty())
        section_children[2].push_back(
            text_node("No behavior settings available. / 暂无行为设置。", "muted", 30));
    if (section_children[4].empty())
        section_children[4].push_back(
            text_node("No advanced settings available. / 暂无高级设置。", "muted", 30));

    if (selected_section == 5U) {
        Json profiles = Json::array();
        profiles.push_back(row_node(Json::array({
            text_node("Profiles / 配置", "accent", 28),
            button_node("settings.profile.save_as", "Save as…", "settings.profile.save_as",
                        Json::object(), "primary"),
            button_node("settings.profile.quick_backup", "Quick Backup",
                        kSettingsActionProfileQuickBackup, Json::object(), "ghost"),
        })));
        profiles.push_back(
            text_node("Save a named snapshot of the current settings. / 保存当前设置的命名快照。",
                      "muted", 32));
        if (profile_names.empty()) {
            profiles.push_back(text_node("No saved profiles. / 暂无已保存配置。", "muted", 28));
        } else {
            for (const auto& name : profile_names) {
                Json controls = Json::array();
                controls.push_back(text_node(name, "value", 28));
                controls.push_back(button_node("settings.profile.preview." + name, "Load preview",
                                               "settings.profile.load_preview", {{"name", name}},
                                               "ghost"));
                controls.push_back(button_node("settings.profile.load." + name, "Load",
                                               kSettingsActionProfileLoad, {{"name", name}},
                                               "primary"));
                controls.push_back(button_node("settings.profile.delete." + name, "Delete",
                                               kSettingsActionProfileDelete, {{"name", name}},
                                               "danger"));
                profiles.push_back(row_node(std::move(controls)));
            }
        }
        if (!profile_preview.empty())
            profiles.push_back(text_node(std::string(profile_preview), "muted", 28));
        section_children[5] = std::move(profiles);
    }

    Json actions = Json::array();
    actions.push_back(button_node("settings.apply", "应用并保存", "settings.apply", Json::object(),
                                  "primary", false, !dirty));
    actions.push_back(button_node("settings.cancel", "撤销草稿", "settings.cancel", Json::object(),
                                  "ghost", false, !dirty));
    actions.push_back(
        button_node("settings.defaults", "恢复默认", "settings.defaults", Json::object(), "ghost"));
    actions.push_back(button_node("settings.refresh", "Refresh", kSettingsActionRefresh,
                                  Json::object(), "ghost"));
    actions.push_back(
        button_node("settings.close", "Close", kSettingsActionClose, Json::object(), "ghost"));
    Json footer = row_node(std::move(actions));
    footer["id"] = "settings-footer";
    footer["dock"] = "bottom";
    footer["height"] = 52;
    footer["padding"] = 8;

    Json nodes = Json::array();
    Json header = card_node("设置", std::move(overview), overview_accent);
    header["id"] = "settings-header";
    header["dock"] = "top";
    header["height"] = 112;
    nodes.push_back(std::move(header));
    nodes.push_back(std::move(footer));
    Json navigation = Json::array();
    for (std::size_t index = 0; index < kSections.size(); ++index)
        navigation.push_back(button_node(
            "settings.section." + std::to_string(index), std::string(kSections[index]),
            "settings.section.select", {{"section", index}},
            index == selected_section ? "primary" : "ghost", index == selected_section));
    Json rail = section_node("设置分类", std::move(navigation), "gold");
    rail["id"] = "settings-category-rail";
    rail["width"] = 190;
    rail["min_width"] = 160;
    rail["weight"] = 0;
    rail["scroll"] = {{"axis", "vertical"}, {"bar", "auto"}, {"wheel", true}};
    if (section_children[selected_section].empty())
        section_children[selected_section].push_back(
            text_node("选择分类查看对应设置；切换分类不会丢弃当前草稿。", "muted", 42));
    Json content = card_node(std::string(kSections[selected_section]),
                             std::move(section_children[selected_section]),
                             selected_section == 5U ? "gold" : "cyan");
    content["id"] = "settings-category-content";
    content["min_width"] = 320;
    content["weight"] = 1.0;
    content["scroll"] = {{"axis", "vertical"}, {"bar", "auto"}, {"wheel", true}};
    nodes.push_back(Json{{"type", "section"},
                         {"id", "settings-category-layout"},
                         {"container", true},
                         {"layout", "horizontal"},
                         {"dock", "fill"},
                         {"orientation", "horizontal"},
                         {"fallback", {{"layout", "vertical"}, {"threshold_width", 760}}},
                         {"children", Json::array({std::move(rail), std::move(content)})}});
    const auto use_semantic_controls = [](auto&& self, Json& items) -> void {
        for (auto& node : items) {
            if (node.value("id", std::string()).starts_with("settings.bool.")) {
                node["type"] = "checkbox";
                node["checked"] = node.value("active", false);
                node["style"] = "default";
            }
            if (node.contains("children") && node["children"].is_array())
                self(self, node["children"]);
        }
    };
    use_semantic_controls(use_semantic_controls, nodes);
    Json result{{"version", 1}, {"layout", "dock"}, {"title", ""}, {"nodes", std::move(nodes)}};
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
    explicit operator bool() const noexcept {
        return acquired_;
    }

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
    const std::string_view payload(bytes == nullptr ? "" : reinterpret_cast<const char*>(bytes),
                                   length);
    try {
        (void)dispatch_action_impl(action, payload);
    } catch (...) {
    }
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
        state().rendered_body = nullptr;
        state().rendered_spec.clear();
        state().accepting = true;
        state().creating = false;
    }
    return SAO_STATUS_OK;
}
#endif

sao_status_t owner_snapshot(Json& out, settings_owner::SettingsOwner::Lease& owner_lease,
                            bool& dirty, std::string& path) noexcept {
    settings_owner::SettingsOwner* owner = nullptr;
    {
        std::lock_guard lock(state().mutex);
        owner = state().owner;
    }
    if (owner == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    owner_lease = owner->acquire_lease();
    if (!owner_lease)
        return SAO_UI_PANEL_STATUS_ERR_BUSY;
    Json owner_document;
    const sao_status_t status = owner_lease->snapshot(owner_document);
    if (status != SAO_STATUS_OK)
        return status;
    {
        std::lock_guard lock(state().mutex);
        if (!state().draft_initialized || !state().draft_dirty) {
            state().draft_snapshot = owner_document;
            state().committed_snapshot = owner_document;
            state().draft_initialized = true;
            state().draft_dirty = false;
        }
        out = state().draft_snapshot;
        dirty = state().draft_dirty;
    }
    std::wstring wide_path;
    if (owner_lease->path(wide_path) == SAO_STATUS_OK)
        path = wide_to_utf8(wide_path);
    return SAO_STATUS_OK;
}
void sync_draft_snapshot(const settings_owner::SettingsOwner::Lease& owner_lease) noexcept {
    if (!owner_lease)
        return;
    Json draft;
    if (owner_lease->snapshot(draft) != SAO_STATUS_OK)
        return;
    std::lock_guard lock(state().mutex);
    state().draft_snapshot = std::move(draft);
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

sao_status_t publish_after_draft_mutation(sao_status_t status, std::string text) noexcept {
    update_status(status, std::move(text));
#if defined(SAO_SETTINGS_PANEL_UI)
    if (state().body != nullptr) {
        const sao_status_t publish_status = publish();
        return publish_status == SAO_STATUS_OK ? status : publish_status;
    }
#endif
    return status;
}

sao_status_t restore_runtime_theme(const Json& snapshot) noexcept {
#if defined(SAO_SETTINGS_PANEL_UI)
    settings_theme::PanelTheme theme = settings_theme::PanelTheme::light;
    const auto themes = snapshot.find("panel_themes");
    if (themes != snapshot.end() && themes->is_object()) {
        const auto active = themes->find("act");
        if (active != themes->end() && active->is_string())
            (void)settings_theme::parse_panel_theme(active->get_ref<const std::string&>(), theme);
    }
    return sao_ui_theme_set_active_id(
        theme == settings_theme::PanelTheme::light ? SAO_UI_THEME_LIGHT : SAO_UI_THEME_DARK);
#else
    (void)snapshot;
    return SAO_STATUS_OK;
#endif
}

sao_status_t restore_runtime_sound(const Json& snapshot) noexcept {
#if defined(SAO_SETTINGS_PANEL_UI)
    try {
        bool enabled = true;
        int32_t volume = 70;
        const auto enabled_value = snapshot.find("sound_enabled");
        if (enabled_value != snapshot.end() && enabled_value->is_boolean())
            enabled = enabled_value->get<bool>();
        const auto volume_value = snapshot.find("sound_volume");
        if (volume_value != snapshot.end() && volume_value->is_number()) {
            const double numeric = volume_value->get<double>();
            if (std::isfinite(numeric))
                volume = static_cast<int32_t>(std::clamp(numeric, 0.0, 100.0));
        }
        sao_status_t status = sao_ui_sound_set_enabled(enabled);
        sao::launcher::refreshUserGuideSoundPolicy();
        if (status == SAO_STATUS_OK)
            status = sao_ui_sound_set_volume(volume);
        sao::launcher::refreshUserGuideSoundPolicy();
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
#else
    (void)snapshot;
    return SAO_STATUS_OK;
#endif
}

#if defined(SAO_SETTINGS_PANEL_UI)
sao_status_t publish() noexcept {
    Json snapshot;
    settings_owner::SettingsOwner::Lease owner_lease;
    bool dirty = false;
    std::string path;
    const sao_status_t snapshot_status = owner_snapshot(snapshot, owner_lease, dirty, path);
    std::string status;
    std::string profile_preview;
    std::vector<std::string> profile_names;
    std::size_t selected_section = 0U;
    sao_status_t status_code = snapshot_status;
    {
        std::lock_guard lock(state().mutex);
        status = state().status_text;
        status_code = state().last_status;
        profile_preview = state().profile_preview;
        profile_names = state().profile_names;
        selected_section = state().selected_section;
        if (snapshot_status != SAO_STATUS_OK) {
            status = status_text(snapshot_status, "Snapshot failed");
            state().last_status = snapshot_status;
            state().status_text = status;
        }
    }
    if (snapshot_status != SAO_STATUS_OK)
        return snapshot_status;
    std::string spec = make_spec(snapshot, status, status_code, dirty, path, profile_preview,
                                 profile_names, selected_section);
    if (spec.size() > kMaximumSpecBytes)
        return update_status(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                             "Settings panel exceeded its size limit");
    sao_ui_panel_body_handle_t body = nullptr;
    {
        std::lock_guard lock(state().mutex);
        body = state().body;
    }
    if (body == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    {
        std::lock_guard lock(state().mutex);
        if (state().body == body && state().rendered_body == body && state().rendered_spec == spec)
            return SAO_STATUS_OK;
    }
    const sao_status_t status_result = sao_ui_panel_body_set_spec(
        body, reinterpret_cast<const std::uint8_t*>(spec.data()), spec.size());
    if (status_result != SAO_STATUS_OK)
        return update_status(status_result, status_text(status_result, "Panel render failed"));
    {
        std::lock_guard lock(state().mutex);
        if (state().body != body)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        state().rendered_body = body;
        state().rendered_spec = std::move(spec);
    }
    return SAO_STATUS_OK;
}
#endif

sao_status_t profile_document(const std::string& name, Json& out) noexcept {
    std::wstring path;
    const sao_status_t path_status = profile_path(name, path);
    if (path_status != SAO_STATUS_OK)
        return path_status;
    const std::filesystem::path file(path);
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return std::filesystem::exists(file) ? SAO_STATUS_ERR_ACCESS_DENIED
                                             : SAO_STATUS_ERR_NOT_FOUND;
    try {
        input >> out;
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return out.is_object() ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
}

std::string profile_failure_text(sao_status_t status, std::string_view operation) {
    switch (status) {
    case SAO_STATUS_ERR_NOT_FOUND:
        return std::string(operation) + ": profile does not exist";
    case SAO_STATUS_ERR_INVALID_ARGUMENT:
        return std::string(operation) + ": profile parse failed";
    case SAO_STATUS_ERR_ACCESS_DENIED:
        return std::string(operation) + ": permission denied";
    default:
        return std::string(operation) + ": save failed";
    }
}

#if defined(SAO_SETTINGS_PANEL_UI)
struct ProfileDialogContext {
    bool deleting{};
    std::string name;
    sao_ui_dialog_handle_t dialog{};
};

void SAO_UI_CALL profile_dialog_callback(SaoUiDialogButton pressed, const char* input,
                                         std::size_t length, void* user_data) noexcept {
    std::unique_ptr<ProfileDialogContext> context(static_cast<ProfileDialogContext*>(user_data));
    if (context == nullptr)
        return;
    const sao_ui_dialog_handle_t dialog = context->dialog;
    if (context->deleting) {
        if (pressed != SAO_UI_DIALOG_BTN_YES) {
            sao_ui_dialog_destroy(dialog);
            return;
        }
        const bool deleted = delete_profile(context->name);
        refresh_profile_names();
        update_status(deleted ? SAO_STATUS_OK : SAO_STATUS_ERR_NOT_FOUND,
                      deleted ? "Profile deleted" : "Profile delete failed");
    } else {
        if (pressed != SAO_UI_DIALOG_BTN_OK) {
            sao_ui_dialog_destroy(dialog);
            return;
        }
        const std::string name(input == nullptr ? "" : std::string(input, length));
        const bool saved = save_profile(name);
        refresh_profile_names();
        update_status(saved ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED,
                      saved ? "Profile saved" : "Profile save failed");
    }
    (void)publish();
    sao_ui_dialog_destroy(dialog);
}

sao_status_t show_profile_save_dialog() noexcept {
    const auto compositor = borrowed_compositor();
    if (compositor == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    auto* context = new (std::nothrow) ProfileDialogContext{};
    if (context == nullptr)
        return SAO_STATUS_ERR_UNKNOWN;
    sao_ui_dialog_handle_t dialog = nullptr;
    sao_status_t status = sao_ui_dialog_create(compositor, nullptr, &dialog);
    if (status != SAO_STATUS_OK) {
        delete context;
        return status;
    }
    context->dialog = dialog;
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_INPUT;
    spec.title_utf8 = "Save settings profile";
    spec.message_utf8 = "Choose a profile name";
    spec.input_prompt_utf8 = "Profile name";
    spec.input_default_utf8 = "";
    spec.input_max_length = 64;
    spec.theme_override = SAO_UI_THEME_COUNT;
    status = sao_ui_dialog_show(dialog, &spec, &profile_dialog_callback, context);
    if (status != SAO_STATUS_OK) {
        sao_ui_dialog_destroy(dialog);
        delete context;
    }
    return status;
}

sao_status_t show_profile_delete_dialog(std::string name) noexcept {
    const auto compositor = borrowed_compositor();
    if (compositor == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    auto* context = new (std::nothrow) ProfileDialogContext{};
    if (context == nullptr)
        return SAO_STATUS_ERR_UNKNOWN;
    context->deleting = true;
    context->name = std::move(name);
    const std::string message = "Delete profile " + context->name + "?";
    const sao_status_t status = sao_ui_dialog_show_ask(
        compositor, nullptr, "Delete profile", message.c_str(), &profile_dialog_callback, context);
    if (status != SAO_STATUS_OK)
        delete context;
    return status;
}
#endif

sao_status_t dispatch_action_impl(std::string_view action, std::string_view payload) noexcept {
    if (action.size() > 128U)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    bool valid = false;
    const Json data = parse_payload(payload, valid);
    if (!valid)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;

    if (action == "settings.sound.preview") {
        const auto cue = data.find("cue");
        if (data.size() != 1 || cue == data.end() || !cue->is_number_integer() || *cue < 0 ||
            *cue >= 15)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(SAO_SETTINGS_PANEL_UI)
        return sao_ui_sound_play(static_cast<SaoUiSoundCue>(cue->get<int>()), 70);
#else
        return SAO_STATUS_ERR_NOT_INITIALIZED;
#endif
    }

    if (action == kSettingsActionClose)
        return close_for_testing();

    if (action == "settings.section.select") {
        const auto section = data.find("section");
        if (data.size() != 1U || section == data.end() || !section->is_number_integer() ||
            *section < 0 || *section >= kSections.size())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        {
            std::lock_guard lock(state().mutex);
            state().selected_section = section->get<std::size_t>();
        }
#if defined(SAO_SETTINGS_PANEL_UI)
        return publish();
#else
        return SAO_STATUS_OK;
#endif
    }

    settings_owner::SettingsOwner::Lease owner_lease;
    Json snapshot;
    bool dirty = false;
    std::string path;
    sao_status_t status = owner_snapshot(snapshot, owner_lease, dirty, path);
    if (status != SAO_STATUS_OK)
        return publish_after_draft_mutation(status, status_text(status, "Settings unavailable"));

    if (action == kSettingsActionRefresh) {
        refresh_profile_names();
        status = update_status(SAO_STATUS_OK, "Refreshed");
#if defined(SAO_SETTINGS_PANEL_UI)
        if (state().body != nullptr)
            return publish();
#endif
        return status;
    }

    if (action == "settings.apply") {
        sync_draft_snapshot(owner_lease);
        status = owner_lease->save();
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(state().mutex);
            state().committed_snapshot = state().draft_snapshot;
            state().draft_dirty = false;
        }
        return publish_after_draft_mutation(status, status == SAO_STATUS_OK ? "Saved just now"
                                                                            : "Save failed");
    }

    if (action == "settings.cancel") {
        Json committed;
        {
            std::lock_guard lock(state().mutex);
            committed = state().committed_snapshot;
        }
        status = owner_lease->restore_snapshot(committed, false);
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(state().mutex);
            state().draft_snapshot = committed;
            state().draft_dirty = false;
        }
        if (status == SAO_STATUS_OK)
            status = restore_runtime_theme(committed);
        if (status == SAO_STATUS_OK)
            status = restore_runtime_sound(committed);
        return publish_after_draft_mutation(status, status == SAO_STATUS_OK ? "Changes cancelled"
                                                                            : "Cancel failed");
    }

    if (action == "settings.defaults") {
        status = owner_lease->restore_snapshot(Json::object(), true);
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(state().mutex);
            state().draft_snapshot = Json::object();
            state().draft_dirty = true;
        }
        if (status == SAO_STATUS_OK)
            status = restore_runtime_theme(Json::object());
        if (status == SAO_STATUS_OK)
            status = restore_runtime_sound(Json::object());
        return publish_after_draft_mutation(status, status == SAO_STATUS_OK
                                                        ? "Defaults restored in draft"
                                                        : "Restore defaults failed");
    }

    if (action == kSettingsActionToggle) {
        std::string key;
        const auto value = data.find("key");
        if (value == data.end() || !value->is_string())
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT, "Invalid setting");
        key = value->get<std::string>();
        const auto current = snapshot.find(key);
        if (current != snapshot.end() && !current->is_boolean())
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT, "Invalid setting");
        if (current == snapshot.end() && key != "sound_enabled")
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT, "Invalid setting");
        const bool next = !(current == snapshot.end() ? true : current->get<bool>());
        status = owner_lease->set_value(key, next);
        if (status == SAO_STATUS_OK) {
            sync_draft_snapshot(owner_lease);
            {
                std::lock_guard lock(state().mutex);
                state().draft_dirty = true;
            }
            if (key == "sound_enabled") {
                const sao_status_t runtime_status = sao_ui_sound_set_enabled(next);
                sao::launcher::refreshUserGuideSoundPolicy();
                if (runtime_status != SAO_STATUS_OK)
                    status = runtime_status;
                else if (next)
                    (void)sao_ui_sound_play(SAO_UI_SOUND_CLICK, 50);
            }
        }
        return publish_after_draft_mutation(
            status, status == SAO_STATUS_OK ? "Draft changes pending" : "Change failed");
    }

    if (action == "settings.volume.set") {
        const auto key_value = data.find("key");
        const auto value = data.find("value");
        if (key_value == data.end() || !key_value->is_string() || value == data.end() ||
            !value->is_number())
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT, "Invalid volume");
        const std::string key = key_value->get<std::string>();
        if (!is_numeric_control(key))
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT,
                                                "Invalid volume setting");
        const double next = std::clamp(value->get<double>(), 0.0, 1.0) * 100.0;
        status = owner_lease->set_value(key, next);
        if (status == SAO_STATUS_OK) {
            sync_draft_snapshot(owner_lease);
            if (key == "sound_volume") {
                const sao_status_t runtime_status =
                    sao_ui_sound_set_volume(static_cast<int32_t>(next));
                sao::launcher::refreshUserGuideSoundPolicy();
                if (runtime_status != SAO_STATUS_OK)
                    status = runtime_status;
            }
            std::lock_guard lock(state().mutex);
            state().draft_dirty = true;
        }
        return publish_after_draft_mutation(
            status, status == SAO_STATUS_OK ? "Draft changes pending" : "Volume change failed");
    }

    if (action == kSettingsActionNumericAdjust) {
        std::string key;
        const auto key_value = data.find("key");
        const auto delta = data.find("delta");
        if (key_value == data.end() || !key_value->is_string() ||
            !is_numeric_control(key_value->get<std::string>()) || delta == data.end() ||
            !delta->is_number())
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT,
                                                "Invalid numeric setting");
        key = key_value->get<std::string>();
        const auto current = snapshot.find(key);
        if (current != snapshot.end() && !current->is_number())
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT,
                                                "Invalid numeric setting");
        if (current == snapshot.end() && key != "sound_volume")
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT,
                                                "Invalid numeric setting");
        const double current_value = current == snapshot.end() ? 70.0 : current->get<double>();
        const double next = std::clamp(current_value + delta->get<double>(), 0.0, 100.0);
        status = owner_lease->set_value(key, next);
        if (status == SAO_STATUS_OK) {
            sync_draft_snapshot(owner_lease);
            {
                std::lock_guard lock(state().mutex);
                state().draft_dirty = true;
            }
            if (key == "sound_volume") {
                const sao_status_t runtime_status =
                    sao_ui_sound_set_volume(static_cast<int32_t>(next));
                if (runtime_status != SAO_STATUS_OK)
                    status = runtime_status;
            }
            sao::launcher::refreshUserGuideSoundPolicy();
        }
        return publish_after_draft_mutation(
            status, status == SAO_STATUS_OK ? "Draft changes pending" : "Change failed");
    }

    if (action == kSettingsActionTheme || action == "settings.theme.select") {
        std::string requested;
        if (!payload_string(data, action == "settings.theme.select" ? "value" : "theme", requested))
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT, "Invalid theme");
        const settings_theme::PanelTheme next =
            requested == "light"  ? settings_theme::PanelTheme::light
            : requested == "dark" ? settings_theme::PanelTheme::dark
                                  : static_cast<settings_theme::PanelTheme>(255);
        if (next != settings_theme::PanelTheme::light && next != settings_theme::PanelTheme::dark)
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT, "Invalid theme");
        Json themes = snapshot.value("panel_themes", Json::object());
        if (!themes.is_object())
            themes = Json::object();
        const char* value = next == settings_theme::PanelTheme::light ? "light" : "dark";
        for (const std::string_view key :
             {"dps", "hp", "bosshp", "skillfx", "alert", "act", "buffmon"})
            themes[std::string(key)] = value;
        status = owner_lease->set_value("panel_themes", std::move(themes));
        if (status == SAO_STATUS_OK) {
            sync_draft_snapshot(owner_lease);
            const sao_status_t runtime_status = sao_ui_theme_set_active_id(
                next == settings_theme::PanelTheme::light ? SAO_UI_THEME_LIGHT : SAO_UI_THEME_DARK);
            if (runtime_status != SAO_STATUS_OK)
                status = runtime_status;
            std::lock_guard lock(state().mutex);
            state().draft_dirty = true;
        }
        return publish_after_draft_mutation(
            status, status == SAO_STATUS_OK ? "Draft changes pending" : "Theme change failed");
    }

    if (action == "settings.profile.save_as") {
#if defined(SAO_SETTINGS_PANEL_UI)
        return show_profile_save_dialog();
#else
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
    }

    if (action == kSettingsActionProfileQuickBackup) {
        const bool saved = save_profile(std::string(kQuickBackupProfileName));
        refresh_profile_names();
        status = saved ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
        return publish_after_draft_mutation(status,
                                            saved ? "Profile saved" : "Profile save failed");
    }

    if (action == "settings.profile.load_preview" || action == kSettingsActionProfileLoad ||
        action == kSettingsActionProfileDelete) {
        std::string name;
        if (!payload_string(data, "name", name))
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT,
                                                "Invalid profile name");
        if (action == "settings.profile.load_preview") {
            Json profile;
            status = profile_document(name, profile);
            std::string preview;
            {
                std::lock_guard lock(state().mutex);
                preview = status == SAO_STATUS_OK
                              ? "Preview: " + std::to_string(profile.size()) + " fields"
                              : profile_failure_text(status, "Load preview");
                state().profile_preview = preview;
            }
            return publish_after_draft_mutation(status, std::move(preview));
        }
        if (action == kSettingsActionProfileDelete) {
#if defined(SAO_SETTINGS_PANEL_UI)
            return show_profile_delete_dialog(name);
#else
            return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
        }
        Json profile;
        status = profile_document(name, profile);
        const Json profile_copy = profile;
        if (status == SAO_STATUS_OK)
            status = owner_lease->restore_snapshot(std::move(profile), true);
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(state().mutex);
            state().draft_snapshot = profile_copy;
            state().draft_initialized = true;
            state().draft_dirty = true;
        }
        if (status == SAO_STATUS_OK)
            status = restore_runtime_theme(profile_copy);
        if (status == SAO_STATUS_OK)
            status = restore_runtime_sound(profile_copy);
        return publish_after_draft_mutation(
            status, status == SAO_STATUS_OK ? "Profile loaded into draft"
                                            : profile_failure_text(status, "Load profile"));
    }
    return publish_after_draft_mutation(SAO_STATUS_ERR_NOT_FOUND, "Unknown settings action");
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
        Json runtime_snapshot = Json::object();
        if (next != nullptr && next->snapshot(runtime_snapshot) != SAO_STATUS_OK)
            runtime_snapshot = Json::object();
        {
            std::lock_guard lock(state().mutex);
            state().owner = next;
            state().draft_snapshot = Json::object();
            state().committed_snapshot = Json::object();
            state().draft_initialized = false;
            state().draft_dirty = false;
            state().profile_preview.clear();
            state().profile_names.clear();
            state().selected_section = 0U;
            state().accepting = next != nullptr;
        }
        if (next == nullptr)
            return SAO_STATUS_OK;
        const sao_status_t theme_status = restore_runtime_theme(runtime_snapshot);
        const sao_status_t sound_status = restore_runtime_sound(runtime_snapshot);
        return theme_status == SAO_STATUS_OK ? sound_status : theme_status;
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
sao_status_t open_config_panel_status() noexcept {
    const sao_ui_compositor_handle_t compositor = borrowed_compositor();
    if (compositor == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    {
        std::lock_guard lock(state().mutex);
        state().compositor = compositor;
    }
    OperationGuard operation;
    if (!operation)
        return SAO_STATUS_ERR_CANCELLED;
    refresh_profile_names();
    const sao_status_t panel_status = ensure_panel();
    if (panel_status != SAO_STATUS_OK) {
        (void)update_status(panel_status, status_text(panel_status, "Panel unavailable"));
        return panel_status;
    }
    const sao_status_t render_status = publish();
    sao_ui_panel_handle_t panel = nullptr;
    {
        std::lock_guard lock(state().mutex);
        panel = state().panel;
    }
    if (render_status != SAO_STATUS_OK || panel == nullptr)
        return render_status != SAO_STATUS_OK ? render_status : SAO_STATUS_ERR_HANDLE_INVALID;
    sao_status_t show_status = sao_ui_panel_show(panel);
    if (show_status != SAO_STATUS_OK)
        return show_status;
    show_status = sao_ui_panel_bring_to_front(panel);
    if (show_status != SAO_STATUS_OK)
        return show_status;
    {
        std::lock_guard lock(state().mutex);
        state().visible = true;
    }
    return SAO_STATUS_OK;
}

void open_config_panel() {
    (void)open_config_panel_status();
}
#else
sao_status_t open_config_panel_status() noexcept {
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
}
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
    {
        std::lock_guard lock(state().mutex);
        if (!state().creating && state().operations == 0U && state().callbacks == 0U &&
            state().panel == nullptr && state().body == nullptr) {
            return SAO_STATUS_OK;
        }
    }
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
        state().rendered_body = nullptr;
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
        Json result{{"panel_id", kSettingsPanelId},
                    {"visible", state().visible},
                    {"last_status", state().last_status},
                    {"status", state().status_text},
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
        return make_spec(snapshot, "Test snapshot", SAO_STATUS_OK, false, {}, {}, list_profiles());
    } catch (...) {
        return {};
    }
}

} // namespace sao::launcher::settings
