# ACT 平台架构

> 当前版本：`5.0.0`。配套文档：`docs/ACT_UI_PARITY_SDK.md`、`docs/HYBRID_MEMORY_TCP.md`、`docs/PLUGIN_SDK.md`、`docs/GAME_STATE_API.md`、`docs/AI_EDITOR.md`。

SAO Auto 的 ACT 平台是一个**游戏无关的共享运行时**，统一处理实时战斗遥测、回放校验、插件、报告、触发器，以及三套 UI 表面：WebView 叠层、Entity/Tk、AI Editor（独立 pywebview IDE 窗口）。

游戏特定逻辑（DPS 面板、Boss 监控、Buff 追踪、自动按键等）全部由插件提供。平台本身零游戏特定 import。`plugins/star_resonance_plugin/` 是参考插件——新游戏适配器应当参照其 `plugin.py` on_load 结构。

任何 ACT 功能在所有 UI 表面都能访问到同一个共享后端能力之前，都不算完成。

## 运行时目标

- 以 TCP 抓包解析作为实时战斗的事实来源 (ground truth)。
- 为插件、回放、历史、触发器、UI 工具发布稳定的 ACT 事件信封。
- 通过能力对等契约保持 WebView 与 Entity 1:1。
- 让插件通过稳定的元数据声明扩展，让受信任的进程内插件在确实需要时使用文档化的引擎句柄。
- 内存探测保持保守、按需开启、只读、可观察。
- 用定向 selftest 验证 ACT 变更，而不是跑全仓库的编译。

## 核心模块

| 区域 | 模块 | 职责 |
| --- | --- | --- |
| 事件信封 | `act_platform/events.py` | 构造规范化的 ACT 事件字典，包含 `topic`、`source`、`game_id`、`parser_id`，并复制 payload。 |
| 事件总线 | `act_platform/event_bus.py` | 同步发布/订阅，自带有界的最近事件历史和回调隔离。 |
| 运行时帮助函数 | `act_platform/runtime.py` | 共享 WebView/Entity 命令辅助：插件、报告、历史、离线导入、时间线、行为日志、死亡回放、图表、钻取、数据源健康、触发器。 |
| 插件 SDK | `act_platform/plugins.py` | 发现插件目录，加载 `plugin.json`，承载 `PluginContext`，暴露插件状态，并向高权限插件提供受信的 `EngineAccess` 句柄。 |
| 解析器适配器 | `act_platform/adapters.py` | 一方解析器适配器契约和内置 `star_resonance_tcp` 包装。 |
| 回放 | `act_replay/` | 离线事件 fixture、回放 harness、平台回归 selftest。 |
| 离线导入 | `act_replay/importer.py` | 把规范化的 ACT JSON/JSONL 文件转成回放可用的事件列表。 |
| 战斗分析 | `engines/combat_analytics.py` | 由 DPS/游戏状态构造 ACT snapshot 与渲染规约。 |
| 触发器引擎 | `engines/act_trigger_engine.py` | 共享的触发器/计时器规则求值器。 |
| 历史/导出 | `engines/dps_history.py` | 滚动战斗持久化、追加式 JSONL 归档、轻量搜索，以及 JSON/CSV/HTML/XML 与压缩 XML 的导入导出。 |
| TCP 桥接 | `net/packet_bridge.py` | 拥有抓包、解析器适配器创建、包回调、数据源健康。 |
| 混合内存源 | `mem_probe/unified_source.py` | 只读的"自身状态"内存桥接，供 memory/hybrid/auto 模式使用。 |
| AI Editor | `ai_editor/` | 独立 pywebview IDE 窗口，多 Provider LLM 对话（OpenAI / Anthropic 原生 / 兼容 API），VSCode 对齐的 tool calling，MCP 服务器集成，VSCode Marketplace 扩展浏览器，dark/light 主题。详见 `docs/AI_EDITOR.md`。 |

## 数据流

实时 TCP 路径：

