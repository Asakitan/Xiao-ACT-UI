// cs_host.cpp — Wave 4 首切片
//
// hostfxr 生命周期 (gate by SAO_HAS_DOTNET_HOSTFXR):
//   1. LoadLibrary hostfxr.dll (从 %DOTNET_ROOT%\host\fxr\<ver>\ 探测)
//   2. GetProcAddress:
//      - hostfxr_initialize_for_dotnet_command_line
//      - hostfxr_get_runtime_delegate
//      - hostfxr_close
//   3. 初始化 CLR (延迟到实际 create_domain / compile_source)
//
// Wave 4 只完成 host 生命周期 + 版本检测, 真编译留 Wave 5。

#include "sao/plugins/csharp_host/cs_host.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shlwapi.h>
#endif

namespace sao::plugins::csharp_host {

namespace {

#if defined(_WIN32)
using hostfxr_handle = void*;

// 官方 hostfxr 签名 (nethost.h 提取, 免依赖头)
using hostfxr_initialize_fn = int32_t (SAO_PLUGINS_CALL*)(
    int argc, const wchar_t** argv, const void* parameters, hostfxr_handle* host_context_handle);
using hostfxr_get_runtime_delegate_fn = int32_t (SAO_PLUGINS_CALL*)(
    hostfxr_handle host_context_handle, int type, void** delegate);
using hostfxr_close_fn = int32_t (SAO_PLUGINS_CALL*)(hostfxr_handle host_context_handle);
#endif

struct cs_host_impl {
#if defined(_WIN32)
    HMODULE hostfxr_module = nullptr;
    hostfxr_initialize_fn init_fn = nullptr;
    hostfxr_get_runtime_delegate_fn get_delegate_fn = nullptr;
    hostfxr_close_fn close_fn = nullptr;
    hostfxr_handle context = nullptr;
#endif
    std::string runtime_version;   // "8.0.x" 之类
    bool available = false;
};

// 存 last host (进程内单例; Wave 4 只支持一个)
static std::unique_ptr<cs_host_impl>& singleton_slot() {
    static std::unique_ptr<cs_host_impl> slot;
    return slot;
}

// 检测 DOTNET_ROOT 或 PATH 里的 hostfxr.dll
#if defined(_WIN32)
static std::wstring probe_hostfxr_path() {
    // 优先 caller 提供
    // 1. %DOTNET_ROOT%\host\fxr\<ver>\hostfxr.dll (取最新版本目录)
    wchar_t env_root[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"DOTNET_ROOT", env_root, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring fxr_dir = std::wstring(env_root) + L"\\host\\fxr";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((fxr_dir + L"\\*").c_str(), &fd);
        std::wstring best_ver;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
                if (fd.cFileName[0] == L'.') continue;
                std::wstring name = fd.cFileName;
                if (name > best_ver) best_ver = name;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!best_ver.empty()) {
            std::wstring dll = fxr_dir + L"\\" + best_ver + L"\\hostfxr.dll";
            if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return dll;
            }
        }
    }
    // 2. Program Files 默认路径
    std::wstring pf_default = L"C:\\Program Files\\dotnet\\host\\fxr";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((pf_default + L"\\*").c_str(), &fd);
    std::wstring best_ver;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if (fd.cFileName[0] == L'.') continue;
            std::wstring name = fd.cFileName;
            if (name > best_ver) best_ver = name;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!best_ver.empty()) {
        std::wstring dll = pf_default + L"\\" + best_ver + L"\\hostfxr.dll";
        if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return dll;
        }
    }
    return {};
}

static std::string extract_runtime_version_from_path(const std::wstring& dll_path) {
    // 从 ...\host\fxr\<ver>\hostfxr.dll 里抽 <ver>
    if (dll_path.empty()) return {};
    std::wstring::size_type end = dll_path.find_last_of(L"\\/");
    if (end == std::wstring::npos) return {};
    std::wstring parent = dll_path.substr(0, end);
    std::wstring::size_type ver_start = parent.find_last_of(L"\\/");
    if (ver_start == std::wstring::npos) return {};
    std::wstring ver = parent.substr(ver_start + 1);
    std::string out;
    out.reserve(ver.size());
    for (wchar_t c : ver) {
        if (c < 128) out.push_back(static_cast<char>(c));
    }
    return out;
}
#endif

} // namespace

// ── init / shutdown / query ─────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_init(const cs_host_config* cfg, cs_host_handle_t* out_host) {
    if (out_host != nullptr) *out_host = nullptr;

