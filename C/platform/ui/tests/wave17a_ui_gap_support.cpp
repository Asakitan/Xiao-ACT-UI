// SAO Auto - Wave 17 / Agent a - platform/ui NOT_IMPLEMENTED classification.
// See wave17a_ui_gap_support.h for the isolation contract.

#include "wave17a_ui_gap_support.h"

#include <algorithm>
#include <string>

namespace sao::ui::wave17a {

// ---------------------------------------------------------------------
// The classification matrix.
//
// Every row is a live NOT_IMPLEMENTED or converted CAPABILITY_MISSING
// return in platform/ui/src as of the Wave 17a static scan.  Adding a
// new row means the wave17a test-suite counters (matrix_totals()) also
// change — that is intentional, so a future site addition without a
// classification touch will red the test.
// ---------------------------------------------------------------------

std::vector<GapRow> classification_matrix() {
    std::vector<GapRow> rows;
    rows.reserve(48);

    // ── alerts.cpp (4 sites, all class A) ───────────────────────────
    rows.push_back({
        "platform/ui/src/alerts.cpp", 305,
        "sao_ui_alerts_speak",
        GapClass::CapabilityGate,
        "SAPI 5 COM only exists on Windows",
        "sapi.dll (Windows SAPI 5 text-to-speech)",
    });
    rows.push_back({
        "platform/ui/src/alerts.cpp", 363,
        "sao_ui_alerts_enum_voices",
        GapClass::CapabilityGate,
        "SAPI 5 voice enumeration is Windows-only",
        "sapi.dll",
    });
    rows.push_back({
        "platform/ui/src/alerts.cpp", 435,
        "sao_ui_alerts_sound_play",
        GapClass::CapabilityGate,
        "PlaySoundW + waveOutSetVolume are Windows-only",
        "winmm.dll",
    });
    rows.push_back({
        "platform/ui/src/alerts.cpp", 461,
        "sao_ui_alerts_sound_stop",
        GapClass::CapabilityGate,
        "PlaySoundW stop path is Windows-only",
        "winmm.dll",
    });

    // ── auto_key.cpp (5 sites, all class A) ─────────────────────────
    rows.push_back({
        "platform/ui/src/auto_key.cpp", 269,
        "sao_ui_auto_key_send_key",
        GapClass::CapabilityGate,
        "SendInput keyboard path is Windows-only",
        "user32.dll SendInput",
    });
    rows.push_back({
        "platform/ui/src/auto_key.cpp", 306,
        "sao_ui_auto_key_send_key_combo",
        GapClass::CapabilityGate,
        "SendInput combo path is Windows-only",
        "user32.dll SendInput",
    });
    rows.push_back({
        "platform/ui/src/auto_key.cpp", 346,
        "sao_ui_auto_key_send_text",
        GapClass::CapabilityGate,
        "SendInput KEYEVENTF_UNICODE is Windows-only",
        "user32.dll SendInput",
    });
    rows.push_back({
        "platform/ui/src/auto_key.cpp", 431,
        "sao_ui_auto_key_send_mouse_click",
        GapClass::CapabilityGate,
        "SendInput MOUSEEVENTF_* + GetSystemMetrics are Windows-only",
        "user32.dll SendInput",
    });
    rows.push_back({
        "platform/ui/src/auto_key.cpp", 451,
        "sao_ui_auto_key_get_key_state",
        GapClass::CapabilityGate,
        "GetAsyncKeyState is Windows-only",
        "user32.dll GetAsyncKeyState",
    });

    // ── d3d11_device.cpp (3 sites, all class A) ─────────────────────
    rows.push_back({
        "platform/ui/src/d3d11_device.cpp", 212,
        "sao_ui_d3d11_device_create",
        GapClass::CapabilityGate,
        "D3D11CreateDevice is Windows-only",
        "d3d11.dll + dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/d3d11_device.cpp", 322,
        "sao_ui_d3d11_device_recreate",
        GapClass::CapabilityGate,
        "GetDeviceRemovedReason is Windows-only",
        "d3d11.dll",
    });
    rows.push_back({
        "platform/ui/src/d3d11_device.cpp", 347,
        "sao_ui_d3d11_device_check_alive",
        GapClass::CapabilityGate,
        "GetDeviceRemovedReason liveness probe is Windows-only",
        "d3d11.dll",
    });

    // ── dcomp_bridge.cpp (8 class A + 4 class C) ────────────────────
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 197,
        "sao_ui_dcomp_bridge_create",
        GapClass::CapabilityGate,
        "DirectComposition is Windows-only",
        "dcomp.dll",
    });
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 208,
        "sao_ui_dcomp_bridge_create (runtime dcomp probe)",
        GapClass::CapabilityGate,
        "dcomp.dll DCompositionCreateDevice symbol missing",
        "dcomp.dll",
    });
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 406,
        "sao_ui_dcomp_bridge_attach",
        GapClass::CapabilityGate,
        "DComp visual tree attach is Windows-only",
        "dcomp.dll",
    });
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 424,
        "sao_ui_dcomp_bridge_detach",
        GapClass::CapabilityGate,
        "DComp visual tree detach is Windows-only",
        "dcomp.dll",
    });
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 444,
        "sao_ui_dcomp_bridge_present",
        GapClass::CapabilityGate,
        "Present + Commit are Windows-only",
        "dcomp.dll + dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 468,
        "sao_ui_dcomp_bridge_resize",
        GapClass::CapabilityGate,
        "IDXGISwapChain::ResizeBuffers is Windows-only",
        "dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 534,
        "sao_ui_dcomp_bridge_upload_bgra",
        GapClass::CapabilityGate,
        "D3D11 texture upload path is Windows-only",
        "d3d11.dll",
    });
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 576,
        "sao_ui_dcomp_bridge_device_removed",
        GapClass::CapabilityGate,
        "GetDeviceRemovedReason is Windows-only",
        "d3d11.dll",
    });
    // Class C legacy — the four WGL_NV_DX_interop2 stubs stay
    // NOT_IMPLEMENTED intentionally.
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 549,
        "sao_ui_dcomp_bridge_register_gl_interop",
        GapClass::LegacySkeleton,
        "Legacy WGL interop stub — no WGL context on the DComp path",
        "WGL_NV_DX_interop2 (deprecated)",
    });
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 554,
        "sao_ui_dcomp_bridge_unregister_gl_interop",
        GapClass::LegacySkeleton,
        "Legacy WGL interop stub",
        "WGL_NV_DX_interop2 (deprecated)",
    });
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 559,
        "sao_ui_dcomp_bridge_lock_texture",
        GapClass::LegacySkeleton,
        "Legacy WGL interop stub",
        "WGL_NV_DX_interop2 (deprecated)",
    });
    rows.push_back({
        "platform/ui/src/dcomp_bridge.cpp", 564,
        "sao_ui_dcomp_bridge_unlock_texture",
        GapClass::LegacySkeleton,
        "Legacy WGL interop stub",
        "WGL_NV_DX_interop2 (deprecated)",
    });

    // ── dxgi_dup.cpp (9 sites, all class A) ─────────────────────────
    rows.push_back({
        "platform/ui/src/dxgi_dup.cpp", 138,
        "sao_ui_dxgi_dup_create",
        GapClass::CapabilityGate,
        "IDXGIOutput1::DuplicateOutput is Windows-only",
        "dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/dxgi_dup.cpp", 416,
        "sao_ui_dxgi_dup_acquire_frame",
        GapClass::CapabilityGate,
        "AcquireNextFrame is Windows-only",
        "dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/dxgi_dup.cpp", 445,
        "sao_ui_dxgi_dup_release_frame",
        GapClass::CapabilityGate,
        "ReleaseFrame is Windows-only",
        "dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/dxgi_dup.cpp", 471,
        "sao_ui_dxgi_dup_get_desc",
        GapClass::CapabilityGate,
        "DXGI_OUTDUPL_DESC is Windows-only",
        "dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/dxgi_dup.cpp", 532,
        "sao_ui_dxgi_dup_reinit",
        GapClass::CapabilityGate,
        "DXGI adapter/output enumeration is Windows-only",
        "dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/dxgi_dup.cpp", 594,
        "sao_ui_dxgi_dup_get_dirty_rects",
        GapClass::CapabilityGate,
        "GetFrameDirtyRects is Windows-only",
        "dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/dxgi_dup.cpp", 681,
        "sao_ui_dxgi_dup_get_move_rects",
        GapClass::CapabilityGate,
        "GetFrameMoveRects is Windows-only",
        "dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/dxgi_dup.cpp", 776,
        "sao_ui_dxgi_dup_get_cursor_info",
        GapClass::CapabilityGate,
        "GetFramePointerShape is Windows-only",
        "dxgi.dll",
    });
    rows.push_back({
        "platform/ui/src/dxgi_dup.cpp", 855,
        "sao_ui_dxgi_dup_copy_to_staging",
        GapClass::CapabilityGate,
        "D3D11 staging CopyResource + Map are Windows-only",
        "d3d11.dll",
    });

    // ── gpu_capture.cpp (10 sites, all class A) ─────────────────────
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 224,
        "start_wgc_session (RoInitialize probe)",
        GapClass::CapabilityGate,
        "RoInitialize failed — WinRT apartment not obtainable",
        "combase.dll (Windows Runtime)",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 286,
        "sao_ui_gpu_capture_create (WGC not compiled)",
        GapClass::CapabilityGate,
        "Windows.Graphics.Capture not compiled into this build",
        "Windows.Graphics.Capture (WinRT)",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 290,
        "sao_ui_gpu_capture_create (WGC runtime probe)",
        GapClass::CapabilityGate,
        "IsGraphicsCaptureSessionSupported false on this OS build",
        "Windows.Graphics.Capture (Windows 10 1903+)",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 326,
        "sao_ui_gpu_capture_ensure_session (WGC not compiled)",
        GapClass::CapabilityGate,
        "WGC not compiled in",
        "Windows.Graphics.Capture",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 332,
        "sao_ui_gpu_capture_ensure_session (UNSUPPORTED latched)",
        GapClass::CapabilityGate,
        "Earlier WGC probe latched UNSUPPORTED",
        "Windows.Graphics.Capture",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 344,
        "sao_ui_gpu_capture_ensure_session (E_NOTIMPL for HWND)",
        GapClass::CapabilityGate,
        "GraphicsCaptureItem CreateForWindow returned E_NOTIMPL",
        "Windows.Graphics.Capture",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 360,
        "sao_ui_gpu_capture_get_latest",
        GapClass::CapabilityGate,
        "WGC not compiled in",
        "Windows.Graphics.Capture",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 416,
        "sao_ui_gpu_capture_client_inset",
        GapClass::CapabilityGate,
        "GetClientRect / GetWindowRect are Windows-only",
        "user32.dll",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 435,
        "sao_ui_gpu_capture_stop",
        GapClass::CapabilityGate,
        "WGC session release is Windows-only",
        "Windows.Graphics.Capture",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 463,
        "sao_ui_gpu_capture_get_state",
        GapClass::CapabilityGate,
        "WGC state getter is Windows-only",
        "Windows.Graphics.Capture",
    });
    // The three Wave 7 CPU functions share a non-Windows stub.
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 776,
        "sao_ui_gpu_capture_bgra_from_texture (non-Windows)",
        GapClass::CapabilityGate,
        "D3D11 staging copy is Windows-only",
        "d3d11.dll",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 781,
        "sao_ui_gpu_capture_premultiply_bgra (non-Windows)",
        GapClass::CapabilityGate,
        "Shares non-Windows stub with bgra_from_texture",
        "d3d11.dll",
    });
    rows.push_back({
        "platform/ui/src/gpu_capture.cpp", 786,
        "sao_ui_gpu_capture_compute_hash (non-Windows)",
        GapClass::CapabilityGate,
        "Shares non-Windows stub with bgra_from_texture",
        "d3d11.dll",
    });

    // ── input.cpp (2 sites, all class A) ────────────────────────────
    rows.push_back({
        "platform/ui/src/input.cpp", 371,
        "sao_ui_input_router_install_ll_hooks",
        GapClass::CapabilityGate,
        "SetWindowsHookExW is Windows-only",
        "user32.dll WH_MOUSE_LL / WH_KEYBOARD_LL",
    });
    rows.push_back({
        "platform/ui/src/input.cpp", 398,
        "sao_ui_input_router_uninstall_ll_hooks",
        GapClass::CapabilityGate,
        "UnhookWindowsHookEx is Windows-only",
        "user32.dll",
    });

    // ── input_router.cpp (1 site, class A) ──────────────────────────
    rows.push_back({
        "platform/ui/src/input_router.cpp", 307,
        "sao_ui_input_router_feed_raw_win32",
        GapClass::CapabilityGate,
        "Consumes raw WM_* messages; Windows-only surface",
        "user32.dll + WM_* virtual keys",
    });

    // ── overlay_host.cpp (3 sites, all class C legacy) ──────────────
    rows.push_back({
        "platform/ui/src/overlay_host.cpp", 675,
        "sao_ui_overlay_host_make_current",
        GapClass::LegacySkeleton,
        "Legacy WGL stub — DComp/D3D compositor has no WGL context",
        "WGL (opengl32.dll deprecated path)",
    });
    rows.push_back({
        "platform/ui/src/overlay_host.cpp", 679,
        "sao_ui_overlay_host_release_current",
        GapClass::LegacySkeleton,
        "Legacy WGL stub",
        "WGL (opengl32.dll deprecated path)",
    });
    rows.push_back({
        "platform/ui/src/overlay_host.cpp", 683,
        "sao_ui_overlay_host_swap_buffers",
        GapClass::LegacySkeleton,
        "Legacy WGL stub",
        "WGL (opengl32.dll deprecated path)",
    });

    // ── z_order.cpp (1 site, class A) ───────────────────────────────
    rows.push_back({
        "platform/ui/src/z_order.cpp", 125,
        "sao_ui_z_order_enforce",
        GapClass::CapabilityGate,
        "SetWindowPos + WS_EX_TOPMOST are Windows-only",
        "user32.dll SetWindowPos",
    });

    return rows;
}