```text
PacketCapture
  -> PacketBridge
  -> StarResonanceParserAdapter
  -> packet_parser.PacketParser
  -> DpsTracker / EncounterManager / GameStateManager
  -> combat_analytics.build_act_snapshot(...)
  -> act_platform.runtime 共享 helper
  -> WebView 与 Entity 表面
```

ACT 事件路径：

```text
parser/replay/runtime 回调
  -> act_platform.events.make_event(...)
  -> EventBus.publish(...)
  -> 插件、行为日志、死亡回放、图表/曲线、时间线/VCR、触发器消费者
```

回放路径：

```text
act_replay fixture
  -> ActReplayHarness
  -> 同样的 tracker/encounter/event-bus 契约
  -> act_replay.selftest 和定向工具 selftest
```

## 事件信封

ACT 事件信封是普通 dict，UI、回放、插件都可以在没有 UI 依赖的情况下消费。

关键字段：

- `schema_version`：规范化的事件 schema 版本。
- `topic`：事件主题，如 `damage`、`skill`、`boss`、`act_snapshot`、`trigger_fired`。
- `observed_at`：事件时间戳。
- `source.name`：发布者名称。
- `source.kind`：来源种类，如 `packet`、`replay`、`runtime`、`plugin`。
- `source.game_id`：游戏 id，目前为 `star_resonance`。
- `source.parser_id`：解析器适配器 id，实时 TCP 解析为 `star_resonance_tcp`。
- `payload`：复制后的事件 payload。

新增生产者请使用 `act_platform.events.make_event(...)`。

### 战斗语义事实 (Combat Semantic Facts)

实时 TCP 的 skill / dungeon / monster / boss 机制事件在进入 ACT 共享消费者之前，会被 `tools.tablekit.combat_preparse` 富化。原始解析字段保持不变，派生字段挂在 `payload.combat_fact` 下，常用字段还会复制到顶层 shortcut。

当前事实族群：

- `skill`：`skill_id`、`skill_level_id`、`skill_name`、`skill_role`、`profession_id`、`sub_profession`、释放者/目标 id。
- `dungeon`：`dungeon_id`、`scene_id`、`scene_uuid`、`dungeon_difficulty`、`dungeon_name`、`flow_state`、目标数量。
- `monster`：`monster_id`、`monster_name`、HP/护盾/破韧/狂暴值字段、`mechanics` 列表、buff 数。
- `boss`：`event_type`、`boss_mechanic_key`、`boss_mechanic_label`、`trigger_family`、host/buff id。

`combat_analytics.build_act_render_spec(...)` 在 `render_spec.context` 下再暴露一组常用 shortcut：`last_skill_id`、`last_skill_name`、`last_skill_role`、`last_boss_mechanic_key`、`last_boss_mechanic_label`、`last_boss_trigger_family`。这些字段供 ACT 触发器、插件、AutoKey 门控、BossRaid 配置、WebView/Entity UI 对等使用。

## 解析器适配器

解析器适配器契约位于 `act_platform.adapters`。

内置适配器：

```text
adapter_id: star_resonance_tcp
game_id: star_resonance
source_kinds: packet
supported_locales: zh-CN
```

`PacketBridge` 现在通过 `StarResonanceParserAdapter` 创建实时的 `packet_parser.PacketParser`，同时保留 `self._parser` 以兼容已有的 player cache、monster cache、scene reset、profession skill cache restore 等方法。实时桥默认使用 `star_resonance_tcp`；高级用户可以通过设置 `act_live_parser_adapter_id` 切换到已加载的插件适配器。实时选择只接受元数据中 `source_kinds` 包含 `packet` 的适配器；缺失的适配器、不可用的插件管理器或非 packet 适配器会回退到 `star_resonance_tcp`，并通过 `PacketBridge.health().parser_adapter_selection` 暴露原因。

