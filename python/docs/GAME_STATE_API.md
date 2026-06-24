# 游戏状态 API 参考

> 面向插件开发者与高级用户。所有接口均为只读。

---

## 快速上手

```python
def on_load(ctx):
    # 订阅状态快照（每次状态变化时触发）
    ctx.on_snapshot(lambda snap: print(snap["context"]))

    # 主动拉取当前快照
    snap = ctx.get_snapshot()

    # 获取 GameStateManager 引擎
    gsm = ctx.engine.get("game_state")
    state = gsm.snapshot()

    # 读内存（hybrid/memory 模式下可用）
    self_info = ctx.mem.self_state()
    if self_info["ok"]:
        print(f'HP: {self_info["hp"]}/{self_info["max_hp"]}')
```

---

## 一、插件上下文 `ctx`

`on_load(ctx)` 的 `ctx` 是 `PluginContext`，提供以下能力：

### 事件订阅

| 方法 | 说明 |
|------|------|
| `ctx.on_snapshot(cb)` | 状态快照变化 |
| `ctx.on_damage(cb)` | 伤害/治疗事件 |
| `ctx.on_heal(cb)` | 治疗事件 |
| `ctx.on_skill(cb)` | 技能事件 |
| `ctx.on_boss(cb)` | Boss 机制事件 |
| `ctx.on_encounter_finalized(cb)` | 战斗结束事件 |
| `ctx.subscribe(topic, cb)` | 订阅任意 topic |
| `ctx.unsubscribe(token)` | 取消订阅（传入订阅返回的 token） |

所有 `on_*` 方法返回 `str` token，用于取消订阅。回调参数是 `dict`。

### 主动查询

| 方法 | 说明 |
|------|------|
| `ctx.get_snapshot()` | 拉取最新 ACT 快照 |
| `ctx.recent_events(limit=20, topic="")` | 最近事件列表 |
| `ctx.emit(topic, payload)` | 发布自定义事件 |

### 引擎访问

| 方法 | 说明 |
|------|------|
| `ctx.engine.get(name)` | 获取命名引擎（返回 `None` 若不存在） |
| `ctx.require_engine(name)` | 获取引擎（不存在则抛异常） |
| `ctx.call_engine(name, method, *args)` | 调用引擎方法 |
| `ctx.register_engine(name, engine)` | 注册自定义引擎 |

常用引擎名：`"game_state"`、`"dps_tracker"`、`"encounter_manager"`、`"trigger_engine"`、`"packet_bridge"`、`"history_store"`、`"auto_key_engine"`、`"boss_raid_engine"`

### 设置

```python
ctx.set_defaults({"my_threshold": 50, "show_overlay": True})
val = ctx.get_setting("my_threshold")   # 50
ctx.set_setting("show_overlay", False)
```

### UI 注册

| 方法 | 说明 |
|------|------|
| `ctx.register_ui_panel(panel_id, metadata, render, on_action)` | 注册 UI 面板 |
| `ctx.register_render_hook(surface, cb, priority=0.0)` | 注册渲染钩子 |
| `ctx.register_menu_category(name, icon, builder, priority=0.0)` | 注册菜单分类 |
| `ctx.open_window(panel_id, width, height)` | 打开面板窗口 |
| `ctx.set_overlay(surface, spec)` | 设置覆盖层 |
| `ctx.clear_overlay(surface)` | 清除覆盖层 |
| `ctx.request_redraw(surface, reason)` | 请求重绘 |
| `ctx.notify(title, message, duration_s=60, kind="plugin")` | 弹通知 |
| `ctx.toast(message)` | 短暂提示 |

### 快捷键 / 定时器

```python
ctx.register_hotkey("my_toggle", my_callback, default_key="CTRL+F9", label="切换覆盖层")
timer_id = ctx.set_interval(poll_func, seconds=1.0)   # 循环
timeout_id = ctx.set_timeout(once_func, seconds=5.0)   # 一次性
ctx.clear_timer(timer_id)
```

