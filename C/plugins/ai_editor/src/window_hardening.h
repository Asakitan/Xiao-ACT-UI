#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

namespace sao::ai_editor {

// Apply anti-screencap hardening to a window that was created with an
// empty title.  Routes writes+reads through the sao_security helper
// (mirrors the Python authority in `sao_auto/python/render/overlay_host.py`
// `OverlayHost.set_capture_mode`: `_ac_apply` + `_ac_verify` in
// `mem_probe._dc`).  The helper picks WDA_EXCLUDEFROMCAPTURE on
// Win10 20H1+ and WDA_MONITOR fallback on older builds, and honours the
// streaming-mode policy flag.  We resolve the exports dynamically because
// sao_platform_ui already PRIVATE-links sao::security::anti_screencap so
// the DLL is resident in the process; consumers of this header do not
// need to add a static link.
//
// Returns true iff:
//   * `hwnd` is non-null
//   * the sao_security anti-screencap DLL is loaded in the current process
//   * `sao_security_anti_screencap_affinity_set_exclude(hwnd)` succeeded
//   * the subsequent `sao_security_anti_screencap_affinity_get(hwnd)`
//     read verifies affinity moved off WDA_NONE (i.e. either
//     EXCLUDEFROMCAPTURE or MONITOR is now in force)
//
// Returns false without side effects when the helper DLL is not
// available (headless tests / stripped builds).  Callers may skip the
// check and continue — the window simply keeps default WDA_NONE.
//
// This helper does NOT touch the window title.  All three AI Editor
// windows (sao_ai_editor_main / webview_bridge / gpu_hunt_panel) already
// create their HWNDs with an empty title constant (see kWindowTitle /
// kWindowClassName definitions in each translation unit), so an extra
// SetWindowTextW would be redundant and adds a spurious user32 write.
inline bool apply_stealth_window(HWND hwnd) {
    if (hwnd == nullptr) {
        return false;
    }
    const HMODULE anti_screencap = ::GetModuleHandleW(
        L"sao_security_anti_screencap.dll");
    if (anti_screencap == nullptr) {
        return false;
    }
    using AffinitySetExcludeFn = int32_t(__stdcall*)(void*);
    using AffinityGetFn = int32_t(__stdcall*)(void*, uint32_t*);
    const auto set_exclude = reinterpret_cast<AffinitySetExcludeFn>(
        ::GetProcAddress(anti_screencap,
                          "sao_security_anti_screencap_affinity_set_exclude"));
    const auto affinity_get = reinterpret_cast<AffinityGetFn>(
        ::GetProcAddress(anti_screencap,
                          "sao_security_anti_screencap_affinity_get"));
    if (set_exclude == nullptr || affinity_get == nullptr) {
        return false;
    }
    if (set_exclude(hwnd) != 0) {
        return false;
    }
    uint32_t observed = 0u;
    if (affinity_get(hwnd, &observed) != 0) {
        return false;
    }
    // 0 == SAO_ASC_AFFINITY_NONE.  Any non-zero (MONITOR = 0x1 /
    // EXCLUDEFROMCAPTURE = 0x11) means the helper wrote and the DWM
    // acknowledged the new affinity.  Streaming-mode-off collapses the
    // apply to a no-op success — the read-back returns NONE and we
    // report failure so the caller knows hardening did not take.
    return observed != 0u;
}

}  // namespace sao::ai_editor

#endif  // _WIN32
