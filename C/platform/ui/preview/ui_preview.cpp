#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <objbase.h>

#include "sao/ui/compositor.h"
#include "sao/ui/input_router.h"
#include "sao/ui/linkstart_intro.h"
#include "sao/ui/panel.h"
#include "sao/ui/sound.h"
#include "sao/ui/theme.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;
constexpr UINT kControls = 101;
constexpr UINT kSettings = 102;
constexpr UINT kConversation = 103;
constexpr UINT kAudio = 104;
constexpr UINT kLight = 201;
constexpr UINT kDark = 202;
constexpr UINT kIntro = 301;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr uint64_t kMaximumWaveBytes = 16ULL * 1024ULL * 1024ULL;

struct AudioCue {
    SaoUiSoundCue cue;
    const char* label;
};

constexpr std::array<AudioCue, SAO_UI_SOUND_COUNT> kAudioCues{{
    {SAO_UI_SOUND_CLICK, "00 Click"},
    {SAO_UI_SOUND_MENU_OPEN, "01 Menu open"},
    {SAO_UI_SOUND_MENU_CLOSE, "02 Menu close"},
    {SAO_UI_SOUND_PANEL, "03 Panel"},
    {SAO_UI_SOUND_SUBMENU, "04 Submenu"},
    {SAO_UI_SOUND_ALERT, "05 Alert"},
    {SAO_UI_SOUND_ALERT_CLOSE, "06 Alert close"},
    {SAO_UI_SOUND_WELCOME, "07 Welcome"},
    {SAO_UI_SOUND_ALO_WELCOME, "08 ALO welcome"},
    {SAO_UI_SOUND_LINK_START, "09 Link Start"},
    {SAO_UI_SOUND_NERVEGEAR, "10 NerveGear"},
    {SAO_UI_SOUND_MESSAGE, "11 Message"},
    {SAO_UI_SOUND_SYSTEM, "12 System"},
    {SAO_UI_SOUND_WARNING, "13 Warning"},
    {SAO_UI_SOUND_EMERGENCY, "14 Emergency"},
}};

static_assert(sizeof(wchar_t) == sizeof(uint16_t));

void require(sao_status_t status) {
    if (status != SAO_STATUS_OK)
        throw std::runtime_error("UI status " + std::to_string(status));
}
Json text(std::string value, const char* style = "value", int height = 28) {
    return {{"type", "text"}, {"text", std::move(value)}, {"style", style}, {"height", height}};
}
Json button(const std::string& id, const char* label, const char* style, bool disabled = false) {
    return {{"type", "button"}, {"id", id},     {"label", label},      {"action", id},
            {"style", style},   {"height", 36}, {"disabled", disabled}};
}
Json card(std::string title, Json children) {
    return {{"type", "card"},
            {"title", std::move(title)},
            {"accent", "cyan"},
            {"children", std::move(children)}};
}

struct Preview {
    HWND window{};
    sao_ui_compositor_handle_t compositor{};
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_handle_t header{};
    sao_ui_panel_handle_t navigation{};
    sao_ui_panel_handle_t footer{};
    sao_ui_input_router_deep_handle_t keyboard{};
    sao_ui_linkstart_handle_t intro{};
    std::vector<uint8_t> pixels;
    uint32_t frame_width{};
    uint32_t frame_height{};
    UINT scene{kControls};
    ULONGLONG last_tick{};
    bool selected{};
    sao_ui_sound_group_t sound_group{};
    bool sound_enabled{};
    int32_t sound_volume{70};
    bool sound_restart_pending{true};
    bool suppress_implicit_sound{};
    bool dirty{true};
    std::string feedback{"预览中的操作只更新本地示例，不调用产品后端。"};
    std::array<char, 256> error{};

    void set_error(const char* message) noexcept {
        std::snprintf(error.data(), error.size(), "%s", message ? message : "Preview error");
    }

    void clear_error() noexcept {
        error[0] = '\0';
    }

    static std::string status_text(sao_status_t status) {
        return std::string(sao_status_str(status)) + " (" + std::to_string(status) + ")";
    }

    sao_status_t refresh_sound_state() noexcept {
        bool enabled = false;
        sao_status_t status = sao_ui_sound_get_enabled(&enabled);
        if (status != SAO_STATUS_OK)
            return status;
        int32_t volume = 0;
        status = sao_ui_sound_get_volume(&volume);
        if (status != SAO_STATUS_OK)
            return status;
        sound_enabled = enabled;
        sound_volume = volume;
        return SAO_STATUS_OK;
    }

