# ACT UI 对等 SDK

> 当前版本：`5.0.0`。本文档配合 `docs/ACT_PLATFORM.md` 与 `docs/PLUGIN_SDK.md` 阅读。

SAO Auto 提供一个小型 Python SDK，用于让 ACT 的 WebView 与 Entity UI 表面保持 1:1 对等。每当 ACT 功能新增或修改路由、动作、payload 字段或渲染行为时都应使用这套契约。

任何只在一个 UI 表面发布的 ACT 能力都不算完成。

## 模块

SDK 位于 `tools.act_ui_parity`。

```python
from tools.act_ui_parity import (
    ENTITY_UI,
    WEBVIEW_UI,
    CapabilityBinding,
    UiAdapterContract,
    assert_ui_parity,
    build_capability_registry,
    validate_ui_parity,
)
```

## 能力注册表

`build_capability_registry()` 返回 ACT 能力矩阵的标准版本，是两套 UI 的事实来源。

每个 `CapabilitySpec` 定义：

- `capability_id`：稳定的 capability id。
- `title`：诊断用的显示名。
- `required_actions`：每套 UI 必须暴露的动作。
- `required_payload_fields`：每套 UI 必须提供的共享 payload 字段。
- `webview_render_hint`：期望的 WebView 呈现形式。
- `entity_render_hint`：期望的 Entity 呈现形式。

已知 capability ID：

- `live_overview`
- `combatant_drilldown`
- `skill_drilldown`
- `action_log`
- `graph_timeseries`
- `encounter_timeline_vcr`
- `history_browser`
- `export`
- `mini_parse`
- `selective_parsing`
- `triggers_timers`
- `plugin_manager`
- `data_source_health`
- `death_recap`
- `offline_import`

## 适配器契约

每套 UI 用 `UiAdapterContract` 与 `CapabilityBinding` 声明它的 ACT 表面。

```python
webview = UiAdapterContract(
    ui_kind=WEBVIEW_UI,
    bindings={
        "plugin_manager": CapabilityBinding(
            capability_id="plugin_manager",
            actions=("list_plugins", "enable", "disable", "reload", "status"),
            payload_fields=("plugins", "enabled", "status", "version", "errors"),
            render_hint="plugin manager page",
            route="/act/plugin_manager",
        ),
    },
)

entity = UiAdapterContract(
    ui_kind=ENTITY_UI,
    bindings={
        "plugin_manager": CapabilityBinding(
            capability_id="plugin_manager",
            actions=("list_plugins", "enable", "disable", "reload", "status"),
            payload_fields=("plugins", "enabled", "status", "version", "errors"),
            render_hint="plugin menu/detail panel with identical operations",
            route="entity://act/plugin_manager",
        ),
    },
)
```

`route` 与 `render_hint` 可以因 UI 而异；`actions` 与共享的 `payload_fields` 必须保持等价。

## 默认契约

可使用 `build_default_contracts()` 获取当前脚手架的 WebView 与 Entity 声明。

```python
from tools.act_ui_parity import build_default_contracts

webview, entity = build_default_contracts()
assert_ui_parity(webview, entity)
```

默认契约适合用于测试和冒烟检查。真实的 UI 适配器最终应从实际的 WebView 与 Entity 路由/动作注册中声明绑定。

## 校验

需要拿到结构化 issue 时使用 `validate_ui_parity(webview, entity)`。

```python
issues = validate_ui_parity(webview, entity)
for issue in issues:
    print(issue.code, issue.ui_kind, issue.capability_id, issue.message)
```

在测试或功能门里使用 `assert_ui_parity(webview, entity)`。任何一侧漂移都会抛出 `AssertionError`。

校验会捕捉：

- 缺失或未知的 capability id；
- 缺失的必备 action；
- 缺失的必备 payload 字段；
- 缺失的 route 或 render hint；
- WebView/Entity 动作漂移；
- WebView/Entity payload 漂移；
- UI kind 声明错误。

## Selftest 报告

直接运行模块即可输出机器可读的对等报告。

```powershell
python -m tools.act_ui_parity
```