通过 `ctx.register_parser_adapter(...)` 注册的插件解析器适配器现在可以被 `PluginParserAdapter` 包装，并由 `PluginManager.invoke_extension("parser_adapters", ...)` 调度，提供受控的 `start`、`stop`、`parse_packet`、`parse_log_line`、`import_file`、`normalize_event` 操作。适配器元数据可以选择 `"isolation": "process"`，让调用走 `act_platform.parser_worker` 一次性子进程桥：JSON stdin/stdout、base64 帧传输、超时强杀。被选为实时 TCP 解析器时，插件 `parse_packet` 的返回会被当作规范化的 ACT 事件行（`event`、`events`、单事件 dict 或列表），并发布到共享 EventBus。通过 `ctx.register_trigger_type(...)` 注册的插件触发器处理函数同样走管理器隔离路径，具备异常隔离、耗时测量与时间预算失败计数。

## 插件扩展

插件可以在 `on_load(ctx)` 中声明这些扩展类型：

- `parser_adapters`
- `exporters`
- `formatters`
- `trigger_types`
- `report_views`
- `timers`

插件状态会暴露已注册的扩展元数据与每个插件的扩展计数。扩展处理函数保持进程内运行，按设计不出现在 JSON 状态中。

受信任的进程内插件还会通过 `PluginContext` 拿到 `ctx.engine`/`ctx.owner`。`EngineAccess` 提供文档化的句柄：当前 owner、事件总线、设置、插件管理器、DPS tracker、历史存储、触发器引擎、抓包桥、内存桥、auto-key 引擎、boss/raid 引擎以及相关运行时动作。这是有意为之的高自由度——进程内 Python 本来就能 import 项目模块，SDK 只是把这种能力变得显式且可通过 `PluginManager.status()["engine_access"]` 检视。进程隔离的解析器适配器仍然只接收 JSON payload，不会获得直接的引擎句柄。

加载/卸载行为：

- 插件之间扩展 id 重复会被拒绝。
- 卸载时移除扩展元数据。
- 加载失败会清理已注册的部分扩展元数据和事件订阅。

## UI 对等

对等契约位于 `tools.act_ui_parity`，文档见 `docs/ACT_UI_PARITY_SDK.md`。

每个 ACT 能力都包含：

- 稳定的 capability id；
- 共享后端 provider；
- 必备 actions；
- 必备 payload 字段；
- WebView 绑定；
- Entity 绑定；
- 对等校验。

当前 capability id 包括 live overview、history、export、Mini-Parse/clipboard formatters、Selective Parsing、triggers/timers、plugin manager、data-source health、action log、graph/timeseries、timeline/VCR、combatant drilldown、skill drilldown、death recap、offline import。

## Selective Parsing

Selective Parsing 是共享的运行时策略，不是 UI 层过滤。默认策略为关闭，所有战斗事件全部如常记录。开启后，`act_selective_parsing_update(owner, policy)` 会保存归一化的 `act_selective_parsing` 策略，模式包含 `self`、`party`、`include`、`exclude`，外加 include/exclude 的 id、名字、源类型、主题。

实时 WebView 与 Entity 的包回调都先发布规范化的伤害/治疗事件，保留 boss/raid 活跃性处理，再调用 `should_record_owner_combat_event(owner, event)`，最后才去更新 ACT DPS/encounter 状态。这样能在保留诊断/动作观察的前提下，阻止未选中的战斗者进入 ACT 总计。`act_selective_parsing_status(owner)` 暴露当前策略、当前 self/party id 及上一次 record/skip 决策。

## Mini-Parse 与剪贴板格式化

Mini-Parse 走和 export/history 同一份报告预览 payload 的共享 runtime helper：

- `act_mini_parse_status(owner, formatter_id="summary_table")`
- `act_mini_parse_preview(owner, formatter_id="summary_table")`
- `act_mini_parse_copy(owner, formatter_id="summary_table")`

内置 formatter id 为 `summary_table`、`chat_ranking`、`json`。插件可以通过 `ctx.register_formatter(id, metadata, handler=...)` 增加紧凑格式化器；handler 接收 Mini-Parse payload，可返回字符串或带 `text`/`value` 的 mapping。旧的 `act_report_copy(owner, fmt="json")` 仍是 JSON 报告 payload，确保已有报告/导出用户和测试不会被悄悄改动。

