// cs_plugin_lifecycle.cpp — Wave 8 / Agent d Phase 8 实装
//
// hostfxr API 用法参考:
//   https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting

#include "sao/plugins/csharp_host/cs_plugin_lifecycle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace sao::plugins::csharp_host {

// ── hostfxr type ids (取自 hostfxr.h) ─────────────────────
enum : int32_t {
    hdt_load_assembly_and_get_function_pointer = 5,
};

// ── hostfxr 官方函数签名 (与 wave 4 cs_host.cpp 里的定义对齐) ────
#if defined(_WIN32)
using hostfxr_handle_t = void*;
typedef int32_t (*hostfxr_initialize_for_runtime_config_fn)(
    const wchar_t* runtime_config_path,
    const void* parameters,
    hostfxr_handle_t* host_context_handle);
typedef int32_t (*hostfxr_get_runtime_delegate_fn)(
    hostfxr_handle_t host_context_handle, int type, void** delegate);
typedef int32_t (*hostfxr_close_fn)(hostfxr_handle_t host_context_handle);

// load_assembly_and_get_function_pointer 委托签名
typedef int32_t (*load_assembly_and_get_function_pointer_fn)(
    const wchar_t* assembly_path,
    const wchar_t* type_name,
    const wchar_t* method_name,
    const wchar_t* delegate_type_name,   // or nullptr for UnmanagedCallersOnly
    void* reserved,
    void** delegate_out);
#endif

namespace {

// ── SDK 记账 ──
static std::mutex g_counter_mu;
static cs_sdk_counters g_counters{};

extern "C" void csharp_sdk_log_info(const char* utf8) {
    std::lock_guard<std::mutex> lk(g_counter_mu);
    ++g_counters.log_info_calls;
    size_t n = utf8 ? std::strlen(utf8) : 0;
    if (n >= sizeof(g_counters.last_log_utf8)) n = sizeof(g_counters.last_log_utf8) - 1;
    if (utf8 && n > 0) std::memcpy(g_counters.last_log_utf8, utf8, n);
    g_counters.last_log_utf8[n] = '\0';
}
extern "C" void csharp_sdk_register_ui_panel(const char* utf8) {
    std::lock_guard<std::mutex> lk(g_counter_mu);
    ++g_counters.register_ui_panel_calls;
    size_t n = utf8 ? std::strlen(utf8) : 0;
    if (n >= sizeof(g_counters.last_panel_id_utf8)) n = sizeof(g_counters.last_panel_id_utf8) - 1;
    if (utf8 && n > 0) std::memcpy(g_counters.last_panel_id_utf8, utf8, n);
    g_counters.last_panel_id_utf8[n] = '\0';
}
extern "C" void csharp_sdk_register_hotkey(const char* id_utf8, const char* key_utf8) {
    std::lock_guard<std::mutex> lk(g_counter_mu);
    ++g_counters.register_hotkey_calls;
    size_t n = id_utf8 ? std::strlen(id_utf8) : 0;
    if (n >= sizeof(g_counters.last_hotkey_id_utf8)) n = sizeof(g_counters.last_hotkey_id_utf8) - 1;
    if (id_utf8 && n > 0) std::memcpy(g_counters.last_hotkey_id_utf8, id_utf8, n);
    g_counters.last_hotkey_id_utf8[n] = '\0';
    n = key_utf8 ? std::strlen(key_utf8) : 0;
    if (n >= sizeof(g_counters.last_hotkey_key_utf8)) n = sizeof(g_counters.last_hotkey_key_utf8) - 1;
    if (key_utf8 && n > 0) std::memcpy(g_counters.last_hotkey_key_utf8, key_utf8, n);
    g_counters.last_hotkey_key_utf8[n] = '\0';
}

// ── 迷你 JSON string reader (读 plugin.json 拿 entry / id 字段) ──
std::string extract_json_string_field(const std::string& src, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = src.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
        ++pos;
    if (pos >= src.size() || src[pos] != ':') return {};
    ++pos;
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
        ++pos;
    if (pos >= src.size() || src[pos] != '"') return {};
    ++pos;
    std::string out;
    while (pos < src.size() && src[pos] != '"') {
        char c = src[pos++];
        if (c == '\\' && pos < src.size()) {
            char e = src[pos++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                default: out += e; break;
            }
        } else out += c;
    }
    return out;
}
std::string parent_dir(const std::string& p) {
    auto pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return ".";
    return p.substr(0, pos);
}
std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
std::wstring utf8_to_wide(const std::string& s) {
#if defined(_WIN32)
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

std::string replace_ext(const std::string& p, const std::string& new_ext) {
    auto dot = p.find_last_of('.');
    auto sep = p.find_last_of("\\/");
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
        return p + new_ext;
    return p.substr(0, dot) + new_ext;
}

} // namespace

// ── plugin 结构体 ──

struct cs_plugin_s {
    cs_host_handle_t host = nullptr;
#if defined(_WIN32)
    hostfxr_handle_t hostfxr_ctx = nullptr;
    hostfxr_close_fn close_fn = nullptr;
#endif
    void* fn_init_sdk = nullptr;
    void* fn_on_load = nullptr;
    void* fn_on_tick = nullptr;
    void* fn_on_unload = nullptr;
    void* fn_get_tick = nullptr;
    std::string entry_path;
    std::string plugin_id;
    bool loaded_ok = false;
};

// ── 从 cs_host_impl 里拿 hostfxr module + fn ptrs ─────────
//
// wave 4 cs_host 有 cs_host_impl (internal) 里的 module + init_fn + get_delegate_fn.
// wave 8 我们不 depend on 这个 internal struct; 而是**自己再 GetProcAddress**
// 从 host 的 hostfxr.dll (LoadLibraryW 的 handle 从 cs_host 里没直接暴露).
//
// 折中: 直接自己 LoadLibraryW hostfxr.dll (probe 复用 wave 4 逻辑, 但因为
// 那个是 file-static 的 probe_hostfxr_path, 我们简化: 从 %DOTNET_ROOT% 或
// C:\Program Files\dotnet\host\fxr\*\hostfxr.dll 找).

#if defined(_WIN32)
static std::wstring probe_hostfxr_path_local() {
    // Program Files 默认路径
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
        if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) return dll;
    }
    return {};
}
#endif

