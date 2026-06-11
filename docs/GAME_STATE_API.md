# GAME_STATE_API

面向 ACT 插件作者的游戏状态与内存扫描接口说明。

这不是游戏官方 API，而是 SAO Auto 通过 TCP 抓包、ACT 状态聚合和只读内存扫描整理出的稳定调用层。插件优先使用 `ctx.get_snapshot()`、`ctx.mem` 和 `ctx.call_runtime("act_mem_...")`；不要直接依赖底层堆地址、偏移或 reader 私有字段。

## 设计边界

- **只读**：内存路径只使用读进程内存，不提供写入、注入、Hook、Detour 或修改游戏状态能力。
- **TCP 仍是战斗事实源**：伤害事件、战斗流程、场景/副本语义仍以 TCP/ACT 事件为准；内存用于补齐自身状态、实体 HP、Boss 动作、名字和游戏内聚合表。
- **Hybrid-gated**：`ctx.mem` 在 `tcp` 模式下会返回 `{"ok": false, "reason": "mode_tcp", ...}`。需要 `hybrid`、`auto` 或 `memory` 模式才会启用内存 facade。
- **地址不稳定**：所有 `obj`、`ent_addr`、扫描命中地址都只对当前游戏进程/当前会话有效。插件不要持久化这些地址。
- **JSON-safe**：大整数/UUID 会转成字符串，地址会转成十六进制字符串，避免 JS Number 精度损坏。
- **失败闭合**：公开接口应返回 JSON 字典并尽量不抛异常；调用方必须检查 `ok`、`reason`、`hint`。

## 插件侧首选入口

### ACT 聚合快照

`ctx.get_snapshot()` 返回当前 ACT 聚合快照，适合 UI、报表、触发器、普通插件逻辑。

常用路径：

| 路径 | 内容 |
| --- | --- |
| `snapshot["context"]` | `dungeon_id`、`dungeon_scene_id`、`dungeon_name`、Boss HP/盾/破防状态、最近技能/Boss 事件。 |
| `snapshot["render_spec"]` | WebView/Entity 共用渲染契约；含 `context`、`boss`、`totals`、`rows`、`sources`。 |
| `snapshot["sources"]` | TCP/Memory 数据源健康摘要。 |
| `snapshot["live"]` | 当前 encounter 的 DPS/HPS/行数据。 |
| `snapshot["triggers"]` | ACT 触发/计时器状态。 |

```python
def on_load(ctx):
    snap = ctx.get_snapshot()
    render = snap.get("render_spec") or {}
    ctx.log((render.get("context") or {}).get("dungeon_name", ""))
```

### 内存扫描 facade

`ctx.mem` 是插件作者使用内存能力的主入口。它会复用运行时已经打开的 `MemStateBridge` / `UnifiedDataSource`，不会为插件额外打开第二个游戏进程句柄。

```python
def on_load(ctx):
    ctx.log(ctx.mem.status())

    def poll_mem():
        status = ctx.mem.status()
        if not status.get("active"):
            return
        self_state = ctx.mem.self_state()
        boss = ctx.mem.boss()
        ctx.log({"self": self_state, "boss": boss})

    ctx.set_interval(poll_mem, 1.0)
```

### Runtime action 形式

同一套能力也暴露在 `act_platform.runtime`，插件可通过 `ctx.call_runtime()` 调用。推荐写全名：

```python
ctx.call_runtime("act_mem_status")
ctx.call_runtime("act_mem_self")
ctx.call_runtime("act_mem_entities", include_monsters=True, max_per_dict=128)
```

需要直接访问引擎时，使用可信 in-process 插件句柄：

```python
bridge = ctx.get_engine("memory_bridge")
packet = ctx.get_engine("packet_bridge")
game_state = ctx.get_engine("game_state")
state_mgr = ctx.get_engine("state_manager")
```

普通插件优先使用 `ctx.mem` 和 `ctx.get_snapshot()`；只有诊断/高级插件才应读取 `memory_bridge` 等底层句柄。

## `ctx.mem` API 总览

先调用 `ctx.mem.catalog()` 可以动态查看当前可用能力、UI action 名称和示例参数。