## 触发器与计时器规则

`ActTriggerEngine` 针对 ACT snapshot/render spec 求值共享规则。

支持的规则类型：

- `damage_total`
- `heal_total`
- `elapsed_s`
- `boss_hp_pct_below`
- `skill_kind`
- `boss_event_type`
- `encounter_start`
- `field_match`
- `timer_preset`

`field_match` 支持点路径查找，运算符包括 `exists`、`eq`、`ne`、`contains`、`regex`、`gt`、`gte`、`lt`、`lte`。

预设辅助：

- `act_trigger_export_presets(owner, rule_ids=None)`
- `act_trigger_import_presets(owner, payload, replace=False)`

## AutoKey / BossRaid 语义联动

AutoKey 与 BossRaid 共用 ACT 平台暴露的语义事实。AutoKey profile 条件支持 `dungeon_is`、`last_skill_is`、`boss_mechanic_is`、`boss_mechanic_family_is`，分别匹配 `combat_fact` 中的 dungeon、技能、boss 机制 id/名称/键。BossRaid 进一步覆盖 P3 机制、双源探测、TTS/横幅备注、`away_boss`/`away_nearest` 方向闪避、编号区域追踪、`walk_to` 与 `goto_teammate` 自动行走，准确出口由 `ZoneComp.entitiesIdInZone_` 推断。

## 快捷键系统

全局快捷键基于组合键解析：支持 `CTRL+F8`、`CTRL+F10`、`CTRL+F12` 等组合，调度感知修饰键，并附带冲突检测——重复绑定同一组合时会显式报错而非静默丢失。插件 hotkey 的占用情况由 `PluginManager` 汇总，UI 可在插件管理器面板查看。

## 报告、历史与回放

当前历史存储是滚动战斗存储，由共享 runtime helper 暴露。`DpsHistoryStore` 还会向 JSONL 归档以及附加的 SQLite 镜像追加 finalized 报告，用于更长期的留存。SQLite schema 版本 2 镜像 encounter/combatant 摘要，并在 finalized 报告包含动作/时间线/触发器/源元数据时镜像这些行。共享行为日志历史查询现在暴露分页行加上按 topic、actor、target、action 分组的轻量分析。JSON、CSV、静态 HTML、结构化 XML、压缩 SAO XML（`xml.gz` / `xml.zip`）导出都基于紧凑的报告结构。

规范化的离线导入由 `act_replay.importer` 加载，通过 `ActReplayHarness` 回放，然后归并为与实时战斗相同的 DPS 报告结构，可通过 `act_platform.runtime.act_offline_import_file(owner, path, persist=True)` 持久化。如果内置规范化导入器拒绝某文件，runtime 可以以 finalized history 的形式导入裸/压缩的 SAO 结构化 XML 报告，再回退到激活的插件解析器适配器，调用其受控的 `import_file` 操作。helper 返回导入元数据、importer/parser id、回放预览或报告预览、生成/导入的报告、持久化的历史项以及刷新后的历史状态。WebView 与 Entity 通过独立的离线导入向导以及共享的报告/导出面板暴露这些动作。

捆绑的 FastAPI repository server 还提供本地只读的 ACT API 给动态报告消费者：

- `GET /api/act/health`
- `GET /api/act/reports?limit=20&q=`
- `GET /api/act/reports/latest`
- `GET /api/act/reports/{index}`
- `GET /api/act/actions?limit=100&encounter_id=`
- `WS /ws/act/reports`

这些端点读取与 `DpsHistoryStore` 相同的文件和 SQLite 行为历史。即使服务进程绑定到 `0.0.0.0`，也默认仅 localhost 可达；只有在显式信任的网络/调试场景下才设置 `SAO_ACT_API_ALLOW_REMOTE=1`。

回放是 ACT 行为的契约关。新增功能若可在脱机条件下验证，请优先增加 fixture/selftest 路径。

## 验证

在 `sao_auto` 目录下推荐的定向验证：

