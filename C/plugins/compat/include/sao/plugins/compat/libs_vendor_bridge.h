// libs_vendor_bridge.h — 老插件的 libs/ + vendor/ 目录处理
//
// 老 midi_piano_plugin 引入的通用能力 (对齐 python plugin_deps.py):
// 每个插件目录里的 ``libs/`` (pip install --target 位置)
// 和 ``vendor/`` (作者随包纯 python) 自动前插到语言侧模块搜索路径。
//
// 新 C++ 平台里各宿主对齐:
//   - Python: PyList_Insert(sys.path, 0, libs/ 或 vendor/ 或 engine/)
//   - Lua: package.path 前插 libs/?.lua / vendor/?.lua
//   - AS: 装载 addon 前先扫这两目录 (as SDK 用 IIncludeCallback)
//   - C#: AssemblyLoadContext.Resolving 事件里优先查 libs/ / vendor/
//   - Emma: 内置 load_script 从这两目录找
//
// 这一层给出**语言无关**的目录发现和路径列表; 各宿主用它注册到自己
// 语言侧。
//
// 优先级 (最高到最低; 对齐 plugin_deps.py 的 _safe_plugin_insert_index):
//   1. <plugin>/engine/         — 插件自带核心库 (最高)
//   2. <plugin>/libs/            — pip install --target 缓存位
//   3. <plugin>/vendor/          — 作者手工随包 (纯 python)
//   4. platform site-packages   — 主程序环境 fallback (最低)
#pragma once

#include <cstdint>
#include <vector>
#include <string>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/loader/plugin_manifest.h"

namespace sao::plugins::compat {

struct discovered_deps_dirs {
    // 老 midi_piano 期望的顺序: engine/ libs/ vendor/ (前者优先级最高)
    std::vector<std::wstring> engine_dirs;
    std::vector<std::wstring> libs_dirs;
    std::vector<std::wstring> vendor_dirs;
    // 摊平版本 (按老优先级顺序): engine → libs → vendor
    std::vector<std::wstring> ordered;
};

// 发现某插件目录下的老 libs/vendor 结构。
// 内部实测目录存在性 (std::filesystem::is_directory), 不存在的路径不加入。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_discover_deps_dirs(const wchar_t* plugin_dir,
                                      discovered_deps_dirs* out_dirs);

// 释放 out_dirs 里可能分配的 wchar 存储 (不透明容器)。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_compat_free_deps_dirs(discovered_deps_dirs* dirs);

// 按语言把发现的目录转成语言侧路径格式 (给各宿主使用):
//   - Python: 直接返回绝对目录字符串列表 (Py 侧 PyList_Insert(sys.path))
//   - Lua: <dir>/?.lua;<dir>/?/init.lua 拼进 package.path
//   - AS: 只返回目录列表, AS 的 IIncludeCallback 用
//   - C#: 目录列表, LoadContext.Resolving 用
//   - Emma: 目录列表, 内置 load_script 用
// out_paths 归属调用方 free (以 wchar_t** 数组返回)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_format_paths_for_language(
    const discovered_deps_dirs* dirs,
    sao::plugins::loader::engine_kind language,
    wchar_t*** out_paths,
    size_t* out_count);

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_compat_free_paths(wchar_t** paths, size_t count);

// 判断插件是否 requires 声明了外置 requirements.txt (影响 install 触发)。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_compat_has_requirements_txt(const wchar_t* plugin_dir);

// ── Wave 4 新增: runtime probe API ──────────────────────────

// 扫 plugin_dir/libs/ + plugin_dir/vendor/, 返回目录字符串列表。
// out_dirs 归属调用方 free。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_libs_vendor_probe(const wchar_t* plugin_dir,
                                     discovered_deps_dirs* out_dirs);

// 把发现的 dirs 拼成 os.pathsep (Win=';', POSIX=':') 分隔的 Python sys.path
// 字符串, 写到 out_buf。out_size 为 buf 长度 (含终止符)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_format_python_sys_path(const discovered_deps_dirs* dirs,
                                          wchar_t* out_buf,
                                          size_t out_size);

// 把发现的 dirs 拼成 Lua package.path pattern (';' 分隔, 每目录展开为
// "<dir>/?.lua;<dir>/?/init.lua")。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_format_lua_package_path(const discovered_deps_dirs* dirs,
                                           wchar_t* out_buf,
                                           size_t out_size);

} // namespace sao::plugins::compat