// ── 公开 API 实装 ────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_load_plugin(cs_host_handle_t host,
                               const char* plugin_json_path_utf8,
                               cs_plugin_handle_t* out_plugin,
                               char** out_error_utf8) {
    if (out_plugin) *out_plugin = nullptr;
    if (out_error_utf8) *out_error_utf8 = nullptr;
    if (host == nullptr || plugin_json_path_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;

#if !defined(_WIN32)
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    // 读 plugin.json
    std::string manifest_body = read_file(plugin_json_path_utf8);
    if (manifest_body.empty()) {
        const char* m = "cannot read plugin.json";
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m)+1)); if (*out_error_utf8) std::strcpy(*out_error_utf8, m); }
        return SAO_ERR_HANDLE_INVALID;
    }
    std::string entry = extract_json_string_field(manifest_body, "entry");
    if (entry.empty()) entry = "prebuilt/HelloPlugin.dll";
    std::string plugin_id = extract_json_string_field(manifest_body, "id");

    std::string dir = parent_dir(plugin_json_path_utf8);
    std::string dll_path = dir + "/" + entry;
    std::string runtimecfg_path = replace_ext(dll_path, ".runtimeconfig.json");

    // 拿 hostfxr module — 用本地 probe (无法从 cs_host_impl 内窥, 是 file-static)
    std::wstring hostfxr_dll = probe_hostfxr_path_local();
    if (hostfxr_dll.empty()) {
        const char* m = "hostfxr.dll not found on system";
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m)+1)); if (*out_error_utf8) std::strcpy(*out_error_utf8, m); }
        return SAO_ERR_NOT_INITIALIZED;
    }
    HMODULE mod = LoadLibraryW(hostfxr_dll.c_str());
    if (mod == nullptr) {
        const char* m = "LoadLibraryW(hostfxr.dll) failed";
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m)+1)); if (*out_error_utf8) std::strcpy(*out_error_utf8, m); }
        return SAO_ERR_OS_CALL_FAILED;
    }
    auto init_fn = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        GetProcAddress(mod, "hostfxr_initialize_for_runtime_config"));
    auto get_delegate_fn = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        GetProcAddress(mod, "hostfxr_get_runtime_delegate"));
    auto close_fn = reinterpret_cast<hostfxr_close_fn>(
        GetProcAddress(mod, "hostfxr_close"));
    if (!init_fn || !get_delegate_fn || !close_fn) {
        const char* m = "hostfxr symbol lookup failed";
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m)+1)); if (*out_error_utf8) std::strcpy(*out_error_utf8, m); }
        return SAO_ERR_OS_CALL_FAILED;
    }

    std::wstring runtimecfg_w = utf8_to_wide(runtimecfg_path);
    hostfxr_handle_t ctx = nullptr;
    int32_t rc = init_fn(runtimecfg_w.c_str(), nullptr, &ctx);
    // rc > 0: success with warnings; rc == 0: success
    if (rc < 0 || ctx == nullptr) {
        std::string m = "hostfxr_initialize_for_runtime_config failed rc=" + std::to_string(rc)
                      + " cfg=" + runtimecfg_path;
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(m.size()+1)); if (*out_error_utf8) { std::memcpy(*out_error_utf8, m.data(), m.size()); (*out_error_utf8)[m.size()] = '\0'; } }
        return SAO_ERR_OS_CALL_FAILED;
    }

    void* delegate_ptr = nullptr;
    rc = get_delegate_fn(ctx, hdt_load_assembly_and_get_function_pointer, &delegate_ptr);
    if (rc != 0 || delegate_ptr == nullptr) {
        close_fn(ctx);
        std::string m = "hostfxr_get_runtime_delegate failed rc=" + std::to_string(rc);
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(m.size()+1)); if (*out_error_utf8) { std::memcpy(*out_error_utf8, m.data(), m.size()); (*out_error_utf8)[m.size()] = '\0'; } }
        return SAO_ERR_OS_CALL_FAILED;
    }
    auto load_fn = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(delegate_ptr);

    // 4 个函数指针
    std::wstring dll_w = utf8_to_wide(dll_path);
    // 用 forward slash 是 OK 的但改成 backslash 更兼容
    for (auto& c : dll_w) if (c == L'/') c = L'\\';
    const wchar_t* type_name = L"SaoAuto.Plugins.HelloCsharp.HelloPlugin, HelloPlugin";

    auto load_one = [&](const wchar_t* method, void** out) -> int32_t {
        return load_fn(dll_w.c_str(), type_name, method, /*delegate_type*/ nullptr,
                       nullptr, out);
    };

    auto* plugin = new cs_plugin_s();
    plugin->host = host;
    plugin->hostfxr_ctx = ctx;
    plugin->close_fn = close_fn;
    plugin->entry_path = dll_path;
    plugin->plugin_id = plugin_id;

    if (int32_t r = load_one(L"InitSdkPointers", &plugin->fn_init_sdk); r != 0) {
        std::string m = "load InitSdkPointers rc=" + std::to_string(r);
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(m.size()+1)); if (*out_error_utf8) { std::memcpy(*out_error_utf8, m.data(), m.size()); (*out_error_utf8)[m.size()] = '\0'; } }
        close_fn(ctx); delete plugin;
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (int32_t r = load_one(L"OnLoad", &plugin->fn_on_load); r != 0) {
        std::string m = "load OnLoad rc=" + std::to_string(r);
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(m.size()+1)); if (*out_error_utf8) { std::memcpy(*out_error_utf8, m.data(), m.size()); (*out_error_utf8)[m.size()] = '\0'; } }
        close_fn(ctx); delete plugin;
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (int32_t r = load_one(L"OnTick", &plugin->fn_on_tick); r != 0) {
        close_fn(ctx); delete plugin;
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (int32_t r = load_one(L"OnUnload", &plugin->fn_on_unload); r != 0) {
        close_fn(ctx); delete plugin;
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (int32_t r = load_one(L"GetTickCount", &plugin->fn_get_tick); r != 0) {
        close_fn(ctx); delete plugin;
        return SAO_ERR_OS_CALL_FAILED;
    }

    // Reset counters for this plugin session (single-instance).
    {
        std::lock_guard<std::mutex> lk(g_counter_mu);
        g_counters = cs_sdk_counters{};
    }

    // Init SDK bridge — 传 struct pointer + sizeof
    cs_sdk_bridge bridge{
        &csharp_sdk_log_info,
        &csharp_sdk_register_ui_panel,
        &csharp_sdk_register_hotkey,
    };
    typedef int32_t (*init_fn_t)(void*, int32_t);
    auto init_sdk_fn = reinterpret_cast<init_fn_t>(plugin->fn_init_sdk);
    if (int32_t r = init_sdk_fn(&bridge, sizeof(bridge)); r != 0) {
        std::string m = "InitSdkPointers rc=" + std::to_string(r);
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(m.size()+1)); if (*out_error_utf8) { std::memcpy(*out_error_utf8, m.data(), m.size()); (*out_error_utf8)[m.size()] = '\0'; } }
        close_fn(ctx); delete plugin;
        return SAO_ERR_OS_CALL_FAILED;
    }

    // OnLoad
    auto on_load = reinterpret_cast<init_fn_t>(plugin->fn_on_load);
    if (int32_t r = on_load(nullptr, 0); r != 0) {
        std::string m = "OnLoad rc=" + std::to_string(r);
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(m.size()+1)); if (*out_error_utf8) { std::memcpy(*out_error_utf8, m.data(), m.size()); (*out_error_utf8)[m.size()] = '\0'; } }
        close_fn(ctx); delete plugin;
        return SAO_ERR_OS_CALL_FAILED;
    }

    plugin->loaded_ok = true;
    *out_plugin = plugin;
    return SAO_OK;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_tick_plugin(cs_plugin_handle_t plugin, char** out_error_utf8) {
    if (out_error_utf8) *out_error_utf8 = nullptr;
    if (plugin == nullptr || plugin->fn_on_tick == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    typedef int32_t (*fn_t)(void*, int32_t);
    auto fn = reinterpret_cast<fn_t>(plugin->fn_on_tick);
    if (int32_t r = fn(nullptr, 0); r != 0) return SAO_ERR_OS_CALL_FAILED;
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_unload_plugin(cs_plugin_handle_t plugin, char** out_error_utf8) {
    if (out_error_utf8) *out_error_utf8 = nullptr;
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    if (plugin->fn_on_unload) {
        typedef int32_t (*fn_t)(void*, int32_t);
        auto fn = reinterpret_cast<fn_t>(plugin->fn_on_unload);
        fn(nullptr, 0);
    }
    if (plugin->close_fn && plugin->hostfxr_ctx) {
        plugin->close_fn(plugin->hostfxr_ctx);
        plugin->hostfxr_ctx = nullptr;
    }
    delete plugin;
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_sdk_counters(cs_plugin_handle_t plugin, cs_sdk_counters* out) {
    if (plugin == nullptr || out == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(g_counter_mu);
    *out = g_counters;
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_read_tick_count(cs_plugin_handle_t plugin, int32_t* out) {
    if (plugin == nullptr || out == nullptr || plugin->fn_get_tick == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    typedef int32_t (*fn_t)(void*, int32_t);
    auto fn = reinterpret_cast<fn_t>(plugin->fn_get_tick);
    *out = fn(nullptr, 0);
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

} // namespace sao::plugins::csharp_host
