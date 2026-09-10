// SAO Auto — launcher/user_menu.h

#pragma once

#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <string>

namespace sao::launcher {

namespace hotkey {
class Owner;
}

bool buildUserDocsIndexPath(const wchar_t* base_dir, wchar_t* path_out,
                            std::size_t path_capacity) noexcept;
bool buildUserDocsIndexPath(const wchar_t* base_dir, std::wstring& path_out) noexcept;

bool openUserDocsIndex(const wchar_t* base_dir, HWND owner) noexcept;
bool openUserDocsIndex(const wchar_t* base_dir, HWND owner, bool native_intro_completed) noexcept;

class UserMenu final {
  public:
    UserMenu() noexcept = default;
    ~UserMenu() noexcept;

    UserMenu(const UserMenu&) = delete;
    UserMenu& operator=(const UserMenu&) = delete;

    bool create(const wchar_t* base_dir) noexcept;
    void destroy() noexcept;
    void bind_hotkey_owner(hotkey::Owner* owner) noexcept;
    void unbind_hotkey_owner(hotkey::Owner* owner) noexcept;
    void processCommandLine(const wchar_t* command_line, bool show_menu_when_empty = true) noexcept;

  private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w_param,
                                       LPARAM l_param) noexcept;

    LRESULT handleMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param) noexcept;
    bool addNotificationIcon() noexcept;
    void showContextMenu(const POINT* activation_point = nullptr) noexcept;
    void openUserGuide() noexcept;
    void showMenuUnavailableError() const noexcept;
    void showUserGuideUnavailableError() const noexcept;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    NOTIFYICONDATAW notification_icon_{};
    UINT taskbar_created_message_ = 0;
    bool notification_icon_added_ = false;
    bool window_class_registered_ = false;
    std::wstring docs_index_path_;
    hotkey::Owner* hotkey_owner_ = nullptr;
};

} // namespace sao::launcher
