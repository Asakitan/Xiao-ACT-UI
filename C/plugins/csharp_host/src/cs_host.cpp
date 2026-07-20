// cs_host.cpp — hostfxr 生命周期与版本探测
//
// hostfxr 生命周期 (gate by SAO_HAS_DOTNET_HOSTFXR):
//   1. LoadLibrary hostfxr.dll (从 %DOTNET_ROOT%\host\fxr\<ver>\ 探测)
//   2. GetProcAddress:
//      - hostfxr_initialize_for_dotnet_command_line
//      - hostfxr_get_runtime_delegate
//      - hostfxr_close
//   3. 初始化 CLR (延迟到实际 create_domain / compile_source)
//
// 源码编译接口保留为 fail-closed；组件加载由预编译程序集路径负责。

#include "sao/plugins/csharp_host/cs_host.h"

#include "cs_host_internal.h"

#include "sao/plugins/loader/loader_status.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shlwapi.h>
#include <windows.h>
#endif

namespace sao::plugins::csharp_host {

namespace {

#if defined(_WIN32)
using hostfxr_initialize_for_dotnet_command_line_fn = int32_t(SAO_CSHOST_HOSTFXR_CALLTYPE*)(
    int argc, const wchar_t** argv, const void* parameters, hostfxr_handle_t* host_context_handle);
#endif

struct cs_host_impl {
#if defined(_WIN32)
    HMODULE hostfxr_module = nullptr;
    hostfxr_initialize_for_dotnet_command_line_fn initialize_command_line = nullptr;
    hostfxr_initialize_for_runtime_config_fn initialize_runtime_config = nullptr;
    hostfxr_get_runtime_delegate_fn get_delegate_fn = nullptr;
    hostfxr_close_fn close_fn = nullptr;
    hostfxr_handle_t context = nullptr;
#endif
    std::mutex mutex;
    size_t active_runtime_clients = 0;
    std::string runtime_version; // "8.0.x" 之类
    std::string last_error;
    bool available = false;
};

// 存 last host (进程内单例，只支持一个)
static std::unique_ptr<cs_host_impl>& singleton_slot() {
    static std::unique_ptr<cs_host_impl> slot;
    return slot;
}

static std::mutex& singleton_mutex() {
    static std::mutex mutex;
    return mutex;
}

int32_t copy_to_buffer(const std::string& value, char* buffer, size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0)
        return SAO_ERR_INVALID_ARGUMENT;
    if (value.size() + 1 > buffer_size) {
        buffer[0] = '\0';
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, value.c_str(), value.size() + 1);
    return SAO_OK;
}

#if defined(_WIN32)
int32_t close_hostfxr_guarded(hostfxr_close_fn close, hostfxr_handle_t context) noexcept {
    if (close == nullptr || context == nullptr)
        return SAO_OK;
#if defined(_MSC_VER)
    __try {
        return close(context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    try {
        return close(context);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}
#endif

// 检测 DOTNET_ROOT 或 PATH 里的 hostfxr.dll
#if defined(_WIN32)
std::vector<uint32_t> version_parts(const std::wstring& value) {
    std::vector<uint32_t> parts;
    uint64_t current = 0;
    bool have_digit = false;
    for (const wchar_t ch : value) {
        if (std::iswdigit(ch) != 0) {
            have_digit = true;
            current = std::min<uint64_t>(current * 10 + (ch - L'0'), UINT32_MAX);
        } else if (ch == L'.' || ch == L'-') {
            if (!have_digit)
                break;
            parts.push_back(static_cast<uint32_t>(current));
            current = 0;
            have_digit = false;
            if (ch == L'-')
                break;
        } else {
            break;
        }
    }
    if (have_digit)
        parts.push_back(static_cast<uint32_t>(current));
    return parts;
}

bool version_less(const std::wstring& left, const std::wstring& right) {
    const auto left_parts = version_parts(left);
    const auto right_parts = version_parts(right);
    if (left_parts != right_parts) {
        return std::lexicographical_compare(left_parts.begin(), left_parts.end(),
                                            right_parts.begin(), right_parts.end());
    }
    return left < right;
}

std::wstring probe_hostfxr_under(const std::wstring& dotnet_root) {
    if (dotnet_root.empty())
        return {};
    const std::wstring fxr_dir = dotnet_root + L"\\host\\fxr";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((fxr_dir + L"\\*").c_str(), &fd);
    std::wstring best_ver;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;
            if (fd.cFileName[0] == L'.')
                continue;
            std::wstring name = fd.cFileName;
            if (!version_parts(name).empty() &&
                (best_ver.empty() || version_less(best_ver, name))) {
                best_ver = std::move(name);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!best_ver.empty()) {
        std::wstring dll = fxr_dir + L"\\" + best_ver + L"\\hostfxr.dll";
        if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return dll;
        }
    }
    return {};
}

std::wstring environment_value(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0)
        return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required)
        return {};
    value.resize(written);
    return value;
}

static std::wstring probe_hostfxr_path() {
    for (const auto* variable : {L"DOTNET_ROOT", L"DOTNET_ROOT_X64"}) {
        const auto candidate = probe_hostfxr_under(environment_value(variable));
        if (!candidate.empty())
            return candidate;
    }
    auto program_files = environment_value(L"ProgramFiles");
    if (program_files.empty())
        program_files = L"C:\\Program Files";
    return probe_hostfxr_under(program_files + L"\\dotnet");
}

static std::string extract_runtime_version_from_path(const std::wstring& dll_path) {
    // 从 ...\host\fxr\<ver>\hostfxr.dll 里抽 <ver>
    if (dll_path.empty())
        return {};
    std::wstring::size_type end = dll_path.find_last_of(L"\\/");
    if (end == std::wstring::npos)
        return {};
    std::wstring parent = dll_path.substr(0, end);
    std::wstring::size_type ver_start = parent.find_last_of(L"\\/");
    if (ver_start == std::wstring::npos)
        return {};
    std::wstring ver = parent.substr(ver_start + 1);
    std::string out;
    out.reserve(ver.size());
    for (wchar_t c : ver) {
        if (c < 128)
            out.push_back(static_cast<char>(c));
    }
    return out;
}
#endif

} // namespace

