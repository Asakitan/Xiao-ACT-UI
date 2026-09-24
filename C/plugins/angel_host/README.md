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
| ``as_engine_preamble.cpp`` | 生成 ``namespace sao_engine`` 段（catalog→typed+``_raw`` 包装），载入时注入每个 plugin module |

## 关键设计

**真 SDK 已内嵌**：默认使用仓库中的 ``vendor/angelscript_2.36.1`` 源码，
运行插件无需另装外部 runtime；配置时找到的 vcpkg/upstream SDK 优先。
显式强制 fallback 或没有可用 SDK 时才构建返回 NOT_IMPLEMENTED 的 stub。

**每插件一个 asIScriptModule**: 隔离编译, 一个插件的语法错误不影响别的
plugin 的 module。

## 可选 SDK 来源

- ``unofficial-angelscript`` / upstream Angelscript package（头 + lib）。
- ``SAO_ANGELSCRIPT_SOURCE_DIR``（离线 SDK 源码；默认指向仓库内嵌版本）。

## Native SDK helper 隔离

共享 runtime bridge 的逻辑名是 ``act_plugin_<plugin_id>__<stem>``；同一文件重复
``load_local`` 和不同目录同 stem 都得到同一逻辑名。旧 helper 用该名字执行
``GetModule(..., asGM_ALWAYS_CREATE)``，第二次加载替换第一次的模块；旧代理析构又
按相同名字 ``DiscardModule``，会删除新代理使用的模块。

``as_ctx_surface.cpp`` 现在为每个 helper 分配独立 ``sao_local_helper_g<N>`` 物理名，
``module_id()`` 仍返回原逻辑名。分配、查找、调用和析构均持有 execution guard；
序号到达 ``uint64_t`` 上限后报错，不回绕复用，已占用物理名直接跳过。
创建前建立 RAII owner，构建、绑定或后续分配失败只清理本实例；析构和访问都核对
模块指针。原生 ``get`` 产生的函数值持有 helper owner，避免代理释放后函数失去模块。

helper 与入口一样执行 legacy source rewrite、条件注入 ``PluginContext@ ctx`` 和
``sao_engine`` preamble，并检查 context 绑定结果；调用携带原 plugin userdata，
保留嵌套 ``load_local`` 的 owner。没有新增 loader-live 状态门槛。
编译错误携带源文件 section、行列和 SDK 消息；执行异常沿现有 exception formatter
保留函数、位置与异常文本，``LocalModule.call/get`` 的失败转成脚本异常。
共享 missing / unsupported / path_only 成功空模块语义保持不变。

当 `ctx.load_local()` 指向 `.py` 时，调用进入共享 pymini-first/CPython-second provider，
不在 AngelScript state 内执行 Python。只有 pymini 预检明确不支持才惰性初始化 CPython；
跨语言只暴露 JSON-compatible member 与 callable。`script_world_clock_angelscript`
已通过该链调用 Pillow `candy_render.py`，提交多节点 overlay 并完成正常 unload；这不改变
本宿主没有源码级 `#include` 预处理的边界。

源码核对发现入口与 helper 都直接 ``AddScriptSection``，没有 ``#include`` 文件预处理；
本切片没有变更入口编译器或引入 include loader，fixture 使用实际支持的
``ctx.load_local``，不宣称 ``#include`` 兼容。

### 成功 fixture 与断言

``tools/provider_probe/fixture/native_sdk/angelscript/``（相对原生构建根）只有根
``plugin.json``，ID 为 ``sdk_angel``、language 为 ``angelscript``、enabled 为 true。
入口 ``plugin.as`` 在 ``on_load`` 内依次验证：

- 同一 counter helper 重复加载后旧状态保留、新状态独立，逻辑 ID 不变。
- 先释放新代理再调用旧代理，以及重新加载后释放旧代理再调用新代理。
- ``left/shared.as`` 与 ``right/shared.as`` 同 stem、不同初值和 origin，互不替换；
	两种释放顺序后幸存代理均可继续调用。
- 显式声明和自动注入的 helper ``ctx`` 均继承入口路径；helper 内嵌套加载也保留 owner。
- ``LocalModule.call/get`` 返回 ``json@``，标量断言用实际 ``stringify()`` API，
	没有假定不存在的 ``as_int`` 或直接函数代理 API。

断言先记录 ``SDK_ANGEL_ASSERT_FAIL <label>``，再故意执行 ``json_parse("{")``
触发现有 JSON 异常，终止 hook；全部断言且显式代理释放完成后才输出：

```text
SDK_ANGEL_NATIVE_OK repeated=isolated siblings=isolated release=both_orders owner=inherited
```

### 后续错误场景建议

- helper 语法错误、缺 include 文件或不支持的 ``#include``：应保留编译行列与文本，
	不转成 missing/unsupported，也不执行另一 provider。
- helper 内无效 JSON、越界访问或其他执行异常：应终止入口 hook，保留 helper 函数与位置，
	不出现成功 marker。
- 文件解析后消失、读失败、绑定失败、已有同名物理模块、序号耗尽：应失败或跳过占用名，
	既存 helper 不被删除，失败构建不遗留本实例模块。
- missing/unsupported/path_only：仍按共享成功空模块合同验证，不将其当作执行异常。
- 原生函数值持有 helper 后释放外层代理、最终函数值释放和 host teardown：单独验证
	owner 生命周期；AS ``get`` 的函数值 JSON 转换不等于可调用的脚本函数句柄。

session-81 审阅补修后定向 Debug 与 provider888/0 已通过；三种 runtime 配置下实际输出成功
marker，重复/同名 helper、双释放顺序与嵌套 ctx 均完成，且无 Python/.NET DLL。
helper 编译与 JSON 执行异常都保留终止诊断，独立原生插件继续，正常 shutdown=0。
文件竞态、序号耗尽、故障注入和一般并发退休未实测。

## 历史测试（2026-08-23 已删除）

旧 ``tests/``、Catch2 与 CTest 注册已删除；此前依赖 SDK 的集成结果仅作历史证据。
