// plugin_lifecycle.h — load / activate / deactivate / unload 状态机
//
// 对齐 Python 源: PluginManager.load_plugin / unload_plugin /
// enable_plugin / disable_plugin / reload_plugin。C++ 侧把 4 个动词做成显式
// 状态机, 每次转换发布生命周期事件供 UI 观察。
//
// 加载流程 (等价于 Python PluginManager.load_plugin, ~30 行 Python 对应到
// C++ 侧的一堆步骤):
//   1. discover — plugin_scanner 出结果 → registry.add_plugin
//   2. validate — 检查 manifest 完整性 (validate_manifest)
//   3. resolve_deps — 拓扑排序 (_topo_sorted_ids), 缺 requires 跳过
//   4. compat.normalize — 补 v1 老字段默认值
//   5. bootstrap_deps — plugin_deps.ensure (前插 libs/ vendor/ 到语言侧)
//   6. host_dispatch — 按 language 调对应宿主的 load_plugin
//        · python_host.load_plugin  → Py_ImportModule
//        · emma_host.load_plugin    → interpreter.execute
//        · lua_host.load_plugin     → lua.dofile
//        · angel_host.load_plugin   → asBuild + asExecute
//        · csharp_host.load_plugin  → cs_compile + reflect OnLoad
//   7. call on_load(ctx) — 宿主已注入 ctx, 直接调用
//   8. call on_enable() — 用户已启用时立即激活
//   9. lifecycle event 发布 — loaded / enabled
//
// 卸载流程 (对称):
//   1. signal stop_event
//   2. call on_disable()
//   3. call on_unload() — 返回 false 可 block 卸载 (等 worker 停车)
//   4. join threads (2s timeout)
//   5. destroy compositor layers / hotkeys / extensions
//   6. host.unload_script
//   7. remove from sys.path / package.path / LoadContext (restore_paths)
#pragma once

#include <cstdint>
#include <string>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"

