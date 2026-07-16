// test_py_v1_manifest_wave2.cpp — 用真实老 Python 平台 plugin.json 校验兼容层
//
// 每个 CASE 都是 assert 死断. 对齐 Wave1c 728 行零依赖 parser 实装:
//   - 真实 star_resonance plugin.json 内嵌 (无外部文件依赖, CI 友好)
//   - 老别名 engine / runtime / deps 归一化
//   - language / entry 缺失互相推断
//   - requires 字典形式扁平化
//   - UTF-8 BOM 容忍
//   - 行注释 (// ...) 容忍
#include "sao/plugins/compat/py_v1_manifest.h"
#include "sao/plugins/loader/plugin_manifest.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace sao::plugins::compat;
using namespace sao::plugins::loader;

namespace {

// ── 真实 plugin.json 内嵌 (从 sao_auto/python/plugins/*/plugin.json 直接复制) ──

// star_resonance_plugin/plugin.json — 完整无删改
constexpr const char* kStarResonanceJson = R"JSON({
    "id": "star_resonance",
    "name": "星痕共鸣 Star Resonance",
    "version": "1.0.0",
    "entry": "plugin.py",
    "description": "星痕共鸣游戏适配器 — TCP/内存抓包、DPS面板、Boss血条、HP面板、AutoKey、BossRaid、Buff监视、技能CD、地图横幅、死亡回放",
    "enabled": false,
    "game_ids": ["star_resonance"],
    "permissions": [
        "engine_access",
        "input_control",
        "memory_access",
        "packet_capture",
        "early_load"
    ],
    "capabilities": [
        {
            "id": "parser_adapters",
            "title": "Star Resonance TCP/Memory Parser",
            "description": "Protobuf packet decoding + IL2CPP memory reading"
        },
        {
            "id": "ui_panels",
            "title": "Game HUD Panels",
            "description": "DPS / BossHP / HP / Alert / SkillFX / BuffMon / AutoKey / BossRaid / Commander / MapBanner / MechBanner / SkillPicker / TriggerTimer / DeathRecap / CombatantDrilldown",
            "actions": [
                "toggle_dps", "toggle_boss_hp", "toggle_hp", "toggle_alert",
                "toggle_skillfx", "toggle_buffmon", "toggle_autokey",
                "toggle_bossraid", "toggle_commander"
            ]
        },
        {
            "id": "menu_categories",
            "title": "Game Menu Categories",
            "description": "自动 / Boss / Burst / 面板 (game-specific items)"
        },
        {
            "id": "data_sources",
            "title": "Packet + Memory Data Sources",
            "description": "TCP packet capture via Npcap + IL2CPP memory reader"
        }
    ],
    "settings_schema": {
        "mem_data_source": {
            "type": "string",
            "default": "tcp",
            "description": "Data source mode: tcp / memory / hybrid / auto"
        },
        "dps_enabled": {
            "type": "boolean",
            "default": true,
            "description": "Show DPS overlay"
        },
        "burst_enabled": {
            "type": "boolean",
            "default": true,
            "description": "Show burst readiness indicator"
        },
        "buffmon_enabled": {
            "type": "boolean",
            "default": true,
            "description": "Show buff monitor"
        },
        "boss_bar_mode": {
            "type": "string",
            "default": "boss_raid",
            "description": "Boss HP bar visibility: always / boss_raid / off"
        },
        "sound_enabled": {
            "type": "boolean",
            "default": true,
            "description": "Enable sound effects"
        }
    }
})JSON";

// example_lua_plugin/plugin.json — 显式 language:"lua" (无 engine/runtime 老别名)
// 我们用另加的 test string 检验别名规范化, 见 kLuaWithRuntimeAlias 等下面.
constexpr const char* kLuaPluginJson = R"JSON({
  "id": "example_lua",
  "name": "Lua Plugin Example",
  "version": "1.0.0",
  "entry": "plugin.lua",
  "language": "lua",
  "description": "Lua 插件示例。展示如何用 Lua 编写平台插件并调用全部 SDK API。",
  "enabled": false,
  "capabilities": [
    "plugin_manager",
    {"id": "ui_panels", "title": "Lua Demo", "actions": ["greet", "count"]}
  ],
  "permissions": [],
  "settings_schema": {
    "greeting": {"type": "string", "default": "你好", "description": "问候语"}
  }
})JSON";

