// Direct precompiled-DLL compatibility API. Production loader integration is
// exposed by cs_loader_adapter.h and uses manifest managed_type/runtimeconfig.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

#include "sao/plugins/csharp_host/cs_error.h"
#include "sao/plugins/csharp_host/cs_host.h"

namespace sao::plugins::csharp_host {

typedef struct cs_plugin_s* cs_plugin_handle_t;

inline constexpr uint32_t SAO_CSHOST_MANAGED_ABI_VERSION = 1;

// The first three fields preserve the legacy fixture layout. New managed
// components must validate struct_size and abi_version before reading appended
// fields. sdk_context is a SaoSdkContext pointer; loader_context is a distinct
// opaque loader plugin_context_t pointer and must never be cast to SaoSdkContext.
struct cs_sdk_bridge {
    void(SAO_PLUGINS_CALL* log_info)(const char* utf8);
    void(SAO_PLUGINS_CALL* register_ui_panel)(const char* utf8);
    void(SAO_PLUGINS_CALL* register_hotkey)(const char* id_utf8, const char* key_utf8);
    uint32_t struct_size;
    uint32_t abi_version;
    void* sdk_context;
    void* loader_context;
};

// OnLoad receives a borrowed pointer to this descriptor and exactly
// sizeof(cs_managed_plugin_context) as argumentSize. Both pointers remain valid
// until logical unload completes. Other lifecycle hooks receive null/zero.
struct cs_managed_plugin_context {
    uint32_t struct_size;
    uint32_t abi_version;
    void* sdk_context;
    void* loader_context;
};

// SDK 记账 (供 test 校验插件真的调了 SDK)
struct cs_sdk_counters {
    uint32_t log_info_calls;
    uint32_t register_ui_panel_calls;
    uint32_t register_hotkey_calls;
    char last_log_utf8[512];
    char last_panel_id_utf8[128];
    char last_hotkey_id_utf8[128];
    char last_hotkey_key_utf8[64];
};

// 加载 plugin.json 指定的预编译 DLL 并调用 OnLoad。
//
// plugin_json_path_utf8: 插件目录里的 plugin.json 绝对路径.
// entry 必须是相对 plugin.json 目录的 .dll；runtimeconfig 可显式声明，
// 否则使用 <AssemblyName>.runtimeconfig.json。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_load_plugin(cs_host_handle_t host, const char* plugin_json_path_utf8,
                               cs_plugin_handle_t* out_plugin, char** out_error_utf8);

// 调 OnTick() 一次.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_tick_plugin(cs_plugin_handle_t plugin, char** out_error_utf8);

// Calls optional OnUnload and logically deactivates the plugin. The assembly is
// process-resident until process exit; loading the same assembly path again is
// rejected while collectible ALC bootstrap is unavailable. Close failures are
// returned and the handle remains valid for a retry.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_unload_plugin(cs_plugin_handle_t plugin, char** out_error_utf8);

// 读 SDK 计数器 (供 test 校验).
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_sdk_counters(cs_plugin_handle_t plugin, cs_sdk_counters* out_counters);

// 调 GetTickCount() 拿托管侧的 s_tickCount 全局.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_read_tick_count(cs_plugin_handle_t plugin, int32_t* out_value);

} // namespace sao::plugins::csharp_host
