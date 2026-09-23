# csmini/ — 纯 C++ C# 子集解释器 (legacy `plugin.cs` 免 CoreCLR)

**职责**: 让 legacy C# 插件 (如 `plugin.cs` + `ctx.*`) 在**不依赖
.NET runtime/hostfxr/Roslyn/dotnet SDK** 的前提下运行在 C++ 插件系统上。
我们自己解析其插件结构与支持的 C# 子集语法, 映射到当前 C++ SDK /
Plugin API, 由 C++ 原生执行。

```
legacy plugin.cs → csmini lexer/parser → AST (tree-walk interpreter)
                 → ctx 对象绑 loader::plugin_context_t (C ABI)
```

## 基础语言兼容面

原生 `.cs` 路径现提供 array/List-backed `Enumerable` 与实例 LINQ：
`Where`、`Select`、`Any`、`All`、`First*`、`Last*`、`Count`、`ToList`、
`ToArray`、`Sum`、`Min`、`Max`、`Distinct`、`Take`、`Skip`、`OrderBy*`。
`HashSet<T>` 保留独立去重语义和集合运算，不污染普通 List/array；attribute、
常用无执行差异 modifier、单声明 `partial`、泛型类型 spelling、`init` accessor 与
单解释器 `lock` 可直接运行。

`async` / `await` 采用同步立即值模型，内置 `Task` / `Task<T>` / `ValueTask`
的 `CompletedTask`、`FromResult`、有界 `Delay`、`WhenAll`、`Result` 与
`GetAwaiter().GetResult()`；不提供线程调度或 continuation。`Guid` 提供系统
CSPRNG 驱动的 RFC 4122 v4、Parse/TryParse/Empty/ToString。真实并发、P/Invoke、
`unsafe`、`ref/out/in`、System.IO/Net/Threading、跨文件 partial 合并和任意托管库
仍保持具名 unsupported；需要这些能力的作者使用预编译 DLL + CoreCLR 路径。

## 与 coreclr 委托的复合路由

`csmini` 是 `engine_kind::csharp` 的默认生产适配器 (composite):

| manifest `cs_runtime` / entry | 路由 |
|---|---|
| `"csmini"` + `.cs` | 无执行语法/特性预检通过后由本解释器原生执行 |
| `"coreclr"` + `.cs` | `SAO_PLUGINS_ERR_UNSUPPORTED`：需要离线预编译，无源编译器 |
| `.dll`（扩展名不区分大小写）+ 缺失 / `auto` / `"coreclr"` | 校验托管 PE/CLR 元数据布局、`managed_type`、entry/runtimeconfig 文件后要求 CoreCLR |
| `"csmini"` + `.dll` | `SAO_PLUGINS_ERR_UNSUPPORTED`，不将二进制交给源码解释器 |
| `.cs` + 缺失 / `auto` | 子集内 → csmini；子集外 → `SAO_PLUGINS_ERR_UNSUPPORTED` + 特性与预编译诊断，不请求安装 .NET |

launcher 将 `plugins.dotnet_root` 配置传给 `csmini_adapter_config::dotnet_root`，
仅配置为空时传 `nullptr`；空值不表示禁用托管宿主。
注册只保存配置，不探测或初始化 hostfxr；原生 `.cs` 路径即使配置了有效
.NET 根也不初始化宿主。首次实际托管加载才解析 hostfxr、初始化宿主并注册
SDK binding provider，注册错误原样传播。显式根是唯一候选位置，失效时
不改用环境或系统根；空根按 `DOTNET_ROOT`、`DOTNET_ROOT_X64`、
`%ProgramFiles%\dotnet` 的既有顺序探测。

查询与实际加载共用无执行分类器；hostfxr 查询与初始化共用 csharp_host
的文件解析器，按数字版本选择含普通 `hostfxr.dll` 文件的最高版本目录，
跳过缺 DLL 或把 DLL 路径建成目录的候选，同版本稳定版优先于预览版。
显式根也支持直接包含 `hostfxr.dll` 的目录。文件布局可用不证明 DLL 能加载、
目标 framework/架构兼容、`managed_type` 可解析或托管入口可调用。

