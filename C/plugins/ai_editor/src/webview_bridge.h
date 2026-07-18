#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "sao/ai_editor/ai_editor_native.h"
#include "sao/ai_editor/ai_editor_status.h"

namespace sao::ai_editor::native {

// Config for a single WebView2 window.
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
};

// Blocking helper: creates an STA window, boots CoreWebView2, spins a
// modal message pump, and returns when the user closes the window (or
// `stop_signaled` becomes true).  Returns SAO_AI_EDITOR_OK on clean
// shutdown, or a domain error when WebView2Loader/CoreWebView2 setup
// fails (missing runtime, missing loader, etc.).
SAO_AI_EDITOR_API int32_t run_webview_bridge(const WebViewConfig& config);

// Detection helper — returns true iff WebView2Loader.dll can be loaded
// and CreateCoreWebView2EnvironmentWithOptions is exported.
SAO_AI_EDITOR_API bool webview_runtime_available();

}  // namespace sao::ai_editor::native
