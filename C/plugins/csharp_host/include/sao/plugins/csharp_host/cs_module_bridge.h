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
    entity_action_v2,
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

struct cs_managed_entity_snapshot_invocation_v2 {
    void* rows;
    uint32_t capacity;
    uint32_t row_stride_bytes;
    uint32_t* out_count;
    uint64_t* out_revision;
    uint64_t* out_content_token;
    uint32_t* out_row_stride_bytes;
};

// Managed v2 callbacks follow loader/entity_provider.h exactly. Probe receives
// rows=null, capacity=0 and row_stride_bytes=0. Fill receives the probed
// physical stride and must reproduce count, revision, nonzero producer token
// and stride. Every string pointer written into a row is borrowed by native and
// must remain valid until the enclosing native catalog snapshot call returns.
// Native copies only the known 80-byte row prefix and ignores physical tails.
struct cs_managed_entity_menu_row_v2 {
    uint32_t struct_size;
    const char* category_id_utf8;
    const char* category_label_utf8;
    const char* category_icon_utf8;
    double category_priority;
    const char* row_label_utf8;
    const char* row_icon_utf8;
    const char* action_id_utf8;
    const char* payload_json_utf8;
    uint8_t can_activate;
    uint8_t keep_menu_open;
    uint8_t close_menu_before;
    uint8_t reserved[5];
};

struct cs_managed_entity_action_invocation {
    const char* action_id_utf8;
    const char* payload_json_utf8;
};

using cs_managed_callback_token_t = void*;

// Managed action v2 receives an invocation carrying the owning session, a
// per-invocation callback token that identifies which pending result sink this
// dispatch owns, the borrowed action id and payload json, and the direct native
// sink pointer plus user data. Managed implementations may either invoke the
// sink pointer directly or route the submission through the appended
// submit_action_result_v2 table slot. The invocation struct itself is borrowed
// and remains valid only until the managed handler returns. All string pointers
// follow the same borrowed lifetime.
struct cs_managed_action_result_v2 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint8_t handled;
    uint8_t reserved[7];
    const char* result_json_utf8;
};

using cs_managed_action_result_sink_fn =
    int32_t(SAO_PLUGINS_CALL*)(const cs_managed_action_result_v2* result, void* sink_user_data);

struct cs_managed_entity_action_invocation_v2 {
    cs_managed_sdk_session_t session;
    cs_managed_callback_token_t callback_token;
    const char* action_id_utf8;
    const char* payload_json_utf8;
    cs_managed_action_result_sink_fn result_sink;
    void* sink_user_data;
};

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

struct cs_managed_entity_provider_descriptor_v2 {
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
    int32_t(SAO_PLUGINS_CALL* register_entity_provider_v2)(
        cs_managed_sdk_session_t session,
        const cs_managed_entity_provider_descriptor_v2* descriptor);
    int32_t(SAO_PLUGINS_CALL* emit_context)(cs_managed_sdk_session_t session,
                                            const char* topic_utf8, const char* payload_json_utf8);
    // Managed action v2 result submission path. Managed handlers registered
    // through register_entity_provider_v2 with a callback of kind
    // entity_action_v2 receive a cs_managed_entity_action_invocation_v2 whose
    // callback_token identifies the pending native sink. Calling this slot
    // resolves the sink by callback_token and forwards the result exactly once
    // per invocation. Submitting twice or with a non-matching token returns
    // SAO_ERR_HANDLE_INVALID; the invocation still fails closed if the managed
    // handler never submits.
    int32_t(SAO_PLUGINS_CALL* submit_action_result_v2)(
        cs_managed_sdk_session_t session, cs_managed_callback_token_t callback_token,
        const cs_managed_action_result_v2* result);
};

// ABI version 1 is retained. Consumers must gate appended entries with
// struct_size; binaries compiled against the original prefix remain valid.
inline constexpr size_t SAO_CSHOST_SDK_TABLE_V1_SIZE =
    offsetof(cs_managed_sdk_table, register_entity_provider_v2);

// V2 extended the append-only tail with register_entity_provider_v2 and
// emit_context (10 total pointers). V2 remains a valid readable extent for
// consumers that lack the action-v2 result submission slot.
inline constexpr size_t SAO_CSHOST_SDK_TABLE_V2_SIZE =
    offsetof(cs_managed_sdk_table, submit_action_result_v2);

