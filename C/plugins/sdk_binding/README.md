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

## ctx_dispatch JSON 方法面（session-44）

- `sao_plugins_sdk_context_dispatch`（`binding_context_dispatch.cpp`）是各
  语言宿主的 JSON 调用面：`method_status` 门先分流 ——
  property/settings/event/ui/hotkey/timer/notification 七组可服务；
  render-hook/menu/run_on_ui/file-dialog 的 request 无 callback slot、
  loader-ctx-only 方法族与 compositor×11（platform-internal）如实
  UNSUPPORTED。完整覆盖矩阵见 `docs/plugin-abi.md` 的
  “`sao_plugins_sdk_context_dispatch` 覆盖模型”一节。
- overlay 为 surface-keyed ctx-scoped：`set_overlay` 走 ui-table 事务替换
  （新帧提交前保留旧帧，提交后再清旧 token），`clear_overlay` 走 session-44
  新增的 `sao_sdk_overlay_clear_surface` 导出；`dismiss_notify` 消费
  `sao_sdk_notify_show` 返回的 token。
- 回调 owner/release：provider_release 优先，legacy_release 为兼容位；
  两者皆空视为无 owned 资源（`call_release` null 守卫，session-44）。

## 反射引擎面（session-57/59/60/61）

### 通用分发与宿主菜单的边界

`register_menu_category`、`register_action_handler` 在 CPython、pymini、Lua、Emma、
AngelScript、managed C# 和 csmini 中走宿主本地的 Entity provider 注册及回调保活路径，
不经过缺少菜单回调字段的通用 JSON request。通用 method ID 36/37/38 和旧
`py_ctx_*`/`lua_ctx_*`/`angel_ctx_*` 导出保留二进制兼容，但不是这些语言的实际菜单入口；
其 unsupported 状态不代表宿主的 `ctx.register_menu_category` 未实现。
`register_menu_surface` 也不等同于 Entity category，当前没有通用 surface 实现。
`ctx_surface` 记录宿主实际绑定的方法名并报告声明缺口，不替代回调生命周期或执行能力检查。

- `binding_engine.h` 定义目录契约：`sdk_engine_function_desc{name, arg_names,
  arg_count, invoke, availability}` + `sdk_engine_group_table{descs, count, probe}`，
  六组 `kEngineGroup{Mem,Net,Ui,GpuHunt,Vt,Misc}` 合计 126 项，全部经
  `sao_plugins_sdk_context_dispatch` 的 `method_engine_call`/`method_engine_list`
  JSON 分发；二进制参数统一 `*_b64`，provider 缺失按 `engine_no_provider`
  fail-closed。
- `mem.read` 的 `bytes_b64` 必须连同 JSON 包落在 8 MiB 结果预算内，
  单次原始请求上限为 6 MiB - 768 B，预留 1024 B 给外层字段与状态。
  更大的读取由调用方分块，不影响 typed `SaoSdkMemTable::read`。
- **vt 组（12 项，session-61）**：`vt.provider_status` / `vt.status` /
  `vt.capabilities` / `vt.probe` / `vt.perf_stats` / `vt.list_hooks` /
  `vt.read_phys`（gpa+size→`data_b64`）/ `vt.write_phys`（gpa+`data_b64`）/
  `vt.hook_page`（gva+`patch_b64`→hook_id,gpa）/ `vt.hide_region`
  （gva+page_count+decoy_mode+`template_b64` 可选→hook_ids[]）/
  `vt.unhook`（hook_id）/ `vt.map_user`。实现落在 `platform/sdk`
  的 `SaoSdkVtTable`（`sao_sdk_vt.h` + `sdk_vt_wire.cpp`）：SDK-local POD 镜像
  rt_io proxy wire 类型，进程级 lazy `sao_rt_io_proxy_runtime_handle_t`
  （autospawn=0 attach-only），runtime/helper/驱动未就绪一律
  `SAO_SDK_ERR_UNSUPPORTED`；typed 结果携带 driver `operation_status` /
  `response_flags`。

## 历史测试（2026-08-23 已删除）

旧 ``tests/`` 与 ``*_host/tests/``、Catch2 和 CTest 注册均已删除；此前的
json_node/barrier/语言桥结果仅作历史证据。
