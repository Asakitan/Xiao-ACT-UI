#include "sao/ui/overlay_host.h"
#include "sao/ui/dc_mutation.h"
#include "overlay_host_internal.h"

#if !defined(SAO_UI_OVERLAY_HOST_TESTING) &&                                                       \
    __has_include("sao_security/anti_screencap/capture_mode.h") &&                                 \
                  __has_include("sao_security/anti_screencap/overlay_host_capture.h")
#define SAO_UI_HAS_ANTI_SCREENCAP_FACADE 1
#include "sao_security/anti_screencap/capture_mode.h"
#include "sao_security/anti_screencap/overlay_host_capture.h"
#endif

#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <limits>
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
    enum class lifecycle_state : uint32_t {
        active,
        retiring,
        retired,
    };

    std::mutex lifetime_mu;
    std::condition_variable lifetime_cv;
    std::recursive_mutex window_mu;
    lifecycle_state lifecycle = lifecycle_state::active;
    uint32_t active_api_leases = 0;
    uint32_t active_callback_entries = 0;
    DWORD destroy_thread_id = 0;

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
    std::recursive_mutex capture_mode_mu;
    std::mutex state_mu;
    bool visible = false;
    bool input_passthrough = true;
    bool activation_enabled = false;
    bool activation_sync_partial = false;
    uint32_t input_sync_state = SAO_UI_OVERLAY_INPUT_SYNCHRONIZED;
    bool window_region_applied = false;
    bool capture_excluded = false;
    bool protection_requested = false;
    sao_ui_overlay_protection_provider_fn_t protection_provider = nullptr;
    void* protection_provider_user_data = nullptr;
#if defined(SAO_UI_HAS_ANTI_SCREENCAP_FACADE)
    bool capture_pair_bound = false;
    SaoAntiScreencapAffinityPair bound_capture_pair{};
#endif
    std::vector<SaoOverlayHostInputRect> previous_input_rects;
    // Effective rect set most recently written via SetWindowRgn — the
    // current+previous union actually applied — so per-tick identical
    // submissions skip the Win32 region transaction entirely.
    std::vector<SaoOverlayHostInputRect> applied_input_rects;

    sao_ui_hit_test_fn_t hit_test_fn = nullptr;
    void* hit_test_user = nullptr;
    sao_ui_hit_test_fn_t activation_hit_test_fn = nullptr;
    void* activation_hit_test_user = nullptr;
    sao_ui_mouse_fn_t mouse_fn = nullptr;
    void* mouse_user = nullptr;

    SaoOverlayHostClientRect client_rect{};
    SaoOverlayHostClientRect desired_rect{};
    uint32_t current_dpi = 96;
    bool dpi_transition_active = false;
    bool dpi_pending_bounds = false;
    SaoOverlayHostClientRect dpi_pending_rect{};
    int32_t cursor_hint = 0;
    int32_t input_cursor_kind = 0;
    bool menu_cursor_active = false;
    HCURSOR app_cursor = nullptr;
    HCURSOR menu_cursor = nullptr;
    uint32_t cursor_dpi = 0;
    uint32_t captured_mouse_buttons = 0;
    uint64_t last_threat_scan_ms = 0u;
    uint64_t last_process_window_sweep_ms = 0u;
    SaoOverlayHostWMCounters wm_counters{};

    sao_ui_size_fn_t size_fn = nullptr;
    void* size_user = nullptr;
    sao_ui_move_fn_t move_fn = nullptr;
    void* move_user = nullptr;
    sao_ui_activate_fn_t activate_fn = nullptr;
    void* activate_user = nullptr;
    sao_ui_dpi_changed_fn_t dpi_changed_fn = nullptr;
    void* dpi_changed_user = nullptr;
    sao_ui_dpi_reflow_fn_t dpi_reflow_fn = nullptr;
    void* dpi_reflow_user = nullptr;
    sao_ui_display_change_fn_t display_change_fn = nullptr;
    void* display_change_user = nullptr;
};

namespace {

struct HostEntryDepthNode {
    sao_ui_overlay_host_s* host = nullptr;
    HostEntryDepthNode* previous = nullptr;
};

thread_local HostEntryDepthNode* current_thread_host_entries = nullptr;

void enter_host_entry(sao_ui_overlay_host_s* host, HostEntryDepthNode* node) noexcept {
    node->host = host;
    node->previous = current_thread_host_entries;
    current_thread_host_entries = node;
}

void leave_host_entry(HostEntryDepthNode* node) noexcept {
    HostEntryDepthNode** cursor = &current_thread_host_entries;
    while (*cursor != nullptr) {
        if (*cursor == node) {
            *cursor = node->previous;
            node->host = nullptr;
            node->previous = nullptr;
            return;
        }
        cursor = &(*cursor)->previous;
    }
}

uint32_t current_thread_host_entry_depth(sao_ui_overlay_host_s* host) noexcept {
    uint32_t depth = 0;
    for (HostEntryDepthNode* node = current_thread_host_entries; node != nullptr;
         node = node->previous) {
        if (node->host == host)
            ++depth;
    }
    return depth;
}

class HostLease {
  public:
    explicit HostLease(sao_ui_overlay_host_s* host) noexcept : host_(host) {
        if (host_ == nullptr)
            return;
        window_lock_ = std::unique_lock<std::recursive_mutex>(host_->window_mu);
        std::lock_guard<std::mutex> lock(host_->lifetime_mu);
        if (host_->lifecycle != sao_ui_overlay_host_s::lifecycle_state::active) {
            host_ = nullptr;
            return;
        }
        ++host_->active_api_leases;
        enter_host_entry(host_, &entry_);
    }

    ~HostLease() {
        release();
    }

    HostLease(const HostLease&) = delete;
    HostLease& operator=(const HostLease&) = delete;

    explicit operator bool() const noexcept {
        return host_ != nullptr;
    }

    sao_ui_overlay_host_s* get() const noexcept {
        return host_;
    }

  private:
    void release() noexcept {
        if (host_ == nullptr)
            return;
        leave_host_entry(&entry_);
        {
            std::lock_guard<std::mutex> lock(host_->lifetime_mu);
            --host_->active_api_leases;
        }
        host_->lifetime_cv.notify_all();
        host_ = nullptr;
    }

    HostEntryDepthNode entry_;
    std::unique_lock<std::recursive_mutex> window_lock_;
    sao_ui_overlay_host_s* host_ = nullptr;
};

class HostLeaseToken {
  public:
    explicit HostLeaseToken(sao_ui_overlay_host_s* host) noexcept : lease(host) {}

    HostLeaseToken(const HostLeaseToken&) = delete;
    HostLeaseToken& operator=(const HostLeaseToken&) = delete;

    HostLease lease;
};
class HostCallbackLease {
  public:
    explicit HostCallbackLease(sao_ui_overlay_host_s* host) noexcept : host_(host) {
        if (host_ == nullptr)
            return;
        std::lock_guard<std::mutex> lock(host_->lifetime_mu);
        if (host_->lifecycle != sao_ui_overlay_host_s::lifecycle_state::active) {
            host_ = nullptr;
            return;
        }
        ++host_->active_callback_entries;
        enter_host_entry(host_, &entry_);
    }

    ~HostCallbackLease() {
        release();
    }

    HostCallbackLease(const HostCallbackLease&) = delete;
    HostCallbackLease& operator=(const HostCallbackLease&) = delete;

    explicit operator bool() const noexcept {
        return host_ != nullptr;
    }

    sao_ui_overlay_host_s* get() const noexcept {
        return host_;
    }

  private:
    void release() noexcept {
        if (host_ == nullptr)
            return;
        leave_host_entry(&entry_);
        {
            std::lock_guard<std::mutex> lock(host_->lifetime_mu);
            --host_->active_callback_entries;
        }
        host_->lifetime_cv.notify_all();
        host_ = nullptr;
    }

