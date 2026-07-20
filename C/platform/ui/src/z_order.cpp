#include "sao/ui/z_order.h"
#include "sao/ui/dc_mutation.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

struct sao_ui_z_order_manager_s {
    std::mutex mu;
    sao_ui_overlay_host_handle_t host = nullptr;
    int32_t policy = SAO_UI_TOPMOST_FOLLOW_GAME;
    uint32_t idle_ms = 250;
    uint32_t active_ms = 500;
    sao_ui_dc_mutation_coordinator_handle_t dc_mutation = nullptr;
    SaoZOrderStatus status{};
};

namespace {

bool valid_policy(int32_t policy) {
    return policy == SAO_UI_TOPMOST_FOLLOW_GAME || policy == SAO_UI_TOPMOST_ALWAYS ||
           policy == SAO_UI_TOPMOST_NEVER;
}

uint64_t monotonic_ns() {
#if defined(_WIN32)
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    ::QueryPerformanceFrequency(&frequency);
    ::QueryPerformanceCounter(&counter);
    if (frequency.QuadPart == 0) return 0;
    return static_cast<uint64_t>((counter.QuadPart * 1000000000LL) / frequency.QuadPart);
#else
    return 0;
#endif
}

bool current_topmost(void* hwnd) {
#if defined(_WIN32)
    if (hwnd == nullptr) return false;
    return (::GetWindowLongPtrW(reinterpret_cast<HWND>(hwnd), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
#else
    (void)hwnd;
    return false;
#endif
}

void update_status(sao_ui_z_order_manager_s* manager, void* host_hwnd, void* game_hwnd,
                   bool game_topmost, bool game_present) {
    manager->status.policy = manager->policy;
    manager->status.is_topmost = current_topmost(host_hwnd);
    manager->status.game_hwnd_snapshot = game_hwnd;
    manager->status.game_topmost_snapshot = game_topmost;
    manager->status.game_present_snapshot = game_present;
    manager->status.last_enforce_ns = monotonic_ns();
}

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_z_order_manager_create(
    sao_ui_overlay_host_handle_t host, sao_ui_z_order_manager_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (host == nullptr || sao_ui_overlay_host_hwnd(host) == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* manager = new (std::nothrow) sao_ui_z_order_manager_s();
    if (manager == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    manager->host = host;
    manager->dc_mutation = static_cast<sao_ui_dc_mutation_coordinator_handle_t>(
        sao_ui_overlay_host_dc_mutation_coordinator(host));
    manager->status.policy = SAO_UI_TOPMOST_FOLLOW_GAME;
    *out_handle = manager;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_z_order_manager_destroy(
    sao_ui_z_order_manager_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_z_order_set_policy(
    sao_ui_z_order_manager_handle_t handle, int32_t policy) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_policy(policy)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    handle->policy = policy;
    handle->status.policy = policy;
    handle->status.last_enforce_ns = 0;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_z_order_set_intervals(
    sao_ui_z_order_manager_handle_t handle, uint32_t idle_ms, uint32_t active_ms) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (idle_ms < 100 || active_ms < 100) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    handle->idle_ms = idle_ms;
    handle->active_ms = active_ms;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_z_order_enforce(
    sao_ui_z_order_manager_handle_t handle, void* game_hwnd, bool game_is_topmost, bool game_present) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mu);
    void* host_value = sao_ui_overlay_host_hwnd(handle->host);
    if (host_value == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if !defined(_WIN32)
    (void)game_hwnd;
    (void)game_is_topmost;
    (void)game_present;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
    HWND host_hwnd = reinterpret_cast<HWND>(host_value);
    HWND game = reinterpret_cast<HWND>(game_hwnd);
    SaoOverlayHostClientRect desired{};
    const sao_status_t bounds_status =
        sao_ui_overlay_host_get_desired_bounds(handle->host, &desired);
    if (bounds_status != SAO_STATUS_OK) return bounds_status;
    if (game != nullptr && !::IsWindow(game)) {
        game = nullptr;
        game_present = false;
        game_is_topmost = false;
    }

    const uint64_t now_ns = monotonic_ns();
    const uint64_t interval_ns = static_cast<uint64_t>(
        game_present ? handle->active_ms : handle->idle_ms) * 1000000ULL;
    const bool unchanged = handle->status.last_enforce_ns != 0 &&
        handle->status.policy == handle->policy &&
        handle->status.game_hwnd_snapshot == game &&
        handle->status.game_topmost_snapshot == game_is_topmost &&
        handle->status.game_present_snapshot == game_present;
    if (unchanged && now_ns >= handle->status.last_enforce_ns &&
        now_ns - handle->status.last_enforce_ns < interval_ns) {
        return SAO_STATUS_OK;
    }

    if (handle->policy == SAO_UI_TOPMOST_NEVER) {
        const BOOL restored = ::SetWindowPos(host_hwnd, nullptr, desired.x, desired.y,
                                             desired.width, desired.height,
                                             SWP_NOACTIVATE | SWP_NOZORDER);
        update_status(handle, host_hwnd, game, game_is_topmost, game_present);
        return restored ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    BOOL succeeded = FALSE;
    if (handle->policy == SAO_UI_TOPMOST_ALWAYS || (game_present && game != nullptr && game_is_topmost)) {
        succeeded = ::SetWindowPos(host_hwnd, HWND_TOPMOST, desired.x, desired.y,
                                   desired.width, desired.height, SWP_NOACTIVATE);
        ++handle->status.branch_game_topmost_count;
    } else if (game_present && game != nullptr) {
        succeeded = ::SetWindowPos(host_hwnd, game, desired.x, desired.y,
                                   desired.width, desired.height, SWP_NOACTIVATE);
        ++handle->status.branch_game_normal_count;
    } else {
        // Clearing TOPMOST and placing the host above normal windows are two
        // different operations.  Keep them adjacent and only here.
        const BOOL cleared = ::SetWindowPos(host_hwnd, HWND_NOTOPMOST, desired.x, desired.y,
                                            desired.width, desired.height, SWP_NOACTIVATE);
        succeeded = cleared &&
            ::SetWindowPos(host_hwnd, HWND_TOP, desired.x, desired.y,
                           desired.width, desired.height, SWP_NOACTIVATE);
        ++handle->status.branch_idle_count;
    }
    update_status(handle, host_hwnd, game, game_is_topmost, game_present);
    return succeeded ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_z_order_hide_exstyle_mask(
    sao_ui_z_order_manager_handle_t handle, uint32_t mask) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (handle->dc_mutation == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    void* hwnd = sao_ui_overlay_host_hwnd(handle->host);
    if (hwnd == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    const std::string args = "{\"mask\":" + std::to_string(mask) + "}";
    return sao_ui_dc_mutation_coordinator_submit_dc(
        handle->dc_mutation, hwnd, "host-exstyle", "hide_exstyle",
        reinterpret_cast<const uint8_t*>(args.data()), args.size());
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_z_order_status(
    sao_ui_z_order_manager_handle_t handle, SaoZOrderStatus* out_status) {
    if (out_status != nullptr) std::memset(out_status, 0, sizeof(*out_status));
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_status == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mu);
    *out_status = handle->status;
    out_status->is_topmost = current_topmost(sao_ui_overlay_host_hwnd(handle->host));
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_z_order_check_leak_patterns(
    sao_ui_z_order_manager_handle_t handle, uint32_t current_exstyle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    // The DComp host must not use layered-window rendering.  It is the one
    // observable combination associated with a stale layered/topmost host.
    return (current_exstyle & SAO_UI_WS_EX_LAYERED) == 0 ? SAO_STATUS_OK
                                                         : SAO_STATUS_ERR_INVALID_ARGUMENT;
}