MatrixTotals matrix_totals() {
    MatrixTotals t;
    const auto rows = classification_matrix();
    t.total_rows = static_cast<uint32_t>(rows.size());
    for (const auto& r : rows) {
        switch (r.gap_class) {
            case GapClass::CapabilityGate: ++t.capability_gate_rows; break;
            case GapClass::Implementable:  ++t.implementable_rows;   break;
            case GapClass::LegacySkeleton: ++t.legacy_skeleton_rows; break;
        }
    }
    return t;
}

// ---------------------------------------------------------------------
// Hermetic mocks.
// ---------------------------------------------------------------------

void SendInputLedger::record_press(uint32_t vk, uint32_t mods,
                                   uint32_t hold_ms) {
    events_.push_back({vk, mods, hold_ms, false});
    ++presses_;
}

void SendInputLedger::record_release(uint32_t vk) {
    events_.push_back({vk, 0u, 0u, true});
    ++releases_;
}

void SendInputLedger::reset() noexcept {
    events_.clear();
    presses_ = 0;
    releases_ = 0;
}

ShellNotifyMockResult ShellNotifyLedger::simulate_add_icon(
    uint32_t /*id*/, std::string_view /*tip*/) {
    ++calls_;
    latest_.status = SAO_STATUS_OK;
    latest_.icon_added = true;
    latest_.icon_removed = false;
    return latest_;
}

