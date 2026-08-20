// SAO Auto — launcher/user_menu.cpp

#include "sao/launcher/user_menu.h"

#include "hotkey_config_panel.h"
#include "settings_config_panel.h"

#include "sao_security/obfuscation/enc_str.h"

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#include "sao/ui/dialog.h"
#endif

#include <cstddef>
#include <cwchar>
#include <iterator>
#include <windowsx.h>

namespace sao::launcher {
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

// ASCII-only widening for identifiers decrypted from SAO_ENC_STR.
// Byte-by-byte widen to keep decrypted plaintext scoped to this TU.
void widen_ascii(const char* src, wchar_t* dst, std::size_t dst_cap) {
    if (!dst || dst_cap == 0) return;
    std::size_t i = 0;
    if (src) {
        for (; src[i] != '\0' && i + 1 < dst_cap; ++i) {
            dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
        }
    }
    dst[i] = L'\0';
}

// Opaque window class name (replaces plaintext "SaoAuto.Launcher.UserMenu").
// Wrap the static array in a struct so C++11 magic-statics give us
// thread-safe once-initialisation across concurrent create() / destroy().
const wchar_t* opaque_window_class_name() noexcept {
    struct Widened {
        wchar_t buf[32];
        Widened() noexcept {
            const auto enc = SAO_ENC_STR("4F5A.um");
            widen_ascii(enc.decrypt(), buf, std::size(buf));
        }
    };
    static const Widened w{};
    return w.buf;
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
    const HINSTANCE result =
        ShellExecuteW(owner, L"open", docs_index_path, nullptr, nullptr,
                      SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

} // namespace

bool buildUserDocsIndexPath(const wchar_t* base_dir,
                            wchar_t* path_out,
                            std::size_t path_capacity) noexcept {
    if (!path_out || path_capacity == 0) {
        return false;
    }
    path_out[0] = L'\0';
    if (!base_dir || base_dir[0] == L'\0') {
        return false;
    }

    std::size_t base_length = wcsnlen_s(base_dir, path_capacity);
    if (base_length == 0 || base_length >= path_capacity) {
        return false;
    }
    while (base_length > 0 &&
           (base_dir[base_length - 1] == L'\\' || base_dir[base_length - 1] == L'/')) {
        --base_length;
    }
    if (base_length == 0) {
        return false;
    }

    constexpr std::size_t suffix_length = std::size(kDocsIndexSuffix) - 1;
    if (base_length > path_capacity - 1 ||
        suffix_length > path_capacity - base_length - 1) {
        return false;
    }

    std::wmemcpy(path_out, base_dir, base_length);
    std::wmemcpy(path_out + base_length, kDocsIndexSuffix, suffix_length + 1);
    return true;
}

bool openUserDocsIndex(const wchar_t* base_dir, HWND owner) noexcept {
    wchar_t docs_index_path[MAX_PATH]{};
    if (!buildUserDocsIndexPath(base_dir, docs_index_path,
                                std::size(docs_index_path))) {
        return false;
    }
    return openExistingUserDocsIndex(docs_index_path, owner);
}

UserMenu::~UserMenu() noexcept {
    destroy();
}

bool UserMenu::create(const wchar_t* base_dir) noexcept {
    destroy();
    if (!buildUserDocsIndexPath(base_dir, docs_index_path_, std::size(docs_index_path_))) {
        return false;
    }

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
    const wchar_t* opaque_class = opaque_window_class_name();
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
        (void)UnregisterClassW(opaque_window_class_name(), instance_);
        window_class_registered_ = false;
    }

    instance_ = nullptr;
    taskbar_created_message_ = 0;
    docs_index_path_[0] = L'\0';
}

void UserMenu::bind_hotkey_owner(hotkey::Owner* owner) noexcept {
    hotkey_owner_ = owner;
}

void UserMenu::unbind_hotkey_owner(hotkey::Owner* owner) noexcept {
    if (owner == nullptr || owner == hotkey_owner_)
        hotkey_owner_ = nullptr;
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
            sao::launcher::settings::open_config_panel();
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
    if (!menu.get() ||
        !AppendMenuW(menu.get(), MF_STRING, kOpenSettingsCommand, L"设置") ||
        !AppendMenuW(menu.get(), MF_STRING, kOpenHotkeysCommand, L"快捷键") ||
        !AppendMenuW(menu.get(), MF_STRING, kOpenUserGuideCommand, L"关于 / 用户指南") ||
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
        sao::launcher::settings::open_config_panel();
    } else if (command == kOpenHotkeysCommand) {
        sao::launcher::hotkey::open_config_panel(hotkey_owner_);
    } else if (command == kOpenUserGuideCommand) {
        openUserGuide();
    } else if (command == kExitCommand) {
        PostQuitMessage(0);
    }
}

void UserMenu::openUserGuide() noexcept {
    if (!openExistingUserDocsIndex(docs_index_path_, window_)) {
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