报告包括：

- `ok`：对等门是否通过。
- `summary`：能力与 issue 计数。
- `issues`：序列化的对等失败。
- `registry`：标准 capability spec。
- `adapters`：WebView 与 Entity 绑定。

## 回归测试

在 `sao_auto` 目录下推荐的验证：

```powershell
python -m py_compile tools\act_ui_parity.py tools\act_ui_parity_selftest.py act_replay\selftest.py
python -m tools.act_ui_parity
python -m unittest tools.act_ui_parity_selftest
python -m act_replay.selftest
```

`act_replay.selftest` 已包含 ACT UI 对等契约与 ACT 插件平台契约。

## 增加或修改一个 ACT 能力

1. 在 `build_capability_registry()` 中新增或更新 `CapabilitySpec`。
2. 同步增加对等的 WebView 与 Entity 绑定。
3. 让 action 名称与共享 payload 字段在两套表面保持一致。
4. 让 route 与 render hint 描述各自 UI 的呈现形式。
5. 在 `tools.act_ui_parity_selftest` 中新增或更新回归覆盖。
6. 运行上述校验命令。

## UI 规则

- Entity 不是 WebView 的轻量子集。
- WebView 可以使用富图表、页面或弹窗。
- Entity 可以使用紧凑面板、表格或菜单。
- 两套 UI 必须保留相同的 capability id、动作集、共享 payload 字段与验收门。
- plugin manager、history、export、trigger/timer、data-source health、offline import 全部纳入对等门。
- Web 菜单与 WebView 桥的所有 ACT 入口（profile/upload/export/cloud/file picker/leaderboard/plugin detached/updater/exit/cloud server/common API helper 等）必须补齐 ack/rollback/异常显示路径，避免裸 `.then`、未捕获 sync throw 与 mojibake，让 WebView 表面与 Entity 表现一致。
- Entity/Tk 面板统一采用 350ms 刷新节流、80 行有界、dirty signature gating；高频/动画视图应迁向 WebView/GPU 路径，避免给 Tk 重绘加压；Cython 仅用于经过 profiling 的热点。

## 当前 trigger / timer 管理表面

`triggers_timers` 通过共享 `act_platform.runtime` helper 接入，WebView 与 Entity 调用同一份规则后端：

- status：`act_trigger_status(owner)`
- enable：`act_trigger_enable(owner, rule_id)`
- disable：`act_trigger_disable(owner, rule_id)`
- reload：`act_trigger_reload(owner)`
- test：`act_trigger_test(owner, rule_id)`
- 导出 preset：`act_trigger_export_presets(owner, rule_ids=None)`
- 导入 preset：`act_trigger_import_presets(owner, payload, replace=False)`

WebView 路由：`web/trigger_timer_manager.html`，从 `SAO Menu > ACT 触发/计时 Trigger Timer` 打开。

Entity 路由：`entity://act/triggers_timers`，从 `SAO 菜单 > 面板 > ACT触发/计时` 打开。

当前面板从 `settings.json` 的 `act_trigger_rules` 键读取规则，支持 enable/disable/reload/test 与共享 preset 导入导出。规则求值除了原有的 summary 规则，还支持基于 ACT snapshot/render 字段点路径的 `field_match` 与 `timer_preset` 倒计时规则。

## 当前 data-source health 表面

`data_source_health` 通过共享 `act_platform.runtime` helper 接入，WebView 与 Entity/Tk 检视的是同一份 PacketBridge + 内存回退快照：

- status：`act_data_source_health(owner)`
- refresh：`act_data_source_health(owner)`
- diagnose：`act_data_source_diagnose(owner)`
- copy：复制 JSON 健康/诊断 payload

WebView 路由：`web/data_source_health.html`，从 `SAO Menu > ACT 数据源健康 Data Source Health` 打开。

Entity 路由：`entity://act/data_source_health`，从 `SAO 菜单 > 面板 > ACT数据源健康` 打开。

