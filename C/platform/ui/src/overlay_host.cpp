#include "sao/ui/overlay_host.h"
#include "sao/ui/dc_mutation.h"

#if !defined(SAO_UI_OVERLAY_HOST_TESTING) &&                                                       \
    __has_include("sao_security/anti_screencap/capture_mode.h") &&                                 \
                  __has_include("sao_security/anti_screencap/overlay_host_capture.h")
#define SAO_UI_HAS_ANTI_SCREENCAP_FACADE 1
#include "sao_security/anti_screencap/capture_mode.h"
#include "sao_security/anti_screencap/overlay_host_capture.h"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <windows.h>
#include <windowsx.h>

#include "input_win32_api.h"
#endif

#if defined(_WIN32)

static_assert(sizeof(HWND) == sizeof(void*), "HWND must remain pointer-width");
static_assert(sizeof(HRGN) == sizeof(void*), "HRGN must remain pointer-width");

struct sao_ui_overlay_host_s {
    HWND hwnd = nullptr;
    HWND control_hwnd = nullptr;
    HWND owner_hwnd = nullptr;
    HINSTANCE hinstance = nullptr;
    ATOM class_atom = 0;
    DWORD owner_thread_id = 0;
    std::wstring class_name;
    sao_ui_dc_mutation_coordinator_handle_t dc_mutation = nullptr;
    void* dc_mutation_token = nullptr;

    std::mutex input_update_mu;
    std::mutex state_mu;
    bool visible = false;
    bool input_passthrough = true;
    uint32_t input_sync_state = SAO_UI_OVERLAY_INPUT_SYNCHRONIZED;
    bool capture_excluded = false;
    std::vector<SaoOverlayHostInputRect> previous_input_rects;

    sao_ui_hit_test_fn_t hit_test_fn = nullptr;
    void* hit_test_user = nullptr;
    sao_ui_mouse_fn_t mouse_fn = nullptr;
    void* mouse_user = nullptr;

    SaoOverlayHostClientRect client_rect{};
    SaoOverlayHostClientRect desired_rect{};
    uint32_t current_dpi = 96;
    SaoOverlayHostWMCounters wm_counters{};

    sao_ui_size_fn_t size_fn = nullptr;
    void* size_user = nullptr;
    sao_ui_move_fn_t move_fn = nullptr;
    void* move_user = nullptr;
    sao_ui_activate_fn_t activate_fn = nullptr;
    void* activate_user = nullptr;
    sao_ui_dpi_changed_fn_t dpi_changed_fn = nullptr;
    void* dpi_changed_user = nullptr;
    sao_ui_display_change_fn_t display_change_fn = nullptr;
    void* display_change_user = nullptr;
};

namespace {

#if !defined(SAO_UI_OVERLAY_HOST_TESTING)
constexpr wchar_t kSingleInstanceMutexName[] =
    L"Local\\{7F3E2A91-4C8B-4D6E-9A15-8B2F0C4E3D7A}";
#endif
constexpr SaoUiDcMutationRect kPhysicalRectScrub{0, 0, 1, 1};
constexpr uint32_t kPhysicalRectScrubSettleMs = 40;
constexpr uint32_t kPhysicalRectScrubTimeoutMs = 2000;

using OverlayHostWin32Api = sao::ui::overlay_host_detail::Win32Api;

const OverlayHostWin32Api& system_win32_api() {
    static const OverlayHostWin32Api api{
        &::SetWindowRgn,      &::GetWindowRgn, &::GetWindowLongPtrW,
        &::SetWindowLongPtrW, &::SetWindowPos, &::DeleteObject,
    };
    return api;
}

std::atomic<const OverlayHostWin32Api*>& win32_api_override() {
    static std::atomic<const OverlayHostWin32Api*> api{nullptr};
    return api;
}

const OverlayHostWin32Api& win32_api() {
    const auto* override_api = win32_api_override().load(std::memory_order_acquire);
    return override_api != nullptr ? *override_api : system_win32_api();
}

std::mutex& global_instance_mutex() {
    static std::mutex mutex;
    return mutex;
}

HANDLE& global_instance_handle() {
    static HANDLE handle = nullptr;
    return handle;
}

bool acquire_single_instance_lock() {
    std::lock_guard<std::mutex> lock(global_instance_mutex());
    if (global_instance_handle() != nullptr)
        return false;
#if defined(SAO_UI_OVERLAY_HOST_TESTING)
    wchar_t test_mutex_name[80]{};
    ::swprintf_s(test_mutex_name, L"Local\\SaoAutoOverlayHost.Test.%08lX",
                 static_cast<unsigned long>(::GetCurrentProcessId()));
    const wchar_t* mutex_name = test_mutex_name;
#else
    const wchar_t* mutex_name = kSingleInstanceMutexName;
#endif
    HANDLE handle = ::CreateMutexW(nullptr, TRUE, mutex_name);
    if (handle == nullptr)
        return false;
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(handle);
        return false;
    }
    global_instance_handle() = handle;
    return true;
}

