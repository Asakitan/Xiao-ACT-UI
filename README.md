# SAO Auto

`SAO Auto` 是面向《星痕共鸣》的 Windows 外部 HUD / 自动化工具。当前仓库处于 `3.0.0` 重构分支，重构目标是按职责分类整理散乱脚本、拆分巨型文件（`sao_theme` / `sao_webview` / `packet_parser` / `config` 等）为多模块，并通过更清晰的接口和数据流维持功能完整性与向后兼容。

- 当前工作分支：`3.0.0`
- 运行版本来源：`config.py` 中的 `APP_VERSION` / `APP_VERSION_LABEL`
- 当前源码版本：`4.4.17`
- 默认运行平台：Windows 10 / 11

## v4.4.17 重点：hybrid 内存补充化（干掉 mem 每 tick O(N) 读，人群/20 人本不再卡）

- **方向**（主人定）：hybrid 仍走 **TCP 为主**，mem 只补「名字 + TCP 不发送的基址」给 TCP 做补充。调查确认 DPS 行 + Boss HP 在 hybrid 下**本就 TCP 为主**（`dps_tracker._mem_primary=False`；Boss HP 仅 `boss_hp_source ∈ none/memory/estimate` 时由 mem 补）。卡的根因是 `MemStateBridge._entity_loop`（1Hz）每 tick 两次 O(N) 持 GIL 内存读——`prov.snapshot()`（读每个可见实体，随人群涨）+ `_poll_mem_damage()`（整张伤害表 + top6，随队伍涨）。
- **慢化采集**：非 memory 模式（hybrid/auto）下，`_entity_loop` 在「定位到 boss 且 `_base_acquired`」后退到 **4s**（冷启动 / boss 消失自动回 1s，不延迟 boss 定位与破防切源）；**memory 模式恒 1s 全量，零变化**（mem 仍主源）。
- **O(1) Boss 路**：`_boss_cast_loop`（~12.5Hz）本就用缓存 obj 指针调 `read_combat`（已返回 `cur/max/hp_pct/breaking/extinction`，之前只取 `cast_skill_id`）；现额外节流 ~3.5Hz 重算 `last_boss_break` + push Boss HP（自门控同慢循环，仅 TCP 未占条时写 `memory`）。Boss 血条/破防反而**更快**（12.5Hz > 1Hz）。
- **防陈旧守卫**：信任缓存 boss obj 前用 `read_u64(obj+uuid 偏移)` 复核仍是同一 uuid，防池化复用把回收对象的 HP 当成 boss（窗口从 ≤4s 收到一个快 tick ~80ms）。
- **净效果**：hybrid 每秒 mem 开销 **O(N)→O(1)**；TCP 仍是权威实时源；mem 只补名字 / 基址 / 破防湮灭 / max_hp 引导。仅改 `mem_state_bridge.py`；无进程单测 17 绿 + boss 全套 42 绿。

## v4.4.16 重点：Boss 反应编辑器（真名映射 + 聚合视图 + 开页不卡）

- **三处 ID 全映射成真名**（读时解析，走权威名表 `names.*`，历史记录一并修复）：场景 `names.dungeon`、Boss `names.boss/monster`、技能 `names.skill→boss_skill→boss_mechanic_skill→…` 级联。实测截图里的 `#500116`→「友方木人掉血buff」、`#121`→「友方木桩」。场景无副本名时回退 `场景#id`（木桩练习区本就无副本名；真副本由 TCP `dungeon_name` 落库）。
- **聚合视图**（契约 `build_boss_reactions_state` 新出 `boss_detail`）：技能/Buff（可绑反应）、机制/状态（徽章）、**时间线**（按出招时刻排序）、**Boss 单位摘要**（技能 N · 机制 M · 血线 K · 时长~Tms）。Tk（`sao_gui_bossraid`）与 WebView（`raid_editor.html`）1:1 同款渲染。
- **开页卡顿修复**：编辑器契约改用 `get_status(include_entities=False)`——不再在锁内建 O(N) 实体表；观测只取选中 Boss（非场景内全部 Boss）；250ms 轮询仅 Entities tab 才建实体表；每 tick 的 `_push_game_state_locked` 也停建实体表（后台同省）。契约多传 `boss_base_id`，切 Boss 服务端重算分组（双端 reload 对称）。