共享 payload 暴露 `sources`、`status`、`latency_ms`、`last_event_ms`、`errors`。当 memory/hybrid 模式启用时，`sources.memory.policy` 还会报告自动扫描启用、轮询间隔、管理员要求、静态回退、区域扫描上限等有效门。Entity/Tk 的面板有意低频运行：刷新经过节流，源/诊断卡片在重建前先比 dirty signature。高频或动画 ACT 面板应优先 WebView/GPU 渲染，Cython 只用于经过测量的热点算术/签名路径。

## 当前 report / export 表面

`export` 通过共享 `act_platform.runtime` helper 与现有 `DpsHistoryStore` exporter 接入：

- status：`act_report_status(owner, limit=20, fmt="json")`
- save：`act_report_export(owner, fmt="json" | "csv" | "html" | "xml" | "xml.gz" | "xml.zip")`
- copy：`act_report_copy(owner, fmt="json")`

WebView 路由：`web/act_report_export.html`，从 `SAO Menu > ACT 报告/导出 Report Export` 打开。

Entity 路由：`entity://act/export`，从 `SAO 菜单 > 面板 > ACT报告/导出` 打开。

共享 payload 保留对等字段 `encounter_id`、`formats`、`selected_format`、`preview`，并增加 `history` 与 `storage_status` 用于用户反馈。JSON、CSV、静态 HTML、结构化 XML 与压缩 XML 由 `DpsHistoryStore.export_report()` 写出，UI 不再重复导出格式逻辑。Entity/Tk 使用低频诊断面板，带刷新节流与 dirty 渲染签名。

## 当前 Mini-Parse / 剪贴板 formatter 表面

`mini_parse` 通过共享 `act_platform.runtime` helper 暴露，使用与 export 相同的报告预览 payload：

- status：`act_mini_parse_status(owner, formatter_id="summary_table")`
- preview：`act_mini_parse_preview(owner, formatter_id="summary_table")`
- copy：`act_mini_parse_copy(owner, formatter_id="summary_table")`

共享 payload 保留 `formatter_id`、`formatters`、`preview`、`text`、`errors`。内置 formatter id 为 `summary_table`、`chat_ranking`、`json`；插件可通过 `ctx.register_formatter(...)` 增加 formatter id。

WebView 使用 pywebview API：`get_mini_parse_status`、`preview_mini_parse`、`copy_mini_parse`。Shim 还暴露命令名 `act.mini_parse.status`、`act.mini_parse.preview`、`act.mini_parse.copy` 给桥驱动的页面。

Entity 路由：`entity://act/mini_parse`，目前由报告/导出面板的 `Mini Copy` 动作以及同一份共享 status payload 暴露。

## 当前 Selective Parsing 表面

`selective_parsing` 通过共享 `act_platform.runtime` helper 接入：

- status：`act_selective_parsing_status(owner)`
- update：`act_selective_parsing_update(owner, policy)`
- clear/默认全部：`act_selective_parsing_clear(owner)`

共享 payload 保留 `enabled`、`mode`、`policy`、`filters`、`last_decision`。默认策略为关闭，记录全部解析事件。开启后，WebView 与 Entity 都调用同一份 `should_record_owner_combat_event(owner, event)` 决策，再去更新 ACT DPS/encounter 状态。

WebView 使用 pywebview API：`get_selective_parsing_status`、`update_selective_parsing`、`clear_selective_parsing`。Shim 也暴露命令名 `act.selective_parsing.status`、`act.selective_parsing.update`、`act.selective_parsing.clear`。

Entity 路由：`entity://act/selective_parsing`，目前由共享 runtime status/actions 与报告/导出面板在 Selective Parsing 启用时的状态提示暴露。

## 当前历史浏览表面

`history_browser` 通过共享 `act_platform.runtime` helper 与现有滚动 `DpsHistoryStore` 接入：

- status/搜索/过滤：`act_history_status(owner, limit=20, query="")`
- load：`act_history_load(owner, index=0, show=True)`
- 删除单条：`act_history_delete(owner, index=0)`
- 清空全部：`act_history_delete(owner, clear=True)`

