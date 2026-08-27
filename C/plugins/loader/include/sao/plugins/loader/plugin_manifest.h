// plugin_manifest.h — plugin.json 解析 (1:1 兼容 Python 平台 manifest 格式)
//
// 对齐 Python 源: act_platform/plugins.py 里 PluginManager._read_manifest 及
// _normalize_* 系列辅助函数, 以及 PluginRecord 数据字段 (~30 项)。
// 字段语义、别名规则、locales 覆盖层机制、requires 的列表/字典双形式解析
// 都在这一层落地。
//
// 不 import 插件: 加载器只读文本, 不 dlopen / 不 exec 脚本; 那是宿主的事。
//
// 字段来源对照 (Python PluginRecord + _read_manifest):
//   id / name / version / description / entry / enabled / language →
//     基础五件套 + 引擎派发字段
//   game_ids / requires / permissions / capabilities →
//     依赖 + 权限 + 能力声明
//   settings_schema / sao_menu / locales →
//     UI 生成 + 菜单集成 + i18n
//   protected / native_entry / native_abi →
//     workshop 保护插件 (workshop_plugin_native_protection)
//   mcp_servers / chat_providers →
//     AI Editor 桥接 (project_ai_editor_plugin_provider_manifest)
//   hotkeys →
//     默认快捷键 (project_hotkey_architecture)
//   abi_version →
//     ABI 版本声明 (缺失 = 1, 表示旧插件)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::loader {

// 5 种目标脚本引擎 (与 plugin.json language / engine 字段对齐)
enum class engine_kind : uint8_t {
    unknown = 0,
    python,       // .py, 默认 (Python plugin.json 若无 language 走这里)
    emma,         // .emma, 内置自研解释器
    angelscript,  // .as, 真 AngelScript SDK
    lua,          // .lua, 真 Lua 5.4 C API
    csharp,       // .cs / .dll, hostfxr + Roslyn
};

// 单条 capability 项 (对齐 Python _normalize_capability)
struct capability_entry {
    std::string id;            // "plugin_manager" / "ui_panels" / ...
    std::string title;
    std::string description;
    std::string route;         // 老字段, 可为空
    std::string render_hint;   // 老字段, 可为空
    std::vector<std::string> actions;       // UI 面板按钮 action_id 列表
    std::vector<std::string> payload_fields;
};

// 单条 hotkey 定义 (对齐 Python hotkeys dict)
struct hotkey_entry {
    std::string hotkey_id;     // 内部 id (register_hotkey 用)
    std::string default_key;   // 默认组合 "CTRL+F6" 之类
    std::string label;         // UI 显示
};

// settings_schema 单项 (对齐 Python 平台 UI 自动生成规则)
struct settings_schema_entry {
    std::string key;
    std::string type;          // "string" / "boolean" / "int" / "float"
    std::string default_json;  // JSON 序列化的默认值
    std::string description;
};

// plugin.json 解析结果 (1:1 对齐 Python PluginRecord 数据段)
//
// 每个字段都可从 plugin.json 直接读, 缺失走 compat/py_v1_manifest.h 补默认。
struct plugin_manifest {
    // ── 核心字段 ──────────────────────────────────────────────
    std::string plugin_id;   // 唯一 id
    std::string name;        // 显示名
    std::string version;     // 语义化版本
    std::string description; // 简介
    std::string entry;       // 入口文件相对路径 (plugin.py / plugin.lua / ...)
    std::string managed_type; // C# component type: Namespace.Type, Assembly
    std::string runtimeconfig; // 可选 .runtimeconfig.json 相对路径
    engine_kind language = engine_kind::unknown;
    bool enabled = false;    // manifest 默认启用状态

    // ── 依赖 / 权限 / 能力 ────────────────────────────────────
    std::vector<std::string> requires_list;  // 前置插件/特性
    std::vector<std::string> permissions;    // 沙箱白名单
    std::vector<std::string> game_ids;       // 兼容: 只对某些 game 生效
    std::vector<capability_entry> capabilities;

    // ── UI / 菜单 / 快捷键 ───────────────────────────────────
    std::vector<hotkey_entry> hotkeys;
    std::string sao_menu_json;          // 原样保留 sao_menu 对象
    std::vector<settings_schema_entry> settings_schema;
    std::string locales_json;           // i18n / locales / translations 原样

    // ── 面板行为标志 (对齐 Python _normalize_extension) ─────
    bool primary = true;      // 是否主面板 (缺失默认 true)
    bool hidden = false;      // 是否默认隐藏
    uint32_t min_width = 0;
    uint32_t min_height = 0;

    // ── AI Editor 桥接 (对齐 project_ai_editor_plugin_provider_manifest) ──
    std::string mcp_servers_json;
    std::string chat_providers_json;

    // ── Workshop 保护 (对齐 project_workshop_plugin_native_protection) ──
    bool protected_plugin = false;
    std::string native_entry;   // e.g. "workshop_native.dll"
    std::string native_abi;     // "sao_plugin_v1"

    // ── 加载时状态 ────────────────────────────────────────────
    std::string source_path;   // manifest 所在插件目录绝对路径
    bool user_installed = false; // scanner 标记来源属于配置的 user_roots
    uint32_t abi_version = 0;  // manifest 声明的 ABI 版本; 0 = 未声明 (兼容 v1)
    std::string parse_error;   // 非空表示解析失败，并保留具体错误文本
};

// 从 utf-8 json 文本解析。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_manifest_parse(const char* utf8_json_ptr,
                           size_t utf8_json_len,
                           plugin_manifest* out_manifest);

// 从磁盘 plugin.json 读并解析。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_manifest_load_from_file(const wchar_t* manifest_path,
                                    plugin_manifest* out_manifest);

// 校验 manifest 完整性 (id 合法, entry 存在, 语言可识别, ...)。
int32_t validate_manifest(const plugin_manifest& m);

int32_t resolve_contained_existing_path(const std::filesystem::path& root,
                                        const std::filesystem::path& candidate,
                                        std::filesystem::path& out_resolved) noexcept;

// engine_kind 枚举 <-> 字符串 (对齐 Python _normalize_engine_name)。
// 兼容别名: "as" → angelscript, "cs"/"c#" → csharp, "engine" 字段 → language。
engine_kind parse_engine_kind(std::string_view name);
std::string_view engine_kind_name(engine_kind kind);

// entry 缺失时按 language 猜: python→plugin.py, emma→plugin.emma, ...
std::string_view guess_default_entry_for(engine_kind kind);

// 从 entry 文件扩展名反推 language (无 language 字段时 fallback)。
// .py → python, .emma → emma, .as → angelscript, .lua → lua, .cs/.dll → csharp。
engine_kind infer_language_from_entry(std::string_view entry_path);

} // namespace sao::plugins::loader
