// plugin_install.h — 一键 zip 导入 (对齐 python plugin_install.py)
//
// 用户拿到一个 .zip 插件包, 双击丢进 SAO 或用 UI "导入插件":
//   1. 解压到临时目录 (zip-slip 防穿越, 拒绝任何 .. 越界)
//   2. 定位 plugin.json (根部或唯一一层子目录)
//   3. 校验 manifest 合法性
//   4. 移进 <base>/user_plugins/<plugin_id>/
//   5. 交给 scanner + registry, 不做加载 —— 上层 refresh_plugin + enable
#pragma once

#include <cstdint>
#include <string>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/loader/loader_status.h"

namespace sao::plugins::loader {

struct install_result {
    bool ok = false;
    bool replaced = false;              // 覆盖了同 id 老版本
    std::string plugin_id;
    std::string name;
    std::string version;
    std::string previous_version;       // 覆盖前的版本, 用于"回滚"提示
    std::wstring installed_path;
    std::string message;                // 失败原因或成功提示
};

// 一键导入 .zip 到指定 user_plugins 目录。
//
// 出参 result 由调用方分配。allow_replace=false 且同 id 已存在时拒绝并回填
// result.message + previous_version, 上层 UI 决定是否二次确认覆盖。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_install_archive(const wchar_t* archive_path,
                            const wchar_t* user_plugins_dir,
                            bool allow_replace,
                            install_result* result);

// 卸载 (对齐 Python act_plugin_uninstall_dialog 语义):
// 拒绝非用户插件, 拒绝仍在运行时。删除目录 (可选进回收站)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_uninstall_plugin(const char* plugin_id,
                             bool to_recycle_bin);

// zip-slip 校验辅助 (导出以便宿主别处复用)。
bool path_is_within_base(const std::wstring& base, const std::wstring& target);

} // namespace sao::plugins::loader