    HostEntryDepthNode entry_;
    sao_ui_overlay_host_s* host_ = nullptr;
};

#if !defined(SAO_UI_OVERLAY_HOST_TESTING)
constexpr wchar_t kSingleInstanceMutexName[] =
    L"Local\\{7F3E2A91-4C8B-4D6E-9A15-8B2F0C4E3D7A}";
#endif
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

LPCWSTR cursor_id_for_hint(int32_t hint) noexcept {
    switch (hint) {
    case 1: return IDC_SIZENS;
    case 2: return IDC_SIZEWE;
    case 3: return IDC_SIZENWSE;
    case 4: return IDC_SIZENESW;
    default: return IDC_ARROW;
    }
}

struct CursorPoint { double x, y; };

bool inside_path(double x, double y, const CursorPoint* points, size_t count) noexcept {
    bool inside = false;
    for (size_t i = 0, j = count - 1; i < count; j = i++) {
        const auto& a = points[i];
        const auto& b = points[j];
        if ((a.y > y) != (b.y > y) &&
            x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

double segment_distance(double x, double y, CursorPoint a, CursorPoint b) noexcept {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double length2 = dx * dx + dy * dy;
    const double t = length2 == 0 ? 0 : std::clamp(((x - a.x) * dx + (y - a.y) * dy) / length2, 0.0, 1.0);
    return std::hypot(x - a.x - t * dx, y - a.y - t * dy);
}

template <typename Predicate>
void paint_cursor(uint32_t* pixels, int size, uint32_t color, Predicate&& contains) noexcept {
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int covered = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    covered += contains((x + (sx + 0.5) / 4.0) * 44.0 / size,
                                        (y + (sy + 0.5) / 4.0) * 44.0 / size) ? 1 : 0;
            const uint32_t a = ((color >> 24) * covered + 8) / 16;
            if (a == 0) continue;
            uint32_t& dst = pixels[y * size + x];
            const uint32_t old_a = dst >> 24;
            const uint32_t inv = 255 - a;
            const auto channel = [&](int shift) {
                return ((color >> shift & 255) * a + (dst >> shift & 255) * inv + 127) / 255;
            };
            dst = ((a + (old_a * inv + 127) / 255) << 24) |
                  (channel(16) << 16) | (channel(8) << 8) | channel(0);
        }
    }
}

HCURSOR make_themed_cursor(bool menu, uint32_t dpi) noexcept {
    const int size = std::clamp(static_cast<int>((44u * std::clamp(dpi, 96u, 192u) + 48u) / 96u), 44, 88);
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = ::GetDC(nullptr);
    HBITMAP color = dc == nullptr ? nullptr : ::CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dc != nullptr) ::ReleaseDC(nullptr, dc);
    if (color == nullptr || bits == nullptr) {
        if (color != nullptr) ::DeleteObject(color);
        return nullptr;
    }
    auto* pixels = static_cast<uint32_t*>(bits);
    std::fill_n(pixels, size * size, 0u);
    if (menu) {
        paint_cursor(pixels, size, 0xB00B657Fu, [](double x, double y) {
            return std::abs(std::hypot((x - 21) / 14.6, (y - 17) / 6.0) - 1) < 0.15;
        });
        paint_cursor(pixels, size, 0xF047D7FFu, [](double x, double y) {
            return std::abs(std::hypot((x - 21) / 13.2, (y - 17) / 5.0) - 1) < 0.105;
        });
        paint_cursor(pixels, size, 0xFFB9F0FFu, [](double x, double y) {
            return std::hypot(x - 34.6, y - 17) < 1.9 || std::hypot(x - 21, y - 17) < 2.2;
        });
    } else {
        paint_cursor(pixels, size, 0xA00B657Fu, [](double x, double y) {
            return std::abs(std::hypot(x - 21, y - 24) - 9.0) < 0.9 && x > 14;
        });
        paint_cursor(pixels, size, 0xFF47D7FFu, [](double x, double y) {
            return (std::abs(x - 21) < 0.8 && y > 30 && y < 37) ||
                   (std::abs(y - 24) < 0.8 && x > 29 && x < 36);
        });
    }
    constexpr std::array<CursorPoint, 7> menu_arrow{{{5.6, 4.4}, {23.4, 14.9}, {17.9, 16.6},
                                                      {24.2, 24.6}, {19.4, 27}, {7, 12.6}, {5.6, 4.4}}};
    constexpr std::array<CursorPoint, 7> app_arrow{{{4, 3}, {23, 15}, {16, 16},
                                                     {23, 28}, {18, 30}, {10, 18}, {4, 3}}};
    const auto& arrow = menu ? menu_arrow : app_arrow;
    paint_cursor(pixels, size, 0xFF081D29u, [&](double x, double y) {
        for (size_t i = 1; i < arrow.size(); ++i)
            if (segment_distance(x, y, arrow[i - 1], arrow[i]) < 1.6) return true;
        return false;
    });
    paint_cursor(pixels, size, menu ? 0xFF47D7FFu : 0xFF233A48u, [&](double x, double y) {
        return inside_path(x, y, arrow.data(), arrow.size() - 1);
    });
    paint_cursor(pixels, size, menu ? 0xFFFFFFFFu : 0xFFB9F0FFu, [&](double x, double y) {
        for (size_t i = 1; i < arrow.size(); ++i)
            if (segment_distance(x, y, arrow[i - 1], arrow[i]) < 0.6) return true;
        return false;
    });
    paint_cursor(pixels, size, menu ? 0xEFFFFFFFu : 0xFF47D7FFu, [&](double x, double y) {
        return segment_distance(x, y, arrow[0], arrow[1]) < 0.75 ||
               segment_distance(x, y, arrow[3], arrow[4]) < 0.8;
    });
    HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);
    if (mask != nullptr) {
        HDC mask_dc = ::CreateCompatibleDC(nullptr);
        if (mask_dc != nullptr) {
            HGDIOBJ old = ::SelectObject(mask_dc, mask);
            ::PatBlt(mask_dc, 0, 0, size, size, BLACKNESS);
            ::SelectObject(mask_dc, old);
            ::DeleteDC(mask_dc);
        } else { ::DeleteObject(mask); mask = nullptr; }
    }
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = static_cast<DWORD>(size * (menu ? 5.6 : 4.0) / 44.0);
    info.yHotspot = static_cast<DWORD>(size * (menu ? 4.4 : 3.0) / 44.0);
    info.hbmColor = color;
    info.hbmMask = mask;
    HCURSOR cursor = mask == nullptr ? nullptr : static_cast<HCURSOR>(::CreateIconIndirect(&info));
    if (mask != nullptr) ::DeleteObject(mask);
    ::DeleteObject(color);
    return cursor;
}

void release_themed_cursors(sao_ui_overlay_host_s* host) noexcept {
    if ((host->app_cursor != nullptr && ::GetCursor() == host->app_cursor) ||
        (host->menu_cursor != nullptr && ::GetCursor() == host->menu_cursor))
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
    if (host->app_cursor != nullptr) ::DestroyCursor(host->app_cursor);
    if (host->menu_cursor != nullptr) ::DestroyCursor(host->menu_cursor);
    host->app_cursor = host->menu_cursor = nullptr;
    host->cursor_dpi = 0;
}

bool cursor_over_host(sao_ui_overlay_host_s* host) noexcept {
    POINT point{};
    bool visible = false;
    {
        std::lock_guard<std::mutex> lock(host->state_mu);
        visible = host->visible;
    }
    return host->hwnd != nullptr && visible &&
           ((::GetCursorPos(&point) && ::WindowFromPoint(point) == host->hwnd) ||
            ::GetCapture() == host->hwnd);
}

void apply_cursor_hint(sao_ui_overlay_host_s* host, bool force = false) noexcept {
    if (host == nullptr || host->owner_thread_id != ::GetCurrentThreadId() ||
        (!force && !cursor_over_host(host))) return;
    int32_t hint = 0, kind = 0;
    bool menu = false;
    uint32_t dpi = 96;
    {
        std::lock_guard<std::mutex> lock(host->state_mu);
        hint = host->cursor_hint;
        kind = host->input_cursor_kind;
        menu = host->menu_cursor_active;
        dpi = host->current_dpi;
    }
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool accessible = !::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) ||
                            (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    LPCWSTR system_id = cursor_id_for_hint(hint);
    if (hint == 0) {
        switch (kind) {
        case 1: system_id = IDC_HAND; break;
        case 2: system_id = IDC_IBEAM; break;
        case 3: system_id = IDC_SIZENS; break;
        case 4: system_id = IDC_SIZEWE; break;
        case 5: system_id = IDC_SIZENWSE; break;
        case 6: system_id = IDC_SIZENESW; break;
        case 7: system_id = IDC_CROSS; break;
        case 8: system_id = IDC_WAIT; break;
        default: break;
        }
    }
    if (kind == 9 && hint == 0) {
        ::SetCursor(nullptr);
        return;
    }
    HCURSOR custom = nullptr;
    if (!accessible && hint == 0 && (kind == 0 || (menu && kind == 1))) {
        if (host->cursor_dpi != dpi || host->app_cursor == nullptr || host->menu_cursor == nullptr) {
            HCURSOR app = make_themed_cursor(false, dpi);
            HCURSOR overlay = make_themed_cursor(true, dpi);
            if (app != nullptr && overlay != nullptr) {
                release_themed_cursors(host);
                host->app_cursor = app;
                host->menu_cursor = overlay;
                host->cursor_dpi = dpi;
            } else {
                if (app != nullptr) ::DestroyCursor(app);
                if (overlay != nullptr) ::DestroyCursor(overlay);
            }
        }
        custom = menu ? host->menu_cursor : host->app_cursor;
    }
    ::SetCursor(custom != nullptr ? custom : ::LoadCursorW(nullptr, system_id));
}
void mark_input_partial(sao_ui_overlay_host_s* host) {
    std::lock_guard<std::mutex> lock(host->state_mu);
    host->input_sync_state = SAO_UI_OVERLAY_INPUT_PARTIAL;
}

sao_status_t last_win32_status() {
    return ::GetLastError() == ERROR_ACCESS_DENIED ? SAO_STATUS_ERR_ACCESS_DENIED
                                                   : SAO_STATUS_ERR_OS_CALL_FAILED;
}

void publish_real_geometry(sao_ui_overlay_host_s* host, const SaoOverlayHostClientRect& geometry) {
    std::lock_guard<std::mutex> lock(host->state_mu);
    host->client_rect = geometry;
    host->desired_rect = geometry;
}