```powershell
e:\Py\python.exe -m py_compile act_platform\events.py act_platform\event_bus.py act_platform\runtime.py act_platform\plugins.py act_platform\adapters.py act_platform\selective_parsing.py act_platform\mini_parse.py engines\act_trigger_engine.py net\packet_bridge.py
e:\Py\python.exe -m act_platform.selftest
e:\Py\python.exe -m act_replay.selftest
e:\Py\python.exe -m unittest tools.act_ui_parity_selftest tools.act_selective_parsing_selftest tools.act_mini_parse_selftest tools.act_trigger_runtime_selftest tools.act_parser_adapter_selftest tools.act_plugin_extensions_selftest
git diff --check
```

不要在没有显式请求的情况下运行 release 打包。涉及打包变更时请同步更新 `XiaoACTUI.spec` 的 hidden imports，使其与新的 ACT 包和 Cython scanner 模块对齐。

## 计划完成度矩阵

下表是当前可自动落地的 ACT 平台计划完成线。"完成"意味着具备共享后端 API、面向用户时具备 WebView/Entity 对等，并具备脱机回归覆盖。"阻塞"意味着需要具体的外部输入才能可靠落地。

| 区域 | 状态 | 已实现边界 | 回归门 |
| --- | --- | --- | --- |
| 共享 ACT 事件 runtime | 完成 | 规范化事件信封、有界 EventBus 历史、回放发布、共享 runtime helper。 | `python -m act_platform.selftest`、`python -m act_replay.selftest` |
| 插件 SDK / 平台 | 完成 | 插件发现、SDK 易用 API、扩展注册表、插件状态、触发器处理函数、解析器适配器、Mini-Parse formatter、插件 fallback 导入。 | `python -m unittest tools.act_plugin_ergonomics_selftest tools.act_plugin_extensions_selftest tools.act_plugin_examples_selftest tools.act_plugin_capability_selftest` |
| 解析器适配器 | 内置/插件契约完成 | 内置 TCP 适配器、插件适配器元数据、实时包适配器选择/回退、可选进程隔离。 | `python -m unittest tools.act_parser_adapter_selftest` |
| 双 UI 对等 | 契约层面完成 | WebView 与 Entity/Tk 暴露相同的 ACT capability id、共享 actions、payload 字段。 | `python -m tools.act_ui_parity`、`python -m unittest tools.act_ui_parity_selftest` |
| 报告/历史/归档 | 完成 | 滚动 JSON 历史、JSONL 归档、SQLite encounter/combatant/action/timeline/trigger/source 镜像、搜索/加载/删除、行为历史分页/分析，以及 JSON/CSV/HTML/XML/压缩 XML 导入导出。 | `python -m unittest tools.act_history_selftest tools.act_action_log_selftest tools.act_report_export_selftest tools.act_offline_import_selftest` |
| Mini-Parse / 剪贴板 | 完成 | 内置紧凑 formatter 预设、插件 formatter 扩展类型、旧 JSON 报告复制兼容、WebView/Entity 复制访问。 | `python -m unittest tools.act_mini_parse_selftest tools.act_plugin_extensions_selftest tools.act_report_export_selftest` |
| Selective Parsing | 本地策略/过滤完成 | 默认关闭的全量记录、self/party/include/exclude 过滤、共享 WebView/Entity 伤害记录门、runtime status/update/clear。 | `python -m unittest tools.act_selective_parsing_selftest tools.act_ui_parity_selftest` |
| 离线导入 | 已知 schema 完成 | 规范化 JSON/JSONL/NDJSON 回放导入、SAO 结构化 XML 与压缩 XML 报告往返、插件适配器 fallback。 | `python -m unittest tools.act_offline_import_selftest` |
| 本地动态报告访问 | 默认本地完成 | 基于同一 `DpsHistoryStore` 的只读 HTTP/WebSocket 快照/历史/动作访问。 | `python -m unittest tools.act_server_api_selftest` |
| 运行时面板 | 共享 helper 层面完成 | 时间线/VCR、图表/曲线、行为日志、战斗成员钻取、技能钻取、数据源健康、触发器/计时器、插件管理器、报告/历史、离线导入、死亡回放界面。 | `python -m unittest tools.act_timeline_selftest tools.act_graph_timeseries_selftest tools.act_death_recap_selftest tools.act_combatant_drilldown_selftest tools.act_skill_drilldown_selftest tools.act_data_source_health_selftest tools.act_trigger_runtime_selftest` |
| TCP+内存混合模式 | 策略/脚手架完成 | 保守的 scan 门、源健康上报、TCP fallback、内存/混合策略暴露。 | `python -m act_replay.selftest`；实地验证仍为手动 |
| Entity/Tk 性能护栏 | 现有面板完成 | ACT Entity 面板使用节流刷新、有界行数与 dirty signature；高频/动画视图优先 WebView/GPU；Cython 仅用于经过 profiling 的热点算术/签名路径。 | 触及模块的 `py_compile` 加可用时的运行时手动冒烟 |

