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
#include <mutex>
#include <string>

#include "sao_core/sao_status.h"
#include "sao_security/abi.h"
#include "sao_security/anti_screencap/window_affinity.h"

namespace sao::ai_editor {

inline HMODULE anti_screencap_module() noexcept {
    static std::mutex module_mutex;
    static HMODULE module = nullptr;
    std::lock_guard<std::mutex> lock(module_mutex);
    if (module != nullptr) return module;

    std::wstring executable_path(32768u, L'\0');
    const DWORD written = ::GetModuleFileNameW(
        nullptr, executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    if (written == 0u || written >= executable_path.size()) return nullptr;
    executable_path.resize(written);

    const size_t separator = executable_path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return nullptr;
    executable_path.resize(separator + 1u);
    executable_path += L"sao_security_anti_screencap.dll";

    module = ::GetModuleHandleW(executable_path.c_str());
    if (module != nullptr) return module;

    module = ::LoadLibraryExW(
        executable_path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    return module;
}

// Apply anti-screencap hardening to a window that was created with an
// empty title.  Routes writes+reads through the sao_security helper
// (mirrors the Python authority in `sao_auto/python/render/overlay_host.py`
// `OverlayHost.set_capture_mode`: `_ac_apply` + `_ac_verify` in
// `mem_probe._dc`).  The helper picks WDA_EXCLUDEFROMCAPTURE on
// Win10 20H1+ and WDA_MONITOR fallback on older builds, and honours the
// streaming-mode policy flag.  We resolve the exports dynamically so the
// process uses the one application-local anti-screencap DLL rather than
// embedding another copy of its process-global state.
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
inline int32_t register_capture_protection_window_status(HWND hwnd) {
    if (hwnd == nullptr) return SAO_ERR_HANDLE_INVALID;
    const HMODULE anti_screencap = anti_screencap_module();
    if (anti_screencap == nullptr) return SAO_ERR_NOT_FOUND;
    using RegisterWindowFn = int32_t (SAO_SECURITY_CALL*)(void*);
    const auto register_window = reinterpret_cast<RegisterWindowFn>(
        ::GetProcAddress(anti_screencap,
                         "sao_security_anti_screencap_register_window"));
    if (register_window == nullptr) return SAO_ERR_NOT_FOUND;
    return register_window(hwnd);
}

inline bool register_capture_protection_window(HWND hwnd) {
    return register_capture_protection_window_status(hwnd) == SAO_OK;
}

inline int32_t unregister_capture_protection_window_status(HWND hwnd) {
    if (hwnd == nullptr) return SAO_ERR_HANDLE_INVALID;
    const HMODULE anti_screencap = anti_screencap_module();
    if (anti_screencap == nullptr) return SAO_ERR_NOT_FOUND;
    using UnregisterWindowFn = int32_t (SAO_SECURITY_CALL*)(void*);
    const auto unregister_window = reinterpret_cast<UnregisterWindowFn>(
        ::GetProcAddress(anti_screencap,
                         "sao_security_anti_screencap_unregister_window"));
    if (unregister_window == nullptr) return SAO_ERR_NOT_FOUND;
    return unregister_window(hwnd);
}

inline bool unregister_capture_protection_window(HWND hwnd) {
    return unregister_capture_protection_window_status(hwnd) == SAO_OK;
}
enum class WebviewHardeningStatus : uint8_t {
    kApplied = 0,
    kDeferredByPolicy = 1,
    kAffinityApplyFailed = 2,
    kRegistrationFailed = 3,
    kInvalidWindow = 4,
};

inline WebviewHardeningStatus classify_webview_hardening(
    bool affinity_applied, bool coordinator_registered) noexcept {
    if (!coordinator_registered) {
        return WebviewHardeningStatus::kRegistrationFailed;
    }
    return affinity_applied
        ? WebviewHardeningStatus::kApplied
        : WebviewHardeningStatus::kAffinityApplyFailed;
}

inline bool webview_hardening_registered(
    WebviewHardeningStatus status) noexcept {
    return status == WebviewHardeningStatus::kApplied ||
           status == WebviewHardeningStatus::kDeferredByPolicy ||
           status == WebviewHardeningStatus::kAffinityApplyFailed;
}

inline WebviewHardeningStatus classify_webview_hardening_status(
    int32_t affinity_status, int32_t registration_status) noexcept {
    if (registration_status != SAO_OK) {
        return WebviewHardeningStatus::kRegistrationFailed;
    }
    if (affinity_status == SAO_OK) {
        return WebviewHardeningStatus::kApplied;
    }
    if (affinity_status == SAO_ASC_APPLY_POLICY_DISABLED) {
        return WebviewHardeningStatus::kDeferredByPolicy;
    }
    return WebviewHardeningStatus::kAffinityApplyFailed;
}

inline int32_t apply_stealth_window_status(
    HWND hwnd, bool bypass_streaming_policy = true) {
    if (hwnd == nullptr) return SAO_ERR_HANDLE_INVALID;
    const HMODULE anti_screencap = anti_screencap_module();
    if (anti_screencap == nullptr) return SAO_ERR_NOT_FOUND;
    using ApplySingleWindowFn = int32_t (SAO_SECURITY_CALL*)(
        void*, uint32_t, bool, bool);
    const auto apply_single_window = reinterpret_cast<ApplySingleWindowFn>(
        ::GetProcAddress(anti_screencap,
                         "sao_security_anti_screencap_apply_single_window_policy"));
    if (apply_single_window == nullptr) return SAO_ERR_NOT_FOUND;
    return apply_single_window(
        hwnd, SAO_ASC_AFFINITY_EXCLUDE_FROM_CAPTURE, true,
        bypass_streaming_policy);
}

inline bool apply_stealth_window(HWND hwnd) {
    return apply_stealth_window_status(hwnd) == SAO_OK;
}

inline WebviewHardeningStatus harden_window(
    HWND hwnd, bool bypass_streaming_policy) {
    if (hwnd == nullptr) return WebviewHardeningStatus::kInvalidWindow;
    const int32_t affinity_status =
        apply_stealth_window_status(hwnd, bypass_streaming_policy);
    const int32_t registration_status =
        register_capture_protection_window_status(hwnd);
    return classify_webview_hardening_status(affinity_status,
                                             registration_status);
}

inline WebviewHardeningStatus harden_webview_window(HWND hwnd) {
    // WebView hardening must honor the streaming policy and never mutate it.
    return harden_window(hwnd, false);
}

}  // namespace sao::ai_editor

#endif  // _WIN32