WebView 路由：`web/act_report_export.html`，从 `SAO Menu > ACT 报告/导出 Report Export` 打开；其历史侧栏支持搜索、加载、删除、清空动作。

Entity 路由：`entity://act/export`，从 `SAO 菜单 > 面板 > ACT报告/导出` 打开；其历史侧栏暴露等价的 load/delete/clear 控件。

共享 payload 保留对等字段 `encounters`、`filters`、`cursor`、`storage_status`。`storage_status.archive` 在当前存储支持时暴露追加式 JSONL 归档路径/计数；`storage_status.sqlite` 暴露附加 SQLite 镜像的路径、schema 版本、encounter/combatant 计数，以及可选的 action/timeline/trigger/source 元数据行计数。每个列出的战斗都附带稳定的、按时间倒序的 `_history_index`，使过滤后的 UI 列表可以加载/删除目标记录而不是按过滤行号删除。Entity/Tk 保持低频与 dirty signature 门控；破坏性动作显式刷新缓存的面板状态而非轮询。

## 当前离线导入表面

`offline_import` 由独立的 WebView/Entity 向导 + 已有报告/历史表面 + 共享后端 provider 协同提供，覆盖规范化回放文件与 SAO 结构化 XML 报告往返，并在不支持的文件类型上回退到插件解析器适配器：

- import/replay/persist：`act_offline_import_file(owner, path, persist=True, show=False, history_limit=20)`
- 向导 status：`act_offline_import_status(owner, history_limit=20)`
- XML 报告回退：SAO `sao_act_report` 的 XML、`xml.gz`、`xml.zip` 与 zip 内含 XML 可作为 finalized 历史报告导入，无需 replay 事件
- 插件回退：激活的插件解析器适配器可通过受控 `import_file` 操作处理不支持的格式
- WebView 选择/导入 API：`choose_offline_import_file()`、`import_offline_report(path, persist=True, show=True)`
- Entity 导入动作：`OfflineImportPanel.import_file()`、`ReportExportPanel.import_offline_file(path=None)`

helper 先尝试规范化 ACT `.json`、`.jsonl`、`.ndjson` 文件，再尝试 SAO 结构化 XML 或压缩 XML 报告导入，最后在内置 importer 拒绝时尝试激活的插件解析器适配器。导入的事件经由 `ActReplayHarness` 回放，XML 报告以 finalized 紧凑报告形式导入；两条路径都可写入 `DpsHistoryStore`。返回保留对等字段 `source_path`、`format`、`importer`、`parser_adapter_id`、`plugin_id`、`self_uid`、`event_count`、`persisted`、`history_item`、`preview`、`snapshot` 与刷新后的 `status`。

独立 WebView 路由：`web/act_offline_import.html`，从 `SAO Menu > ACT 离线导入 Offline Import` 打开；可选择或接受路径，导入，显示最近导入状态/预览，加载最近历史报告。

WebView 报告/导出路由：`web/act_report_export.html`，从 `SAO Menu > ACT 报告/导出 Report Export` 打开；其侧栏可选择规范化回放文件或 SAO XML 报告文件，或接受粘贴路径，导入并刷新历史。

独立 Entity 路由：`entity://act/offline_import`，从 `SAO 菜单 > 面板 > ACT离线导入向导` 打开；与 WebView 向导一致，提供原生文件选择器、import、status、history load 动作。

Entity 报告/导出路由：`entity://act/export`，从 `SAO 菜单 > 面板 > ACT报告/导出` 打开；导入按钮打开原生文件选择器，导入选中的回放/报告，并刷新同一份预览/历史面板。

## 当前 timeline / VCR 表面

`encounter_timeline_vcr` 通过共享 `act_platform.runtime` helper 与 owner 域的 ACT `EventBus` 接入：

- status/open：`act_timeline_status(owner, limit=80, query="")`
- play：`act_timeline_play(owner, speed=1.0)`
- pause：`act_timeline_pause(owner)`
- step：`act_timeline_step(owner, delta_ms=1000)`
- seek：`act_timeline_seek(owner, cursor_ms=0)`
- 设置速度：`act_timeline_set_speed(owner, speed=1.0)`
- 过滤：`act_timeline_filter(owner, query="")`

