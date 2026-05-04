# mem_probe → 完全替代 TCP 解析器 — 实施计划 v3

> **本计划遵守的硬约束 (来自 AGENTS.md + 2026-05-05 主人对话):**
>
> ✅ **可以新建 .py 模块** — 主人 2026-05-05 明示解除 "不要添加新py" 约束 (此前需求, 现已豁免)。仍优先扩展现有文件; 但模块化分文件 (每个 watcher 独立) 更清晰时, 直接新建。
> ❌ **不能砍帧率 / 不能砍特效** — HP/ID/BOSSHP/DPS 面板必须保 60 FPS, 扫描线等不能删。
> ❌ **不要走打包/release 工作流** — 默认仅 `git commit/push`, 不打包不上传, 除非主人明示。
> ✅ **每改完跑 `python -m py_compile <file>` + `git diff --check` 验证** — 不是 `compileall`(会被 il2cpp/bin 下的 Py2 脚本污染)。
> ✅ **重战斗压力测试** — 不接受"idle 看着 OK"; 验证必须在战斗 + 8 人队 + boss 场景下做。
> ✅ **Cython 化所有大计算** — 主人长期偏好, 已记入 memory; 全堆扫 / 紧循环不能留 Python 内层。

---

## 0. 主人本次需求 (2026-05-05) 复述

1. **完全替代 TCP 解析器** — `packet_bridge.PacketBridge` 内部数据源切换到 mem_probe; TCP 仅在 first_run 用一次抓 known_uid
2. **智能选 first_run / warm / cache** — max_hp 一局/一天/一周变几百次, 只有登录瞬间稳定; cache/warm 校验不能依赖 max_hp 数值
3. **替代后分析 进入/离开地牢、进入/离开战斗** 等高级状态, **同时满足 entity 菜单 (sao_gui.py) 和 webview 菜单 (web/menu.html) 所有数据需要**

---

## 1. 关键约束如何体现到设计

### 1.1 模块化文件结构 — 各 watcher 独立 .py

主人 2026-05-05 解除"不新增 .py"约束后, 采用清晰模块化:

| 文件 | 职责 | 状态 |
|---|---|---|
| `mem_probe/cy_memscan.py` + `_sao_cy_memscan.pyx` | Cython AVX2 扫描内核 | ✅ 已完成 |
| `mem_probe/locator.py` | SmartLocator (first/warm/cache + multi-target) | 已完成 SELF, 待加 multi-target |
| `mem_probe/tcp_source.py` | TCP anchor-only 数据源 | ✅ 已完成 |
| `mem_probe/self_watcher.py` | **新建** — SELF polling + on_self_update | Phase 2 |
| `mem_probe/scene_watcher.py` | **新建** — scene/dungeon polling + on_scene_change | Phase 3 |
| `mem_probe/entity_watcher.py` | **新建** — monster/player tracking + on_monster_update | Phase 4 |
| `mem_probe/combat_watcher.py` | **新建** — in_combat + buff event + on_boss_event | Phase 5 |
| `mem_probe/damage_watcher.py` | **新建** — damage event (路径 A) 或转发 (路径 B) | Phase 6 |
| `mem_probe/unified_source.py` | **新建** — UnifiedDataSource: 集成所有 watchers, 兼容 PacketBridge API | Phase 7 |
| `tools/mem_probe/il2cpp/mem_self_state_provider.py` | **保留**, 作为 legacy / dev CLI 测试; 不再当主集成入口 | - |

**模块化优势 vs 单文件膨胀:**
- 每个 watcher 独立调试 (单独 import, 单独 unit test)
- Phase 2~6 可并行开发 (不冲突)
- 主程序通过 `UnifiedDataSource` 单一入口对接, 内部组装

**主程序集成点仍最小化:**
- `packet_bridge.py` 加 `data_source='auto'|'tcp'|'memory'|'hybrid'` 参数 (Phase 7)
- `sao_webview.py:1815` 仅加 1 行 settings 读取
- 其他主程序文件 0 改动

### 1.2 60 FPS / 不砍特效 — polling 频率约束

mem_probe polling 不能阻塞主线程, 不能跟 fisheye/HP overlay 抢 CPU:

- **SELF poll**: 50ms (已有, 复用 `MemSelfStateProvider.POLL_INTERVAL = 0.5` 改成 0.05) — 但单次 read < 5ms, 在后台 daemon thread
- **scene poll**: 500ms (scene 切换不会更频繁)
- **entity poll**: fast-path 100ms (已知 entity HP 增量读); discovery-path 1000ms (全堆扫 monster klass)
- **combat poll**: 50ms (in_combat 必须紧)
- **每 polling 内核必须 nogil** — 让出 GIL 给主线程

### 1.3 Entity 菜单 + webview 菜单 双满足

两条菜单的数据流:

