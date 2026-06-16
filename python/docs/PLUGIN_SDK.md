# 插件 SDK

> 版本 `5.0.0`。配套文档：`ACT_PLATFORM.md`、`AI_EDITOR.md`。

SAO ACT UI 的插件系统让游戏适配器以独立目录的形式加载，不修改平台代码。

## 目录结构

```
plugins/<plugin_id>/
├── plugin.json          — 清单
├── plugin.py            — 入口 (on_load / on_enable / on_disable / on_unload)
├── requirements.txt     — 第三方依赖 (可选)
├── vendor/              — 手动放置的纯 Python 包 (可选)
├── libs/                — pip 自动安装目标 (自动创建)
├── assets/              — 数据文件
├── web/                 — HTML 面板
├── engines/             — 游戏引擎模块
├── mem/                 — 内存读取器
├── net/                 — 网络抓包
├── panels/              — Tk 面板
├── cython/              — 编译加速 (.pyd)
└── protocol/            — 协议解析
```

`user_plugins/` 结构相同，用于用户安装的插件，更新时不被覆盖。

## plugin.json

```json
{
  "id": "star_resonance",
  "name": "星痕共鸣 Star Resonance",
  "version": "1.0.0",
  "entry": "plugin.py",
  "enabled": true,
  "game_ids": ["star_resonance"],
  "requires": [],
  "permissions": ["engine_access", "input_control"],
  "capabilities": [
    {"id": "ui_panels", "title": "面板", "actions": ["toggle_dps", "toggle_hp"]}
  ],
  "settings_schema": {
    "sound_enabled": {"type": "boolean", "default": true}
  },
  "mcpServers": {
    "sr_tools": {"transport": "internal", "name": "SR Game Tools"}
  }
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `id` | 是 | 稳定 ID，建议与文件夹同名 |
| `name` | 否 | 显示名，默认 = id |
| `version` | 否 | 默认 `0.1.0` |
| `entry` | 否 | 入口文件，默认 `plugin.py`，必须在插件目录内 |
| `enabled` | 否 | 启动时自动加载，默认 true |
| `game_ids` | 否 | 适配的游戏 ID 列表 |
| `requires` | 否 | 依赖的其他插件 ID（按顺序加载） |
| `permissions` | 否 | 声明的权限标记 |
| `capabilities` | 否 | ACT capability 声明 |
| `settings_schema` | 否 | 插件设置字段元数据 |
| `mcpServers` | 否 | 向 AI Editor 暴露的 MCP 服务器 |

## 生命周期

```
加载 → on_load(ctx) → on_enable() → [运行中] → on_disable() → on_unload()
```

```python
def on_load(ctx):
    """初始化。ctx 是 PluginContext 实例。"""
    ctx.ensure_requirements(install=True)  # bootstrap 依赖
    ctx.register_menu_category('自动', '⚡', build_menu, priority=10)
    ctx.register_ui_panel('dps', {...}, render=render_dps, on_action=on_action)
    ctx.register_engine('game_state', my_game_state_manager)
    ctx.subscribe('damage', on_damage)

def on_enable():
    """启用。"""

def on_disable():
    """禁用。"""

def on_unload():
    """卸载。sys.path 和事件订阅自动清理。"""