托管通用入口 `csharp_host/src/cs_component.cpp` 只接受预编译 DLL、runtimeconfig
和 `managed_type`；`cs_compile.cpp` 的源编译入口仍是保留 ABI。
子集外源码与显式 `coreclr` 源码均提示预编译 `.dll` 并提供 runtimeconfig 与
`managed_type`；没有自动安装 SDK、Roslyn 编译或换宿主重跑。
缺文件、非法 UTF-8/路径越界、读取失败和语法错误返回实际错误，不映射为缺运行时。
runtimeconfig 缺省由 entry 同名推导，必须是插件根内普通文件且包含合法 JSON
`runtimeOptions` 对象；托管 DLL 检查 PE/CLR、metadata 根版本区及 stream 目录/数据范围，
不执行代码，也不承担完整 metadata 表/IL 验证。
原生最小例见 `examples/example_csharp_plugin`，预编译托管例见 `examples/hello_csharp`。

## 管线

| 阶段 | 文件 |
|------|------|
| 词法 | `csmini_lexer.{h,cpp}` — C# tokenizer (interp-string islands/verbatim/`$@`) |
| 语法 | `csmini_parser.{h,cpp}` — 递归下降 → AST; `csmini_preflight_subset` 特性预检 |
| 值系统 | `csmini_value.{h,cpp}` — `CsObj`/`CsRef`, dict/array/class/instance/exception/native |
| 求值 | `csmini_interp.{h,cpp}` + `csmini_ops`/`csmini_calls`/`csmini_ast.h` |
| 内建 | `csmini_builtins.cpp`, `csmini_stdlib{,2}.cpp` — `Console`/`Math`/`Convert`/`DateTime`(unix 秒)/`JsonSerializer`/string.*/`List<T>`/`Dictionary<K,V>`/`HashSet<T>`/`Enumerable`/Task/ValueTask/Guid/异常 facades |
| ctx 桥 | `csmini_ctx.cpp` — `ctx.*` 对象 → `sao_plugins_ctx_*` C ABI（~70 方法，positional args）+ 反射引擎面 `ctx.engine.*`（`sao_plugins_sdk_context_dispatch`/`method_engine_call`，私有链接 `sao_plugins_sdk_binding`+`sao::sdk`，owned `SaoSdkContext` 经 `sao_sdk_bind_context` 惰性绑定） |
| 宿主编排 | `csmini_host.cpp`, `csmini_bridge.cpp`, `csmini_loader_adapter.cpp` |
| 公共 | `csmini_common.h` — `cs_error{code,kind,message,pos,file,features}`、`src_pos` |

## 范围模型 / 控制流

- 作用域：locals → this 成员 → class attrs (statics + bound methods) →
  globals 链式解析; class 体在 module dict 评估为 `CsClassObj`。
- 控制流靠 C++ exception 信号：`sig_raise{exc}`/`sig_return{value}`/
  `sig_break`/`sig_continue`；脚本可见异常是 `CsExcObj` 值。
- `cs_error.features` 记录触到的子集外特性名；源码分类失败返回预编译诊断，
  不委托 CoreCLR 源编译。
- System.IO/Net/Threading、运行时 attribute、跨声明 partial、真实异步调度、
  `unsafe` 与 `ref/out/in` 等仍为子集外特性（`throw_unsupported` 或 preflight
  特性名拒绝）；当前 LINQ、metadata-only attribute、async-lite 与 Guid facade
  属于上述有界原生兼容面。

预检实际解析 AST，再按语法位置与作用域检查特性；字符串/注释中的关键字、普通
contextual identifier、同名自定义成员不因文本命中而误判为 LINQ/异步。
词法/语法错误保持终止错误。该预检不是完整 C# 类型检查器；识别到不支持的语法
可提前返回，后续语法不保证已验证。

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