```
Entity 菜单 (sao_gui.py)
    └── 读 GameStateManager.state.* 字段 (HP/UID/skill_slots/...)

webview 菜单 (web/menu.html)
    └── SAOWebAPI 调 GameStateManager.state.to_dict() 转 JSON
        + 直接调 dps_tracker / boss_raid_engine 拿数据
```

**关键观察:** 两个菜单都通过 `GameStateManager.state` 拿数据 — **只要 GameState 字段全, 两边自动一致**。 mem_probe 替换后, 数据源换了 (TCP → memory), 但**输出到 state 的字段名/类型/语义完全不变**, 主程序两个菜单零修改自动 work。

---

## 2. 现状清点 (mem_probe 已完成 + 未完成)

### 2.1 ✅ 已完成 (Phase 0~B)

- `mem_probe/cy_memscan.py` + `_sao_cy_memscan.pyx` — Cython AVX2 扫描内核 (16 GB/s u64, 13 GB/s u32, 多 needle hash set)
- `mem_probe/locator.py: SmartLocator` — first_run / warm / cache 三段定位; v1 (UID forward) + v2 (HP reverse) 双算法; 8 workers 并行 RPM
- `mem_probe/tcp_source.py: TcpSnapshotSource` — TCP 抓 anchor; `all_seen_hp` / `all_seen_max_hp` 永不淘汰; 300ms hp_history 窗口
- `mem_probe/anchors.json` — 持久化 SELF refs + offsets (uid_off / attr_slot_off / cur_hp_off / max_hp_off / hp_width)
- 6 个 il2cpp 工具文件全部 cython 化扫描内层 (mem_dump_metadata / find_owner / forward_walk / discover_hp_field / resolver / static_resolver / self_reader)

### 2.2 ❌ 未完成 (本计划 Phase 0~10)

**待新建文件 (mem_probe/ 包内):**
- `mem_probe/self_watcher.py` (Phase 2)
- `mem_probe/scene_watcher.py` (Phase 3)
- `mem_probe/entity_watcher.py` (Phase 4)
- `mem_probe/combat_watcher.py` (Phase 5)
- `mem_probe/damage_watcher.py` (Phase 6)
- `mem_probe/unified_source.py` (Phase 7)

**待扩展现有文件:**
- `mem_probe/locator.py` — 当前只锚定 SELF, 需要扩展 multi-target (SceneManager / EntityCollection / BuffSystem)
- `tools/mem_probe/il2cpp/static_dps_source.py` — 已有 `get_self_snapshot`, 加 `get_entities_snapshot`
- `packet_bridge.py` — 当前只有 TCP 模式, 需要加 `data_source` 参数 + delegate to UnifiedDataSource
- `packet_parser.py` — Phase 9 加 `subscribed_messages` 过滤
- `sao_webview.py` — Phase 7 加 1 行 settings 读取; Phase 10 加 1 个 SAOWebAPI 方法
- `web/menu.html` + `sao_gui.py` — Phase 10 各加 1 个诊断 entry

**保留作 dev CLI 测试 (不当主集成):**
- `tools/mem_probe/il2cpp/mem_self_state_provider.py` — 历史 prototype, 可留作 reference

---

## 3. 完整数据需求矩阵 (entity + webview 双菜单)

### 3.1 GameState 字段 (来源: game_state.py:88-163)

| 字段类别 | 字段 | 当前 TCP 路径 | mem_probe 路径 (Phase) |
|---|---|---|---|
| **身份** | `player_name / player_id / level_base / level_extra / season_exp / fight_point` | SyncContainerData | CharSerialize → CharBase / RoleLevel / SeasonMedalInfo (Phase 2) |
| **HP** | `hp_current / hp_max / hp_pct` | AttrChanges 实时 | UserFightAttr.CurHp / MaxHp (Phase 2, 已 prototype) |
| **体力** | `stamina_current / stamina_max / stamina_pct` | EnergyInfo + ResourceValue | EnergyItem.* (Phase 2) |
| **职业** | `profession_id / profession_name` | SyncContainerData | ProfessionList.CurProfessionId (Phase 2) |
| **战斗** | `in_combat` | AttrCombatState (104) | UserFightAttr.attr[104] (Phase 5) |
| **技能** | `skill_slots[0..8]` (id/cd/icon) | SkillCD deltas | UserFightAttr.CdInfo[] (Phase 2) |
| **Boss** | `boss_current_hp / total_hp / shield / breaking / ...` | DamageEventNotify + BuffEventNotify | EntityCollection (Phase 4) + BuffSystem (Phase 5) |
| **Raid** | `boss_raid_active / phase / timer_text / ...` | BossRaidEngine 自维护 | 不变 (BossRaidEngine 仍消费 monster/boss event) |
| **状态** | `recognition_ok / packet_active / error_msg` | PacketBridge.is_alive | UnifiedDataSource.health() (Phase 7) |