    void set_sound_result(std::string_view operation, sao_status_t status, std::string success) {
        if (status == SAO_STATUS_OK)
            feedback = std::move(success);
        else
            feedback = std::string(operation) + "：" + status_text(status);
    }

    sao_status_t ensure_sound_group() noexcept {
        if (sound_group != 0)
            return SAO_STATUS_OK;
        sao_ui_sound_group_t created = 0;
        const sao_status_t status = sao_ui_sound_group_create(&created);
        if (status == SAO_STATUS_OK)
            sound_group = created;
        return status;
    }

    void toggle_sound_enabled() {
        const bool requested = !sound_enabled;
        sao_status_t status = sao_ui_sound_set_enabled(requested);
        const sao_status_t refresh_status = refresh_sound_state();
        if (status == SAO_STATUS_OK)
            status = refresh_status;
        if (!sound_enabled || requested)
            sound_restart_pending = true;
        set_sound_result("Audio enabled 更新失败", status,
                         sound_enabled ? "Audio enabled=true；下一次 cue/WAV 将按需启动音频引擎。"
                                       : "Audio enabled=false；Preview 保持静音。面板仍可操作。");
    }

    void set_sound_volume_preset(int32_t volume) {
        sao_status_t status = sao_ui_sound_set_volume(volume);
        const sao_status_t refresh_status = refresh_sound_state();
        if (status == SAO_STATUS_OK)
            status = refresh_status;
        if (sound_volume == 0)
            sound_restart_pending = true;
        set_sound_result("Audio volume 更新失败", status,
                         "Audio volume=" + std::to_string(sound_volume) +
                             "%；enabled=" + (sound_enabled ? "true" : "false") + "。");
    }

    void stop_sound_group() {
        const sao_status_t status = sound_group == 0 ? SAO_STATUS_ERR_NOT_INITIALIZED
                                                     : sao_ui_sound_group_stop(sound_group);
        set_sound_result("Sound group stop 失败", status,
                         "已停止 Preview sound group " + std::to_string(sound_group) + "。");
    }

    void recreate_sound_group() {
        const sao_ui_sound_group_t previous = sound_group;
        sao_status_t status = SAO_STATUS_OK;
        if (previous != 0)
            status = sao_ui_sound_group_destroy(previous);
        if (status == SAO_STATUS_OK) {
            sound_group = 0;
            status = ensure_sound_group();
        }
        set_sound_result("Sound group destroy/recreate 失败", status,
                         "Sound group 已从 " + std::to_string(previous) + " 重建为 " +
                             std::to_string(sound_group) + "。");
    }

    void shutdown_sound() {
        sao_status_t status = sao_ui_sound_shutdown();
        const sao_status_t refresh_status = refresh_sound_state();
        if (status == SAO_STATUS_OK)
            status = refresh_status;
        if (status == SAO_STATUS_OK)
            sound_restart_pending = true;
        set_sound_result(
            "Sound shutdown 失败", status,
            "音频引擎已 shutdown；enabled=" + std::string(sound_enabled ? "true" : "false") +
                "，下一次有效播放将 lazy restart。");
    }

    void reenable_sound_for_lazy_restart() {
        const sao_status_t shutdown_status = sao_ui_sound_shutdown();
        const sao_status_t enable_status = sao_ui_sound_set_enabled(true);
        sao_status_t status = shutdown_status != SAO_STATUS_OK ? shutdown_status : enable_status;
        const sao_status_t refresh_status = refresh_sound_state();
        if (status == SAO_STATUS_OK)
            status = refresh_status;
        if (shutdown_status == SAO_STATUS_OK)
            sound_restart_pending = true;
        set_sound_result("Sound re-enable 失败", status,
                         "音频引擎已 shutdown 并 re-enable；下一次 cue/WAV 将 lazy restart。");
    }