硬阻塞跟进：

- 第三方 pcap/log 导入和非 SAO 的 ACT 压缩 XML 变体需要真实样本或权威 schema。
- ODBC/FTP 导出需要目标 schema、凭据和安全策略决策。
- 原生 C# ACT provider 需要具体的 provider 契约和 runtime 集成目标。
- WebView/Entity 实地冒烟需要可用的 UI/游戏 runtime 会话。

## ACT 功能覆盖审计

| ACT 风格功能簇 | 当前边界 |
| --- | --- |
| 战斗 DPS/HPS、战斗者行、历史、回放、导出、报告复制 | 通过共享 tracker/history/export/runtime helper 完成。 |
| 高级面板：行为日志、死亡回放、图表/曲线、时间线/VCR、战斗者/技能钻取 | 通过共享后端 helper 与对等注册表完成。 |
| 触发器/计时器规则 | 内置规则类型、计时预设、preset 导入导出、插件触发器处理函数完成。 |
| 多游戏 / 解析器作者 | 内置 Star Resonance 适配器、插件解析器适配器、实时选择/回退、离线导入回退、可选进程隔离完成；真正的第三方协议仍需样本/契约。 |
| Selective Parsing | 本地 ACT 记录策略完成；默认仍记录全部。 |
| Mini-Parse / 剪贴板预设 | 内置文本预设和插件 formatter 完成；旧 JSON 报告复制不变。 |
| ODBC/FTP/远程发布 | 在目标 schema、凭据处理、安全策略未提供前阻塞。 |
| 非 SAO ACT XML/pcap/log 导入 | 在真实文件或权威 schema 提供前阻塞。 |
| 原生 C# ACT provider / Logitech LCD | 没有具体 provider 契约/硬件和显式请求时不在范围内。 |

## Open Edges

- 插件 `parse_packet` 处理函数现在可以被选为实时包 EventBus 发布、离线导入回退、元数据发现以及可选的 `isolation: "process"` 进程隔离。默认进程内运行以保持低开销。
- 离线导入支持规范化 JSON/JSONL 回放、裸/压缩 SAO 结构化 XML 往返、滚动历史持久化，以及通过 HTTP/WebSocket 的本地动态报告读取；更广泛的 pcap/log 与第三方 ACT 兼容压缩 XML 变体在拿到具体样本前仍是后续工作。
- 历史拥有滚动存储的轻量搜索、追加式 JSONL 战斗归档以及附加的 SQLite 镜像（encounter/combatant/action/timeline/trigger/source 元数据）。共享行为日志面表面可在 EventBus 行与 SQLite 行为历史行之间按 source/encounter 切换、分页并返回 topic/actor/target/action 轻量分析；专用的 timeline/trigger/source 历史标签仍待将来实现。
- WebView/Entity 实地冒烟仍依赖一次 UI runtime 会话。
- 混合内存源对扫描启用、管理员要求、轮询间隔、区域扫描上限、静态回退控制有保守的策略门；实时游戏/UI 验证仍需手动。详见 `docs/HYBRID_MEMORY_TCP.md`。
