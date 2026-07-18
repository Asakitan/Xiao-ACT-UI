// plugin_context.h — C ABI plugin_context_t (对齐 Python PluginContext)
//
// Python PluginContext (act_platform/plugins.py) 有 ~70 方法; C++ 侧按家族提供
// log / event / setting / register_* / compositor / hotkey / timer / notify /
// io / engine。缺少平台 primitive 的家族明确返回 UNSUPPORTED。
#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/loader/loader_status.h"

namespace sao::plugins::loader {

typedef struct plugin_context_s plugin_context_t;
typedef struct plugin_handle_s* plugin_handle_t;

// 回调族 (对齐 Python register_* 的第 N 个参数; 每种 hook 一个类型)
using event_callback_fn = void (*)(const char* topic_utf8,
                                   const char* event_json_utf8, void* user_data);
using render_callback_fn = int32_t (*)(const char* payload_json_utf8,
                                       char** out_spec_json_utf8, void* user_data);
using action_callback_fn = int32_t (*)(const char* action_id_utf8,
                                       const char* payload_json_utf8,
                                       char** out_result_json_utf8, void* user_data);
using render_hook_fn = int32_t (*)(const char* surface_utf8,
                                   const char* payload_json_utf8,
                                   char** out_spec_json_utf8, void* user_data);
using hotkey_callback_fn = void (*)(void* user_data);
using timer_callback_fn = void (*)(void* user_data);
using data_source_start_fn = int32_t (*)(void* user_data);
using data_source_stop_fn = int32_t (*)(void* user_data);
using compositor_cursor_pos_fn = void (*)(float x, float y, void* user_data);
using compositor_mouse_button_fn = void (*)(uint32_t button, bool pressed, void* user_data);
using compositor_cursor_leave_fn = void (*)(void* user_data);
using compositor_scroll_fn = void (*)(float dx, float dy, void* user_data);

// Loader-neutral platform capability bridge.  The loader copies the
// provider vtable and never includes or links the platform SDK.  A provider
// owns one isolated session per canonical plugin_context_t.
#define SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION_MAJOR 1u
#define SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION_MINOR 0u
#define SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION \
    ((SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION_MAJOR << 16u) | \
     SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION_MINOR)

using plugin_context_platform_token_t = uint64_t;
using plugin_context_platform_session_t = void*;

struct plugin_context_platform_session_spec {
    uint32_t struct_size;
    const char* plugin_id_utf8;
    const char* plugin_path_utf8;
    const char* plugin_version_utf8;
};

struct plugin_context_platform_provider {
    uint32_t abi_version;
    uint32_t struct_size;
    void* user_data;

    void (SAO_PLUGINS_CALL* retain)(void* user_data);
    void (SAO_PLUGINS_CALL* release)(void* user_data);

    int32_t (SAO_PLUGINS_CALL* create_session)(
        void* user_data,
        const plugin_context_platform_session_spec* spec,
        plugin_context_platform_session_t* out_session);
    int32_t (SAO_PLUGINS_CALL* quiesce_session)(
        void* user_data, plugin_context_platform_session_t session);
    int32_t (SAO_PLUGINS_CALL* destroy_session)(
        void* user_data, plugin_context_platform_session_t session);

    int32_t (SAO_PLUGINS_CALL* register_hotkey)(
        void* user_data, plugin_context_platform_session_t session,
        const char* hotkey_id_utf8, const char* default_key_utf8,
        const char* label_utf8, hotkey_callback_fn callback,
        void* callback_user_data,
        plugin_context_platform_token_t* out_provider_token);
    int32_t (SAO_PLUGINS_CALL* unregister_hotkey)(
        void* user_data, plugin_context_platform_session_t session,
        plugin_context_platform_token_t provider_token);

    int32_t (SAO_PLUGINS_CALL* register_timer)(
        void* user_data, plugin_context_platform_session_t session,
        double seconds, bool one_shot, timer_callback_fn callback,
        void* callback_user_data,
        plugin_context_platform_token_t* out_provider_token);
    int32_t (SAO_PLUGINS_CALL* unregister_timer)(
        void* user_data, plugin_context_platform_session_t session,
        plugin_context_platform_token_t provider_token);