### 3.2 5 个 callback 的 Payload schema (来源: packet_parser.py)

要全部产出兼容 dict, 不动现有消费者:

```python
# on_self_update(PlayerData)
{ name, uid, uuid, level, hp, max_hp, profession_id, profession,
  fight_point, skill_slot_map, skill_cd_map, stamina_*, energy_*,
  level_extra, season_exp,
  attr_skill_cd, attr_skill_cd_pct, attr_cd_accelerate_pct,
  temp_attr_cd_pct, temp_attr_cd_fixed, temp_attr_cd_accel }

# on_damage(DamageEvent)
{ timestamp, attacker_uid, attacker_is_self, target_uuid,
  target_is_player, target_is_monster, target_is_combat_target,
  damage, skill_id, hit_event_id, owner_level, damage_source }

# on_monster_update(MonsterData)
{ uuid, max_hp, hp, max_extinction, is_dead, profession_id }

# on_boss_event(BuffEventNotify)
{ event_type (47/58/51/88), host_uuid, buff_id, source_uid }

# on_scene_change(SceneEvent | None)
{ kind ('hard'|'soft'|''), reason ('layer_change'|'scene_restart'|'dungeon_enter'|'dungeon_leave'),
  preserve_combat, reset_on_next_damage }
```

---

## 4. 实施分阶段 (10 phases, ~10 天)

每阶段都明确: 目标 / 修改文件 / 验收 / Cython 化要求。**所有改动都是 patch 现有文件**, 不新增 .py。

---

### Phase 0 — SmartLocator 健壮性: 不依赖 max_hp 数值 (~半天)

**目标:** 主人需求 #2 — max_hp 频繁变化下 cache/warm 仍稳。

**改动 (patch 进 `mem_probe/locator.py`):**
- `_try_cached_value_anchor`: 删除 `cur_hp/max_hp` 数值校验, 仅 `*(obj+0) == klass_ptr` + UID 匹配
- `_warm_locate_value_anchor`: 删除 `last_max_hp ±20%` 校验; 改用:
  - `*(char_obj+0)` 在某模块范围内
  - `*(char_obj+uid_off) == known_uid`
  - `*(attr_obj+0)` 在某模块范围内
  - sanity range: `0 < cur_hp <= max_hp` 且 `1000 < max_hp < 1e8` (不挑值)
- 新加 `_validate_self_ref_alive(refs)` (3 RPM, < 1ms) 给后台 polling 用

**验收:**
- 同 PID cache 命中 < 50ms ✅ (已经 31ms)
- max_hp 翻倍后跨 PID warm 仍能定位 SELF
- 重战斗中后台 `_validate_self_ref_alive` 100% 通过

**Cython:** 无新需求, 复用现有内核。

---

### Phase 1 — value-anchor 自动发现子结构 offsets (~1 天)

**目标:** 让 `SelfRefs` 在无 dump.cs 时也能填 `char_base / role_level / profession_list / energy_item / season_medal_info`。

**改动 (patch 进 `mem_probe/locator.py`):**
新增 `_va_discover_substructs(char_obj, tcp_snap)` 方法 (在 first_run 末尾调用):
1. 在 char_obj +0..+0x300 扫所有 8 字节对齐 ptr 槽
2. 对每个 ptr deref 后读 0x40 字节
3. 用 TCP ground truth 反推子结构身份:
   - `level_base` → 找 deref 后含此值的 ptr → `role_level`
   - `profession_id` → 同理 → `profession_list`
   - `energy_max` → 同理 → `energy_item`
   - `fight_point` → 同理 → `char_base`
4. 持久化所有发现到 `anchors.smart_locator.anchors.self.substructs`

**验收:** value-anchor first_run 后 `SelfRefs.char_base / role_level / ...` 全非零; 字段读取与 TCP 一致。

**Cython:** 无 (子结构 ptr 数量 < 100, Python 试错即可)。

---

### Phase 2 — MemSelfWatcher: 完整 SELF 数据源 (~1 天)

**目标:** 替代 `on_self_update` callback。

**改动: 新建 `mem_probe/self_watcher.py`** (参考现有 `MemSelfStateProvider` 设计):
```python
class MemSelfWatcher:
    """SELF polling: 50ms 内核 + 80ms throttled on_self_update.

    不依赖 dump.cs (用 SmartLocator + Phase 1 自动发现的子结构 offsets)。
    后台 daemon thread, 完全 cython 化字段批量读。
    """
    POLL_INTERVAL_MS = 50
    EMIT_THROTTLE_MS = 80   # 与 TCP 路径节奏对齐
    LIFE_CHECK_INTERVAL_S = 5.0

    def __init__(self, refs: SelfRefs, locator: SmartLocator,
                 on_self_update: Callable[[dict], None],
                 on_status_change: Callable[[str, str], None]):
        ...

    def start(self) / stop(self): ...

    def _read_full(self) -> dict:
        """单次完整读: HP/stamina/level/profession/fight_point/
        skill_slots[]/skill_cd[]/resources/in_combat。
        ~10 个字段 / 1 次 batch RPM (cython 内核), < 5ms。
        返回与 packet_parser.PlayerData.to_dict() 兼容的 dict。"""
        ...

    def _validate_alive(self) -> bool:
        """3 RPM, < 1ms — 检查 SELF refs 还指向有效对象."""
        ...
```