void release_single_instance_lock() {
    std::lock_guard<std::mutex> lock(global_instance_mutex());
    HANDLE& handle = global_instance_handle();
    if (handle != nullptr) {
        ::ReleaseMutex(handle);
        ::CloseHandle(handle);
        handle = nullptr;
    }
}

std::wstring make_class_name() {
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    static std::atomic<uint32_t> sequence{0};
    const uint32_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
    const uint32_t pid = static_cast<uint32_t>(::GetCurrentProcessId());
    const uint32_t mix_a =
        static_cast<uint32_t>(counter.LowPart) ^ (pid * 0x9E3779B1u);
    const uint32_t mix_b =
        static_cast<uint32_t>(counter.HighPart) ^ (seq * 0xC2B2AE35u);
    wchar_t name[64]{};
    ::swprintf_s(name, L"{%08lX-%04lX-%04lX-%04lX-%08lX%04lX}",
                 static_cast<unsigned long>(mix_a),
                 static_cast<unsigned long>((mix_b >> 16) & 0xFFFFu),
                 static_cast<unsigned long>(mix_b & 0xFFFFu),
                 static_cast<unsigned long>((pid ^ seq) & 0xFFFFu),
                 static_cast<unsigned long>(counter.LowPart ^
                                             (counter.HighPart << 3)),
                 static_cast<unsigned long>(seq & 0xFFFFu));
    return name;
}

uint32_t query_window_dpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    const auto get_dpi =
        user32 == nullptr
            ? nullptr
            : reinterpret_cast<GetDpiForWindowFn>(::GetProcAddress(user32, "GetDpiForWindow"));
    const UINT dpi = get_dpi == nullptr ? 0 : get_dpi(hwnd);
    return dpi == 0 ? 96 : dpi;
}

void mark_input_partial(sao_ui_overlay_host_s* host) {
    std::lock_guard<std::mutex> lock(host->state_mu);
    host->input_sync_state = SAO_UI_OVERLAY_INPUT_PARTIAL;
}

sao_status_t last_win32_status() {
    return ::GetLastError() == ERROR_ACCESS_DENIED ? SAO_STATUS_ERR_ACCESS_DENIED
                                                   : SAO_STATUS_ERR_OS_CALL_FAILED;
}

sao_status_t submit_physical_rect_scrub(sao_ui_overlay_host_s* host) {
    if (host->dc_mutation == nullptr)
        return SAO_STATUS_OK;
    const sao_status_t status = sao_ui_dc_mutation_coordinator_submit_hide_window_rect(
        host->dc_mutation, host->hwnd, &kPhysicalRectScrub, kPhysicalRectScrubSettleMs,
        kPhysicalRectScrubTimeoutMs);
    return status == SAO_STATUS_ERR_NOT_INITIALIZED ? SAO_STATUS_OK : status;
}

void publish_real_geometry(sao_ui_overlay_host_s* host, const SaoOverlayHostClientRect& geometry) {
    std::lock_guard<std::mutex> lock(host->state_mu);
    host->client_rect = geometry;
    host->desired_rect = geometry;
}

bool restore_passthrough_style(sao_ui_overlay_host_s* host, LONG_PTR old_style) {
    const auto& api = win32_api();
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = api.set_window_long_ptr_w(host->hwnd, GWL_EXSTYLE, old_style);
    if (previous == 0 && ::GetLastError() != ERROR_SUCCESS)
        return false;
    if (!api.set_window_pos(host->hwnd, nullptr, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                                SWP_FRAMECHANGED)) {
        return false;
    }
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR readback = api.get_window_long_ptr_w(host->hwnd, GWL_EXSTYLE);
    return (readback != 0 || ::GetLastError() == ERROR_SUCCESS) && readback == old_style;
}