    void play_audio_cue(size_t index) {
        if (index >= kAudioCues.size()) {
            feedback = "Cue 索引越界。";
            return;
        }
        sao_status_t status = ensure_sound_group();
        const bool was_pending = sound_restart_pending;
        if (status == SAO_STATUS_OK)
            status = sao_ui_sound_play_in_group(kAudioCues[index].cue, 100, sound_group);
        if (status != SAO_STATUS_OK) {
            set_sound_result("Cue 播放失败", status, {});
            return;
        }
        if (!sound_enabled) {
            feedback = std::string(kAudioCues[index].label) + " 已请求；enabled=false，保持静音。";
        } else if (sound_volume == 0) {
            feedback = std::string(kAudioCues[index].label) + " 已请求；volume=0%，保持静音。";
        } else {
            sound_restart_pending = false;
            feedback = std::string(kAudioCues[index].label) + " 已在 group " +
                       std::to_string(sound_group) +
                       " 播放，volume=" + std::to_string(sound_volume) + "%" +
                       (was_pending ? "；lazy restart 完成。" : "。");
        }
    }

    void choose_and_play_wave() {
        std::array<wchar_t, 32768> path{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window;
        dialog.lpstrFilter = L"PCM RIFF/WAVE (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.lpstrDefExt = L"wav";
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                       OFN_DONTADDTORECENT;
        if (!GetOpenFileNameW(&dialog)) {
            const DWORD dialog_error = CommDlgExtendedError();
            feedback = dialog_error == 0 ? "WAV 选择已取消；Audio 状态保持不变。"
                                         : "WAV open-file dialog 失败，extended error=" +
                                               std::to_string(dialog_error) + "。";
            return;
        }

        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(path.data(), GetFileExInfoStandard, &attributes)) {
            feedback = "WAV 文件状态读取失败，Win32 error=" + std::to_string(GetLastError()) + "。";
            return;
        }
        if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            feedback = "WAV 格式错误：选择结果不是文件。";
            return;
        }
        ULARGE_INTEGER file_size{};
        file_size.HighPart = attributes.nFileSizeHigh;
        file_size.LowPart = attributes.nFileSizeLow;
        if (file_size.QuadPart == 0) {
            feedback = "WAV 格式错误：文件为空。";
            return;
        }
        if (file_size.QuadPart > kMaximumWaveBytes) {
            feedback =
                "WAV 超限：" + std::to_string(file_size.QuadPart) + " bytes，最大允许 16 MiB。";
            return;
        }