**字段映射** (与 `packet_parser.PlayerData` 一对一):
- name, uid, uuid, level, hp, max_hp, profession_id, profession, fight_point
- skill_slot_map, skill_cd_map (含 begin_time + duration 算 progress)
- stamina_*, energy_*, level_extra, season_exp
- attr_skill_cd*, temp_attr_cd_* (从 user_fight_attr.attrs[id] 读)

**验收:** dual run 模式 (TCP 路径 + mem 路径同时跑), 输出 PlayerData dict 字段对照, drift < 5%。

**Cython:** RPM 批量读字段 — 加 `_sao_cy_memscan.pyx` 新内核 `read_struct_fields_batch(handle, base_addr, [(off, width), ...]) -> bytes` 一次 syscall 读多字段。

---

### Phase 3 — Scene / Dungeon 检测 (~半天)

**目标:** 替代 `on_scene_change` callback + 提供 dungeon enter/leave。

**改动:**
- **patch `mem_probe/locator.py`**: 加 `_locate_scene_manager()` — first_run 时扫 SceneManager klass 单例; 持久化 `anchors.scene_manager.{obj_addr, scene_id_off, dungeon_id_off, layer_off}`
- **新建 `mem_probe/scene_watcher.py: MemSceneWatcher`**:
  - 500ms 一次读 (scene_id, dungeon_id, layer)
  - 检测变化 → 推断 kind/reason → 调 `on_scene_change` callback

**事件分类:**
```python
if last_dungeon_id == 0 and dungeon_id != 0:
    {'kind': 'hard', 'reason': 'dungeon_enter'}
elif last_dungeon_id != 0 and dungeon_id == 0:
    {'kind': 'hard', 'reason': 'dungeon_leave'}
elif last_dungeon_id == dungeon_id and last_scene_id != scene_id:
    {'kind': 'soft', 'reason': 'layer_change', 'preserve_combat': True}
else:
    {'kind': 'hard', 'reason': 'scene_restart'}
```

**验收:** 主人在游戏内进入/离开地牢, 切层, 各触发对应 callback; payload 与 TCP 路径一致。

**Cython:** 无 (单字段轮询)。

---

### Phase 4 — Entity tracking (monster + nearby player) (~1.5 天)

**目标:** 替代 `on_monster_update` callback; 提供 entity 列表给 webview boss bar + Entity 菜单。

**改动:**
- **patch `mem_probe/locator.py`**: 加 `_locate_entity_collection()` — 找 EntityCollection / NearbyEntities 容器对象; 持久化 monster klass + collection 字段 offsets
- **patch `tools/mem_probe/il2cpp/static_dps_source.py`**: 加 `get_entities_snapshot() -> List[EntitySnap]` (mem_probe 内部 helper)
- **新建 `mem_probe/entity_watcher.py: MemEntityWatcher`**:
  - `_entities_known: dict[uuid, EntityState]`
  - fast-path 100ms 增量 HP 读 (cython batch RPM)
  - discovery-path 1000ms 全堆扫 monster klass (cython find_aligned_u64)
  - emit `on_monster_update` (dict schema 兼容 TCP `MonsterData.to_dict()`)
  - 加 `entity disappear` 检测 (klass scan 缺失即视为离场)

**关键性能点:**
- 100 个 entity × 5 字段 × 5ms = 2.5s 串行 — **必须 4 线程并行 + Cython batch RPM 内核**
- 目标: 100 entities 同场景下 100ms 完成增量 polling

**Cython 内核新增:**
- `_sao_cy_memscan.pyx`: `read_array_fields_batch(handle, base_addrs[], offsets[]) -> bytes`
  一次性读 N entities × M fields, 返回 packed bytes; Python 侧 unpack 解读

**验收:**
- 主城 0 entity 不报错
- 战斗场景 8 人 + 多 monster 下 polling < 100ms / 轮
- webview boss bar 与 TCP 模式一致显示
- 重战斗压力下不卡 60 FPS overlay

---

### Phase 5 — in_combat + buff event (~1 天)

**目标:** 替代 `on_boss_event` callback (break/shield/overdrive 检测) + 提供 in_combat。

