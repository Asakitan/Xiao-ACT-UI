# loader/ — 通用插件加载器

**职责**: 发现、加载、管理插件生命周期。**不 import 任何具体插件**, 只按
manifest 的 language 字段派发给 5 种 host 之一。**直接加载旧 Python 插件, 不
改任何插件文件**。

## 对齐的 Python 源

- ``act_platform/plugins.py`` (核心 PluginManager + PluginRecord + PluginContext)
- ``act_platform/plugin_install.py`` (一键 zip 导入)
- ``act_platform/plugin_deps.py`` (vendor/libs 兜底)
- ``act_platform/runtime.py`` (workspace 探测 + default_plugin_dirs)

## 骨架文件列表

| 文件 | 职责 |
|------|------|
| ``plugin_manifest.h`` | plugin.json 解析 (对齐 Python _read_manifest) |
| ``plugin_registry.h`` | 全局注册表 (对齐 _records + _extensions 家族) |
| ``plugin_scanner.h`` | 目录扫描 + workspace walkup (对齐 discover + sync_discovery) |
| ``plugin_install.h`` | zip 导入 (对齐 install_plugin_archive) |
| ``plugin_deps.h`` | libs/vendor 依赖引导 (对齐 ensure_requirements) |
| ``plugin_lifecycle.h`` | load/unload/enable/disable 状态机 + host adapter vtable |
| ``plugin_isolation.h`` | 崩溃隔离 (进程内屏障 + 可选子进程) |
| ``plugin_context.h`` | ctx C ABI 结构 (对齐 PluginContext ~70 方法) |

## 关键设计

**发现-注册-加载三段分离** (与 Python 一致):

1. ``scanner.discover(cfg)`` → 只读 manifest, 不 dlopen 任何东西
2. ``registry.add_plugin(manifest)`` → 建 handle, 记账
3. ``lifecycle.load(handle)`` → 按 language 派发给 host, 拿回 ctx

**平台不 import 插件**: 平台代码只调 ``loader.*`` 的 C ABI, 拿到的
``plugin_handle_t`` / ``extension_record`` 都是 **不透明句柄**。插件贡献的
UI panel / hotkey / data source 全部通过 SDK ABI 反向注册 (见
``plugin_context.h`` 里的 ``sao_plugins_ctx_register_*``)。

**宿主 vtable 注册**: 每个 ``*_host/`` 在启动时调
``sao_plugins_lifecycle_register_host_adapter(language, vtable)`` 注册自己
的适配器。loader 里没有任何 ``if (language == python) …`` 分支。

## 内部 helper 退休与排空

`plugin_context_lifetime_internal.h` 提供树内 `context_runtime_lease` 与
`context_runtime_resource`；loader 按 ctx 世代拥有 attachment，不依赖 script_ctx
或具体语言实现，不修改公开 C ABI 布局。资源释放关闭新调用和注册准入，在途 lease
或注册未排空时返回 `BUSY` 并保留资源供重试；卸载入口在 helper 调用或清理重入时
提前返回 `BUSY`，不等待当前回调。同步 data-source stop 可在当前清理作用域调用
既有 helper，但不接受新的 callback 注册。

事件/平台 quiesce → data-source stop → Entity destroy → platform release →
清除注册之后，才在 loader/ctx mutex 之外调用 attachment 的 `retire()`。
mini helper 随后排空 builder 的 SDK 回调、释放 callback boxes 和 interpreter；
外部保留的 proxy/callable 失效，同 ID 重载获得独立世代。
mini entry/helper 的 SDK 回调使用创建 ctx 桥时保存的弱世代身份取得 lease，
不持有 runtime owner 强引用；入口解释器的 SDK 回调重入卸载也返回 `BUSY`。
本修改经静态调用链、磁盘读回、聚焦独立审阅和全量 Debug / RelWithDebInfo 编译链接检查；
session-79 未运行并发卸载/真实扩展/provider callback/面板验收。
legacy facade OBJECT target 私有链接 loader，继承 manifest 公开头使用的
`PUBLIC nlohmann_json::nlohmann_json` 依赖；loader 在 facade-only 分支之外创建，
不反向依赖 facade 或具体宿主，最终 facade 仍显式链接 loader。
新增边的无环性和 facade-only 条件仅经静态复核，未重新配置该变体。
launcher 私有 locale helper 已使用 C++ linkage，补修后两配置编译确认 C4190 消失；
build-tree bundle 均更新，本轮未运行 Hardened、发布验收或刷新 ship。

## Entity provider/root ABI

``entity_provider.h`` 提供 adapter-neutral ``context_entity_provider_descriptor``，可携带
一个 descriptor-owned ``entity_root_contribution_descriptor``。脚本 host 只能在 canonical
``plugin_context_t`` 上登记 snapshot/action callbacks；loader 负责：

1. 用 owner plugin ID canonicalize provider ID，并分配单调 generation；
2. 在 enable/disable/unload 中统一 activate、quiesce、rundown 和 destroy；
3. 深拷贝 callback rows，在同一 catalog revision 中发布 provider 与由其 rows 派生的 root
  action refs；
4. 对 UTF-8、数量、字段/快照字节、重复 provider/root/contribution identity 和 generation
  执行 fail-closed 校验；
5. stale/disabled invocation 分别返回 invalid-handle/busy，不把 host callback 生命周期泄漏给
  launcher。

launcher token、D1 root registry 与 Entity complete-tree publication 不属于该 C ABI。
CPython/pymini、Lua、Emma、AngelScript、managed C#/csmini 的宿主桥均复用该 contract；
通用 JSON SDK request 没有菜单 callback 字段，不是这些宿主的菜单入口。