| 方法 | 用途 | 主要返回 |
| --- | --- | --- |
| `status()` | 查询内存 facade 状态。未启用内存时也可调用。 | `mode`、`active`、`bridge`、`armed`、`provider_mode`、`last_error`、`process_attached`、`module_base`。 |
| `catalog()` | 返回当前可用内存能力清单。 | `categories[]`: `id`、`name`、`action`、`hint`、`available`、`example`。 |
| `self_state()` | 读取自身状态缓存和最近 self snapshot。 | UID、HP、职业、名字、技能 CD 数、资源、死亡状态、等级、战力、场景、体力等。 |
| `entities(include_monsters=True, include_npcs=False, max_per_dict=128)` | 当前可见实体 HP/战斗态。 | `entities[]`: uuid/base_id/kind/HP/破防/过载/眩晕/施法技能/对象地址。 |
| `boss()` | 当前 Boss 候选。 | `boss`: bossDict 优先，否则最高 MaxHP 实体。 |
| `boss_actions()` | Boss/怪施法和战斗状态流。 | `actions[]`: cast edge、skill、actor state、破防/过载/眩晕、HP。 |
| `boss_action(uuid)` | 查询单个 Boss/怪最近动作。 | `action` 或 `None`。 |
| `damage_totals(total_type=1)` | 读取游戏自己的 `DamageDataMgr` 总表。 | `totals`: uuid -> total；`source`: `live`/`cache`。 |
| `skill_damage(uuid)` | 读取某玩家逐技能伤害。 | `skills`: skill_id -> value。 |
| `attr_map(ent_addr)` | 解码某个实体的完整属性表。 | `attrs`: attr_id -> value。 |
| `resolve_name(kind, id)` | 用离线/运行时名字表解析 ID。 | `name`。 |
| `read_at(addr, dtype="u64")` | 对一个地址做多路解码。 | `hint`: u32/i32/u64/i64/f32/utf16/cstr/ptr/module/class hint。 |
| `read_many(addrs, dtype="u64")` | 批量地址多路解码，最多 256 个。 | `hints[]`。 |
| `search(value, dtype, align=0)` | 异步全堆值搜索。 | `job_id`。 |
| `search_status(job_id)` | 查询搜索任务。 | `state`、`progress`、`count`、`results[]`。 |
| `narrow(job_id, value)` | 用新值在上次命中里收敛。 | `job_id` / `state`。 |
| `search_cancel(job_id)` | 取消搜索。 | `state`。 |
| `search_list()` | 列出搜索任务。 | `jobs[]`。 |

## Runtime action 对照表

这些函数都在 `act_platform.runtime` 中，插件可用 `ctx.call_runtime("函数名", **kwargs)` 调用。

| Runtime action | 等价 `ctx.mem` 调用 |
| --- | --- |
| `act_mem_status` | `ctx.mem.status()` |
| `act_mem_catalog` | `ctx.mem.catalog()` |
| `act_mem_self` | `ctx.mem.self_state()` |
| `act_mem_entities` | `ctx.mem.entities(...)` |
| `act_mem_boss` | `ctx.mem.boss()` |
| `act_mem_boss_actions` | `ctx.mem.boss_actions()` |
| `act_mem_boss_action` | `ctx.mem.boss_action(uuid)` |
| `act_mem_damage` | `ctx.mem.damage_totals(total_type=...)` |
| `act_mem_skill_damage` | `ctx.mem.skill_damage(uuid)` |
| `act_mem_attr_map` | `ctx.mem.attr_map(ent_addr)` |
| `act_mem_resolve_name` | `ctx.mem.resolve_name(kind, id)` |
| `act_mem_read` | `ctx.mem.read_at(addr, dtype)` |
| `act_mem_read_many` | `ctx.mem.read_many(addrs, dtype)` |
| `act_mem_search` | `ctx.mem.search(value, dtype, align=...)` |
| `act_mem_search_status` | `ctx.mem.search_status(job_id)` |
| `act_mem_narrow` | `ctx.mem.narrow(job_id, value)` |
| `act_mem_search_cancel` | `ctx.mem.search_cancel(job_id)` |
| `act_mem_search_list` | `ctx.mem.search_list()` |
| `act_mem_scope_status` | 一次返回 `status` + `catalog` + `self` + `entities` + `damage` + 可选 search 状态。 |

## 返回结构细节

### `status()`

示例字段：

```json
{
  "ok": true,
  "mode": "hybrid",
  "active": true,
  "bridge": "hybrid",
  "armed": true,
  "provider_mode": "memory",
  "last_error": "",
  "process": "Star.exe#12345",
  "process_attached": true,
  "module_base": "0x7ff6..."
}
```

`active=true` 表示当前模式允许内存访问且 bridge 已挂载；`armed=true` 表示已经复用到可读进程句柄。

