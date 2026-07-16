# compat/ — 老 Python 平台插件向下兼容层

**职责**: 一个在 Python 平台跑得好好的插件, 不改一个字直接放进新 C++ 平台
的 ``plugins/<plugin_id>/`` 就能跑。

## 对齐的 Python 源

- ``act_platform/plugins.py`` (老字段规范化 _normalize_* 系列)
- ``act_platform/plugin_install.py`` (import 时的 lenient parsing)
- ``act_platform/plugin_deps.py`` (libs/vendor 目录发现)

## 骨架文件列表

| 文件 | 职责 |
|------|------|
| ``py_v1_manifest.h`` | 老 manifest 字段规范化 (deps/engine/requires 字典) |
| ``py_v1_ctx_shim.h`` | 老 ctx 方法名别名 (add_hotkey/register_script) |
| ``libs_vendor_bridge.h`` | 目录 libs/ + vendor/ 发现 (对齐 midi_piano) |
| ``migration.h`` | 弃用字段清单 + 诊断报告 |

## 老字段清单 (对齐 Python)

manifest 侧:
- ``engine`` → ``language`` (老别名)
- ``deps`` → ``requires`` (老别名)
- ``requires: []`` 数组形式 vs ``requires: {}`` 字典形式 (两种都收)
- ``capabilities`` 字符串形式 vs 对象形式 (两种都收)
- ``sao_menu.description_short`` → ``sao_menu.description`` (老名字)
- ``entry`` 缺失 → 猜 ``plugin.<ext>``
- ``abi_version`` 缺失 (老 v1) → shim 到 v2

ctx 方法侧:
- ``add_hotkey`` → ``register_hotkey`` (老别名)
- ``register_script`` → ``register_ui_panel`` (老别名)
- ``add_menu_item`` → ``register_menu_category`` (老别名)
- ``get_snapshot()`` / ``snapshot_value(path, default)`` 老签名 (兼容)

## 关键设计

**不阻止加载**: 老字段 / 老方法都能用, 只是打 warn。给作者时间迁移。

**诊断优先**: ``sao_plugins_compat_scan_deprecated`` 生成"你用了哪些老字
段"报告, 供开发时 UI 展示。

**libs / vendor 通用化**: Python 平台 midi_piano 用 libs + vendor bootstrap
后, 各插件都可以走同套路。C++ 侧 compat 提供**语言无关的目录发现**, 各
宿主用它接自己语言侧的搜索路径。

## vcpkg 依赖

- ``nlohmann-json`` (老字段 requires 字典 / 列表两形式解析)

## 测试

见 ``tests/``。
