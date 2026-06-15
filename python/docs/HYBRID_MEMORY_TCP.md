# 混合 TCP + 内存数据源

> 当前版本：`4.6.74`（review-chain Batch 202）。本文与 `docs/ACT_PLATFORM.md` 中的"解析器适配器"与"数据源健康"小节配套阅读。

本文描述 SAO Auto ACT 平台保守的数据源策略。

简单地讲：TCP 是安全的战斗数据源；内存探测必须只读、聚焦自身状态，并在失败时安全回退到 TCP 可见的健康状态，而不是凭空捏造战斗数据。

## 安全边界

允许：

- 只读的进程内存读取。
- 由 TCP 确认的语义锚点。
- 低频自身状态轮询。
- 验证后复用缓存的地址。
- 清晰的健康上报与回退原因。

不允许：

- 内存写入。
- 代码注入。
- Hook 或 detour。
- 绕过反作弊控制。
- 把易变的堆地址当作长期事实。
- 在没有显式校验的情况下让内存数据覆盖 TCP 战斗事实。

## 运行时模式

`PacketBridge` 接受这些 `data_source` 模式：

| 模式 | 行为 |
| --- | --- |
| `tcp` | 启动 TCP 抓包与 `StarResonanceParserAdapter`；不启动内存源。 |
| `memory` | 仅启动 `UnifiedDataSource`。当前作用域只覆盖自身状态；战斗/实体/boss/场景流不会由 memory-only 模式产生。 |
| `hybrid` | 启动 `UnifiedDataSource`，再保持 TCP 抓包作为战斗/实体/boss/场景的事实来源。当内存可用时丰富自身身份/HP/资源。 |
| `auto` | 先尝试 memory/hybrid 启动；失败时尽可能回退到 TCP。 |

## 当前实现

`mem_probe/unified_source.py` 提供 `UnifiedDataSource`，是 `PacketBridge` 在 memory/hybrid/auto 模式下期望的入口。

重要约束：

- `UnifiedDataSource` 不再创建另一个 `PacketBridge`，因此启动不会递归。
- 它把内存自身状态工作委托给 `MemStateBridge`。
- 它把活跃的 `PacketBridge` 传给 `MemStateBridge`，让 TCP 解析器状态可用于构建锚点包。
- 不支持的数据流仍按 TCP-owned 或 fallback-owned 上报。

健康中当前的 watcher 归属：

```json
{
  "self": "memory_first",
  "combat": "tcp_fallback",
  "entity": "tcp_fallback",
  "boss": "tcp_fallback",
  "scene": "tcp_fallback"
}
```

## 锚点策略

内存路径在扫描之前应该使用由 TCP 确认的语义锚点。

锚点示例：

- 已确认的玩家 uid；
- 解析器状态中的等级/职业事实；
- 观测到的技能等级 id；
- 可用时的场景/副本上下文。

`MemStateBridge` 会通过 `AnchorMemoryReader.build_anchor_from_parser(parser)` 由实时 packet parser 构造 `AnchorPack`。

`AnchorMemoryReader.find_self(anchor)` 仅在 `anchor.is_strong()` 为 true 时才会扫描。候选地址需通过回读交叉校验，例如 uid、最大 HP、角色等级、职业、匹配的技能 id 等。

## 冷启动定位与名称表

为了在没有静态偏移的情况下也能稳定锚定，关键扫描循环（块读、区域提示、共享 IL2CPP 类索引）已迁移到 Cython（`_sao_cy_*` 模块）。在锚点弱时，TCP 场景锚点 `scene_id`、`scene_uuid` 还会作为内存 base 候选的过滤条件，进一步收窄首轮扫描面。

通用 `skill` 表也按语义拆成多张表：skill / dungeon / monster / boss / nameplate / map name / banner 等，运行时由 `NameResolver` 路由；`skill` 仍作为兼容 fallback。这些事实由 `tools.tablekit.combat_preparse` 写入事件 `payload.combat_fact`。

## 缓存与混合补充链

TCP/MEM 缓存路径已改为隐私感知的本地缓存：运行时缓存只本地保存，签入仓库的缓存不再包含原始包/私有 payload。

混合补充链（自身锚定、活体名表填充、ACT 重定位、补充 MEM 路径、铭牌来源、地图中心/横幅命名、Mem Scope 共享区域等）全部由 `MemStateBridge` 暴露，并接入 watcher 归属上报。Mem Scope 对齐与搜索结果会被 clamp，源徽章与搜索数值有守卫，畸形 status/catalog/entity/damage payload 不会污染状态。

