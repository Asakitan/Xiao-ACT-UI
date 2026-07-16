// plugin_isolation.h — 崩溃隔离机制
//
// 两种隔离模式:
//   1. 进程内异常屏障 —— 每个宿主调 script 时用 SEH __try/__except (Win) 或
//      sigaction (POSIX) 兜底; 只捕获脚本崩溃, 不吃平台代码的 bug
//   2. 子进程隔离 (可选, 高危插件) —— 独立子进程跑宿主 + 脚本,
//      通过命名管道 IPC 传 SDK 请求。崩溃只挂掉子进程, 平台不炸
//
// 对齐 Python 源: PluginManager._record_failure / _record_event_failure
// (它只做异常屏障, 没有子进程隔离; C++ 侧后者作为增强)。
#pragma once

#include <cstdint>
#include <string>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/loader/loader_status.h"

namespace sao::plugins::loader {

enum class isolation_mode : uint8_t {
    in_process = 0,      // 默认: 异常屏障, 快, 但脚本可以踩平台内存
    subprocess = 1,      // 子进程 + IPC, 慢, 但真隔离
    subprocess_sandbox,  // + Job Object / seccomp 权限限制
};

struct isolation_config {
    isolation_mode mode = isolation_mode::in_process;
    // 子进程模式: 命名管道名前缀 (id 由 loader 生成)
    std::string ipc_prefix;
    // 子进程模式: 超时 (毫秒), 心跳丢失后强杀
    uint32_t heartbeat_timeout_ms = 5000;
    // 子进程模式: 允许的宿主二进制路径
    std::wstring host_executable;
};

// 为某插件 arm 隔离屏障。当前 loader 未拥有完整的进程/沙箱 primitive，
// 所有模式均 fail-closed 返回 UNSUPPORTED，不登记虚假的已启用状态。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_isolation_arm(plugin_handle_t plugin,
                          const isolation_config* cfg);

// 记录失败并累加计数。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_isolation_record_failure(plugin_handle_t plugin,
                                     const char* utf8_error_message);

// 查询失败计数与最后一次错误信息。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_isolation_failure_stats(plugin_handle_t plugin,
                                    uint32_t* out_failure_count,
                                    const char** out_last_error_utf8);

} // namespace sao::plugins::loader