## v4.4.0 重点：Boss 技能聚合（按地图/场景/Boss 记录 + 标记特殊技能）

- **Boss 技能源 = buff 列表**：boss 每次出招会在 `BuffComp` buff 列表里新增一个瞬时 buff，`base_id` 即技能身份、`Duration` 即读条窗口。修了一个 `ZList<T>` 偏移 bug（`items_@0x18`/`size_@0x20`，`items_` 前有 `recyclePooledObj_` bool）——之前所有 buff 读成空就是它。`base_id`+时长内存权威，离线名字表只当提示。**对称 TCP 路径**：`_tcp_detect_boss_skills_locked` diff `monster.buff_list`（`BuffInfoSync`）的新 base_id；`MEM_PRIORITY_WINDOW=3s` 自仲裁——hybrid 用内存、纯 TCP 用包，不双触发。
- **持久聚合**（`engines/boss_skill_store.py`，原子写）：场景 → Boss → 观测，跨重启留存。三类观测：技能（出招）、机制（破防/护盾/霸体/碎裂/死亡/部位，来自 `on_boss_event`）、状态（狂暴/无敌**起手**，带 HP%/用时）。按并发状态打标签，血线/定时由离散度自动推导。按当前场景（`GameState` dungeon_scene_id/dungeon_id/dungeon_name）归档。
- **场景化反应编辑器**（契约 `build_boss_reactions_state` 加 `scene_key`）：场景选择器 → 该场景的 Boss → 观测技能/机制，带彩色类型徽章（施法/狂暴/无敌/护盾/霸体/破防/碎裂/眩晕/血线/定时/死亡/部位）。Tk（`sao_gui_bossraid`）与 WebView（`raid_editor.html`）1:1；技能行内联反应编辑，机制/状态作为标记信息行。

## v4.3.0 重点：内存驱动 Boss 反应（自动躲技能 / 自动操作）

- **内存 Boss 动作 feed**（`mem_probe/il2cpp/mem_boss_action_reader.py`）：`BossActionTracker` 每 tick 边沿检测任意 boss/怪的 `cast_skill_id`（attr 100，开始施法 0→技能id）+ 破防/过载/眩晕/血量；`mem_state_bridge` 加 10–20Hz boss 专用快轮询，施法开始到事件 <100ms。施法时长尽力深挖：`BossDurationProbe` 读 `BuffComp.BuffItem.Duration`（偏移对照 dump fdc7111b 已验证），失败回退「学习」EMA。只读，经 `MemAccess.boss_actions()` 暴露。
- **bossraid 引擎接入**：`on_mem_boss_action` 更新施法态、自动发现每个 boss 放过的技能、用内存 skill_id 触发现有 `boss_skill` phase 触发、补上缺失的 `on_self_dead_change` 死亡门控。
- **BossAutoKeyLinkage 从「警报文字」升级为「技能 id 精确」**：新增触发类型 `boss_cast`/`boss_breaking`/`boss_overdrive`/`boss_stun` + `on_boss_action` 入口，按 `skill_id`/`boss_base_id` 匹配，支持 `delay_ms`/`lead_ms`/`sequence`（自动躲技能 + 进攻连招）。完全向后兼容旧映射。
- **新增「Boss 反应」可视化编辑器**（BossRaid 面板 Tk tab + WebView `raid_editor.html` tab，契约 `build_boss_reactions_state`）：选 boss → 看内存自动发现的它放过的技能 → 给每个技能/进攻窗口指派反应按键与时机。纯 TCP 模式显示「切 hybrid」横幅。

## v4.2.0 重点：Mem Scope 内存浏览器 + 插件内存 API

