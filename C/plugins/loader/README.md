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

## Entity provider/root ABI（W21-B3c-D2）

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

launcher token、D1 root registry 与 Entity complete-tree publication 不属于该 C ABI。Python
adapter 已接入；Lua/AngelScript/Emma/C# 后续复用同一 contract。

## plugin.json Schema (1:1 对齐 Python 平台)

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

**已验证的老插件** (纯 Python, Python 平台里跑得好好的, 目标 100% 无改动加载):

- ``hide_seek_plugin`` (纯 Python + cv2)
- ``midi_piano_plugin`` (Python + vendor 依赖 bootstrap)
- ``star_resonance_plugin`` (Python + mem_probe 集成)
- ``example_emma_plugin`` (Emma DSL)
- ``example_angelscript_plugin`` (AngelScript 子集)
- ``example_lua_plugin`` (Lua via lupa → C++ 侧真 Lua 5.4)
- ``example_csharp_plugin`` (C# via pythonnet → C++ 侧 hostfxr + Roslyn)

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
- 无 3rdparty JSON 依赖 (compat/py_v1_manifest 内置手写 JSON 解析器)
- 无 3rdparty 引擎依赖 (那些落在 ``*_host/`` 各自)

## 测试

见 ``tests/`` 目录, 需要 ``-DSAO_BUILD_TESTS=ON``。
