# ACT 平台架构

> 版本 `5.0.0`。配套文档：`PLUGIN_SDK.md`、`HYBRID_MEMORY_TCP.md`、`AI_EDITOR.md`。

SAO ACT UI 是**游戏无关的共享运行时**。游戏特定逻辑全部由插件提供。`plugins/star_resonance_plugin/` 是参考适配器。

## 核心模块

| 模块 | 职责 |
|------|------|
| `act_platform/event_bus.py` | 同步发布/订阅，有界历史，回调隔离 |
| `act_platform/events.py` | 规范化 ACT 事件信封 (`make_event`) |
| `act_platform/plugins.py` | 插件发现、加载、SDK (`PluginContext`)、引擎句柄 |
| `act_platform/adapters.py` | 解析器适配器契约 |
| `act_platform/runtime.py` | 共享 helper（报告/历史/触发器/面板状态） |
| `act_platform/selective_parsing.py` | 选择性记录策略 |
| `act_platform/ui_spec.py` | 声明式 UI 构建器 |
| `act_platform/render_hooks.py` | 渲染钩子注册表 |
| `mem_probe/` | 通用内存扫描（进程/扫描器/驱动），插件注入桥接 |
| `ai_editor/` | AI IDE（LLM 对话/tool calling/MCP/扩展市场） |

## 事件系统

事件信封是普通 dict：

```python
{
    "schema_version": 1,
    "id": "abc123...",
    "topic": "damage",
    "observed_at": 1718000000.0,
    "source": {"name": "tcp_parser", "kind": "packet", "game_id": "star_resonance"},
    "payload": { ... }
}
```

主题：`damage`、`heal`、`skill`、`dungeon`、`scene`、`monster`、`boss`、`boss_state`、`self_state`、`encounter_started`、`encounter_updated`、`encounter_finalized`、`act_snapshot`、`trigger_fired`。

## 数据流

```
插件数据源 (TCP/Memory)
  → PacketBridge / UnifiedDataSource
  → 插件 Parser Adapter
  → EventBus.publish(make_event(...))
  → 插件订阅者 / 触发器 / 行为日志 / 面板
```

## 插件扩展

插件在 `on_load(ctx)` 中注册：

- `parser_adapters` — 包解析器
- `exporters` — 导出格式
- `formatters` — Mini-Parse 格式化
- `trigger_types` — 触发器规则
- `report_views` — 报告视图
- `timers` — 计时器
- `ui_panels` — HUD 面板
- `menu_categories` — 游戏菜单
- `data_sources` — 数据源
- `engines` — 游戏引擎实例

详见 `PLUGIN_SDK.md`。

## UI 表面

平台提供三套 UI：

| 表面 | 技术 | 入口 |
|------|------|------|
| WebView | pywebview + WebView2/EdgeChromium | `sao_webview.py` |
| Entity/Tk | tkinter + GPU overlay | `sao_gui.py` + `gui_modules/` |
| AI Editor | pywebview 独立窗口 | `ai_editor/app.py` |

插件通过 `ctx.register_ui_panel()` 注册面板，平台在所有表面上渲染。

## 触发器

`ActTriggerEngine` 由插件通过 `ctx.register_trigger_type()` 注册规则类型。支持 `field_match`（点路径 + 运算符）、`timer_preset`、自定义 handler。

## 报告与历史

插件提供 DPS tracker 和 encounter manager 实例，平台负责滚动存储、JSONL 归档、SQLite 镜像、JSON/CSV/HTML/XML 导入导出。

## 选择性记录

默认全量记录。插件可调 `act_selective_parsing_update(owner, policy)` 设置 self/party/include/exclude 过滤。

## 打包

- `XiaoACTUI.spec` 的 `collect_plugins()` 收集插件文件（排除 il2cpp/out、il2cpp/bin 等开发产物）
- 插件第三方依赖通过 `requirements.txt` + `ctx.ensure_requirements()` 自管理
- 敏感模块（`mem_probe/`、`license/`）只走全量 rebuild，不走 delta 增量

## 验证

```bash
python -m act_platform.selftest
python -m act_replay.selftest
python -m py_compile act_platform/events.py act_platform/event_bus.py act_platform/plugins.py
```