WebView 路由：`web/act_timeline_vcr.html`，从 `SAO Menu > ACT 时间线/VCR Timeline` 打开。

Entity 路由：`entity://act/encounter_timeline_vcr`，从 `SAO 菜单 > 面板 > ACT时间线/VCR` 打开。

共享 payload 保留对等字段 `encounter_id`、`events`、`cursor_ms`、`speed`、`filters`。事件行是从 `EventBus.recent_events()` 派生的紧凑信封，UI 不直接解析包 payload。Entity/Tk 保持 350ms 节流刷新与 dirty 事件签名；动画或高频回放视觉应迁移到 WebView/GPU 路径，避免给 Tk 重绘加压。

## 当前行为日志表面

`action_log` 通过共享 `act_platform.runtime` helper 接入，live 模式使用 owner 域 ACT `EventBus` 紧凑行，history 模式使用 SQLite 行为历史行：

- status/open：`act_action_log_status(owner, limit=80, query="", topic="", cursor_ms=None, source="live", encounter_id="", offset=0)`
- search：`act_action_log_search(owner, query="", limit=80, source="live", encounter_id="", offset=0)`
- filter：`act_action_log_filter(owner, topic="", query=None, limit=80, source="live", encounter_id="", offset=0)`
- 跳到时间：`act_action_log_jump_to_time(owner, cursor_ms=0, limit=80, source="live", encounter_id="", offset=0)`
- copy：`act_action_log_copy(owner, limit=80, query="", topic="", source="live", encounter_id="", offset=0)`

WebView 路由：`web/act_action_log.html`，从 `SAO Menu > ACT 行为日志 Action Log` 打开。

Entity 路由：`entity://act/action_log`，从 `SAO 菜单 > 面板 > ACT行为日志` 打开。

两个路由都暴露 live/history 源切换与 previous/next 分页。`source="history"` 查询 `DpsHistoryStore.list_sqlite_actions(...)`，可按 `encounter_id` 缩窄，并返回与实时行为日志相同的行/列/光标结构。

共享 payload 保留对等字段 `encounter_id`、`source`、`rows`、`columns`、`filters`、`cursor`、`analytics`。`cursor` 现包含 `offset`、`total_row_count`、页索引/页数、上一页/下一页标志。`analytics` 汇总总行数、当前页、时间范围、数值合计与按 topic/actor/target/action 分组计数。行从 EventBus 信封或 SQLite 行为历史行派生，包含光标高亮元数据。Entity/Tk 渲染限制在当前 80 行页内，刷新节流为 350ms，并在重建前比 dirty signature。

## 当前死亡回放表面

`death_recap` 通过共享 `act_platform.runtime` helper 接入 owner 域 ACT `EventBus` 紧凑行：

- status/open：`act_death_recap_status(owner, limit=80, window_s=8.0, entity_id=None)`
- copy：`act_death_recap_copy(owner, limit=80, window_s=8.0, entity_id=None)`

WebView 路由：`web/act_death_recap.html`，从 `SAO Menu > ACT 死亡回放 Death Recap` 打开，使用 `get_death_recap_status(...)` 与 `copy_death_recap(...)`。

Entity 路由：`entity://act/death_recap`，从 `SAO 菜单 > 面板 > ACT死亡回放` 打开。

共享 payload 保留对等字段 `encounter_id`、`death`、`rows`、`columns`、`summary`、`window`、`filters`。runtime 从 `death` 主题、`is_dead/dead` 标志或 `hp=0` 自身/怪物状态找到最近一次死亡信号，并按配置窗口汇总入伤、治疗、护盾与减伤。Entity/Tk 把面板限制在 80 行内，刷新节流 350ms。

## 当前图表 / 曲线表面

`graph_timeseries` 通过共享 `act_platform.runtime` helper 接入 owner 域 ACT `EventBus` 紧凑行：

