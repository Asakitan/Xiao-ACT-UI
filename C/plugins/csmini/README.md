# csmini/ — 纯 C++ C# 子集解释器 (legacy `plugin.cs` 免 CoreCLR)

**职责**: 让 legacy C# 插件 (如 `plugin.cs` + `ctx.*`) 在**不依赖
.NET runtime/hostfxr/Roslyn/dotnet SDK** 的前提下运行在 C++ 插件系统上。
我们自己解析其插件结构与支持的 C# 子集语法, 映射到当前 C++ SDK /
Plugin API, 由 C++ 原生执行。

```
legacy plugin.cs → csmini lexer/parser → AST (tree-walk interpreter)
                 → ctx 对象绑 loader::plugin_context_t (C ABI)
```

## 与 coreclr 委托的复合路由

`csmini` 是 `engine_kind::csharp` 的默认生产适配器 (composite):

| manifest `cs_runtime` / entry | 路由 |
|---|---|
| `"csmini"` | 本解释器原生执行 |
| `"coreclr"` | 委托 `sao_plugins_cshost_*` (仅当 .NET runtime 可用) |
| entry `*.dll` | 始终 coreclr REQUIRED（无 host → fail-closed） |
| 缺失 / `auto` | 源码预检: 子集内 → csmini; 子集外 → coreclr 委托; 无 host → `SAO_PLUGINS_ERR_UNSUPPORTED` + 特性名 |

`csmini_adapter_config::dotnet_root` 非空才尝试 coreclr 委托
(hostfxr 从 `<root>\host\fxr\<ver>\hostfxr.dll` 扫描); 为空时
coreclr/子集外路由直接 fail-closed。launcher 在 csmini 启用时以
`dotnet_root=nullptr` 注册——即不探测 .NET runtime，纯 csmini。

## 管线

| 阶段 | 文件 |
|------|------|
| 词法 | `csmini_lexer.{h,cpp}` — C# tokenizer (interp-string islands/verbatim/`$@`) |
| 语法 | `csmini_parser.{h,cpp}` — 递归下降 → AST; `csmini_preflight_subset` 特性预检 |
| 值系统 | `csmini_value.{h,cpp}` — `CsObj`/`CsRef`, dict/array/class/instance/exception/native |
| 求值 | `csmini_interp.{h,cpp}` + `csmini_ops`/`csmini_calls`/`csmini_ast.h` |
| 内建 | `csmini_builtins.cpp`, `csmini_stdlib{,2}.cpp` — `Console`/`Math`/`Convert`/`DateTime`(unix 秒)/`JsonSerializer`/string.*/`List<T>`/`Dictionary<K,V>`/异常 facades |
| ctx 桥 | `csmini_ctx.cpp` — `ctx.*` 对象 → `sao_plugins_ctx_*` C ABI（~70 方法，positional args）+ 反射引擎面 `ctx.engine.*`（`sao_plugins_sdk_context_dispatch`/`method_engine_call`，私有链接 `sao_plugins_sdk_binding`+`sao::sdk`，owned `SaoSdkContext` 经 `sao_sdk_bind_context` 惰性绑定） |
| 宿主编排 | `csmini_host.cpp`, `csmini_bridge.cpp`, `csmini_loader_adapter.cpp` |
| 公共 | `csmini_common.h` — `cs_error{code,kind,message,pos,file,features}`、`src_pos` |

## 范围模型 / 控制流

- 作用域：locals → this 成员 → class attrs (statics + bound methods) →
  globals 链式解析; class 体在 module dict 评估为 `CsClassObj`。
- 控制流靠 C++ exception 信号：`sig_raise{exc}`/`sig_return{value}`/
  `sig_break`/`sig_continue`；脚本可见异常是 `CsExcObj` 值。
- `cs_error.features` 记录触到的子集外特性名 → 供路由层决定
  fail-closed 还是委托 coreclr。
- `is`/`as`、System.IO、Guid、attributes、LINQ/async/unsafe/ref-out-in
  等为子集外特性（`throw_unsupported` 或 preflight 特性名拒绝）。

## 生命周期 hooks

```csharp
public class Plugin {
    public static void OnLoad(PluginContext ctx) { ctx.log("hi"); }
    public static void OnEnable()  { }
    public static void OnDisable() { }
    public static bool OnUnload()  { return true; } // false → veto
}
```

loader hook 名 (`on_load`… `on_unload`) 映射到类静态方法 `OnLoad`…
`OnUnload`; class 解析先找名为 `Plugin` 的 class, 否则取第一个暴露
static `OnLoad` 的 class。`OnLoad` 带参数时注入 ctx 对象。

## 插件接入入口 (public header)

`include/sao/plugins/csmini/csmini_host.h`:
- `sao_plugins_csmini_register_loader_adapter(cfg, &owner)` — 注册
  `engine_kind::csharp` 生产 adapter。
- `sao_plugins_csmini_unregister_loader_adapter(owner)` — 先卸载全部
  lifecycle 插件再调用。
- `sao_plugins_csmini_register_script_engine()` /
  `sao_plugins_csmini_unregister_script_engine()` — `.cs` 的 script_ctx
  provider (`runtime_bridge` priority=10)，独立于 adapter。

## 构建

`SAO_PLUGINS_ENABLE_CSMINI`(默认 ON) 控制子目录加入;
`SAO_PLUGINS_ENABLE_CSHARP` + `sao_plugins_csharp_host` 存在时定义
`SAO_PLUGINS_ENABLE_CORECLR` 私有宏启用 coreclr 委托通道。
