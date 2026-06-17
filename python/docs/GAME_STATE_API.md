# 游戏状态 API

> 版本 `5.0.0`。面向插件作者的游戏状态与内存扫描接口。

这不是游戏官方 API。这是 SAO ACT 通过 TCP 抓包和只读内存扫描整理出的调用层。

## 获取游戏状态

### 通过插件 SDK

```python
def on_load(ctx):
    # 方式一：订阅快照
    ctx.on_snapshot(lambda snap: print(snap))

    # 方式二：主动获取
    snap = ctx.get_snapshot()

    # 方式三：通过引擎句柄
    gsm = ctx.engine.get('game_state')
    state = gsm.snapshot()
```

### 通过 AI Editor

```
engine(action="game_state")
engine(action="entity_list")
engine(action="dps_summary")
```

## GameStateManager

由星痕共鸣插件提供（`plugins/star_resonance_plugin/engines/game_state.py`）。

### 主要方法

| 方法 | 说明 |
|------|------|
| `.snapshot()` | 返回当前状态的浅拷贝 |
| `.update(**kwargs)` | 部分更新 + 通知订阅者 |
| `.subscribe(callback)` | 注册状态变化监听 |
| `.load_cache(settings)` | 从设置恢复缓存身份 |

### 状态字段

**身份：**
`player_name`、`level_base`、`level_extra`、`player_id`、`profession_id`、`profession_name`、`fight_point`

**生命：**
`hp_current`、`hp_max`、`hp_pct`、`stamina_current`、`stamina_max`、`stamina_pct`

**技能：**
`skill_slots`（列表）、`custom_skill_slots`（列表）、`burst_ready`

**Buff：**
`self_buffs`（列表）、`server_time_offset_ms`

**战斗：**
`in_combat`（AttrCombatState 104）

**Boss：**
`boss_current_hp`、`boss_total_hp`、`boss_hp_source`、`boss_shield_active`、`boss_shield_pct`、`boss_breaking_stage`、`boss_extinction_pct`、`boss_cast_skill_id`、`boss_cast_skill_name`、`boss_cast_active`、`boss_cast_duration_ms`、`boss_stun`、`boss_invincible`、`boss_in_overdrive`

**副本：**
`dungeon_id`、`dungeon_scene_id`、`dungeon_difficulty`、`dungeon_name`

**元数据：**
`capture_timestamp`、`recognition_ok`、`packet_active`、`error_msg`

## 内存扫描接口

### MemAccess (ctx.mem)

```python
mem = ctx.mem  # MemAccess 实例（hybrid 模式下可用）

# 自身状态
self_state = mem.self_state()  # {"hp": 50000, "max_hp": 100000, ...}

# 实体列表
entities = mem.entities(include_monsters=True)

# Boss 状态
boss = mem.boss()

# 属性读取
attrs = mem.attr_map(entity_addr, [1, 100, 10000])

# 状态
status = mem.status()  # 连接/缓存/健康信息
```

所有方法在 TCP-only 模式下返回 `{"ok": False, "reason": "...", "hint": "..."}`。

所有返回值是 JSON-safe dict（地址为 hex 字符串，大整数为字符串）。

### 直接内存操作

通过 AI Editor 的 `engine(action="eval/exec")`：

```
engine(action="exec", code="""
from mem_probe.process import GameProcess
with GameProcess() as proc:
    base = proc.main_module().base
    data = proc.read_bytes(base, 16)
    _output.append(f'Base: 0x{base:X}, Header: {data.hex()}')
""")
```

### mem_probe.scanner

多帧值搜索：

```python
from mem_probe.scanner import scan, narrow

candidates = scan(process, target_value=50000, dtype='i32')
# 改变游戏内数值后...
refined = narrow(process, candidates, new_value=49500, dtype='i32')
```

### mem_probe.cy_memscan

AVX2 加速的内存扫描。Cython 编译的 `.pyd`，有纯 Python fallback：

```python
from mem_probe import cy_memscan
result = cy_memscan.find_aligned_u64_in_range(buffer, target, offset, size)
```

## 进程附加

`mem_probe.process.GameProcess`：

```python
from mem_probe.process import GameProcess

with GameProcess() as proc:
    print(f"PID: {proc.pid}, Name: {proc.name}")
    print(proc.main_module())  # ModuleInfo(name, base, size)
    for region in proc.memory_regions():
        print(f"0x{region.base:X} size={region.size}")
```

进程名由调用方或游戏插件显式传入。需要管理员权限。
