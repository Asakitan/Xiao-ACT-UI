#include "sao_core/window.h"

#include <windows.h>

#include <cstring>
#include <limits>

namespace {

constexpr wchar_t kWindowClassName[] = L"SaoLegacyCoreLayeredWindow";

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool ensure_window_class(HINSTANCE instance) noexcept {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(instance, kWindowClassName, &existing) != FALSE) {
        return true;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&window_class) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

struct EnumContext {
    void** handles;
    size_t capacity;
    size_t count;
    bool overflow;
};

BOOL CALLBACK enum_window_callback(HWND hwnd, LPARAM parameter) {
    auto* context = reinterpret_cast<EnumContext*>(parameter);
    if (context->handles != nullptr) {
        if (context->count >= context->capacity) {
            context->overflow = true;
            return FALSE;
        }
        context->handles[context->count] = hwnd;
    }
    ++context->count;
    return TRUE;
}

bool get_window_long_checked(HWND hwnd, int index, uint32_t* out_value) noexcept {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR value = GetWindowLongPtrW(hwnd, index);
    if (value == 0 && GetLastError() != ERROR_SUCCESS) {
        return false;
    }
    *out_value = static_cast<uint32_t>(value);
    return true;
}

}  // namespace

extern "C" int32_t SAO_CORE_CALL sao_core_window_create_layered_topmost(
    const wchar_t* title, int32_t x, int32_t y, int32_t w, int32_t h, void** out_hwnd) {
    if (out_hwnd != nullptr) {
        *out_hwnd = nullptr;
    }
    if (out_hwnd == nullptr || title == nullptr || w <= 0 || h <= 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr || !ensure_window_class(instance)) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClassName,
        title,
        WS_POPUP,
        x,
        y,
        w,
        h,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (hwnd == nullptr) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    *out_hwnd = hwnd;
    return SAO_OK;
}

extern "C" int32_t SAO_CORE_CALL sao_core_window_destroy(void* hwnd) {
    if (hwnd == nullptr || IsWindow(static_cast<HWND>(hwnd)) == FALSE) {
        return SAO_ERR_HANDLE_INVALID;
    }
    return DestroyWindow(static_cast<HWND>(hwnd)) != FALSE ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
}

extern "C" int32_t SAO_CORE_CALL sao_core_window_enumerate(
    void** out_hwnds, size_t max_hwnds, size_t* out_hwnd_count) {
    if (out_hwnd_count != nullptr) {
        *out_hwnd_count = 0;
    }
    if (max_hwnds > std::numeric_limits<size_t>::max() / sizeof(*out_hwnds)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (out_hwnds != nullptr && max_hwnds != 0) {
        std::memset(out_hwnds, 0, max_hwnds * sizeof(*out_hwnds));
    }
    if (out_hwnd_count == nullptr || (out_hwnds == nullptr && max_hwnds != 0)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    EnumContext count_context{nullptr, 0, 0, false};
    SetLastError(ERROR_SUCCESS);
    if (EnumWindows(enum_window_callback, reinterpret_cast<LPARAM>(&count_context)) == FALSE) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_hwnd_count = count_context.count;
    if (out_hwnds == nullptr) {
        return SAO_OK;
    }
    if (max_hwnds < count_context.count) {
        return SAO_ERR_BUFFER_TOO_SMALL;
    }

    EnumContext fill_context{out_hwnds, max_hwnds, 0, false};
    SetLastError(ERROR_SUCCESS);
    const BOOL enum_ok = EnumWindows(enum_window_callback, reinterpret_cast<LPARAM>(&fill_context));
    if (enum_ok == FALSE || fill_context.overflow) {
        std::memset(out_hwnds, 0, max_hwnds * sizeof(*out_hwnds));
        *out_hwnd_count = fill_context.overflow ? fill_context.count + 1 : 0;
        return fill_context.overflow ? SAO_ERR_BUFFER_TOO_SMALL : SAO_ERR_OS_CALL_FAILED;
    }

    *out_hwnd_count = fill_context.count;
    return SAO_OK;
}

extern "C" int32_t SAO_CORE_CALL sao_core_window_get_info(
    void* opaque_hwnd, SaoCoreWindowInfo* out_info) {
    if (out_info != nullptr) {
        *out_info = {};
    }
    if (out_info == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    HWND hwnd = static_cast<HWND>(opaque_hwnd);
    if (hwnd == nullptr || IsWindow(hwnd) == FALSE) {
        return SAO_ERR_HANDLE_INVALID;
    }

    RECT window_rect{};
    RECT client_rect{};
    if (GetWindowRect(hwnd, &window_rect) == FALSE || GetClientRect(hwnd, &client_rect) == FALSE) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    POINT client_points[2]{
        {client_rect.left, client_rect.top},
        {client_rect.right, client_rect.bottom},
    };
    SetLastError(ERROR_SUCCESS);
    if (MapWindowPoints(hwnd, nullptr, client_points, 2) == 0 &&
        GetLastError() != ERROR_SUCCESS) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    SaoCoreWindowInfo info{};
    DWORD pid = 0;
    info.thread_id = GetWindowThreadProcessId(hwnd, &pid);
    if (info.thread_id == 0) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    info.pid = pid;
    info.hwnd = hwnd;
    info.window_rect = SaoCoreRect{
        window_rect.left, window_rect.top, window_rect.right, window_rect.bottom};
    info.client_rect_screen = SaoCoreRect{
        client_points[0].x, client_points[0].y, client_points[1].x, client_points[1].y};
    if (!get_window_long_checked(hwnd, GWL_STYLE, &info.style) ||
        !get_window_long_checked(hwnd, GWL_EXSTYLE, &info.extended_style)) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    info.visible = IsWindowVisible(hwnd) != FALSE ? 1u : 0u;
    info.minimized = IsIconic(hwnd) != FALSE ? 1u : 0u;

    SetLastError(ERROR_SUCCESS);
        if (GetWindowTextW(
            hwnd,
            info.title,
            static_cast<int>(sizeof(info.title) / sizeof(info.title[0]))) == 0 &&
        GetLastError() != ERROR_SUCCESS) {
        return SAO_ERR_OS_CALL_FAILED;
    }
        if (GetClassNameW(
            hwnd,
            info.class_name,
            static_cast<int>(sizeof(info.class_name) / sizeof(info.class_name[0]))) == 0) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    *out_info = info;
    return SAO_OK;
}