entry 读取检查文件类型、完整大小和每段实际读取字节数。执行前先登记解释器
所有权；初始化或执行失败后保留记录，由 lifecycle 的 `on_load` 失败分支负责
回滚，不在仍有回调时析构解释器。失败初始化不调用未就绪的 unload hook，
失败 `OnLoad` 不接受 unload veto；普通成功加载保持原 veto 语义。
卸载先排空 loader 注册与回调，再释放解释器；失败保留记录供重试。
托管 component/context/binding/session 逐项提交清理进度，component close
失败保留句柄，原始加载诊断不被清理错误覆盖。adapter 注销或 host shutdown
失败同样保留 owner；后续可重试注销，不重新注册已撤销的 adapter。

## 插件接入入口 (public header)

`include/sao/plugins/csmini/csmini_host.h`:
- `sao_plugins_csmini_register_loader_adapter(cfg, &owner)` — 注册
  `engine_kind::csharp` 生产 adapter。
- `sao_plugins_csmini_unregister_loader_adapter(owner)` — 先卸载全部
  lifecycle 插件再调用。
- `sao_plugins_csmini_adapter_requires_dotnet(owner, manifest, &required, &available, &reason)`
  — 输出先清为 `false/false/nullptr`；仅合法托管 DLL 清单令 `required=true`。
  原生源码返回 `SAO_OK` 与 `false/false`；子集外源码返回 `UNSUPPORTED` 与
  `false/false`；语法/路径/I/O 错误仍为错误。`available` 仅表示编入委托且
  hostfxr 文件布局存在，查询不加载 DLL、不执行插件/静态初始化器/SDK。
  非空 `reason` 通过 `sao_plugins_csmini_free_string` 释放。
- `sao_plugins_csmini_register_script_engine()` /
  `sao_plugins_csmini_unregister_script_engine()` — `.cs` 的 script_ctx
  provider (`runtime_bridge` priority=10)，独立于 adapter。

## Helper 生命周期

`load_local` helper 由 loader 的 per-context attachment 强持有，与调用方语言无关，
不再使用常驻 helper map。module/function proxy 只保留弱身份与值键；callback
实参、容器内和返回的 callable 使用同一调用 lease，导出的类方法同时固定原 class。
退休后调用返回 `HANDLE_INVALID`，成员枚举返回空列表，不访问旧解释器或 ctx。
在途调用或重入卸载返回 `BUSY`，保留资源供外层排空后重试；事件、平台、data source
和 Entity 注册清理完成后才调用 `csmini_drop_callbacks`，先排空 builder 的 SDK
回调再释放 cb_box、导出值和 interpreter，最终析构不持 loader/helper/ctx mutex
或解释器 guard。entry 与 helper 的 SDK engine callback 均通过 ctx 创建时记录的
弱世代身份取得 lease，过期身份不重新按 ctx 地址绑定，也不形成
attachment→interpreter→owner 强引用环；`interpreter::call` 固定 callable 强引用，
防止嵌套回调替换原绑定而提前释放正在执行的函数。
session-81 审阅补修后已完成 Debug 三定向目标与 provider888/0；session-82 的
`native_literals` 又覆盖 LINQ、HashSet、Task/ValueTask、Guid、metadata-only
attribute、`init`、单解释器 `lock` 与单声明 partial，并在当前 Debug 构建上完成
加载/启用/卸载。默认/伪 .NET 根/真实 .NET 根的原生路径均无 .NET DLL，有效托管
DLL 仍只在实际激活时初始化并完成清理。一般并发退休、全部故障注入和 CoreCLR-off
构建尚未覆盖；metadata 根/stream 边界基线证据为 `build/native-sdk-postreview-*.log`。
最终 Review 只改 AI Editor execution runtime，因此 csmini fixture 与 provider888/0
证据保持；共享全量 Debug 与 Hardened 22PE/77文件 ship/acceptance 已按最终 C++ 重跑通过。本轮证据见
[`session-82`](../../docs/session-log/session-82-mini-runtimes-vscode-execution.md)。

## 构建

`SAO_PLUGINS_ENABLE_CSMINI`(默认 ON) 控制子目录加入;
`SAO_PLUGINS_ENABLE_CSHARP` + `sao_plugins_csharp_host` 存在时定义
`SAO_PLUGINS_ENABLE_CORECLR` 私有宏启用 coreclr 委托通道。