        sao_status_t status = ensure_sound_group();
        const bool was_pending = sound_restart_pending;
        if (status == SAO_STATUS_OK) {
            status = sao_ui_sound_play_wav_utf16(reinterpret_cast<const uint16_t*>(path.data()),
                                                 100, sound_group);
        }
        if (status == SAO_STATUS_ERR_INVALID_ARGUMENT) {
            feedback = "WAV 格式错误：仅接受 bounded 16-bit PCM RIFF/WAVE mono/stereo；status=" +
                       status_text(status) + "。";
        } else if (status != SAO_STATUS_OK) {
            set_sound_result("WAV 播放失败", status, {});
        } else if (!sound_enabled) {
            feedback = "WAV 已通过边界与格式检查；enabled=false，保持静音。";
        } else if (sound_volume == 0) {
            feedback = "WAV 已通过边界与格式检查；volume=0%，保持静音。";
        } else {
            sound_restart_pending = false;
            feedback = "WAV 已在 Preview group " + std::to_string(sound_group) +
                       " 播放，volume=" + std::to_string(sound_volume) + "%" +
                       (was_pending ? "；lazy restart 完成。" : "。");
        }
    }

    bool handle_audio_action(std::string_view name) {
        if (name == "preview.audio.enabled") {
            toggle_sound_enabled();
            return true;
        }
        if (name == "preview.audio.group.stop") {
            stop_sound_group();
            return true;
        }
        if (name == "preview.audio.group.recreate") {
            recreate_sound_group();
            return true;
        }
        if (name == "preview.audio.shutdown") {
            shutdown_sound();
            return true;
        }
        if (name == "preview.audio.reenable") {
            reenable_sound_for_lazy_restart();
            return true;
        }
        if (name == "preview.audio.open_wav") {
            choose_and_play_wave();
            return true;
        }
        constexpr std::array<int32_t, 5> volumes{0, 25, 50, 75, 100};
        for (const int32_t volume : volumes) {
            if (name == "preview.audio.volume." + std::to_string(volume)) {
                set_sound_volume_preset(volume);
                return true;
            }
        }
        for (size_t index = 0; index < kAudioCues.size(); ++index) {
            if (name == "preview.audio.cue." + std::to_string(index)) {
                play_audio_cue(index);
                return true;
            }
        }
        return false;
    }

    ~Preview() {
        if (intro)
            sao_ui_linkstart_destroy(intro);
        if (sound_group) {
            (void)sao_ui_sound_group_destroy(sound_group);
            sound_group = 0;
        }
        if (keyboard)
            sao_ui_input_router_deep_destroy(keyboard);
        if (footer)
            sao_ui_panel_destroy(footer);
        if (navigation)
            sao_ui_panel_destroy(navigation);
        if (header)
            sao_ui_panel_destroy(header);
        if (panel)
            sao_ui_panel_destroy(panel);
        if (compositor)
            sao_ui_compositor_destroy(compositor);
        (void)sao_ui_sound_shutdown();
    }
    void invalidate() {
        dirty = true;
        InvalidateRect(window, nullptr, FALSE);
    }
    void set_content(sao_ui_panel_handle_t target, Json nodes) {
        const std::string spec =
            Json{{"version", 1}, {"title", ""}, {"nodes", std::move(nodes)}}.dump();
        require(sao_ui_panel_set_spec(target, reinterpret_cast<const uint8_t*>(spec.data()),
                                      spec.size()));
    }
    void publish() {
        set_content(header, Json::array({text("SAO Native UI Preview / 固定头区", "title", 30),
                                         text(feedback, "accent", 24),
                                         text("本地示例 · 真实虚拟面板 · CPU 合成 · 非产品页面",
                                              "muted", 22)}));
        Json nav = Json::array(
            {text("独立导航区域", "title", 28),
             button("preview.scene.controls", "控件", scene == kControls ? "primary" : "ghost"),
             button("preview.scene.settings", "设置", scene == kSettings ? "primary" : "ghost"),
             button("preview.scene.conversation", "对话",
                    scene == kConversation ? "primary" : "ghost"),
             button("preview.scene.audio", "Audio", scene == kAudio ? "primary" : "ghost")});
        for (int index = 0; index < 16; ++index)
            nav.push_back(button("preview.category." + std::to_string(index),
                                 ("示例分类 " + std::to_string(index + 1)).c_str(), "ghost"));
        set_content(navigation, std::move(nav));
        Json nodes = Json::array();
        if (scene == kControls) {
            nodes.push_back(card(
                "按钮与状态",
                Json::array(
                    {Json{{"type", "row"},
                          {"children",
                           Json::array({button("preview.primary", "主要操作", "primary"),
                                        button("preview.secondary", "次要操作", "default"),
                                        button("preview.disabled", "不可用", "ghost", true)})}},
                     Json{{"type", "row"},
                          {"children",
                           Json::array({button("preview.toggle", selected ? "已选中" : "未选中",
                                               selected ? "primary" : "ghost"),
                                        Json{{"type", "badge"},
                                             {"text", "就绪"},
                                             {"style", "ok"},
                                             {"height", 24}},
                                        Json{{"type", "badge"},
                                             {"text", "错误示例"},
                                             {"style", "bad"},
                                             {"height", 24}}})}},
                     Json{
                         {"type", "bar"}, {"pct", 62}, {"caption", "进度示例 62%"}, {"height", 28}},
                     text("标题、长文本与系统字体回退 / Typography", "title", 32),
                     text("中文、English、1234567890；长路径与文本在真实窗口中观察折行和省略。",
                          "value", 54)})));
        } else if (scene == kSettings) {
            Json fields = Json::array();
            for (int index = 0; index < 18; ++index)
                fields.push_back(card(
                    "设置组 " + std::to_string(index + 1),
                    Json::array(
                        {text("示例说明：用于观察长表单的布局、裁剪、滚动和焦点。", "muted", 36),
                         button("preview.field." + std::to_string(index), "示例操作",
                                "default")})));
            nodes.push_back(card("长表单 · 正文独立滚动", std::move(fields)));
        } else if (scene == kConversation) {
            Json messages = Json::array();
            for (int index = 0; index < 20; ++index)
                messages.push_back(
                    card(index % 2 == 0 ? "你" : "Assistant",
                         Json::array(
                             {text("本地对话示例 " + std::to_string(index + 1), "value", 32),
                              text("这段内容用于观察长对话与输入区的位置，不会调用任何模型或工具。",
                                   "muted", 48)})));
            nodes.push_back(card("长对话 · 正文独立滚动", std::move(messages)));
        } else {
            Json audio =
                Json::array({text("启动默认静音；所有显式 cue 与自选 WAV 使用 Preview 独立 group。",
                                  "muted", 38),
                             text("enabled=" + std::string(sound_enabled ? "true" : "false") +
                                      " · volume=" + std::to_string(sound_volume) +
                                      "% · group=" + std::to_string(sound_group) + " · engine=" +
                                      (sound_restart_pending ? "lazy restart pending" : "active"),
                                  "accent", 38),
                             Json{{"type", "checkbox"},
                                  {"id", "preview.audio.enabled"},
                                  {"label", sound_enabled ? "Enabled: ON" : "Enabled: OFF"},
                                  {"action", "preview.audio.enabled"},
                                  {"checked", sound_enabled},
                                  {"height", 36}}});
            Json volume_row = Json::array();
            for (const int32_t volume : std::array<int32_t, 5>{0, 25, 50, 75, 100}) {
                const std::string id = "preview.audio.volume." + std::to_string(volume);
                volume_row.push_back(button(id, (std::to_string(volume) + "%").c_str(),
                                            sound_volume == volume ? "primary" : "ghost"));
            }
            audio.push_back(Json{{"type", "row"}, {"children", std::move(volume_row)}});
            audio.push_back(Json{
                {"type", "row"},
                {"children",
                 Json::array(
                     {button("preview.audio.group.stop", "Stop group", "default"),
                      button("preview.audio.group.recreate", "Destroy / recreate", "default")})}});
            audio.push_back(
                Json{{"type", "row"},
                     {"children",
                      Json::array(
                          {button("preview.audio.shutdown", "Sound shutdown", "ghost"),
                           button("preview.audio.reenable", "Shutdown + re-enable", "primary")})}});
            audio.push_back(
                button("preview.audio.open_wav", "选择 bounded PCM RIFF/WAVE...", "default"));
            audio.push_back(
                text("15 cue grid · requested volume 100 · effective=min(requested, preset)",
                     "title", 30));
            for (size_t first = 0; first < kAudioCues.size(); first += 3) {
                Json cue_row = Json::array();
                for (size_t index = first; index < std::min(first + 3, kAudioCues.size());
                     ++index) {
                    cue_row.push_back(button("preview.audio.cue." + std::to_string(index),
                                             kAudioCues[index].label, "default"));
                }
                audio.push_back(Json{{"type", "row"}, {"children", std::move(cue_row)}});
            }
            nodes.push_back(card("Audio scene / native sound API", std::move(audio)));
        }
        set_content(panel, std::move(nodes));
        if (scene == kConversation) {
            set_content(footer,
                        Json::array({text("固定输入区 · 当前 input 仍为展示/激活路径", "muted", 24),
                                     Json{{"type", "input"},
                                          {"id", "preview.input"},
                                          {"value", "当前通用 input 的显示与激活路径"},
                                          {"action", "preview.edit"},
                                          {"height", 64}},
                                     button("preview.send", "本地操作示例", "primary")}));
        } else if (scene == kAudio) {
            set_content(
                footer,
                Json::array({text("Audio 快捷操作 · WAV 上限 16 MiB", "title", 28),
                             Json{{"type", "row"},
                                  {"children", Json::array({button("preview.audio.open_wav",
                                                                   "选择 WAV", "primary"),
                                                            button("preview.audio.group.stop",
                                                                   "Stop group", "ghost")})}},
                             text("取消选择只更新 feedback，不生成错误。", "muted", 24)}));
        } else {
            set_content(
                footer,
                Json::array({text("固定操作区 / 正文滚动不移动此区域", "title", 28),
                             Json{{"type", "row"},
                                  {"children",
                                   Json::array({button("preview.apply", "应用示例", "primary"),
                                                button("preview.cancel", "撤销示例", "ghost")})}},
                             text("所有操作仅影响预览示例，不保存产品配置。", "muted", 24)}));
        }
        invalidate();
    }
    static void SAO_UI_CALL action(const char* name, const uint8_t*, size_t, void* context) {
        auto& self = *static_cast<Preview*>(context);
        try {
            self.clear_error();
            const std::string_view action_name = name ? name : "";
            self.suppress_implicit_sound = action_name.starts_with("preview.audio.");
            const bool handled = self.handle_audio_action(action_name);
            if (!handled) {
                if (action_name == "preview.toggle")
                    self.selected = !self.selected;
                if (action_name == "preview.scene.controls")
                    self.scene = kControls;
                if (action_name == "preview.scene.settings")
                    self.scene = kSettings;
                if (action_name == "preview.scene.conversation")
                    self.scene = kConversation;
                if (action_name == "preview.scene.audio")
                    self.scene = kAudio;
                self.feedback = "本地事件：" + std::string(action_name);
            }
            self.publish();
        } catch (const std::exception& ex) {
            self.feedback = "Preview action 异常：" + std::string(ex.what());
            self.set_error(ex.what());
            self.invalidate();
        } catch (...) {
            self.feedback = "Preview action 异常。";
            self.set_error("Preview action failed");
            self.invalidate();
        }
    }
    void create_panel(const char* id, sao_ui_panel_handle_t* result) {
        SaoPanelConfig config{};
        config.panel_id_utf8 = id;
        config.title_utf8 = "UI Preview";
        config.default_width = 1000;
        config.default_height = 680;
        config.min_width = 1;
        config.min_height = 1;
        config.rendering_mode = SAO_UI_PANEL_RENDER_NATIVE;
        require(sao_ui_panel_create(compositor, &config, result));
        require(sao_ui_panel_set_action_handler(*result, action, this));
        require(sao_ui_panel_set_visible(*result, true));
    }
    void initialize() {
        require(sao_ui_sound_set_enabled(false));
        require(refresh_sound_state());
        require(ensure_sound_group());
        sound_restart_pending = true;
        feedback = "Audio 默认静音；进入 Audio scene 后可显式启用。";
        require(sao_ui_theme_set_active_id(SAO_UI_THEME_LIGHT));
        // A null host avoids overlay-window and D3D initialization.
        require(sao_ui_compositor_create(nullptr, nullptr, &compositor));
        create_panel("ui-preview.header", &header);
        create_panel("ui-preview.navigation", &navigation);
        create_panel("ui-preview.body", &panel);
        create_panel("ui-preview.footer", &footer);
        require(sao_ui_input_router_deep_create(compositor, &keyboard));
        publish();
        resize();
    }
    void end_intro() {
        KillTimer(window, kAnimationTimer);
        if (intro) {
            (void)sao_ui_linkstart_dismiss(intro);
            sao_ui_linkstart_destroy(intro);
            intro = nullptr;
        }
        invalidate();
    }
    void resize() {
        if (!panel)
            return;
        if (IsIconic(window))
            return;
        RECT bounds{};
        GetClientRect(window, &bounds);
        if (bounds.right <= 0 || bounds.bottom <= 0)
            return;
        const int width = bounds.right;
        const int height = bounds.bottom;
        const int top = std::min(116, height / 3);
        const int bottom = std::min(164, height / 2);
        const int left = width >= 760 ? 188 : 0;
        const int body_height = std::max(1, height - top - bottom);
        require(sao_ui_panel_set_geometry(header, 0, 0, width, top));
        require(sao_ui_panel_set_visible(navigation, left != 0));
        if (left != 0)
            require(sao_ui_panel_set_geometry(navigation, 0, top, left, height - top));
        require(sao_ui_panel_set_geometry(panel, left, top, width - left, body_height));
        require(sao_ui_panel_set_geometry(footer, left, height - bottom, width - left, bottom));
        if (intro) {
            const sao_status_t status =
                sao_ui_linkstart_resize(intro, static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height), GetDpiForWindow(window));
            if (status != SAO_STATUS_OK) {
                end_intro();
                require(status);
            }
        }
        invalidate();
    }
    void command(UINT id) {
        if (intro)
            end_intro();
        clear_error();
        if (id == kLight || id == kDark) {
            require(
                sao_ui_theme_set_active_id(id == kLight ? SAO_UI_THEME_LIGHT : SAO_UI_THEME_DARK));
            feedback = id == kLight ? "本地事件：切换浅色主题。" : "本地事件：切换深色主题。";
            publish();
        } else if (id == kIntro) {
            feedback = "本地事件：启动 Link Start preview。";
            publish();
            RECT bounds{};
            GetClientRect(window, &bounds);
            SaoUiLinkStartConfig config{};
            config.struct_size = sizeof(config);
            config.width_px = static_cast<uint32_t>(std::max(1L, bounds.right));
            config.height_px = static_cast<uint32_t>(std::max(1L, bounds.bottom));
            require(sao_ui_linkstart_create(compositor, nullptr, &config, &intro));
            sao_status_t status = sao_ui_linkstart_resize(
                intro, config.width_px, config.height_px, GetDpiForWindow(window));
            if (status != SAO_STATUS_OK) {
                end_intro();
                require(status);
            }
            status = sao_ui_linkstart_show(intro);
            if (status != SAO_STATUS_OK) {
                end_intro();
                require(status);
            }
            last_tick = GetTickCount64();
            if (!SetTimer(window, kAnimationTimer, 16, nullptr)) {
                end_intro();
                throw std::runtime_error("Preview timer creation failed");
            }
            invalidate();
        } else if (id >= kControls && id <= kAudio) {
            scene = id;
            feedback =
                id == kAudio ? "本地事件：切换到 Audio scene。" : "本地事件：切换 Preview scene。";
            publish();
        }
    }
    void tick() {
        if (!intro)
            return;
        if (IsIconic(window)) {
            end_intro();
            return;
        }
        const ULONGLONG now = GetTickCount64();
        const int32_t elapsed = static_cast<int32_t>(std::min<ULONGLONG>(now - last_tick, 250));
        last_tick = now;
        const sao_status_t status = sao_ui_linkstart_tick(intro, elapsed);
        if (status != SAO_STATUS_OK) {
            end_intro();
            require(status);
        }
        bool active = false;
        require(sao_ui_linkstart_is_active(intro, &active));
        if (!active)
            end_intro();
        invalidate();
    }
    void paint(HDC target = nullptr) noexcept {
        PAINTSTRUCT paint_state{};
        HDC dc = target ? target : BeginPaint(window, &paint_state);
        try {
            if (dirty && compositor) {
                size_t bytes = 0;
                auto status = sao_ui_compositor_snapshot_bgra(
                    compositor, pixels.data(), pixels.size(), &frame_width, &frame_height, &bytes);
                if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
                    if (bytes > 64U * 1024U * 1024U)
                        throw std::runtime_error("Preview raster exceeds 64 MiB");
                    pixels.resize(bytes);
                    status =
                        sao_ui_compositor_snapshot_bgra(compositor, pixels.data(), pixels.size(),
                                                        &frame_width, &frame_height, &bytes);
                }
                require(status);
                dirty = false;
            }
            if (!pixels.empty() && frame_width && frame_height) {
                BITMAPINFO bitmap{};
                bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bitmap.bmiHeader.biWidth = static_cast<LONG>(frame_width);
                bitmap.bmiHeader.biHeight = -static_cast<LONG>(frame_height);
                bitmap.bmiHeader.biPlanes = 1;
                bitmap.bmiHeader.biBitCount = 32;
                bitmap.bmiHeader.biCompression = BI_RGB;
                SetDIBitsToDevice(dc, 0, 0, frame_width, frame_height, 0, 0, 0, frame_height,
                                  pixels.data(), &bitmap, DIB_RGB_COLORS);
            }
        } catch (const std::exception& ex) {
            set_error(ex.what());
        } catch (...) {
            set_error("Preview paint failed");
        }
        if (error[0] != '\0') {
            SetTextColor(dc, RGB(190, 30, 30));
            SetBkColor(dc, RGB(255, 255, 255));
            TextOutA(dc, 12, 12, error.data(), static_cast<int>(std::strlen(error.data())));
        }
        if (!target)
            EndPaint(window, &paint_state);
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* preview = reinterpret_cast<Preview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        preview = static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        preview->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(preview));
    }
    if (!preview)
        return DefWindowProcW(window, message, wparam, lparam);
    try {
        switch (message) {
        case WM_COMMAND:
            preview->command(LOWORD(wparam));
            return 0;
        case WM_SIZE:
            preview->resize();
            return 0;
        case WM_PAINT:
            preview->paint();
            return 0;
        case WM_PRINTCLIENT:
            preview->paint(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wparam == kAnimationTimer)
                preview->tick();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = {420, 480};
            limits->ptMaxTrackSize = {3840, 2160};
            return 0;
        }
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR: {
            if (message == WM_KEYDOWN && wparam == VK_ESCAPE && preview->intro) {
                preview->end_intro();
                return 0;
            }
            bool consumed = false;
            if (preview->keyboard && !preview->intro) {
                preview->suppress_implicit_sound = false;
                sao_ui_sound_event_scope_t sound_scope = 0;
                if (message == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_SPACE) &&
                    preview->sound_group != 0)
                    require(sao_ui_sound_event_begin(preview->sound_group, &sound_scope));
                const auto status = sao_ui_input_router_feed_raw_win32(preview->keyboard, message,
                                                                       wparam, lparam, &consumed);
                if (sound_scope != 0) {
                    const auto sound_status = preview->suppress_implicit_sound
                                                  ? sao_ui_sound_event_cancel(sound_scope)
                                                  : sao_ui_sound_event_commit(sound_scope);
                    require(sound_status);
                }
                if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_NOT_FOUND)
                    require(status);
                preview->invalidate();
            }
            if (consumed)
                return 0;
            break;
        }
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_MOUSEWHEEL: {
            if (!preview->compositor || preview->intro)
                break;
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (message == WM_MOUSEWHEEL)
                ScreenToClient(window, &point);
            if (message == WM_MOUSEMOVE) {
                TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
                TrackMouseEvent(&tracking);
            }
            if (message == WM_LBUTTONDOWN) {
                SetFocus(window);
                SetCapture(window);
            }
            if (message == WM_LBUTTONUP && GetCapture() == window)
                ReleaseCapture();
            preview->suppress_implicit_sound = false;
            sao_ui_sound_event_scope_t sound_scope = 0;
            if (message == WM_LBUTTONUP && preview->sound_group != 0)
                require(sao_ui_sound_event_begin(preview->sound_group, &sound_scope));
            const auto status = sao_ui_compositor_dispatch_mouse(
                preview->compositor, message, point.x, point.y, 0,
                message == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(wparam) : 0);
            if (sound_scope != 0) {
                const auto sound_status = preview->suppress_implicit_sound
                                              ? sao_ui_sound_event_cancel(sound_scope)
                                              : sao_ui_sound_event_commit(sound_scope);
                require(sound_status);
            }
            if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_NOT_FOUND)
                require(status);
            preview->invalidate();
            return 0;
        }
        case WM_DESTROY:
            KillTimer(window, kAnimationTimer);
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
    } catch (const std::exception& ex) {
        preview->set_error(ex.what());
        preview->invalidate();
    } catch (...) {
        preview->set_error("Preview operation failed");
        preview->invalidate();
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com))
        return 1;
    int result = 1;
    try {
        Preview preview;
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = L"SaoNativeUiPreview";
        if (!RegisterClassW(&cls))
            throw std::runtime_error("Preview class registration failed");
        HMENU menu = CreateMenu();
        if (!menu)
            throw std::runtime_error("Preview menu creation failed");
        if (!(AppendMenuW(menu, MF_STRING, kControls, L"控件") &&
              AppendMenuW(menu, MF_STRING, kSettings, L"长表单") &&
              AppendMenuW(menu, MF_STRING, kConversation, L"长对话") &&
              AppendMenuW(menu, MF_STRING, kAudio, L"Audio") &&
              AppendMenuW(menu, MF_STRING, kLight, L"浅色") &&
              AppendMenuW(menu, MF_STRING, kDark, L"深色") &&
              AppendMenuW(menu, MF_STRING, kIntro, L"Link Start"))) {
            DestroyMenu(menu);
            throw std::runtime_error("Preview menu population failed");
        }
        HWND window = CreateWindowExW(0, cls.lpszClassName,
                                      L"SAO 原生 UI 预览 — CPU 合成 / 本地数据 / 无产品后端",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1040, 780,
                                      nullptr, menu, instance, &preview);
        if (!window) {
            DestroyMenu(menu);
            throw std::runtime_error("Preview window creation failed");
        }
        try {
            preview.initialize();
            ShowWindow(window, show);
            MSG message{};
            BOOL received;
            while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            result = received == 0 ? 0 : 1;
            if (IsWindow(window))
                DestroyWindow(window);
        } catch (...) {
            DestroyWindow(window);
            throw;
        }
    } catch (const std::exception& ex) {
        MessageBoxA(nullptr, ex.what(), "SAO UI preview", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, L"预览初始化失败", L"SAO UI preview", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return result;
}
