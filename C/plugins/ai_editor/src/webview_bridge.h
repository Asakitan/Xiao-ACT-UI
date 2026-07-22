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
    // Phase C wire-up: when `sao_mmf_name_utf8` is non-empty, the WebView2
    // HWND is moved off-screen (SetWindowPos to -32000, -32000) and its
    // rendered content is captured via PrintWindow into a MMF ring at
    // ~30 fps.  A main-process compositor layer consuming that MMF then
    // renders the webview inside the compositor.  Leave empty to keep
    // the legacy visible-HWND behaviour (standalone SaoAiEditor.exe run).
    std::string sao_mmf_name_utf8;
    // Optional shared-memory input event ring name.  When non-empty the
    // bridge polls the ring for InputEvent records and forwards them via
    // SendMessage to the WebView2 HWND, unblocking keyboard/mouse routing
    // from the main-process compositor.  Ignored when
    // `sao_mmf_name_utf8` is empty (input ring is only meaningful when
    // the visible HWND is off-screen).
    std::string sao_input_ring_name_utf8;
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
