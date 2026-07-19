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

#define SAO_PLUGIN_DEPS_PROVIDER_ABI_VERSION_MAJOR 1u
#define SAO_PLUGIN_DEPS_PROVIDER_ABI_VERSION_MINOR 0u
#define SAO_PLUGIN_DEPS_PROVIDER_ABI_VERSION                                            \
    ((SAO_PLUGIN_DEPS_PROVIDER_ABI_VERSION_MAJOR << 16u) |                              \
     SAO_PLUGIN_DEPS_PROVIDER_ABI_VERSION_MINOR)

typedef struct deps_session_s* deps_session_t;

struct deps_session_spec {
    uint32_t struct_size;
    const char* plugin_id_utf8;
    const wchar_t* plugin_dir;
};

struct deps_provider {
    uint32_t abi_version;
    uint32_t struct_size;
    void* user_data;

    void(SAO_PLUGINS_CALL* retain)(void* user_data);
    void(SAO_PLUGINS_CALL* release)(void* user_data);
    int32_t(SAO_PLUGINS_CALL* create_session)(void* user_data,
                                              const deps_session_spec* spec,
                                              void** out_provider_session);
    int32_t(SAO_PLUGINS_CALL* attach_path)(void* user_data, void* provider_session,
                                          const wchar_t* absolute_dir);
    int32_t(SAO_PLUGINS_CALL* restore_path)(void* user_data, void* provider_session,
                                           const wchar_t* absolute_dir);
    int32_t(SAO_PLUGINS_CALL* close_session)(void* user_data, void* provider_session);
};

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

// Exactly one host dependency owner may be registered. The loader copies the
// provider prefix and retains it once per attached session.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_deps_register_provider(const deps_provider* provider);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_deps_unregister_provider();

// Attaches a discovery record transactionally. A failed restore or close
// returns an owned out_session so the caller can retry teardown.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_deps_attach(
    const char* plugin_id_utf8, const wchar_t* plugin_dir,
    const deps_bootstrap_record* record, deps_session_t* out_session);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_deps_session_restore(deps_session_t session);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_deps_session_close(deps_session_t session);

// 卸载时还原路径 (调用 setter 的对偶 —— 由宿主提供 remove_fn)。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_deps_restore(const deps_bootstrap_record* record);

// dist 名 → import 名映射 (Pillow → PIL, PyYAML → yaml, ...)。
const std::unordered_map<std::string, std::string>& dist_to_import_name_map();

} // namespace sao::plugins::loader