// hide_seek_plugin/plugin.json — requires 是数组形式 ["star_resonance"]
constexpr const char* kHideSeekJson = R"JSON({
  "id": "hide_seek_plugin",
  "name": "Auto Hide & Seek",
  "version": "1.0.0",
  "entry": "plugin.py",
  "description": "自动躲猫猫 — 自包含示例插件。",
  "enabled": false,
  "requires": ["star_resonance"],
  "subscriptions": [],
  "capabilities": [
    "plugin_manager",
    {"id": "ui_panels", "title": "Hide & Seek control", "actions": ["toggle"]},
    {"id": "automation", "title": "CV hide-and-seek automation"}
  ],
  "permissions": ["engine_access", "input_control"]
})JSON";

// ── 老别名合成 fixture ──────────────────────────────────

// 用 engine 老字段而非 language (Wave1c parser 应识别)
constexpr const char* kLuaWithEngineAlias = R"JSON({
  "id": "legacy_engine_field",
  "name": "Legacy",
  "version": "1.0.0",
  "entry": "plugin.lua",
  "engine": "lua"
})JSON";

// 用 runtime 老字段
constexpr const char* kLuaWithRuntimeAlias = R"JSON({
  "id": "legacy_runtime_field",
  "name": "Legacy",
  "version": "1.0.0",
  "entry": "plugin.lua",
  "runtime": "lua"
})JSON";

// 无 entry, 只有 language:python → 应推 entry=plugin.py
constexpr const char* kMissingEntryPython = R"JSON({
  "id": "no_entry_python",
  "name": "NoEntry",
  "version": "1.0.0",
  "language": "python"
})JSON";

// 无 language, 只有 entry:plugin.lua → 应推 language=lua
constexpr const char* kMissingLanguageLuaEntry = R"JSON({
  "id": "no_lang_lua_entry",
  "name": "NoLang",
  "version": "1.0.0",
  "entry": "plugin.lua"
})JSON";

// 用 deps 老别名替代 requires
constexpr const char* kDepsAliasJson = R"JSON({
  "id": "deps_alias",
  "name": "Deps",
  "version": "1.0.0",
  "entry": "plugin.py",
  "language": "python",
  "deps": ["star_resonance", "act_platform"]
})JSON";

// requires 字典形式
constexpr const char* kDictRequiresJson = R"JSON({
  "id": "dict_requires",
  "name": "DictRequires",
  "version": "1.0.0",
  "entry": "plugin.py",
  "language": "python",
  "requires": {"act_platform": ">=1.0", "runtime_features": ["rgba_frame"]}
})JSON";

// UTF-8 BOM 前缀 (0xEF 0xBB 0xBF) + 简单 JSON
// BOM 用真实字节表达
static const unsigned char kBomJsonBytes[] = {
    0xEF, 0xBB, 0xBF,
    '{', '"', 'i', 'd', '"', ':', '"', 'b', 'o', 'm', '"', ',',
         '"', 'l', 'a', 'n', 'g', 'u', 'a', 'g', 'e', '"', ':', '"', 'p', 'y', 't', 'h', 'o', 'n', '"', ',',
         '"', 'e', 'n', 't', 'r', 'y', '"', ':', '"', 'p', 'l', 'u', 'g', 'i', 'n', '.', 'p', 'y', '"',
    '}'
};

// // 行注释在字段间
constexpr const char* kLineCommentJson = R"JSON({
  // 顶部注释
  "id": "commented",
  "name": "Commented", // 尾部注释
  "version": "1.0.0",
  // 这行也是注释
  "entry": "plugin.py",
  "language": "python"
})JSON";