- **MemAccess facade**（`mem_probe/mem_access.py`，全只读、hybrid 门控、JSON 安全）：把 mem_scan 全部可读资源（self / entities / boss / damage / 逐技能伤害 / 按地址 attr_map / 名字解析 / 任意地址读）做成稳定接口，外加异步按值搜索 job 管理器（每个命中带多路解码提示：u32/i32/f32/utf16/cstr/指针 + 所在模块+偏移 + 疑似 klass）。地址以十六进制串、uuid 以字符串返回；绝不写内存。
- **插件 SDK**：新增 `ctx.mem` 门面 + `act_mem_*` 运行时动作（`ctx.call_runtime("mem_*")`）；修复 `ctx.get_engine("memory_bridge")` 在 hybrid 模式取不到真正活引擎（嵌套于 `_packet_engine._mem_source`）的问题。
- **声明式 UI 新增 `input` 文本框叶子**（Tk + WebView 1:1），按钮触发时把同面板输入值回传到 `payload["inputs"]`，并在定时重绘中保持焦点/已输入文本；任意插件面板可复用。
- **新增一等公民 Mem Scope 面板**（Entity `sao_gui_mem_scope.py` + WebView `mem_scope.html`，契约 `act_mem_scope_status`）：浏览资源目录（每条带提示）、查看实时 self/entities/damage、手动按值搜索 + 多帧 narrow 收敛。并新增 `mem_scope` 渲染面供插件 hook/overlay。

## v4.0.0 大版本重点

`4.0.0` 把 3.x 阶段的 ACT 工作整理为一条独立的「ACT 平台」能力线，并让名称表流水线彻底脱离外部邻居仓库、完全自包含。相对 `3.2.22` 的主要变化：

- **ACT 平台**：新增 `act_platform/` 包（EventBus、解析适配器、进程隔离 parser worker、mini-parse、选择性解析、可信插件引擎与示例插件）。新增 ACT XML 报告导入 / 导出与压缩 roundtrip、SQLite + JSONL 历史归档、本地 report API、行为日志分页分析、离线导入向导、死亡回放（entity + WebView）、live parser 适配器选择与健康面板，以及 1:1 双 UI 聚合 cockpit 重设计。
- **ACT 覆盖率**：幻想技能（ultimate）buff 覆盖率 % + 触发次数、self buff/debuff 持续时间追踪、按目标 / 元素 / min-max 拆分并附中文名、encounter 驱动的聚合窗口与 hybrid memory 策略闸门。
- **名称表自包含**：classifier 与全量重建不再依赖邻居仓库；新增 20+ 个语义 `assets/name_tables/*.json`、smart 解析、taxonomy/kind 拆分、fallback 路由、tcp name cache + 延迟 mem 启动，运行期表以本机内存解析为权威、覆盖陈旧邻居数据。
- **GPU / UI**：LinkStart GPU 开场动画打磨与连续相位 / 相机过渡、entity 面板强制 GPU、场景切换地图名横幅居中、避免把编辑器窗口误判为游戏、packet mem 启动稳定化。

## 当前 3.x 重构线重点

- `packet_parser/` 已从旧单文件解析器拆成 `enums.py`、`skills.py`、`helpers.py`、`data.py`、`parser.py`，并通过 `packet_parser/__init__.py` 维持 `from packet_parser import PacketParser` 等旧导入兼容。
- `net/` 承载抓包与 `PacketBridge`，负责把 TCP/packet 事件推送到状态模型、DPS、Boss HP、Boss Raid 与 ACT 上下文。
- `engines/` 承载状态、识别结果聚合、DPS/HPS、Boss Raid、AutoKey、ACT 触发、Hide & Seek 等核心逻辑。
- `gui_modules/` 承载 Entity/Tk HUD 的拆分模块、菜单、浮窗、DPS、Boss HP、HP、Commander、BuffMon、热键与生命周期 mixin。
- `render/` 与 `ui_gpu/` 承载 GPU overlay、渲染 worker、捕获同步、SkillFX pipeline、菜单/弹窗 GPU 布局与 compositor。
- `vision/` 承载窗口定位、截图、STA/技能识别与 Cython 加速入口。
- `act_replay/` 提供离线 ACT/TCP 合同自检，便于不进游戏也能验证 parser → encounter → render_spec 的关键链路。
- `mem_probe/` 与 `tools/tablekit/` 是只读内存/名称表/IL2CPP 研究工具链；它们用于定位和验证数据，不应默认进入发布包。

## 功能列表