sao_status_t drain_pending_dpi_bounds(sao_ui_overlay_host_s* host) noexcept {
    SaoOverlayHostClientRect pending{};
    {
        std::lock_guard<std::mutex> lock(host->state_mu);
        if (!host->dpi_pending_bounds) return SAO_STATUS_OK;
        if (host->dpi_transition_active) return SAO_UI_STATUS_ERR_BUSY;
        pending = host->dpi_pending_rect;
    }
    const sao_status_t status = sao_ui_overlay_host_set_bounds(
        host, pending.x, pending.y, pending.width, pending.height);
    if (status != SAO_STATUS_OK) return status;
    std::lock_guard<std::mutex> lock(host->state_mu);
    if (host->dpi_pending_bounds &&
        host->dpi_pending_rect.x == pending.x &&
        host->dpi_pending_rect.y == pending.y &&
        host->dpi_pending_rect.width == pending.width &&
        host->dpi_pending_rect.height == pending.height) {
        host->dpi_pending_rect = {};
        host->dpi_pending_bounds = false;
    }
    return SAO_STATUS_OK;
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

sao_status_t apply_activation_unlocked(sao_ui_overlay_host_s* host, bool enabled) {
    if (host == nullptr || host->hwnd == nullptr || !::IsWindow(host->hwnd))
        return SAO_STATUS_ERR_HANDLE_INVALID;
    const auto& api = win32_api();
    const auto publish_state = [&](LONG_PTR style, bool partial) {
        std::lock_guard<std::mutex> lock(host->state_mu);
        host->activation_enabled = (style & WS_EX_NOACTIVATE) == 0;
        host->activation_sync_partial = partial;
    };
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR old_style = api.get_window_long_ptr_w(host->hwnd, GWL_EXSTYLE);
    if (old_style == 0 && ::GetLastError() != ERROR_SUCCESS) {
        const sao_status_t status = last_win32_status();
        std::lock_guard<std::mutex> lock(host->state_mu);
        host->activation_sync_partial = true;
        return status;
    }
    const auto rollback = [&](sao_status_t failure) {
        const bool restored = restore_passthrough_style(host, old_style);
        publish_state(old_style, !restored);
        return restored ? failure : SAO_STATUS_ERR_OS_CALL_FAILED;
    };
    const LONG_PTR new_style = enabled
                                   ? old_style & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE)
                                   : old_style | WS_EX_NOACTIVATE;
    if (new_style != old_style) {
        ::SetLastError(ERROR_SUCCESS);
        const LONG_PTR previous = api.set_window_long_ptr_w(host->hwnd, GWL_EXSTYLE, new_style);
        if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
            const sao_status_t status = last_win32_status();
            publish_state(old_style, false);
            return status;
        }
        if (!api.set_window_pos(host->hwnd, nullptr, 0, 0, 0, 0,
                                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                                    SWP_FRAMECHANGED)) {
            return rollback(SAO_STATUS_ERR_OS_CALL_FAILED);
        }
    }
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR readback = api.get_window_long_ptr_w(host->hwnd, GWL_EXSTYLE);
    if (readback == 0 && ::GetLastError() != ERROR_SUCCESS) {
        const sao_status_t status = last_win32_status();
        if (new_style != old_style)
            return rollback(status);
        publish_state(old_style, true);
        return status;
    }
    if (((readback & WS_EX_NOACTIVATE) == 0) != enabled) {
        if (new_style != old_style)
            return rollback(SAO_STATUS_ERR_ACCESS_DENIED);
        publish_state(readback, true);
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    publish_state(readback, false);
    return SAO_STATUS_OK;
}

bool input_rect_has_valid_bounds(const SaoOverlayHostInputRect& rect) noexcept {
    const int64_t right = static_cast<int64_t>(rect.x) + rect.width;
    const int64_t bottom = static_cast<int64_t>(rect.y) + rect.height;
    return rect.x != INT32_MIN && rect.y != INT32_MIN && rect.width > 0 && rect.height > 0 &&
           right > INT32_MIN && bottom > INT32_MIN && right <= INT32_MAX &&
           bottom <= INT32_MAX;
}

bool append_rect(HRGN destination, const SaoOverlayHostInputRect& rect) {
    if (!input_rect_has_valid_bounds(rect))
        return false;
    const int64_t right = static_cast<int64_t>(rect.x) + rect.width;
    const int64_t bottom = static_cast<int64_t>(rect.y) + rect.height;
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

uint32_t mouse_button_bit(UINT message, WPARAM wparam) noexcept {
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
        message == WM_LBUTTONDBLCLK) {
        return MK_LBUTTON;
    }
    if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
        message == WM_RBUTTONDBLCLK) {
        return MK_RBUTTON;
    }
    if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ||
        message == WM_MBUTTONDBLCLK) {
        return MK_MBUTTON;
    }
    if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
        message == WM_XBUTTONDBLCLK) {
        return GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? MK_XBUTTON1 : MK_XBUTTON2;
    }
    return 0;
}

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
    } else if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
               message == WM_XBUTTONDBLCLK) {
        button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4;
    }
    callback(message, point.x, point.y, button,
             message == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(wparam) : 0, user_data);
}

