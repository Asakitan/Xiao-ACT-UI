#include "settings_config_panel.h"
#include "sao/launcher/user_guide_webview.h"

#include "hotkey_config_panel.h"
#include "sao/ui/sound.h"
#include "sao/ui/streaming_flow.h"
#include "settings_codec_internal.h"
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
#if __has_include("sao/sdk/sao_sdk_platform_panels.h")
#define SAO_SETTINGS_HAS_PANEL_OPEN 1
#include "sao/sdk/sao_sdk_platform_panels.h"
#endif
#endif
#if __has_include("sao/ui/file_picker.h")
#define SAO_SETTINGS_HAS_FILE_PICKER 1
#include "sao/ui/file_picker.h"
#endif
#include "sao/ui/theme.h"
#endif

// License/account status needs the entitlement service TU and the provider
// config TU — both are SaoAuto-only (sao_ui_preview links neither), so the
// SAO_LINKED_LICENSE define is the capability marker.
#if defined(SAO_LINKED_LICENSE) && __has_include("license_provider.h") &&                          \
    __has_include("sao/launcher/provider_config.h")
#define SAO_SETTINGS_HAS_LICENSE_PROVIDER 1
#include "license_provider.h"
#include "sao/launcher/provider_config.h"
#endif

// Plugin snapshot needs the loader registry headers; the define pair mirrors
// plugin_manager_panel_internal.cpp.
#if (defined(SAO_LINKED_PLUGINS) || defined(SAO_LAUNCHER_PLUGIN_MANAGER_WITH_LOADER)) &&           \
    __has_include("sao/plugins/loader/plugin_lifecycle.h") &&                                      \
    __has_include("sao/plugins/loader/plugin_registry.h") &&                                       \
    __has_include("sao/plugins/loader/plugin_manifest.h")
#define SAO_SETTINGS_HAS_PLUGIN_LOADER 1
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
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
#include <shellapi.h>
#endif

#if defined(_WIN32) && defined(SAO_SETTINGS_PANEL_UI)
// dialog_file_picker.cpp exports this Win32 open-file dialog but ships no
// public header; declare it here (cdecl, same as SAO_UI_CALL).
extern "C" sao_status_t __cdecl sao_ui_dialog_file_picker_show(
    const char* title_utf8, const char* filter_utf8, char* out_path_utf8,
    size_t out_capacity);
#endif

