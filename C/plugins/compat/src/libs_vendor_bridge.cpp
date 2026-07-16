// libs_vendor_bridge.cpp — Wave 4 首切片
//
// discover / probe / format 三块:
//   1. discover_deps_dirs (老 API, 保留 stub OK)
//   2. libs_vendor_probe (Wave 4 新): 扫 libs/ + vendor/
//   3. format_python_sys_path / format_lua_package_path (Wave 4 新)

#include "sao/plugins/compat/libs_vendor_bridge.h"

#include <filesystem>
#include <string>

namespace sao::plugins::compat {

namespace {

bool dir_exists(const std::wstring& p) {
    std::error_code ec;
    return std::filesystem::is_directory(std::filesystem::path(p), ec);
}

// 拼终止字符串到 buf, 返回所需长度 (含终止符)。空间不够时不写 (给
// out_size==0 计算长度用)。
int32_t copy_to_wbuf(const std::wstring& src,
                     wchar_t* out_buf,
                     size_t out_size) {
    if (out_buf == nullptr || out_size == 0) {
        // 只查所需长度
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    if (src.size() + 1 > out_size) {
        out_buf[0] = L'\0';
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    for (size_t i = 0; i < src.size(); ++i) out_buf[i] = src[i];
    out_buf[src.size()] = L'\0';
    return SAO_OK;
}

} // namespace

// ── 保留旧 stub ──────────────────────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_discover_deps_dirs(const wchar_t* /*plugin_dir*/,
                                      discovered_deps_dirs* out_dirs) {
    if (out_dirs != nullptr) *out_dirs = discovered_deps_dirs{};
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_compat_free_deps_dirs(discovered_deps_dirs* dirs) {
    if (dirs == nullptr) return;
    dirs->engine_dirs.clear();
    dirs->libs_dirs.clear();
    dirs->vendor_dirs.clear();
    dirs->ordered.clear();
}

// ── Wave 4 新增: probe + format ─────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_libs_vendor_probe(const wchar_t* plugin_dir,
                                     discovered_deps_dirs* out_dirs) {
    if (out_dirs == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_dirs = discovered_deps_dirs{};
    if (plugin_dir == nullptr || plugin_dir[0] == L'\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    std::wstring base(plugin_dir);
    // 去尾部斜杠 (支持带/不带都行)
    while (!base.empty() && (base.back() == L'/' || base.back() == L'\\')) {
        base.pop_back();
    }
    if (base.empty()) return SAO_ERR_INVALID_ARGUMENT;

    const std::wstring engine_dir = base + L"\\engine";
    const std::wstring libs_dir = base + L"\\libs";
    const std::wstring vendor_dir = base + L"\\vendor";

    if (dir_exists(engine_dir)) {
        out_dirs->engine_dirs.push_back(engine_dir);
        out_dirs->ordered.push_back(engine_dir);
    }
    if (dir_exists(libs_dir)) {
        out_dirs->libs_dirs.push_back(libs_dir);
        out_dirs->ordered.push_back(libs_dir);
    }
    if (dir_exists(vendor_dir)) {
        out_dirs->vendor_dirs.push_back(vendor_dir);
        out_dirs->ordered.push_back(vendor_dir);
    }
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_format_python_sys_path(const discovered_deps_dirs* dirs,
                                          wchar_t* out_buf,
                                          size_t out_size) {
    if (dirs == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    // Python os.pathsep: Windows=';', POSIX=':'
#if defined(_WIN32)
    const wchar_t sep = L';';
#else
    const wchar_t sep = L':';
#endif
    std::wstring joined;
    bool first = true;
    for (const auto& d : dirs->ordered) {
        if (!first) joined.push_back(sep);
        joined += d;
        first = false;
    }
    return copy_to_wbuf(joined, out_buf, out_size);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_format_lua_package_path(const discovered_deps_dirs* dirs,
                                           wchar_t* out_buf,
                                           size_t out_size) {
    if (dirs == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    // Lua package.path pattern: "<dir>/?.lua;<dir>/?/init.lua"
    std::wstring joined;
    bool first = true;
    for (const auto& d : dirs->ordered) {
        if (!first) joined.push_back(L';');
        joined += d;
        joined += L"\\?.lua;";
        joined += d;
        joined += L"\\?\\init.lua";
        first = false;
    }
    return copy_to_wbuf(joined, out_buf, out_size);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_format_paths_for_language(
    const discovered_deps_dirs* /*dirs*/,
    sao::plugins::loader::engine_kind /*language*/,
    wchar_t*** out_paths,
    size_t* out_count) {
    // 保留 stub (旧签名不动)
    if (out_paths != nullptr) *out_paths = nullptr;
    if (out_count != nullptr) *out_count = 0;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_compat_free_paths(wchar_t** /*paths*/, size_t /*count*/) {
    // 保留 stub
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_compat_has_requirements_txt(const wchar_t* plugin_dir) {
    if (plugin_dir == nullptr || plugin_dir[0] == L'\0') return false;
    std::wstring path(plugin_dir);
    while (!path.empty() && (path.back() == L'/' || path.back() == L'\\')) {
        path.pop_back();
    }
    path += L"\\requirements.txt";
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

} // namespace sao::plugins::compat