- Windows 透明 SAO 风格 HUD。
- `webview` 与 `entity` 两套 UI 模式，`entity` 是 Tk/原生窗口路径，`webview` 是 pywebview + WebView2 路径。
- `HP / 等级 / 身份 / 技能状态` 默认通过 packet/TCP 同步。
- `STA` 默认通过 vision 识图识别，避免和 packet 状态互相抢写。
- 独立 `Boss HP` 覆盖条、Boss break / extinction / revive hold 等状态处理。
- 独立 `DPS / HPS` 面板、排行榜、单人明细、技能拆分、上次战斗报告与历史报告。
- `Burst Ready`、SkillFX、菜单粒子、扫描线、浮动面板等 SAO 风格视觉效果。
- `Boss Raid` 阶段计时、时间轴提醒、狂暴倒计时与 Boss ↔ AutoKey 联动。
- `ACT Triggers/Timers` 双 UI 管理面板，支持从 `act_trigger_rules` 查看、启用/禁用、重载与合成测试触发器。
- `ACT Data Source Health` 双 UI 健康面板，展示 TCP / hybrid / memory fallback 的运行状态、延迟、最近事件间隔、诊断与可复制 JSON 快照。
- `ACT Report/Export` 双 UI 报告导出面板，复用 `DpsHistoryStore` 输出 JSON/CSV，并提供报告预览、历史摘要与复制载荷。
- `ACT History Browser` 双 UI 历史浏览能力，支持搜索/过滤、载入历史报告、删除单条记录与清空历史，并通过共享 runtime 保持 WebView 与 Entity/Tk 行为一致。
- `ACT Timeline/VCR` 双 UI 时间线面板，基于共享 ACT EventBus 展示 encounter 事件流，并提供播放、暂停、步进、seek、倍速和过滤控制。
- `ACT Action Log` 双 UI 行为日志，基于共享 EventBus 表格行支持搜索、topic 过滤、跳转到时间点和复制 JSON 载荷。
- `ACT Graph/Timeseries` 双 UI 图表/曲线面板，基于共享 EventBus 生成 damage/heal/event/boss HP 时间序列，支持 metric 选择、topic/query 过滤、时间范围缩放和 JSON 导出。
- `ACT Combatant Drilldown` 双 UI 战斗成员钻取面板，复用 DPS tracker 的实体明细与技能拆分，支持打开、返回、技能过滤和目标聚焦。
- `ACT Skill Drilldown` 双 UI 技能钻取面板，复用共享技能明细和 EventBus timeline refs，支持打开、返回、过滤和复制 JSON 载荷。
- 自动按键 profile、本地导入导出、脚本仓库上传/下载、结构化触发条件。
- `Commander` 队伍面板、BuffMon、Hide & Seek、置顶/隐藏面板等辅助功能。
- 远程更新检查、`runtime-delta` / `full-package` 下载、独立 `update.exe` 应用更新并重启主程序。

## 运行要求

- Windows 10 / 11。
- Python 3.11 与 `pip`。
- Microsoft Edge WebView2 Runtime。
- 抓包模式建议安装 Npcap。
- `requirements.txt` 中的主线依赖包括 `pywebview`、`numpy`、`Pillow`、`mss`、`windows-capture`、`pythonnet`、`moderngl`、`glfw`、`skia-python`、`Cython`、`opencv-python`、`pynput`、`pygame`、`zstandard`、`protobuf` 等。

> 注意：`Cython` 是构建期必需依赖，仓库内的 `_sao_cy_*.pyx` / `_sao_cy_*.pyd` 用于加速 packet、memscan、像素、战斗与 UI helper 热路径。

## 快速开始

在 `sao_auto` 目录下执行：

```powershell
pip install -r requirements.txt
python main.py
```

等价批处理入口：

```powershell
.\Start.bat
```

常用模式：

```powershell
python main.py             # 根据 settings.json 的 ui_mode 启动 HUD
python main.py --test      # 单次窗口定位 + 截图识别测试
python main.py --headless  # 无 HUD，终端输出识别状态
```

`main.py` 会先设置 DPI 感知、提升进程优先级、注册退出时更新应用 hook，然后按 `settings.json` 的 `ui_mode` 选择 UI：