### 其他

| 属性/方法 | 说明 |
|-----------|------|
| `ctx.plugin_id` | 插件 ID |
| `ctx.path` | 插件目录绝对路径 |
| `ctx.web_path` | 插件 `web/` 目录路径 |
| `ctx.assets_path` | 插件 `assets/` 目录路径 |
| `ctx.mem` | 内存访问门面 `MemAccess` |
| `ctx.ui` | 声明式 UI 构建器 |
| `ctx.log(message)` | 写插件日志 |
| `ctx.load_local(relative_path)` | 加载插件目录下的 `.py` 模块（返回 `ModuleType`） |
| `ctx.load_script(relative_path)` | 脚本插件加载同语言子文件（`.lua`/`.as`/`.emma`），合并到同一作用域；Lua 也可用 `dofile` |
| `ctx.run_on_ui(callback)` | 在 UI 线程执行 |
| `ctx.ensure_requirements(install=True)` | 安装插件依赖 |

---

## 二、事件载荷格式

### 伤害事件 (`on_damage`)

```python
{
    "timestamp": float,
    "attacker_uid": int,
    "target_uuid": int,
    "skill_id": int,              # 基础技能 ID
    "skill_name": str,
    "damage": int,
    "is_crit": bool,
    "is_heal": bool,
    "is_immune": bool,
    "is_absorbed": bool,
    "element": int,               # 元素 ID（0=无）
    "attacker_is_self": bool,
    "target_is_player": bool,
    "target_is_monster": bool,
}
```

### 技能事件 (`on_skill`)

```python
{
    "timestamp": float,
    "kind": str,                  # "client_use", "server_stage_end" 等
    "skill_id": int,
    "skill_level_id": int,
    "skill_name": str,
    "skill_role": str,
    "skill_category": str,        # "player_skill", "boss_skill"
    "is_ultimate": bool,
    "is_player_skill": bool,
    "is_boss_skill": bool,
}
```

### Boss 事件 (`on_boss`)

```python
{
    "timestamp": float,
    "event_type": int,
    "host_uuid": int,             # Boss UUID
    "buff_category": str,
    "boss_mechanic_key": str,
    "boss_mechanic_label": str,
    "trigger_family": str,
}
```

### ACT 快照 (`on_snapshot` / `get_snapshot()`)

```python
{
    "live": {...},                # 当前战斗 DPS 实时数据（无战斗时为 null）
    "last_report": {...},         # 最近一次已结算的战斗报告
    "history": [...],             # 历史报告列表
    "context": {...},             # GameState 派生上下文
    "encounter": {...},           # 当前/历史战斗元数据
    "sources": {...},             # 数据源健康状态
    "render_spec": {...},         # UI 渲染契约
    "triggers": {...},            # 触发器引擎快照
}
```

---

## 三、GameStateManager

通过 `ctx.engine.get("game_state")` 获取。

### 方法

| 方法 | 说明 |
|------|------|
| `.snapshot()` | 返回当前状态的浅拷贝（线程安全） |
| `.update(**kwargs)` | 部分更新 + 通知订阅者 |
| `.subscribe(callback)` | 注册变化监听 |
| `.unsubscribe(callback)` | 移除监听 |

### 状态字段

**身份**

| 字段 | 类型 | 说明 |
|------|------|------|
| `player_name` | str | 角色名 |
| `player_id` | int | 玩家 ID |
| `level_base` | int | 基础等级（上限 60） |
| `level_extra` | int | 赛季加算等级 |
| `season_exp` | int | 赛季经验 |
| `profession_id` | int | 职业 ID |
| `profession_name` | str | 职业名 |
| `fight_point` | int | 战力 |

**生命 / 体力**

| 字段 | 类型 | 说明 |
|------|------|------|
| `hp_current` | int | 当前 HP |
| `hp_max` | int | 最大 HP |
| `hp_pct` | float | HP 百分比 |
| `stamina_current` | int | 当前体力 |
| `stamina_max` | int | 最大体力 |
| `stamina_pct` | float | 体力百分比 |