sao_status_t apply_passthrough_unlocked(sao_ui_overlay_host_s* host, bool passthrough) {
    if (host == nullptr || host->hwnd == nullptr || !::IsWindow(host->hwnd)) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    const auto& api = win32_api();
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR old_style = api.get_window_long_ptr_w(host->hwnd, GWL_EXSTYLE);
    if (old_style == 0 && ::GetLastError() != ERROR_SUCCESS) {
        return last_win32_status();
    }
    const LONG_PTR new_style = passthrough ? old_style | WS_EX_TRANSPARENT
                                           : old_style & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    if (new_style != old_style) {
        ::SetLastError(ERROR_SUCCESS);
        const LONG_PTR previous = api.set_window_long_ptr_w(host->hwnd, GWL_EXSTYLE, new_style);
        if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
            return last_win32_status();
        }
        if (!api.set_window_pos(host->hwnd, nullptr, 0, 0, 0, 0,
                                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                                    SWP_FRAMECHANGED)) {
            if (!restore_passthrough_style(host, old_style))
                mark_input_partial(host);
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
    }
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR readback = api.get_window_long_ptr_w(host->hwnd, GWL_EXSTYLE);
    if (readback == 0 && ::GetLastError() != ERROR_SUCCESS) {
        const sao_status_t status = last_win32_status();
        if (new_style != old_style && !restore_passthrough_style(host, old_style)) {
            mark_input_partial(host);
        }
        return status;
    }
    if (((readback & WS_EX_TRANSPARENT) != 0) != passthrough) {
        if (new_style != old_style && !restore_passthrough_style(host, old_style)) {
            mark_input_partial(host);
        }
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    {
        std::lock_guard<std::mutex> lock(host->state_mu);
        host->input_passthrough = passthrough;
    }
    return SAO_STATUS_OK;
}

bool append_rect(HRGN destination, const SaoOverlayHostInputRect& rect) {
    const int64_t right = static_cast<int64_t>(rect.x) + rect.width;
    const int64_t bottom = static_cast<int64_t>(rect.y) + rect.height;
    if (rect.width <= 0 || rect.height <= 0 || right > INT32_MAX || bottom > INT32_MAX)
        return false;
    HRGN item = ::CreateRectRgn(rect.x, rect.y, static_cast<int>(right), static_cast<int>(bottom));
    if (item == nullptr)
        return false;
    const int result = ::CombineRgn(destination, destination, item, RGN_OR);
    ::DeleteObject(item);
    return result != ERROR;
}

class OwnedRegion {
  public:
    explicit OwnedRegion(HRGN region) noexcept : region_(region) {}

    ~OwnedRegion() {
        if (region_ != nullptr)
            win32_api().delete_object(region_);
    }

    OwnedRegion(const OwnedRegion&) = delete;
    OwnedRegion& operator=(const OwnedRegion&) = delete;

    HRGN get() const noexcept {
        return region_;
    }

    void release() noexcept {
        region_ = nullptr;
    }

  private:
    HRGN region_ = nullptr;
};

void dispatch_mouse(sao_ui_overlay_host_s* host, UINT message, WPARAM wparam, LPARAM lparam) {
    sao_ui_mouse_fn_t callback = nullptr;
    void* user_data = nullptr;
    {
        std::lock_guard<std::mutex> lock(host->state_mu);
        callback = host->mouse_fn;
        user_data = host->mouse_user;
    }
    if (callback == nullptr)
        return;

    POINT point{};
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_MOUSELEAVE) {
        (void)::GetCursorPos(&point);
    } else {
        point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    }
    if (message != WM_MOUSEWHEEL && message != WM_CAPTURECHANGED &&
        message != WM_CANCELMODE && message != WM_MOUSELEAVE) {
        ::ClientToScreen(host->hwnd, &point);
    }
    int32_t button = -1;
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK) {
        button = 0;
    } else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
               message == WM_RBUTTONDBLCLK) {
        button = 1;
    } else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ||
               message == WM_MBUTTONDBLCLK) {
        button = 2;
    }
    callback(message, point.x, point.y, button,
             message == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(wparam) : 0, user_data);
}

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        if (create != nullptr) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
    }
    auto* host = reinterpret_cast<sao_ui_overlay_host_s*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const bool is_render_host = host != nullptr && host->hwnd == hwnd;

    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND)
        return 1;
    if (!is_render_host) {
        if (message == WM_NCDESTROY)
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    if (message == WM_NCHITTEST) {
        sao_ui_hit_test_fn_t callback = nullptr;
        void* user_data = nullptr;
        bool passthrough = true;
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            callback = host->hit_test_fn;
            user_data = host->hit_test_user;
            passthrough = host->input_passthrough;
        }
        if (passthrough)
            return HTTRANSPARENT;
        return callback == nullptr ||
                       callback(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), user_data)
                   ? HTCLIENT
                   : HTTRANSPARENT;
    }

    switch (message) {
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
        dispatch_mouse(host, message, wparam, lparam);
        return 0;
    }
    case WM_MOUSELEAVE:
    case WM_MOUSEWHEEL:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
        dispatch_mouse(host, message, wparam, lparam);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        ::SetCapture(hwnd);
        dispatch_mouse(host, message, wparam, lparam);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        dispatch_mouse(host, message, wparam, lparam);
        if (::GetCapture() == hwnd)
            ::ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lparam) != hwnd)
            dispatch_mouse(host, message, wparam, lparam);
        return 0;
    case WM_CANCELMODE:
        dispatch_mouse(host, message, wparam, lparam);
        if (::GetCapture() == hwnd)
            ::ReleaseCapture();
        return 0;
    case WM_SIZE: {
        sao_ui_size_fn_t callback = nullptr;
        void* user_data = nullptr;
        const int32_t width = LOWORD(lparam);
        const int32_t height = HIWORD(lparam);
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            host->client_rect.width = width;
            host->client_rect.height = height;
            ++host->wm_counters.size_events;
            callback = host->size_fn;
            user_data = host->size_user;
        }
        if (callback != nullptr)
            callback(width, height, user_data);
        return 0;
    }
    case WM_MOVE: {
        sao_ui_move_fn_t callback = nullptr;
        void* user_data = nullptr;
        const int32_t x = GET_X_LPARAM(lparam);
        const int32_t y = GET_Y_LPARAM(lparam);
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            host->client_rect.x = x;
            host->client_rect.y = y;
            ++host->wm_counters.move_events;
            callback = host->move_fn;
            user_data = host->move_user;
        }
        if (callback != nullptr)
            callback(x, y, user_data);
        return 0;
    }
    case WM_ACTIVATE: {
        sao_ui_activate_fn_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            ++host->wm_counters.activate_events;
            callback = host->activate_fn;
            user_data = host->activate_user;
        }
        if (callback != nullptr)
            callback(LOWORD(wparam) != WA_INACTIVE, user_data);
        return 0;
    }
    case WM_WINDOWPOSCHANGING:
        // z_order.cpp is the only component allowed to change z-order.
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            ++host->wm_counters.windowposchanging_events;
        }
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        const uint32_t dpi = LOWORD(wparam);
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
        bool geometry_published = false;
        if (suggested != nullptr) {
            x = suggested->left;
            y = suggested->top;
            width = suggested->right - suggested->left;
            height = suggested->bottom - suggested->top;
            geometry_published = width > 0 && height > 0 &&
                                 win32_api().set_window_pos(hwnd, nullptr, x, y, width, height,
                                                            SWP_NOACTIVATE | SWP_NOZORDER) != FALSE;
            if (geometry_published) {
                publish_real_geometry(host, {x, y, width, height});
                (void)::DwmFlush();
                (void)submit_physical_rect_scrub(host);
            }
        }
        sao_ui_dpi_changed_fn_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            host->current_dpi = dpi == 0 ? 96 : dpi;
            ++host->wm_counters.dpichanged_events;
            callback = host->dpi_changed_fn;
            user_data = host->dpi_changed_user;
        }
        if (callback != nullptr)
            callback(dpi, x, y, width, height, user_data);
        return 0;
    }
    case WM_DISPLAYCHANGE: {
        sao_ui_display_change_fn_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            host->current_dpi = query_window_dpi(hwnd);
            ++host->wm_counters.displaychange_events;
            callback = host->display_change_fn;
            user_data = host->display_change_user;
        }
        if (callback != nullptr) {
            callback(static_cast<uint32_t>(wparam), LOWORD(lparam), HIWORD(lparam), user_data);
        }
        return 0;
    }
    case WM_NCDESTROY:
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

