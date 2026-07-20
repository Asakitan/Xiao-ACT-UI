// as_plugin_lifecycle.h — AngelScript 插件生命周期
//
// 在内存源码执行之外提供完整插件加载:
//   1. 从 plugin.json 找 entry .as
//   2. 读 .as 文件, AddScriptSection + Build
//   3. 注册 3 条 legacy compatibility 全局函数:
//        - void log_info(const string& in msg)
//        - void register_ui_panel(const string& in panel_id)
//        - void register_hotkey(const string& in hotkey_id, const string& in default_key)
//   4. 调 on_load() / on_tick() / on_unload() 三个全局 no-arg 函数
//   5. 可读取 compatibility 调用诊断计数
//
// 本 legacy API 没有 loader canonical context，因此不声明完整 sdk_binding
// PluginContext 能力；需要平台 SDK 的插件必须走 loader adapter API。

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

#include "sao/plugins/angel_host/as_host.h"

namespace sao::plugins::angel_host {

typedef struct as_plugin_s* as_plugin_handle_t;

// Legacy compatibility 全局函数的诊断计数；不表示平台 SDK 注册成功。
struct as_sdk_counters {
    uint32_t log_info_calls = 0;
    uint32_t register_ui_panel_calls = 0;
    uint32_t register_hotkey_calls = 0;
    // 收集的最后一条 log 消息 (test 断言用)
    char last_log_utf8[512] = {0};
    char last_panel_id_utf8[128] = {0};
    char last_hotkey_id_utf8[128] = {0};
    char last_hotkey_key_utf8[64] = {0};
};

// 加载 plugin.json 指定的插件 (读 entry .as → 编译 → 注册 SDK → 调 on_load).
//
// plugin_json_path_utf8: 指向插件目录里的 plugin.json 绝对/相对路径.
//   entry 从 JSON 里读, 若缺则默认 "main.as", 与 plugin.json 同目录.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_load_plugin(as_host_handle_t host, const char* plugin_json_path_utf8,
                               as_plugin_handle_t* out_plugin, char** out_error_utf8);

// 调 on_tick() 一次.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_tick_plugin(as_plugin_handle_t plugin, char** out_error_utf8);

// 调 on_unload() 并释放插件资源 (但保留 host engine, 由调用方 shutdown).
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unload_plugin(as_plugin_handle_t plugin, char** out_error_utf8);

// 读 legacy compatibility 诊断计数器。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_get_sdk_counters(as_plugin_handle_t plugin, as_sdk_counters* out_counters);

// 主动调一个已定义的全局 no-arg 函数 (供 test 探测 tick_count 全局).
// 若函数不存在返回 SAO_ERR_HANDLE_INVALID.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ashost_read_global_int(
    as_plugin_handle_t plugin, const char* global_var_name_utf8, int32_t* out_value);

} // namespace sao::plugins::angel_host
