// binding_common.h — 各宿主绑定的公共类型转换 + 异常屏障 + SDK 函数目录
//
// 5 个宿主 (python / emma / angel / lua / csharp) 都要把:
//   1. SDK C ABI (由 loader/plugin_context.h + platform/sdk/*.h 定义) 暴露给
//      各自语言
//   2. 各自语言值 (PyObject / lua_Value / asIScriptObject / lua_State 栈项 /
//      Emma variant / .NET object) 转成 C ABI 能接受的类型
//   3. 语言侧 callable → SDK 回调函数指针 (统一屏障 + user_data 生命周期)
//
// 公共部分抽在这里, 各宿主的 binding_<lang>.h 只做语言侧特化。
//
// 关键设计原则:
//   - sdk_method_id 是稳定名称/数值目录，不代表 generic JSON dispatcher
//     自动具备每个方法的 typed callback 能力
//   - 每种语言由自己的 host 暴露 ctx；带回调的注册直接走 host + loader typed ABI
//   - generic JSON dispatcher 只处理 sdk_context_call_request 明确携带的 callback 类型
//   - 所有跨 ABI JSON 用 json_node 中间格式 (avoid copy)
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "sao/plugins/loader/loader_status.h"
#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

struct SaoSdkContext;

namespace sao::plugins::sdk_binding {

inline constexpr size_t kMaximumBindingJsonBytes = 8U * 1024U * 1024U;
inline constexpr size_t kMaximumBindingJsonDepth = 64U;
inline constexpr size_t kMaximumBindingJsonNodes = 16384U;
inline constexpr size_t kMaximumBindingJsonStringBytes = 1024U * 1024U;
inline constexpr size_t kMaximumBindingJsonTotalStringBytes = 4U * 1024U * 1024U;

inline bool sao_plugins_binding_bounded_json_c_string(const char* value,
                                                       size_t& out_size) noexcept {
    if (value == nullptr) return false;
    const auto* terminator = static_cast<const char*>(
        std::memchr(value, '\0', kMaximumBindingJsonBytes + 1U));
    if (terminator == nullptr) return false;
    out_size = static_cast<size_t>(terminator - value);
    return out_size != 0;
}

// json 中立类型 (中间格式)
using json_value =
    std::variant<std::nullptr_t,                                      // nil / null / None
                 bool,                                                // true / false
                 int64_t,                                             // integer
                 double,                                              // number
                 std::string,                                         // utf-8 string
                 std::vector<class json_node>,                        // array
                 std::vector<std::pair<std::string, class json_node>> // object (顺序保留)
                 >;

class json_node {
  public:
    json_value value;

    json_node() = default;
    json_node(json_value v) : value(std::move(v)) {}
    static json_node from_string(std::string_view s);
    std::string to_string() const;
};

// SDK 稳定方法目录。名称和数值用于跨宿主识别；目录成员不等于 generic
// JSON dispatcher 或每个语言 host 都具备该能力。带 typed callback 的方法由
// 各 host 仅在能直接满足 loader 契约时暴露。
enum class sdk_method_id : uint16_t {
    // 属性 (readonly)
    prop_plugin_id = 0,
    prop_path,
    prop_web_path,
    prop_assets_path,
    prop_should_stop,

    // 日志
    method_log,

    // 事件
    method_subscribe,
    method_subscribe_once,
    method_unsubscribe,
    method_on_damage,
    method_on_heal,
    method_on_skill,
    method_on_boss,
    method_on_snapshot,
    method_on_encounter_finalized,
    method_emit,
    method_get_snapshot,
    method_snapshot_value,
    method_recent_events,

    // 设置
    method_get_setting,
    method_setting, // alias
    method_set_setting,
    method_set_defaults,

    // 扩展注册族
    method_register_parser_adapter,
    method_register_exporter,
    method_register_formatter,
    method_register_trigger_type,
    method_register_report_view,
    method_register_timer,
    method_register_ui_panel,
    method_register_render_hook,
    method_set_overlay,
    method_clear_overlay,
    method_register_hotkey,
    method_register_engine,
    method_register_data_source,
    method_register_menu_category,
    method_register_menu_surface,
    method_register_action_handler,
    method_request_redraw,

    // 定时器
    method_set_interval,
    method_set_timeout,
    method_clear_timer,
    method_run_on_ui,

    // 通知 / 对话框 / 窗口
    method_notify,
    method_dismiss_notify,
    method_toast,
    method_open_file,
    method_open_window,

    // Compositor Layer 族
    method_create_compositor_layer,
    method_upload_compositor_frame,
    method_set_compositor_layer_mmf_source,
    method_set_compositor_layer_shared_texture_source,
    method_set_compositor_layer_position,
    method_set_compositor_layer_visible,
    method_set_compositor_layer_input,
    method_destroy_compositor_layer,
    method_compositor_gpu_interop_available,
    method_compositor_layer_shared_texture_active,
    method_compositor_display_refresh_hz,

