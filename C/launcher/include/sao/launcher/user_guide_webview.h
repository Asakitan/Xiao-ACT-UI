// SAO Auto — launcher/user_guide_webview.h
//
// Opens the offline user guide in an in-process WebView2 window instead of
// handing the URL to the default browser.  WebView2 runs on a dedicated STA
// UI thread so the launcher's compositor/thread apartment remains unchanged.
#pragma once

#include <windows.h>

namespace sao::launcher {

// Shows the user guide (docs\html\index.html) in a WebView2 window.
// Returns true when the window was created and WebView2 initialization
// started successfully; false when WebView2Loader.dll / the WebView2
// runtime is unavailable, in which case the caller should fall back to
// ShellExecute (default browser).
bool openUserGuideInWebView(const wchar_t* docs_index_path) noexcept;

// Closes the guide thread and releases its WebView2/COM/module resources.
// Returns false only when the thread did not stop within the bounded wait;
// callers may retry during their normal shutdown sequence.
bool shutdownUserGuideWebView() noexcept;

// Notify the guide STA after the native sound preferences change.
void refreshUserGuideSoundPolicy() noexcept;

} // namespace sao::launcher