#if defined(SAO_UI_HAS_ANTI_SCREENCAP_FACADE)
sao_status_t map_anti_screencap_status(int32_t status) noexcept {
    if (status == SAO_ASC_APPLY_STATE_INDETERMINATE) {
        // The anti layer uses -10 for indeterminate; platform UNKNOWN keeps
        // that state distinct from the platform capability-missing code.
        return SAO_STATUS_ERR_UNKNOWN;
    }
    // Input space is the canonical sao_status_e numeric namespace (via the
    // SaoStatus compat spellings): -5 is NOT_IMPLEMENTED, -6 UNKNOWN, -7/-8/-9
    // remain TIMEOUT/CANCELLED/ABI_MISMATCH domain codes in the anti layer's
    // own API and are mapped to their same-valued canonical codes.
    switch (status) {
    case 0: return SAO_STATUS_OK;
    case -1: return SAO_STATUS_ERR_INVALID_ARGUMENT;
    case -2: return SAO_STATUS_ERR_NOT_INITIALIZED;
    case -3: return SAO_STATUS_ERR_HANDLE_INVALID;
    case -4: return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    case -5: return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    case -6: return SAO_STATUS_ERR_UNKNOWN;
    case -7: return SAO_STATUS_ERR_TIMEOUT;
    case -8: return SAO_STATUS_ERR_CANCELLED;
    case -9: return SAO_STATUS_ERR_ABI_MISMATCH;
    case -20: return SAO_STATUS_ERR_OS_CALL_FAILED;
    case -21: return SAO_STATUS_ERR_ACCESS_DENIED;
    case -22: return SAO_STATUS_ERR_NOT_FOUND;
    case -41: return SAO_STATUS_ERR_READ_FAULT;
    case SAO_ASC_APPLY_POLICY_DISABLED: return SAO_STATUS_ERR_CAPABILITY_MISSING;
    default: return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t unbind_host_capture_pair(sao_ui_overlay_host_s* host) noexcept;
sao_status_t unregister_host_threat_windows(sao_ui_overlay_host_s* host) noexcept;
#endif

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        if (create != nullptr) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
    }
    auto* host = reinterpret_cast<sao_ui_overlay_host_s*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCDESTROY && host != nullptr) {
        bool externally_destroyed = false;
        {
            std::lock_guard<std::recursive_mutex> window_lock(host->window_mu);
            if (host->hwnd == hwnd &&
                ((host->app_cursor != nullptr && ::GetCursor() == host->app_cursor) ||
                 (host->menu_cursor != nullptr && ::GetCursor() == host->menu_cursor)))
                ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
            std::lock_guard<std::mutex> lock(host->lifetime_mu);
            externally_destroyed =
                host->lifecycle == sao_ui_overlay_host_s::lifecycle_state::active;
            if (host->hwnd == hwnd)
                host->hwnd = nullptr;
            if (host->control_hwnd == hwnd)
                host->control_hwnd = nullptr;
            if (host->owner_hwnd == hwnd)
                host->owner_hwnd = nullptr;
        }
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
#if defined(SAO_UI_HAS_ANTI_SCREENCAP_FACADE)
        if (externally_destroyed) {
            (void)unbind_host_capture_pair(host);
            (void)unregister_host_threat_windows(host);
        }
#endif
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }
    std::unique_lock<std::recursive_mutex> window_lock;
    if (host != nullptr)
        window_lock = std::unique_lock<std::recursive_mutex>(host->window_mu);
    HostCallbackLease callback_lease(host);
    host = callback_lease.get();
    const bool is_render_host = host != nullptr && host->hwnd == hwnd;

    if (message == WM_MOUSEACTIVATE) {
        bool activation_enabled = false;
        sao_ui_hit_test_fn_t activation_hit_test = nullptr;
        void* activation_user = nullptr;
        if (is_render_host) {
            std::lock_guard<std::mutex> lock(host->state_mu);
            activation_enabled = host->activation_enabled;
            activation_hit_test = host->activation_hit_test_fn;
            activation_user = host->activation_hit_test_user;
        }
        if (!activation_enabled || activation_hit_test == nullptr)
            return MA_NOACTIVATE;
        POINT point{};
        if (!::GetCursorPos(&point))
            return MA_NOACTIVATE;
        try {
            return activation_hit_test(point.x, point.y, activation_user) ? MA_ACTIVATE
                                                                          : MA_NOACTIVATE;
        } catch (...) {
            return MA_NOACTIVATE;
        }
    }
    if (message == WM_ERASEBKGND)
        return 1;
    if (!is_render_host) {
        if (message == WM_NCDESTROY)
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    if (message == WM_SETCURSOR) {
        if (LOWORD(lparam) != HTCLIENT)
            return ::DefWindowProcW(hwnd, message, wparam, lparam);
        apply_cursor_hint(host, true);
        return TRUE;
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
        if (callback == nullptr)
            return HTCLIENT;
        try {
            return callback(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), user_data)
                       ? HTCLIENT
                       : HTTRANSPARENT;
        } catch (...) {
            return HTTRANSPARENT;
        }
    }

    switch (message) {
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
        apply_cursor_hint(host, true);
        dispatch_mouse(host, message, wparam, lparam);
        return 0;
    }
    case WM_MOUSELEAVE:
        dispatch_mouse(host, message, wparam, lparam);
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            host->cursor_hint = 0;
        }
        if (::GetCapture() != hwnd &&
            ((host->app_cursor != nullptr && ::GetCursor() == host->app_cursor) ||
             (host->menu_cursor != nullptr && ::GetCursor() == host->menu_cursor)))
            ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        apply_cursor_hint(host);
        break;
    case WM_MOUSEWHEEL:
        dispatch_mouse(host, message, wparam, lparam);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDBLCLK:
        host->captured_mouse_buttons |= mouse_button_bit(message, wparam);
        ::SetCapture(hwnd);
        dispatch_mouse(host, message, wparam, lparam);
        return message == WM_XBUTTONDOWN || message == WM_XBUTTONDBLCLK ? TRUE : 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
        dispatch_mouse(host, message, wparam, lparam);
        host->captured_mouse_buttons &= ~mouse_button_bit(message, wparam);
        if (host->captured_mouse_buttons == 0 && ::GetCapture() == hwnd)
            ::ReleaseCapture();
        return message == WM_XBUTTONUP ? TRUE : 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lparam) != hwnd) {
            host->captured_mouse_buttons = 0;
            dispatch_mouse(host, message, wparam, lparam);
        }
        return 0;
    case WM_KILLFOCUS:
    case WM_ACTIVATEAPP:
        if (message == WM_ACTIVATEAPP && wparam != 0)
            break;
        host->captured_mouse_buttons = 0;
        dispatch_mouse(host, WM_CANCELMODE, 0, 0);
        if (::GetCapture() == hwnd)
            ::ReleaseCapture();
        break;
    case WM_CANCELMODE:
        host->captured_mouse_buttons = 0;
        dispatch_mouse(host, message, wparam, lparam);
        if (::GetCapture() == hwnd)
            ::ReleaseCapture();
        return 0;
    case WM_SIZE: {
        RECT client{};
        if (!::GetClientRect(hwnd, &client))
            return 0;
        const int64_t width_value = static_cast<int64_t>(client.right) - client.left;
        const int64_t height_value = static_cast<int64_t>(client.bottom) - client.top;
        const int32_t width =
            width_value <= 0
                ? 0
                : width_value > std::numeric_limits<int32_t>::max()
                      ? std::numeric_limits<int32_t>::max()
                      : static_cast<int32_t>(width_value);
        const int32_t height =
            height_value <= 0
                ? 0
                : height_value > std::numeric_limits<int32_t>::max()
                      ? std::numeric_limits<int32_t>::max()
                      : static_cast<int32_t>(height_value);
        sao_ui_size_fn_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            if (host->dpi_transition_active) return 0;
            host->client_rect.width = width;
            host->client_rect.height = height;
            ++host->wm_counters.size_events;
            callback = host->size_fn;
            user_data = host->size_user;
        }
        if (callback != nullptr) {
            try {
                callback(width, height, user_data);
            } catch (...) {
            }
        }
        return 0;
    }
    case WM_MOVE: {
        sao_ui_move_fn_t callback = nullptr;
        void* user_data = nullptr;
        const int32_t x = GET_X_LPARAM(lparam);
        const int32_t y = GET_Y_LPARAM(lparam);
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            if (host->dpi_transition_active) return 0;
            host->client_rect.x = x;
            host->client_rect.y = y;
            ++host->wm_counters.move_events;
            callback = host->move_fn;
            user_data = host->move_user;
        }
        if (callback != nullptr) {
            try {
                callback(x, y, user_data);
            } catch (...) {
            }
        }
        return 0;
    }
    case WM_ACTIVATE: {
        sao_ui_activate_fn_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            if (host->dpi_transition_active) return 0;
            ++host->wm_counters.activate_events;
            callback = host->activate_fn;
            user_data = host->activate_user;
        }
        if (callback != nullptr) {
            try {
                callback(LOWORD(wparam) != WA_INACTIVE, user_data);
            } catch (...) {
            }
        }
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
        if (suggested == nullptr) return 0;
        const int32_t x = suggested->left;
        const int32_t y = suggested->top;
        const int32_t width = suggested->right - suggested->left;
        const int32_t height = suggested->bottom - suggested->top;
        if (width <= 0 || height <= 0) return 0;
        const uint32_t new_dpi = LOWORD(wparam) == 0u ? 96u : LOWORD(wparam);
        const float new_scale = static_cast<float>(new_dpi) / 96.0F;

        uint32_t old_dpi = 96u;
        SaoOverlayHostClientRect old_client{};
        SaoOverlayHostClientRect old_desired{};
        sao_ui_dpi_reflow_fn_t reflow = nullptr;
        void* reflow_user = nullptr;
        sao_ui_dpi_changed_fn_t callback = nullptr;
        void* callback_user = nullptr;
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            if (host->dpi_transition_active) return 0;
            old_dpi = host->current_dpi == 0u ? 96u : host->current_dpi;
            old_client = host->client_rect;
            old_desired = host->desired_rect;
            reflow = host->dpi_reflow_fn;
            reflow_user = host->dpi_reflow_user;
            callback = host->dpi_changed_fn;
            callback_user = host->dpi_changed_user;
            host->dpi_transition_active = true;
        }

        bool reflow_prepared = false;
        auto rollback_prepare = [&]() noexcept {
            if (reflow_prepared && reflow != nullptr) {
                try {
                    (void)reflow(new_dpi, old_dpi,
                                 static_cast<float>(old_dpi) / 96.0F, reflow_user);
                } catch (...) {
                }
            }
            {
                std::lock_guard<std::mutex> lock(host->state_mu);
                host->client_rect = old_client;
                host->desired_rect = old_desired;
                host->current_dpi = old_dpi;
                host->dpi_transition_active = false;
            }
            (void)drain_pending_dpi_bounds(host);
        };

        if (reflow != nullptr) {
            sao_status_t reflow_status = SAO_STATUS_OK;
            reflow_prepared = true;
            try {
                reflow_status = reflow(old_dpi, new_dpi, new_scale, reflow_user);
            } catch (...) {
                reflow_status = SAO_STATUS_ERR_UNKNOWN;
            }
            if (reflow_status != SAO_STATUS_OK) {
                rollback_prepare();
                return 0;
            }
        }

        if (win32_api().set_window_pos(hwnd, nullptr, x, y, width, height,
                                       SWP_NOACTIVATE | SWP_NOZORDER) == FALSE) {
            rollback_prepare();
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            host->client_rect = {x, y, width, height};
            host->desired_rect = {x, y, width, height};
            host->current_dpi = new_dpi;
            ++host->wm_counters.dpichanged_events;
        }
        apply_cursor_hint(host);
        (void)::DwmFlush();
        if (callback != nullptr) {
            try {
                callback(new_dpi, x, y, width, height, callback_user);
            } catch (...) {
            }
        }
        {
            std::lock_guard<std::mutex> lock(host->state_mu);
            host->dpi_transition_active = false;
        }
        (void)drain_pending_dpi_bounds(host);
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
            try {
                callback(static_cast<uint32_t>(wparam), LOWORD(lparam), HIWORD(lparam), user_data);
            } catch (...) {
            }
        }
        return 0;
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

bool destroy_created_window(HWND& window) {
    if (window == nullptr)
        return true;
    const HWND candidate = window;
    if (::DestroyWindow(candidate) || !::IsWindow(candidate)) {
        window = nullptr;
        return true;
    }
    return false;
}

bool destroy_created_host(sao_ui_overlay_host_s* host) {
    (void)destroy_created_window(host->hwnd);
    (void)destroy_created_window(host->control_hwnd);
    (void)destroy_created_window(host->owner_hwnd);
    if (host->hwnd != nullptr && !::IsWindow(host->hwnd))
        host->hwnd = nullptr;
    if (host->control_hwnd != nullptr && !::IsWindow(host->control_hwnd))
        host->control_hwnd = nullptr;
    if (host->owner_hwnd != nullptr && !::IsWindow(host->owner_hwnd))
        host->owner_hwnd = nullptr;
    if (host->hwnd != nullptr || host->control_hwnd != nullptr || host->owner_hwnd != nullptr)
        return false;
    if (host->class_atom != 0) {
        if (!::UnregisterClassW(host->class_name.c_str(), host->hinstance))
            return false;
        host->class_atom = 0;
    }
    release_themed_cursors(host);
    delete host;
    return true;
}

