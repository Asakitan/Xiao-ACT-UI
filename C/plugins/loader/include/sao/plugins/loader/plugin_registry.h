// plugin_registry.h — 全局插件注册表 (进程内单例)
//
// 对齐 Python 源: act_platform/plugins.py 的 PluginManager._records 字典 +
// _register_extension / _menu_categories / _plugin_engines / _data_sources 等
// 分类注册容器。C++ 侧改成 shared_mutex 保护的线程安全查询结构。
//
// 平台代码永远不 include 具体插件的 header —— 它只通过这里查 "有哪些插件"
// "该插件贡献了什么扩展 / UI 面板 / 数据源 / 引擎"。反向调用一律走 SDK C ABI。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/loader/plugin_manifest.h"

namespace sao::plugins::loader {

typedef struct plugin_registry_s* registry_handle_t;
typedef struct plugin_handle_s* plugin_handle_t;

// 扩展分类 (对齐 Python EXTENSION_KINDS)
enum class extension_kind : uint8_t {
    parser_adapter = 0,
    exporter,
    formatter,
    trigger_type,
    report_view,
    timer,
    ui_panel,
    menu_category,
    data_source,
};

// 一条扩展记录 (对齐 Python _normalize_extension 输出)
struct extension_record {
    std::string plugin_id;
    extension_kind kind = extension_kind::ui_panel;
    std::string id;
    std::string title;
    std::string description;
    std::string route;
    // 对宿主不透明的原始 payload (json 文本), 由 sdk_binding 反解成语言侧对象。
    std::string payload_json;
};

// 全局单例获取。第一次调用时惰性建注册表。
extern "C" SAO_PLUGINS_API registry_handle_t SAO_PLUGINS_CALL
sao_plugins_registry_instance(void);

// 注册一个新发现的插件 (由 plugin_scanner 调用)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_registry_add_plugin(registry_handle_t reg,
                                const plugin_manifest* manifest,
                                plugin_handle_t* out_handle);

// 按 id 查询插件句柄。
extern "C" SAO_PLUGINS_API plugin_handle_t SAO_PLUGINS_CALL
sao_plugins_registry_find(registry_handle_t reg, const char* plugin_id);

// 移除插件 (卸载后调用)。安全地 no-op 于 null。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_registry_remove(registry_handle_t reg, plugin_handle_t handle);

// 由 SDK 反向调用: 为某插件登记一条扩展。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_registry_add_extension(registry_handle_t reg,
                                   plugin_handle_t owner,
                                   const extension_record* record);

// 快照当前所有插件的 manifest (供 UI 列表使用)。
std::vector<plugin_manifest> snapshot_manifests(registry_handle_t reg);

// 快照某分类的所有扩展 (供平台按分类分发)。
std::vector<extension_record> snapshot_extensions(registry_handle_t reg,
                                                  extension_kind kind);

} // namespace sao::plugins::loader