- `webview`：默认 WebView HUD 模式。
- `entity`：Tk / Entity HUD 模式。
- `sao`：历史别名，启动时会映射为 `entity`。

如果首选 UI 启动失败，程序会尝试回退到另一套 UI，最后再回退到 headless。

## 默认数据源与热键

当前 `settings.json` 默认数据源为混合模式：

| 数据 | 默认来源 |
| --- | --- |
| `hp` | `packet` |
| `level` | `packet` |
| `identity` | `packet` |
| `skills` | `packet` |
| `stamina` | `vision` |

默认热键来自 `config.DEFAULT_HOTKEYS`，可在 `settings.json` 覆盖：

| 热键 | 功能 |
| --- | --- |
| `F5` | 开始 / 停止识别 |
| `F6` | 开始 / 停止自动按键 |
| `F7` | 开始 / 停止 Boss Raid |
| `F8` | Boss Raid 下一阶段 |
| `F9` | 切换置顶 |
| `F10` | 隐藏 / 恢复面板 |
| `F11` | 切换 Hide & Seek |

默认游戏窗口匹配：窗口标题包含 `Star` / `星痕共鸣`，进程名为 `star.exe`。

## 目录概览

```text
sao_auto/
├─ main.py                         # 程序入口与 CLI 参数
├─ config.py                       # 版本、路径、默认热键、默认数据源、SettingsManager
├─ settings.json                   # 本地运行配置与 UI 状态缓存
├─ sao_gui.py                      # Entity/Tk HUD 聚合入口，核心实现已拆到 gui_modules/
├─ sao_webview.py                  # WebView HUD 主入口
├─ act_platform/                   # ACT 事件总线 / 解析适配器 / 插件引擎 / 选择性解析
├─ act_replay/                     # 离线 ACT/TCP replay 合同自检
├─ engines/                        # GameState、DPS、Boss Raid、AutoKey、ACT、Hide & Seek
├─ gui_modules/                    # Entity HUD 拆分模块、面板、mixin、热键、弹窗
├─ net/                            # packet_capture / packet_bridge
├─ packet_parser/                  # 拆分后的 TCP 协议解析器包
├─ render/                         # GPU overlay、render worker、capture sync、SkillFX pipeline
├─ ui_gpu/                         # GPU 菜单 / 弹窗 / HUD layout 与 compositor
├─ vision/                         # 截图识别、STA/技能识别、vision 加速
├─ mem_probe/                      # 只读内存探针与 IL2CPP 研究工具
├─ tools/                          # tablekit、基准、smoke test、诊断工具
├─ web/                            # WebView HTML / CSS / JS 资源
├─ assets/                         # 字体、音效、技能名表、名称表等资源
├─ proto/                          # protobuf 文件与生成模块
├─ server/                         # AutoKey / BossRaid 脚本仓库 FastAPI 服务
├─ update_host/                    # 远程更新 FastAPI 服务与发布工具
├─ updater/                        # 客户端更新检查、下载、应用调度
├─ update_apply.py                 # 外部更新应用器
├─ build_cython_ext.py             # Cython 扩展构建脚本
├─ build_delta.py                  # runtime-delta 更新包构建脚本
├─ build_full_package.py           # full-package 更新包构建脚本
├─ build_release.bat               # 客户端 + updater + 脚本服务端发布目录构建
└─ build_update_host_package.bat   # 更新服务端部署包构建
```

## ACT / TCP 合同自检

离线 ACT replay 自检用于验证 parser 归一化事件、dungeon/scene 元信息、skill lifecycle、Boss HP render spec、DPS snapshot、Entity/WebView packet 回调刷新等合同。

ACT 快照以 TCP 为主数据源，`settings.json` 的 `mem_data_source` 仅用于只读 memory/hybrid 补齐缺失的自身状态字段。双 UI 共享 `render_spec`：`render_spec.rows` 驱动 WebView 与 Entity/Tk 的 live DPS 列表，`sources.summary.data_source` 驱动 ACT 来源 badge，`triggers.emitted` / `triggers.recent` 会在 live 摘要行显示最近一条 ACT alert。