void destroy_created_host(sao_ui_overlay_host_s* host) {
    if (host->hwnd != nullptr)
        ::DestroyWindow(host->hwnd);
    if (host->control_hwnd != nullptr)
        ::DestroyWindow(host->control_hwnd);
    if (host->owner_hwnd != nullptr)
        ::DestroyWindow(host->owner_hwnd);
    if (host->class_atom != 0)
        ::UnregisterClassW(host->class_name.c_str(), host->hinstance);
    delete host;
}

} // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_create(
    const SaoOverlayHostConfig* config, sao_ui_overlay_host_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (!acquire_single_instance_lock())
        return SAO_STATUS_ERR_ALREADY_EXISTS;

    auto* host = new (std::nothrow) sao_ui_overlay_host_s();
    if (host == nullptr) {
        release_single_instance_lock();
        return SAO_STATUS_ERR_UNKNOWN;
    }
    host->hinstance = ::GetModuleHandleW(nullptr);
    host->owner_thread_id = ::GetCurrentThreadId();
    host->class_name = make_class_name();
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = overlay_wndproc;
    window_class.hInstance = host->hinstance;
    window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = host->class_name.c_str();
    host->class_atom = ::RegisterClassExW(&window_class);
    if (host->class_atom == 0) {
        delete host;
        release_single_instance_lock();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    const bool explicit_bounds = config != nullptr && config->width > 0 && config->height > 0;
    const int32_t width = explicit_bounds ? config->width : ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int32_t height =
        explicit_bounds ? config->height : ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int32_t x = explicit_bounds ? config->origin_x : ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int32_t y = explicit_bounds ? config->origin_y : ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const wchar_t* title =
        config != nullptr && config->title_utf16 != nullptr ? config->title_utf16 : L"";

    host->owner_hwnd =
        ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, host->class_name.c_str(), L"",
                          WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, host->hinstance, host);
    host->control_hwnd =
        host->owner_hwnd == nullptr
            ? nullptr
            : ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, host->class_name.c_str(), L"",
                                WS_POPUP, 0, 0, 1, 1, host->owner_hwnd, nullptr, host->hinstance,
                                host);
    host->hwnd = host->control_hwnd == nullptr
                     ? nullptr
                     : ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                         host->class_name.c_str(), title,
                                         WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, x, y,
                                         width > 0 ? width : 1920, height > 0 ? height : 1080,
                                         host->owner_hwnd, nullptr, host->hinstance, host);
    if (host->hwnd == nullptr) {
        destroy_created_host(host);
        release_single_instance_lock();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    MARGINS margins{-1, -1, -1, -1};
    ::DwmExtendFrameIntoClientArea(host->hwnd, &margins);
    host->client_rect = {x, y, width > 0 ? width : 1920, height > 0 ? height : 1080};
    host->desired_rect = host->client_rect;
    host->current_dpi = query_window_dpi(host->hwnd);
    OwnedRegion empty(::CreateRectRgn(0, 0, 0, 0));
    if (empty.get() == nullptr || !::SetWindowRgn(host->hwnd, empty.get(), FALSE)) {
        destroy_created_host(host);
        release_single_instance_lock();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    empty.release();
    host->dc_mutation =
        config == nullptr
            ? nullptr
            : static_cast<sao_ui_dc_mutation_coordinator_handle_t>(config->dc_mutation_coordinator);
    if (host->dc_mutation != nullptr) {
        const sao_status_t register_status = sao_ui_dc_mutation_coordinator_register(
            host->dc_mutation, host->hwnd, &host->dc_mutation_token);
        if (register_status != SAO_STATUS_OK) {
            destroy_created_host(host);
            release_single_instance_lock();
            return register_status;
        }
    }
    *out_handle = host;
    return SAO_STATUS_OK;
}

extern "C" bool SAO_UI_CALL sao_ui_overlay_host_destroy(sao_ui_overlay_host_handle_t handle) {
    if (handle == nullptr)
        return true;
    if (handle->owner_thread_id != ::GetCurrentThreadId())
        return false;
    if (handle->dc_mutation != nullptr && handle->hwnd != nullptr &&
        !sao_ui_dc_mutation_coordinator_invalidate(handle->dc_mutation, handle->hwnd, 1.0)) {
        return false;
    }
    if (handle->hwnd != nullptr && ::GetCapture() == handle->hwnd) {
        dispatch_mouse(handle, WM_CANCELMODE, 0, 0);
        (void)::ReleaseCapture();
    }
#if defined(SAO_UI_HAS_ANTI_SCREENCAP_FACADE)
    SaoAntiScreencapAffinityPair pair{};
    pair.primary_hwnd = handle->hwnd;
    pair.decoy_hwnd = handle->control_hwnd;
    uint32_t effective = SAO_ASC_MODE_INVALID;
    const int32_t affinity_rc = sao_security_anti_screencap_overlay_host_set_capture_mode(
        &pair, SAO_ASC_MODE_NORMAL, &effective);
    if (affinity_rc != SAO_STATUS_OK)
        return false;
    (void)sao_security_anti_screencap_bind_active_pair(nullptr);
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        handle->capture_excluded = false;
    }
#else
    // Without the security provider there is no authoritative affinity
    // owner.  Keep the state explicitly unprotected rather than claiming a
    // capture exclusion that cannot be verified.
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        handle->capture_excluded = false;
    }