#if defined(SAO_UI_HAS_ANTI_SCREENCAP_FACADE)
SaoAntiScreencapAffinityPair capture_topology(sao_ui_overlay_host_s* host) {
    SaoAntiScreencapAffinityPair pair{};
    pair.primary_hwnd = host->hwnd;
    pair.decoy_hwnd = host->control_hwnd;
    pair.owner_hwnd = host->owner_hwnd;
    return pair;
}

void mark_host_capture_pair_bound(sao_ui_overlay_host_s* host,
                                  const SaoAntiScreencapAffinityPair& pair) noexcept {
    std::lock_guard<std::mutex> lock(host->state_mu);
    host->bound_capture_pair = pair;
    host->capture_pair_bound = true;
}

sao_status_t unbind_host_capture_pair(sao_ui_overlay_host_s* host) noexcept {
    std::lock_guard<std::recursive_mutex> transaction_lock(host->capture_mode_mu);
    SaoAntiScreencapAffinityPair pair{};
    {
        std::lock_guard<std::mutex> lock(host->state_mu);
        if (!host->capture_pair_bound)
            return SAO_STATUS_OK;
        pair = host->bound_capture_pair;
    }
    const sao_status_t status = map_anti_screencap_status(
        sao_security_anti_screencap_overlay_host_unbind_if_matches(&pair));
    if (status != SAO_STATUS_OK) {
        const bool window_destroyed =
            (pair.primary_hwnd == nullptr ||
             !::IsWindow(static_cast<HWND>(pair.primary_hwnd))) ||
            (pair.decoy_hwnd == nullptr ||
             !::IsWindow(static_cast<HWND>(pair.decoy_hwnd))) ||
            (pair.owner_hwnd == nullptr ||
             !::IsWindow(static_cast<HWND>(pair.owner_hwnd)));
        if (!window_destroyed)
            return status;
    }
    std::lock_guard<std::mutex> lock(host->state_mu);
    if (host->capture_pair_bound &&
        host->bound_capture_pair.primary_hwnd == pair.primary_hwnd &&
        host->bound_capture_pair.decoy_hwnd == pair.decoy_hwnd &&
        host->bound_capture_pair.owner_hwnd == pair.owner_hwnd) {
        host->bound_capture_pair = {};
        host->capture_pair_bound = false;
        host->capture_excluded = false;
        host->protection_requested = false;
    }
    return SAO_STATUS_OK;
}

sao_status_t unregister_host_threat_windows(sao_ui_overlay_host_s* host) noexcept {
    SaoAntiScreencapAffinityPair pair{};
    {
        std::lock_guard<std::mutex> lock(host->state_mu);
        pair = host->capture_pair_bound ? host->bound_capture_pair : capture_topology(host);
    }
    void* windows[] = {pair.primary_hwnd, pair.decoy_hwnd, pair.owner_hwnd,
                       host->hwnd, host->control_hwnd, host->owner_hwnd};
    sao_status_t first_error = SAO_STATUS_OK;
    constexpr size_t window_count = sizeof(windows) / sizeof(windows[0]);
    for (size_t index = 0; index < window_count; ++index) {
        if (windows[index] == nullptr)
            continue;
        bool duplicate = false;
        for (size_t prior = 0; prior < index; ++prior) {
            if (windows[prior] == windows[index]) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            const sao_status_t status = map_anti_screencap_status(
                sao_security_anti_screencap_unregister_window(windows[index]));
            if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_HANDLE_INVALID &&
                first_error == SAO_STATUS_OK) {
                first_error = status;
            }
        }
    }
    return first_error;
}
#endif

sao_status_t rollback_created_host(sao_ui_overlay_host_s* host) {
    if (host == nullptr) return SAO_STATUS_OK;
    sao_status_t first_error = SAO_STATUS_OK;
#if defined(SAO_UI_HAS_ANTI_SCREENCAP_FACADE)
    const sao_status_t unbind_status = unbind_host_capture_pair(host);
    if (unbind_status != SAO_STATUS_OK && first_error == SAO_STATUS_OK)
        first_error = unbind_status;
    const sao_status_t unregister_status = unregister_host_threat_windows(host);
    if (unregister_status != SAO_STATUS_OK && first_error == SAO_STATUS_OK)
        first_error = unregister_status;
#endif
    if (host->dc_mutation != nullptr && host->hwnd != nullptr &&
        !sao_ui_dc_mutation_coordinator_invalidate(host->dc_mutation, host->hwnd, 1.0)) {
        if (first_error == SAO_STATUS_OK) first_error = SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (first_error != SAO_STATUS_OK) return first_error;
    if (!destroy_created_host(host)) return SAO_STATUS_ERR_OS_CALL_FAILED;
    release_single_instance_lock();
    return SAO_STATUS_OK;
}

class OverlayHostCreateGuard {
  public:
    explicit OverlayHostCreateGuard(bool lock_owned = false) noexcept
        : lock_owned_(lock_owned) {}
    ~OverlayHostCreateGuard() {
        if (host_ != nullptr) {
            if (rollback_created_host(host_) == SAO_STATUS_OK) {
                host_ = nullptr;
                lock_owned_ = false;
            }
        } else if (lock_owned_) {
            release_single_instance_lock();
        }
    }
    OverlayHostCreateGuard(const OverlayHostCreateGuard&) = delete;
    OverlayHostCreateGuard& operator=(const OverlayHostCreateGuard&) = delete;
    void adopt(sao_ui_overlay_host_s* host) noexcept { host_ = host; }
    sao_status_t fail(sao_status_t original,
                      sao_ui_overlay_host_handle_t* out_handle) {
        if (host_ == nullptr) {
            if (lock_owned_) release_single_instance_lock();
            lock_owned_ = false;
            return original;
        }
        const sao_status_t cleanup = rollback_created_host(host_);
        if (cleanup == SAO_STATUS_OK) {
            host_ = nullptr;
            lock_owned_ = false;
            return original;
        }
        if (out_handle != nullptr) *out_handle = host_;
        host_ = nullptr;
        lock_owned_ = false;
        return cleanup;
    }
    void commit(sao_ui_overlay_host_handle_t* out_handle) noexcept {
        if (out_handle != nullptr) *out_handle = host_;
        host_ = nullptr;
        lock_owned_ = false;
    }
  private:
    sao_ui_overlay_host_s* host_ = nullptr;
    bool lock_owned_ = false;
};
thread_local sao_ui_overlay_host_s* active_capture_transaction = nullptr;

sao_status_t invoke_protection_provider(sao_ui_overlay_host_s* host, bool enable) noexcept {
    if (host->protection_provider == nullptr)
        return SAO_STATUS_OK;
    try {
        return host->protection_provider(
            host->hwnd, host->control_hwnd, host->owner_hwnd, enable,
            host->protection_provider_user_data);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

} // namespace

sao_status_t set_capture_mode_impl(sao_ui_overlay_host_s* host, bool exclude);

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_create(
    const SaoOverlayHostConfig* config, sao_ui_overlay_host_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (!acquire_single_instance_lock())
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    OverlayHostCreateGuard create_guard(true);

    try {
    auto* host = new (std::nothrow) sao_ui_overlay_host_s();
    if (host == nullptr)
        return create_guard.fail(SAO_STATUS_ERR_UNKNOWN, out_handle);
    create_guard.adopt(host);
    host->hinstance = ::GetModuleHandleW(nullptr);
    host->owner_thread_id = ::GetCurrentThreadId();
    host->class_name = make_class_name();
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = overlay_wndproc;
    window_class.hInstance = host->hinstance;
    window_class.hCursor = nullptr;
    window_class.lpszClassName = host->class_name.c_str();
    host->class_atom = ::RegisterClassExW(&window_class);
    if (host->class_atom == 0)
        return create_guard.fail(SAO_STATUS_ERR_OS_CALL_FAILED, out_handle);

    // Default surface is the primary monitor only. The primary monitor
    // always sits at virtual-screen origin (0,0), so the default bounds
    // are SM_CX/CYSCREEN at (0,0). Spanning SM_*VIRTUALSCREEN instead
    // made the hRender window cover every attached monitor and UI
    // rendered on secondary screens; the SaoOverlayHostConfig contract
    // (see overlay_host.h) pins the default to the primary screen, and
    // a secondary-monitor surface must be requested via explicit
    // width/height/origin.
    const bool explicit_bounds = config != nullptr && config->width > 0 && config->height > 0;
    const int32_t width = explicit_bounds ? config->width : ::GetSystemMetrics(SM_CXSCREEN);
    const int32_t height =
        explicit_bounds ? config->height : ::GetSystemMetrics(SM_CYSCREEN);
    const int32_t x = explicit_bounds ? config->origin_x : 0;
    const int32_t y = explicit_bounds ? config->origin_y : 0;
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
        return create_guard.fail(SAO_STATUS_ERR_OS_CALL_FAILED, out_handle);
    }

    MARGINS margins{-1, -1, -1, -1};
    ::DwmExtendFrameIntoClientArea(host->hwnd, &margins);
    host->client_rect = {x, y, width > 0 ? width : 1920, height > 0 ? height : 1080};
    host->desired_rect = host->client_rect;
    host->current_dpi = query_window_dpi(host->hwnd);
    if (config != nullptr) {
        host->protection_provider = config->protection_provider;
        host->protection_provider_user_data = config->protection_provider_user_data;
    }
    host->dc_mutation =
        config == nullptr
            ? nullptr
            : static_cast<sao_ui_dc_mutation_coordinator_handle_t>(config->dc_mutation_coordinator);
    if (host->dc_mutation != nullptr) {
        const sao_status_t register_status = sao_ui_dc_mutation_coordinator_register(
            host->dc_mutation, host->hwnd, &host->dc_mutation_token);
        if (register_status != SAO_STATUS_OK) {
            return create_guard.fail(register_status, out_handle);
        }
    }
    if (config != nullptr && config->sao_screencap_protection) {
        const sao_status_t protection_status =
            sao_ui_overlay_host_set_capture_mode(host, true);
        if (protection_status != SAO_STATUS_OK) {
            return create_guard.fail(protection_status, out_handle);
        }
    }
    create_guard.commit(out_handle);
    return SAO_STATUS_OK;
    } catch (...) {
        return create_guard.fail(SAO_STATUS_ERR_UNKNOWN, out_handle);
    }
}

extern "C" bool SAO_UI_CALL sao_ui_overlay_host_destroy(sao_ui_overlay_host_handle_t handle) {
    bool retire_started = false;
    try {
    if (handle == nullptr)
        return true;

    if (current_thread_host_entry_depth(handle) != 0u)
        return false;

    const DWORD current_thread_id = ::GetCurrentThreadId();
    {
        std::lock_guard<std::mutex> lock(handle->lifetime_mu);
        if (handle->lifecycle == sao_ui_overlay_host_s::lifecycle_state::retired)
            return true;
        if (handle->owner_thread_id != current_thread_id ||
            handle->lifecycle != sao_ui_overlay_host_s::lifecycle_state::active) {
            return false;
        }
        handle->lifecycle = sao_ui_overlay_host_s::lifecycle_state::retiring;
        handle->destroy_thread_id = current_thread_id;
        retire_started = true;
    }

    {
        std::unique_lock<std::mutex> lock(handle->lifetime_mu);
        handle->lifetime_cv.wait(lock, [handle] {
            return handle->active_api_leases == 0 && handle->active_callback_entries == 0;
        });
    }

    bool success = true;
    if (handle->dc_mutation != nullptr && handle->hwnd != nullptr &&
        !sao_ui_dc_mutation_coordinator_invalidate(handle->dc_mutation, handle->hwnd, 1.0)) {
        success = false;
    }
    if (success && handle->hwnd != nullptr && ::GetCapture() == handle->hwnd) {
        dispatch_mouse(handle, WM_CANCELMODE, 0, 0);
        (void)::ReleaseCapture();
    }
#if defined(SAO_UI_HAS_ANTI_SCREENCAP_FACADE)
    bool capture_pair_bound = false;
    bool protection_requested = false;
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        capture_pair_bound = handle->capture_pair_bound;
        protection_requested = handle->protection_requested;
    }
    if (success && protection_requested && handle->hwnd != nullptr &&
        handle->control_hwnd != nullptr &&
        set_capture_mode_impl(handle, false) != SAO_STATUS_OK) {
        success = false;
    }
    if (success && capture_pair_bound &&
        unbind_host_capture_pair(handle) != SAO_STATUS_OK) {
        success = false;
    }
    if (success && unregister_host_threat_windows(handle) != SAO_STATUS_OK)
        success = false;
#else
    // Without the security provider there is no authoritative affinity
    // owner.  Keep the state explicitly unprotected rather than claiming a
    // capture exclusion that cannot be verified.
    if (success) {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        handle->capture_excluded = false;
    }
#endif
    if (success && !destroy_created_window(handle->hwnd))
        success = false;
    if (success && !destroy_created_window(handle->control_hwnd))
        success = false;
    if (success && !destroy_created_window(handle->owner_hwnd))
        success = false;
    if (success) {
        release_themed_cursors(handle);
        if (handle->class_atom != 0)
            ::UnregisterClassW(handle->class_name.c_str(), handle->hinstance);
        release_single_instance_lock();
        {
            std::lock_guard<std::mutex> lock(handle->lifetime_mu);
            handle->lifecycle = sao_ui_overlay_host_s::lifecycle_state::retired;
            handle->destroy_thread_id = 0;
        }
    } else {
        std::lock_guard<std::mutex> lock(handle->lifetime_mu);
        handle->lifecycle = sao_ui_overlay_host_s::lifecycle_state::active;
        handle->destroy_thread_id = 0;
    }
    handle->lifetime_cv.notify_all();
    return success;
    } catch (...) {
        if (retire_started && handle != nullptr) {
            try {
                {
                    std::lock_guard<std::mutex> lock(handle->lifetime_mu);
                    if (handle->lifecycle == sao_ui_overlay_host_s::lifecycle_state::retiring &&
                        handle->destroy_thread_id == ::GetCurrentThreadId()) {
                        handle->lifecycle = sao_ui_overlay_host_s::lifecycle_state::active;
                        handle->destroy_thread_id = 0;
                    }
                }
                handle->lifetime_cv.notify_all();
            } catch (...) {
            }
        }
        return false;
    }
}

