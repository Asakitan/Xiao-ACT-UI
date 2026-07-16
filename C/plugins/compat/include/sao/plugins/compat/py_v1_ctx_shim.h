// py_v1_ctx_shim.h — 老 PluginContext 接口的向下兼容适配
//
// 老 Python 平台 PluginContext 里的方法名和签名细节 (对齐 plugins.py 的
// ~70 个 method + property):
//
// 事件族:
//   - subscribe(topic, callback) / subscribe_once(topic, callback) / unsubscribe(token)
//   - on(topic, callback=None) [decorator]
//   - on_damage / on_heal / on_skill / on_boss / on_snapshot /
//     on_encounter_finalized
//   - emit(topic, payload=None)
//   - get_snapshot() / snapshot_value(path, default=None) / recent_events(limit=20, topic="")
//
// 设置族:
//   - get_setting(key, default=None) / setting(key, default=None) [alias]
//   - set_setting(key, value) / set_defaults(dict)
//
// 扩展注册族:
//   - register_parser_adapter(id, meta=None, handler=None)
//   - register_exporter(id, meta=None, handler=None)
//   - register_formatter(id, meta=None, handler=None)
//   - register_trigger_type(id, meta=None, handler=None)
//   - register_report_view(id, meta=None, handler=None)
//   - register_timer(id, meta=None, handler=None)
//   - register_ui_panel(id, meta=None, render=None, on_action=None)
//   - register_render_hook(surface, callback, priority=0.0)
//   - set_overlay(surface, spec) / clear_overlay(surface=None)
//   - register_hotkey(id, callback, default_key="", label="")
//   - register_menu_category(name, icon, builder, priority=0.0)
//   - register_menu_surface(surface_id, descriptor, priority=0.0)
//   - register_action_handler(handler)
//   - register_engine(name, engine)
//   - register_data_source(id, meta=None, start=None, stop=None)
//   - register_thread(thread)
//   - request_redraw(surface="", reason="")
//
// 定时器 / UI 线程:
//   - set_interval(callback, seconds) / set_timeout(callback, seconds) /
//     clear_timer(token) / run_on_ui(callback)
//
// 通知 / 对话框 / 窗口:
//   - notify(title, message, duration_s=60.0, kind="plugin") / dismiss_notify() /
//     toast(msg)
//   - open_file(filters, title, initial_dir, hwnd_owner) / open_window(panel_id="", w=0, h=0)
//
// Compositor Layer (9 项):
//   - create_compositor_layer(name, w, h, x=0, y=0, z=140, click_through=True,
//                              high_fps=False, target_fps=0)
//   - upload_compositor_frame(name, bgra_bytes, w, h, x=None, y=None)
//   - set_compositor_layer_mmf_source(name, mmf_name)
//   - set_compositor_layer_shared_texture_source(name, handle, w, h)
//   - compositor_gpu_interop_available()
//   - compositor_layer_shared_texture_active(name)
//   - compositor_display_refresh_hz()
//   - set_compositor_layer_position(name, x, y) / _visible(name, v) / _input(name, ...)
//   - destroy_compositor_layer(name)
//
// 引擎 / 依赖:
//   - get_engine(name, default=None) / require_engine(name)
//   - call_engine(engine_name, method, *args, **kwargs) / call_runtime(action, *args, **kwargs)
//   - ensure_requirements(install=True) / load_local(rel)
//
// 属性:
//   - plugin_id / path / web_path / assets_path / should_stop /
//     event_bus / owner / engine / ui / mem
//
// C++ 侧默认接口和它一致, 但有些老细节要 shim:
//   - callback 里可能返回 None (Python 允许) → C++ 里当作空 result
//   - subscribe(topic, callback) 返回 str token (对齐 old event_bus) → shim
//     用同格式 "topic:hex" 序列
//   - dict 输出的 key 顺序 (老 Python 里靠 dict 保序) → C++ 用 ordered_map
//   - open_file(filters) 老形式 [{"name": "...", "spec": "*.txt"}] 与新形式
//     "text|*.txt|all|*.*" 都要支持
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/sdk_binding/binding_common.h"

namespace sao::plugins::compat {

typedef struct plugin_context_s* plugin_context_ptr;

// ctx 是以 opaque pointer 传入的现代 SaoSdkContext。shim 使用静态方法映射，
// 所有注册资源仍由原 context/provider 持有并随其生命周期清理。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_arm_v1_ctx_shim(plugin_context_ptr ctx);

// 输出 v1 → modern SDK 的方法映射、metadata 和当前 provider 能力报告。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_v1_report(plugin_context_ptr ctx, char** out_report_json_utf8);

// Dispatch a legacy method name with object-shaped JSON arguments. Callback
// slots are forwarded unchanged to sdk_binding. Unknown or semantically
// incompatible methods fail closed with UNSUPPORTED.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_v1_call(
    plugin_context_ptr ctx,
    const char* method_name,
    sao::plugins::sdk_binding::sdk_context_call_request* request);

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_compat_free_string(char* value);

// 检测某方法调用是否属于 v1 遗留 (用于 shim 内 dispatch 时区分)。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_compat_is_v1_method(const char* method_name);

// 老 open_file 的 filters 格式 (dict list) 转成新格式 (pipe-separated string)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_convert_open_file_filters(const char* v1_filters_json,
                                              char** out_v2_pipe_string);

// 校验现代 context 并返回同一 opaque handle，不建立代理对象或第二套 store。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_ctx_v1_wrap(plugin_context_ptr new_ctx,
                               plugin_context_ptr* out_v1_ctx);

// 注册 v1 方法名 → 新 method_id 映射 (如 "add_hotkey" → METHOD_ADD_HOTKEY)。
// method_id 用 uint16_t 存 sdk_method_id 值 (由调用方查表)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_ctx_v1_register_alias(const char* old_name,
                                         uint16_t new_method_id);

// 查询 v1 alias 是否已注册, 返回新 method_id (未注册返 0xFFFF)。
extern "C" SAO_PLUGINS_API uint16_t SAO_PLUGINS_CALL
sao_plugins_compat_ctx_v1_lookup_alias(const char* old_name);

// 清空显式注册的自定义 alias 表 (测试用)，不影响内置静态映射。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_compat_ctx_v1_clear_aliases(void);

} // namespace sao::plugins::compat