namespace sao::launcher::settings {
namespace {
using Json = nlohmann::ordered_json;

constexpr std::size_t kMaximumActionBytes = 4096U;
constexpr std::size_t kMaximumSpecBytes = 256U * 1024U;
constexpr std::uintmax_t kMaximumProfileBytes = 16U * 1024U * 1024U;
constexpr std::size_t kFilePickerPathBytes = 8192U;
constexpr std::array<std::string_view, 12> kSections{
    "Overview / 概览",  "Appearance / 外观", "Behavior / 行为", "Audio / 音频",
    "Advanced / 高级",  "Profiles / 配置",   "Hotkeys / 快捷键", "Plugins / 插件",
    "License / 授权",   "Account / 账户",    "Files / 文件",    "Other / 其他"};

// Sections whose content the spec generator fills with dedicated cards
// instead of the generic per-key loop.
constexpr int kSectionHotkeys = 6;
constexpr int kSectionPlugins = 7;
constexpr int kSectionLicense = 8;
constexpr int kSectionAccount = 9;
constexpr int kSectionFiles = 10;
constexpr int kSectionOther = 11;

// Bools the panel may write live. Everything else renders read-only so a
// switch never pretends to control a value the runtime only honours at boot
// (topmost, sao_screencap_protection) or another owner applies (nervgear).
constexpr std::array<std::string_view, 2> kEditableBools{"sound_enabled", "streaming_mode"};

struct PanelState final {
    std::mutex mutex;
    settings_owner::SettingsOwner* owner{};
#if defined(SAO_SETTINGS_PANEL_UI)
    sao_ui_compositor_handle_t compositor{};
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    sao_ui_panel_body_handle_t rendered_body{};
#if defined(SAO_SETTINGS_HAS_FILE_PICKER) && SAO_SETTINGS_HAS_FILE_PICKER
    sao_ui_file_picker_handle_t file_picker{};
#endif
#endif
    // Latched when a change notification lands on a foreign thread; the next
    // owner-thread refresh clears it. Bookkeeping only — publish() always
    // re-reads the owner document either way.
    bool external_change_pending{};
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
    bool committed_owner_dirty{};
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

sao_status_t first_error(sao_status_t first, sao_status_t second) noexcept {
    return first == SAO_STATUS_OK ? second : first;
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
              {"height", 36}};
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

bool key_contains(std::string_view key, std::string_view part) {
    return key.find(part) != std::string_view::npos;
}

bool is_audio_key(std::string_view key) {
    return key_contains(key, "sound") || key_contains(key, "audio") ||
           key_contains(key, "volume") || key_contains(key, "tts");
}

bool is_numeric_control(std::string_view key) {
    // Only sound_volume has a live runtime binding; master/audio/tts/volume
    // were a whitelist for keys the settings file never produces.
    return key == "sound_volume";
}

bool is_editable_bool(std::string_view key) {
    return std::find(kEditableBools.begin(), kEditableBools.end(), key) !=
           kEditableBools.end();
}

int section_index(std::string_view key, const Json& value) {
    if (key == "streaming_mode" || key == "sao_screencap_protection" ||
        key_contains(key, "anti_screencap") || key_contains(key, "capture"))
        return 4;
    if (key == "panel_themes" || key_contains(key, "theme") || key_contains(key, "appearance") ||
        key_contains(key, "display"))
        return 1;
    if (is_audio_key(key))
        return 3;
    if (key == "nervgear_mode" || key == "topmost")
        return 2;
    if (key == "user_guide_presented" || key == "game_cache")
        return kSectionOther;
    if (value.is_boolean() && key != "streaming_mode")
        return 2;
    return kSectionOther;
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

std::string humanize_key(std::string_view key) {
    std::string label;
    label.reserve(key.size());
    bool capitalize = true;
    for (const unsigned char character : key) {
        if (character == '_' || character == '-') {
            if (!label.empty() && label.back() != ' ')
                label.push_back(' ');
            capitalize = true;
            continue;
        }
        if (capitalize && character >= 'a' && character <= 'z')
            label.push_back(static_cast<char>(character - ('a' - 'A')));
        else
            label.push_back(static_cast<char>(character));
        capitalize = false;
    }
    return label;
}

std::string setting_label(std::string_view key) {
    if (key == "topmost") return "Topmost window / 窗口置顶";
    if (key == "nervgear_mode") return "NervGear mode / NervGear 模式";
    if (key == "sao_screencap_protection")
        return "Screen-capture protection / 防截屏保护";
    if (key == "user_guide_presented") return "User guide presented / 已显示用户指南";
    if (key == "hotkeys") return "Hotkeys / 快捷键";
    if (key == "game_cache") return "Game cache / 游戏缓存";
    if (key == "sound_volume") return "Audio volume / 音频音量";
    if (key == "tts_volume") return "Speech volume / 语音音量";
    if (key == "overlay_opacity") return "Overlay opacity / 覆盖层不透明度";
    if (key == "plugins_enabled") return "Enabled plugins / 已启用插件";
    if (key == "license_tier") return "License tier / 许可证等级";
    if (key == "theme_id") return "Theme identifier / 主题标识";
    return humanize_key(key) + " (" + std::string(key) + ")";
}

std::string percent_label(double value) {
    if (!std::isfinite(value))
        return "Invalid / 无效";
    const double rounded = std::round(std::clamp(value, 0.0, 100.0) * 10.0) / 10.0;
    return format_number(Json(rounded)) + "%";
}

std::string draft_feedback(std::string label, std::string value) {
    return std::move(label) + ": " + std::move(value) +
           " — unsaved draft; Apply saves it and Discard restores the committed value. / "
           "未保存草稿；应用会保存，丢弃会恢复已提交值。";
}

Json build_theme_row(const settings_theme::PanelTheme theme) {
    Json children = Json::array();
    children.push_back(text_node("Panel appearance / 界面外观", "value", 28));
    children.push_back(Json{{"type", "dropdown"},
                            {"id", "settings.theme"},
                            {"action", "settings.theme.select"},
                            {"selected_id", theme == settings_theme::PanelTheme::light ? 0 : 1},
                            {"items", Json::array({
                                          {{"id", 0}, {"label", "Light / 浅色"}, {"value", "light"}},
                                          {{"id", 1}, {"label", "Dark / 深色"}, {"value", "dark"}},
                                      })}});
    return row_node(std::move(children));
}

// Runtime truth for sound controls when the settings document never stored
// the key; the rows then show the live mixer value instead of a fabricated
// default.
bool runtime_sound_enabled() noexcept {
#if defined(SAO_SETTINGS_PANEL_UI)
    bool enabled = true;
    if (sao_ui_sound_get_enabled(&enabled) == SAO_STATUS_OK)
        return enabled;
#endif
    return true;
}

double runtime_sound_volume() noexcept {
#if defined(SAO_SETTINGS_PANEL_UI)
    std::int32_t volume = 70;
    if (sao_ui_sound_get_volume(&volume) == SAO_STATUS_OK)
        return std::clamp(static_cast<double>(volume), 0.0, 100.0);
#endif
    return 70.0;
}

bool runtime_streaming_enabled() noexcept {
#if defined(SAO_SETTINGS_PANEL_UI)
    return sao_streaming_flow_get_mode();
#else
    return true;
#endif
}

Json open_panel_button(std::string_view name, std::string label) {
    return button_node("settings.open." + std::string(name), std::move(label),
                       "settings.panel.open", Json{{"name", std::string(name)}}, "ghost");
}

#if defined(SAO_SETTINGS_HAS_LICENSE_PROVIDER) && SAO_SETTINGS_HAS_LICENSE_PROVIDER
std::string license_tier_label(int32_t tier) {
    switch (tier) {
    case 0: return "Unknown / 未知";
    case 1: return "Free / 免费";
    case 2: return "Paid / 付费";
    case 3: return "Internal / 内部";
    default: return "Tier " + std::to_string(tier);
    }
}

std::string expiry_label(std::int64_t expiry_ms) {
    if (expiry_ms <= 0)
        return "No expiry / 无到期";
    const std::time_t seconds = static_cast<std::time_t>(expiry_ms / 1000);
    std::tm broken{};
#if defined(_WIN32)
    if (localtime_s(&broken, &seconds) != 0)
        return "unknown";
#else
    if (localtime_r(&seconds, &broken) == nullptr)
        return "unknown";
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &broken) == 0)
        return "unknown";
    return buffer;
}
#endif

Json hotkeys_section(const Json& snapshot) {
    Json rows = Json::array();
    std::size_t total = 0;
    try {
        const auto hotkeys = snapshot.find("hotkeys");
        if (hotkeys != snapshot.end() && hotkeys->is_object()) {
            total = hotkeys->size();
            std::size_t shown = 0;
            for (auto it = hotkeys->begin(); it != hotkeys->end() && shown < 8U; ++it) {
                if (!it.value().is_object())
                    continue;
                const Json& binding = it.value();
                const std::uint32_t vk = binding.value("vk", 0U);
                const std::uint32_t modifiers = binding.value("mods", 0U);
                std::string description = binding.value("description", std::string());
                if (description.empty())
                    description = it.key();
                rows.push_back(row_node(Json::array({
                    text_node(description, "value", 28),
                    badge_node(sao::launcher::hotkey::format_combo_utf8(vk, modifiers),
                               "accent"),
                })));
                ++shown;
            }
        }
    } catch (...) {
        rows.push_back(text_node("Hotkey list read failed. / 快捷键读取失败。", "warn", 30));
    }
    if (total == 0U) {
        rows.push_back(text_node("No hotkeys saved yet. / 尚未保存快捷键。", "muted", 30));
    } else if (total > 8U) {
        rows.push_back(text_node("…and " + std::to_string(total - 8U) + " more. / 另有 " +
                                     std::to_string(total - 8U) + " 个。",
                                 "muted", 28));
    }
    rows.push_back(row_node(Json::array({
        open_panel_button("hotkeys", "打开快捷键面板 / Open hotkey panel")})));
    return Json::array({card_node("Hotkeys / 快捷键", std::move(rows), "cyan")});
}

Json plugins_section() {
    Json rows = Json::array();
#if defined(SAO_SETTINGS_HAS_PLUGIN_LOADER) && SAO_SETTINGS_HAS_PLUGIN_LOADER
    try {
        namespace Loader = sao::plugins::loader;
        const Loader::registry_handle_t registry = Loader::sao_plugins_registry_instance();
        if (registry == nullptr) {
            rows.push_back(
                text_node("Plugin runtime is not started. / 插件运行时未启动。", "muted", 32));
        } else {
            const auto manifests = Loader::snapshot_manifests(registry);
            std::size_t enabled = 0, active = 0, failed = 0;
            for (const auto& manifest : manifests) {
                if (manifest.enabled)
                    ++enabled;
                const Loader::plugin_handle_t handle =
                    Loader::sao_plugins_registry_find(registry, manifest.plugin_id.c_str());
                if (handle == nullptr)
                    continue;
                const auto state = Loader::sao_plugins_lifecycle_state(handle);
                if (state == Loader::lifecycle_state::loaded_active)
                    ++active;
                else if (state == Loader::lifecycle_state::failed)
                    ++failed;
            }
            rows.push_back(text_node(std::to_string(manifests.size()) + " plugins, " +
                                         std::to_string(enabled) + " enabled, " +
                                         std::to_string(active) + " active, " +
                                         std::to_string(failed) + " failed. / 共 " +
                                         std::to_string(manifests.size()) + " 个插件，" +
                                         std::to_string(enabled) + " 启用，" +
                                         std::to_string(active) + " 运行中，" +
                                         std::to_string(failed) + " 失败。",
                                     "value", 40));
            if (failed != 0U)
                rows.push_back(text_node(
                    "Some plugins failed; open the manager for details. / 有插件加载失败。",
                    "warn", 30));
        }
    } catch (...) {
        rows.push_back(text_node("Plugin snapshot failed. / 插件状态读取失败。", "warn", 32));
    }
#else
    rows.push_back(text_node(
        "Plugin loader is not linked in this build. / 此构建未链接插件加载器。", "muted", 32));
#endif
    rows.push_back(row_node(Json::array({
        open_panel_button("plugins", "打开插件管理 / Open plugin manager")})));
    return Json::array({card_node("Plugins / 插件", std::move(rows), "cyan")});
}

Json license_section() {
    Json rows = Json::array();
#if defined(SAO_SETTINGS_HAS_LICENSE_PROVIDER) && SAO_SETTINGS_HAS_LICENSE_PROVIDER
    sao_license_provider_status_t provider{};
    provider.struct_size = sizeof(provider);
    if (sao_license_provider_status(&provider) == 0 && provider.initialized != 0) {
        rows.push_back(row_node(Json::array({
            text_node("Tier / 等级", "value", 28),
            badge_node(license_tier_label(provider.tier),
                       provider.tier == 2 || provider.tier == 3 ? "accent" : "muted"),
        })));
        rows.push_back(row_node(Json::array({
            badge_node(provider.activated != 0 ? "Activated / 已激活" : "Not activated / 未激活",
                       provider.activated != 0 ? "ok" : "muted"),
            badge_node(provider.revoked != 0 ? "Revoked / 已吊销" : "Valid / 有效",
                       provider.revoked != 0 ? "danger" : "ok"),
            badge_node(provider.heartbeat_running != 0 ? "Heartbeat on / 心跳运行"
                                                       : "Heartbeat off / 心跳停止",
                       provider.heartbeat_running != 0 ? "ok" : "warn"),
        })));
        rows.push_back(text_node("Expiry / 到期: " + expiry_label(provider.expiry_ms),
                                 "muted", 30));
    } else {
        rows.push_back(
            text_node("License provider is not running. / 许可服务未运行。", "muted", 32));
    }
#else
    rows.push_back(text_node(
        "License provider is not linked in this build. / 此构建未链接许可提供方。", "muted", 32));
#endif
    rows.push_back(row_node(Json::array({
        open_panel_button("license", "打开授权面板 / Open license panel")})));
    return Json::array({card_node("License / 授权", std::move(rows), "cyan")});
}

Json account_section() {
    Json rows = Json::array();
#if defined(SAO_SETTINGS_HAS_LICENSE_PROVIDER) && SAO_SETTINGS_HAS_LICENSE_PROVIDER
    try {
        const auto config = sao::launcher::launcherProviderConfigurationSnapshot();
        sao_license_provider_status_t provider{};
        provider.struct_size = sizeof(provider);
        const bool have_status =
            sao_license_provider_status(&provider) == 0 && provider.initialized != 0;
        rows.push_back(text_node("License endpoint / 许可端点", "value", 28));
        rows.push_back(text_node(config.license.endpoint.empty()
                                     ? "not configured / 未配置"
                                     : config.license.endpoint,
                                 "muted", 34));
        rows.push_back(text_node("Hardware ID / 硬件标识", "value", 28));
        rows.push_back(text_node(have_status && provider.hwid_masked[0] != '\0'
                                     ? provider.hwid_masked
                                     : "unavailable / 未获取",
                                 "muted", 30));
    } catch (...) {
        rows.push_back(text_node("Account snapshot failed. / 账户信息读取失败。", "warn", 32));
    }
#else
    rows.push_back(text_node(
        "Account details unavailable in this build. / 此构建不提供账户信息。", "muted", 32));
#endif
    return Json::array({card_node("Account / 账户", std::move(rows), "cyan")});
}

Json files_section(std::string_view settings_path_utf8) {
    Json rows = Json::array();
    std::string profiles_path_utf8;
    std::wstring profiles_wide;
    if (profiles_directory(profiles_wide) == SAO_STATUS_OK)
        profiles_path_utf8 = wide_to_utf8(profiles_wide);
    Json settings_children = Json::array({
        text_node("Settings file / 设置文件", "value", 28),
        text_node(settings_path_utf8.empty() ? "unknown / 未知" : std::string(settings_path_utf8),
                  "muted", 34),
    });
    Json profiles_children = Json::array({
        text_node("Profiles folder / 配置目录", "value", 28),
        text_node(profiles_path_utf8.empty() ? "unknown / 未知" : profiles_path_utf8, "muted", 34),
    });
#if defined(_WIN32)
    settings_children.push_back(row_node(Json::array({button_node(
        "settings.files.reveal.settings", "在资源管理器中显示 / Reveal in Explorer",
        "settings.files.reveal", {{"target", "settings"}}, "ghost")})));
    profiles_children.push_back(row_node(Json::array({button_node(
        "settings.files.reveal.profiles", "打开配置目录 / Open profiles folder",
        "settings.files.reveal", {{"target", "profiles"}}, "ghost")})));
#endif
    rows.push_back(card_node("Settings file / 设置文件", std::move(settings_children), "cyan"));
    rows.push_back(
        card_node("Profiles folder / 配置目录", std::move(profiles_children), "cyan"));
    rows.push_back(card_node(
        "Import / Export 导入导出",
        Json::array({
            text_node("Export writes the current settings document; import loads a file into "
                      "the draft — apply to save it. / 导出写入当前设置文档；导入先载入草稿，"
                      "应用后保存。",
                      "muted", 44),
            row_node(Json::array({
                button_node("settings.files.export", "导出设置… / Export…", "settings.files.export",
                            Json::object(), "primary"),
                button_node("settings.files.import", "导入设置… / Import…", "settings.files.import",
                            Json::object(), "ghost"),
            })),
        }),
        "gold"));
    return rows;
}

std::string make_spec(const Json& snapshot, std::string_view status, sao_status_t status_code,
                      bool dirty, std::string_view path, std::string_view profile_preview,
                      const std::vector<std::string>& profile_names,
                      std::size_t selected_section = 0U) {
    selected_section = std::min(selected_section, kSections.size() - 1U);
    std::array<Json, 12> section_children;
    for (auto& children : section_children)
        children = Json::array();

    const std::string_view overview_accent =
        status_code == SAO_STATUS_OK ? (dirty ? "gold" : "ok") : "danger";
    Json overview = Json::array();
    overview.push_back(row_node(Json::array({text_node("偏好设置", "title", 40),
        badge_node(status_code != SAO_STATUS_OK ? "操作失败" : dirty ? "未保存" : "已保存", overview_accent),
        text_node(std::string(status), "muted", 28)})));
    overview.push_back(text_node(
        dirty ? "更改已实时预览。应用以保存，或丢弃草稿恢复原值。"
              : "让 SAO 适合你的工作方式。选择分类调整外观、行为与声音。",
        dirty ? "warn" : "muted", 28));

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
            has_sound_enabled = has_sound_enabled || key == "sound_enabled";
            has_sound_volume = has_sound_volume || key == "sound_volume";
            // Dedicated sections own these keys; the generic loop skips them.
            if (key == "panel_themes" || key == "hotkeys")
                continue;
            const int index = section_index(key, value);
            if (static_cast<std::size_t>(index) != selected_section)
                continue;
            const std::string display_key = setting_label(key);
            if (value.is_boolean()) {
                const bool on = value.get<bool>();
                if (is_editable_bool(key)) {
                    section_children[index].push_back(row_node(Json::array({
                        button_node("settings.bool." + key, display_key, kSettingsActionToggle,
                                    {{"key", key}}, on ? "primary" : "ghost", on),
                        badge_node(on ? "On / 开" : "Off / 关", on ? "ok" : "muted"),
                    })));
                } else {
                    // Read-only: the value is real but no live writer exists
                    // for this key, so render a badge instead of a switch.
                    section_children[index].push_back(row_node(Json::array({
                        text_node(display_key, "value", 28),
                        badge_node(on ? "On / 开" : "Off / 关", on ? "ok" : "muted"),
                        badge_node("只读 / read-only", "muted"),
                    })));
                }
            } else if (is_numeric_control(key) && value.is_number()) {
                const double numeric_value = value.get<double>();
                const double effective_value =
                    std::isfinite(numeric_value) ? std::clamp(numeric_value, 0.0, 100.0) : 0.0;
                Json controls = Json::array(
                    {text_node(display_key, "value", 28),
                     Json{{"type", "slider"},
                          {"id", "settings.volume." + key},
                          {"action", "settings.volume.set"},
                          {"payload", {{"key", key}}},
                          {"value", effective_value / 100.0},
                          {"show_value_label", false}},
                     badge_node(percent_label(numeric_value), "accent")});
                section_children[index].push_back(row_node(std::move(controls)));
            } else {
                section_children[index].push_back(text_node(display_key + ": " + summary(value), "muted", 32));
            }
        }
    }
    // When the document never stored the sound keys the runtime still has
    // live values — show those (marked as defaults) instead of fabricating
    // On/70% pretending to be real settings.
    if (!has_sound_enabled) {
        const bool enabled = runtime_sound_enabled();
        section_children[3].push_back(row_node(Json::array({
            button_node("settings.bool.sound_enabled", "UI sound / 界面音效",
                        kSettingsActionToggle, {{"key", "sound_enabled"}},
                        enabled ? "primary" : "ghost", enabled),
            badge_node(enabled ? "On / 开" : "Off / 关", enabled ? "ok" : "muted"),
            badge_node("默认 Default", "muted"),
        })));
    }
    if (!has_sound_volume) {
        const double volume = runtime_sound_volume();
        Json controls = Json::array(
            {text_node("Sound volume / 音效音量", "value", 28),
             Json{{"type", "slider"},
                  {"id", "settings.volume.sound_volume"},
                  {"action", "settings.volume.set"},
                  {"payload", {{"key", "sound_volume"}}},
                  {"value", volume / 100.0},
                  {"show_value_label", false}},
             badge_node(percent_label(volume), "accent"),
             badge_node("默认 Default", "muted")});
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

    // Dedicated nav sections — built only when selected, matching the
    // profiles pattern (content embeds section_children[selected_section]).
    if (selected_section == static_cast<std::size_t>(kSectionHotkeys))
        section_children[kSectionHotkeys] = hotkeys_section(snapshot);
    if (selected_section == static_cast<std::size_t>(kSectionPlugins))
        section_children[kSectionPlugins] = plugins_section();
    if (selected_section == static_cast<std::size_t>(kSectionLicense))
        section_children[kSectionLicense] = license_section();
    if (selected_section == static_cast<std::size_t>(kSectionAccount))
        section_children[kSectionAccount] = account_section();
    if (selected_section == static_cast<std::size_t>(kSectionFiles))
        section_children[kSectionFiles] = files_section(path);
    if (section_children[kSectionOther].empty())
        section_children[kSectionOther].push_back(
            text_node("No other settings. / 无其他设置项。", "muted", 30));

    Json actions = Json::array();
    actions.push_back(button_node("settings.defaults", "恢复默认",
                                  "settings.defaults", Json::object(), "ghost"));
    actions.push_back(button_node("settings.refresh", "刷新", kSettingsActionRefresh,
                                  Json::object(), "ghost"));
    actions.push_back(Json{{"type", "spacer"}, {"weight", 1.0}, {"height", 1}});
    actions.push_back(button_node("settings.cancel", "丢弃草稿", "settings.cancel",
                                  Json::object(), "ghost", false, !dirty));
    actions.push_back(button_node("settings.close", "关闭", kSettingsActionClose,
                                  Json::object(), "ghost"));
    actions.push_back(button_node("settings.apply", "应用更改", "settings.apply",
                                  Json::object(), "primary", false, !dirty));
    Json footer = row_node(std::move(actions));
    footer["id"] = "settings-footer";
    footer["dock"] = "bottom";
    footer["height"] = 64;
    footer["padding"] = 12;

    Json nodes = Json::array();
    Json header = section_node("", std::move(overview), overview_accent);
    header["id"] = "settings-header";
    header["dock"] = "top";
    header["height"] = 104;
    header["padding"] = 12;
    header["gap"] = 4;
    header["scroll"] = {{"axis", "vertical"}, {"bar", "auto"}, {"wheel", true}};
    nodes.push_back(std::move(header));
    nodes.push_back(std::move(footer));
    Json navigation = Json::array();
    for (std::size_t index = 0; index < kSections.size(); ++index)
        navigation.push_back(button_node(
            "settings.section." + std::to_string(index), std::string(kSections[index]),
            "settings.section.select", {{"section", index}},
                index == selected_section ? "nav-active" : "nav", index == selected_section));
            Json rail = section_node("PREFERENCES", std::move(navigation), "gold");
    rail["id"] = "settings-category-rail";
    rail["width"] = 208;
    rail["min_width"] = 176;
    rail["min_height"] = 96;
    rail["weight"] = 0;
    rail["scroll"] = {{"axis", "vertical"}, {"bar", "auto"}, {"wheel", true}};
    if (section_children[0].empty()) {
        section_children[0] = Json::array({
            text_node("从常用设置开始，或在左侧选择完整分类。", "muted", 32),
            card_node("外观与显示", Json::array({
                text_node("调整浅色、深色与各面板的显示偏好。", "muted", 32),
                button_node("settings.overview.appearance", "打开外观设置", "settings.section.select", {{"section", 1}}, "ghost") })),
            card_node("音效与提示", Json::array({
                text_node("管理界面提示音、音量和试听；更改会即时预览。", "muted", 32),
                button_node("settings.overview.audio", "打开音频设置", "settings.section.select", {{"section", 3}}, "ghost") })),
            card_node("配置与草稿", Json::array({
                text_node("切换分类不会丢弃草稿；使用底部操作应用或撤销更改。", "muted", 42),
                button_node("settings.overview.profiles", "管理配置", "settings.section.select", {{"section", 5}}, "ghost") }))
        });
    }
    if (section_children[selected_section].empty())
        section_children[selected_section].push_back(
            text_node("选择分类查看对应设置；切换分类不会丢弃当前草稿。", "muted", 42));
    if (selected_section == 0U && !path.empty())
        section_children[0].push_back(text_node("设置位置  " + std::string(path), "muted", 40));
    Json content = section_node(std::string(kSections[selected_section]),
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
                         {"padding", 8},
                         {"gap", 16},
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
#if defined(SAO_SETTINGS_HAS_FILE_PICKER) && SAO_SETTINGS_HAS_FILE_PICKER
sao_ui_file_picker_handle_t ensure_file_picker() noexcept;
void SAO_UI_CALL export_picker_result(sao_status_t picker_status, const char* path_utf8,
                                      void* user_data) noexcept;
#endif

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
#if defined(SAO_SETTINGS_HAS_FILE_PICKER) && SAO_SETTINGS_HAS_FILE_PICKER
    // Best-effort: the export affordance reports its own error when absent.
    (void)ensure_file_picker();
#endif
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
    const bool owner_dirty = owner_lease->dirty();
    {
        std::lock_guard lock(state().mutex);
        if (!state().draft_initialized || !state().draft_dirty) {
            state().draft_snapshot = owner_document;
            state().committed_snapshot = owner_document;
            state().draft_initialized = true;
            state().draft_dirty = false;
            state().committed_owner_dirty = owner_dirty;
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
        return first_error(status, publish_status);
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
        const sao_status_t enabled_status = sao_ui_sound_set_enabled(enabled);
        sao::launcher::refreshUserGuideSoundPolicy();
        const sao_status_t volume_status = sao_ui_sound_set_volume(volume);
        sao::launcher::refreshUserGuideSoundPolicy();
        return first_error(enabled_status, volume_status);
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
#else
    (void)snapshot;
    return SAO_STATUS_OK;
#endif
}

#if defined(SAO_SETTINGS_PANEL_UI)
sao_status_t apply_streaming_mode_runtime(bool enabled) noexcept {
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    bool applied = false;
    const sao_sdk_status_t status =
        sao_sdk_platform_apply_streaming_mode(enabled ? 1 : 0, &applied);
    if (applied && status == SAO_SDK_OK)
        return SAO_STATUS_OK;
#endif
    const uint64_t generation = sao_streaming_flow_set_mode(enabled);
    if (generation == 0 && !sao_streaming_flow_get_mode() && !enabled)
        return SAO_STATUS_OK;
    if (generation == 0 || sao_streaming_flow_get_mode() != enabled)
        return SAO_STATUS_ERR_TIMEOUT;
    return SAO_STATUS_OK;
}
#endif

sao_status_t restore_runtime_streaming(const Json& snapshot) noexcept {
#if defined(SAO_SETTINGS_PANEL_UI)
    const auto value = snapshot.find("streaming_mode");
    if (value != snapshot.end() && !value->is_boolean())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return apply_streaming_mode_runtime(value != snapshot.end() && value->get<bool>());
#else
    (void)snapshot;
    return SAO_STATUS_OK;
#endif
}

sao_status_t restore_runtime_settings(const Json& snapshot) noexcept {
    const sao_status_t theme_status = restore_runtime_theme(snapshot);
    const sao_status_t sound_status = restore_runtime_sound(snapshot);
    const sao_status_t streaming_status = restore_runtime_streaming(snapshot);
    return first_error(theme_status, first_error(sound_status, streaming_status));
}

sao_status_t rollback_draft_mutation(const settings_owner::SettingsOwner::Lease& owner_lease,
                                     const Json& snapshot, bool owner_dirty,
                                     bool draft_dirty) noexcept {
    if (!owner_lease)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    const sao_status_t owner_status = owner_lease->restore_snapshot(snapshot, owner_dirty);
    if (owner_status == SAO_STATUS_OK) {
        const sao_status_t runtime_status = restore_runtime_settings(snapshot);
        try {
            std::lock_guard lock(state().mutex);
            state().draft_snapshot = snapshot;
            state().draft_initialized = true;
            state().draft_dirty = draft_dirty;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        return first_error(owner_status, runtime_status);
    }
    // Document still holds the mutation; keep runtime at the mutated state.
    sync_draft_snapshot(owner_lease);
    std::lock_guard lock(state().mutex);
    state().draft_initialized = true;
    state().draft_dirty = true;
    return owner_status;
}

sao_status_t merge_live_game_cache(Json& profile, const Json& live) noexcept {
    try {
        const auto live_cache = live.find("game_cache");
        if (live_cache == live.end() || !live_cache->is_object())
            return SAO_STATUS_OK;
        Json merged = Json::object();
        const auto profile_cache = profile.find("game_cache");
        if (profile_cache != profile.end() && profile_cache->is_object())
            merged = *profile_cache;
        for (auto it = live_cache->begin(); it != live_cache->end(); ++it)
            merged[it.key()] = it.value();
        profile["game_cache"] = std::move(merged);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (const nlohmann::json::exception&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

#if defined(SAO_SETTINGS_PANEL_UI)

#if defined(SAO_SETTINGS_HAS_FILE_PICKER) && SAO_SETTINGS_HAS_FILE_PICKER
// Lazy creation — ensure_panel attempts this at register time; a second try
// here keeps export usable if that earlier call raced the compositor.
sao_ui_file_picker_handle_t ensure_file_picker() noexcept {
    sao_ui_file_picker_handle_t picker = nullptr;
    sao_ui_compositor_handle_t compositor = nullptr;
    {
        std::lock_guard lock(state().mutex);
        picker = state().file_picker;
        compositor = state().compositor;
    }
    if (picker != nullptr)
        return picker;
    if (compositor == nullptr)
        compositor = borrowed_compositor();
    if (compositor == nullptr)
        return nullptr;
    if (sao_ui_file_picker_create(compositor, &picker) != SAO_STATUS_OK)
        return nullptr;
    {
        std::lock_guard lock(state().mutex);
        if (state().file_picker == nullptr)
            state().file_picker = picker;
        else
            picker = state().file_picker;
    }
    return picker;
}

void SAO_UI_CALL export_picker_result(sao_status_t picker_status, const char* path_utf8,
                                      void*) noexcept {
    sao_status_t status = picker_status;
    std::string message;
    if (status == SAO_STATUS_OK && (path_utf8 == nullptr || *path_utf8 == '\0'))
        status = SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (status == SAO_STATUS_OK) {
        settings_owner::SettingsOwner* owner = nullptr;
        {
            std::lock_guard lock(state().mutex);
            owner = state().owner;
        }
        Json document;
        auto owner_lease = owner == nullptr ? settings_owner::SettingsOwner::Lease{}
                                            : owner->acquire_lease();
        if (!owner_lease || owner_lease->snapshot(document) != SAO_STATUS_OK)
            status = SAO_STATUS_ERR_NOT_INITIALIZED;
        std::string envelope;
        if (status == SAO_STATUS_OK)
            status = settings_codec::encode(document, envelope);
        if (status == SAO_STATUS_OK) {
            try {
                std::ofstream output(
                    std::filesystem::path(std::u8string(
                        reinterpret_cast<const char8_t*>(path_utf8))),
                    std::ios::binary | std::ios::trunc);
                if (!output) {
                    status = SAO_STATUS_ERR_OS_CALL_FAILED;
                } else {
                    output.write(envelope.data(),
                                 static_cast<std::streamsize>(envelope.size()));
                    if (!output)
                        status = SAO_STATUS_ERR_OS_CALL_FAILED;
                }
            } catch (...) {
                status = SAO_STATUS_ERR_OS_CALL_FAILED;
            }
        }
        message = status == SAO_STATUS_OK ? "Settings exported. / 设置已导出。"
                                          : "Export failed. / 导出失败。";
    } else {
        message = status == SAO_STATUS_ERR_CANCELLED ? "Export cancelled. / 已取消导出。"
                                                     : "Export picker failed. / 导出失败。";
    }
    update_status(status, std::move(message));
    (void)publish();
}
#endif

sao_status_t publish() noexcept {
    settings_owner::SettingsOwner::Lease owner_lease;
    Json snapshot;
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
    try {
        const std::filesystem::path file(path);
        const auto unavailable_status = [&file]() noexcept {
            std::error_code exists_error;
            const bool exists = std::filesystem::exists(file, exists_error);
            return !exists && !exists_error ? SAO_STATUS_ERR_NOT_FOUND
                                            : SAO_STATUS_ERR_ACCESS_DENIED;
        };
        std::error_code file_error;
        const std::uintmax_t file_size = std::filesystem::file_size(file, file_error);
        if (file_error)
            return unavailable_status();
        if (file_size > kMaximumProfileBytes)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        std::ifstream input(file, std::ios::binary);
        if (!input)
            return unavailable_status();
        input >> out;
        return out.is_object() ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (const nlohmann::json::exception&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

std::string profile_failure_text(sao_status_t status, std::string_view operation) {
    switch (status) {
    case SAO_STATUS_ERR_NOT_FOUND:
        return std::string(operation) + ": profile does not exist";
    case SAO_STATUS_ERR_INVALID_ARGUMENT:
        return std::string(operation) + ": profile parse failed";
    case SAO_STATUS_ERR_ACCESS_DENIED:
        return std::string(operation) + ": permission denied";
    case SAO_STATUS_ERR_BUFFER_TOO_SMALL:
        return std::string(operation) + ": profile is too large";
    default:
        return std::string(operation) + ": operation failed";
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
        std::wstring validated_path;
        const sao_status_t validation_status = profile_path(name, validated_path);
        const bool saved = validation_status == SAO_STATUS_OK && save_profile(name);
        refresh_profile_names();
        update_status(saved ? SAO_STATUS_OK
                            : validation_status != SAO_STATUS_OK
                                ? validation_status
                                : SAO_STATUS_ERR_OS_CALL_FAILED,
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
        // Play at the live mixer volume; on read failure request the maximum
        // and let the engine clamp to the global level (min(request, global)).
        std::int32_t volume = 100;
        (void)sao_ui_sound_get_volume(&volume);
        return sao_ui_sound_play(static_cast<SaoUiSoundCue>(cue->get<int>()),
                                 std::clamp(volume, 0, 100));
#else
        return SAO_STATUS_ERR_NOT_INITIALIZED;
#endif
    }

    if (action == "settings.panel.open") {
        std::string name;
        if (data.size() != 1 || !payload_string(data, "name", name))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        static constexpr std::array<std::string_view, 7> kRoutablePanels{
            "settings", "hotkeys", "plugins", "license", "workshop", "process", "memory"};
        if (std::find(kRoutablePanels.begin(), kRoutablePanels.end(),
                      std::string_view(name)) == kRoutablePanels.end())
            return publish_after_draft_mutation(SAO_STATUS_ERR_NOT_FOUND,
                                                "Unknown panel. / 未知面板。");
#if defined(SAO_SETTINGS_HAS_PANEL_OPEN) && SAO_SETTINGS_HAS_PANEL_OPEN
        const sao_sdk_status_t sdk_status = sao_sdk_platform_open_panel(name.c_str());
        const sao_status_t mapped =
            sdk_status == SAO_SDK_OK ? SAO_STATUS_OK
            : sdk_status == SAO_SDK_ERR_NOT_FOUND ? SAO_STATUS_ERR_NOT_FOUND
            : sdk_status == SAO_SDK_ERR_NOT_INITIALIZED ? SAO_STATUS_ERR_NOT_INITIALIZED
                                                        : SAO_STATUS_ERR_UNKNOWN;
        return publish_after_draft_mutation(
            mapped, mapped == SAO_STATUS_OK ? "Opened " + name + ". / 已打开。"
                                            : "Panel unavailable. / 面板不可用。");
#else
        return publish_after_draft_mutation(
            SAO_STATUS_ERR_CAPABILITY_MISSING,
            "Panel routing is not available in this build. / 此构建不支持面板跳转。");
#endif
    }

    if (action == "settings.files.reveal") {
        std::string target;
        if (data.size() != 1 || !payload_string(data, "target", target) ||
            (target != "settings" && target != "profiles"))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
        std::wstring wide;
        sao_status_t status = SAO_STATUS_OK;
        if (target == "profiles") {
            status = profiles_directory(wide);
        } else {
            settings_owner::SettingsOwner* owner = nullptr;
            {
                std::lock_guard lock(state().mutex);
                owner = state().owner;
            }
            status = owner == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : owner->path(wide);
        }
        if (status == SAO_STATUS_OK && wide.empty())
            status = SAO_STATUS_ERR_NOT_FOUND;
        if (status == SAO_STATUS_OK) {
            HINSTANCE result;
            if (target == "settings") {
                const std::wstring params = L"/select,\"" + wide + L"\"";
                result = ::ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(),
                                         nullptr, SW_SHOWNORMAL);
            } else {
                result = ::ShellExecuteW(nullptr, L"explore", wide.c_str(), nullptr, nullptr,
                                         SW_SHOWDEFAULT);
            }
            if (reinterpret_cast<INT_PTR>(result) <= 32)
                status = SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        return publish_after_draft_mutation(
            status, status == SAO_STATUS_OK ? "Revealed in Explorer. / 已在资源管理器中显示。"
                                            : "Reveal failed. / 打开失败。");
#else
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
    }

    if (action == "settings.files.export") {
#if defined(SAO_SETTINGS_PANEL_UI) && SAO_SETTINGS_HAS_FILE_PICKER
        sao_ui_file_picker_handle_t picker = ensure_file_picker();
        if (picker == nullptr)
            return publish_after_draft_mutation(
                SAO_STATUS_ERR_NOT_INITIALIZED,
                "File picker unavailable. / 文件选择器不可用。");
        std::string initial_utf8;
        {
            settings_owner::SettingsOwner* owner = nullptr;
            {
                std::lock_guard lock(state().mutex);
                owner = state().owner;
            }
            if (owner != nullptr) {
                std::wstring owner_path;
                if (owner->path(owner_path) == SAO_STATUS_OK)
                    initial_utf8 = wide_to_utf8(owner_path);
            }
        }
        SaoUiFilePickerConfig config{};
        config.title_utf8 = "Export settings / 导出设置";
        config.initial_path_utf8 = initial_utf8.empty() ? nullptr : initial_utf8.c_str();
        config.filter_utf8 = "Settings|*.json";
        config.mode = SAO_UI_FILE_PICKER_SAVE;
        const sao_status_t shown =
            sao_ui_file_picker_show(picker, &config, &export_picker_result, nullptr);
        return publish_after_draft_mutation(
            shown, shown == SAO_STATUS_OK ? "Choose where to save. / 选择保存位置。"
                                          : "Export picker failed. / 导出窗口打开失败。");
#else
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
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
    const bool owner_dirty_before = owner_lease->dirty();

    if (action == kSettingsActionRefresh) {
        refresh_profile_names();
        status = update_status(
            SAO_STATUS_OK,
            dirty ? "Refreshed; the current draft is still unsaved. / 已刷新；当前草稿仍未保存。"
                  : "Refreshed; there are no pending changes. / 已刷新；没有待处理更改。");
#if defined(SAO_SETTINGS_PANEL_UI)
        if (state().body != nullptr)
            return publish();
#endif
        return status;
    }

    if (action == "settings.apply") {
        Json committed;
        bool committed_owner_dirty = false;
        {
            std::lock_guard lock(state().mutex);
            committed = state().committed_snapshot;
            committed_owner_dirty = state().committed_owner_dirty;
        }
        sync_draft_snapshot(owner_lease);
        status = owner_lease->save();
        if (status == SAO_STATUS_OK) {
            {
                std::lock_guard lock(state().mutex);
                state().committed_snapshot = state().draft_snapshot;
                state().draft_dirty = false;
                state().committed_owner_dirty = false;
            }
            return publish_after_draft_mutation(
                status, "Draft applied and saved. / 草稿已应用并保存。");
        }
        const sao_status_t save_status = status;
        const sao_status_t rollback_status = rollback_draft_mutation(
            owner_lease, committed, committed_owner_dirty, false);
        return publish_after_draft_mutation(
            rollback_status == SAO_STATUS_OK ? save_status : rollback_status,
            rollback_status == SAO_STATUS_OK
                ? "Save failed; committed settings were restored and the draft was cleared. / 保存失败；已恢复提交值并清除草稿。"
                : "Save failed and committed settings could not be fully restored. / 保存失败，且提交值未能完整恢复。");
    }

    if (action == "settings.cancel") {
        Json committed;
        bool committed_owner_dirty = false;
        {
            std::lock_guard lock(state().mutex);
            committed = state().committed_snapshot;
            committed_owner_dirty = state().committed_owner_dirty;
        }
        status = owner_lease->restore_snapshot(committed, committed_owner_dirty);
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(state().mutex);
            state().draft_snapshot = committed;
            state().draft_dirty = false;
        }
        if (status == SAO_STATUS_OK)
            status = restore_runtime_settings(committed);
        return publish_after_draft_mutation(
            status, status == SAO_STATUS_OK
                        ? "Draft discarded; committed settings restored. / 草稿已丢弃；已恢复提交值。"
                        : "Discard failed; review the current values. / 丢弃失败；请检查当前值。");
    }

    if (action == "settings.defaults") {
        status = owner_lease->restore_snapshot(Json::object(), true);
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(state().mutex);
            state().draft_snapshot = Json::object();
            state().draft_dirty = true;
        }
        if (status == SAO_STATUS_OK) {
            const sao_status_t runtime_status = restore_runtime_settings(Json::object());
            if (runtime_status != SAO_STATUS_OK) {
                const sao_status_t rollback_status = rollback_draft_mutation(
                    owner_lease, snapshot, owner_dirty_before, dirty);
                status = first_error(runtime_status, rollback_status);
            }
        }
        return publish_after_draft_mutation(
            status,
            status == SAO_STATUS_OK
                ? "Defaults loaded into the draft. Apply to save or discard to restore committed settings. / 默认值已载入草稿；应用以保存，丢弃以恢复提交值。"
                : "Defaults could not be loaded into the draft. / 默认值未能载入草稿。");
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
        if (current == snapshot.end() && key != "sound_enabled" && key != "streaming_mode")
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT, "Invalid setting");
        const bool next = current == snapshot.end() ? key == "sound_enabled" : !current->get<bool>();
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
                if (runtime_status != SAO_STATUS_OK) {
                    const sao_status_t rollback_status = rollback_draft_mutation(
                        owner_lease, snapshot, owner_dirty_before, dirty);
                    status = first_error(runtime_status, rollback_status);
                } else if (next) {
                    (void)sao_ui_sound_play(SAO_UI_SOUND_CLICK, 50);
                }
            } else if (key == "streaming_mode") {
                const sao_status_t runtime_status = apply_streaming_mode_runtime(next);
                if (runtime_status != SAO_STATUS_OK) {
                    const sao_status_t rollback_status = rollback_draft_mutation(
                        owner_lease, snapshot, owner_dirty_before, dirty);
                    status = first_error(runtime_status, rollback_status);
                }
            }
        }
        return publish_after_draft_mutation(
            status, status == SAO_STATUS_OK
                        ? draft_feedback(setting_label(key), next ? "On / 开" : "Off / 关")
                        : setting_label(key) + " could not be changed. / 更改失败。");
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
                if (runtime_status != SAO_STATUS_OK) {
                    const sao_status_t rollback_status = rollback_draft_mutation(
                        owner_lease, snapshot, owner_dirty_before, dirty);
                    status = first_error(runtime_status, rollback_status);
                }
            }
            if (status == SAO_STATUS_OK) {
                std::lock_guard lock(state().mutex);
                state().draft_dirty = true;
            }
        }
        return publish_after_draft_mutation(
            status, status == SAO_STATUS_OK
                        ? draft_feedback(setting_label(key), percent_label(next))
                        : setting_label(key) + " could not be changed. / 更改失败。");
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
        const double current_value =
            current == snapshot.end() ? runtime_sound_volume() : current->get<double>();
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
                if (runtime_status != SAO_STATUS_OK) {
                    const sao_status_t rollback_status = rollback_draft_mutation(
                        owner_lease, snapshot, owner_dirty_before, dirty);
                    status = first_error(runtime_status, rollback_status);
                }
            }
            sao::launcher::refreshUserGuideSoundPolicy();
        }
        return publish_after_draft_mutation(
            status, status == SAO_STATUS_OK
                        ? draft_feedback(setting_label(key), percent_label(next))
                        : setting_label(key) + " could not be changed. / 更改失败。");
    }

    if (action == "settings.files.import") {
#if defined(_WIN32) && defined(SAO_SETTINGS_PANEL_UI)
        char picked[kFilePickerPathBytes]{};
        const sao_status_t pick_status =
            sao_ui_dialog_file_picker_show("Import settings / 导入设置",
                                           "Settings file|*.json|All files|*.*", picked,
                                           sizeof(picked));
        if (pick_status == SAO_STATUS_ERR_CANCELLED)
            return publish_after_draft_mutation(SAO_STATUS_OK,
                                                "Import cancelled. / 已取消导入。");
        if (pick_status != SAO_STATUS_OK)
            return publish_after_draft_mutation(pick_status,
                                                "File picker failed. / 文件选择失败。");
        try {
            const std::filesystem::path file(std::u8string(
                reinterpret_cast<const char8_t*>(picked)));
            std::error_code file_error;
            const std::uintmax_t file_size = std::filesystem::file_size(file, file_error);
            if (file_error)
                return publish_after_draft_mutation(SAO_STATUS_ERR_NOT_FOUND,
                                                    "Import file missing. / 导入文件不存在。");
            if (file_size > settings_codec::kMaxEnvelopeBytes)
                return publish_after_draft_mutation(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                                                    "Import file too large. / 导入文件过大。");
            std::ifstream input(file, std::ios::binary);
            if (!input)
                return publish_after_draft_mutation(SAO_STATUS_ERR_ACCESS_DENIED,
                                                    "Import file unreadable. / 无法读取导入文件。");
            std::string raw(static_cast<std::size_t>(file_size), '\0');
            input.read(raw.data(), static_cast<std::streamsize>(file_size));
            raw.resize(static_cast<std::size_t>(input.gcount()));
            if (input.bad())
                return publish_after_draft_mutation(SAO_STATUS_ERR_OS_CALL_FAILED,
                                                    "Import read failed. / 读取导入文件失败。");
            settings_codec::DecodeResult decoded;
            status = settings_codec::decode(raw, decoded);
            if (status != SAO_STATUS_OK)
                return publish_after_draft_mutation(status,
                                                    "Import decode failed. / 导入解析失败。");
            if (!decoded.document.is_object())
                return publish_after_draft_mutation(
                    SAO_STATUS_ERR_INVALID_ARGUMENT,
                    "Import file is not a settings document. / 导入文件不是设置文档。");
            Json live_snapshot;
            const bool owner_was_dirty = owner_lease->dirty();
            status = owner_lease->snapshot(live_snapshot);
            if (status == SAO_STATUS_OK)
                status = merge_live_game_cache(decoded.document, live_snapshot);
            const Json document_copy = decoded.document;
            if (status == SAO_STATUS_OK)
                status = owner_lease->restore_snapshot(std::move(decoded.document), true);
            if (status == SAO_STATUS_OK) {
                const sao_status_t runtime_status = restore_runtime_settings(document_copy);
                if (runtime_status == SAO_STATUS_OK) {
                    std::lock_guard lock(state().mutex);
                    state().draft_snapshot = document_copy;
                    state().draft_initialized = true;
                    state().draft_dirty = true;
                } else {
                    const sao_status_t rollback_status = rollback_draft_mutation(
                        owner_lease, live_snapshot, owner_was_dirty, dirty);
                    status = first_error(runtime_status, rollback_status);
                }
            }
            return publish_after_draft_mutation(
                status,
                status == SAO_STATUS_OK
                    ? "Settings imported into the draft. Apply to save or discard to restore committed settings. / 设置已导入草稿；应用以保存，丢弃以恢复提交值。"
                    : "Import failed. / 导入失败。");
        } catch (const std::bad_alloc&) {
            return publish_after_draft_mutation(SAO_STATUS_ERR_UNKNOWN,
                                                "Import failed. / 导入失败。");
        } catch (const nlohmann::json::exception&) {
            return publish_after_draft_mutation(SAO_STATUS_ERR_INVALID_ARGUMENT,
                                                "Import parse failed. / 导入解析失败。");
        } catch (...) {
            return publish_after_draft_mutation(SAO_STATUS_ERR_OS_CALL_FAILED,
                                                "Import failed. / 导入失败。");
        }
#else
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
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
            if (runtime_status != SAO_STATUS_OK) {
                const sao_status_t rollback_status = rollback_draft_mutation(
                    owner_lease, snapshot, owner_dirty_before, dirty);
                status = first_error(runtime_status, rollback_status);
            } else {
                std::lock_guard lock(state().mutex);
                state().draft_dirty = true;
            }
        }
        return publish_after_draft_mutation(
            status,
            status == SAO_STATUS_OK
                ? draft_feedback("Panel appearance / 界面外观",
                                 next == settings_theme::PanelTheme::light ? "Light / 浅色"
                                                                          : "Dark / 深色")
                : "Theme could not be changed. / 主题更改失败。");
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
        Json live_snapshot;
        const bool owner_was_dirty = owner_lease->dirty();
        if (status == SAO_STATUS_OK)
            status = owner_lease->snapshot(live_snapshot);
        if (status == SAO_STATUS_OK)
            status = merge_live_game_cache(profile, live_snapshot);
        const Json profile_copy = profile;
        if (status == SAO_STATUS_OK)
            status = owner_lease->restore_snapshot(std::move(profile), true);
        if (status == SAO_STATUS_OK) {
            const sao_status_t runtime_status = restore_runtime_settings(profile_copy);
            if (runtime_status == SAO_STATUS_OK) {
                std::lock_guard lock(state().mutex);
                state().draft_snapshot = profile_copy;
                state().draft_initialized = true;
                state().draft_dirty = true;
            } else {
                const sao_status_t rollback_status = rollback_draft_mutation(
                    owner_lease, live_snapshot, owner_was_dirty, dirty);
                status = first_error(runtime_status, rollback_status);
            }
        }
        return publish_after_draft_mutation(
            status,
            status == SAO_STATUS_OK
                ? "Profile loaded into the draft. Apply to save or discard to restore committed settings. / 配置已载入草稿；应用以保存，丢弃以恢复提交值。"
                : profile_failure_text(status, "Load profile"));
    }
    return publish_after_draft_mutation(SAO_STATUS_ERR_NOT_FOUND, "Unknown settings action");
}

// Owner-side change callback (settings_owner::change_callback_fn). Fires on
// whichever thread committed the change — external_refresh handles the
// owner-thread requirement internally, so this only bridges the C ABI.
void on_owner_settings_changed(void*) noexcept {
    (void)sao_launcher_settings_config_external_refresh();
}

} // namespace

extern "C" sao_status_t sao_launcher_settings_config_external_refresh(void) {
#if defined(SAO_SETTINGS_PANEL_UI)
    try {
        if (require_owner_thread() != SAO_STATUS_OK) {
            // Foreign thread (owner callbacks run on the committing thread):
            // latch and let the next owner-thread pass repaint.
            std::lock_guard lock(state().mutex);
            if (state().accepting)
                state().external_change_pending = true;
            return SAO_STATUS_OK;
        }
        {
            std::lock_guard lock(state().mutex);
            state().external_change_pending = false;
        }
        refresh_profile_names();
        sao_ui_panel_body_handle_t body = nullptr;
        {
            std::lock_guard lock(state().mutex);
            body = state().body;
        }
        if (body == nullptr) {
            // Panel not constructed — still resync the committed snapshot so
            // the next open starts from the updated document.
            settings_owner::SettingsOwner::Lease lease;
            Json document;
            bool dirty = false;
            std::string path;
            (void)owner_snapshot(document, lease, dirty, path);
            return SAO_STATUS_OK;
        }
        {
            std::lock_guard lock(state().mutex);
            if (state().draft_dirty)
                state().status_text =
                    "Settings changed outside this panel; your draft is kept. / "
                    "设置已在面板外更改，草稿已保留。";
        }
        return publish();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    return SAO_STATUS_OK;
#endif
}

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
        if (previous != nullptr) {
            (void)previous->unsubscribe_change(&on_owner_settings_changed, nullptr);
            previous->retire_and_wait();
        }
        auto* next = reinterpret_cast<settings_owner::SettingsOwner*>(owner_opaque);
        if (next != nullptr)
            next->resume_after_retire();
        Json runtime_snapshot = Json::object();
        if (next != nullptr && next->snapshot(runtime_snapshot) != SAO_STATUS_OK)
            runtime_snapshot = Json::object();
        if (next != nullptr)
            (void)next->subscribe_change(&on_owner_settings_changed, nullptr);
        {
            std::lock_guard lock(state().mutex);
            state().owner = next;
            state().external_change_pending = false;
            state().draft_snapshot = Json::object();
            state().committed_snapshot = Json::object();
            state().draft_initialized = false;
            state().draft_dirty = false;
            state().committed_owner_dirty = false;
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
#if defined(SAO_SETTINGS_HAS_FILE_PICKER) && SAO_SETTINGS_HAS_FILE_PICKER
    sao_ui_file_picker_handle_t file_picker = nullptr;
    bool picker_only_teardown = false;
#endif
    bool action_attached = false;
    bool event_attached = false;
    {
        std::lock_guard lock(state().mutex);
        if (state().creating || state().operations != 0U || state().callbacks != 0U)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        if (state().panel == nullptr && state().body == nullptr
#if defined(SAO_SETTINGS_HAS_FILE_PICKER) && SAO_SETTINGS_HAS_FILE_PICKER
            && state().file_picker == nullptr
#endif
        )
            return SAO_STATUS_OK;
        if (state().panel == nullptr || state().body == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        state().retiring = true;
        state().accepting = false;
        panel = state().panel;
        action_attached = state().action_attached;
        event_attached = state().event_attached;
#if defined(SAO_SETTINGS_HAS_FILE_PICKER) && SAO_SETTINGS_HAS_FILE_PICKER
        file_picker = state().file_picker;
        picker_only_teardown = panel == nullptr && file_picker != nullptr;
#endif
    }
#if defined(SAO_SETTINGS_HAS_FILE_PICKER) && SAO_SETTINGS_HAS_FILE_PICKER
    if (picker_only_teardown) {
        // Picker survived a partial teardown — destroy it without a panel.
        // Fires the stored callback (cancelled) so it must run unlocked.
        (void)sao_ui_file_picker_try_destroy(file_picker);
        std::lock_guard lock(state().mutex);
        state().file_picker = nullptr;
        state().retiring = false;
        state().accepting = true;
        return SAO_STATUS_OK;
    }
#endif
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
#if defined(SAO_SETTINGS_HAS_FILE_PICKER) && SAO_SETTINGS_HAS_FILE_PICKER
    // The picker is owner-thread too.  Destroy it before unregistering the
    // panel so its cancel callback cannot publish into a released body.
    if (status == SAO_STATUS_OK && file_picker != nullptr)
        (void)sao_ui_file_picker_try_destroy(file_picker);
#endif
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
#if defined(SAO_SETTINGS_HAS_FILE_PICKER) && SAO_SETTINGS_HAS_FILE_PICKER
        state().file_picker = nullptr;
#endif
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
