# lua_host/ — Lua 5.4 真 C API 嵌入

**职责**: 用 Lua 5.4 C API 提供进程内 Lua 插件运行时，不经 Python/lupa、外部
Lua 可执行程序或 .NET。

## 升级说明 vs Python 平台

Python 侧 ``lua_runtime.py`` 走:
```
Python plugin.py → PluginContext (Python obj) → lupa Lua proxy → LuaJIT / Lua 5.x
```

C++ 侧:
```
C++ ctx → lua_State (真 5.4) → 插件 lua script
```

插件通过宿主内嵌的 Lua 5.4 执行。

## 对齐的 Python 源

- ``act_platform/scripting/lua_runtime.py`` (_LuaProxy: 每个 ctx 方法手动
  包 Lua callback → C++ 侧改成用 lua_ref + closure 存 Lua fn ref)

## 接口文件列表

| 文件 | 职责 |
|------|------|
| ``lua_host.h`` | lua_State 生命周期 (luaL_newstate + custom allocator) |
| ``lua_module_bridge.h`` | 注册 canonical ctx userdata、回调与本地模块桥 |
| ``lua_call.h`` | plugin.lua 加载 + hook 分派 |
| ``lua_sandbox.h`` | _ENV 白名单沙箱 |
| ``lua_stdlib.h`` | 按沙箱选装 io/os/package |
| ``lua_error.h`` | 栈顶错误串 + debug.traceback → SAO_STATUS |

## 关键设计

**每插件独立 lua_State**: 一个插件的 syntax error / stack corruption 不影响
别的 plugin。

**冒号 vs 点号方法**: C++ 侧插件 ``.lua`` 里应该用 ``ctx:log("hi")`` (方法调
用, 隐式传 self); Python 侧 lupa 因为把 ctx 当 Python object 用双 syntax。
新平台推荐**统一冒号**, 老 plugin.lua 里的 ``ctx.log(...)`` 通过 metatable
的 ``__index`` 转发也能跑 (compat 层)。

**ctx.engine 反射引擎面**: ``ctx.engine`` 是 sdk_binding 引擎目录的命名
函数表 (``mem.read_u64`` → ``ctx.engine.mem_read_u64``); 位置参数按
catalog ``arg_names`` 顺序映射, 单个 dict 表按 kwargs 处理。
``ctx.engine.list()`` 返回目录数组, ``ctx.engine.on(channel, fn)`` 注册
通用回调通道, ``ctx.engine_call(name, args)`` 是透传入口。调用走
bridge 自带的 ``SaoSdkContext`` + ``method_engine_call`` JSON dispatch;
非零 status 抛 ``engine call <name> failed: <status>``, 缺 provider
fail closed。该面要求 manifest 声明 ``engine_access`` (或 ``unsafe``)。

## 运行库与构建开关

- ``SAO_PLUGINS_ENABLE_LUA=ON`` 时建立宿主 target；关闭时不建立该 target。
- 默认优先使用树内 ``vendor/lua/src``，编译 ``sao_lua_vendored`` 静态库并定义
  ``SAO_HAS_LUA=1``；``lua.c`` / ``luac.c`` 不进入该库，本地包不启动解释器进程。
- ``SAO_LUA_SOURCE_DIR`` 可指定预置源码；未解析到源码时才尝试系统/vcpkg Lua 5.4。
  此自定义包路径的链接方式由包决定，不概称所有构建均为静态链接。
- 没有解析到 Lua 且 ``SAO_PLUGINS_LUA_REQUIRE_RUNTIME=OFF`` 时，宿主 API 编译为
  明确返回不可用状态的 stub；该开关为 ON 时配置失败。
- ``SAO_PLUGINS_LUA_AUTO_FETCH`` 只是兼容开关，运行库下载已禁用。

## 本地 Lua SDK 与受控包

