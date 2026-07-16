#include "sao/core/window.h"

#ifdef _WIN32
#include <windows.h>

#include <cwchar>
#include <string>

namespace {

struct FindWindowContext {
    uint32_t pid;
    const wchar_t* class_name;
    const wchar_t* title_substring;
    HWND found;
};

BOOL CALLBACK find_window_callback(HWND hwnd, LPARAM parameter) {
    auto* context = reinterpret_cast<FindWindowContext*>(parameter);
    DWORD window_pid = 0;
    GetWindowThreadProcessId(hwnd, &window_pid);
    if (window_pid != context->pid || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    if (context->class_name != nullptr) {
        wchar_t class_name[256]{};
        if (GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name))) == 0 ||
            CompareStringOrdinal(class_name, -1, context->class_name, -1, TRUE) != CSTR_EQUAL) {
            return TRUE;
        }
    }

    if (context->title_substring != nullptr) {
        const int title_length = GetWindowTextLengthW(hwnd);
        std::wstring title(static_cast<size_t>(title_length) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, title.data(), title_length + 1);
        title.resize(static_cast<size_t>(copied));
        if (title.find(context->title_substring) == std::wstring::npos) {
            return TRUE;
        }
    }

    context->found = hwnd;
    return FALSE;
}

}  // namespace
#endif

extern "C" sao_status_t SAO_CORE_CALL sao_core_window_get_screen_info(
    SaoScreenInfo* out_info) {
#ifdef _WIN32
    if (out_info == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_info = SaoScreenInfo{
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN),
        static_cast<uint32_t>(GetSystemMetrics(SM_CMONITORS)),
        0,
    };
    return SAO_STATUS_OK;
#else
    (void)out_info;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_window_find_top_level_by_pid(
    uint32_t pid,
    const wchar_t* class_utf16,
    const wchar_t* title_substr_utf16,
    void** out_hwnd) {
    if (out_hwnd == nullptr || pid == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_hwnd = nullptr;
#ifdef _WIN32
    try {
        FindWindowContext context{pid, class_utf16, title_substr_utf16, nullptr};
        if (EnumWindows(find_window_callback, reinterpret_cast<LPARAM>(&context)) == FALSE &&
            context.found == nullptr) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        if (context.found == nullptr) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        *out_hwnd = context.found;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_window_get_client_rect_screen(
    void* hwnd, SaoRect* out_rect) {
#ifdef _WIN32
    if (hwnd == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (out_rect == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    RECT rect{};
    if (GetClientRect(static_cast<HWND>(hwnd), &rect) == FALSE) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    POINT points[2]{{rect.left, rect.top}, {rect.right, rect.bottom}};
    SetLastError(ERROR_SUCCESS);
    if (MapWindowPoints(static_cast<HWND>(hwnd), nullptr, points, 2) == 0 &&
        GetLastError() != ERROR_SUCCESS) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    *out_rect = SaoRect{points[0].x, points[0].y, points[1].x, points[1].y};
    return SAO_STATUS_OK;
#else
    (void)hwnd;
    (void)out_rect;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_window_is_visible(
    void* hwnd, bool* out_visible) {
    if (out_visible == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_visible = false;
#ifdef _WIN32
    if (hwnd == nullptr || IsWindow(static_cast<HWND>(hwnd)) == FALSE) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    *out_visible = IsWindowVisible(static_cast<HWND>(hwnd)) != FALSE;
    return SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_window_is_foreground(
    void* hwnd, bool* out_is_foreground) {
    if (out_is_foreground == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_is_foreground = false;
#ifdef _WIN32
    if (hwnd == nullptr || IsWindow(static_cast<HWND>(hwnd)) == FALSE) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    *out_is_foreground = GetForegroundWindow() == static_cast<HWND>(hwnd);
    return SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}
