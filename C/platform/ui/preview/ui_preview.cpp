#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>

#include "sao/ui/compositor.h"
#include "sao/ui/input_router.h"
#include "sao/ui/linkstart_intro.h"
#include "sao/ui/panel.h"
#include "sao/ui/sound.h"
#include "sao/ui/theme.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;
constexpr UINT kControls = 101;
constexpr UINT kSettings = 102;
constexpr UINT kConversation = 103;
constexpr UINT kLight = 201;
constexpr UINT kDark = 202;
constexpr UINT kIntro = 301;
constexpr UINT_PTR kAnimationTimer = 1;

void require(sao_status_t status) {
    if (status != SAO_STATUS_OK)
        throw std::runtime_error("UI status " + std::to_string(status));
}
Json text(std::string value, const char* style = "value", int height = 28) {
    return {{"type", "text"}, {"text", std::move(value)}, {"style", style}, {"height", height}};
}
Json button(const std::string& id, const char* label, const char* style, bool disabled = false) {
    return {{"type", "button"}, {"id", id}, {"label", label}, {"action", id},
            {"style", style}, {"height", 36}, {"disabled", disabled}};
}
Json card(std::string title, Json children) {
    return {{"type", "card"}, {"title", std::move(title)}, {"accent", "cyan"},
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
    bool dirty{true};
    std::string feedback{"预览中的操作只更新本地示例，不调用产品后端。"};
    std::array<char, 256> error{};

    void set_error(const char* message) noexcept {
        std::snprintf(error.data(), error.size(), "%s", message ? message : "Preview error");
    }

    ~Preview() {
        if (intro) sao_ui_linkstart_destroy(intro);
        if (keyboard) sao_ui_input_router_deep_destroy(keyboard);
        if (footer) sao_ui_panel_destroy(footer);
        if (navigation) sao_ui_panel_destroy(navigation);
        if (header) sao_ui_panel_destroy(header);
        if (panel) sao_ui_panel_destroy(panel);
        if (compositor) sao_ui_compositor_destroy(compositor);
        (void)sao_ui_sound_shutdown();
    }
    void invalidate() {
        dirty = true;
        InvalidateRect(window, nullptr, FALSE);
    }
    void set_content(sao_ui_panel_handle_t target, Json nodes) {
        const std::string spec = Json{{"version", 1}, {"title", ""}, {"nodes", std::move(nodes)}}.dump();
        require(sao_ui_panel_set_spec(target, reinterpret_cast<const uint8_t*>(spec.data()), spec.size()));
    }
    void publish() {
        set_content(header, Json::array({
            text("SAO Native UI Preview / 固定头区", "title", 30),
            text(feedback, "accent", 24),
            text("本地示例 · 真实虚拟面板 · CPU 合成 · 非产品页面", "muted", 22)}));
        Json nav = Json::array({text("独立导航区域", "title", 28),
            button("preview.scene.controls", "控件", scene == kControls ? "primary" : "ghost"),
            button("preview.scene.settings", "设置", scene == kSettings ? "primary" : "ghost"),
            button("preview.scene.conversation", "对话", scene == kConversation ? "primary" : "ghost")});
        for (int index = 0; index < 16; ++index)
            nav.push_back(button("preview.category." + std::to_string(index),
                ("示例分类 " + std::to_string(index + 1)).c_str(), "ghost"));
        set_content(navigation, std::move(nav));
        Json nodes = Json::array();
        if (scene == kControls) {
            nodes.push_back(card("按钮与状态", Json::array({
                Json{{"type", "row"}, {"children", Json::array({
                    button("preview.primary", "主要操作", "primary"),
                    button("preview.secondary", "次要操作", "default"),
                    button("preview.disabled", "不可用", "ghost", true)})}},
                Json{{"type", "row"}, {"children", Json::array({
                    button("preview.toggle", selected ? "已选中" : "未选中", selected ? "primary" : "ghost"),
                    Json{{"type", "badge"}, {"text", "就绪"}, {"style", "ok"}, {"height", 24}},
                    Json{{"type", "badge"}, {"text", "错误示例"}, {"style", "bad"}, {"height", 24}}})}},
                Json{{"type", "bar"}, {"pct", 62}, {"caption", "进度示例 62%"}, {"height", 28}},
                text("标题、长文本与系统字体回退 / Typography", "title", 32),
                text("中文、English、1234567890；长路径与文本在真实窗口中观察折行和省略。", "value", 54)})));
        } else if (scene == kSettings) {
            Json fields = Json::array();
            for (int index = 0; index < 18; ++index)
                fields.push_back(card("设置组 " + std::to_string(index + 1), Json::array({
                    text("示例说明：用于观察长表单的布局、裁剪、滚动和焦点。", "muted", 36),
                    button("preview.field." + std::to_string(index), "示例操作", "default")})));
            nodes.push_back(card("长表单 · 正文独立滚动", std::move(fields)));
        } else {
            Json messages = Json::array();
            for (int index = 0; index < 20; ++index)
                messages.push_back(card(index % 2 == 0 ? "你" : "Assistant", Json::array({
                    text("本地对话示例 " + std::to_string(index + 1), "value", 32),
                    text("这段内容用于观察长对话与输入区的位置，不会调用任何模型或工具。", "muted", 48)})));
            nodes.push_back(card("长对话 · 正文独立滚动", std::move(messages)));
        }
        set_content(panel, std::move(nodes));
        if (scene == kConversation) {
            set_content(footer, Json::array({
                text("固定输入区 · 当前 input 仍为展示/激活路径", "muted", 24),
                Json{{"type", "input"}, {"id", "preview.input"}, {"value", "当前通用 input 的显示与激活路径"},
                     {"action", "preview.edit"}, {"height", 64}},
                button("preview.send", "本地操作示例", "primary")}));
        } else {
            set_content(footer, Json::array({text("固定操作区 / 正文滚动不移动此区域", "title", 28),
                Json{{"type", "row"}, {"children", Json::array({
                    button("preview.apply", "应用示例", "primary"),
                    button("preview.cancel", "撤销示例", "ghost")})}},
                text("所有操作仅影响预览示例，不保存产品配置。", "muted", 24)}));
        }
        invalidate();
    }
    static void SAO_UI_CALL action(const char* name, const uint8_t*, size_t, void* context) {
        auto& self = *static_cast<Preview*>(context);
        try {
            if (name && std::strcmp(name, "preview.toggle") == 0) self.selected = !self.selected;
            if (name && std::strcmp(name, "preview.scene.controls") == 0) self.scene = kControls;
            if (name && std::strcmp(name, "preview.scene.settings") == 0) self.scene = kSettings;
            if (name && std::strcmp(name, "preview.scene.conversation") == 0) self.scene = kConversation;
            self.feedback = "本地事件：" + std::string(name ? name : "");
            self.publish();
        } catch (const std::exception& ex) { self.set_error(ex.what()); self.invalidate(); }
        catch (...) { self.set_error("Preview action failed"); self.invalidate(); }
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
        if (intro) { sao_ui_linkstart_destroy(intro); intro = nullptr; }
        invalidate();
    }
    void resize() {
        if (!panel) return;
        if (intro) end_intro();
        if (IsIconic(window)) return;
        RECT bounds{};
        GetClientRect(window, &bounds);
        if (bounds.right <= 0 || bounds.bottom <= 0) return;
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
        invalidate();
    }
    void command(UINT id) {
        if (intro) end_intro();
        if (id == kLight || id == kDark) {
            require(sao_ui_theme_set_active_id(id == kLight ? SAO_UI_THEME_LIGHT : SAO_UI_THEME_DARK));
            invalidate();
        } else if (id == kIntro) {
            RECT bounds{};
            GetClientRect(window, &bounds);
            SaoUiLinkStartConfig config{};
            config.struct_size = sizeof(config);
            config.width_px = static_cast<uint32_t>(std::max(1L, bounds.right));
            config.height_px = static_cast<uint32_t>(std::max(1L, bounds.bottom));
            require(sao_ui_linkstart_create(compositor, nullptr, &config, &intro));
            require(sao_ui_linkstart_show(intro));
            last_tick = GetTickCount64();
            if (!SetTimer(window, kAnimationTimer, 16, nullptr)) {
                end_intro();
                throw std::runtime_error("Preview timer creation failed");
            }
            invalidate();
        } else if (id >= kControls && id <= kConversation) {
            scene = id;
            publish();
        }
    }
    void tick() {
        if (!intro) return;
        if (IsIconic(window)) { end_intro(); return; }
        const ULONGLONG now = GetTickCount64();
        const int32_t elapsed = static_cast<int32_t>(std::min<ULONGLONG>(now - last_tick, 250));
        last_tick = now;
        require(sao_ui_linkstart_tick(intro, elapsed));
        bool active = false;
        require(sao_ui_linkstart_is_active(intro, &active));
        if (!active) end_intro();
        invalidate();
    }
    void paint(HDC target = nullptr) noexcept {
        PAINTSTRUCT paint_state{};
        HDC dc = target ? target : BeginPaint(window, &paint_state);
        try {
            if (dirty && compositor) {
                size_t bytes = 0;
                auto status = sao_ui_compositor_snapshot_bgra(compositor, pixels.data(), pixels.size(), &frame_width, &frame_height, &bytes);
                if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
                    if (bytes > 64U * 1024U * 1024U) throw std::runtime_error("Preview raster exceeds 64 MiB");
                    pixels.resize(bytes);
                    status = sao_ui_compositor_snapshot_bgra(compositor, pixels.data(), pixels.size(), &frame_width, &frame_height, &bytes);
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
        } catch (const std::exception& ex) { set_error(ex.what()); }
        catch (...) { set_error("Preview paint failed"); }
        if (error[0] != '\0') {
            SetTextColor(dc, RGB(190, 30, 30));
            SetBkColor(dc, RGB(255, 255, 255));
            TextOutA(dc, 12, 12, error.data(), static_cast<int>(std::strlen(error.data())));
        }
        if (!target) EndPaint(window, &paint_state);
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* preview = reinterpret_cast<Preview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        preview = static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        preview->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(preview));
    }
    if (!preview) return DefWindowProcW(window, message, wparam, lparam);
    try {
        switch (message) {
        case WM_COMMAND: preview->command(LOWORD(wparam)); return 0;
        case WM_SIZE: preview->resize(); return 0;
        case WM_PAINT: preview->paint(); return 0;
        case WM_PRINTCLIENT: preview->paint(reinterpret_cast<HDC>(wparam)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: if (wparam == kAnimationTimer) preview->tick(); return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = {420, 480};
            limits->ptMaxTrackSize = {3840, 2160};
            return 0;
        }
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR: {
            if (message == WM_KEYDOWN && wparam == VK_ESCAPE && preview->intro) { preview->end_intro(); return 0; }
            bool consumed = false;
            if (preview->keyboard && !preview->intro) {
                const auto status = sao_ui_input_router_feed_raw_win32(preview->keyboard, message, wparam, lparam, &consumed);
                if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_NOT_FOUND) require(status);
                preview->invalidate();
            }
            if (consumed) return 0;
            break;
        }
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_MOUSEWHEEL: {
            if (!preview->compositor || preview->intro) break;
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (message == WM_MOUSEWHEEL) ScreenToClient(window, &point);
            if (message == WM_MOUSEMOVE) {
                TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
                TrackMouseEvent(&tracking);
            }
            if (message == WM_LBUTTONDOWN) { SetFocus(window); SetCapture(window); }
            if (message == WM_LBUTTONUP && GetCapture() == window) ReleaseCapture();
            const auto status = sao_ui_compositor_dispatch_mouse(preview->compositor, message, point.x, point.y, 0,
                message == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(wparam) : 0);
            if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_NOT_FOUND) require(status);
            preview->invalidate();
            return 0;
        }
        case WM_DESTROY: KillTimer(window, kAnimationTimer); PostQuitMessage(0); return 0;
        case WM_NCDESTROY: SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
        default: break;
        }
    } catch (const std::exception& ex) { preview->set_error(ex.what()); preview->invalidate(); }
    catch (...) { preview->set_error("Preview operation failed"); preview->invalidate(); }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    int result = 1;
    try {
        Preview preview;
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = L"SaoNativeUiPreview";
        if (!RegisterClassW(&cls)) throw std::runtime_error("Preview class registration failed");
        HMENU menu = CreateMenu();
        if (!menu) throw std::runtime_error("Preview menu creation failed");
        if (!(AppendMenuW(menu, MF_STRING, kControls, L"控件") &&
              AppendMenuW(menu, MF_STRING, kSettings, L"长表单") &&
              AppendMenuW(menu, MF_STRING, kConversation, L"长对话") &&
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
        if (!window) { DestroyMenu(menu); throw std::runtime_error("Preview window creation failed"); }
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
            if (IsWindow(window)) DestroyWindow(window);
        } catch (...) { DestroyWindow(window); throw; }
    } catch (const std::exception& ex) {
        MessageBoxA(nullptr, ex.what(), "SAO UI preview", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, L"预览初始化失败", L"SAO UI preview", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return result;
}
