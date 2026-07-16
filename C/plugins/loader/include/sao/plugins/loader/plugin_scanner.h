// plugin_scanner.h — 目录扫描 (plugins/* + user_plugins/*)
//
// 对齐 Python 源: PluginManager.discover / sync_discovery / refresh_plugin,
// 以及 default_plugin_dirs() (workspace-root 向上走 marker 探测)。
//
// C++ 侧用 std::filesystem 迭代目录, 每个子目录一个 manifest_path 判断,
// 用 mtime + size 生成签名做缓存, 未变则复用旧记录。
//
// 扫描规则 (对齐 Python 平台):
//   1. 内置根 (builtin_roots)   — <base>/plugins/, 只读, 不允许 uninstall
//   2. 用户根 (user_roots)      — <base>/user_plugins/, 可写, 允许 uninstall
//   3. workspace 根 (workspace) — 向上走探测 .git / sao_auto / .vscode / tools
//                                markers, 找到后再看 <workspace>/plugins/
//   4. 每根按 max_depth 递归 (默认 1 = 只扫直接子目录)
//   5. 每目录里若存在 plugin.json 即当作一个插件目录
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/loader/plugin_manifest.h"

namespace sao::plugins::loader {

// 扫描配置
struct scan_config {
    // 系统内置 plugins/ 目录 (只读, 不允许 uninstall)
    std::vector<std::wstring> builtin_roots;
    // 用户 user_plugins/ 目录 (可写, 允许 uninstall)
    std::vector<std::wstring> user_roots;
    // 是否走 workspace-root 探测 (对齐 Python default_plugin_dirs)
    bool enable_workspace_walkup = true;
    // 每目录深度上限, 默认 1 (只扫直接子目录, 与 Python 平台一致)
    uint32_t max_depth = 1;
};

// 扫描结果一条记录
struct scanned_plugin {
    plugin_manifest manifest;
    bool is_user_installed = false;   // 位于 user_roots 之下
    bool is_workspace_plugin = false; // 通过 walkup 找到
    uint64_t manifest_mtime_ns = 0;
    uint64_t manifest_size = 0;
};

// 扫描所有配置根, 返回发现的所有 plugin manifest。
// out_plugins 由 sao_plugins_scanner_free 释放。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_scanner_discover(const scan_config* cfg,
                             scanned_plugin** out_plugins,
                             size_t* out_count);

// 释放 sao_plugins_scanner_discover 分配的数组。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_scanner_free(scanned_plugin* plugins, size_t count);

// 单目录轻量刷新 (对齐 Python refresh_plugin, 用于 one-click import 完成后)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_scanner_refresh_one(const wchar_t* plugin_dir,
                                scanned_plugin* out_plugin);

// 计算发现签名 (对齐 Python _discovery_signature): 供上层判断是否需重扫。
// 组成: 每个根的 mtime + 每个 manifest.json 的 mtime + size 全部 hash。
extern "C" SAO_PLUGINS_API uint64_t SAO_PLUGINS_CALL
sao_plugins_scanner_signature(const scan_config* cfg);

// 向上走探测 workspace 根目录 (对齐 Python default_plugin_dirs 的 walkup)。
// 从 start_dir 向上找第一个含 .git / sao_auto / tools / .vscode / .github
// marker 的目录, 返回其绝对路径; 找不到返回空串。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_scanner_find_workspace_root(const wchar_t* start_dir,
                                        wchar_t** out_root_path);

// 释放 find_workspace_root 分配的字符串。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_scanner_free_wstring(wchar_t* str);

// 判断给定目录是否是有效插件目录 (含 plugin.json)。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_scanner_is_plugin_dir(const wchar_t* plugin_dir);

} // namespace sao::plugins::loader
