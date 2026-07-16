// py_v1_manifest.h — 老 Python 平台 manifest 字段兼容
//
// 老字段清单 (对齐 Python plugins.py 里各种 _normalize_* 的 lenient 分支):
//   - language 缺失 / 走 "engine" 别名 / 从 entry 扩展名反推
//   - entry 缺失 → 猜 plugin.<ext>
//   - runtime 字段 (老 v1 别名) → engine → language
//   - requires 数组形式 ["star_resonance"] vs 字典形式
//     {"act_platform": ">=1.0", "runtime_features": ["rgba_frame"]}
//   - deps: 老字段, 归到 requires
//   - capabilities 字符串形式 ["plugin_manager"] vs 对象形式 [{id: "..."}]
//   - abi_version 缺失 (老 v1) → 默认为 1, compat 层 shim 到 v2
//   - sao_menu 老字段 category / name 二选一 (name → category)
//   - locales / i18n / translations 三个都能识别 (同义)
//   - hidden / primary panel 布尔标签 (老版本没有 → 默认 primary=true / hidden=false)
//   - protected / native_entry / native_abi (workshop 保护插件)
//   - permissions 缺失 → 保守空集合
//   - mcpServers / chatProviders (AI Editor 时代新加, 老 manifest 缺失允许)
//   - hotkeys 字典形式 {"toggle": "F6"} → 内部规范化 hotkey_entry 列表
//
// 兼容策略: 老字段读进来, 缺失走默认值, 冲突走 v1 → v2 优先 (语义更严格的
// 那份优先; e.g. 同时有 language 和 engine 时以 language 为准, engine 只作
// 缺失时的 fallback)。
#pragma once

#include <cstdint>
#include <string>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/loader/plugin_manifest.h"

namespace sao::plugins::compat {

// 老字段规范化 (在 loader 解析 plugin.json 之后调, 补默认 / 别名映射)。
// 内部按顺序:
//   1. 若 language 空 → 用 engine / runtime 字段, 都没 → infer from entry 扩展名
//   2. 若 entry 空 → guess_default_entry_for(language)
//   3. 若 abi_version == 0 → 视为 v1, 输出到 warnings_json
//   4. 若 requires 已经是列表 → 保持; 若是字典 → 转为 "key:value" 字符串列表
//   5. 若 permissions 缺失 → 保守 []
//   6. 若 hotkeys 是 map → 转为 hotkey_entry 列表
//   7. 若 mcpServers / chatProviders 缺失 → 空 json object
// out_warnings_json_utf8 归属调用方 free。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_normalize_v1_manifest(sao::plugins::loader::plugin_manifest* manifest,
                                         char** out_warnings_json_utf8);

// 老 requires 字典形式 → 规范字符串列表 (对齐 python _normalize_requires)。
// 例如 {"runtime_features": ["rgba_frame"]} → ["runtime_feature:rgba_frame"];
// {"act_platform": ">=1.0"} → ["act_platform>=1.0"]。
// 输入若已经是数组 [...] 则原样输出。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_normalize_requires(const char* requires_json_utf8,
                                      char** out_normalized_json_utf8);

// 猜 entry: language=python 且 entry="" → "plugin.py" 等。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_guess_entry(sao::plugins::loader::engine_kind language,
                               char** out_entry);

// 从 utf-8 JSON 文本解析一个 plugin.json 到 manifest (实装, 非 stub)。
// 对齐 python _read_manifest, 支持所有已知字段 + 老别名 (engine/runtime/deps)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_parse_manifest_json(const char* utf8_json_ptr,
                                       size_t utf8_json_len,
                                       sao::plugins::loader::plugin_manifest* out_manifest,
                                       char** out_error_utf8);

// 从磁盘 plugin.json 读并解析 (实装; utf-8 或 utf-8-bom 都支持)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_load_manifest_file(const wchar_t* manifest_path,
                                      sao::plugins::loader::plugin_manifest* out_manifest,
                                      char** out_error_utf8);

// 释放本模块返回的 UTF-8 字符串。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_compat_free_string(char* str);

} // namespace sao::plugins::compat