// ── init / shutdown / query ─────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_init(const cs_host_config* cfg, cs_host_handle_t* out_host) {
    if (out_host == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_host = nullptr;

#if !defined(_WIN32)
    (void)cfg;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        std::lock_guard singleton_lock(singleton_mutex());
        if (singleton_slot() != nullptr) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        auto impl = std::make_unique<cs_host_impl>();

        std::wstring dll_path;
        if (cfg != nullptr && cfg->hostfxr_path != nullptr && cfg->hostfxr_path[0] != L'\0') {
            dll_path = cfg->hostfxr_path;
        } else {
            dll_path = probe_hostfxr_path();
        }
        if (dll_path.empty()) {
            impl->available = false;
            auto* raw = impl.get();
            singleton_slot() = std::move(impl);
            *out_host = reinterpret_cast<cs_host_handle_t>(raw);
            return SAO_ERR_NOT_INITIALIZED;
        }

        impl->hostfxr_module =
            LoadLibraryExW(dll_path.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (impl->hostfxr_module == nullptr)
            return SAO_ERR_OS_CALL_FAILED;

        impl->initialize_command_line =
            reinterpret_cast<hostfxr_initialize_for_dotnet_command_line_fn>(
                GetProcAddress(impl->hostfxr_module, "hostfxr_initialize_for_dotnet_command_line"));
        impl->initialize_runtime_config =
            reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
                GetProcAddress(impl->hostfxr_module, "hostfxr_initialize_for_runtime_config"));
        impl->get_delegate_fn = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
            GetProcAddress(impl->hostfxr_module, "hostfxr_get_runtime_delegate"));
        impl->close_fn = reinterpret_cast<hostfxr_close_fn>(
            GetProcAddress(impl->hostfxr_module, "hostfxr_close"));

        if (impl->initialize_command_line == nullptr ||
            impl->initialize_runtime_config == nullptr || impl->get_delegate_fn == nullptr ||
            impl->close_fn == nullptr) {
            FreeLibrary(impl->hostfxr_module);
            return SAO_ERR_OS_CALL_FAILED;
        }

        impl->runtime_version = extract_runtime_version_from_path(dll_path);
        impl->available = true;

        auto* raw = impl.get();
        singleton_slot() = std::move(impl);
        *out_host = reinterpret_cast<cs_host_handle_t>(raw);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_shutdown(cs_host_handle_t host) {
    if (host == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard singleton_lock(singleton_mutex());
        auto& slot = singleton_slot();
        auto* impl = reinterpret_cast<cs_host_impl*>(host);
        if (slot.get() != impl)
            return SAO_ERR_HANDLE_INVALID;
        {
            std::lock_guard lock(impl->mutex);
            if (impl->active_runtime_clients != 0) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            impl->last_error.clear();
        }

#if defined(_WIN32)
        if (impl->context != nullptr && impl->close_fn != nullptr) {
            const int32_t close_status = close_hostfxr_guarded(impl->close_fn, impl->context);
            if (close_status != 0) {
                impl->last_error = "hostfxr_close failed rc=" + std::to_string(close_status);
                return SAO_ERR_OS_CALL_FAILED;
            }
            impl->context = nullptr;
        }
        if (impl->hostfxr_module != nullptr) {
            if (FreeLibrary(impl->hostfxr_module) == 0) {
                impl->last_error =
                    "FreeLibrary(hostfxr) failed win32=" + std::to_string(GetLastError());
                return SAO_ERR_OS_CALL_FAILED;
            }
            impl->hostfxr_module = nullptr;
        }
#endif
        slot.reset();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_create_domain(
    cs_host_handle_t host, const char* /*domain_name_utf8*/, cs_domain_handle_t* out_domain) {
    if (out_domain != nullptr)
        *out_domain = nullptr;
    if (host == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard singleton_lock(singleton_mutex());
        auto* impl = reinterpret_cast<cs_host_impl*>(host);
        if (singleton_slot().get() != impl)
            return SAO_ERR_HANDLE_INVALID;
        if (!impl->available)
            return SAO_ERR_NOT_INITIALIZED;
        return SAO_ERR_NOT_IMPLEMENTED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_destroy_domain(cs_domain_handle_t /*domain*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_cshost_runtime_version(cs_host_handle_t host) {
    thread_local std::string version_snapshot;
    version_snapshot.clear();
    if (host == nullptr)
        return version_snapshot.c_str();
    try {
        auto* impl = reinterpret_cast<cs_host_impl*>(host);
        std::lock_guard lock(singleton_mutex());
        if (singleton_slot().get() != impl)
            return version_snapshot.c_str();
        version_snapshot = impl->runtime_version;
        return version_snapshot.c_str();
    } catch (...) {
        version_snapshot.clear();
        return version_snapshot.c_str();
    }
}

// ── is_available + get_runtime_version(buf) 便捷版本 ─────────────────

// 静态查询: 检测 hostfxr 是否可用 (无需先 init)。
// 供测试和 sdk_binding 用来判断 C# 支持。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_is_available(bool* out_available) {
    if (out_available == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if !defined(_WIN32)
    *out_available = false;
    return SAO_OK;
#else
    try {
        const std::wstring probed = probe_hostfxr_path();
        *out_available = !probed.empty();
        return SAO_OK;
    } catch (...) {
        *out_available = false;
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}

// 获取 runtime 版本到调用方 buffer (更适合 C ABI, 免生命周期问题)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_runtime_version(cs_host_handle_t host, char* buf, size_t buf_size) {
    if (host == nullptr || buf == nullptr || buf_size == 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(singleton_mutex());
        auto* impl = reinterpret_cast<cs_host_impl*>(host);
        if (singleton_slot().get() != impl)
            return SAO_ERR_HANDLE_INVALID;
        return copy_to_buffer(impl->runtime_version, buf, buf_size);
    } catch (...) {
        buf[0] = '\0';
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_get_assembly_unload_mode(
    cs_host_handle_t host, cs_assembly_unload_mode* out_mode) {
    if (host == nullptr || out_mode == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(singleton_mutex());
        if (singleton_slot().get() != reinterpret_cast<cs_host_impl*>(host))
            return SAO_ERR_HANDLE_INVALID;
        *out_mode = cs_assembly_unload_mode::process_resident;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_last_error(cs_host_handle_t host, char* buf, size_t buf_size) {
    if (host == nullptr || buf == nullptr || buf_size == 0)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard singleton_lock(singleton_mutex());
        auto* impl = reinterpret_cast<cs_host_impl*>(host);
        if (singleton_slot().get() != impl)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(impl->mutex);
        return copy_to_buffer(impl->last_error, buf, buf_size);
    } catch (...) {
        buf[0] = '\0';
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t cshost_acquire_runtime_api(cs_host_handle_t host, cshost_runtime_api& out_api) noexcept {
    out_api = {};
#if !defined(_WIN32)
    (void)host;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (host == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard singleton_lock(singleton_mutex());
        auto* impl = reinterpret_cast<cs_host_impl*>(host);
        if (singleton_slot().get() != impl)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(impl->mutex);
        if (!impl->available || impl->hostfxr_module == nullptr) {
            return SAO_ERR_NOT_INITIALIZED;
        }
        if (impl->initialize_runtime_config == nullptr || impl->get_delegate_fn == nullptr ||
            impl->close_fn == nullptr) {
            return SAO_ERR_OS_CALL_FAILED;
        }
        ++impl->active_runtime_clients;
        out_api.module = impl->hostfxr_module;
        out_api.initialize_for_runtime_config = impl->initialize_runtime_config;
        out_api.get_runtime_delegate = impl->get_delegate_fn;
        out_api.close = impl->close_fn;
        out_api.owner = host;
        return SAO_OK;
    } catch (...) {
        out_api = {};
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}

void cshost_release_runtime_api(cshost_runtime_api& api) noexcept {
#if defined(_WIN32)
    try {
        std::lock_guard singleton_lock(singleton_mutex());
        auto* impl = reinterpret_cast<cs_host_impl*>(api.owner);
        if (singleton_slot().get() == impl && impl != nullptr) {
            std::lock_guard lock(impl->mutex);
            if (impl->active_runtime_clients > 0) {
                --impl->active_runtime_clients;
            }
        }
    } catch (...) {
    }
#endif
    api = {};
}

} // namespace sao::plugins::csharp_host