    // 引擎 / 依赖
    method_get_engine,
    method_require_engine,
    method_call_engine,
    method_call_runtime,
    method_ensure_requirements,
    method_load_local,
    method_time,

    method_count_, // sentinel
};

static_assert(static_cast<uint16_t>(sdk_method_id::method_register_hotkey) == 33);
static_assert(static_cast<uint16_t>(sdk_method_id::method_register_menu_category) == 36);
static_assert(static_cast<uint16_t>(sdk_method_id::method_register_menu_surface) == 37);
static_assert(static_cast<uint16_t>(sdk_method_id::method_register_action_handler) == 38);
static_assert(static_cast<uint16_t>(sdk_method_id::method_request_redraw) == 39);

enum class language_host_kind : uint8_t {
    python = 0,
    lua,
    angel,
    emma,
    csharp,
    count_,
};

enum class language_binding_operation : uint16_t {
    runtime_register = 0,
    module_init,
    context_bind,
    value_to_json,
    json_to_value,
    callback_wrap,
    method_table,
    static_call,
};

inline constexpr uint32_t SAO_LANGUAGE_HOST_PROVIDER_ABI_VERSION = 1;
inline constexpr uint32_t SAO_LANGUAGE_HOST_PROVIDER_ISOLATED_PYTHON_ABI = 1u << 0;

struct language_binding_request {
    void* runtime = nullptr;
    void* context = nullptr;
    void* value = nullptr;
    void* extra = nullptr;
    int32_t index = 0;
    const char* name_utf8 = nullptr;
    const char* secondary_name_utf8 = nullptr;
    const uint8_t* input = nullptr;
    size_t input_size = 0;
    uint8_t* output = nullptr;
    size_t output_capacity = 0;
    size_t* out_required = nullptr;
    void** out_object = nullptr;
    void** out_callback = nullptr;
    void** out_user_data = nullptr;
};

struct language_host_adapter_vtable {
    uint32_t struct_size = sizeof(language_host_adapter_vtable);
    uint32_t abi_version = SAO_LANGUAGE_HOST_PROVIDER_ABI_VERSION;
    uint32_t flags = 0;
    language_host_kind language = language_host_kind::python;
    bool(SAO_PLUGINS_CALL* available)(void* user_data) = nullptr;
    int32_t(SAO_PLUGINS_CALL* load_plugin)(void* context, void* runtime, void** out_plugin,
                                           void* user_data) = nullptr;
    int32_t(SAO_PLUGINS_CALL* unload_plugin)(void* plugin, void* user_data) = nullptr;
    int32_t(SAO_PLUGINS_CALL* invoke)(void* plugin, const char* method_name_utf8,
                                      const uint8_t* args_json_utf8, size_t args_size,
                                      uint8_t* out_result_json_utf8, size_t out_capacity,
                                      size_t* out_required, char* out_error_utf8,
                                      size_t error_capacity, void* user_data) = nullptr;
    int32_t(SAO_PLUGINS_CALL* dispatch)(language_binding_operation operation,
                                        language_binding_request* request,
                                        void* user_data) = nullptr;
    void* user_data = nullptr;