**技能**

| 字段 | 类型 | 说明 |
|------|------|------|
| `skill_slots` | list[dict] | 技能槽列表（见下方结构） |
| `custom_skill_slots` | list[dict] | 自定义技能槽 |
| `burst_ready` | bool | 大招是否就绪 |

技能槽结构：

```python
{
    "index": int,             # 槽位（0-9 主技能，10-14 自定义）
    "state": str,             # "ready" | "cooldown" | "insufficient_energy" | "unknown"
    "cooldown_pct": float,    # 0.0=就绪 ~ 1.0=满 CD
    "remaining_ms": int,      # 剩余冷却毫秒
    "charge_count": int,      # 充能层数
    "active": bool,           # 当前是否激活中
    "ready_edge": bool,       # 是否刚从 CD 变为就绪
    "insufficient_energy": bool,
}
```

**Buff**

| 字段 | 类型 | 说明 |
|------|------|------|
| `self_buffs` | list[dict] | 自身 Buff 列表 |
| `server_time_offset_ms` | int | 服务器时间偏移 |

Buff 结构：

```python
{
    "id": int,                # Buff ID
    "uuid": int,              # Buff UUID
    "begin_ms": int,          # 服务器时钟起始
    "duration_ms": int,       # 总持续时间
    "layer": int,             # 层数
    "count": int,             # 叠加数
    "name": str,              # 名称
}
```

**战斗**

| 字段 | 类型 | 说明 |
|------|------|------|
| `in_combat` | bool | 是否处于战斗状态 |

**Boss**

| 字段 | 类型 | 说明 |
|------|------|------|
| `boss_current_hp` | int | Boss 当前 HP |
| `boss_total_hp` | int | Boss 最大 HP |
| `boss_hp_source` | str | HP 数据来源 |
| `boss_hp_est_pct` | float | 估算 HP 百分比 |
| `boss_shield_active` | bool | 是否有护盾 |
| `boss_shield_pct` | float | 护盾百分比 |
| `boss_breaking_stage` | int | 破防阶段 |
| `boss_extinction_pct` | float | 削韧百分比 |
| `boss_cast_skill_id` | int | 当前施法技能 ID |
| `boss_cast_skill_name` | str | 当前施法技能名 |
| `boss_cast_active` | bool | 是否正在施法 |
| `boss_cast_duration_ms` | int | 施法持续时间 |
| `boss_stun` | bool | 是否眩晕 |
| `boss_invincible` | bool | 是否无敌 |
| `boss_in_overdrive` | bool | 是否狂暴 |
| `boss_raid_active` | bool | 团本机制是否激活 |
| `boss_raid_phase` | int | 团本阶段 |
| `boss_raid_phase_name` | str | 团本阶段名 |
| `boss_enrage_remaining` | float | 狂暴剩余时间 |
| `boss_enrage_urgency` | str | 狂暴紧迫度 |
| `boss_timer_text` | str | Boss 计时器文本 |
| `boss_mechanic_event` | dict | 当前机制事件 |
| `boss_mechanic_countdowns` | list | 机制倒计时列表 |
| `boss_total_damage` | int | 累计总伤害 |
| `boss_dps` | float | 当前 DPS |

**副本**

| 字段 | 类型 | 说明 |
|------|------|------|
| `dungeon_id` | int | 副本 ID |
| `dungeon_scene_id` | int | 场景 ID |
| `dungeon_difficulty` | int | 难度 |
| `dungeon_name` | str | 副本名 |

**事件**

| 字段 | 类型 | 说明 |
|------|------|------|
| `last_dungeon_event` | dict | 最近副本事件 |
| `last_skill_event` | dict | 最近技能事件 |
| `last_boss_event` | dict | 最近 Boss 事件 |

**窗口**