- status/open：`act_graph_timeseries_status(owner, metric="damage", limit=120, query=None, topic=None, time_range_ms=None)`
- 选择指标：`act_graph_timeseries_select_metric(owner, metric="damage", limit=120)`
- 缩放：`act_graph_timeseries_zoom(owner, time_range_ms=0, limit=120)`
- 过滤：`act_graph_timeseries_filter(owner, query=None, topic=None, limit=120)`
- 导出：`act_graph_timeseries_export(owner, metric=None, limit=120, query=None, topic=None)`

WebView 路由：`web/act_graph_timeseries.html`，从 `SAO Menu > ACT 图表/曲线 Graph` 打开。

Entity 路由：`entity://act/graph_timeseries`，从 `SAO 菜单 > 面板 > ACT图表/曲线` 打开。

共享 payload 保留对等字段 `encounter_id`、`series`、`metrics`、`time_range_ms`、`filters`。当前序列是 `damage`、`heal`、`event_count` 的轻量累计曲线，加上 boss payload 提供 HP 百分比时的仪表型 `boss_hp_pct`。WebView 拥有更丰富的 canvas 图表路径；Entity/Tk 渲染有界紧凑条形/表格，刷新节流 350ms 并使用 dirty signature，避免给 CPU 增加重绘压力。

## 当前战斗成员钻取表面

`combatant_drilldown` 通过共享 `act_platform.runtime` helper 接入，复用现有 DPS tracker 实体明细数据：

- status/open：`act_combatant_drilldown_status(owner, combatant_id=None, query=None, focus_target=None)`
- back：`act_combatant_drilldown_back(owner)`
- filter：`act_combatant_drilldown_filter(owner, combatant_id=None, query="")`
- 聚焦目标：`act_combatant_drilldown_focus_target(owner, combatant_id=None, target_id="")`

WebView 路由：`web/act_combatant_drilldown.html`，从 `SAO Menu > ACT 战斗成员 Drilldown` 打开。

Entity 路由：`entity://act/combatant_drilldown`，从 `SAO 菜单 > 面板 > ACT成员钻取` 打开。

共享 payload 保留对等字段 `encounter_id`、`combatant_id`、`summary`、`skills`、`incoming`、`outgoing`。实时明细来自 `DpsTracker.get_entity_detail(uid)`；当存储报告中包含实体行时可作为报告回退。Entity/Tk 把表面保持有界、刷新节流 350ms 并使用 dirty signature。

## 当前技能钻取表面

`skill_drilldown` 通过共享 `act_platform.runtime` helper 接入，复用战斗成员技能明细以及 owner 域 ACT `EventBus` 时间线引用：

- status/open：`act_skill_drilldown_status(owner, combatant_id=None, skill_id=None, query=None, limit=80)`
- back：`act_skill_drilldown_back(owner)`
- filter：`act_skill_drilldown_filter(owner, combatant_id=None, skill_id=None, query="", limit=80)`
- copy：`act_skill_drilldown_copy(owner, combatant_id=None, skill_id=None, query=None, limit=80)`

WebView 路由：`web/act_skill_drilldown.html`，从 `SAO Menu > ACT 技能 Drilldown` 打开。

Entity 路由：`entity://act/skill_drilldown`，从 `SAO 菜单 > 面板 > ACT技能钻取` 打开。

共享 payload 保留对等字段 `encounter_id`、`combatant_id`、`skill_id`、`casts`、`hits`、`crit_rate`、`timeline_refs`。静态技能事实来自 `DpsTracker.get_entity_detail(uid)`；实时引用从 `EventBus.recent_events()` 按 `skill_id` 过滤。Entity/Tk 把表面限制在 80 条 timeline ref，刷新节流 350ms 并使用 dirty signature。

## 安全注意

把 SDK 视为交付门，而不是只在文档里出现的元数据。如果 PR 修改了 ACT 行为，应该在同一个变更里更新注册表、两套 UI 声明与对等测试。

把脚手架绑定替换为真正的适配器声明时，请保持 SDK 与 UI 框架解耦，让回放、WebView、Entity 与插件代码都能共用同一份契约。