    int32_t (SAO_PLUGINS_CALL* show_notify)(
        void* user_data, plugin_context_platform_session_t session,
        const char* title_utf8, const char* message_utf8, double duration_s,
        const char* kind_utf8,
        plugin_context_platform_token_t* out_provider_token);
    int32_t (SAO_PLUGINS_CALL* dismiss_notify)(
        void* user_data, plugin_context_platform_session_t session,
        plugin_context_platform_token_t provider_token);

    int32_t (SAO_PLUGINS_CALL* register_render_hook)(
        void* user_data, plugin_context_platform_session_t session,
        const char* surface_utf8, float priority, render_hook_fn callback,
        void* callback_user_data,
        plugin_context_platform_token_t* out_provider_token);
    int32_t (SAO_PLUGINS_CALL* unregister_render_hook)(
        void* user_data, plugin_context_platform_session_t session,
        plugin_context_platform_token_t provider_token);

    int32_t (SAO_PLUGINS_CALL* set_overlay)(
        void* user_data, plugin_context_platform_session_t session,
        const char* surface_utf8, const char* spec_json_utf8,
        plugin_context_platform_token_t* out_provider_token);
    int32_t (SAO_PLUGINS_CALL* clear_overlay)(
        void* user_data, plugin_context_platform_session_t session,
        plugin_context_platform_token_t provider_token);

    int32_t (SAO_PLUGINS_CALL* request_redraw)(
        void* user_data, plugin_context_platform_session_t session,
        const char* surface_utf8, const char* reason_utf8);
};

// Exactly one process-wide provider may be registered.  Unregistering while
// any canonical context owns a provider session returns BUSY.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_platform_provider(
    const plugin_context_platform_provider* provider);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unregister_platform_provider();

// ── 生命周期 ──
extern "C" SAO_PLUGINS_API plugin_context_t* SAO_PLUGINS_CALL
sao_plugins_ctx_create(plugin_handle_t plugin);
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_ctx_destroy(plugin_context_t* ctx);

// ── 属性 (只读, 对齐 Python plugin_id/path/web_path/assets_path/should_stop) ──
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_ctx_plugin_id(plugin_context_t* ctx);
extern "C" SAO_PLUGINS_API const wchar_t* SAO_PLUGINS_CALL
sao_plugins_ctx_path(plugin_context_t* ctx);
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ctx_should_stop(plugin_context_t* ctx);

// ── 日志 ──
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_ctx_log(plugin_context_t* ctx, const char* utf8_message);