// Current full size includes the action-v2 result submission slot. Consumers
// compiled against this header must publish struct_size >= V2 size and use
// safe_readable_extent when iterating unknown producer sizes.
inline constexpr size_t SAO_CSHOST_SDK_TABLE_CURRENT_SIZE = sizeof(cs_managed_sdk_table);

// Convert a producer-declared struct_size into a byte extent that is safe to
// read from a compiled cs_managed_sdk_table copy. Zero and oversize inputs
// clamp to the compiled size. Undersize inputs report exactly the declared
// prefix so the caller can gate optional appended slots on the boundary.
inline constexpr size_t safe_readable_extent(uint32_t struct_size,
                                             size_t compiled_size) noexcept {
    return (struct_size == 0 || struct_size > compiled_size) ? compiled_size
                                                              : static_cast<size_t>(struct_size);
}

#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(cs_managed_entity_snapshot_invocation_v2) == 48);
static_assert(offsetof(cs_managed_entity_snapshot_invocation_v2, row_stride_bytes) == 12);
static_assert(offsetof(cs_managed_entity_snapshot_invocation_v2, out_content_token) == 32);
static_assert(sizeof(cs_managed_entity_menu_row_v2) == 80);
static_assert(alignof(cs_managed_entity_menu_row_v2) == 8);
static_assert(offsetof(cs_managed_entity_menu_row_v2, can_activate) == 72);
static_assert(offsetof(cs_managed_entity_menu_row_v2, reserved) == 75);
static_assert(sizeof(cs_managed_entity_provider_descriptor_v2) == 72);
static_assert(offsetof(cs_managed_entity_provider_descriptor_v2, snapshot) == 56);
static_assert(SAO_CSHOST_SDK_TABLE_V1_SIZE == 72);
static_assert(offsetof(cs_managed_sdk_table, register_entity_provider_v2) == 72);
static_assert(offsetof(cs_managed_sdk_table, emit_context) == 80);
static_assert(SAO_CSHOST_SDK_TABLE_V2_SIZE == 88);
static_assert(offsetof(cs_managed_sdk_table, submit_action_result_v2) == 88);
static_assert(sizeof(cs_managed_sdk_table) == 96);
static_assert(SAO_CSHOST_SDK_TABLE_CURRENT_SIZE == 96);
static_assert(sizeof(cs_managed_action_result_v2) == 24);
static_assert(alignof(cs_managed_action_result_v2) == 8);
static_assert(offsetof(cs_managed_action_result_v2, struct_size) == 0);
static_assert(offsetof(cs_managed_action_result_v2, abi_version) == 4);
static_assert(offsetof(cs_managed_action_result_v2, handled) == 8);
static_assert(offsetof(cs_managed_action_result_v2, reserved) == 9);
static_assert(offsetof(cs_managed_action_result_v2, result_json_utf8) == 16);
static_assert(sizeof(cs_managed_entity_action_invocation_v2) == 48);
static_assert(alignof(cs_managed_entity_action_invocation_v2) == 8);
static_assert(offsetof(cs_managed_entity_action_invocation_v2, session) == 0);
static_assert(offsetof(cs_managed_entity_action_invocation_v2, callback_token) == 8);
static_assert(offsetof(cs_managed_entity_action_invocation_v2, action_id_utf8) == 16);
static_assert(offsetof(cs_managed_entity_action_invocation_v2, payload_json_utf8) == 24);
static_assert(offsetof(cs_managed_entity_action_invocation_v2, result_sink) == 32);
static_assert(offsetof(cs_managed_entity_action_invocation_v2, sink_user_data) == 40);
static_assert(safe_readable_extent(0, sizeof(cs_managed_sdk_table)) ==
              sizeof(cs_managed_sdk_table));
static_assert(safe_readable_extent(SAO_CSHOST_SDK_TABLE_V1_SIZE, sizeof(cs_managed_sdk_table)) ==
              SAO_CSHOST_SDK_TABLE_V1_SIZE);
static_assert(safe_readable_extent(SAO_CSHOST_SDK_TABLE_V2_SIZE, sizeof(cs_managed_sdk_table)) ==
              SAO_CSHOST_SDK_TABLE_V2_SIZE);
static_assert(safe_readable_extent(sizeof(cs_managed_sdk_table) + 32,
                                    sizeof(cs_managed_sdk_table)) == sizeof(cs_managed_sdk_table));
#endif

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
