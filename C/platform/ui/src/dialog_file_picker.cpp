// dialog_file_picker.cpp — SAOFilePicker port (Phase 13).
//
// Uses IFileDialog (COM CLSID_FileOpenDialog) — the modern Vista+ picker;
// falls back to comdlg32.GetOpenFileNameW via dialog_native_comdlg32.cpp.

#include "sao/ui/dialog.h"

#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shobjidl.h>
#include <objbase.h>
#endif

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_file_picker_show(
    const char* title_utf8, const char* filter_utf8,
    char* out_path_utf8, size_t out_capacity) {
    if (out_path_utf8 == nullptr || out_capacity == 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    out_path_utf8[0] = '\0';
#if defined(_WIN32)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                              COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                          IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg));
    if (FAILED(hr) || dlg == nullptr) {
        if (SUCCEEDED(hr)) CoUninitialize();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    // Set title (UTF-8 → UTF-16).
    if (title_utf8) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, title_utf8, -1, nullptr, 0);
        if (wlen > 0) {
            std::wstring w(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, title_utf8, -1, w.data(), wlen);
            dlg->SetTitle(w.c_str());
        }
    }
    // Parse pipe-separated "Name|*.ext|Name2|*.ext2" into COMDLG_FILTERSPEC[].
    std::vector<COMDLG_FILTERSPEC> filters;
    std::vector<std::wstring> filter_storage;
    if (filter_utf8 && *filter_utf8) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, filter_utf8, -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, filter_utf8, -1, w.data(), wlen);
        size_t pos = 0;
        while (pos < w.size()) {
            size_t bar = w.find(L'|', pos);
            if (bar == std::wstring::npos) break;
            filter_storage.push_back(w.substr(pos, bar - pos)); // name
            pos = bar + 1;
            bar = w.find(L'|', pos);
            std::wstring spec = (bar == std::wstring::npos)
                                    ? w.substr(pos)
                                    : w.substr(pos, bar - pos);
            filter_storage.push_back(std::move(spec));
            if (bar == std::wstring::npos) break;
            pos = bar + 1;
        }
        for (size_t i = 0; i + 1 < filter_storage.size(); i += 2) {
            COMDLG_FILTERSPEC f{filter_storage[i].c_str(),
                                  filter_storage[i + 1].c_str()};
            filters.push_back(f);
        }
        if (!filters.empty()) {
            dlg->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
        }
    }
    hr = dlg->Show(nullptr);
    if (FAILED(hr)) {
        dlg->Release();
        CoUninitialize();
        return SAO_STATUS_ERR_CANCELLED;
    }
    IShellItem* item = nullptr;
    hr = dlg->GetResult(&item);
    if (SUCCEEDED(hr) && item) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            int u8len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0,
                                              nullptr, nullptr);
            if (u8len > 0 && static_cast<size_t>(u8len) <= out_capacity) {
                WideCharToMultiByte(CP_UTF8, 0, path, -1, out_path_utf8,
                                     static_cast<int>(out_capacity), nullptr, nullptr);
            }
            CoTaskMemFree(path);
        }
        item->Release();
    }
    dlg->Release();
    CoUninitialize();
    return out_path_utf8[0] ? SAO_STATUS_OK : SAO_STATUS_ERR_CANCELLED;
#else
    (void)title_utf8;
    (void)filter_utf8;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}
