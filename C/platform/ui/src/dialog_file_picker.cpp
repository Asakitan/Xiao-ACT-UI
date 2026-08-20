// dialog_file_picker.cpp — SAOFilePicker port (Phase 13).
//
// Uses IFileDialog (COM CLSID_FileOpenDialog) — the modern Vista+ picker;
// falls back to comdlg32.GetOpenFileNameW via dialog_native_comdlg32.cpp.

#include "sao/ui/dialog.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shobjidl.h>
#include <objbase.h>
#endif

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dialog_native_open_file(const char* filter_utf8, char* out_path_utf8,
                               size_t out_capacity);

#if defined(_WIN32)
namespace {

template <typename T>
struct ComRelease {
    void operator()(T* value) const noexcept {
        if (value != nullptr)
            value->Release();
    }
};

template <typename T>
using ComPtr = std::unique_ptr<T, ComRelease<T>>;

struct CoTaskMemFreeDeleter {
    void operator()(wchar_t* value) const noexcept {
        if (value != nullptr)
            CoTaskMemFree(value);
    }
};

class ComInitialization final {
public:
    ComInitialization() noexcept
        : result_(CoInitializeEx(nullptr,
                                 COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)),
          owns_uninitialize_(result_ == S_OK || result_ == S_FALSE) {}

    ~ComInitialization() {
        if (owns_uninitialize_)
            CoUninitialize();
    }

    ComInitialization(const ComInitialization&) = delete;
    ComInitialization& operator=(const ComInitialization&) = delete;

    HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
    bool owns_uninitialize_;
};

bool is_picker_cancelled(HRESULT result) noexcept {
    return result == E_ABORT || result == HRESULT_FROM_WIN32(ERROR_CANCELLED);
}

bool is_picker_unavailable(HRESULT result) noexcept {
    return result == REGDB_E_CLASSNOTREG || result == CLASS_E_CLASSNOTAVAILABLE ||
           result == E_NOINTERFACE;
}

}  // namespace
#endif

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_file_picker_show(
    const char* title_utf8, const char* filter_utf8,
    char* out_path_utf8, size_t out_capacity) {
    try {
        if (out_path_utf8 == nullptr || out_capacity == 0)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        out_path_utf8[0] = '\0';
#if defined(_WIN32)
        ComInitialization com;
        if (com.result() == RPC_E_CHANGED_MODE || FAILED(com.result()))
            return SAO_STATUS_ERR_OS_CALL_FAILED;

        IFileOpenDialog* raw_dialog = nullptr;
        const HRESULT create_hr = CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog,
            reinterpret_cast<void**>(&raw_dialog));
        ComPtr<IFileOpenDialog> dialog(raw_dialog);
        if (FAILED(create_hr) || dialog == nullptr) {
            if (is_picker_unavailable(create_hr)) {
                return sao_ui_dialog_native_open_file(filter_utf8, out_path_utf8,
                                                      out_capacity);
            }
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }

        HRESULT hr = S_OK;
        // Set title (UTF-8 → UTF-16).
        if (title_utf8 != nullptr) {
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, title_utf8, -1,
                                                 nullptr, 0);
            if (wlen <= 0)
                return SAO_STATUS_ERR_OS_CALL_FAILED;
            std::wstring title(static_cast<size_t>(wlen), L'\0');
            if (MultiByteToWideChar(CP_UTF8, 0, title_utf8, -1, title.data(),
                                    wlen) != wlen)
                return SAO_STATUS_ERR_OS_CALL_FAILED;
            hr = dialog->SetTitle(title.c_str());
            if (FAILED(hr))
                return SAO_STATUS_ERR_OS_CALL_FAILED;
        }

        // Parse pipe-separated "Name|*.ext|Name2|*.ext2" into
        // COMDLG_FILTERSPEC[].
        std::vector<COMDLG_FILTERSPEC> filters;
        std::vector<std::wstring> filter_storage;
        if (filter_utf8 != nullptr && *filter_utf8 != '\0') {
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, filter_utf8, -1,
                                                 nullptr, 0);
            if (wlen <= 0)
                return SAO_STATUS_ERR_OS_CALL_FAILED;
            std::wstring filter(static_cast<size_t>(wlen), L'\0');
            if (MultiByteToWideChar(CP_UTF8, 0, filter_utf8, -1, filter.data(),
                                    wlen) != wlen)
                return SAO_STATUS_ERR_OS_CALL_FAILED;

            size_t pos = 0;
            while (pos < filter.size()) {
                size_t bar = filter.find(L'|', pos);
                if (bar == std::wstring::npos)
                    break;
                filter_storage.push_back(filter.substr(pos, bar - pos));
                pos = bar + 1;
                bar = filter.find(L'|', pos);
                std::wstring spec = bar == std::wstring::npos
                                        ? filter.substr(pos)
                                        : filter.substr(pos, bar - pos);
                filter_storage.push_back(std::move(spec));
                if (bar == std::wstring::npos)
                    break;
                pos = bar + 1;
            }
            for (size_t i = 0; i + 1 < filter_storage.size(); i += 2) {
                filters.push_back(
                    {filter_storage[i].c_str(), filter_storage[i + 1].c_str()});
            }
            if (!filters.empty()) {
                hr = dialog->SetFileTypes(static_cast<UINT>(filters.size()),
                                          filters.data());
                if (FAILED(hr))
                    return SAO_STATUS_ERR_OS_CALL_FAILED;
            }
        }

        hr = dialog->Show(nullptr);
        if (FAILED(hr))
            return is_picker_cancelled(hr) ? SAO_STATUS_ERR_CANCELLED
                                           : SAO_STATUS_ERR_OS_CALL_FAILED;

        IShellItem* raw_item = nullptr;
        hr = dialog->GetResult(&raw_item);
        ComPtr<IShellItem> item(raw_item);
        if (FAILED(hr) || item == nullptr)
            return SAO_STATUS_ERR_OS_CALL_FAILED;

        PWSTR raw_path = nullptr;
        hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path);
        std::unique_ptr<wchar_t, CoTaskMemFreeDeleter> path(raw_path);
        if (FAILED(hr) || path == nullptr)
            return SAO_STATUS_ERR_OS_CALL_FAILED;

        const int u8len = WideCharToMultiByte(CP_UTF8, 0, path.get(), -1,
                                              nullptr, 0, nullptr, nullptr);
        if (u8len <= 0)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        if (static_cast<size_t>(u8len) > out_capacity)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        if (WideCharToMultiByte(CP_UTF8, 0, path.get(), -1, out_path_utf8,
                                u8len, nullptr, nullptr) != u8len)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        return SAO_STATUS_OK;
#else
        (void)title_utf8;
        (void)filter_utf8;
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        if (out_path_utf8 != nullptr && out_capacity != 0)
            out_path_utf8[0] = '\0';
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}