// ── 辅助 ──

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// ── CASE 1: 用真实 star_resonance plugin.json parse 0 error ──
void case_parse_star_resonance_plugin_json_no_errors() {
    plugin_manifest m{};
    char* err = nullptr;
    int32_t rc = sao_plugins_compat_parse_manifest_json(
        kStarResonanceJson, std::strlen(kStarResonanceJson), &m, &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    assert(m.plugin_id == "star_resonance");
    assert(m.name == std::string("星痕共鸣 Star Resonance"));
    assert(m.version == "1.0.0");
    assert(m.entry == "plugin.py");
    assert(m.language == engine_kind::python);  // language 未声明, 从 entry .py 推
    assert(m.enabled == false);
    assert(m.game_ids.size() == 1);
    assert(m.game_ids[0] == "star_resonance");
    assert(m.permissions.size() == 5);
    assert(contains(m.permissions, "memory_access"));
    assert(contains(m.permissions, "packet_capture"));
    assert(m.capabilities.size() == 4);
    assert(m.capabilities[0].id == "parser_adapters");
    assert(m.capabilities[1].id == "ui_panels");
    assert(m.capabilities[1].actions.size() == 9);
    assert(contains(m.capabilities[1].actions, "toggle_dps"));
    assert(m.settings_schema.size() == 6);
    // parse_error 空
    assert(m.parse_error.empty());

    // normalize 后走一遍, 允许有 warnings (缺 abi_version 会警告)
    char* warns = nullptr;
    rc = sao_plugins_compat_normalize_v1_manifest(&m, &warns);
    assert(rc == SAO_OK);
    assert(warns != nullptr);
    // 应至少有 abi_version 那条 warn, 没有 language / entry warn
    std::string ws(warns);
    assert(ws.find("abi_version absent") != std::string::npos);
    assert(ws.find("language absent") == std::string::npos);
    assert(ws.find("entry absent") == std::string::npos);
    sao_plugins_compat_free_string(warns);
    std::printf("  [OK] parse_star_resonance_plugin_json_no_errors\n");
}

// ── CASE 2: engine 老别名归一化到 language ──
void case_parse_lua_plugin_json_engine_alias_normalized() {
    // 老 engine 字段
    plugin_manifest m{};
    char* err = nullptr;
    int32_t rc = sao_plugins_compat_parse_manifest_json(
        kLuaWithEngineAlias, std::strlen(kLuaWithEngineAlias), &m, &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    assert(m.language == engine_kind::lua);  // engine → language
    assert(m.entry == "plugin.lua");

    // runtime 老字段
    plugin_manifest m2{};
    rc = sao_plugins_compat_parse_manifest_json(
        kLuaWithRuntimeAlias, std::strlen(kLuaWithRuntimeAlias), &m2, &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    assert(m2.language == engine_kind::lua);  // runtime → language

    // 现代 language 字段 (真实 example_lua plugin.json) 直接就是 lua
    plugin_manifest m3{};
    rc = sao_plugins_compat_parse_manifest_json(
        kLuaPluginJson, std::strlen(kLuaPluginJson), &m3, &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    assert(m3.language == engine_kind::lua);
    assert(m3.plugin_id == "example_lua");
    std::printf("  [OK] parse_lua_plugin_json_engine_alias_normalized\n");
}

// ── CASE 3: entry 缺失 + language=python → entry=plugin.py ──
void case_parse_missing_entry_infers_from_language() {
    plugin_manifest m{};
    char* err = nullptr;
    int32_t rc = sao_plugins_compat_parse_manifest_json(
        kMissingEntryPython, std::strlen(kMissingEntryPython), &m, &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    assert(m.language == engine_kind::python);
    assert(m.entry == "plugin.py");
    std::printf("  [OK] parse_missing_entry_infers_from_language\n");
}

// ── CASE 4: language 缺失 + entry=plugin.lua → language=lua ──
void case_parse_missing_language_infers_from_entry() {
    plugin_manifest m{};
    char* err = nullptr;
    int32_t rc = sao_plugins_compat_parse_manifest_json(
        kMissingLanguageLuaEntry, std::strlen(kMissingLanguageLuaEntry), &m, &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    assert(m.language == engine_kind::lua);
    assert(m.entry == "plugin.lua");
    std::printf("  [OK] parse_missing_language_infers_from_entry\n");
}

// ── CASE 5: deps 老别名 merge 进 requires ──
void case_parse_deps_alias_merged_into_requires() {
    plugin_manifest m{};
    char* err = nullptr;
    int32_t rc = sao_plugins_compat_parse_manifest_json(
        kDepsAliasJson, std::strlen(kDepsAliasJson), &m, &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    // 老 deps 字段 → normalize_requires → requires_list
    assert(m.requires_list.size() == 2);
    assert(contains(m.requires_list, "star_resonance"));
    assert(contains(m.requires_list, "act_platform"));
    std::printf("  [OK] parse_deps_alias_merged_into_requires\n");
}

// ── CASE 6: requires 字典形式 → 扁平化 list ──
void case_parse_dict_requires_flattened() {
    plugin_manifest m{};
    char* err = nullptr;
    int32_t rc = sao_plugins_compat_parse_manifest_json(
        kDictRequiresJson, std::strlen(kDictRequiresJson), &m, &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    // {"act_platform": ">=1.0"} → "act_platform>=1.0"
    assert(contains(m.requires_list, "act_platform>=1.0"));
    // Wave 8 parity 修正: Python 侧硬编码 runtime_features 展开为单数
    // "runtime_feature:<item>", 不是复数 "runtime_features:...".  见
    // act_platform.plugins._normalize_requires + fixture deep_nested_requires_dict.
    assert(contains(m.requires_list, "runtime_feature:rgba_frame"));
    std::printf("  [OK] parse_dict_requires_flattened\n");
}

// ── CASE 7: UTF-8 BOM 前缀应容忍 ──
void case_parse_utf8_bom_tolerated() {
    plugin_manifest m{};
    char* err = nullptr;
    int32_t rc = sao_plugins_compat_parse_manifest_json(
        reinterpret_cast<const char*>(kBomJsonBytes),
        sizeof(kBomJsonBytes),
        &m,
        &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    assert(m.plugin_id == "bom");
    assert(m.language == engine_kind::python);
    assert(m.entry == "plugin.py");
    std::printf("  [OK] parse_utf8_bom_tolerated\n");
}

// ── CASE 8: // 行注释应容忍 ──
void case_parse_json_line_comment_tolerated() {
    plugin_manifest m{};
    char* err = nullptr;
    int32_t rc = sao_plugins_compat_parse_manifest_json(
        kLineCommentJson, std::strlen(kLineCommentJson), &m, &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    assert(m.plugin_id == "commented");
    assert(m.language == engine_kind::python);
    assert(m.entry == "plugin.py");
    std::printf("  [OK] parse_json_line_comment_tolerated\n");
}

// ── CASE bonus: hide_seek requires 数组形式保留 ──
void case_parse_hide_seek_requires_array_preserved() {
    plugin_manifest m{};
    char* err = nullptr;
    int32_t rc = sao_plugins_compat_parse_manifest_json(
        kHideSeekJson, std::strlen(kHideSeekJson), &m, &err);
    assert(rc == SAO_OK);
    assert(err == nullptr);
    assert(m.plugin_id == "hide_seek_plugin");
    assert(m.requires_list.size() == 1);
    assert(m.requires_list[0] == "star_resonance");
    assert(m.permissions.size() == 2);
    assert(contains(m.permissions, "engine_access"));
    std::printf("  [OK] parse_hide_seek_requires_array_preserved\n");
}

} // namespace

int main() {
    std::printf("test_py_v1_manifest_wave2:\n");
    case_parse_star_resonance_plugin_json_no_errors();
    case_parse_lua_plugin_json_engine_alias_normalized();
    case_parse_missing_entry_infers_from_language();
    case_parse_missing_language_infers_from_entry();
    case_parse_deps_alias_merged_into_requires();
    case_parse_dict_requires_flattened();
    case_parse_utf8_bom_tolerated();
    case_parse_json_line_comment_tolerated();
    case_parse_hide_seek_requires_array_preserved();
    std::printf("py_v1_manifest_wave2: 9 cases passed\n");
    return 0;
}