## 健康字段

`PacketBridge.health()` 返回顶层桥状态，并在内存/解析器存在时一并包含其细节。

典型顶层字段：

- `data_source`
- `running`
- `error_msg`
- `parser_adapter`
- `parser_adapter_selection`：请求的适配器 id、最终选择的适配器 id、选择模式（`builtin` 或 `plugin`），以及实时桥拒绝某请求的插件适配器时的回退原因。
- `mem`

`UnifiedDataSource.health()` 包含：

- `data_source`：`unified`
- `requested_mode`
- `mode`
- `running`
- `started`
- `alive`
- `is_memory_active`
- `status`
- `last_error`
- `started_at`
- `uptime_s`
- `watchers`
- `policy`：内存扫描的有效门与 provider 扫描状态
- `self`
- `snapshot_available`

数据源健康 UI 应优先使用这些健康字段，而不是依据模式标签猜测。

## 失败与回退规则

- 在 `memory` 模式下内存启动失败时，桥应明确报错，而不是悄悄声称已有战斗数据。
- 在 `auto` 模式下内存启动失败时，TCP 可继续作为安全回退。
- 当轮询持续失败时，`MemSelfStateProvider` 可以把内部模式切到 `tcp` 并回报错误。
- 不可能成立的自身状态值必须忽略或视为陈旧。
- 缺少 Cython scanner 支持时必须以可见警告降级，而不是让包路径崩溃。

## 配置建议

默认推荐 TCP 作为运行模式。

已实现的内存扫描门：

- `mem_data_source`：选择 `tcp`、`memory`、`hybrid` 或 `auto`。
- `mem_auto_scan_enabled`：为 false 时 `UnifiedDataSource.start()` 安全失败，`hybrid`/`auto` 可继续走 TCP 回退。
- `mem_auto_scan_interval_s`：把 provider 轮询限制到 `0.1..30.0` 秒。
- `mem_require_admin`：为 true 时若进程未提权则拒绝内存启动。
- `mem_max_scan_regions_mb`：限制锚点扫描使用的私有可读内存区域集；`0` 表示不限。
- `mem_allow_static_fallback`：为 false 时 provider 拒绝静态包回退，仅使用锚点命中。
- `mem_show_risk_warning`：在策略健康中报告，供 UI/runtime 决定是否显示风险提示。

这些门通过 `UnifiedDataSource.health().policy` 上报，包括 provider 轮询间隔、静态回退模式、区域扫描上限以及区域列表是否被截断。

## 故障排查

使用共享数据源健康 helper：

```powershell
e:\Py\python.exe -m unittest tools.unified_source_selftest tools.act_data_source_health_selftest
```

当游戏与 UI runtime 都可用时的人工检查：

- `getDataSourceHealth` 显示请求模式与活跃的内存状态。
- 在 `hybrid` 下 TCP 伤害仍在更新 DPS。
- 内存 HP/自身身份能够更新而不产生战斗事件。
- 弱锚不会触发宽扫描。
- 切换 `tcp`/`hybrid`/`auto` 干净重启。
- 健康上报回退原因，而不是隐藏内存失败。

## 验证

推荐脱机验证：

```powershell
e:\Py\python.exe -m py_compile mem_probe\unified_source.py mem_probe\il2cpp\mem_state_bridge.py mem_probe\il2cpp\mem_self_state_provider.py mem_probe\il2cpp\mem_state_anchor.py net\packet_bridge.py
e:\Py\python.exe -m unittest tools.unified_source_selftest tools.act_data_source_health_selftest
e:\Py\python.exe -m act_replay.selftest
git diff --check
```

实地验证必须人工开启，不要让 CI 类测试依赖一个真实的游戏进程。

## 升级建议

升级到 `4.6.74` 后，建议在切换运行模式前查看健康字段中的 `policy.region_scan_capped`、`policy.static_fallback_mode` 与 `requested_mode/mode` 差异，确认运行时门已生效。当 Cython memscan 不可用、私有区域不可读或锚点弱时，`UnifiedDataSource` 会显式 fail-closed，并经 `policy` 与 `error_msg` 暴露原因；`PacketBridge` 仍可以独立保持 TCP 战斗源运行。
