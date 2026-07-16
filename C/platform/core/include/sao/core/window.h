// SAO Auto — HWND helpers.
//
// Every ui/overlay wire-up wants "find the game window" and "how big is
// the screen".  Those helpers live here because they're not overlay-
// specific — the launcher pings them at startup, and plugins use them
// via the SDK.
//
// HWNDs are exposed as opaque void* to keep the header free of <windows.h>.

#pragma once

#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SaoScreenInfo {
    int32_t primary_width;
    int32_t primary_height;
    int32_t virtual_left;
    int32_t virtual_top;
    int32_t virtual_width;
    int32_t virtual_height;
    uint32_t monitor_count;
    uint32_t _pad;
};

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_window_get_screen_info(
    SaoScreenInfo* out_info);

// Find a top-level window belonging to the given PID.  If class_utf16 or
// title_substr_utf16 are null, they act as wildcards.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_window_find_top_level_by_pid(
    uint32_t pid,
    const wchar_t* class_utf16,       // may be null
    const wchar_t* title_substr_utf16, // may be null
    void** out_hwnd);

// Rectangle-in-screen-coordinates for the given HWND.
struct SaoRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_window_get_client_rect_screen(
    void* hwnd, SaoRect* out_rect);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_window_is_visible(
    void* hwnd, bool* out_visible);

// Whether the given HWND is currently the foreground window (used by the
// overlay z-order state machine to decide when to hide).
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_window_is_foreground(
    void* hwnd, bool* out_is_foreground);

#ifdef __cplusplus
}  // extern "C"
#endif