extern "C" void* SAO_UI_CALL sao_ui_overlay_host_hwnd(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    return lease ? handle->hwnd : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void* SAO_UI_CALL sao_ui_overlay_host_control_hwnd(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    return lease ? handle->control_hwnd : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void* SAO_UI_CALL sao_ui_overlay_host_owner_hwnd(
    sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    return lease ? handle->owner_hwnd : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_acquire_lease(
    sao_ui_overlay_host_handle_t handle, void* expected_hwnd, void** out_lease,
    void** out_hwnd) {
    try {
    if (out_lease == nullptr || out_hwnd == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_lease = nullptr;
    *out_hwnd = nullptr;
    auto* token = new (std::nothrow) HostLeaseToken(handle);
    if (token == nullptr || !token->lease) {
        delete token;
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    auto* host = token->lease.get();
    if (host->owner_thread_id != ::GetCurrentThreadId() ||
        (expected_hwnd != nullptr && host->hwnd != expected_hwnd) ||
        host->hwnd == nullptr || !::IsWindow(host->hwnd)) {
        const bool owner_thread = host->owner_thread_id == ::GetCurrentThreadId();
        delete token;
        return owner_thread ? SAO_STATUS_ERR_INVALID_ARGUMENT
                            : SAO_STATUS_ERR_ACCESS_DENIED;
    }
    *out_hwnd = host->hwnd;
    *out_lease = token;
    return SAO_STATUS_OK;
    } catch (...) {
        if (out_lease != nullptr) *out_lease = nullptr;
        if (out_hwnd != nullptr) *out_hwnd = nullptr;
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

extern "C" void SAO_UI_CALL sao_ui_overlay_host_release_lease(void* lease) {
    try {
        delete static_cast<HostLeaseToken*>(lease);
    } catch (...) {
    }
}
extern "C" void* SAO_UI_CALL
sao_ui_overlay_host_dc_mutation_coordinator(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    return lease ? handle->dc_mutation : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void* SAO_UI_CALL sao_ui_overlay_host_hglrc(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return nullptr;
    return nullptr;
    } catch (...) {
        return nullptr;
    }
}
extern "C" void* SAO_UI_CALL sao_ui_overlay_host_hdc(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return nullptr;
    return nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_bounds(
    sao_ui_overlay_host_handle_t handle, int32_t x, int32_t y, int32_t width, int32_t height) {
    try {
    HostLease lease(handle);
    if (!lease || handle->hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (width <= 0 || height <= 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        if (handle->dpi_transition_active) {
            handle->dpi_pending_rect = {x, y, width, height};
            handle->dpi_pending_bounds = true;
            return SAO_UI_STATUS_ERR_BUSY;
        }
    }
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
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_get_desired_bounds(
    sao_ui_overlay_host_handle_t handle, SaoOverlayHostClientRect* out_rect) {
    try {
    if (out_rect != nullptr)
        std::memset(out_rect, 0, sizeof(*out_rect));
    HostLease lease(handle);
    if (!lease || handle->hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_rect == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    *out_rect = handle->desired_rect;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_set_visible(sao_ui_overlay_host_handle_t handle, bool visible) {
    try {
    HostLease lease(handle);
    if (!lease || handle->hwnd == nullptr || handle->control_hwnd == nullptr) {
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
    if (!visible && ((handle->app_cursor != nullptr && ::GetCursor() == handle->app_cursor) ||
                     (handle->menu_cursor != nullptr && ::GetCursor() == handle->menu_cursor)))
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
    if (!visible)
        return SAO_STATUS_OK;
    (void)::DwmFlush();
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_activation_enabled(
    sao_ui_overlay_host_handle_t handle, bool enabled) {
    try {
        HostLease lease(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (handle->owner_thread_id != ::GetCurrentThreadId())
            return SAO_STATUS_ERR_ACCESS_DENIED;
        std::lock_guard<std::mutex> update_lock(handle->input_update_mu);
        return apply_activation_unlocked(handle, enabled);
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

extern "C" bool SAO_UI_CALL
sao_ui_overlay_host_activation_enabled(sao_ui_overlay_host_handle_t handle) {
    try {
        HostLease lease(handle);
        if (!lease)
            return false;
        std::lock_guard<std::mutex> lock(handle->state_mu);
        return handle->activation_enabled;
    } catch (...) {
        return false;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_require_owner_thread(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    return handle->owner_thread_id == ::GetCurrentThreadId()
               ? SAO_STATUS_OK
               : SAO_STATUS_ERR_ACCESS_DENIED;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_set_input_passthrough(sao_ui_overlay_host_handle_t handle, bool passthrough) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (handle->owner_thread_id != ::GetCurrentThreadId()) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::lock_guard<std::mutex> update_lock(handle->input_update_mu);
    return apply_passthrough_unlocked(handle, passthrough);
    } catch (...) {
        if (handle != nullptr) {
            try {
                HostLease recovery_lease(handle);
                if (recovery_lease) mark_input_partial(handle);
            } catch (...) {
            }
        }
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

extern "C" bool SAO_UI_CALL
sao_ui_overlay_host_input_passthrough(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return false;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return handle->input_passthrough;
    } catch (...) {
        return false;
    }
}

namespace sao::ui::overlay_host_detail {

sao_status_t set_input_region_ex(sao_ui_overlay_host_handle_t handle,
                                 const SaoOverlayHostInputRect* rects, size_t rect_count,
                                 uint32_t flags) noexcept {
    try {
    HostLease lease(handle);
    if (!lease || handle->hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (rect_count != 0 && rects == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if ((flags & ~kInputRegionSkipPrevUnion) != 0u)
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
    // Single temporal-union owner: compositor sync_host_rgn already folds
    // the previous frame's spans into `rects` (and the union-disabled path
    // is a pixel-exact debugging switch), so skip the host's own stored-
    // previous union when the caller opts out via the flag.
    const bool skip_prev_union = (flags & kInputRegionSkipPrevUnion) != 0u;
    // The effective window region is current ∪ (previous unless the caller is
    // the single temporal-union owner).  Recompute that set and compare it
    // against what was last written: identical submissions skip the whole
    // CreateRectRgn/GetWindowRgn/SetWindowRgn transaction instead of paying
    // three Win32 region calls on every compositor tick.
    std::vector<SaoOverlayHostInputRect> effective;
    effective.reserve(current.size() + (skip_prev_union ? 0u : previous.size()));
    for (const auto& rect : current)
        effective.push_back(rect);
    if (!skip_prev_union) {
        for (const auto& rect : previous)
            effective.push_back(rect);
    }
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        if (handle->window_region_applied &&
            handle->applied_input_rects.size() == effective.size() &&
            std::equal(handle->applied_input_rects.begin(), handle->applied_input_rects.end(),
                       effective.begin(),
                       [](const SaoOverlayHostInputRect& a,
                          const SaoOverlayHostInputRect& b) noexcept {
                           return a.x == b.x && a.y == b.y && a.width == b.width &&
                                  a.height == b.height;
                       })) {
            handle->previous_input_rects = std::move(current);
            handle->input_sync_state = SAO_UI_OVERLAY_INPUT_SYNCHRONIZED;
            return SAO_STATUS_OK;
        }
    }
    if (!skip_prev_union) {
        for (const auto& rect : previous) {
            if (!append_rect(region.get(), rect))
                return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
    }
    OwnedRegion rollback_region(::CreateRectRgn(0, 0, 0, 0));
    if (rollback_region.get() == nullptr)
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    const auto& api = win32_api();
    bool had_window_region = false;
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        had_window_region = handle->window_region_applied;
    }
    if (had_window_region && api.get_window_rgn(handle->hwnd, rollback_region.get()) == ERROR) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (!api.set_window_rgn(handle->hwnd, region.get(), TRUE)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    region.release();

    const bool passthrough =
        current.empty() && (skip_prev_union || previous.empty());
    const sao_status_t passthrough_status = apply_passthrough_unlocked(handle, passthrough);
    if (passthrough_status != SAO_STATUS_OK) {
        const bool rollback_ok = had_window_region
                                     ? api.set_window_rgn(handle->hwnd, rollback_region.get(), TRUE)
                                     : api.set_window_rgn(handle->hwnd, nullptr, TRUE);
        if (!rollback_ok) {
            {
                std::lock_guard<std::mutex> lock(handle->state_mu);
                handle->window_region_applied = true;
            }
            mark_input_partial(handle);
        } else if (had_window_region) {
            rollback_region.release();
        }
        return passthrough_status;
    }

    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        handle->previous_input_rects = std::move(current);
        handle->applied_input_rects = std::move(effective);
        handle->input_sync_state = SAO_UI_OVERLAY_INPUT_SYNCHRONIZED;
        handle->window_region_applied = true;
    }
    return SAO_STATUS_OK;
    } catch (...) {
        if (handle != nullptr) {
            try {
                HostLease recovery_lease(handle);
                if (recovery_lease) mark_input_partial(handle);
            } catch (...) {
            }
        }
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::ui::overlay_host_detail

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_input_region(
    sao_ui_overlay_host_handle_t handle, const SaoOverlayHostInputRect* rects, size_t rect_count) {
    return sao::ui::overlay_host_detail::set_input_region_ex(handle, rects, rect_count, 0u);
}

extern "C" uint32_t SAO_UI_CALL
sao_ui_overlay_host_input_sync_state(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_UI_OVERLAY_INPUT_PARTIAL;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return handle->input_sync_state;
    } catch (...) {
        return SAO_UI_OVERLAY_INPUT_PARTIAL;
    }
}

sao_status_t set_capture_mode_impl(sao_ui_overlay_host_s* handle, bool exclude) {
    if (handle == nullptr || handle->hwnd == nullptr || handle->control_hwnd == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (active_capture_transaction == handle)
        return SAO_UI_STATUS_ERR_BUSY;
    std::lock_guard<std::recursive_mutex> transaction_lock(handle->capture_mode_mu);
    struct CaptureTransactionScope {
        sao_ui_overlay_host_s* previous{};
        explicit CaptureTransactionScope(sao_ui_overlay_host_s* current) noexcept
            : previous(active_capture_transaction) {
            active_capture_transaction = current;
        }
        ~CaptureTransactionScope() {
            active_capture_transaction = previous;
        }
    } transaction_scope(handle);
#if defined(SAO_UI_HAS_ANTI_SCREENCAP_FACADE)
    const SaoAntiScreencapAffinityPair pair = capture_topology(handle);
    bool previous_requested = false;
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        previous_requested = handle->protection_requested;
    }
    uint32_t effective = SAO_ASC_MODE_INVALID;
    if (exclude) {
        const sao_status_t affinity_status = map_anti_screencap_status(
            sao_security_anti_screencap_overlay_host_set_capture_mode(
                &pair, SAO_ASC_MODE_EXCLUDED, &effective));
        if (affinity_status != SAO_STATUS_OK) return affinity_status;
        mark_host_capture_pair_bound(handle, pair);
        const sao_status_t provider_status = previous_requested
            ? SAO_STATUS_OK
            : invoke_protection_provider(handle, true);
        if (provider_status != SAO_STATUS_OK) {
            uint32_t ignored = SAO_ASC_MODE_INVALID;
            const sao_status_t unwind = map_anti_screencap_status(
                sao_security_anti_screencap_overlay_host_set_capture_mode(
                    &pair,
                    previous_requested ? SAO_ASC_MODE_EXCLUDED : SAO_ASC_MODE_NORMAL,
                    &ignored));
            if (unwind != SAO_STATUS_OK)
                return unwind;
            if (!previous_requested) {
                const sao_status_t unbind_status = unbind_host_capture_pair(handle);
                if (unbind_status != SAO_STATUS_OK)
                    return unbind_status;
            }
            return provider_status;
        }
    } else {
        const sao_status_t provider_status = previous_requested
            ? invoke_protection_provider(handle, false)
            : SAO_STATUS_OK;
        if (provider_status != SAO_STATUS_OK) return provider_status;
        const sao_status_t affinity_status = map_anti_screencap_status(
            sao_security_anti_screencap_overlay_host_set_capture_mode(
                &pair, SAO_ASC_MODE_NORMAL, &effective));
        if (affinity_status != SAO_STATUS_OK) {
            if (previous_requested) (void)invoke_protection_provider(handle, true);
            return affinity_status;
        }
        mark_host_capture_pair_bound(handle, pair);
    }
    const sao_status_t threat_status = map_anti_screencap_status(
        sao_security_anti_screencap_react_to_capture_threat(exclude));
    if (threat_status != SAO_STATUS_OK) {
        // The registered-window sweep failed; the host must not claim
        // exclusion.  Unwind the mode change and surface the failure.
        const sao_status_t unwind = map_anti_screencap_status(
            sao_security_anti_screencap_overlay_host_set_capture_mode(
                &pair, previous_requested ? SAO_ASC_MODE_EXCLUDED : SAO_ASC_MODE_NORMAL,
                &effective));
        if (!previous_requested)
            (void)invoke_protection_provider(handle, false);
        else if (!exclude)
            (void)invoke_protection_provider(handle, true);
        if (unwind != SAO_STATUS_OK) return unwind;
        mark_host_capture_pair_bound(handle, pair);
        if (!previous_requested) {
            const sao_status_t unbind_status = unbind_host_capture_pair(handle);
            if (unbind_status != SAO_STATUS_OK)
                return unbind_status;
        }
        return threat_status;
    }
    std::lock_guard<std::mutex> lock(handle->state_mu);
    // MONITORED is a compatibility fallback, not strict exclusion.  Do not
    // advertise the host as capture-excluded unless the facade applied an
    // actual exclusion mode.
    handle->capture_excluded =
        effective == SAO_ASC_MODE_STREAMING || effective == SAO_ASC_MODE_EXCLUDED;
    handle->protection_requested = exclude;
    return SAO_STATUS_OK;
#else
    (void)exclude;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->capture_excluded = false;
    handle->protection_requested = false;
    return SAO_STATUS_ERR_NOT_INITIALIZED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_set_capture_mode(sao_ui_overlay_host_handle_t handle, bool exclude) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    return set_capture_mode_impl(handle, exclude);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" bool SAO_UI_CALL
sao_ui_overlay_host_capture_excluded(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return false;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return handle->capture_excluded;
    } catch (...) {
        return false;
    }
}

extern "C" bool SAO_UI_CALL sao_ui_overlay_host_visible(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return false;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return handle->visible;
    } catch (...) {
        return false;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_get_state(sao_ui_overlay_host_handle_t handle, SaoOverlayHostState* out_state) {
    try {
    HostLease lease(handle);
    if (!lease)
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
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// LEGACY classification: the WGL make-current / release-current /
// swap-buffers surface is intentionally stubbed on the D3D/DComp
// production compositor.  overlay_host.h line 213-214 declares them as
// "Legacy WGL operations ... return SAO_STATUS_ERR_NOT_IMPLEMENTED in
// the D3D/DComp production path".  Existing tests
// (test_overlay_host_main_chain.cpp line 176) explicitly assert this
// return code as a contract, so promoting to CAPABILITY_MISSING would
// silently break the contract.  See PLAN.md §1.5 for the taxonomy.
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_make_current(
    sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED; // legacy WGL stub
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_release_current(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED; // legacy WGL stub
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_swap_buffers(
    sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED; // legacy WGL stub
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_pump_messages(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease || handle->hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (handle->owner_thread_id != ::GetCurrentThreadId())
        return SAO_STATUS_ERR_ACCESS_DENIED;
#if defined(SAO_UI_HAS_ANTI_SCREENCAP_FACADE)
    // Threat sweep: the cached detector runs one fresh module scan per
    // second.  When protection was requested and something is detected,
    // raise the exclusion reaction so every registered window is swept.
    {
        const uint64_t now_ms = ::GetTickCount64();
        bool threat_scan_due = false;
        bool process_window_sweep_due = false;
        {
            std::lock_guard<std::mutex> lock(handle->state_mu);
            if (handle->protection_requested) {
                if (handle->last_threat_scan_ms == 0u ||
                    now_ms - handle->last_threat_scan_ms >= 1000u) {
                    handle->last_threat_scan_ms = now_ms;
                    threat_scan_due = true;
                }
                if (handle->last_process_window_sweep_ms == 0u ||
                    now_ms - handle->last_process_window_sweep_ms >= 2000u) {
                    handle->last_process_window_sweep_ms = now_ms;
                    process_window_sweep_due = true;
                }
            }
        }
        if (threat_scan_due &&
            sao_security_anti_screencap_cached_flags(handle->hwnd, 1000u) != 0u) {
            const sao_status_t reaction_status = map_anti_screencap_status(
                sao_security_anti_screencap_react_to_capture_threat(true));
            (void)reaction_status;
        }
        // Startup-gap sweep: lazily register every current-process top-level
        // window that has not been registered explicitly. This is advisory;
        // a transient enumeration failure must not terminate the UI loop.
        if (process_window_sweep_due) {
            const int32_t register_status =
                sao_security_anti_screencap_register_process_windows();
            if (register_status < 0) {
                const sao_status_t mapped_status =
                    map_anti_screencap_status(register_status);
                (void)mapped_status;
            }
        }
    }
#endif
    MSG message{};
    for (uint32_t count = 0; count < 128 && ::PeekMessageW(&message, handle->hwnd, 0, 0, PM_REMOVE);
         ++count) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    const sao_status_t pending_status = drain_pending_dpi_bounds(handle);
    return pending_status == SAO_UI_STATUS_ERR_BUSY ? SAO_STATUS_OK : pending_status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_overlay_host_msg_wait(sao_ui_overlay_host_handle_t handle, uint32_t timeout_ms) {
    try {
    HostLease lease(handle);
    if (!lease || handle->hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    return ::MsgWaitForMultipleObjectsEx(0, nullptr, timeout_ms, QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE) == WAIT_FAILED
               ? SAO_STATUS_ERR_OS_CALL_FAILED
               : SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_overlay_host_set_cursor_hint_(sao_ui_overlay_host_handle_t handle, int32_t hint) {
    try {
    HostLease lease(handle);
    if (!lease)
        return;
    {
        std::lock_guard<std::mutex> lock(handle->state_mu);
        handle->cursor_hint = hint >= 0 && hint <= 4 ? hint : 0;
    }
    apply_cursor_hint(handle);
    } catch (...) {
        return;
    }
}

namespace sao::ui::overlay_host_detail {

void set_menu_cursor(sao_ui_overlay_host_handle_t host, bool visible) noexcept {
    HostLease lease(host);
    if (!lease) return;
    {
        std::lock_guard<std::mutex> lock(host->state_mu);
        if (host->menu_cursor_active == visible) return;
        host->menu_cursor_active = visible;
    }
    apply_cursor_hint(host);
}

void set_input_cursor(sao_ui_overlay_host_handle_t host, int32_t cursor_kind) noexcept {
    HostLease lease(host);
    if (!lease) return;
    {
        std::lock_guard<std::mutex> lock(host->state_mu);
        host->input_cursor_kind = cursor_kind;
    }
    apply_cursor_hint(host);
}

} // namespace sao::ui::overlay_host_detail

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_hit_test(
    sao_ui_overlay_host_handle_t handle, sao_ui_hit_test_fn_t fn, void* user_data) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->hit_test_fn = fn;
    handle->hit_test_user = user_data;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_activation_hit_test(
    sao_ui_overlay_host_handle_t handle, sao_ui_hit_test_fn_t fn, void* user_data) {
    try {
        HostLease lease(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(handle->state_mu);
        handle->activation_hit_test_fn = fn;
        handle->activation_hit_test_user = user_data;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_mouse(
    sao_ui_overlay_host_handle_t handle, sao_ui_mouse_fn_t fn, void* user_data) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->mouse_fn = fn;
    handle->mouse_user = user_data;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_get_client_rect(
    sao_ui_overlay_host_handle_t handle, SaoOverlayHostClientRect* out_rect) {
    try {
    if (out_rect != nullptr)
        std::memset(out_rect, 0, sizeof(*out_rect));
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_rect == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    *out_rect = handle->client_rect;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" uint32_t SAO_UI_CALL
sao_ui_overlay_host_current_dpi(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease)
        return 0;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return handle->current_dpi;
    } catch (...) {
        return 0;
    }
}

extern "C" float SAO_UI_CALL sao_ui_overlay_host_scale_factor(sao_ui_overlay_host_handle_t handle) {
    try {
    HostLease lease(handle);
    if (!lease) return 0.0F;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    return static_cast<float>(handle->current_dpi == 0 ? 96u : handle->current_dpi) / 96.0F;
    } catch (...) {
        return 0.0F;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_get_wm_counters(
    sao_ui_overlay_host_handle_t handle, SaoOverlayHostWMCounters* out_counters) {
    try {
    if (out_counters != nullptr)
        std::memset(out_counters, 0, sizeof(*out_counters));
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_counters == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    *out_counters = handle->wm_counters;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_size_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_size_fn_t fn, void* user_data) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->size_fn = fn;
    handle->size_user = user_data;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_move_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_move_fn_t fn, void* user_data) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->move_fn = fn;
    handle->move_user = user_data;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_activate_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_activate_fn_t fn, void* user_data) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->activate_fn = fn;
    handle->activate_user = user_data;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_dpi_changed_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_dpi_changed_fn_t fn, void* user_data) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->dpi_changed_fn = fn;
    handle->dpi_changed_user = user_data;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_dpi_reflow_fn(sao_ui_overlay_host_handle_t handle, sao_ui_dpi_reflow_fn_t fn, void* user_data) {
    try {
    HostLease lease(handle);
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->dpi_reflow_fn = fn;
    handle->dpi_reflow_user = user_data;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_display_change_fn(
    sao_ui_overlay_host_handle_t handle, sao_ui_display_change_fn_t fn, void* user_data) {
    try {
    HostLease lease(handle);
    if (!lease)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->state_mu);
    handle->display_change_fn = fn;
    handle->display_change_user = user_data;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

#if defined(SAO_UI_OVERLAY_HOST_TESTING)
namespace sao::ui::overlay_host_detail {

void set_win32_api_for_testing(const Win32Api* api) noexcept {
    win32_api_override().store(api, std::memory_order_release);
}

void reset_win32_api_for_testing() noexcept {
    win32_api_override().store(nullptr, std::memory_order_release);
}

bool input_rect_has_valid_bounds_for_testing(const SaoOverlayHostInputRect& rect) noexcept {
    return input_rect_has_valid_bounds(rect);
}

} // namespace sao::ui::overlay_host_detail
#endif

#endif
