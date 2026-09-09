// SAO Auto — launcher/user_menu.cpp

#include "sao/launcher/user_menu.h"
#include "sao/launcher/app.h"
#include "sao/launcher/args.h"
#include "sao/launcher/single_instance.h"
#include "sao/launcher/user_guide_webview.h"
#include "sao/launcher/working_dir.h"

#include "hotkey_config_panel.h"
#include "settings_config_panel.h"

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#include "sao/ui/dialog.h"
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cwchar>
#include <iterator>
#include <string>
#include <cstdio>
#include <windowsx.h>

namespace sao::launcher {
namespace settings { sao_status_t open_config_panel_status() noexcept; }
namespace hotkey { sao_status_t open_config_panel_status(Owner* owner) noexcept; }
namespace {

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
// Same lookup as tool_launch_internal.cpp's borrow_platform_compositor():
// the compositor is already bound into the SDK platform binding by the
// time UserMenu can show an error (bringUpUi() calls sao_ui_bring_online()
// before user_menu_.create()), so no owning reference needs to be threaded
// through UserMenu's constructor.
sao_ui_compositor_handle_t borrow_platform_compositor() noexcept {
    void* raw = nullptr;
    if (sao_sdk_platform_get_ui_compositor(&raw) != SAO_SDK_OK) {
        return nullptr;
    }
    return static_cast<sao_ui_compositor_handle_t>(raw);
}
#endif

sao_status_t g_settings_menu_status = SAO_STATUS_OK;
sao_status_t g_hotkey_menu_status = SAO_STATUS_OK;
const char* menu_status_text(sao_status_t status) noexcept {
#if defined(SAO_LAUNCHER_UI_OFF_LINK_SMOKE)
    (void)status;
    return "unavailable";
#else
    return sao_status_str(status);
#endif
}
std::wstring menu_status_suffix(sao_status_t status) {
    if (status == SAO_STATUS_OK) return {};
    std::wstring result = L" [";
    const char* text = menu_status_text(status);
    while (text != nullptr && *text != '\0') result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text++)));
    result += L" " + std::to_wstring(status) + L"]";
    return result;
}
void show_menu_open_failure(HWND owner, const char* panel, sao_status_t status) noexcept {
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer), "launcher menu open %s failed: %s (%d)\n", panel, menu_status_text(status), status);
    OutputDebugStringA(buffer);
    std::string message = std::string(panel) + ": " + menu_status_text(status) + " (" + std::to_string(status) + ")";
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    if (sao_ui_compositor_handle_t compositor = borrow_platform_compositor()) {
        (void)sao_ui_dialog_show_error(compositor, nullptr, "SAO Auto", message.c_str(), nullptr, nullptr);
        return;
    }
#endif
    MessageBoxA(owner, message.c_str(), "SAO Auto", MB_OK | MB_ICONERROR | MB_TASKMODAL);
}
constexpr wchar_t kDocsIndexSuffix[] = L"\\docs\\html\\index.html";
constexpr wchar_t kMenuTitle[] = L"SAO Auto";
constexpr UINT kNotificationIconId = 1;
constexpr UINT kNotificationMessage = WM_APP + 1;
constexpr UINT kOpenUserGuideCommand = 1001;
constexpr UINT kOpenSettingsCommand = 1002;
constexpr UINT kOpenHotkeysCommand = 1003;
constexpr UINT kExitCommand = 1004;

class PopupMenu final {
public:
    PopupMenu() noexcept : handle_(CreatePopupMenu()) {}

    ~PopupMenu() noexcept {
        if (handle_) {
            DestroyMenu(handle_);
        }
    }

    PopupMenu(const PopupMenu&) = delete;
    PopupMenu& operator=(const PopupMenu&) = delete;

    [[nodiscard]] HMENU get() const noexcept { return handle_; }

private:
    HMENU handle_ = nullptr;
};

UINT notificationEvent(LPARAM l_param) noexcept {
    const UINT packed = LOWORD(static_cast<DWORD_PTR>(l_param));
    switch (packed) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
    case NIN_SELECT:
    case NIN_KEYSELECT:
        return packed;
    default:
        return static_cast<UINT>(l_param);
    }
}

