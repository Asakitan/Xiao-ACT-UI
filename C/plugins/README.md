# plugins/ — 多引擎插件宿主 (SAO Auto C++ 平台)

本目录承载**独立 C++ 平台**的插件系统。5 种插件语言并存, 平台**不 import 任何具体
插件** —— 平台代码永不引用具体插件的符号或路径, 插件通过 SDK C ABI (由 platform/sdk
的 ``sao/sdk/*.h`` + ``sao/scripting/*.h`` 提供) 反向注册能力。

## 目录导览

| 模块 | 定位 | 对齐 Python 源 |
|------|------|----------------|
| ``loader/`` | 通用插件加载器 (发现/生命周期/隔离) | ``act_platform/plugins.py`` |
| ``sdk_binding/`` | SDK C ABI ↔ 5 种脚本引擎的桥 | ``act_platform/scripting/*.py`` |
| ``python_host/`` | 嵌入 CPython 3.11 | ``plugins.py`` (Python 分支) |
| ``emma_host/`` | Emma 自研解释器 | ``scripting/emma_runtime.py`` |
| ``angel_host/`` | AngelScript 真 SDK 嵌入 | ``scripting/angel_runtime.py`` |
| ``lua_host/`` | Lua 5.4 真 C API 嵌入 | ``scripting/lua_runtime.py`` |
| ``csharp_host/`` | .NET hostfxr，加载预编译托管组件 | ``scripting/csharp_runtime.py`` |
| ``pymini/`` | 原生 Python 子集 + 可选 CPython 委托 | Python 语言的 composite adapter |
| ``csmini/`` | 原生 C# 子集 + 可选 coreclr 委托 | C# 语言的 composite adapter |
| ``compat/`` | 旧 Python 平台插件的向下兼容层 | ``plugin_install.py`` + ``plugin_deps.py`` |
| ``examples/`` | 5 种引擎的最小示例插件骨架 | ``python/plugins/example_*_plugin/`` |
| ``hide_seek/`` | 首方 hide-seek 插件 (旧) | (直接 cv:: 端口) |
| ``midi_piano/`` | 首方 MIDI piano 插件 (旧) | (手写 SMF reader) |

## 五种语言身份与运行模式

| `language` | 运行模式 | 作者入口与前置条件 |
|---|---|---|
| `python` | `py_runtime: pymini / cpython / auto` | `.py`；pymini 原生子集，CPython 需要可用 `plugins.python_home` 与依赖模块 |
| `lua` | Lua host | `.lua`；ctx userdata 用冒号，`ctx.ui` 构造器用点号 |
| `emma` | 内置 Emma host | `.emma`；解释器支持的 DSL 与已绑定 ctx 方法 |
| `angelscript` | AngelScript host | `.as`；真实静态类型，`PluginContext@`、`dictionary@` UI |
| `csharp` | `cs_runtime: csmini / coreclr / auto` | csmini `.cs` 静态 hooks；coreclr 预编译 `.dll` + runtimeconfig + `managed_type` |

Python `auto` 先进行源码/本地依赖子集预检，再按需要委托 CPython；
C# `.cs` 使用 parser-backed 子集预检，子集外明确要求预编译，不自动源编译。
pymini 当前原生覆盖 walrus、同步 async/await/async for/with、65,536 项上限的
eager generator、包导入及 `uuid`/`secrets`/`statistics`/`importlib`；csmini
原生覆盖常用 LINQ、独立 `HashSet<T>`、同步 Task/ValueTask、Guid、attribute、
`init` 与 `lock`。两者面向不依赖二进制扩展、真实异步调度、P/Invoke 或平台专用
第三方库的基础插件；不把这些扩展写成完整 CPython/.NET 兼容声明。
Python composite 注册不加载 CPython；只有选中完整 Python 的插件真正加载时才初始化。
缺少 Python 时，launcher 延期相应插件和依赖者，独立 pymini 插件仍可原生运行。
语法/文件错误与运行期异常保留插件错误，执行后不换解释器重试。
托管 C# 当前通用加载器只接受预编译 DLL；不存在自动 Roslyn 源编译链。
csmini 注册只保存配置，原生 `.cs` 不加载 .NET；有效托管 DLL 实际加载才初始化。
launcher 缺 .NET 时仅延期托管插件及依赖者；显式 `plugins.dotnet_root` 是唯一
探测根，空根使用默认发现，available 仅表示文件布局，详见 `csmini/README.md`。
默认 Lua/AngelScript 使用随产品构建的引擎，Emma 内置，无需安装外部语言运行库。
session-81 Review 的 metadata 根/stream 边界补修后，定向 Debug 重建与 provider888/0
（含新增损坏查询）、csmini 原生 fixture、Lua/Emma 真实菜单回调和正常清理均复验通过。
完整 UI/任意语言语法、全量产品和发布矩阵不由这些探针代替。
五个主要示例提供 `locales.zh-CN` / `locales.en-US` 名称；本地化元数据不改变语言身份或运行模式，
也不自动翻译脚本中的硬编码文案，实际界面选择由 loader/launcher 消费方负责。

## 关键设计：平台不 import 插件，兼容范围按宿主核对

Python 平台里 ``PluginManager`` 用 ``importlib.util.spec_from_file_location`` 加载
每个插件的 ``plugin.py`` (或 ``plugin.emma`` / ``.as`` / ``.lua`` / ``.cs``)。
新 C++ 平台采取**同构的反向注入**:

- **平台侧**: ``loader/`` 只做发现 + 生命周期 + ``plugin_context_t`` (SDK 的 C ABI 结构)
  注入 —— 一个 ``void*`` 传给宿主。宿主再把它映射进各语言。
- **宿主侧**: 每个 ``*_host/`` 拿到 ``plugin_context_t*``, 在自己语言里暴露 ``ctx.log``
  / ``ctx.register_ui_panel`` / ``ctx.subscribe`` 等 —— 这一步用**语言原生绑定机制**:
  - Python: 手写 ``PyModuleDef`` (``sao_sdk`` 内置模块; **不用 pybind11** —— 拉包大)
  - Lua 5.4: ctx userdata/metatable 方法及 `ctx.ui` 构造器绑定
  - AngelScript: ``asIScriptEngine::RegisterObjectType`` 把 ``ctx`` 注册为 AS 类型
  - C#: csmini 原生 ctx 桥，或托管组件接收带版本与大小的 SDK 函数表
  - Emma: 直接是 C++ 层的 ``_EmmaInterpreter`` 移植, ``ctx`` 就是宿主 C++ 对象引用

**关键含义**: 平台二进制不需要在编译期知道 "hide_seek 插件叫什么", 也不需要
链接任何插件符号。运行时 ``loader/plugin_scanner`` 扫 ``plugins/`` 和
``user_plugins/`` 目录, 按 ``plugin.json`` 的 ``language`` 字段派发给
``python_host``/``emma_host``/``angel_host``/``lua_host``/``csharp_host``, 每个宿主
从对应入口加载脚本或托管组件, 通过 SDK C ABI 反向调平台；Python/C# 构建了 mini
宿主时由 composite adapter 拥有相应语言路由。

**用户 goal 第 2 条 (核心)**: **"插件需要能直接读取旧的 5 种语言的插件, 可以直接
调用我们的系统"**。落地为:

1. manifest 由当前 `loader/src/plugin_manifest.cpp` 解析，兼容层保留部分旧字段归一化；
  支持字段与别名以源码为准，不以全部旧字段原样识别为承诺。
2. Python `on_load(ctx)` 获得选中宿主的 ctx 对象；CPython 与 pymini 的语言、导入和依赖范围不同。
3. Lua / AS / C# / Emma 通过各自绑定提供 ctx 方法；名字相同不代表参数、返回值、回调和能力全部等价。
4. ``compat/libs_vendor_bridge.h`` 让插件的 ``libs/`` / ``vendor/`` / ``engine/``
   目录自动前插到宿主的模块搜索路径 (对齐 ``plugin_deps.py``)。

**加载流程 stepwise** (对齐 Python ``PluginManager.load_plugin``):

```
scanner.discover()                          → 每 plugin_dir 读 plugin.json
compat.parse_manifest_json()                → 手写 JSON 解析器 → plugin_manifest
compat.normalize_v1_manifest()              → 补默认字段 / 老别名映射
registry.add_plugin(manifest)               → 建 handle
lifecycle.topo_sort()                        → 按 requires 拓扑排序
compat.discover_deps_dirs(plugin_dir)       → 找 libs/ vendor/ engine/
plugin_deps.ensure()                        → 前插到语言侧模块搜索路径
lifecycle.register_host_adapter()           → 各 host 已在启动时注册
lifecycle.load(handle)                       → 派发 vtable.load_plugin
  → py_host.load_plugin(manifest, ctx)      → importlib.spec_from_file_location
  → emma_host.load_plugin(manifest, ctx)    → interpreter.execute
  → lua_host.load_plugin(manifest, ctx)     → luaL_loadfile + lua_pcall
  → angel_host.load_plugin(manifest, ctx)   → CScriptBuilder.BuildModule
  → csmini.load_plugin(manifest, ctx)       → 子集解释执行，或委托预编译托管组件
lifecycle.call_on_load(handle)               → 每 host 的 vtable.call_on_load(ctx)
lifecycle.call_on_enable(handle)             → 用户已启用时立即激活
```

## Emma 是什么

Emma 是 SAO Auto 平台**自研的、零依赖的、内置的**脚本语言。定位是**给不想装 Python
/ Lua / .NET 却又想写插件的作者一个开箱即用的选择**。设计目标:

1. **语法接近 Python**: ``let``, ``fn``, ``if/elif/else/end``, ``while``, ``for x in
   list``, ``return``, ``and/or/not``。缩进不参与语法, 显式 ``end`` 结束块 (类似 Ruby /
   Emma 的名字灵感)。
2. **一等公民 dict / list / string**: ``[a, b, c]`` 数组字面量, ``{key: value}`` 字典
   字面量, ``"str" .. "cat"`` 字符串拼接 (Lua 风的 ``..`` 运算符)。
3. **零外部依赖的解释器**: Python 版本用 ``re`` + 手写 tokenizer/parser/interpreter
   (见 ``emma_runtime.py`` 的 ``_TOKEN_RE``, ``_Parser``, ``_EmmaInterpreter``)。C++
   版本同样零依赖 (只用 std::string / std::variant / std::unordered_map /
   std::function)。
4. **通过 ctx 访问平台**：Emma `on_load(ctx)` 获得宿主绑定对象；支持的参数和返回值
  以 `emma_host` 的当前绑定为准，不由 Python 接口名称推断。

**示例** (对齐 ``python/plugins/example_emma_plugin/plugin.emma``):

```emma
let click_count = 0

fn on_load(ctx)
    ctx.log("Emma 插件已加载!")
    ctx.register_ui_panel("emma_demo",
        {title: "Emma Demo"}, render_panel, on_panel_action)
end

fn render_panel(payload)
    return ctx.ui.panel("Emma Plugin", [
        ctx.ui.kv("点击次数", tostring(click_count)),
        ctx.ui.button("计数 +1", "count"),
    ])
end

fn on_panel_action(action_id, payload)
    if action_id == "count"
        click_count = click_count + 1
        ctx.request_redraw("emma_demo")
    end
    return {ok: true}
end
```

**为什么不用 Lua / AngelScript 代替 Emma?** 二者的原生 C 集成模式 (Lua ``luaL_*`` /
AS ``asIScriptEngine``) 对**新手插件作者**并不友好:

- Lua 需要理解 stack / metatable / colon-vs-dot method syntax
- AngelScript 需要理解静态类型 / handle (``@``) / 强类型注册

Emma 的定位是让插件作者只写 ``fn on_load(ctx) ctx.log("hi") end`` 就能开始。**它不是
产线关键路径**, 只是一个把最低门槛做进平台的语言。想要性能 / 类型的作者选 Lua /
AS / C# / Python。

## plugin.json 格式（字段示意，带注释时不是可直接解析的 JSON）

``` json
{
    "id": "唯一_id (小写下划线)",
    "name": "显示名 (可 i18n via locales 字段)",
    "version": "1.0.0",
    "description": "简介",
    "language": "python",  // "emma" / "angelscript" / "lua" / "csharp"
    "entry": "plugin.py",  // ".emma" / ".as" / ".lua" / ".cs" / ".dll"
    "enabled": false,      // 用户设置持久化后覆盖此默认
    "requires": ["star_resonance"],   // 前置插件 id / 特性
    "permissions": [],     // 沙箱白名单 (fs / net / process / hotkey)
    "capabilities": [      // 声明式能力集
        "plugin_manager",
        {"id": "ui_panels", "title": "Demo",
         "actions": ["greet", "count"]}
    ],
    "settings_schema": {   // 平台自动生成 Settings UI
        "greeting": {"type": "string", "default": "你好"}
    },
    "sao_menu": {          // 出现在 SAO 环形菜单里的按钮 (可选)
        "category": "工具", "icon": "star",
        "action_id": "script.overlay.set_enabled"
    },
    "locales": {           // 各语言覆盖层
        "zh-CN": {"name": "示例插件"},
        "en-US": {"name": "Example Plugin"}
    },
    "hotkeys": {           // 默认快捷键
        "toggle": "F6"
    },
    "mcpServers": {},      // 兼容 AI Editor MCP 声明
    "chatProviders": []    // 兼容 AI Editor chat provider 声明
}
```

**旧字段无痛保留** (见 ``compat/py_v1_manifest.h``): ``requires`` 支持列表和字典两种
形式; ``entry`` 可省略, 加载器按 ``language`` 猜 ``plugin.<ext>``; ``capabilities``
可以是字符串数组或对象数组。旧字段 ``deps``, ``engine`` (作为 ``language`` 别名) 也支持。

## 旧插件迁移策略

迁移目标是保留旧插件，逐项验证 manifest、运行模式、依赖、ctx 方法和生命周期；
复制目录与扫描发现成功本身不证明加载或 UI 行为兼容。

- ``compat/py_v1_manifest.h``: 老字段读取器 (兼容 v1 manifest 里的 ``requires: []``
  vs ``requires: {"act_platform": ">=1.0"}``)。
- ``compat/py_v1_ctx_shim.h``: 老 ``ctx`` 接口的适配器 (比如老名字
  ``register_ui_panel`` 传两个 callable 就正常工作)。
- ``compat/libs_vendor_bridge.h``: 老插件的 ``libs/`` + ``vendor/`` 目录自动前插到宿主
  的模块搜索路径 (对应 ``plugin_deps.py``)。
- ``compat/migration.h``: 用不到的老字段 (比如废弃的 ``priority`` 单位) 给一个警告
  日志, 但**不**阻止加载。

**历史迁移验证清单（2026-08-23 测试删除前）**：

下列条目只记录旧迁移覆盖面，不是当前测试入口或执行指令：

- ``hide_seek_plugin`` (纯 Python + cv2)
- ``midi_piano_plugin`` (Python + vendor 依赖 bootstrap)
- ``star_resonance_plugin`` (Python + mem_probe 集成)
- ``example_emma_plugin`` (Emma DSL)
- ``example_angelscript_plugin`` (AngelScript 子集)
- ``example_lua_plugin`` (Lua via lupa)
- ``example_csharp_plugin`` (C# via pythonnet)

## vcpkg 依赖清单

- ``lua`` 5.4 (真 ``lua_State`` 嵌入; 替代 Python 的 ``lupa`` 双跳)
- ``unofficial-angelscript`` (真 ``asIScriptEngine`` 嵌入; 替代 Python 的手写解释器)
- ``python3`` >= 3.11 (可选; 冻结态 CPython 也可以从随包目录加载)
- ``nlohmann-json`` (plugin.json 解析)
- ``fmt`` (日志格式化)

.NET 侧（非 vcpkg）使用已存在的 hostfxr/runtime 与作者预编译组件；普通示例不下载运行时。

## 构建

```bash
cmake -S . -B build -DSAO_BUILD_PLUGINS=ON
cmake --build build --config Release
```

单模块开关 (`plugins/CMakeLists.txt` 定义):

- ``SAO_PLUGINS_ENABLE_PYTHON`` (默认 ON; 嵌入支持仍取决于 Python 开发依赖)
- ``SAO_PLUGINS_ENABLE_LUA`` (默认 ON; vcpkg lua)
- ``SAO_PLUGINS_ENABLE_ANGEL`` (默认 ON; vcpkg unofficial-angelscript)
- ``SAO_PLUGINS_ENABLE_EMMA`` (默认 ON; 零依赖)
- ``SAO_PLUGINS_ENABLE_CSHARP`` (默认 ON; 托管执行需要匹配的 .NET runtime)
- ``SAO_PLUGINS_ENABLE_PYMINI`` / ``SAO_PLUGINS_ENABLE_CSMINI`` (默认 ON; 原生子集解释器)

## 关键 ABI 版本

``SAO_PLUGINS_ABI_VERSION`` = **2**  (v1 是旧的 sao_plugins.dll 单库;
v2 是新的多宿主分层布局)。任何插件 manifest 声明 ``abi_version`` 与之不匹配
时, ``loader/plugin_manifest`` 拒绝加载并给出**明确升级路径**说明。

## 当前状态

以下为接口导览，声明存在不等于所有运行模式均可调用；实际能力、回调和生命周期
以 host 实现与集成验证为准。

**具体扩展点**:

- ``loader/plugin_manifest.h`` — plugin_manifest POD 扩到含所有 Python 平台字段
  (含 ``primary`` / ``hidden`` / ``mcpServers`` / ``chatProviders`` / ``protected`` /
  ``native_entry`` / ``native_abi`` / hotkeys 列表 / settings_schema 列表)
- ``loader/plugin_context.h`` — SDK 方法从 ~10 扩到 60+ (含 compositor layer 8 项 /
  register_extension 通用 / 全部 hook 族)
- ``loader/plugin_lifecycle.h`` — 加 host_adapter_vtable 供 5 host 注册
- ``loader/plugin_scanner.h`` — 加 workspace 探测 / 三根扫描
- ``sdk_binding/binding_common.h`` — sdk_method_id 枚举列出所有 60+ SDK 方法
- ``sdk_binding/binding_python.h`` — 手写 PyMethodDef 全签名 (65+ PyCFunction)
- ``sdk_binding/binding_lua.h`` — Lua lua_CFunction 全签名 (55+)
- ``sdk_binding/binding_angel.h`` — AS RegisterObjectMethod 全签名 (45+)
- ``sdk_binding/binding_csharp.h`` — sao_csharp_ctx_* P/Invoke 全签名 (60+)
- ``sdk_binding/binding_emma.h`` — emma_ctx_method_fn 表接口
- 各 ``*_host/`` — load_plugin / call_on_load / call_on_enable / call_on_disable /
  call_on_unload 生命周期完整

**实现依据**：loader、语言宿主和 ctx 桥均有实现，能力需逐路径检查；
头文件数量不等于已验证兼容性，旧“唯一实装 compat”的骨架描述已过期。

**旧入口现状**: lua/angel/emma/csharp host 的 ``sao_plugins_*_load_script`` /
``call_hook`` / ``unload_script`` 均已实装, 只在对应 runtime gate 未满足时
返回显式 ``SAO_ERR_NOT_IMPLEMENTED``/capability 信号。python_host 旧入口
``sao_plugins_pyhost_load_script`` / ``unload_script`` 现委托进程内 CPython
singleton 的正规 ``load_plugin`` / ``unload_plugin`` 链 (借用复用 host
handle + GIL scope), singleton 未建立时报 ``SAO_ERR_NOT_INITIALIZED``,
非 embed 构建报 ``SAO_ERR_NOT_IMPLEMENTED``。

**顶层 CMakeLists** 已把 ``loader/`` ``sdk_binding/`` ``python_host/`` ``emma_host/``
``angel_host/`` ``lua_host/`` ``csharp_host/`` ``compat/`` 加进 build。

**``examples/``** 不进 build —— 只作为运行时被 ``loader/plugin_scanner`` 扫描的
骨架, 让插件作者对齐目录布局。

**``hide_seek/``** + **``midi_piano/``** 保留原样, 分别构建为
``sao_plugin_hide_seek.dll`` / ``sao_plugin_midi_piano.dll``, 上层通用 loader
通过原有 ABI 加载。

## 历史遗留说明（v1 单库骨架）

原有 ``sao_plugins.dll`` (``src/sao_plugins.cpp`` + ``sao_plugins_abi_version()`` /
``sao_plugins_set_log_callback()``) 是 v1 单库 ABI, 只覆盖 loader + Lua/AngelScript
两语言。v2 分层扩展到 5 语言, ``sao_plugins.dll`` 保留只作为 v1 兼容外层, 内部
delegate 到新的 ``sao_plugins_loader.dll`` + ``sao_plugins_*_host.dll``。
