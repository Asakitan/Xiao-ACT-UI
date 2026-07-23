#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "sao/ai_editor/ai_editor_native.h"
#include "sao/ai_editor/ai_editor_status.h"

namespace sao::ai_editor::native {

// Config for one off-screen WebView2 technical surface. The HWND is never a
// user-visible panel; the SaoAuto compositor is the only visible consumer.
struct WebViewConfig {
    std::string url;              // navigate target (about:blank if empty)
    std::string user_data_folder; // required by WebView2
    std::string window_title;     // Win32 window caption
    int width = 1280;
    int height = 800;
    // When true, incoming WebMessageReceived JSON payloads are routed into
    // the NativeRuntime hidden behind `runtime_handle`.  Requires
    // `runtime_handle` to be non-null.
    bool bridge_native_runtime = true;
    sao_ai_editor_runtime_t runtime_handle = nullptr;
    // Required compositor bridge frame ring. The WebView2 HWND is created
    // off-screen and captured into this SOPF-compatible MMF source.
    std::string sao_mmf_name_utf8;
    // Required compositor input ring. The bridge polls InputEvent records
    // and forwards them to the off-screen WebView2 HWND.
    std::string sao_input_ring_name_utf8;
};

// Blocking helper: creates an off-screen STA surface, boots CoreWebView2,
// spins its message pump, and returns after a bridge close request or setup
// failure. Missing/invalid MMF or input rings fail closed before WebView2 is
// exposed; no visible fallback exists.
SAO_AI_EDITOR_API int32_t run_webview_bridge(const WebViewConfig& config);

// Detection helper — returns true iff WebView2Loader.dll can be loaded
// and CreateCoreWebView2EnvironmentWithOptions is exported.
SAO_AI_EDITOR_API bool webview_runtime_available();

}  // namespace sao::ai_editor::native
