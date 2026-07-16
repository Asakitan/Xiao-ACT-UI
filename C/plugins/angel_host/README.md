# angel_host/ — AngelScript 真 SDK 嵌入

**职责**: 用真 AngelScript SDK (asIScriptEngine 家族) 提供 AngelScript 插件运行时。

## 严重 upgrade 说明 vs Python 平台

Python 侧 ``angel_runtime.py`` 是**手写的 AngelScript-**_like_** 子集解释器** ——
- 纯 Python AST + eval
- **没有** ``asIScriptEngine`` 调用
- 不支持真 static typing / handle (@) 语义
- 是一个"能跑对齐 Emma 语法示例的 fake 解释器"

C++ 平台的 angel_host 走**真 SDK**:
- ``asIScriptEngine::RegisterObjectType`` 注册 ``PluginContext``
- ``ScriptModule::Build`` 真编译到 bytecode
- ``asIScriptContext::Execute`` 真执行 (可选 JIT via asJITCompiler)
- 真类型系统 / class / handle / reference count

因此这是 **strict upgrade**, 不是 lateral port。Python 侧写的 ``.as`` 脚本
在新 host 里也能跑 (语法是 AngelScript 的子集, 真 SDK 兼容超集), 但真 SDK
额外允许:
- ``class MyClass { ... }`` 真类定义
- ``dictionary d; d["k"] = v;`` 真 stl-style dict
- ``PluginContext@ ctx`` 真 handle (引用计数)

## 对齐的 Python 源

- ``act_platform/scripting/angel_runtime.py`` (作为语义参考, 底层实现完全不同)

## 骨架文件列表

| 文件 | 职责 |
|------|------|
| ``as_host.h`` | asIScriptEngine 生命周期 |
| ``as_module_bridge.h`` | RegisterObjectType 注册 PluginContext + ctx 绑定 |
| ``as_call.h`` | .as 编译 → Build → Execute + hook 分派 |
| ``as_sandbox.h`` | addon 白名单 (file/net 默认拒) |
| ``as_stdlib.h`` | 注册官方 addon (string / array / dictionary / math) |
| ``as_error.h`` | GetExceptionString → SAO_STATUS |

## 关键设计

**用真 SDK 意味着依赖 vcpkg**: ``unofficial-angelscript``。如果 vcpkg 里
拿不到, 本 host 用 stub 编译, 无效返回 NOT_IMPLEMENTED。

**每插件一个 asIScriptModule**: 隔离编译, 一个插件的语法错误不影响别的
plugin 的 module。

## vcpkg 依赖

- ``unofficial-angelscript`` (真 SDK, 头 + lib)

## 测试

见 ``tests/``。集成测试要求 SDK 可用, 否则跳过。