可选 ACT 触发规则通过 `settings.json` 的 `act_trigger_rules` 配置。未配置时不会产生 ACT alert；配置后 WebView 与 Entity/Tk 启动时都会创建同一套 `ActTriggerEngine` 并在每次 live snapshot 中评估。

示例：

```json
{
	"act_trigger_rules": [
		{"id": "boss-low-hp", "type": "boss_hp_pct_below", "threshold": 35, "message": "Boss HP below 35%"},
		{"id": "boss-event-101", "type": "boss_event_type", "event_type": 101, "message": "Boss event 101"},
		{"id": "party-damage", "type": "damage_total", "threshold": 1000000, "message": "Party damage 1M"}
	]
}
```

在 `sao_auto` 目录运行：

```powershell
python -m act_replay.selftest
```

需要复放单个 JSONL 事件 fixture 时，可以运行：

```powershell
python -m act_replay.run_fixture demo_events
python -m act_replay.run_fixture act_replay\fixtures\demo_events.jsonl --rules .\my-act-rules.json
```

`run_fixture` 只消费归一化后的 ACT replay 事件，不启动 Npcap、WebView 或 Entity 窗口；适合把真实 TCP 回调导出的事件落成 JSONL 后快速验证 `render_spec`、DPS rows、Boss HP 与 trigger 输出。

这个命令是 3.x TCP-first ACT 栈的首选轻量验证。不要用仓库级 `compileall` 当唯一验证门，因为历史工具目录中可能存在不适合 Python 3.11 直接编译的旧脚本。

常用开发校验：

```powershell
python -m py_compile main.py config.py sao_gui.py sao_webview.py
python -m act_replay.selftest
git diff --check
```

## 自动按键与 Boss Raid

自动按键支持：

- 本地 profile 管理。
- 动作顺序、按键、次数、延迟、重触发间隔。
- `Boss Raid` 事件联动触发。
- 结构化条件判断。
- JSON 导入 / 导出。
- 云端搜索、下载、上传脚本。

常用条件包括：

- `hp_pct_gte` / `hp_pct_lte`
- `sta_pct_gte`
- `burst_ready_is`
- `slot_state_is`
- `profession_is`
- `player_name_is`

`Boss Raid` 支持 profile 新建、保存、删除、导入导出、云端搜索/下载/上传、阶段推进、时间轴提醒、狂暴倒计时、Boss 条显示模式切换，并可通过 Boss 事件触发 AutoKey。

## 自建服务端

仓库内包含两个互相独立的 FastAPI 服务端。

### 脚本仓库服务端

用于自动按键脚本与 Boss Raid 配置仓库，默认端口为 `9983`。

```powershell
cd sao_auto
python server/app.py
```

或显式使用 uvicorn：

```powershell
cd sao_auto
python -m uvicorn server.app:app --host 0.0.0.0 --port 9983
```

主要端点：

- `GET /health`
- `GET /api/scripts`
- `GET /api/scripts/{script_id}`
- `GET /api/scripts/{script_id}/export`
- `POST /api/upload-token/issue`
- `POST /api/scripts`
- `GET /api/boss-raids`
- `GET /api/boss-raids/{raid_id}`
- `POST /api/boss-raids`

本地数据库默认位于 `server/data/scripts.db`。上传签名优先读取 `SAO_UPLOAD_SECRET`，否则会在 `server/data/upload_secret.txt` 生成本地 secret。

### 更新服务端

用于客户端远程更新，默认端口为 `9973`。客户端默认访问 `config.DEFAULT_UPDATE_HOST`，可通过 `settings.json` 的 `update_host` 覆盖。

```powershell
cd sao_auto/update_host
pip install fastapi uvicorn
$env:UPDATE_HOST_RELEASE_DIR = "$pwd\releases"
python -m uvicorn app:app --host 0.0.0.0 --port 9973
```

主要端点：

- `GET /api/health`
- `GET /api/update/latest`
- `GET /api/update/summary`
- `GET /api/update/anchor`
- `POST /api/update/anchor`
- `POST /api/update/publish`
- `GET /downloads/<channel>/<target>/<file>`

