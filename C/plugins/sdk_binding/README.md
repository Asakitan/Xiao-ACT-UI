# sdk_binding/ — SDK C ABI ↔ 5 种脚本引擎的桥

**职责**: 5 种脚本引擎 (Python / Emma / AngelScript / Lua / C#) 都要把 SDK 的
C ABI 暴露给自己的语言。这一层做**类型转换 + 异常屏障**, 各语言侧只放语言
特化。

## 对齐的 Python 源

- ``act_platform/scripting/base.py`` (ScriptRuntime / ContextProxy / ScriptModule)
- ``act_platform/scripting/emma_runtime.py`` (_EmmaProxy 手动转发每个 ctx 方法)
- ``act_platform/scripting/lua_runtime.py`` (_LuaProxy)
- ``act_platform/scripting/angel_runtime.py`` (子集解释器直接调 ctx)
- ``act_platform/scripting/csharp_runtime.py`` (pythonnet dynamic)

## 骨架文件列表

| 文件 | 职责 |
|------|------|
| ``binding_common.h`` | 公共 json_node 中间类型 + 异常屏障 |
| ``binding_python.h`` | 手写 PyModuleDef → 内置 ``_sao_plugin_native`` 模块 |
| ``binding_emma.h`` | Emma 值 ↔ C ABI utf8 json |
| ``binding_angel.h`` | asIScriptEngine::RegisterObjectMethod 注册 SDK |
| ``binding_lua.h`` | luaL_register 注册 ctx 全局表 |
| ``binding_csharp.h`` | hostfxr GetFunctionPointer + P/Invoke 双向 |

## 关键原则

**只做转换**: 每个 binding_<lang>.cpp 只翻译类型 + 转发到 ``loader/plugin_context.h``
的 ``sao_plugins_ctx_*`` C ABI。业务逻辑一律不在这里 —— 那是 loader 的事。

**异常屏障**: 每次跨语言调用都包在 ``sao_plugins_binding_barrier`` 里, 拦
下所有异常 → 转成 ``SAO_ERR_*`` status + 错误字符串, 交给宿主决定
"这个脚本崩了/是否要 failure count +1"。对齐 Python 的
``PluginManager._record_failure``。

## vcpkg 依赖

- ``nlohmann-json`` (json_node 底层实现)
- 语言特定依赖不在这, 落在各 ``*_host/`` 里

## 测试

见 ``tests/``: 语言无关部分 (json_node / barrier) 有单测, 语言特定的桥
在对应 ``*_host/tests/`` 里跑集成测试。
