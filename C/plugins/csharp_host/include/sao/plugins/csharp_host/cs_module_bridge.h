// cs_module_bridge.h — 通过 NativeInterop 暴露 SDK
//
// 托管侧代码用 [LibraryImport] 声明到本 host 导出的 sao_csharp_ctx_* 函数,
// 把 IntPtr(ctx) 从 native 一路带到 managed。native 层做 handle 校验。
//
// 我们在托管侧提供一个 SAO.PluginContext 类, 里面暴露 static + instance 方法
// (Log / Subscribe / RegisterUiPanel / ...), 内部走 P/Invoke。老 C# 插件
// (对齐 example_csharp_plugin/plugin.cs) 拿到的 dynamic ctx 就是本类的实例,
// 支持 ctx.log(...) / ctx.register_ui_panel(...) 两种命名。
//
// 关键: 反向 P/Invoke 到 sao_plugins.dll 的 sao_csharp_ctx_* 导出符号
// (由 sdk_binding/binding_csharp.cpp 实现)。托管侧代码 (SaoPluginContext.cs)
// 由本 module_bridge 在插件加载前编译到 domain。
#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/plugins/csharp_host/cs_plugin_lifecycle.h"
#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::csharp_host {

#if defined(_WIN32)
#define SAO_CSHOST_MANAGED_CALL __stdcall
#else
#define SAO_CSHOST_MANAGED_CALL
#endif

typedef struct cs_domain_s* cs_domain_handle_t;

inline constexpr uint32_t SAO_CSHOST_SDK_TABLE_ABI_VERSION = 1;

enum class cs_managed_callback_kind : uint32_t {
    event = 0,
    hotkey,
    timer,
    panel_action,
    entity_snapshot,
    entity_action,
};

using cs_managed_callback_invoke_fn = int32_t(SAO_CSHOST_MANAGED_CALL*)(
    void* gc_handle, cs_managed_callback_kind kind, const void* invocation);
using cs_managed_callback_retain_fn = int32_t(SAO_CSHOST_MANAGED_CALL*)(void* gc_handle);
using cs_managed_callback_release_fn = int32_t(SAO_CSHOST_MANAGED_CALL*)(void* gc_handle);

struct cs_managed_callback_descriptor {
    uint32_t struct_size;
    uint32_t abi_version;
    cs_managed_callback_kind kind;
    uint32_t reserved;
    void* gc_handle;
    cs_managed_callback_invoke_fn invoke;
    cs_managed_callback_retain_fn retain;
    cs_managed_callback_release_fn release;
};

struct cs_managed_event_invocation {
    const char* topic_utf8;
    const uint8_t* payload_json_utf8;
    size_t payload_size;
};

struct cs_managed_hotkey_invocation {
    uint64_t hotkey_id;
};

struct cs_managed_timer_invocation {
    uint64_t timer_id;
};

struct cs_managed_panel_action_invocation {
    const char* action_key_utf8;
    const uint8_t* action_json_utf8;
    size_t action_size;
};

struct cs_managed_entity_snapshot_invocation {
    void* rows;
    uint32_t capacity;
    uint32_t* out_count;
    uint64_t* out_revision;
};

struct cs_managed_entity_action_invocation {
    const char* action_id_utf8;
    const char* payload_json_utf8;
};

using cs_managed_callback_token_t = void*;

struct cs_managed_sdk_call {
    uint32_t struct_size;
    uint32_t reserved;
    uint16_t method_id;
    uint16_t reserved2;
    const char* args_json_utf8;
    size_t args_size;
    const cs_managed_callback_descriptor* callback;
    char* out_result_json_utf8;
    size_t out_capacity;
    size_t* out_required;
    cs_managed_callback_token_t* out_callback_token;
};

struct cs_managed_entity_provider_descriptor {
    uint32_t struct_size;
    const char* provider_id_utf8;
    const char* contribution_id_utf8;
    const char* root_id_utf8;
    const char* name_utf8;
    const char* icon_utf8;
    double priority;
    const cs_managed_callback_descriptor* snapshot;
    const cs_managed_callback_descriptor* action_handler;
};

struct cs_managed_sdk_table {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t(SAO_PLUGINS_CALL* dispatch)(cs_managed_sdk_session_t session,
                                        cs_managed_sdk_call* call);
    int32_t(SAO_PLUGINS_CALL* release_callback)(cs_managed_sdk_session_t session,
                                                cs_managed_callback_token_t callback_token);
    int32_t(SAO_PLUGINS_CALL* log)(cs_managed_sdk_session_t session, const char* message_utf8);
    int32_t(SAO_PLUGINS_CALL* register_engine)(cs_managed_sdk_session_t session,
                                               const char* name_utf8, void* engine);
    int32_t(SAO_PLUGINS_CALL* get_engine)(cs_managed_sdk_session_t session, const char* name_utf8,
                                          void** out_engine);
    int32_t(SAO_PLUGINS_CALL* register_entity_provider)(
        cs_managed_sdk_session_t session, const cs_managed_entity_provider_descriptor* descriptor);
    int32_t(SAO_PLUGINS_CALL* unregister_entity_provider)(cs_managed_sdk_session_t session,
                                                          const char* provider_id_utf8);
    int32_t(SAO_PLUGINS_CALL* last_error)(cs_managed_sdk_session_t session, char* out_error_utf8,
                                          size_t out_capacity, size_t* out_required);
};

// 每 domain 注入一个 ctx 的 IntPtr (作为 [ThreadStatic] 或 AsyncLocal)。
// 内部先编译一份 SaoPluginContext.cs (embedded 源码) 到 domain, 然后设
// SAO.PluginContext._nativeHandle 静态字段为 ctx_handle。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_inject_ctx(cs_domain_handle_t domain, void* ctx_handle);

// 返回随包的 SaoPluginContext.cs 源码 (embedded, 编译时嵌进 sao_plugins.dll)。
// 供 cs_compile 在编译插件前预先注入。
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_cshost_embedded_ctx_source(void);

// 返回随包的 SaoPlugin.dll (预编译版本, 若插件不 include 源码时 fallback)。
// 供 cs_host 在 domain 创建时优先加载。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_load_embedded_sdk_dll(cs_domain_handle_t domain);

// 从托管侧一次性获取所有 SDK 方法的 native function pointer 表。
// 托管侧: [LibraryImport("sao_plugins", EntryPoint="sao_plugins_cshost_get_sdk_table")]
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_sdk_table(void** out_fn_ptrs, size_t* inout_count);

} // namespace sao::plugins::csharp_host