更多发布细节见 `update_host/README.md`。

## 打包与更新发布

客户端发布目录构建：

```powershell
.\build_release.bat
```

该脚本会：

1. 构建 Cython 加速扩展。
2. 通过 `XiaoACTUI.spec` 构建 `XiaoACTUI.exe`。
3. 通过 `update.spec` 构建独立 `update.exe`。
4. 通过 `server/AutoKeyServer.spec` 构建 `AutoKeyServer.exe`。
5. 生成 `dist/release/XiaoACTUI` 与 `dist/release/AutoKeyServer`。
6. 将 `web/`、`assets/`、`proto/`、`icon.ico` 从 `runtime/` 提升到客户端顶层，便于增量更新。

单独 PyInstaller 命令：

```powershell
pyinstaller --clean --noconfirm XiaoACTUI.spec
pyinstaller --clean --noconfirm update.spec
pyinstaller --clean --noconfirm server/AutoKeyServer.spec
pyinstaller --clean --noconfirm update_host/UpdateHost.spec
```

构建 `runtime-delta`：

```powershell
python build_delta.py --version 4.0.0 --files runtime/sao_gui.py web/menu.html assets/sounds/click.wav
```

构建 `full-package`：

```powershell
python build_full_package.py --version 4.0.0
```

开发发布助手会默认读取 `config.APP_VERSION`，需要发布到更新服务端时再显式使用上传参数：

```powershell
python dev_publish.py --dry-run
python dev_publish.py --upload
```

> 发布/打包会触发 Cython、PyInstaller 或远端上传，普通代码修复不应自动运行这些命令。

## 远程更新机制

- 客户端启动后会后台检查 `settings.json` 中的 `update_host`；为空时回退到 `config.DEFAULT_UPDATE_HOST`。
- `runtime-delta` 的 zip 内路径与客户端 `BASE_DIR` 对齐，例如 `runtime/sao_gui.py`、`web/menu.html`、`assets/sounds/x.wav`。
- `runtime-delta` 不应覆盖启动器 exe 本身；改到 `XiaoACTUI.exe`、Python runtime、DLL 或启动 spec 时使用 `full-package`。
- `full-package` 包含 `XiaoACTUI.exe`、`update.exe`、`runtime/`、`web/`、`assets/`、`proto/` 等完整客户端目录。
- `update.exe` 在主程序退出后应用更新；`staging/`、`backup/`、`update_state.json`、`update_apply.log` 都是本地运行产物。

## 开发注意事项

- 3.x 重构允许新增模块，但新增模块应归入对应子包；需要保留旧顶层导入时，在 `__init__.py` 或旧入口文件中 re-export。
- `gui_modules/` 已由 `XiaoACTUI.spec` 的 `collect_submodules` 自动收集；拆分动态导入模块时仍要检查 `.spec` hiddenimports。
- packet / ACT / Boss HP 改动优先补 `act_replay.selftest` 或针对性 smoke test。
- 性能相关改动要关注重战斗路径，不要只看 idle 状态。
- 不建议继续尝试内存注入或内存修改方案；`mem_probe/` 与 `tools/tablekit/` 默认只读诊断。
- GitHub 推送目标为 `origin`，不是更新服务端发布；远程发布需要另外执行 `dev_publish.py --upload` 或 update_host 发布工具。

## 历史更新记录

早期 `2.0.x` / `2.1.x` 记录主要覆盖双 UI、远程更新、Entity 面板 parity、SAO Menu 60Hz 调度、packet/vision 闸门拆分、updater 版本比较修复等历史工作。当前 README 以 3.x 重构线和当前源码结构为准；需要追溯旧变更时，请查看 Git 历史、`AGENTS.md` 的仓库记忆和 `rollout_summaries/`。

## 致谢

项目开发过程中直接受益于以下开源仓库和公开资料：

- `StarResonanceDps`
- `StarResonanceDamageCounter`

若某些第三方代码、资源或生成产物来自上游项目，则仍应遵循其原始许可证。

## License

- `sao_auto` 目录下未特别注明的原创代码：MIT。
- 第三方项目、第三方资源、第三方生成产物：以各自原始许可证为准。