#if !defined(_WIN32)
    (void)cfg;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    auto impl = std::make_unique<cs_host_impl>();

    // 用 caller 指定 or 自动探测
    std::wstring dll_path;
    if (cfg != nullptr && cfg->hostfxr_path != nullptr && cfg->hostfxr_path[0] != L'\0') {
        dll_path = cfg->hostfxr_path;
    } else {
        dll_path = probe_hostfxr_path();
    }
    if (dll_path.empty()) {
        // hostfxr 不可用 — 优雅降级
        impl->available = false;
        auto* raw = impl.release();
        singleton_slot().reset(raw);
        *out_host = reinterpret_cast<cs_host_handle_t>(raw);
        return SAO_ERR_NOT_INITIALIZED;
    }

    impl->hostfxr_module = LoadLibraryW(dll_path.c_str());
    if (impl->hostfxr_module == nullptr) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    impl->init_fn = reinterpret_cast<hostfxr_initialize_fn>(
        GetProcAddress(impl->hostfxr_module, "hostfxr_initialize_for_dotnet_command_line"));
    impl->get_delegate_fn = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        GetProcAddress(impl->hostfxr_module, "hostfxr_get_runtime_delegate"));
    impl->close_fn = reinterpret_cast<hostfxr_close_fn>(
        GetProcAddress(impl->hostfxr_module, "hostfxr_close"));

    if (impl->init_fn == nullptr || impl->get_delegate_fn == nullptr
        || impl->close_fn == nullptr) {
        FreeLibrary(impl->hostfxr_module);
        return SAO_ERR_OS_CALL_FAILED;
    }

    impl->runtime_version = extract_runtime_version_from_path(dll_path);
    impl->available = true;

    // CLR init 延迟到 create_domain / compile_source (Wave 5)
    auto* raw = impl.release();
    singleton_slot().reset(raw);
    *out_host = reinterpret_cast<cs_host_handle_t>(raw);
    return SAO_OK;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_shutdown(cs_host_handle_t host) {
    if (host == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    auto* impl = reinterpret_cast<cs_host_impl*>(host);

#if defined(_WIN32)
    if (impl->context != nullptr && impl->close_fn != nullptr) {
        impl->close_fn(impl->context);
        impl->context = nullptr;
    }
    if (impl->hostfxr_module != nullptr) {
        FreeLibrary(impl->hostfxr_module);
        impl->hostfxr_module = nullptr;
    }
#endif

    // 若是 singleton, 让 unique_ptr 释放
    auto& slot = singleton_slot();
    if (slot.get() == impl) {
        slot.reset();
    } else {
        // 非 singleton (不应发生) — 保守释放
        delete impl;
    }
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_create_domain(cs_host_handle_t host,
                                 const char* /*domain_name_utf8*/,
                                 cs_domain_handle_t* out_domain) {
    if (out_domain != nullptr) *out_domain = nullptr;
    if (host == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    auto* impl = reinterpret_cast<cs_host_impl*>(host);
    if (!impl->available) return SAO_ERR_NOT_INITIALIZED;
    // Wave 5 才真建 AssemblyLoadContext
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_destroy_domain(cs_domain_handle_t /*domain*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_cshost_runtime_version(cs_host_handle_t host) {
    if (host == nullptr) return "";
    auto* impl = reinterpret_cast<cs_host_impl*>(host);
    return impl->runtime_version.c_str();
}

// ── Wave 4 新增: is_available + get_runtime_version(buf) 便捷版本 ─────

// 静态查询: 检测 hostfxr 是否可用 (无需先 init)。
// 供测试和 sdk_binding 用来判断 C# 支持。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_is_available(bool* out_available) {
    if (out_available == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if !defined(_WIN32)
    *out_available = false;
    return SAO_OK;
#else
    std::wstring probed = probe_hostfxr_path();
    *out_available = !probed.empty();
    return SAO_OK;
#endif
}

// 获取 runtime 版本到调用方 buffer (更适合 C ABI, 免生命周期问题)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_runtime_version(cs_host_handle_t host,
                                       char* buf,
                                       size_t buf_size) {
    if (host == nullptr || buf == nullptr || buf_size == 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* impl = reinterpret_cast<cs_host_impl*>(host);
    const std::string& v = impl->runtime_version;
    if (v.size() + 1 > buf_size) {
        buf[0] = '\0';
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(buf, v.c_str(), v.size() + 1);
    return SAO_OK;
}

} // namespace sao::plugins::csharp_host