    // Optional append-only ABI extension. Providers whose struct_size ends
    // before this field retain the v1 layout and must use track_callback for
    // callback ownership.
    void(SAO_PLUGINS_CALL* release_callback)(void* callback_user_data, void* user_data) = nullptr;
};

inline constexpr size_t SAO_LANGUAGE_HOST_ADAPTER_V1_SIZE =
    offsetof(language_host_adapter_vtable, release_callback);

// Hard maximum concurrent callback ownership count for each binding. Storage
// is reserved before provider load; callbacks are never migrated to a
// process-global fallback owner.
inline constexpr size_t SAO_SDK_BINDING_MAX_CALLBACK_OWNERSHIP = 64;

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_register_language_host(const language_host_adapter_vtable* adapter);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_unregister_language_host(language_host_kind language);

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_binding_language_host_available(language_host_kind language);

// 方法名字符串 (给宿主注册时用) — SDK method → utf-8 name (对齐 Python 侧)
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_binding_method_name(sdk_method_id method);

// 反查: 语言侧的字符串 name → sdk_method_id (未知返回 method_count_)。
extern "C" SAO_PLUGINS_API sdk_method_id SAO_PLUGINS_CALL
sao_plugins_binding_method_from_name(const char* name);

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_binding_validate_json_text(const uint8_t* data, size_t size);

// 通用异常屏障: 语言侧回调调用平台 SDK 时用这个包起来。
using barrier_fn = int32_t(SAO_PLUGINS_CALL*)(void* user_data);

// 执行 fn(user_data), 拦截所有异常 (Win: SEH __try/__except; POSIX: sigaction),
// 返回 SAO_STATUS。失败时 out_error_utf8 填错误信息 (调用方 free)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_barrier(barrier_fn fn, void* user_data, char** out_error_utf8);

// 释放屏障返回的错误字符串。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_binding_free_error(char* err);

// 通用 callback 生命周期: 语言侧闭包 wrap 到 native fn ptr + user_data。
// 释放时调 release_callback 归还语言侧引用 (Py_DECREF / luaL_unref / GC handle
// free / EmmaCallable release)。
using release_callback_fn = void(SAO_PLUGINS_CALL*)(void* user_data);

// Register one wrapped callback for the current binding call scope. Ownership
// transfers only when registration returns SAO_OK. A call without a current
// binding, BUSY, or any other failure leaves callback_user_data owned by the
// caller/provider. Release callbacks must report failure by throwing before
// consuming ownership; a failed release is retried while the runtime remains
// alive.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_track_callback(void* user_data, release_callback_fn release_fn);

// Looks up and releases one provider-owned callback exactly once. Unknown,
// ambiguous, or already-released callback handles are ignored.
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_release_callback(language_host_kind language, void* callback_user_data);

// 卸载时释放全部登记 (供 unload_plugin 调)。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_binding_release_all_callbacks(void);

// 每个 language activate() 后返回 opaque plugin_binding_handle_t；所有
// load/unload/invoke 均经过注册 provider 和 common barrier。

typedef struct plugin_binding_s* plugin_binding_handle_t;

// 中央 dispatcher: 按 method_id 派发。
//   - args_ptr: utf-8 json (可为空)
//   - args_size: json 长度 (0 = 空参数)
//   - ret_ptr / ret_size: 输出 buffer (可为空表示不需要返回值)
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_plugin_load(language_host_kind language, void* plugin_ctx, void* runtime,
                                plugin_binding_handle_t* out_plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_plugin_unload(plugin_binding_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_binding_plugin_invoke(
    plugin_binding_handle_t plugin, const char* method_name_utf8, const uint8_t* args_json_utf8,
    size_t args_size, uint8_t* out_result_json_utf8, size_t out_capacity, size_t* out_required);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_plugin_last_error(plugin_binding_handle_t plugin, char* out_error_utf8,
                                      size_t out_capacity, size_t* out_required);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_binding_dispatch_provider(
    language_host_kind language, language_binding_operation operation,
    language_binding_request* request);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_sdk_bind_call(plugin_binding_handle_t plugin, sdk_method_id method_id,
                          const char* args_ptr, size_t args_size, char* ret_ptr, size_t ret_size);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_sdk_bind_call_ex(
    plugin_binding_handle_t plugin, sdk_method_id method_id, const char* args_ptr, size_t args_size,
    char* ret_ptr, size_t ret_size, size_t* out_required);

using sdk_context_event_callback_fn = void(SAO_PLUGINS_CALL*)(const char* topic_utf8,
                                                              const uint8_t* payload_json_utf8,
                                                              size_t payload_size, void* user_data);
using sdk_context_hotkey_callback_fn = void(SAO_PLUGINS_CALL*)(uint64_t hotkey_id, void* user_data);
using sdk_context_timer_callback_fn = void(SAO_PLUGINS_CALL*)(uint64_t timer_id, void* user_data);
using sdk_context_panel_action_callback_fn =
    void(SAO_PLUGINS_CALL*)(const char* action_key_utf8, const uint8_t* action_json_utf8,
                            size_t action_size, void* user_data);

struct sdk_context_call_request {
    const char* args_json_utf8 = nullptr;
    size_t args_size = 0;
    sdk_context_event_callback_fn event_callback = nullptr;
    sdk_context_hotkey_callback_fn hotkey_callback = nullptr;
    sdk_context_timer_callback_fn timer_callback = nullptr;
    sdk_context_panel_action_callback_fn panel_action_callback = nullptr;
    void* callback_user_data = nullptr;
    char* out_result_json_utf8 = nullptr;
    size_t out_capacity = 0;
    size_t* out_required = nullptr;
};

// Dispatches the subset whose semantics are provided directly by a modern
// SaoSdkContext. Provider-backed calls return UNSUPPORTED when the context has
// no matching provider slot. There is deliberately no Entity action-v2 callback
// slot here: register_action_handler remains host-owned and must not be encoded
// into JSON or tracked by sdk_binding. Registrations stay owned by SaoSdkContext.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_sdk_context_dispatch(
    const SaoSdkContext* ctx, sdk_method_id method_id, sdk_context_call_request* request);

// Side-effect-free capability probe used by compatibility reports.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_sdk_context_method_status(const SaoSdkContext* ctx, sdk_method_id method_id);

// 测试用: 查询绑定层是否记录了 add_hotkey 注册 (name key)，验证 dispatch 命中。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_binding_test_has_hotkey(plugin_binding_handle_t plugin, const char* hotkey_id);

// 测试用: 查询绑定层 publish_event 记录 (topic 命中次数)。
extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_plugins_binding_test_event_count(plugin_binding_handle_t plugin, const char* topic);

// 测试用: 查询 log 记录 (最近一条)。
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_binding_test_last_log(plugin_binding_handle_t plugin);

} // namespace sao::plugins::sdk_binding
