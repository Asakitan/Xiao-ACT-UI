// plugin_deps.h — 插件外置依赖引导 (对齐 python plugin_deps.py)
//
// 任一插件都可声明 requirements.txt。加载器按顺序让这些依赖存在于插件目录内:
//   1. libs/    —— dev 联网态自动 pip install --target; 冻结态跳过
//   2. vendor/  —— 插件随包自带的纯 Python / .NET / Lua 副本
//   3. site     —— 上面都没有时退回主程序环境; 只作 fallback
//
// C++ 侧的通用抽象: 各宿主 (python_host / lua_host / ...) 都可以调用这一层,
// 传入 "语言侧模块搜索路径 setter", 由本模块把 <plugin>/libs, <plugin>/vendor,
// <plugin>/engine 目录按语言方式前插入。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::loader {

// 依赖引导记录 (对齐 python plugin_deps 返回的 rec 字段)
struct deps_bootstrap_record {
    // 前插到语言侧搜索路径的绝对目录 (卸载时逆序还原)
    std::vector<std::wstring> added_paths;
    // 每个 dist 的解析结果: "libs" / "vendor" / "pip→libs" /
    // "site(fallback)" / "missing"
    std::unordered_map<std::string, std::string> deps_summary;
};

// 语言无关的路径注入回调 —— 各宿主传入自己的 setter (Python 是
// PyList_Insert(sys.path); Lua 是 package.path 拼接; C# 是 AssemblyLoadContext
// 的 resolving 事件; ...)。
using path_prepend_fn = std::function<void(const std::wstring& absolute_dir)>;

// 引导某插件的依赖到语言侧搜索路径。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_deps_ensure(const wchar_t* plugin_dir,
                        bool allow_pip_install,
                        deps_bootstrap_record* out_record);

// 卸载时还原路径 (调用 setter 的对偶 —— 由宿主提供 remove_fn)。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_deps_restore(const deps_bootstrap_record* record);

// dist 名 → import 名映射 (Pillow → PIL, PyYAML → yaml, ...)。
const std::unordered_map<std::string, std::string>& dist_to_import_name_map();

} // namespace sao::plugins::loader
