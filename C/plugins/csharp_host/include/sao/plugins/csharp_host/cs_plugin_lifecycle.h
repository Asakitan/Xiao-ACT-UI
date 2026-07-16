// cs_plugin_lifecycle.h — Wave 8 / Agent d Phase 8
//
// 用 hostfxr load_assembly_and_get_function_pointer 加载 hello_csharp.dll:
//   1. hostfxr_initialize_for_runtime_config(HelloPlugin.runtimeconfig.json)
//   2. hostfxr_get_runtime_delegate(hdt_load_assembly_and_get_function_pointer)
//   3. load_assembly_and_get_function_pointer:
//        - "SaoAuto.Plugins.HelloCsharp.HelloPlugin::InitSdkPointers"
//        - "SaoAuto.Plugins.HelloCsharp.HelloPlugin::OnLoad"
//        - "SaoAuto.Plugins.HelloCsharp.HelloPlugin::OnTick"
//        - "SaoAuto.Plugins.HelloCsharp.HelloPlugin::OnUnload"
//   4. 调 InitSdkPointers(bridge, sizeof(bridge)) 注入 SDK 3 条 C 函数
//   5. 调 OnLoad() / OnTick() / OnUnload()
//   6. SDK 记账通过 static state, 供 test 观察

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

#include "sao/plugins/csharp_host/cs_host.h"

namespace sao::plugins::csharp_host {

typedef struct cs_plugin_s* cs_plugin_handle_t;

// 与 hello_csharp/HelloPlugin.cs 里的 SdkBridge 结构体字节对齐 (顺序也要一致).
struct cs_sdk_bridge {
    void (*log_info)(const char* utf8);
    void (*register_ui_panel)(const char* utf8);
    void (*register_hotkey)(const char* id_utf8, const char* key_utf8);
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

// 加载 plugin.json 指定的插件 (读 entry .dll → hostfxr load →
// InitSdkPointers → OnLoad).
//
// plugin_json_path_utf8: 插件目录里的 plugin.json 绝对路径.
// entry (从 JSON 里读) 相对 plugin.json 目录, e.g. "prebuilt/HelloPlugin.dll";
// 同目录必须有 <AssemblyName>.runtimeconfig.json.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_load_plugin(cs_host_handle_t host,
                               const char* plugin_json_path_utf8,
                               cs_plugin_handle_t* out_plugin,
                               char** out_error_utf8);

// 调 OnTick() 一次.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_tick_plugin(cs_plugin_handle_t plugin,
                               char** out_error_utf8);

// 调 OnUnload() 并释放插件资源.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_unload_plugin(cs_plugin_handle_t plugin,
                                 char** out_error_utf8);

// 读 SDK 计数器 (供 test 校验).
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_sdk_counters(cs_plugin_handle_t plugin,
                                    cs_sdk_counters* out_counters);

// 调 GetTickCount() 拿托管侧的 s_tickCount 全局.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_read_tick_count(cs_plugin_handle_t plugin,
                                   int32_t* out_value);

} // namespace sao::plugins::csharp_host
