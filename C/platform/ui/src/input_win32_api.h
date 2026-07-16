#pragma once

#if defined(_WIN32)

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "sao/ui/input.h"

namespace sao::ui::input_detail {

using SetWindowsHookExWFn = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
using UnhookWindowsHookExFn = BOOL(WINAPI*)(HHOOK);
using CallNextHookExFn = LRESULT(WINAPI*)(HHOOK, int, WPARAM, LPARAM);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetCurrentThreadIdFn = DWORD(WINAPI*)();

struct Win32Api {
    SetWindowsHookExWFn set_windows_hook_ex_w;
    UnhookWindowsHookExFn unhook_windows_hook_ex;
    CallNextHookExFn call_next_hook_ex;
    GetAsyncKeyStateFn get_async_key_state;
    GetCurrentThreadIdFn get_current_thread_id;
};

#if defined(SAO_UI_INPUT_TESTING)
void set_win32_api_for_testing(const Win32Api* api) noexcept;
void reset_win32_api_for_testing() noexcept;
LRESULT invoke_mouse_proc_for_testing(int code, WPARAM wparam, LPARAM lparam);
LRESULT invoke_keyboard_proc_for_testing(int code, WPARAM wparam, LPARAM lparam);
bool active_router_present_for_testing() noexcept;
std::uintptr_t mouse_hook_for_testing(sao_ui_input_router_handle_t handle) noexcept;
std::uintptr_t keyboard_hook_for_testing(sao_ui_input_router_handle_t handle) noexcept;
bool retry_retired_hooks_for_testing() noexcept;
#endif

}  // namespace sao::ui::input_detail

namespace sao::ui::overlay_host_detail {

using SetWindowRgnFn = int(WINAPI*)(HWND, HRGN, BOOL);
using GetWindowRgnFn = int(WINAPI*)(HWND, HRGN);
using GetWindowLongPtrWFn = LONG_PTR(WINAPI*)(HWND, int);
using SetWindowLongPtrWFn = LONG_PTR(WINAPI*)(HWND, int, LONG_PTR);
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);

struct Win32Api {
    SetWindowRgnFn set_window_rgn;
    GetWindowRgnFn get_window_rgn;
    GetWindowLongPtrWFn get_window_long_ptr_w;
    SetWindowLongPtrWFn set_window_long_ptr_w;
    SetWindowPosFn set_window_pos;
};

#if defined(SAO_UI_OVERLAY_HOST_TESTING)
void set_win32_api_for_testing(const Win32Api* api) noexcept;
void reset_win32_api_for_testing() noexcept;
#endif

}  // namespace sao::ui::overlay_host_detail

#endif