| 字段 | 类型 | 说明 |
|------|------|------|
| `window_rect` | tuple | 窗口矩形 |
| `window_width` | int | 窗口宽度 |
| `window_height` | int | 窗口高度 |

**元数据**

| 字段 | 类型 | 说明 |
|------|------|------|
| `capture_timestamp` | float | 采集时间戳 |
| `recognition_ok` | bool | 识别是否正常 |
| `packet_active` | bool | 抓包是否活跃 |
| `error_msg` | str | 错误信息 |

---

## 四、内存访问 `ctx.mem`

`ctx.mem` 是 `MemAccess` 实例。在 **hybrid** 或 **memory** 模式下可用，TCP-only 模式下所有读取方法返回 `{"ok": False, "reason": "...", "hint": "..."}`。

所有返回值是 JSON 安全的 dict（地址为十六进制字符串，大整数为字符串）。方法不会抛异常。

### 状态检查

```python
status = ctx.mem.status()
# {
#     "ok": bool,        是否可用
#     "mode": str,       当前模式
#     "active": bool,    内存桥是否激活
#     "process": str,    进程名
#     "module_base": str, 主模块基址（hex）
#     "last_error": str,
# }

catalog = ctx.mem.catalog()  # 列出所有可用读取操作及示例
```

### 自身状态

```python
me = ctx.mem.self_state()
# {
#     "ok": True,
#     "uid": str,            # 自身 UID
#     "hp": int,
#     "max_hp": int,
#     "hp_pct": float,
#     "level": int,
#     "profession_id": int,
#     "char_name": str,
#     "stamina": int,
#     "origin_energy": int,
#     "is_dead": bool,
#     "skill_cd_count": int,
#     "resources": dict,
# }
```

### 实体列表

```python
result = ctx.mem.entities(include_monsters=True, include_npcs=False)
# {
#     "ok": True,
#     "count": int,
#     "entities": [
#         {
#             "uuid": int,
#             "config_uuid": int,
#             "base_id": int,
#             "kind": str,           # "player", "monster", "npc"
#             "name": str,
#             "cur_hp": int,
#             "max_hp": int,
#             "hp_pct": float,
#             "breaking_stage": int,
#             "overdrive": bool,
#             "stun": bool,
#             "cast_skill_id": int,
#         },
#         ...
#     ]
# }
```

### Boss 状态

```python
boss = ctx.mem.boss()
# {"ok": True, "boss": {...}}  或  {"ok": True, "boss": None}

actions = ctx.mem.boss_actions()
# {
#     "ok": True,
#     "count": int,
#     "actions": [
#         {
#             "boss_uuid": int,
#             "boss_base_id": int,
#             "boss_name": str,
#             "skill_id": int,
#             "skill_name": str,
#             "cast_active": bool,
#             "cast_elapsed_ms": int,
#             "cast_duration_ms": int,
#             "breaking_stage": int,
#             "overdrive": bool,
#             "stun": bool,
#             "extinction": float,
#             "hp": int,
#             "max_hp": int,
#             "hp_pct": float,
#         },
#     ]
# }

# 查询指定 Boss
single = ctx.mem.boss_action(uuid=12345)
```

### 伤害数据

```python
totals = ctx.mem.damage_totals(total_type=1)
# {"ok": True, "total_type": 1, "totals": {uuid: damage_value, ...}, "source": str}

skills = ctx.mem.skill_damage(uuid=12345)
# {"ok": True, "uuid": str, "skills": {skill_id: damage_value, ...}}
```

### 属性读取

```python
attrs = ctx.mem.attr_map(ent_addr=0x1A2B3C4D)
# {"ok": True, "ent_addr": "0x1A2B3C4D", "attrs": {attr_id: value, ...}}
```

### 名称查询

```python
name = ctx.mem.resolve_name(kind="monster", id=114)
# {"ok": True, "kind": "monster", "id": 114, "name": "敌方木桩"}

# kind 可选: "monster", "dungeon", "skill", "monster_skill"
```

### 底层地址读取

