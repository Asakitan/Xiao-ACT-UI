#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_core/abi.h"
#include "sao_core/sao_status.h"

struct SaoLegacyCoreRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct SaoLegacyCoreWindowInfo {
    void* hwnd;
    uint32_t pid;
    uint32_t thread_id;
    SaoLegacyCoreRect window_rect;
    SaoLegacyCoreRect client_rect_screen;
    uint32_t style;
    uint32_t extended_style;
    uint32_t visible;
    uint32_t minimized;
    wchar_t title[256];
    wchar_t class_name[256];
};

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_window_create_layered_topmost(const wchar_t* title, int32_t x, int32_t y, int32_t w,
                                              int32_t h, void** out_hwnd);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_window_destroy(void* hwnd);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_window_enumerate(void** out_hwnds, size_t max_hwnds, size_t* out_hwnd_count);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_window_get_info(void* hwnd, SaoLegacyCoreWindowInfo* out_info);