loader 为每个插件建立独立 state，先装标准库、按 manifest 权限 arm 沙箱，再配置
插件搜索路径与 canonical ctx，最后执行入口；本地 helper 共用调用者的 state，
不建立另一解释器或跨插件全局模块缓存。

- ``require`` 仍由 ``require``（或既有 ``unsafe``）权限控制；搜索顺序为现存
  ``engine/``、``libs/``、``vendor/``、插件根，每个根支持 ``?.lua`` 与 ``?/init.lua``。
- 路径配置使用沙箱保存的原始 package 表，即 ``require`` closure 实际使用的表，
  而非公开的裁剪副本；该既有路径正确，无需重新排序沙箱初始化。
- 唯一 searcher 仅加载插件根内的文本 Lua，解析后检查 canonical 路径边界；公开
  package 不暴露 path/cpath/searchers/loadlib。registry 的 loaded 表独立于原始标准库，
  只预装沙箱允许的模块副本，避免 ``require('package')`` 或 ``package.loaded._G``
  取回原始表；disarm 恢复原 registry loaded 表。
- ``ctx:load_local('helper.lua')`` 返回模块成员代理：helper 返回 table 时导出该表，
  否则导出 helper 的独立环境；支持标量成员和函数成员调用，函数参数/结果受既有
  script-value 转换约束，不宣称支持任意 Lua 对象或返回原生 Lua closure。
- helper 环境继承**当前沙箱** globals，并显式绑定 canonical 调用者 ctx；同一 bridge
  按 canonical、大小写不敏感的完整路径复用模块，保留状态和函数别名；不同目录同名
  helper 不合并。递归加载尚未完成的同一路径返回带文件名的错误，失败项不进入缓存。
- 模块与 callable userdata 重复创建时保留已存在的 metatable；所有 helper 引用随
  bridge 正常 teardown 失效并释放 registry ref，回调/菜单仍使用既有 owner 清理链。
- 缺少 live Lua context 为终止错误，不返回允许换 provider 的 NOT_IMPLEMENTED；
  helper 语法/执行失败保留 Lua 文件名与消息。共享 bridge 的 missing/unsupported
  成功返回空值合同不变。

## 原生 SDK fixture

``tools/provider_probe/fixture/native_sdk/lua/`` 是一个插件目录，根 manifest 为
``sdk_lua`` / ``language: lua`` / ``enabled: true``，只声明 ``require`` 权限。
入口自行用 ``error()`` 检查 root package、嵌套 vendor/libs require、裁剪 loaded 表、
helper 标量/函数、重复句柄/别名、跨 enable 保留状态、同名路径隔离、无返回值环境导出
与 helper 内菜单/动作注册。

预期 on_load 日志为 ``NATIVE_SDK_LUA_OK``，on_enable 为
``NATIVE_SDK_LUA_ENABLED``；实际触发菜单或 ``sdk_lua.verify`` 动作后才应出现
``NATIVE_SDK_LUA_CALLBACK_OK``，注册成功不等于回调已实测。
失败源位于同一插件下 ``failures/``，没有额外 manifest：复制 fixture 后将根 entry
分别设为 ``failures/load_syntax.lua`` 或 ``failures/load_execution.lua``，预期加载失败
并报告 ``syntax.lua`` 的 Lua 语法消息或 ``execution.lua`` 的
``NATIVE_SDK_LUA_EXECUTION_FAILURE``，而不是静默空值或换解释器重跑。
session-81 审阅补修后定向 Debug 与 provider888/0 已通过，三种 runtime 配置下此 fixture 的
load/enable/unload 均成功且无 Python/.NET DLL；syntax/execution helper 错误保留
原消息并完成清理。真实菜单回调输出 ``NATIVE_SDK_LUA_CALLBACK_OK``，invoke=0、
handled=1、refresh=0、shutdown=0；单独 action-handler 回调及并发退休尚未实测。

## 历史测试（2026-08-23 已删除）

旧 ``tests/``、Catch2 与 CTest 注册已删除，不再存在可运行测试入口。