```python
result = ctx.mem.read_at(addr="0x1A2B3C4D", dtype="u64")
# {"ok": True, "hint": {...}}

batch = ctx.mem.read_many(addrs=[0x100, 0x200], dtype="i32")
```

---

## 五、内存搜索

### 通过 `ctx.mem`（异步 Job）

适合在插件中做自定义值搜索。后台线程运行，不阻塞 UI。

```python
# 1. 开始搜索
job = ctx.mem.search(value=50000, dtype="i32")
job_id = job["job_id"]

# 2. 查看进度
status = ctx.mem.search_status(job_id)
# {"ok": True, "state": "done", "count": 42, "results": [...]}

# 3. 游戏内改变数值后，缩小范围
ctx.mem.narrow(job_id, value=49500)

# 4. 再次查看
status = ctx.mem.search_status(job_id)

# 管理
ctx.mem.search_cancel(job_id)
ctx.mem.search_list()  # 查看所有 Job
```

支持的 dtype：`"i32"`, `"u32"`, `"i64"`, `"u64"`, `"f32"`, `"f64"`, `"utf16"`

### 通过 `mem_probe.scanner`（同步）

适合脚本和 AI Editor 中使用。

```python
from mem_probe.scanner import scan, narrow
from mem_probe.process import GameProcess

with GameProcess() as proc:
    # 第一帧：扫描所有匹配值
    candidates = scan(proc, value=50000, dtype="i32")
    print(f"找到 {len(candidates)} 个候选地址")

    # 游戏内改变数值后，第二帧缩小范围
    refined = narrow(proc, candidates, value=49500, dtype="i32")
    print(f"缩小到 {len(refined)} 个地址")
```

`scan()` 参数：

| 参数 | 说明 |
|------|------|
| `pm` | `GameProcess` 实例 |
| `value` | 要搜索的值 |
| `dtype` | 数据类型 |
| `align` | 对齐字节数（0=自动） |
| `max_hits` | 最大命中数（默认 200000） |
| `max_region_size` | 最大区域大小（默认 256MB） |

### Cython 加速扫描 `mem_probe.cy_memscan`

Cython 编译的扫描内核，有纯 Python 回退。插件中一般不直接调用——`ctx.mem.search()` 已在内部使用。

```python
from mem_probe import cy_memscan

# 在 buffer 中搜索对齐的 u64 值
result = cy_memscan.find_aligned_u64_in_range(buffer, target, offset, size)
```

---

## 六、进程操作 `mem_probe.process`

需要管理员权限。进程名由调用方显式传入。

```python
from mem_probe.process import GameProcess

with GameProcess() as proc:
    print(f"PID: {proc.pid}, 进程: {proc.name}")

    # 主模块
    mod = proc.main_module()  # ModuleInfo(name, base, size)
    print(f"基址: 0x{mod.base:X}, 大小: {mod.size}")

    # 模块列表
    for m in proc.list_modules():
        print(f"{m.name} @ 0x{m.base:X}")

    # 内存区域
    for region in proc.iter_regions():
        print(f"0x{region.base:X} size={region.size}")

    # 读取
    data = proc.read_bytes(mod.base, 16)
    val = proc.read_u64(mod.base)
    name = proc.read_utf16(addr, max_len=64)
```

读取方法一览：

| 方法 | 返回类型 |
|------|----------|
| `read_bytes(addr, size)` | `Optional[bytes]` |
| `read_bytes_into(addr, buf, size)` | `int`（读入字节数，零拷贝） |
| `read_u32(addr)` | `Optional[int]` |
| `read_u64(addr)` | `Optional[int]` |
| `read_i32(addr)` | `Optional[int]` |
| `read_i64(addr)` | `Optional[int]` |
| `read_f32(addr)` | `Optional[float]` |
| `read_f64(addr)` | `Optional[float]` |
| `read_cstr(addr, max_len)` | `Optional[str]`（ASCII） |
| `read_utf16(addr, max_len)` | `Optional[str]`（UTF-16） |

