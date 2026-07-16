// py_sandbox.h — Python 层沙箱 (限制 import 白名单等)
//
// 插件 manifest.permissions 里声明的沙箱开关映射到这里的策略:
//   - "fs":     允许 open() / os.path 系, 否则替换成 fake fs
//   - "net":    允许 socket / urllib / requests
//   - "process": 允许 subprocess / os.system (默认拒)
//   - "hotkey":  允许 register_hotkey (SDK 层已控)
//   - "packet_capture": 允许绑定 npcap / winpcap
//   - "memory_access":  允许 ReadProcessMemory (SDK 层控)
//   - "input_control":  允许模拟输入 (SendInput / SetWindowsHookEx)
//   - "engine_access":  允许 register_engine 供他插件 require
//   - "early_load":     早于其他插件加载 (系统级插件)
//   - "unsafe":  关闭所有沙箱 (只对 trusted 内置插件)
//
// 对齐 Python 源: 主平台没做真正的 sandbox (对 python 类插件); C++ 版本
// 加固为可选。默认插件的 permissions=[] 意味着最保守策略。
//
// 白名单 import 机制:
//   - meta_path finder: PathFinder 前插一个 sao_import_gate finder
//   - sao_import_gate.find_spec(name, ...) 检查:
//       1. name 在 always_allowed 集 (builtins, sys, io 等基础) → 直接返回 None
//          让默认机制处理
//       2. name 在 import_whitelist → 同上
//       3. name 在 always_denied 集 (os.system, subprocess, ctypes.util) →
//          抛 ImportError("blocked by sandbox")
//       4. 否则按 permissions 里的 allow_* 判断
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::python_host {

// 权限位 (对齐 plugin.json permissions 字段)
enum class permission_flag : uint32_t {
    none          = 0x0000,
    fs            = 0x0001,
    net           = 0x0002,
    process       = 0x0004,
    hotkey        = 0x0008,
    packet_capture = 0x0010,
    memory_access = 0x0020,
    input_control = 0x0040,
    engine_access = 0x0080,
    early_load    = 0x0100,
    unsafe        = 0x8000,     // 关闭全部沙箱
};

struct py_sandbox_config {
    uint32_t permissions;       // permission_flag 位或
    // 允许 import 的模块白名单 (以外的抛 ImportError)。allow_unsafe=true 时忽略。
    const char* const* import_whitelist;
    uint32_t import_whitelist_count;
    // 显式拒绝的模块黑名单 (即使有 unsafe 也拒, 通常留空)
    const char* const* import_blacklist;
    uint32_t import_blacklist_count;
    // 是否允许 __import__ 深层 / 完整包 (False 时 subpackage 也走白名单)
    bool strict_submodule_check = true;
};

// 为 plugin 装沙箱 (装完对该 plugin 的所有后续 import + open 生效)。
// 内部: 建 sao_import_gate MetaPathFinder, 挂进 sys.meta_path 头部。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_sandbox_arm(const char* plugin_id_utf8,
                               const py_sandbox_config* cfg);

// 卸插件时解除。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_sandbox_disarm(const char* plugin_id_utf8);

// 从 manifest permissions 数组 (字符串列表) 生成 permission_flag 位或。
extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_parse_permissions(const char* const* permissions_utf8,
                                     uint32_t count);

// 已知的默认白名单/黑名单 (unsafe 关闭时用)。
extern "C" SAO_PLUGINS_API const char* const* SAO_PLUGINS_CALL
sao_plugins_pyhost_default_import_whitelist(size_t* out_count);

extern "C" SAO_PLUGINS_API const char* const* SAO_PLUGINS_CALL
sao_plugins_pyhost_default_import_blacklist(size_t* out_count);

} // namespace sao::plugins::python_host