bool openExistingUserDocsIndex(const wchar_t* docs_index_path,
                               HWND owner) noexcept {
    if (!docs_index_path || docs_index_path[0] == L'\0') return false;
    const DWORD attributes = GetFileAttributesW(docs_index_path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    // 优先用程序内 WebView2 打开；不可用时退回默认浏览器。
    if (openUserGuideInWebView(docs_index_path)) {
        return true;
    }
    const HINSTANCE result =
        ShellExecuteW(owner, L"open", docs_index_path, nullptr, nullptr,
                      SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

} // namespace

bool buildUserDocsIndexPath(const wchar_t* base_dir,
                            wchar_t* path_out,
                            std::size_t path_capacity) noexcept {
    if (!path_out || path_capacity == 0) return false;
    path_out[0] = L'\0';
    std::wstring path;
    if (!buildUserDocsIndexPath(base_dir, path)) return false;
    if (path.size() + 1u > path_capacity) return false;
    std::wmemcpy(path_out, path.c_str(), path.size() + 1u);
    return true;
}

bool buildUserDocsIndexPath(const wchar_t* base_dir,
                            std::wstring& path_out) noexcept {
    path_out.clear();
    if (!base_dir || base_dir[0] == L'\0') return false;
    try {
        path_out.assign(base_dir);
        while (!path_out.empty() &&
               (path_out.back() == wchar_t(92) || path_out.back() == L'/'))
            path_out.pop_back();
        if (path_out.empty()) return false;
        path_out += kDocsIndexSuffix;
        return true;
    } catch (...) {
        path_out.clear();
        return false;
    }
}

bool openUserDocsIndex(const wchar_t* base_dir, HWND owner) noexcept {
    std::wstring docs_index_path;
    return buildUserDocsIndexPath(base_dir, docs_index_path) &&
        openExistingUserDocsIndex(docs_index_path.c_str(), owner);
}

UserMenu::~UserMenu() noexcept {
    destroy();
}

bool UserMenu::create(const wchar_t* base_dir) noexcept {
    destroy();
    if (!base_dir || !*base_dir)
        return false;
    if (!buildUserDocsIndexPath(base_dir, docs_index_path_)) return false;

    instance_ = GetModuleHandleW(nullptr);
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!instance_ || taskbar_created_message_ == 0) {
        destroy();
        return false;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &UserMenu::windowProc;
    window_class.hInstance = instance_;
    window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    if (!window_class.hIcon) {
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    const wchar_t* opaque_class = kSingleInstanceWindowClassName;
    window_class.lpszClassName = opaque_class;
    if (RegisterClassExW(&window_class) == 0) {
        destroy();
        return false;
    }
    window_class_registered_ = true;

    window_ = CreateWindowExW(0, opaque_class, kMenuTitle, WS_OVERLAPPED, 0, 0, 0, 0,
                              nullptr, nullptr, instance_, this);
    if (!window_ || !addNotificationIcon()) {
        destroy();
        return false;
    }
    return true;
}

void UserMenu::destroy() noexcept {
    g_settings_menu_status = SAO_STATUS_OK;
    g_hotkey_menu_status = SAO_STATUS_OK;
    hotkey_owner_ = nullptr;
    if (notification_icon_added_) {
        (void)Shell_NotifyIconW(NIM_DELETE, &notification_icon_);
        notification_icon_added_ = false;
    }
    notification_icon_ = {};

    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (window_class_registered_ && instance_) {
        (void)UnregisterClassW(kSingleInstanceWindowClassName, instance_);
        window_class_registered_ = false;
    }

    instance_ = nullptr;
    taskbar_created_message_ = 0;
    docs_index_path_.clear();
}

void UserMenu::bind_hotkey_owner(hotkey::Owner* owner) noexcept {
    hotkey_owner_ = owner;
}

void UserMenu::unbind_hotkey_owner(hotkey::Owner* owner) noexcept {
    if (owner == nullptr || owner == hotkey_owner_)
        hotkey_owner_ = nullptr;
}

void UserMenu::processCommandLine(const wchar_t* command_line,
                                  bool show_menu_when_empty) noexcept {
    if (!window_ || command_line == nullptr) return;
    AppState command_state{};
    bool should_exit = false;
    int exit_code = SAO_EXIT_OK;
    if (!parseCommandLineText(command_line, command_state, should_exit, exit_code))
        return;

    ShowWindow(window_, SW_SHOWNOACTIVATE);
    SetForegroundWindow(window_);
    if (!command_state.open_path.empty()) {
        if (!openExistingUserDocsIndex(command_state.open_path.c_str(), window_))
            showUserGuideUnavailableError();
    } else if (show_menu_when_empty) {
        showContextMenu();
    }
}

LRESULT CALLBACK UserMenu::windowProc(HWND window,
                                      UINT message,
                                      WPARAM w_param,
                                      LPARAM l_param) noexcept {
    UserMenu* menu = reinterpret_cast<UserMenu*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        menu = static_cast<UserMenu*>(create->lpCreateParams);
        if (!menu) {
            return FALSE;
        }
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(menu)) == 0 &&
            GetLastError() != ERROR_SUCCESS) {
            return FALSE;
        }
    }
    if (menu) {
        return menu->handleMessage(window, message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT UserMenu::handleMessage(HWND window,
                                UINT message,
                                WPARAM w_param,
                                LPARAM l_param) noexcept {
    if (message == WM_COPYDATA) {
        const auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(l_param);
        if (copy == nullptr || copy->dwData != kSingleInstanceCopyDataTag ||
            copy->lpData == nullptr || copy->cbData < sizeof(SingleInstancePayloadHeader))
            return 0;
        const auto* header = static_cast<const SingleInstancePayloadHeader*>(copy->lpData);
        if (header->magic != kSingleInstancePayloadMagic ||
            header->version != kSingleInstancePayloadVersion || header->reserved != 0 ||
            header->target_exe_chars == 0 || header->command_line_chars == 0)
            return 0;
        const std::size_t strings_bytes =
            (static_cast<std::size_t>(header->target_exe_chars) +
             static_cast<std::size_t>(header->command_line_chars)) * sizeof(wchar_t);
        if (strings_bytes > copy->cbData - sizeof(SingleInstancePayloadHeader))
            return 0;
        const auto* strings = reinterpret_cast<const wchar_t*>(
            static_cast<const std::uint8_t*>(copy->lpData) + sizeof(SingleInstancePayloadHeader));
        const wchar_t* command_line = strings + header->target_exe_chars;
        if (strings[header->target_exe_chars - 1u] != L'\0' ||
            command_line[header->command_line_chars - 1u] != L'\0')
            return 0;
        std::wstring current_exe;
        std::wstring current_canonical_exe;
        if (!getCurrentModulePath(current_exe) ||
            !singleInstanceCanonicalInstallPath(current_exe.c_str(),
                                                current_canonical_exe) ||
            singleInstanceInstallIdentity(current_canonical_exe.c_str()) !=
                header->install_identity ||
            singleInstanceInstallIdentity(strings) != header->install_identity)
            return 0;
        processCommandLine(command_line, true);
        return 1;
    }
    if (message == taskbar_created_message_) {
        notification_icon_added_ = false;
        if (!addNotificationIcon()) {
            showMenuUnavailableError();
        }
        return 0;
    }
    if (message == kNotificationMessage) {
        const UINT event = notificationEvent(l_param);
        if (event == WM_LBUTTONDBLCLK) {
            g_settings_menu_status = sao::launcher::settings::open_config_panel_status();
            if (g_settings_menu_status != SAO_STATUS_OK) show_menu_open_failure(window_, "Settings", g_settings_menu_status);
            return 0;
        }
        if (event == WM_LBUTTONUP || event == WM_RBUTTONUP || event == WM_CONTEXTMENU ||
            event == NIN_SELECT || event == NIN_KEYSELECT) {
            POINT activation{};
            const POINT* activation_point = nullptr;
            if (event == NIN_SELECT || event == NIN_KEYSELECT) {
                activation.x = GET_X_LPARAM(w_param);
                activation.y = GET_Y_LPARAM(w_param);
                activation_point = &activation;
            }
            showContextMenu(activation_point);
            return 0;
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

bool UserMenu::addNotificationIcon() noexcept {
    if (!window_) {
        return false;
    }

    notification_icon_ = {};
    notification_icon_.cbSize = sizeof(notification_icon_);
    notification_icon_.hWnd = window_;
    notification_icon_.uID = kNotificationIconId;
    notification_icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    notification_icon_.uCallbackMessage = kNotificationMessage;
    notification_icon_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    if (!notification_icon_.hIcon) {
        notification_icon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    if (!notification_icon_.hIcon) {
        return false;
    }
    lstrcpynW(notification_icon_.szTip, kMenuTitle,
              static_cast<int>(std::size(notification_icon_.szTip)));

    notification_icon_added_ = Shell_NotifyIconW(NIM_ADD, &notification_icon_) != FALSE;
    if (!notification_icon_added_)
        return false;
    notification_icon_.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &notification_icon_)) {
        (void)Shell_NotifyIconW(NIM_DELETE, &notification_icon_);
        notification_icon_added_ = false;
        return false;
    }
    return true;
}

void UserMenu::showContextMenu(const POINT* activation_point) noexcept {
    PopupMenu menu;
    const std::wstring settings_label = L"设置" + menu_status_suffix(g_settings_menu_status);
    const std::wstring hotkey_label = L"快捷键" + menu_status_suffix(g_hotkey_menu_status);
    if (!menu.get() ||
        !AppendMenuW(menu.get(), MF_STRING, kOpenSettingsCommand, settings_label.c_str()) ||
        !AppendMenuW(menu.get(), MF_STRING, kOpenHotkeysCommand, hotkey_label.c_str()) ||
        !AppendMenuW(menu.get(), MF_STRING, kOpenUserGuideCommand, L"关于与用户指南") ||
        !AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr) ||
        !AppendMenuW(menu.get(), MF_STRING, kExitCommand, L"退出")) {
        showMenuUnavailableError();
        return;
    }
    (void)SetMenuDefaultItem(menu.get(), kOpenSettingsCommand, FALSE);

    POINT cursor = activation_point == nullptr ? POINT{} : *activation_point;
    if (activation_point == nullptr && !GetCursorPos(&cursor)) {
        showMenuUnavailableError();
        return;
    }

    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        cursor.x, cursor.y, 0, window_, nullptr);
    (void)PostMessageW(window_, WM_NULL, 0, 0);

    if (command == kOpenSettingsCommand) {
        g_settings_menu_status = sao::launcher::settings::open_config_panel_status();
        if (g_settings_menu_status != SAO_STATUS_OK) show_menu_open_failure(window_, "Settings", g_settings_menu_status);
    } else if (command == kOpenHotkeysCommand) {
        g_hotkey_menu_status = sao::launcher::hotkey::open_config_panel_status(hotkey_owner_);
        if (g_hotkey_menu_status != SAO_STATUS_OK) show_menu_open_failure(window_, "Hotkeys", g_hotkey_menu_status);
    } else if (command == kOpenUserGuideCommand) {
        openUserGuide();
    } else if (command == kExitCommand) {
        PostQuitMessage(0);
    }
}

void UserMenu::openUserGuide() noexcept {
    if (!openExistingUserDocsIndex(docs_index_path_.c_str(), window_)) {
        showUserGuideUnavailableError();
    }
}

void UserMenu::showMenuUnavailableError() const noexcept {
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    if (sao_ui_compositor_handle_t compositor = borrow_platform_compositor()) {
        sao_ui_dialog_show_error(compositor, nullptr, "SAO Auto",
                                  "启动器菜单暂时不可用，请稍后重试。", nullptr, nullptr);
        return;
    }
#endif
    MessageBoxW(window_, L"启动器菜单暂时不可用，请稍后重试。", kMenuTitle,
                MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

void UserMenu::showUserGuideUnavailableError() const noexcept {
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    if (sao_ui_compositor_handle_t compositor = borrow_platform_compositor()) {
        sao_ui_dialog_show_error(compositor, nullptr, "SAO Auto",
                                  "用户指南暂时不可用。请重新安装或修复 SAO Auto 后重试。",
                                  nullptr, nullptr);
        return;
    }
#endif
    MessageBoxW(window_, L"用户指南暂时不可用。请重新安装或修复 SAO Auto 后重试。", kMenuTitle,
                MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

} // namespace sao::launcher