---

## 七、AI Editor 引擎调用

通过 AI Editor 的 `engine()` 工具可调用：

```
engine(action="game_state")       # 当前游戏状态
engine(action="entity_list")      # 实体列表
engine(action="dps_summary")      # DPS 汇总
engine(action="system_info")      # 系统信息
engine(action="plugins")          # 插件列表
engine(action="memory_status")    # 内存源健康状态
engine(action="settings_get", key="some_key")
engine(action="settings_set", key="some_key", value="val")
```

执行任意代码：

```
engine(action="exec", code="""
from mem_probe.process import GameProcess
with GameProcess() as proc:
    base = proc.main_module().base
    data = proc.read_bytes(base, 16)
    _output.append(f'Base: 0x{base:X}, Header: {data.hex()}')
""")
```

---

## 八、常用示例

### 监控自身 HP 并报警

```python
def on_load(ctx):
    def check_hp(snap):
        gs = snap.get("context", {})
        pct = gs.get("hp_pct", 1.0)
        if pct < 0.3:
            ctx.toast(f"⚠ HP 过低: {pct:.0%}")
    ctx.on_snapshot(check_hp)
```

### 记录每次受到的伤害

```python
def on_load(ctx):
    def on_hit(event):
        p = event.get("payload", event)
        if not p.get("attacker_is_self") and p.get("target_is_player"):
            ctx.log(f'受击: {p["skill_name"]} → {p["damage"]}')
    ctx.on_damage(on_hit)
```

### Boss 施法提醒

```python
def on_load(ctx):
    gsm = ctx.engine.get("game_state")

    def poll():
        s = gsm.snapshot()
        if s.boss_cast_active:
            ctx.notify("Boss 施法", s.boss_cast_skill_name, duration_s=3)

    ctx.set_interval(poll, seconds=0.5)
```

### 内存读取 Boss 实时状态

```python
def on_load(ctx):
    def show_boss():
        boss = ctx.mem.boss()
        if boss["ok"] and boss["boss"]:
            b = boss["boss"]
            ctx.log(f'Boss HP: {b["hp"]}/{b["max_hp"]} 破防:{b["breaking_stage"]}')
    ctx.set_interval(show_boss, seconds=1.0)
```

### 搜索内存中的自定义数值

```python
def on_load(ctx):
    def start_search():
        job = ctx.mem.search(value=999, dtype="i32")
        if job["ok"]:
            ctx.log(f'搜索已启动: {job["job_id"]}')

            def check():
                s = ctx.mem.search_status(job["job_id"])
                if s.get("done"):
                    ctx.log(f'找到 {s["count"]} 个地址')
                    ctx.clear_timer(t)

            t = ctx.set_interval(check, seconds=0.5)
    start_search()
```

### 列出附近所有怪物

```python
def on_load(ctx):
    def list_monsters():
        r = ctx.mem.entities(include_monsters=True)
        if r["ok"]:
            for e in r["entities"]:
                if e["kind"] == "monster":
                    ctx.log(f'{e["name"]} HP:{e["cur_hp"]}/{e["max_hp"]}')
    ctx.set_interval(list_monsters, seconds=2.0)
```

---

## 九、设计约定

- **只读**：所有接口均为只读。不提供写入游戏内存的方法。
- **线程安全**：`GameStateManager` 内部有锁；`MemAccess` 方法设计为并发安全。搜索 Job 在后台守护线程运行。
- **不抛异常**：`MemAccess` 方法不会 raise——不可用时返回 `{"ok": False, "reason": "...", "hint": "..."}`。
- **JSON 安全**：所有 `MemAccess` 返回值可直接 `json.dumps()`。64 位地址为 hex 字符串，超大整数为字符串（避免 JS `Number` 精度丢失）。
- **模式门控**：`ctx.mem` 在 TCP-only 模式下返回错误 dict。hybrid/memory 模式下完整可用。
- **管理员权限**：内存扫描需要以管理员身份运行。