### `self_state()`

核心字段：

| 字段 | 来源/含义 |
| --- | --- |
| `uid` | 自身 CharId，字符串。 |
| `hp` / `max_hp` / `hp_pct` | 自身血量。 |
| `profession_id` / `char_name` | 职业与名字。 |
| `skill_cd_count` | 最近内存技能 CD 条目数量。 |
| `is_dead` | 自身死亡状态。 |
| `resources` | 资源计数，结构随游戏字段而变。 |
| `level_base` / `season_exp` / `fight_point` | 角色等级/经验/战力，能读到时返回。 |
| `stamina` / `stamina_max` / `origin_energy` | 体力/能量字段，能读到时返回。 |
| `scene_map_id` | 内存读到的当前场景 map id，能读到时返回。 |

### `entities()` / `boss()`

实体行字段：

| 字段 | 含义 |
| --- | --- |
| `uuid` | `ZEntity.Uuid`，字符串。 |
| `config_uuid` | 运行时配置 uuid，字符串。 |
| `base_id` | 模板/名字表 ID。 |
| `kind` | `entity` / `boss` / `monster` / `npc`。 |
| `name` | 已解析名字；可能为空。 |
| `cur_hp` / `max_hp` / `hp_pct` | 实体实时 HP。 |
| `obj` | 当前会话实体对象地址，十六进制字符串；可作为 `attr_map()` 输入，但不要持久化。 |
| `breaking_stage` | 破防阶段 attr。 |
| `overdrive` | 过载/狂暴 attr。 |
| `stun` | 眩晕 attr。 |
| `cast_skill_id` | 当前施法技能 attr，可能为 `None`。 |

### `boss_actions()`

每条 action 代表 Boss/怪的最新施法/状态记录：

- `boss_uuid`、`boss_base_id`、`boss_name`
- `skill_id`、`skill_name`
- `cast_edge`: `start` / `end` / `none`
- `cast_active`
- `actor_state`
- `cast_elapsed_ms`
- `cast_duration_ms`
- `cast_duration_src`: `learned` / `buff` / `none`
- `breaking_stage`、`overdrive`、`stun`、`extinction`
- `hp`、`max_hp`、`hp_pct`

### `damage_totals()` / `skill_damage()`

`total_type` 对应游戏 `DamageShowTotalType`：

| 值 | 含义 |
| --- | --- |
| `1` | 总伤害 Damage |
| `2` | 总治疗 Cure |
| `3` | 总承伤 TakeDamage |

`damage_totals()` 读取 `DamageDataMgr.totalPlayerValue_`；`skill_damage(uuid)` 读取 `DamageDataMgr.damageValue_[uuid]` 的逐技能 `actualValue`。

### `attr_map(ent_addr)` 常用 attr id

这些 ID 来自当前 reader/packet enum 使用的字段，适合诊断或插件高级逻辑：

| Attr ID | 含义 |
| --- | --- |
| `1` | 实体显示名 NAME（字符串 attr，部分实体才有）。 |
| `100` | 当前/触发技能 ID。 |
| `440` | `max_extinction`。 |
| `441` | `extinction`。 |
| `442` | `max_stun`。 |
| `443` | `stun`。 |
| `444` | `overdrive`。 |
| `455` | `breaking_stage`。 |
| `471` | `hated_char_id`。 |
| `11310` | 当前 HP。 |
| `11320` | 最大 HP。 |

`attr_map()` 的输出是诊断/高级能力，不是稳定业务 schema。普通插件优先使用 `entities()`、`boss()`、`boss_actions()` 已归一化字段。

### `read_at()` / `read_many()`

`dtype` 参数保留给 UI/API 一致性；当前实现会对地址做多路解码并返回 `hint`，而不是只返回某一种类型。

支持的扫描/解码类型包括：`i32`、`u32`、`i64`、`u64`、`f32`、`f64`、`utf16`。

### `search()` / `narrow()`

手动值搜索是异步 job：

```python
job = ctx.mem.search(12345, "u32")
job_id = job.get("job_id")

# 后续轮询
status = ctx.mem.search_status(job_id)

# 数值变化后收敛
ctx.mem.narrow(job_id, 12000)
```

当前限制：

- 最大并发搜索：2。
- 单 job TTL：约 300 秒。
- 返回结果默认截断到 200 条，内部最多保留约 200,000 命中。
- 单 region 扫描上限为 256 MB；超大扫描不要放在 UI 高频路径。

