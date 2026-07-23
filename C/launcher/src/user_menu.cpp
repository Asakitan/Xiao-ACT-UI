// SAO Auto — launcher/user_menu.cpp

#include "sao/launcher/user_menu.h"

#include "sao_security/obfuscation/enc_str.h"

#include <cstddef>
#include <cwchar>
#include <iterator>

namespace sao::launcher {
namespace {

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
constexpr UINT kExitCommand = 1002;

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

bool isMouseActivationMessage(UINT message) noexcept {
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_CONTEXTMENU;
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
    if (message == kNotificationMessage && isMouseActivationMessage(static_cast<UINT>(l_param))) {
        showContextMenu();
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
    return notification_icon_added_;
}

void UserMenu::showContextMenu() noexcept {
    PopupMenu menu;
    if (!menu.get() ||
        !AppendMenuW(menu.get(), MF_STRING, kOpenUserGuideCommand, L"关于 / 用户指南") ||
        !AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr) ||
        !AppendMenuW(menu.get(), MF_STRING, kExitCommand, L"退出")) {
        showMenuUnavailableError();
        return;
    }

    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        showMenuUnavailableError();
        return;
    }

    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        cursor.x, cursor.y, 0, window_, nullptr);
    (void)PostMessageW(window_, WM_NULL, 0, 0);

    if (command == kOpenUserGuideCommand) {
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
    MessageBoxW(window_, L"启动器菜单暂时不可用，请稍后重试。", kMenuTitle,
                MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

void UserMenu::showUserGuideUnavailableError() const noexcept {
    MessageBoxW(window_, L"用户指南暂时不可用。请重新安装或修复 SAO Auto 后重试。", kMenuTitle,
                MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

} // namespace sao::launcher
