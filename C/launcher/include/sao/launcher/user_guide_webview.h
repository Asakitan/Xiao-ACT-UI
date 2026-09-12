#pragma once

namespace sao::launcher {

// Guide lifecycle calls run on the borrowed compositor's owner thread.
bool openUserGuideInWebView(const wchar_t* docs_index_path) noexcept;
bool openUserGuideInWebView(const wchar_t* docs_index_path, bool native_intro_completed) noexcept;
void tickUserGuideWebView() noexcept;
void hideUserGuideWebView() noexcept;
bool userGuideWebViewReady() noexcept;
bool shutdownUserGuideWebView() noexcept;
void refreshUserGuideSoundPolicy() noexcept;

} // namespace sao::launcher