ShellNotifyMockResult ShellNotifyLedger::simulate_modify_icon(
    uint32_t /*id*/, std::string_view /*tip*/) {
    ++calls_;
    latest_.status = SAO_STATUS_OK;
    latest_.icon_added = false;
    latest_.icon_removed = false;
    return latest_;
}

ShellNotifyMockResult ShellNotifyLedger::simulate_remove_icon(uint32_t /*id*/) {
    ++calls_;
    latest_.status = SAO_STATUS_OK;
    latest_.icon_added = false;
    latest_.icon_removed = true;
    return latest_;
}

ShellNotifyMockResult ShellNotifyLedger::simulate_show_balloon(
    uint32_t /*id*/, std::string_view /*title*/, std::string_view /*body*/) {
    ++calls_;
    ++latest_.balloon_show_count;
    latest_.status = SAO_STATUS_OK;
    return latest_;
}

void ShellNotifyLedger::reset() noexcept {
    calls_ = 0;
    latest_ = {};
}

bool DcompLateBindMock::resolve(bool pretend_dll_loaded,
                                bool pretend_symbol_present) noexcept {
    last_dll_loaded_ = pretend_dll_loaded;
    last_symbol_present_ = pretend_symbol_present;
    return pretend_dll_loaded && pretend_symbol_present;
}

GapClass DcompLateBindMock::classify_outcome(bool loaded,
                                             bool symbol) const noexcept {
    // When both the DLL and the symbol are present we can build a real
    // DComp bridge — no gap.  Otherwise the wave17a taxonomy pins us to
    // "capability gate" because the code path is implemented but the
    // capability is absent.
    if (loaded && symbol) return GapClass::Implementable;
    return GapClass::CapabilityGate;
}

bool status_is_capability_gate(sao_status_t status) noexcept {
    return status == SAO_STATUS_ERR_CAPABILITY_MISSING;
}

bool status_is_legacy_skeleton(sao_status_t status) noexcept {
    return status == SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

bool taxonomy_codes_are_distinct() noexcept {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED != SAO_STATUS_ERR_CAPABILITY_MISSING;
}

} // namespace sao::ui::wave17a