**改动: 新建 `mem_probe/combat_watcher.py: MemCombatWatcher`**:
- `on_combat_change(in_combat: bool)` callback (50ms poll, attr_obj 内单 i32 读)
- `on_buff_event({event_type, host_uuid, buff_id, ...})` callback
- `_poll_buffs()` (200ms): 对每个已知 entity 读 buff_list, diff 上次 buff_id set, emit `on_boss_event`
- 依赖 `MemEntityWatcher` 提供的 `entities_known` 字典 (通过 `UnifiedDataSource` 注入)

**关键 buff event types** (与 TCP 一致):
- 47: shield_broken
- 51: super_armor_broken
- 58: enter_breaking
- 88: into_fracture_state

**验收:** 进入/离开战斗触发; boss 破盾/破防/进入硬直 buff event 触发, 与 TCP 一致。

**Cython:** 无 (buff 数量少, 不构成热点)。

---

### Phase 6 — Damage event 提供 (路径 A / 路径 B) (~1.5 天)

**目标:** 替代 `on_damage` callback。

**调研任务 (Phase 6 第一天):**
- il2cpp 找 "Combat", "Damage", "Hit", "Log", "History" 关键词类
- 看有没有 `CombatLogManager` / `DamageHistoryBuffer` / `RecentHits` ring buffer

**路径 A (内存 ring buffer 找到):**
- 100% mem 替代; TCP 完全关掉
- **新建 `mem_probe/damage_watcher.py: MemDamageWatcher`** + Cython 内核 `decode_log_entries(buf) -> List[dict]`
- 50ms poll ring buffer head/tail 解码新条目, emit `on_damage`

**路径 B (推荐, 降级):**
- **新建 `mem_probe/damage_watcher.py: MemDamageWatcher`** 但内部仅作为 TCP damage 事件的转发层
- 改 `packet_parser.py` 加 `subscribe_only=['damage']` 模式, 只解析 damage_notify
- `MemDamageWatcher` 内部启动一个 minimal `PacketBridge` (damage-only 订阅), 转发 damage event
- TCP CPU 占用 < 1% (相比当前 ~10%)
- 主程序 0 功能损失

**验收:**
- 路径 A: TCP 完全停掉, damage event 流仍完整
- 路径 B: TCP 仅订阅 damage, 其他 4 callback 全部 mem_probe

**Cython:** 路径 A 才需要; 路径 B 复用现有 `_sao_cy_packet`。

---

### Phase 7 — PacketBridge 内部多路复用 (核心集成, ~1 天)

**目标:** 主人需求 #1 — `packet_bridge.PacketBridge` 内部加 `data_source` 模式切换, 主程序零改动。

**改动 (patch `packet_bridge.py`):**
```python
class PacketBridge:
    def __init__(self, state_mgr, *,
                 data_source: str = 'auto',  # 'tcp' | 'memory' | 'hybrid' | 'auto'
                 on_damage=None, on_monster_update=None,
                 on_boss_event=None, on_scene_change=None):
        self._mode = data_source
        if data_source == 'auto':
            # 启动时尝试 mem_probe; 失败回退 TCP
            ...
        self._tcp_capture = ... if mode in ('tcp', 'hybrid', 'fallback') else None
        self._mem_provider = ... if mode in ('memory', 'hybrid') else None
        # callbacks 路由到对应 source
```

**模式语义:**
- `'tcp'`: 当前行为, TCP 全功能
- `'memory'`: 全 mem (Phase 6 路径 A 必需), TCP 完全关闭
- `'hybrid'`: TCP 仅 first_run 锚定 + Phase 6 路径 B damage; 其他 callback 全 mem (推荐默认)
- `'auto'`: 自动选 — mem_probe 锚定成功 → hybrid; 失败 → tcp

**主程序改动 (sao_webview.py: 1815):** 只加一个 settings 读取:
```python
mode = settings.get('data_source', 'auto')
self._packet_bridge = PacketBridge(
    self._state_mgr,
    data_source=mode,  # ← 新增, 其他不变
    on_damage=self._on_damage,
    on_monster_update=self._on_monster_update,
    on_boss_event=self._on_boss_event,
    on_scene_change=self._on_scene_change,
)
```

**验收:** settings.json 切换 `data_source: 'memory'`, 整个 SAO-UI 跑起来, 重战斗下 webview/Entity 菜单全部数据正常, 60 FPS 不掉。

**Cython:** 无新需求 (集成层)。

---

### Phase 8 — Multi-target SmartLocator + 持久化 schema 升级 (~1 天)

**目标:** SmartLocator 同时锚定 SELF + SceneManager + EntityCollection + BuffSystem; 跨重启 warm 全部并行重定位。

