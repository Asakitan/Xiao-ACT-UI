// dialog_native_comdlg32.cpp — legacy comdlg32.GetOpenFileNameW fallback.
//
// Phase 14 (Python parity closure) — port of act_platform/native_dialog.py.
// Used only when the modern IFileDialog is unavailable.

#include "sao/ui/dialog.h"

#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#endif

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dialog_native_open_file(const char* filter_utf8, char* out_path_utf8,
                                size_t out_capacity) {
    if (out_path_utf8 == nullptr || out_capacity == 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    out_path_utf8[0] = '\0';
#if defined(_WIN32)
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    std::wstring filter_w;
    if (filter_utf8 && *filter_utf8) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, filter_utf8, -1, nullptr, 0);
        if (wlen > 0) {
            filter_w.resize(static_cast<size_t>(wlen));
            MultiByteToWideChar(CP_UTF8, 0, filter_utf8, -1, filter_w.data(), wlen);
            // filter_utf8 uses '|' as separator; convert to embedded NULs.
            for (auto& c : filter_w) {
                if (c == L'|') c = L'\0';
            }
            filter_w.push_back(L'\0');
            ofn.lpstrFilter = filter_w.c_str();
        }
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return SAO_STATUS_ERR_CANCELLED;
    int u8len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0 || static_cast<size_t>(u8len) > out_capacity)
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out_path_utf8,
                         static_cast<int>(out_capacity), nullptr, nullptr);
    return SAO_STATUS_OK;
#else
    (void)filter_utf8;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}