namespace sao::plugins::loader {

typedef struct plugin_context_s plugin_context_t;

#define SAO_PLUGIN_NATIVE_QUERY_SYMBOL "sao_plugin_query_descriptor"
#define SAO_PLUGIN_NATIVE_ON_LOAD_SYMBOL "sao_plugin_on_load"
#define SAO_PLUGIN_NATIVE_ON_ENABLE_SYMBOL "sao_plugin_on_enable"
#define SAO_PLUGIN_NATIVE_ON_DISABLE_SYMBOL "sao_plugin_on_disable"
#define SAO_PLUGIN_NATIVE_ON_UNLOAD_SYMBOL "sao_plugin_on_unload"

struct native_plugin_descriptor {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t capability_count;
    const char* const* capabilities;
    const char* plugin_version;
    uint32_t entity_provider_count;
    const native_entity_provider_descriptor* entity_providers;
};

using native_plugin_query_fn = int32_t(SAO_PLUGINS_CALL*)(
    native_plugin_descriptor* out_descriptor);

// 插件生命周期状态 (对齐 Python PluginRecord.enabled/loaded/active 三元组)
enum class lifecycle_state : uint8_t {
    unknown = 0,
    discovered,       // manifest 已读, 记录已建, 未 load
    validating,       // validate_manifest 进行中
    resolving_deps,   // 等 requires 前置就绪
    bootstrapping,    // plugin_deps.ensure 前插 libs/vendor
    loading,          // 宿主装脚本, 调 on_load
    loaded_active,    // on_load + on_enable 完成, 正在跑
    loaded_disabled,  // 已 load 但 on_disable 完成, 可 re-enable 快速回到 active
    unloading,        // unload_plugin 进行中 (等 worker 停车)
    unloaded,         // 完全卸载, 待 GC / 待 refresh
    failed,           // 加载失败 (last_error 非空), 达 max_failures 后自动 disabled
    enabling,         // on_enable 正在执行，拒绝并发 lifecycle 转换
    disabling,        // provider rundown / on_disable 正在执行
};

// 生命周期事件 (对齐 Python 平台通过 event_bus 发布的 "plugin_lifecycle" topic)
enum class lifecycle_event : uint8_t {
    discovered = 0,     // 首次发现
    manifest_changed,   // 二次 discover 发现 manifest 有变
    validate_failed,    // manifest 校验不过
    load_started,       // 开始 load
    loaded,             // load_plugin 成功
    load_failed,
    enabled,            // 用户点了开关 (on_enable 完成)
    disabled,           // (on_disable 完成)
    unload_started,
    unloaded,           // unload_plugin 成功
    unload_blocked,     // on_unload 返回 false, 等 worker
    forgotten,          // forget_plugin (从注册表移除)
    unload_failed,
    disable_failed,
};

// 事件回调 (由平台侧订阅, 用于 UI 更新)
using lifecycle_event_cb = void (*)(plugin_handle_t plugin,
                                    lifecycle_event event,
                                    const char* utf8_message,
                                    void* user_data);

// 宿主适配器 vtable —— loader 通过 language 派发到对应宿主, 不硬编码。
// 每个 *_host 在初始化时调 register_host_adapter 注册一份 vtable。
struct host_adapter_vtable {
    // 加载单个插件脚本 (读 entry 文件 → 执行 → 拿 on_load 等 hook)。
    int32_t (SAO_PLUGINS_CALL* load_plugin)(
        plugin_handle_t plugin,
        const plugin_manifest* manifest,
        void* host_user_data);
    // 调 on_load(ctx) hook。
    int32_t (SAO_PLUGINS_CALL* call_on_load)(plugin_handle_t plugin,
                                             void* host_user_data);
    // 调 on_enable() hook。
    int32_t (SAO_PLUGINS_CALL* call_on_enable)(plugin_handle_t plugin,
                                               void* host_user_data);
    // 调 on_disable() hook。
    int32_t (SAO_PLUGINS_CALL* call_on_disable)(plugin_handle_t plugin,
                                                void* host_user_data);
    // 调 on_unload() hook, 允许返回 false 阻断卸载 (等 worker)。
    int32_t (SAO_PLUGINS_CALL* call_on_unload)(plugin_handle_t plugin,
                                               bool* out_allow_unload,
                                               void* host_user_data);
    // 卸载脚本 (释放 interpreter state / GC assembly / lua_close)。
    int32_t (SAO_PLUGINS_CALL* unload_plugin)(plugin_handle_t plugin,
                                              void* host_user_data);
    // 宿主的用户数据 (host 自身的 state pointer)
    void* host_user_data;
};

// 注册一个宿主适配器 (每 language 一份)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_register_host_adapter(engine_kind language,
                                            const host_adapter_vtable* vtable);

// 注销一个宿主适配器。仍有该 language 的插件 context 或活动调用时返回 BUSY。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_unregister_host_adapter(engine_kind language);

// 查询 loader 为插件生命周期持有的 canonical context。
// 返回的指针是借用引用，仅在下一次 lifecycle 转换前有效；adapter 回调期间稳定。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_get_context(plugin_handle_t plugin,
                                  plugin_context_t** out_context);

// 载入一个插件 (查 language 派发给对应宿主的 vtable.load_plugin)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_load(plugin_handle_t plugin);

// 卸载一个插件 (调宿主 unload_plugin, 等 worker 停车, 移除扩展记录)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_unload(plugin_handle_t plugin);

// 用户 UI 开关: 持久化 enable + 立即 load。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_enable(plugin_handle_t plugin);

// 用户 UI 开关: 持久化 disable + 立即 unload (可 re-enable)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_disable(plugin_handle_t plugin);

// 重新加载 (unload + load, 用于开发时热更)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_reload(plugin_handle_t plugin);

// 查询当前状态。
extern "C" SAO_PLUGINS_API lifecycle_state SAO_PLUGINS_CALL
sao_plugins_lifecycle_state(plugin_handle_t plugin);

// 拓扑排序 (对齐 Python _topo_sorted_ids): DFS visit requires 前置。
// 返回 handles 按 load 顺序排列; 缺少前置或存在环时返回明确错误。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_topo_sort(plugin_handle_t* handles,
                                size_t count,
                                plugin_handle_t* out_sorted_handles);

// 订阅生命周期事件 (供 UI / 日志观察)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_subscribe(lifecycle_event_cb callback,
                                void* user_data,
                                uint32_t* out_token);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_lifecycle_unsubscribe(uint32_t token);

} // namespace sao::plugins::loader
