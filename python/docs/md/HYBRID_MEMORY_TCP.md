# 游戏数据源 — 插件开发者指南

> 配套文档：`PLUGIN_SDK.md`、`ACT_PLATFORM.md`、`GAME_STATE_API.md`。

本文面向**插件开发者**，介绍如何从插件中读取游戏实时数据。

**范围说明**：TCP / 内存 / 混合三种数据模式的架构思路是平台通用的——任何游戏插件都可以照这个模式实现自己的 `PacketBridge` 风格数据源。但本文中 `ctx.mem` 的具体方法和返回字段（`self_state()`、`boss()`、`entities()` 等）是内置 `star_resonance_plugin` 的实现（`plugins/star_resonance_plugin/mem/mem_access.py`），仅在该插件加载时可用。要为新游戏接入内存数据源，需要实现自己的桥接类并通过 `mem_probe.unified_source.set_bridge_classes()` 注入——细节见文末[注入自定义桥接](#注入自定义桥接高级)一节。

---

## 三种数据模式

| 模式 | 配置值 | 你能拿到什么 | 适用场景 |
|------|--------|-------------|---------|
| **TCP** | `tcp` | 战斗事件流（伤害/治疗/技能）、DPS 统计、队友列表 | 只做 DPS 统计，不需要额外信息 |
| **内存** | `memory` | 自身状态、全实体 HP、Boss 血条/破防/技能、伤害总表、逐技能伤害、场景 ID、属性表 | 纯内存读取，不抓包 |
| **混合** | `hybrid` / `auto` | **TCP 全部能力** + 内存补充（Boss 实时血条、自身状态、场景 ID、实体列表） | **推荐**。TCP 负责事件流，内存补充 TCP 拿不到的实时快照 |

- `auto`：根据运行环境自动选择 `hybrid`（有管理员权限）或 `tcp`（无权限）。
- `hybrid` 下 DPS 行和战斗事件流始终走 TCP，内存仅提供 O(1) 实时查询。
- 内存启动失败时自动回退 TCP，不会中断插件。

### 用户怎么切换

设置项 `mem_data_source`，值为 `tcp` / `memory` / `hybrid` / `auto`。插件读取方式：

```python
mode = ctx.get_setting("mem_data_source") or "tcp"
```

---

## 从插件读取游戏数据

### 方式一：事件订阅（TCP 事件流）

所有模式下可用。通过 `ctx` 订阅实时事件：

```python
def on_load(ctx):
    # 伤害事件
    @ctx.on_damage
    def handle_damage(evt):
        print(f"玩家 {evt.get('source_name')} 造成 {evt.get('damage')} 伤害")

    # Boss 事件
    @ctx.on_boss
    def handle_boss(evt):
        print(f"Boss 状态变化: {evt}")

    # 快照（定期全局状态）
    @ctx.on_snapshot
    def handle_snapshot(snap):
        print(f"当前 DPS 行数: {len(snap.get('rows', []))}")

    # 通用事件
    ctx.subscribe("scene", lambda evt: print(f"场景切换: {evt}"))
```

**可订阅的事件 topic**：`damage`、`heal`、`skill`、`boss`、`boss_state`、`self_state`、`scene`、`monster`、`encounter_started`、`encounter_updated`、`encounter_finalized`、`act_snapshot`、`trigger_fired`。

### 方式二：`ctx.mem` 内存查询（hybrid / memory / auto）

`ctx.mem` 是一个只读门面对象（`MemAccess`），在 `hybrid`、`memory`、`auto` 模式下可用。TCP 模式下调用会返回 `{"ok": False, "reason": "mode_tcp"}`。

**所有方法都返回 JSON 安全的 dict，永远不抛异常。** 内存未就绪时返回 `{"ok": False, "reason": ..., "hint": ...}`。

```python
def on_load(ctx):
    mem = ctx.mem
    if mem is None:
        print("内存接口不可用")
        return

    # 查看内存引擎状态
    st = mem.status()
    print(f"模式={st['mode']}, 活跃={st['active']}, 已附加={st['armed']}")
```

---

## `ctx.mem` API 参考

### 状态查询（所有模式可调用）

#### `mem.status() -> dict`

返回内存引擎当前状态。不受模式限制。

```python
{
    "ok": True,
    "mode": "hybrid",           # 当前数据源模式
    "active": True,             # 内存引擎是否活跃
    "bridge": "hybrid",         # 桥接类型: hybrid / memory_only / none
    "armed": True,              # 进程句柄是否就绪
    "process": "GameApp#12345", # 附加的进程
    "module_base": "0x7ff6...", # 主模块基址
}
```

#### `mem.catalog() -> dict`

列出所有可用的查询方法及其状态。适合做功能发现。

```python
result = mem.catalog()
for cat in result["categories"]:
    print(f"{cat['name']}: {'可用' if cat['available'] else '未就绪'}")
```

### 自身状态

#### `mem.self_state() -> dict`

读取当前角色的实时状态。

```python
state = mem.self_state()
if state["ok"]:
    print(f"UID={state['uid']}, HP={state['hp']}/{state['max_hp']}")
    print(f"职业={state['profession_id']}, 名字={state['char_name']}")
    print(f"等级={state.get('level')}, 战力={state.get('fight_point')}")
    print(f"场景={state.get('scene_map_id')}, 死亡={state['is_dead']}")
```

返回字段：`uid`、`hp`、`max_hp`、`hp_pct`、`profession_id`、`char_name`、`skill_cd_count`、`is_dead`、`resources`、`level`、`level_base`、`fight_point`、`scene_map_id`、`stamina`、`stamina_max`。

### 实体列表

#### `mem.entities(include_monsters=True, include_npcs=False, max_per_dict=128) -> dict`

读取当前可见的全部实体（含 TCP 未上报的屏外/开怪前目标）。

```python
result = mem.entities(include_monsters=True, include_npcs=True)
if result["ok"]:
    for ent in result["entities"]:
        print(f"[{ent['kind']}] {ent['name']} HP={ent['cur_hp']}/{ent['max_hp']} ({ent['hp_pct']:.1%})")
```

每个实体包含：`uuid`、`base_id`、`kind`、`name`、`cur_hp`、`max_hp`、`hp_pct`、`obj`（内存地址）、`breaking_stage`、`overdrive`、`stun`、`cast_skill_id`。

### Boss

#### `mem.boss() -> dict`

获取当前血量最高的 Boss 实体。

```python
result = mem.boss()
if result["ok"] and result["boss"]:
    b = result["boss"]
    print(f"Boss: {b['name']} HP={b['cur_hp']}/{b['max_hp']}")
```

#### `mem.boss_actions() -> dict`

获取所有 Boss/怪物正在施放的技能，驱动自动躲避和 Boss 机制提示。

```python
result = mem.boss_actions()
if result["ok"]:
    for act in result["actions"]:
        if act["cast_active"]:
            print(f"{act['boss_name']} 正在施放 {act['skill_name']}(id={act['skill_id']})")
            print(f"  已持续 {act['cast_elapsed_ms']}ms, 破防={act['breaking_stage']}")
```

每个动作包含：`boss_uuid`、`boss_base_id`、`boss_name`、`skill_id`、`skill_name`、`cast_edge`（`start`/`end`/`none`）、`cast_active`、`cast_elapsed_ms`、`cast_duration_ms`、`breaking_stage`、`overdrive`、`stun`、`extinction`、`hp`、`max_hp`、`hp_pct`。

#### `mem.boss_action(uuid) -> dict`

查询单个实体的动作状态。

### 伤害数据

#### `mem.damage_totals(total_type=1) -> dict`

读取游戏内置 DamageDataMgr 的每玩家总伤。

```python
result = mem.damage_totals(total_type=1)  # 1=总输出伤害
if result["ok"]:
    for uid, total in result["totals"].items():
        print(f"UID {uid}: 总伤 {total}")
```

`total_type`：`1`=总输出、`2`=总治疗、`3`=总承伤。

#### `mem.skill_damage(uuid) -> dict`

获取某玩家的逐技能伤害明细。

```python
result = mem.skill_damage(uuid=some_uid)
if result["ok"]:
    for skill_id, value in result["skills"].items():
        print(f"技能 {skill_id}: {value}")
```

### 属性与名字

#### `mem.attr_map(ent_addr) -> dict`

读取任意实体的全属性表（HP、破防、过载、眩晕、灭却、施法技能等）。

```python
# ent_addr 来自 entities() 返回的 obj 字段
result = mem.attr_map(ent_addr="0x1a2b3c4d5e6f")
if result["ok"]:
    attrs = result["attrs"]  # {attr_id_str: value}
```

#### `mem.resolve_name(kind, id) -> dict`

将 ID 解析为显示名。

```python
result = mem.resolve_name("monster", 103309)
print(result["name"])  # 怪物名
```

`kind` 可选值：`monster`、`dungeon`、`skill`、`monster_skill`。

### 原始内存读取

#### `mem.read_at(addr, dtype="u64") -> dict`

读取任意地址并多路解码。返回的 `hint` 包含 u32/i32/u64/f32/utf16/cstr 解码结果及所在模块信息。

```python
result = mem.read_at("0x7ff600001234")
if result["ok"]:
    h = result["hint"]
    print(f"地址 {h['addr']}: u32={h['as'].get('u32')}, 所在={h['in']}")
```

#### `mem.read_many(addrs, dtype="u64") -> dict`

批量读取（上限 256 个地址）。

### 异步值搜索

用于在全进程堆中搜索某个值，类似 CE 的首次扫描/再次扫描。

```python
# 首次扫描
r = mem.search(value=99999, dtype="i32")
job_id = r["job_id"]  # "s1"

# 轮询结果
import time
while True:
    st = mem.search_status(job_id)
    if st["done"]:
        print(f"找到 {st['count']} 个地址")
        for hit in st["results"]:
            print(f"  {hit['addr']}: {hit.get('as', {})}")
        break
    time.sleep(0.5)

# 改变游戏中的值后再次扫描（收敛）
r2 = mem.narrow(job_id, value=99998)

# 取消 / 列出所有搜索任务
mem.search_cancel(job_id)
mem.search_list()
```

支持的 dtype：`i32`、`u32`、`i64`、`u64`、`f32`、`f64`、`utf16`。

---

## 通过引擎句柄访问

除了 `ctx.mem`，插件也可以通过 `ctx.engine` 获取底层引擎：

```python
def on_load(ctx):
    # 获取 PacketBridge（TCP 引擎）
    pb = ctx.engine.get("packet_bridge")

    # 获取 MemStateBridge（内存引擎，hybrid/memory 下才有）
    mb = ctx.engine.get("memory_bridge")

    # 获取 DPS 追踪器
    dps = ctx.engine.get("dps_tracker")

    # 获取游戏状态管理器
    state = ctx.engine.get("state_manager")
```

**推荐优先使用 `ctx.mem` 和事件订阅**，引擎句柄仅在需要底层控制时使用。

---

## 健康检查

插件可以检查数据源的运行状态：

```python
def check_health(ctx):
    pb = ctx.engine.get("packet_bridge")
    if pb is None:
        return
    src = getattr(pb, "_mem_source", None)
    if src is None:
        return
    h = src.health()
    print(f"模式={h['requested_mode']}, 状态={h['status']}, 运行={h['running']}")
    print(f"回退原因={h.get('last_error', '')}")
    print(f"自身: UID={h['self']['uid']}, HP={h['self']['hp']}/{h['self']['max_hp']}")
```

`health()` 关键字段：

| 字段 | 说明 |
|------|------|
| `requested_mode` | 用户设置的模式 |
| `mode` | 实际运行模式 |
| `running` | 是否正在运行 |
| `alive` | 桥接是否存活 |
| `is_memory_active` | 内存引擎是否活跃 |
| `deferred` | 是否延迟启动中（等 TCP 场景事件） |
| `last_error` | 最近错误信息 |
| `watchers` | 各数据维度的来源（`memory_first` / `tcp_fallback`） |
| `self` | 自身状态快照（uid、hp、max_hp、name 等） |

---

## 注入自定义桥接（高级）

如果你在为新游戏编写适配插件，需要注入自己的内存桥接类：

```python
from mem_probe.unified_source import set_bridge_classes

def on_load(ctx):
    from .mem.my_bridge import MyBridge, MySelfStateProvider
    set_bridge_classes(MyBridge, MySelfStateProvider)
```

未注入时 `UnifiedDataSource` 无法启动内存桥接，自动回退 TCP。

---

## 完整插件示例

一个简单的 Boss HP 监控插件：

```python
"""boss_hp_monitor — 每秒打印 Boss 血量"""

_timer = None

def on_load(ctx):
    global _timer

    def poll_boss():
        mem = ctx.mem
        if mem is None:
            return
        result = mem.boss()
        if not result["ok"]:
            return
        boss = result.get("boss")
        if boss:
            print(f"[BossHP] {boss['name']}: {boss['cur_hp']}/{boss['max_hp']} ({boss['hp_pct']:.1%})")

    _timer = ctx.set_interval(poll_boss, 1.0)

    # 同时订阅 TCP Boss 事件作为补充
    @ctx.on_boss
    def on_boss_event(evt):
        print(f"[BossHP-TCP] Boss 事件: {evt.get('action', 'unknown')}")

def on_unload():
    pass  # ctx.set_interval 的清理由平台自动处理
```

对应的 `plugin.json`：

```json
{
    "id": "boss_hp_monitor",
    "name": "Boss HP 监控",
    "permissions": ["engine_access"]
}
```

---

## 注意事项

- **只读**：`ctx.mem` 只读进程内存（`PROCESS_VM_READ`），没有写入路径。
- **管理员权限**：`hybrid` / `memory` 模式需要管理员权限才能附加进程。`auto` 模式会自动检测，无权限时回退 TCP。
- **64 位地址精度**：超过 JS `2^53` 的整数会自动转为字符串，避免精度丢失。
- **线程安全**：`ctx.mem` 的所有方法是线程安全的，可以从任何线程调用。
- **预热延迟**：切到 `hybrid` 后首次内存扫描约 1-5 秒，期间部分方法返回 `not_armed`，之后都是 O(1) 缓存读取。

---

## 快速对照表

| 我想做什么 | 用什么 | 需要什么模式 |
|-----------|--------|------------|
| 监听伤害/治疗事件 | `ctx.on_damage` / `ctx.on_heal` | 任意 |
| 获取 DPS 统计快照 | `ctx.on_snapshot` / `ctx.get_snapshot()` | 任意 |
| 读自身 HP/职业/等级 | `ctx.mem.self_state()` | hybrid / memory / auto |
| 读 Boss 血条 | `ctx.mem.boss()` | hybrid / memory / auto |
| 读 Boss 正在放的技能 | `ctx.mem.boss_actions()` | hybrid / memory / auto |
| 列出所有可见实体 | `ctx.mem.entities()` | hybrid / memory / auto |
| 读伤害总表 | `ctx.mem.damage_totals()` | hybrid / memory / auto |
| 按值搜索内存地址 | `ctx.mem.search()` + `narrow()` | hybrid / memory / auto |
| 读任意内存地址 | `ctx.mem.read_at()` | hybrid / memory / auto |
| 解析怪物/技能名字 | `ctx.mem.resolve_name()` | hybrid / memory / auto |
