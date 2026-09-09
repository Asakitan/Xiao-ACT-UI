#include "native_text_edit.h"
#include "sao/ui/input_router.h"
#include "widget_paint_internal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <commctrl.h>
#include <imm.h>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace sao::ui::detail {
namespace {
constexpr UINT_PTR kEditSubclass = 0x53414f45;
struct EditProxy {
    HWND owner{};
    HWND edit{};
    HWND previous_focus{};
    LONG_PTR previous_style{};
    sao_ui_widget_handle_t widget{};
    TextEditAction action;
    std::string initial;
    bool composing{};
    bool finishing{};
    POINT candidate{};
    int width{1};
    int height{1};
};
// HWND and IME contexts stay on their creating thread. Retired proxy shells
// remain until that thread exits, so reentrant window messages never use freed state.
thread_local std::unordered_map<HWND, std::unique_ptr<EditProxy>> proxies;

std::wstring wide(const std::string& text) {
    if (text.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring result(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), n);
    return result;
}
std::string utf8(std::wstring_view text) {
    if (text.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string result(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), n, nullptr, nullptr);
    return result;
}
void synchronize(EditProxy& proxy, bool commit, bool submit = false) {
    TextEditSnapshot state;
    if (!proxy.widget || !text_edit_snapshot(proxy.widget, state) || !IsWindow(proxy.edit))
        return;
    const int length = GetWindowTextLengthW(proxy.edit);
    if (length < 0 || length > 1048576)
        return;
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int actual = GetWindowTextW(proxy.edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(std::max(0, actual)));
    DWORD begin = 0, end = 0;
    SendMessageW(proxy.edit, EM_GETSEL, reinterpret_cast<WPARAM>(&begin),
                 reinterpret_cast<LPARAM>(&end));
    const std::string encoded = utf8(text);
    // WM_CHAR can deliver the high surrogate before the low surrogate. Keep the
    // last complete snapshot until the EDIT contains a valid Unicode string.
    if (!text.empty() && encoded.empty())
        return;
    state.text = encoded;
    state.selection_start =
        utf8(std::wstring_view(text).substr(0, std::min<size_t>(begin, text.size()))).size();
    state.selection_end =
        utf8(std::wstring_view(text).substr(0, std::min<size_t>(end, text.size()))).size();
    if (HIMC imc = ImmGetContext(proxy.edit)) {
        const std::string prefix = state.text.substr(0, state.selection_end);
        const auto newline = prefix.find_last_of('\n');
        const std::string line = newline == std::string::npos ? prefix : prefix.substr(newline + 1);
        float advance = 0.0F, line_height = 20.0F;
        (void)measure_text_dwrite(line.c_str(), 15.0F, &advance, &line_height);
        const auto row = static_cast<int>(std::count(prefix.begin(), prefix.end(), '\n'));
        COMPOSITIONFORM composition{};
        composition.dwStyle = CFS_POINT;
        composition.ptCurrentPos = {
            proxy.candidate.x + static_cast<LONG>(std::min(
                                    advance, static_cast<float>(std::max(0, proxy.width - 18)))),
            proxy.candidate.y + std::min(row * 20, std::max(0, proxy.height - 24))};
        ImmSetCompositionWindow(imc, &composition);
        CANDIDATEFORM candidate{};
        candidate.dwStyle = CFS_CANDIDATEPOS;
        candidate.ptCurrentPos = composition.ptCurrentPos;
        ImmSetCandidateWindow(imc, &candidate);
        ImmReleaseContext(proxy.edit, imc);
    }
    state.composition.clear();
    if (proxy.composing) {
        if (HIMC imc = ImmGetContext(proxy.edit)) {
            const LONG bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
            if (bytes > 0 && bytes <= 65536 && bytes % sizeof(wchar_t) == 0) {
                std::wstring preedit(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
                if (ImmGetCompositionStringW(imc, GCS_COMPSTR, preedit.data(), bytes) == bytes)
                    state.composition = utf8(preedit);
            }
            ImmReleaseContext(proxy.edit, imc);
        }
    }
    if (text_edit_update(proxy.widget, state) == SAO_STATUS_OK && proxy.action) {
        auto action = proxy.action;
        action(state.text, submit   ? TextEditPhase::Submit
                           : commit ? TextEditPhase::Commit
                                    : TextEditPhase::Change);
    }
}
void finish(EditProxy& proxy, bool commit, bool submit = false) {
    if (!proxy.widget || proxy.finishing)
        return;
    proxy.finishing = true;
    if (proxy.composing && IsWindow(proxy.edit)) {
        if (HIMC imc = ImmGetContext(proxy.edit)) {
            ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(proxy.edit, imc);
        }
        proxy.composing = false;
    }
    const auto widget = proxy.widget;
    if (commit)
        synchronize(proxy, true, submit);
    else {
        TextEditSnapshot restored;
        if (text_edit_snapshot(widget, restored)) {
            restored.text = proxy.initial;
            restored.composition.clear();
            restored.selection_start = restored.selection_end = restored.text.size();
            (void)text_edit_update(widget, restored);
        }
        if (proxy.action) {
            auto action = proxy.action;
            action(proxy.initial, TextEditPhase::Cancel);
        }
    }
    proxy.widget = nullptr;
    proxy.action = {};
    (void)sao_ui_widget_set_focused(widget, false);
    if (IsWindow(proxy.edit)) {
        HWND edit = std::exchange(proxy.edit, nullptr);
        DestroyWindow(edit);
    }
    if (IsWindow(proxy.owner)) {
        const auto style = GetWindowLongPtrW(proxy.owner, GWL_EXSTYLE);
        SetWindowLongPtrW(proxy.owner, GWL_EXSTYLE,
                          (style & ~WS_EX_NOACTIVATE) | (proxy.previous_style & WS_EX_NOACTIVATE));
    }
    if (IsWindow(proxy.previous_focus) && GetForegroundWindow() == proxy.owner)
        SetFocus(proxy.previous_focus);
    proxy.finishing = false;
}
LRESULT CALLBACK edit_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR,
                           DWORD_PTR ref) {
    auto* proxy = reinterpret_cast<EditProxy*>(ref);
    try {
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, edit_proc, kEditSubclass);
            if (proxy->edit == window)
                proxy->edit = nullptr;
            return DefSubclassProc(window, message, wp, lp);
        }
        if (!proxy->widget || proxy->finishing)
            return DefSubclassProc(window, message, wp, lp);
        if ((message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) && IsWindow(proxy->owner)) {
            // The focused EDIT is deliberately offscreen. Wheel input belongs
            // to the visible compositor viewport under the actual cursor.
            return SendMessageW(proxy->owner, message, wp, lp);
        }
        if (message == WM_IME_STARTCOMPOSITION)
            proxy->composing = true;
        if (message == WM_KEYDOWN && !proxy->composing) {
            TextEditSnapshot state;
            if ((wp == VK_RETURN && text_edit_snapshot(proxy->widget, state) &&
                 (!state.multiline || (GetKeyState(VK_CONTROL) & 0x8000))) ||
                wp == VK_TAB) {
                const HWND owner = proxy->owner;
                const bool traverse = wp == VK_TAB;
                const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                finish(*proxy, true, !traverse);
                if (traverse && IsWindow(owner)) {
                    SetFocus(owner);
                    // The host routes keys from its message loop, not WndProc.
                    // Capture Shift before posting so a quick release is stable.
                    PostMessageW(owner, SAO_UI_NATIVE_TEXT_TAB_MESSAGE, reverse ? 1 : 0, 0);
                }
                return 0;
            }
            if (wp == VK_ESCAPE) {
                finish(*proxy, false);
                return 0;
            }
            if (wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                SendMessageW(window, EM_SETSEL, 0, -1);
                synchronize(*proxy, false);
                return 0;
            }
        }
        if (message == WM_KILLFOCUS) {
            const auto result = DefSubclassProc(window, message, wp, lp);
            finish(*proxy, true);
            return result;
        }
        const auto result = DefSubclassProc(window, message, wp, lp);
        if (message == WM_IME_ENDCOMPOSITION)
            proxy->composing = false;
        if (message == WM_CHAR || message == WM_KEYUP || message == WM_PASTE || message == WM_CUT ||
            message == WM_CLEAR || message == WM_UNDO || message == WM_IME_COMPOSITION ||
            message == WM_IME_ENDCOMPOSITION)
            synchronize(*proxy, false);
        return result;
    } catch (...) {
        return DefSubclassProc(window, message, wp, lp);
    }
}
} // namespace
bool begin_native_text_edit(void* value, sao_ui_widget_handle_t widget, int x, int y, int width,
                            int height, TextEditAction action) noexcept {
    try {
        HWND owner = static_cast<HWND>(value);
        if (!IsWindow(owner) || GetWindowThreadProcessId(owner, nullptr) != GetCurrentThreadId())
            return false;
        TextEditSnapshot state;
        if (!text_edit_snapshot(widget, state) || state.readonly)
            return false;
        auto& pointer = proxies[owner];
        if (!pointer)
            pointer = std::make_unique<EditProxy>();
        auto& proxy = *pointer;
        if (proxy.finishing)
            return false;
        if (proxy.widget == widget && IsWindow(proxy.edit)) {
            SetFocus(proxy.edit);
            return true;
        }
        finish(proxy, true);
        proxy.owner = owner;
        proxy.previous_focus = GetFocus();
        proxy.previous_style = GetWindowLongPtrW(owner, GWL_EXSTYLE);
        proxy.initial = state.text;
        proxy.candidate = {8192 + x + 8, 8192 + y + 20};
        proxy.width = std::max(1, width);
        proxy.height = std::max(1, height);
        const auto text = wide(state.text);
        const DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL |
                            (state.multiline ? ES_MULTILINE | ES_AUTOVSCROLL : 0) |
                            (state.password ? ES_PASSWORD : 0);
        HWND edit = CreateWindowExW(0, L"EDIT", text.c_str(), style, -8192, -8192,
                                    std::max(1, width), std::max(1, height), owner, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
        if (!edit)
            return false;
        proxy.edit = edit;
        if (!SetWindowSubclass(edit, edit_proc, kEditSubclass,
                               reinterpret_cast<DWORD_PTR>(&proxy))) {
            DestroyWindow(edit);
            proxy.edit = nullptr;
            return false;
        }
        proxy.widget = widget;
        proxy.action = std::move(action);
        proxy.composing = false;
        SendMessageW(edit, EM_LIMITTEXT, static_cast<WPARAM>(state.max_length), 0);
        const auto begin =
            wide(state.text.substr(0, std::min(state.selection_start, state.text.size()))).size();
        const auto end =
            wide(state.text.substr(0, std::min(state.selection_end, state.text.size()))).size();
        SendMessageW(edit, EM_SETSEL, begin, end);
        SetWindowLongPtrW(owner, GWL_EXSTYLE, proxy.previous_style & ~WS_EX_NOACTIVATE);
        SetForegroundWindow(owner);
        SetFocus(edit);
        if (HIMC imc = ImmGetContext(edit)) {
            COMPOSITIONFORM composition{};
            composition.dwStyle = CFS_POINT;
            composition.ptCurrentPos = proxy.candidate;
            ImmSetCompositionWindow(imc, &composition);
            CANDIDATEFORM candidate{};
            candidate.dwStyle = CFS_CANDIDATEPOS;
            candidate.ptCurrentPos = composition.ptCurrentPos;
            ImmSetCandidateWindow(imc, &candidate);
            ImmReleaseContext(edit, imc);
        }
        (void)sao_ui_widget_set_focused(widget, true);
        synchronize(proxy, false);
        return true;
    } catch (...) {
        return false;
    }
}
void end_native_text_edit(void* value, bool commit) noexcept {
    try {
        const auto found = proxies.find(static_cast<HWND>(value));
        if (found != proxies.end())
            finish(*found->second, commit);
    } catch (...) {
    }
}
void end_native_text_edit_for_widget(void* value, sao_ui_widget_handle_t widget,
                                     bool commit) noexcept {
    try {
        const auto found = proxies.find(static_cast<HWND>(value));
        if (found != proxies.end() && found->second->widget == widget)
            finish(*found->second, commit);
    } catch (...) {
    }
}
} // namespace sao::ui::detail
#else
namespace sao::ui::detail {
bool begin_native_text_edit(void*, sao_ui_widget_handle_t, int, int, int, int,
                            TextEditAction) noexcept {
    return false;
}
void end_native_text_edit(void*, bool) noexcept {}
void end_native_text_edit_for_widget(void*, sao_ui_widget_handle_t, bool) noexcept {}
} // namespace sao::ui::detail
#endif