#endif
    if (handle->hwnd != nullptr && !::DestroyWindow(handle->hwnd))
        return false;
    handle->hwnd = nullptr;
    if (handle->control_hwnd != nullptr && !::DestroyWindow(handle->control_hwnd))
        return false;
    handle->control_hwnd = nullptr;
    if (handle->owner_hwnd != nullptr && !::DestroyWindow(handle->owner_hwnd))
        return false;
    handle->owner_hwnd = nullptr;
    if (handle->class_atom != 0)
        ::UnregisterClassW(handle->class_name.c_str(), handle->hinstance);
    release_single_instance_lock();
    delete handle;
    return true;
}

extern "C" void* SAO_UI_CALL sao_ui_overlay_host_hwnd(sao_ui_overlay_host_handle_t handle) {
    return handle == nullptr ? nullptr : handle->hwnd;
}

extern "C" void* SAO_UI_CALL sao_ui_overlay_host_control_hwnd(sao_ui_overlay_host_handle_t handle) {
    return handle == nullptr ? nullptr : handle->control_hwnd;
}

extern "C" void* SAO_UI_CALL
sao_ui_overlay_host_dc_mutation_coordinator(sao_ui_overlay_host_handle_t handle) {
    return handle == nullptr ? nullptr : handle->dc_mutation;
}