## 底层读者与游戏对象映射

插件通常不需要直接 import 下列模块，但理解它们有助于判断数据可信度。

| 能力 | 模块 | 游戏对象/链路 | 公开层 |
| --- | --- | --- | --- |
| 统一数据源 | `mem_probe.unified_source.UnifiedDataSource` | `PacketBridge` 的 `memory` / `hybrid` / `auto` 适配器。 | `PacketBridge.health()`、`ctx.get_snapshot()["sources"]`。 |
| 自身状态桥 | `mem_probe.il2cpp.mem_state_bridge.MemStateBridge` | 把内存 self/entity/boss/damage 读数推到 `GameStateManager`、`DpsTracker`、BossRaid/AutoKey。 | `ctx.mem`、`ctx.get_engine("memory_bridge")`。 |
| 自身后台 provider | `mem_probe.il2cpp.mem_self_state_provider.MemSelfStateProvider` | 轮询 `StaticDpsSource` / `AnchorMemoryReader`，失败后回退 TCP。 | `ctx.mem.self_state()`、data-source health policy。 |
| TCP 锚点定位 | `mem_probe.il2cpp.mem_state_anchor.AnchorMemoryReader` | `Zproto.CharSerialize` -> `Zproto.UserFightAttr` / `CharBase` / `RoleLevel` / `EnergyItem` / `SceneData`。 | `self_state()`。 |
| 静态 self 快照 | `mem_probe.il2cpp.static_dps_source.StaticDpsSource` | bundle/script layout + instance cache，读取 `SelfSnapshot`。 | `MemSelfStateProvider` 内部使用。 |
| 实体管理器 | `mem_probe.il2cpp.mem_entity_mgr.EntityMgrReader` | `Panda.ZGame.ZEntityMgr` 的 `entityDict_`、`bossDict_`、`monsterDict_`、`npcDict_`。 | `entities()`、`boss()`。 |
| 实体战斗属性 | `mem_probe.il2cpp.mem_entity_combat.EntityCombatReader` | `ZEntity.attrs_` -> `ZAttrCollection.cacheSlim_` -> attr id/value。 | `entities()`、`attr_map()`。 |
| 游戏伤害表 | `mem_probe.il2cpp.mem_damage_reader.MemDamageReader` | `Panda.ZGame.DamageDataMgr` 的总伤/治疗/承伤/逐技能字典。 | `damage_totals()`、`skill_damage()`。 |
| Boss 动作 | `mem_probe.il2cpp.mem_boss_action_reader.BossActionTracker` | `ZEntity` actor state、`BuffComp`、`cast_skill_id`，边沿检测施法。 | `boss_actions()`、BossRaid/AutoKey。 |
| 场景名 | `mem_probe.il2cpp.mem_map_name_reader.MapNameReader` | `SceneConfigMgr` / `Bokura.SceneTableBase` / `StringPoolRuntimeImpl`。 | `GameState.dungeon_name`、ACT context。 |
| 名牌名字 | `mem_probe.il2cpp.mem_nameplate_reader.NameplateReader` | 世界名牌 UI widget：uuid + text。 | 实体名字修正、TCP name cache。 |
| 坐标/躲避上下文 | `mem_probe.il2cpp.mem_dodge_context.DodgeContext` | Camera、玩家/实体坐标、ZoneEnt 成员列表。 | AutoDodge 内部；尚未作为通用插件 API 承诺。 |
| Cython 扫描内核 | `mem_probe.cy_memscan` / `_sao_cy_memscan` | aligned u32/u64 搜索、批量读、实体战斗批量解码。 | 所有高频 reader 的性能后端。 |

## 数据源策略与配置

`PacketBridge` 支持 `mem_data_source`：

| 模式 | 含义 |
| --- | --- |
| `tcp` | 只启 TCP 抓包；`ctx.mem` 只返回状态和 gate 错误。 |
| `memory` | 尝试只启内存源；当前不承诺 memory-only 产生完整战斗事件流。 |
| `hybrid` | TCP 负责战斗事实，内存补充 self/entity/boss/action/damage 聚合。推荐插件调试模式。 |
| `auto` | 尝试内存/hybrid，失败时保留 TCP fallback。 |

常见 gate：