**改动 (patch `mem_probe/locator.py`):**
- `SmartLocator.locate()` 返回 `AllRefs` dataclass, 含每个 target 的 refs
- 扩展 `anchors.json schema_version: 2`:
```json
{
  "smart_locator": {
    "schema_version": 2,
    "known_uid": 36668136,
    "anchors": {
      "self": { ... },                         // Phase B 已有
      "scene_manager": { ... },                // Phase 3 添加
      "entity_collection": { ... },            // Phase 4 添加
      "buff_system": { ... },                  // Phase 5 添加
      "combat_log": null                       // Phase 6 路径 A 添加 (or null = 路径 B)
    }
  }
}
```
- schema migration: 读到 v1 → 自动重定位 all targets → 写 v2

**验收:** anchors.json 包含 4~5 个 anchor, warm 启动并行重定位每个 target ~3s 内完成。

**Cython:** 无 (集成层)。

---

### Phase 9 — TCP 降级到 anchor-only (~半天)

**目标:** TCP 不再常驻; first_run 触发时启动一次抓 known_uid 后立即停。

**改动:**
- **patch `tools/mem_probe/auto_locate.py: _TcpSource`** (现有, anchor-only 模式) — 已经是 anchor-only, 复用
- **patch `mem_probe/tcp_source.py: TcpSnapshotSource`** — 加 `mode='full'|'anchor_only'` 参数, anchor_only 时 PacketBridge 只订阅 SyncContainerData
- **patch `packet_parser.py: PacketParser`** — 加 `subscribed_messages: set` 参数, 不在订阅列表的 packet drop
- **patch `packet_bridge.py: PacketBridge`** — `mode='hybrid'` 时把 anchor_only 传下去

**验收:** `data_source='hybrid'` 模式下, TCP 解析 CPU < 1%, 网络流量不下降但 parser cycles 暴跌。

**Cython:** 无。

---

### Phase 10 — 自愈 + 监控 + 回归测试 (~1 天)

**目标:** 7×24 跑得住, 重战斗压力下不崩。

**改动:**
- **patch 各 watcher 文件** (self/scene/entity/combat/damage):
  - 任一 polling 连续 N 次失败 → 触发 `SmartLocator.locate()` 重定位
  - 重定位失败 → 通知 `UnifiedDataSource.on_status_change('error', ...)` → 自动切回 TCP 模式
- **patch `mem_probe/unified_source.py: UnifiedDataSource`**: 加 `health() -> dict` 聚合各 watcher 状态
- **patch `packet_bridge.py: PacketBridge`**: `health()` delegate 给 `UnifiedDataSource.health()`
- **patch `sao_webview.py: SAOWebAPI`**: 加 `getDataSourceHealth()` JS-bridge 给 webview 诊断面板用
- **patch `web/menu.html`**: 在「面板」类下加「数据源状态」诊断 entry (显示 mode / mem_health / tcp_health)
- **patch `sao_gui.py`**: Entity 菜单同步加诊断 entry (复用现有菜单 build 流程)

**验收:**
- 8 小时连续战斗无崩溃
- 主动 kill Star.exe → SAO-UI 30s 内恢复 (重启游戏后)
- max_hp 翻倍 (装备切换) 不触发 false positive 重定位

**Cython:** 无。

---

## 5. 实施顺序与里程碑

| 阶段 | 工期 | 累计 | 主人可观察验收 |
|---|---|---|---|
| Phase 0 | ½天 | ½ | `python -m mem_probe.locator warm` 跨 PID 仍正常 (max_hp 不挑值) |
| Phase 1 | 1天 | 1½ | warm 输出含 char_base / role_level / ... 全部子结构非零 |
| Phase 2 | 1天 | 2½ | dual-run mode: mem PlayerData 与 TCP PlayerData 字段一致 |
| Phase 3 | ½天 | 3 | 进/出地牢触发 callback, payload 正确 |
| Phase 4 | 1½天 | 4½ | webview boss bar / Entity 菜单 monster 列表 mem 模式一致 |
| Phase 5 | 1天 | 5½ | 战斗中 boss 破盾/破防触发 buff event, in_combat toggle 正确 |
| Phase 6 | 1½天 | 7 | damage event 流: 路径 A 或路径 B 任一跑通 |
| Phase 7 | 1天 | 8 | settings `data_source='memory'` 一开 SAO-UI 全功能跑通 |
| Phase 8 | 1天 | 9 | anchors.json v2 schema, 跨重启 warm 全 target 并行重定位 |
| Phase 9 | ½天 | 9½ | hybrid 模式 TCP CPU < 1% |
| Phase 10 | 1天 | 10½ | 8 小时战斗稳定 + 诊断面板 |

**总工期 ~10½ 天**。

**主人审核切入点:** 每完成 1~2 个 Phase 跑一次 e2e (主人开游戏 + 跑命令), 验证 callback 与 TCP 一致后再继续。

---

## 6. 文件改动清单 (模块化分文件)

### 6.1 新建文件 (mem_probe/ 包内)