```

## PluginContext API

### 事件

| 方法 | 说明 |
|------|------|
| `ctx.subscribe(topic, callback)` | 订阅事件 |
| `ctx.on_damage(callback)` | 订阅伤害事件 |
| `ctx.on_boss(callback)` | 订阅 Boss 事件 |
| `ctx.on_snapshot(callback)` | 订阅 ACT 快照 |
| `ctx.on_encounter_finalized(callback)` | 战斗结束 |
| `ctx.emit(topic, payload)` | 发布事件 |
| `ctx.get_snapshot()` | 获取最新 ACT 快照 |
| `ctx.recent_events(topic, limit)` | 最近事件 |

### UI

| 方法 | 说明 |
|------|------|
| `ctx.register_ui_panel(id, meta, render, on_action)` | 注册声明式 UI 面板 |
| `ctx.register_render_hook(surface, hook)` | 注册渲染钩子 |
| `ctx.register_menu_category(name, icon, builder, priority)` | 注册菜单分类 |
| `ctx.open_window(panel_id, width, height)` | 弹出独立窗口 |
| `ctx.request_redraw(panel_id)` | 请求面板重绘 |
| `ctx.set_overlay(surface, data)` | 设置 GPU 叠层数据 |
| `ctx.notify(title, message, duration_s)` | 显示通知 |
| `ctx.toast(message, level)` | 显示 Toast |

### 引擎

| 方法 | 说明 |
|------|------|
| `ctx.register_engine(name, instance)` | 注册引擎实例 |
| `ctx.register_data_source(id, meta, start, stop)` | 注册数据源 |
| `ctx.register_parser_adapter(id, meta, handler)` | 注册解析器适配器 |
| `ctx.register_trigger_type(type_id, meta, handler)` | 注册触发器类型 |
| `ctx.register_hotkey(key, callback, label)` | 注册快捷键 |

### 设置

| 方法 | 说明 |
|------|------|
| `ctx.get_setting(key, default)` | 读取插件设置 |
| `ctx.set_setting(key, value)` | 写入插件设置 |
| `ctx.set_defaults(dict)` | 设置默认值 |

### 依赖与模块

| 方法 | 说明 |
|------|------|
| `ctx.ensure_requirements(install)` | 从 requirements.txt bootstrap 依赖到 libs/ |
| `ctx.load_local(path)` | 加载插件内的 Python 模块 |

### EngineAccess (ctx.engine)

受信任的进程内插件通过 `ctx.engine` 获取引擎句柄：

```python
tracker = ctx.engine.get('dps_tracker')
bridge = ctx.engine.get('packet_bridge')
state = ctx.engine.get('game_state')
```

可用句柄名：`owner`、`event_bus`、`plugin_manager`、`settings`、`game_state`、`state_manager`、`dps_tracker`、`history_store`、`encounter_manager`、`trigger_engine`、`packet_bridge`、`memory_bridge`、`auto_key_engine`、`boss_raid_engine`、`window_locator`。

## 扩展类型

插件可注册以下扩展：

| 类型 | 注册方法 | 说明 |
|------|---------|------|
| `parser_adapters` | `register_parser_adapter` | TCP/内存包解析器 |
| `exporters` | `register_exporter` | 报告导出格式 |
| `formatters` | `register_formatter` | Mini-Parse 格式化 |
| `trigger_types` | `register_trigger_type` | 触发器规则 |
| `report_views` | `register_report_view` | 报告视图 |
| `timers` | `register_timer` | 倒计时器 |
| `ui_panels` | `register_ui_panel` | HUD 面板 |
| `menu_categories` | `register_menu_category` | 游戏菜单 |
| `data_sources` | `register_data_source` | 数据源 |

## 依赖管理

在插件目录下放 `requirements.txt`：

```
numpy>=1.24
opencv-python-headless>=4.8
protobuf>=4.24
```

在 `on_load` 第一行调用 `ctx.ensure_requirements(install=True)`。流程：

1. 扫描 `vendor/` 和 `libs/` 已有的包
2. 缺失的用 `pip install --target libs/` 安装
3. `vendor/` + `libs/` prepend 到 `sys.path`
4. 卸载时自动恢复 `sys.path`

PyInstaller 打包时第三方包已在 runtime/ 中，此机制用于源码运行和便携分发。

## mem_probe 桥接

`mem_probe/` 是通用内存扫描基础设施。游戏特化桥接由插件注入：

```python
from mem_probe.unified_source import set_bridge_classes

def on_load(ctx):
    from .mem.il2cpp.mem_state_bridge import MemStateBridge
    from .mem.il2cpp.mem_self_state_provider import MemSelfStateProvider
    set_bridge_classes(MemStateBridge, MemSelfStateProvider)
```

## MCP 工具

插件可向 AI Editor 暴露工具：

```python
from ai_editor.mcp_client import InternalMcpProvider

provider = InternalMcpProvider("my_plugin")
provider.add_tool("get_hp", "Read HP", {"type":"object","properties":{}},
                  handler=lambda: {"hp": 50000})
```

也可在 plugin.json 的 `mcpServers` 中声明。

## 安全

插件运行在主进程内。事件回调保持精简，重型工作开后台线程。事件总线隔离回调异常并记入失败统计。