| 设置 | 作用 |
| --- | --- |
| `mem_auto_scan_enabled` | false 时内存扫描拒绝启动。 |
| `mem_auto_scan_interval_s` | self provider 轮询间隔，内部夹在 `0.1..30.0` 秒。 |
| `mem_require_admin` | true 时要求当前进程管理员权限。 |
| `mem_max_scan_regions_mb` | 限制锚点扫描的 readable private region 总量；`0` 表示不限制。 |
| `mem_allow_full_heap_scan` | hybrid/auto 默认更保守，避免无界全堆扫描。 |
| `mem_allow_static_fallback` | 允许锚点失败后走静态 bundle/cache fallback；`memory` 模式默认更宽松。 |
| `mem_defer_until_tcp_scene` | hybrid/auto 默认延迟重扫描，等 TCP 场景/全量同步等强锚触发。 |
| `mem_start_on_scene` / `mem_start_on_full_sync` | 控制哪些 TCP 事件可以触发 deferred memory 启动。 |
| `mem_show_risk_warning` | 供 UI/health 展示风险提示。 |

这些值会出现在 `UnifiedDataSource.health()["policy"]` 或 `ctx.mem.status()` / data-source health 中。

## 调用建议

- 普通 UI/报表插件：先用 `ctx.get_snapshot()`，只有确实需要实时实体 HP、Boss 动作或游戏内聚合表时再用 `ctx.mem`。
- 高频轮询：`self_state()`、默认 `entities()` 会尽量用 bridge 缓存；仍建议 `0.5s` 到 `1s` 以上间隔。
- 手动搜索：只用于诊断或开发工具；不要在战斗热路径里自动发起全堆搜索。
- 地址读取：只读 `entities()[i]["obj"]` 这类当前快照给出的地址；不要读取用户输入的任意地址作为普通插件功能。
- `attr_map()`：适合高级插件/诊断，不适合作为公共业务 schema。能用 `boss_actions()` / `entities()` 就不要自己解析 attr。
- 插件事件回调应快速返回；需要持续轮询时用 `ctx.set_interval()` 或插件自己的后台线程。

## 不应承诺的能力

- 不承诺写内存、调用游戏函数、Hook 游戏函数、模拟内部 RPC。
- 不承诺跨版本固定地址或固定对象布局；布局通过名字/活对象解析和 fallback 自愈，但仍可能随更新失效。
- 不承诺 weak anchor（只有 uid）的扫描一定可信；强锚至少需要 uid + level/profession 或足够技能 ID。
- 不承诺所有实体都有名字、HP、Boss 技能时长或 Buff 源；字段缺失时应显示为空/unknown。
- 不承诺 `memory` 模式下能完整替代 TCP 战斗事件流；ACT 战斗记录仍以 TCP/replay 合同为准。
- Shield 仍是 TCP-only；内存 boss break 只覆盖破防阶段和灭却/韧性类读数。

## 最小插件示例

```python
_timer = ""


def on_load(ctx):
    ctx.log("GAME_STATE_API demo loaded")

    @ctx.on("act_snapshot")
    def on_act_snapshot(event):
        snap = event.get("payload") or {}
        render = snap.get("render_spec") or {}
        boss = render.get("boss") or {}
        if boss.get("hp_pct") is not None:
            ctx.log(f"ACT boss hp: {boss.get('hp_pct')}")

    def poll_mem():
        status = ctx.mem.status()
        if not status.get("active"):
            return
        self_state = ctx.mem.self_state()
        boss = ctx.mem.boss()
        actions = ctx.mem.boss_actions()
        ctx.log({
            "self_hp": (self_state.get("hp"), self_state.get("max_hp")),
            "boss": boss.get("boss"),
            "actions_n": actions.get("count", 0),
        })

    global _timer
    _timer = ctx.set_interval(poll_mem, 1.0)


def on_unload():
    # ctx.clear_timer(_timer) 可在保存 ctx 时调用；省略时 manager unload 也会清理插件 timer。
    pass
```

## 验证命令

从 `sao_auto` 目录运行：

```powershell
e:\Py\python.exe -m unittest tools.mem_access_selftest tools.unified_source_selftest tools.act_data_source_health_selftest
e:\Py\python.exe -m unittest tools.mem_scope_panel_selftest tools.mem_scope_ui_selftest
e:\Py\python.exe -m unittest tools.mem_entity_combat_selftest
e:\Py\python.exe -m act_platform.selftest
e:\Py\python.exe -m act_replay.selftest
git diff --check -- docs/GAME_STATE_API.md
```

Live 验证需要游戏进程和当前数据源模式支持，不应作为 CI 阻塞项。