// ── 事件族 (对齐 subscribe / subscribe_once / on_damage / ...) ──
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_subscribe(plugin_context_t* ctx, const char* topic_utf8,
                          event_callback_fn callback, void* user_data,
                          uint32_t* out_token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_subscribe_once(plugin_context_t* ctx, const char* topic_utf8,
                               event_callback_fn callback, void* user_data,
                               uint32_t* out_token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unsubscribe(plugin_context_t* ctx, uint32_t token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_emit(plugin_context_t* ctx, const char* topic_utf8,
                     const char* payload_json_utf8);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_get_snapshot(plugin_context_t* ctx, char** out_snapshot_json);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_snapshot_value(plugin_context_t* ctx, const char* path_utf8,
                               char** out_value_json);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_recent_events(plugin_context_t* ctx, uint32_t limit,
                              const char* topic_utf8, char** out_events_json);

// ── 设置族 ──
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_get_setting(plugin_context_t* ctx, const char* key,
                            char** out_json_utf8);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_setting(plugin_context_t* ctx, const char* key,
                            const char* value_json_utf8);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_defaults(plugin_context_t* ctx, const char* defaults_json_utf8);

// ── UI 面板 / 渲染钩子 / overlay ──
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_ui_panel(plugin_context_t* ctx, const char* panel_id,
                                  const char* meta_json_utf8,
                                  render_callback_fn render,
                                  action_callback_fn on_action, void* user_data);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_render_hook(plugin_context_t* ctx, const char* surface_utf8,
                                     float priority, render_hook_fn hook,
                                     void* user_data, uint32_t* out_token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unregister_render_hook(plugin_context_t* ctx, uint32_t token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_overlay(plugin_context_t* ctx, const char* surface_utf8,
                            const char* spec_json_utf8);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_clear_overlay(plugin_context_t* ctx, const char* surface_utf8);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_request_redraw(plugin_context_t* ctx, const char* surface_utf8,
                               const char* reason_utf8);

// ── 扩展注册族 (parser/exporter/formatter/trigger/report/timer/menu/data) ──
// kind_utf8: "parser_adapter" / "exporter" / "formatter" / "trigger_type" /
//            "report_view" / "timer"
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_extension(plugin_context_t* ctx, const char* kind_utf8,
                                   const char* extension_id_utf8,
                                   const char* metadata_json_utf8,
                                   void* handler_fn_ptr, void* user_data);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_menu_category(plugin_context_t* ctx, const char* name_utf8,
                                       const char* icon_utf8, void* builder_fn_ptr,
                                       float priority, void* user_data);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_menu_surface(plugin_context_t* ctx, const char* surface_id_utf8,
                                      const char* descriptor_json_utf8, float priority);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_data_source(plugin_context_t* ctx, const char* source_id_utf8,
                                     const char* metadata_json_utf8,
                                     data_source_start_fn start,
                                     data_source_stop_fn stop, void* user_data);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_engine(plugin_context_t* ctx, const char* name_utf8,
                                void* engine_ptr);
extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_ctx_get_engine(plugin_context_t* ctx, const char* name_utf8);

// ── 热键 ──
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_hotkey(plugin_context_t* ctx, const char* hotkey_id,
                                const char* default_key, const char* label,
                                hotkey_callback_fn callback, void* user_data);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unregister_hotkey(plugin_context_t* ctx, const char* hotkey_id);

// ── 定时器 ──
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_interval(plugin_context_t* ctx, timer_callback_fn callback,
                             double seconds, void* user_data, char** out_token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_timeout(plugin_context_t* ctx, timer_callback_fn callback,
                            double seconds, void* user_data, char** out_token);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_clear_timer(plugin_context_t* ctx, const char* token);

// ── 通知 / Toast / 文件对话框 / 打开面板窗口 ──
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_notify(plugin_context_t* ctx, const char* title_utf8,
                       const char* message_utf8, double duration_s,
                       const char* kind_utf8);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_dismiss_notify(plugin_context_t* ctx);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_toast(plugin_context_t* ctx, const char* message_utf8);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_open_file(plugin_context_t* ctx, const char* filters_json_utf8,
                          const char* title_utf8, const wchar_t* initial_dir,
                          intptr_t hwnd_owner, wchar_t** out_selected_path);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_open_window(plugin_context_t* ctx, const char* panel_id_utf8,
                            uint32_t width, uint32_t height);

// ── Compositor Layer 族 (对齐 Python 9 项 create/upload/set/destroy) ──
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_create_compositor_layer(plugin_context_t* ctx, const char* name_utf8,
                                        uint32_t width, uint32_t height,
                                        int32_t x, int32_t y, int32_t z,
                                        bool click_through, bool high_fps,
                                        uint32_t target_fps);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_upload_compositor_frame(plugin_context_t* ctx, const char* name_utf8,
                                        const uint8_t* bgra_bytes, size_t bytes_len,
                                        uint32_t width, uint32_t height);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_compositor_layer_position(plugin_context_t* ctx,
                                              const char* name_utf8,
                                              int32_t x, int32_t y);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_compositor_layer_visible(plugin_context_t* ctx,
                                             const char* name_utf8, bool visible);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_destroy_compositor_layer(plugin_context_t* ctx, const char* name_utf8);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_compositor_layer_input(plugin_context_t* ctx, const char* name_utf8,
                                           compositor_cursor_pos_fn cursor_pos,
                                           compositor_mouse_button_fn mouse_btn,
                                           compositor_cursor_leave_fn cursor_leave,
                                           compositor_scroll_fn scroll,
                                           void* user_data);

// ── 依赖 / 加载 ──
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_ensure_requirements(plugin_context_t* ctx, bool install,
                                    char** out_report_json);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_load_local(plugin_context_t* ctx, const char* relative_path,
                           wchar_t** out_absolute_path);

// ── 内存分配释放 ──
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_ctx_free_string(char* str);
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_ctx_free_wstring(wchar_t* str);

} // namespace sao::plugins::loader