| 新建文件 | 职责 | Phase |
|---|---|---|
| `mem_probe/self_watcher.py` | `MemSelfWatcher` — SELF polling 50ms + 80ms throttled `on_self_update` | Phase 2 |
| `mem_probe/scene_watcher.py` | `MemSceneWatcher` — scene/dungeon polling 500ms + `on_scene_change` | Phase 3 |
| `mem_probe/entity_watcher.py` | `MemEntityWatcher` — monster fast-path 100ms + discovery 1000ms + `on_monster_update` | Phase 4 |
| `mem_probe/combat_watcher.py` | `MemCombatWatcher` — in_combat 50ms + buff list 200ms + `on_boss_event` | Phase 5 |
| `mem_probe/damage_watcher.py` | `MemDamageWatcher` — combat log ring buffer 解码 (路径 A) 或转发 TCP-only (路径 B) | Phase 6 |
| `mem_probe/unified_source.py` | `UnifiedDataSource` — 集成所有 watchers, 兼容 PacketBridge API | Phase 7 |

### 6.2 扩展现有文件

| 文件 | 改动 | Phase | 风险 |
|---|---|---|---|
| `mem_probe/locator.py` | Phase 0 校验放宽; Phase 1 自动子结构发现; Phase 8 multi-target (含 SceneManager / EntityCollection / BuffSystem) | 0,1,8 | 中 — 核心定位 |
| `mem_probe/cy_memscan.py` + `_sao_cy_memscan.pyx` | Phase 2 `read_struct_fields_batch`; Phase 4 `read_array_fields_batch`; Phase 6A `decode_log_entries` | 2,4,6 | 低 — 新增内核, 不动旧 |
| `mem_probe/tcp_source.py` | Phase 9 anchor_only mode | 9 | 低 |
| `packet_bridge.py` | Phase 7 加 `data_source='auto'\|'tcp'\|'memory'\|'hybrid'` 参数, 内部委托 `UnifiedDataSource`; Phase 10 `health()` | 7,10 | **高** — 核心数据入口 |
| `packet_parser.py` | Phase 9 加 `subscribed_messages: set` 过滤 | 9 | 中 |
| `sao_webview.py` | Phase 7 加 1 行 `settings.get('data_source','auto')` 传给 PacketBridge; Phase 10 加 `SAOWebAPI.getDataSourceHealth()` JS-bridge | 7,10 | 低 |
| `web/menu.html` | Phase 10 在「面板」类下加「数据源状态」诊断 entry | 10 | 低 |
| `sao_gui.py` | Phase 10 Entity 菜单 `_build_menu_children` 同步诊断 entry | 10 | 低 |

### 6.3 不动的现有文件

- `tools/mem_probe/il2cpp/mem_self_state_provider.py` — 保留作 dev CLI 测试入口, 不再当主集成
- `tools/mem_probe/il2cpp/static_dps_source.py` — 用作 SmartLocator 内部依赖, 已经 cython 化, 不再膨胀
- `tools/mem_probe/auto_locate.py / refine.py / fingerprint.py / pointer_chain.py` — 研究 CLI, 不动
- `game_state.py / dps_tracker.py / boss_raid_engine.py / sao_gui_*.py` — 0 改动 (它们消费的是 GameState / callback, mem_probe 替换是源, 不是消费者)

---

## 7. Cython 化任务清单 (主人长期偏好, 每 Phase 必检)

| Phase | 数据量 | 内核 | 文件 |
|---|---|---|---|
| Phase 2 | SELF 字段批量 RPM 读 (~10 字段 / 50ms) | `read_struct_fields_batch` | `_sao_cy_memscan.pyx` |
| Phase 4 | 100 entities × 5 字段 / 100ms | `read_array_fields_batch` | `_sao_cy_memscan.pyx` |
| Phase 4 | 全堆 monster klass scan / 1s | 复用 `find_aligned_u64` (已有 AVX2) | - |
| Phase 5 | buff list 增量扫 (小数据) | 不需要 cy | - |
| Phase 6A | combat log ring buffer 解码 | `decode_log_entries` 或复用 `_sao_cy_packet` | `_sao_cy_memscan.pyx` |

每 Phase 启动时自检: 任何"会调几百次以上 RPM 或扫 GB 数据"的 path 必须 cython 化, 否则主人会拒。

---

## 8. 60 FPS / 重战斗压力 验收 (硬约束)

每个 Phase 完成都要在**重战斗场景**下验证 (不接受 idle 看着 OK):

| 测试场景 | 验收标准 |
|---|---|
| **8 人队 + boss 战 (5 min)** | HP/DPS/BossHP/SkillFX 面板全程 60 FPS, 无掉帧 |
| **Entity 菜单打开 + 战斗中** | 菜单刷新流畅, 子菜单切换 < 100ms |
| **webview 菜单打开 + 战斗中** | 同上, web panel 更新 < 100ms |
| **重战斗 + 鱼眼开** | 鱼眼维持 ≥ 30 FPS (符合 entity perf plan 目标) |
| **mem_probe polling CPU 占用** | 后台 daemon thread 总 CPU < 5% (vs TCP 当前 ~10%) |