## plugin.json Schema (1:1 对齐 Python 平台)

`manifest_locale_resolver` 对 `locales_json` 有界解析，区域键忽略大小写并将 `_`
归一为 `-`，按完整区域→语言部分→manifest 原文逐项回退。空、非字符串、含 NUL
及超预算翻译不进入展示；launcher 将 Windows 用户区域语言接到管理器名称/描述、
tab 名称与 `sao_menu.actions.<id>.label`，Entity 刷新时重新同步。此机制不修改 ID、
动作 token、运行时或权限，也不自动翻译脚本硬编码文字。

| 字段 | 类型 | 必填 | 老别名 | 说明 |
|------|------|------|--------|------|
| ``id`` | string | 是 | | 唯一 id (小写下划线) |
| ``name`` | string | 是 | | 显示名 |
| ``version`` | string | 是 | | 语义化版本 |
| ``description`` | string | 否 | | 简介 |
| ``entry`` | string | 否 | | 缺失时按 language 猜 plugin.py/.emma/.as/.lua/.cs |
| ``language`` | string | 否 | ``engine``, ``runtime`` | python/emma/angelscript/lua/csharp; 缺失时从 entry 扩展名反推 |
| ``enabled`` | bool | 否 | | 默认 false, 用户设置持久化后覆盖 |
| ``requires`` | array or object | 否 | ``deps`` | 前置插件/特性; 数组或字典两种形式都接受 |
| ``permissions`` | array | 否 | | 沙箱白名单 (fs/net/process/hotkey/memory_access/…) |
| ``game_ids`` | array | 否 | | 只对某些 game 生效 (对齐 star_resonance) |
| ``capabilities`` | array | 否 | | 字符串或对象; 每对象 ``{id, title, description?, actions?}`` |
| ``settings_schema`` | object | 否 | | ``{key: {type, default, description}}`` |
| ``sao_menu`` | object | 否 | | 环形菜单集成 |
| ``locales`` / ``i18n`` / ``translations`` | object | 否 | | i18n 覆盖 (三个字段任一识别) |
| ``hotkeys`` | object | 否 | | ``{hotkey_id: "F6"}`` 默认键 |
| ``primary`` | bool | 否 | | 是否主面板 (默认 true) |
| ``hidden`` | bool | 否 | | 是否默认隐藏 |
| ``min_width`` / ``min_height`` | int | 否 | | 面板最小尺寸 |
| ``mcpServers`` | object | 否 | | AI Editor MCP 声明 |
| ``chatProviders`` | array | 否 | | AI Editor chat provider 声明 |
| ``protected`` | bool | 否 | | workshop 保护插件 (native protection) |
| ``native_entry`` | string | 否 | | 保护插件的 native dll 名 |
| ``native_abi`` | string | 否 | | 保护插件的 ABI 版本 |
| ``abi_version`` | int | 否 | | 声明的 ABI 版本; 缺失 (0) → 视为 v1, compat 层升到 v2 |

## 旧插件迁移 (零改动)

- Python 平台的 ``plugin.json`` **一个字不改** 直接可用
- ``entry`` 可省略 → 按 language 猜 ``plugin.<ext>``
- 老的 ``requires: []`` 数组和新的 ``requires: {"runtime_features": [...]}``
  字典两种形式都被 ``plugin_manifest`` 解析器接受
- 老 ``deps`` 字段作为 ``requires`` 别名保留
- 老 ``engine`` / ``runtime`` 字段作为 ``language`` 别名保留
- 老 ``i18n`` / ``translations`` 作为 ``locales`` 别名保留

**历史兼容目标**（旧 Python 平台来源清单，不代表本轮 native 运行验收）:

- ``hide_seek_plugin`` (纯 Python + cv2)
- ``midi_piano_plugin`` (Python + vendor 依赖 bootstrap)
- ``star_resonance_plugin`` (Python + mem_probe 集成)
- ``example_emma_plugin`` (Emma DSL)
- ``example_angelscript_plugin`` (AngelScript 子集)
- ``example_lua_plugin`` (Lua via lupa → C++ 侧真 Lua 5.4)
- ``example_csharp_plugin``（当前示例为显式 csmini 子集；预编译 hostfxr 示例为
  ``hello_csharp``，宿主不自动执行 Roslyn 源码编译）

## 目录扫描规则

三类根 (对齐 Python default_plugin_dirs):

1. **builtin_roots** — ``<base>/plugins/``, 只读, 不允许 uninstall
2. **user_roots** — ``<base>/user_plugins/``, 可写, 允许 uninstall
3. **workspace root** — 向上走探测 ``.git`` / ``sao_auto`` / ``.vscode`` /
   ``tools`` / ``.github`` marker, 找到后再看 ``<workspace>/plugins/``

每根按 ``max_depth`` 递归 (默认 1 = 只扫直接子目录)。每子目录含
``plugin.json`` 即当作一个插件目录。

## vcpkg 依赖

- ``fmt`` (日志格式化, 可选)
- `nlohmann_json` 3（必需；loader 公开 manifest 头使用，由 target 的 PUBLIC 依赖传递）
- 无 3rdparty 引擎依赖 (那些落在 ``*_host/`` 各自)

## 历史测试（2026-08-23 已删除）

旧 ``tests/`` 目录、``SAO_BUILD_TESTS`` 与 CTest 注册已删除；本节不再是可运行入口。