extern "C" void* SAO_UI_CALL sao_ui_overlay_host_hglrc(sao_ui_overlay_host_handle_t) {
    return nullptr;
}
extern "C" void* SAO_UI_CALL sao_ui_overlay_host_hdc(sao_ui_overlay_host_handle_t) {
    return nullptr;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_bounds(
    sao_ui_overlay_host_handle_t handle, int32_t x, int32_t y, int32_t width, int32_t height) {
    if (handle == nullptr || handle->hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (width <= 0 || height <= 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!win32_api().set_window_pos(handle->hwnd, nullptr, x, y, width, height,
                                    SWP_NOACTIVATE | SWP_NOZORDER)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    publish_real_geometry(handle, {x, y, width, height});
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        handle->current_dpi = query_window_dpi(handle->hwnd);
    }
    (void)::DwmFlush();
    return submit_physical_rect_scrub(handle);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_get_desired_bounds(
    sao_ui_overlay_host_handle_t handle, SaoOverlayHostClientRect* out_rect) {
    if (out_rect != nullptr)
        std::memset(out_rect, 0, sizeof(*out_rect));
    if (handle == nullptr || handle->hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_rect == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    *out_rect = handle->desired_rect;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_set_visible(sao_ui_overlay_host_handle_t handle, bool visible) {
    if (handle == nullptr || handle->hwnd == nullptr || handle->control_hwnd == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (visible) {
        SaoOverlayHostClientRect desired{};
        {
            std::lock_guard<std::mutex> lock(handle->state_mu);
            desired = handle->desired_rect;
        }
        if (!win32_api().set_window_pos(handle->hwnd, nullptr, desired.x, desired.y, desired.width,
                                        desired.height, SWP_NOACTIVATE | SWP_NOZORDER)) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        publish_real_geometry(handle, desired);
    } else if (::GetCapture() == handle->hwnd) {
        dispatch_mouse(handle, WM_CANCELMODE, 0, 0);
        (void)::ReleaseCapture();
    }
    ::ShowWindow(handle->hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    ::ShowWindow(handle->control_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        handle->visible = visible;
    }
    if (!visible)
        return SAO_STATUS_OK;
    (void)::DwmFlush();
    return submit_physical_rect_scrub(handle);
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_require_owner_thread(sao_ui_overlay_host_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    return handle->owner_thread_id == ::GetCurrentThreadId()
               ? SAO_STATUS_OK
               : SAO_STATUS_ERR_ACCESS_DENIED;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_set_input_passthrough(sao_ui_overlay_host_handle_t handle, bool passthrough) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (handle->owner_thread_id != ::GetCurrentThreadId()) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::lock_guard<std::mutex> update_lock(handle->input_update_mu);
    return apply_passthrough_unlocked(handle, passthrough);
}

extern "C" bool SAO_UI_CALL
sao_ui_overlay_host_input_passthrough(sao_ui_overlay_host_handle_t handle) {
    if (handle == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return handle->input_passthrough;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_input_region(
    sao_ui_overlay_host_handle_t handle, const SaoOverlayHostInputRect* rects, size_t rect_count) {
    if (handle == nullptr || handle->hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (rect_count != 0 && rects == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (handle->owner_thread_id != ::GetCurrentThreadId()) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::lock_guard<std::mutex> update_lock(handle->input_update_mu);
    std::vector<SaoOverlayHostInputRect> current;
    current.reserve(rect_count);
    for (size_t index = 0; index < rect_count; ++index) {
        if (rects[index].width <= 0 || rects[index].height <= 0)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        current.push_back(rects[index]);
    }
    OwnedRegion region(::CreateRectRgn(0, 0, 0, 0));
    if (region.get() == nullptr)
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    std::vector<SaoOverlayHostInputRect> previous;
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        previous = handle->previous_input_rects;
    }
    for (const auto& rect : current) {
        if (!append_rect(region.get(), rect))
            return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    for (const auto& rect : previous) {
        if (!append_rect(region.get(), rect))
            return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    OwnedRegion rollback_region(::CreateRectRgn(0, 0, 0, 0));
    if (rollback_region.get() == nullptr)
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    const auto& api = win32_api();
    if (api.get_window_rgn(handle->hwnd, rollback_region.get()) == ERROR) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (!api.set_window_rgn(handle->hwnd, region.get(), TRUE)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    region.release();

    const bool passthrough = current.empty() && previous.empty();
    const sao_status_t passthrough_status = apply_passthrough_unlocked(handle, passthrough);
    if (passthrough_status != SAO_STATUS_OK) {
        if (!api.set_window_rgn(handle->hwnd, rollback_region.get(), TRUE)) {
            mark_input_partial(handle);
        } else {
            rollback_region.release();
        }
        return passthrough_status;
    }

    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        handle->previous_input_rects = std::move(current);
        handle->input_sync_state = SAO_UI_OVERLAY_INPUT_SYNCHRONIZED;
    }
    return SAO_STATUS_OK;
}

extern "C" uint32_t SAO_UI_CALL
sao_ui_overlay_host_input_sync_state(sao_ui_overlay_host_handle_t handle) {
    if (handle == nullptr)
        return SAO_UI_OVERLAY_INPUT_PARTIAL;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return handle->input_sync_state;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_set_capture_mode(sao_ui_overlay_host_handle_t handle, bool exclude) {
    if (handle == nullptr || handle->hwnd == nullptr || handle->control_hwnd == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
#if defined(SAO_UI_HAS_ANTI_SCREENCAP_FACADE)
    SaoAntiScreencapAffinityPair pair{};
    pair.primary_hwnd = handle->hwnd;
    pair.decoy_hwnd = handle->control_hwnd;
    uint32_t effective = SAO_ASC_MODE_INVALID;
    const int32_t rc = sao_security_anti_screencap_overlay_host_set_capture_mode(
        &pair, exclude ? SAO_ASC_MODE_EXCLUDED : SAO_ASC_MODE_NORMAL, &effective);
    if (rc != SAO_STATUS_OK) {
        return rc;
    }
    std::lock_guard<std::mutex> lock(handle->state_mu);
    // MONITORED is a compatibility fallback, not strict exclusion.  Do not
    // advertise the host as capture-excluded unless the facade applied an
    // actual exclusion mode.
    handle->capture_excluded =
        effective == SAO_ASC_MODE_STREAMING || effective == SAO_ASC_MODE_EXCLUDED;
    return SAO_STATUS_OK;
#else
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->capture_excluded = false;
    return SAO_STATUS_ERR_NOT_INITIALIZED;
#endif
}

extern "C" bool SAO_UI_CALL
sao_ui_overlay_host_capture_excluded(sao_ui_overlay_host_handle_t handle) {
    if (handle == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return handle->capture_excluded;
}

extern "C" bool SAO_UI_CALL sao_ui_overlay_host_visible(sao_ui_overlay_host_handle_t handle) {
    if (handle == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return handle->visible;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_get_state(sao_ui_overlay_host_handle_t handle, SaoOverlayHostState* out_state) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_state == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    out_state->geometry = handle->client_rect;
    out_state->dpi = handle->current_dpi;
    out_state->visible = handle->visible;
    out_state->input_passthrough = handle->input_passthrough;
    out_state->capture_excluded = handle->capture_excluded;
    out_state->_pad[0] = 0;
    return SAO_STATUS_OK;
}

// LEGACY classification: the WGL make-current / release-current /
// swap-buffers surface is intentionally stubbed on the D3D/DComp
// production compositor.  overlay_host.h line 213-214 declares them as
// "Legacy WGL operations ... return SAO_STATUS_ERR_NOT_IMPLEMENTED in
// the D3D/DComp production path".  Existing tests
// (test_overlay_host_main_chain.cpp line 176) explicitly assert this
// return code as a contract, so promoting to CAPABILITY_MISSING would
// silently break the contract.  See PLAN.md §1.5 for the taxonomy.
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_make_current(sao_ui_overlay_host_handle_t) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED; // legacy WGL stub
}
extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_release_current(sao_ui_overlay_host_handle_t) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED; // legacy WGL stub
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_swap_buffers(sao_ui_overlay_host_handle_t) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED; // legacy WGL stub
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_pump_messages(sao_ui_overlay_host_handle_t handle) {
    if (handle == nullptr || handle->hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    MSG message{};
    for (uint32_t count = 0; count < 128 && ::PeekMessageW(&message, handle->hwnd, 0, 0, PM_REMOVE);
         ++count) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_msg_wait(sao_ui_overlay_host_handle_t handle, uint32_t timeout_ms) {
    if (handle == nullptr || handle->hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    return ::MsgWaitForMultipleObjectsEx(0, nullptr, timeout_ms, QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE) == WAIT_FAILED
               ? SAO_STATUS_ERR_OS_CALL_FAILED
               : SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_hit_test(
    sao_ui_overlay_host_handle_t handle, sao_ui_hit_test_fn_t fn, void* user_data) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->hit_test_fn = fn;
    handle->hit_test_user = user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_mouse(
    sao_ui_overlay_host_handle_t handle, sao_ui_mouse_fn_t fn, void* user_data) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->mouse_fn = fn;
    handle->mouse_user = user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_get_client_rect(
    sao_ui_overlay_host_handle_t handle, SaoOverlayHostClientRect* out_rect) {
    if (out_rect != nullptr)
        std::memset(out_rect, 0, sizeof(*out_rect));
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_rect == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    *out_rect = handle->client_rect;
    return SAO_STATUS_OK;
}

extern "C" uint32_t SAO_UI_CALL
sao_ui_overlay_host_current_dpi(sao_ui_overlay_host_handle_t handle) {
    if (handle == nullptr)
        return 0;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return handle->current_dpi;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_get_wm_counters(
    sao_ui_overlay_host_handle_t handle, SaoOverlayHostWMCounters* out_counters) {
    if (out_counters != nullptr)
        std::memset(out_counters, 0, sizeof(*out_counters));
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_counters == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    *out_counters = handle->wm_counters;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_size_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_size_fn_t fn, void* user_data) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->size_fn = fn;
    handle->size_user = user_data;
    return SAO_STATUS_OK;
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_move_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_move_fn_t fn, void* user_data) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->move_fn = fn;
    handle->move_user = user_data;
    return SAO_STATUS_OK;
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_activate_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_activate_fn_t fn, void* user_data) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->activate_fn = fn;
    handle->activate_user = user_data;
    return SAO_STATUS_OK;
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_dpi_changed_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_dpi_changed_fn_t fn, void* user_data) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->dpi_changed_fn = fn;
    handle->dpi_changed_user = user_data;
    return SAO_STATUS_OK;
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_display_change_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_display_change_fn_t fn, void* user_data) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->display_change_fn = fn;
    handle->display_change_user = user_data;
    return SAO_STATUS_OK;
}

#if defined(SAO_UI_OVERLAY_HOST_TESTING)
namespace sao::ui::overlay_host_detail {

void set_win32_api_for_testing(const Win32Api* api) noexcept {
    win32_api_override().store(api, std::memory_order_release);
}

void reset_win32_api_for_testing() noexcept {
    win32_api_override().store(nullptr, std::memory_order_release);
}

} // namespace sao::ui::overlay_host_detail
#endif

#endif