**与现有 entity perf plan 协同:**
mem_probe 替代 TCP 释放主线程 ~10% CPU → 自然帮助 fisheye 30→60 FPS, dps_tracker 锁竞争缓解。这是**叠加收益**, 不冲突。

---

## 9. 风险评估

| 风险 | 概率 | 缓解 |
|---|---|---|
| `mem_probe/` 包内 6 个新 watcher 文件管理复杂度 | 低 | 命名一致 (`*_watcher.py`), `unified_source.py` 是唯一外部入口; 各 watcher 独立可测 |
| `packet_bridge.py` 加 mode 切换破坏现有 TCP 路径 | 高 | 默认 `data_source='auto'`, mem 失败自动回退 TCP; 加 unit test 覆盖每 mode |
| max_hp 频繁变化导致 polling false alert | 中 | Phase 0 校验只用 invariants, 不用数值 |
| 反作弊 polling 频率 > TCP 抓包频率 | 低 | mem_probe 只 RPM 不 hook, 风险 ≤ 现有 mem_probe; 50ms poll 远低于 mouse poll 频率 |
| Damage event 路径 A 找不到 ring buffer | 中 | 路径 B (TCP anchor + damage-only) 是稳妥退路, 主程序无功能损失 |
| 多 target SmartLocator first_run 时间过长 | 低 | 4 个 target 并行扫 (ThreadPoolExecutor), 总时间 ≈ 单个 target |
| 重战斗下 entity polling 跟不上 | 中 | Phase 4 的 4 线程 + Cython batch RPM, 100 entity / 100ms 是设计目标 |

---

## 10. 持久化 schema v2 最终目标

```jsonc
{
  "smart_locator": {
    "schema_version": 2,
    "known_uid": 36668136,
    "known_uid_set_at": 1777912114,
    "last_pid": 28908,
    "last_located_at": 1777912114,
    "last_located_via": "value_anchor_first_run",

    "anchors": {
      "self": {
        "klass_name": "Zproto.CharSerialize",
        "klass_ptr": "0x...",
        "obj_addr": "0x...",
        "uid_off": 16, "attr_slot_off": 136,
        "cur_hp_off": 24, "max_hp_off": 16, "hp_width": 8,
        "substructs": {
          "user_fight_attr":  {"obj_addr": "0x...", "klass_ptr": "0x..."},
          "char_base":        {"slot_off": 32, "klass_ptr": "0x..."},
          "role_level":       {"slot_off": 80, "klass_ptr": "0x..."},
          "profession_list":  {"slot_off": 96, "klass_ptr": "0x..."},
          "energy_item":      {"slot_off": 168,"klass_ptr": "0x..."},
          "season_medal_info":{"slot_off": 192,"klass_ptr": "0x..."}
        }
      },
      "scene_manager": {
        "klass_name": "GameClient.SceneManager",
        "klass_ptr": "0x...", "obj_addr": "0x...",
        "scene_id_off": 32, "dungeon_id_off": 40, "layer_off": 48
      },
      "entity_collection": {
        "klass_name": "GameClient.EntityCollection",
        "obj_addr": "0x...",
        "monster_list_head_off": 16, "monster_list_count_off": 24,
        "monster_klass_ptr": "0x..."
      },
      "buff_system": {
        "obj_addr": "0x...",
        "list_head_off": ...
      },
      "combat_log": null   /* path B (TCP-anchor damage) */
    },

    "diagnostics": {
      "last_max_hp_seen": 483921,
      "last_cur_hp_seen": 483921,
      "last_elapsed_s": 5.83
    }
  }
}
```

---

## 11. 主人现在能做的 next-step

人家强烈建议按 **Phase 0 → Phase 1 → Phase 2 → Phase 7** 这条路径:

1. **Phase 0 (½天)** — 修 max_hp 校验 bug, 主人需求 #2 立刻满足
2. **Phase 1 (1天)** — value-anchor 自动发现子结构, 让 SELF refs 全字段可读
3. **Phase 2 (1天)** — `MemSelfStateProvider` 升级为完整 SELF 数据源, 输出 `PlayerData` dict
4. **Phase 7 (1天)** — `PacketBridge` 加 `data_source='memory'` mode, 主程序 settings 切换试跑

跑通这 4 个 phase, 主人就能在 settings 切到 `data_source='memory'`, **关掉 TCP 抓包看 SELF 数据从内存来**。

之后 Phase 3/4/5/6 再分别接管 scene/entity/combat/damage event。

主人审一下 v2 plan 嗷呜～ ฅฅ
