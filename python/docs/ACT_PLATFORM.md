# SAO ACT 平台

SAO ACT UI 是一个游戏无关的战斗分析平台。所有游戏特定逻辑由插件提供，平台本身只负责事件分发、UI 渲染、数据存储和触发器执行。

配套文档：`PLUGIN_SDK.md`、`HYBRID_MEMORY_TCP.md`、`AI_EDITOR.md`。

---

## 平台能做什么

- **实时 DPS / HPS 统计** — 伤害、治疗、技能分布，支持单人和队伍
- **Boss 状态追踪** — 血量、护盾、破韧、技能施放
- **战斗回放与报告** — 历史战斗存档，导出 JSON / CSV / HTML / XML
- **触发器系统** — 自定义条件触发 TTS 语音提醒、屏幕横幅
- **Boss 机制编辑器** — 编排躲避提示和技能时间线
- **内存辅助读取** — Hybrid 模式下直读游戏内存，补充网络抓包数据
- **插件扩展** — 一键安装 `.zip` 插件包，零编译即用
- **内置 AI 编辑器** — 多模型 LLM 对话，支持工具调用和 MCP 集成

## 数据模式

平台支持三种数据获取模式：

| 模式 | 说明 |
|------|------|
| **TCP** | 纯网络抓包，无需管理员权限 |
| **Memory** | 纯内存读取，需要管理员权限 |
| **Hybrid**（推荐） | TCP 为主、内存补充，兼顾完整性和低延迟 |

在设置面板中切换模式。Hybrid 模式下，DPS 数据走 TCP，Boss 血量和自身状态走内存实时读取。

## 事件系统

平台的核心是一个发布/订阅事件总线。插件产生事件，其他插件或平台组件消费事件。

每个事件是一个标准信封：

```python
{
    "topic": "damage",
    "observed_at": 1718000000.0,
    "source": {"name": "tcp_parser", "kind": "packet", "game_id": "star_resonance"},
    "payload": {
        "attacker_id": 12345,
        "target_id": 67890,
        "skill_id": 1001,
        "damage": 85000,
        "element": "fire",
        "is_critical": True
    }
}
```

### 可用事件主题

| 主题 | 触发时机 |
|------|---------|
| `damage` | 造成伤害 |
| `heal` | 治疗 |
| `skill` | 技能释放 |
| `boss_state` | Boss 状态变化（血量/破韧/技能） |
| `self_state` | 自身状态变化（血量/体力/Buff） |
| `encounter_started` | 战斗开始 |
| `encounter_updated` | 战斗数据更新 |
| `encounter_finalized` | 战斗结束 |
| `act_snapshot` | DPS 表快照刷新 |
| `trigger_fired` | 触发器命中 |
| `dungeon` | 副本进入/退出 |
| `scene` | 场景切换 |
| `monster` | 怪物出现/消失 |

## 数据流

```
游戏进程
  → 网络抓包 / 内存读取
  → 插件解析器（将原始数据转为标准事件）
  → 事件总线
  → DPS 统计 / Boss 追踪 / 触发器 / 面板渲染 / 历史存档
```

## 用户界面

平台同时提供两套 UI，数据完全同步：

| 界面 | 说明 |
|------|------|
| **WebView** | 基于浏览器引擎的现代界面，支持主题切换 |
| **原生窗口** | 轻量级原生窗口，支持 GPU 叠层显示 |

两套界面可同时打开，所有操作和数据实时同步。

## 触发器配置

触发器让你在特定条件满足时自动执行动作（语音提醒、屏幕横幅等）。

### 内置触发器类型

**字段匹配** — 当事件字段满足条件时触发：

```
示例：Boss 血量低于 30% 时语音提醒
  事件主题：boss_state
  条件：payload.hp_pct < 0.3
  动作：TTS "Boss 血量不足三成"
```

**计时器** — 固定间隔触发：

```
示例：每 60 秒提醒吃药
  类型：timer_preset
  间隔：60 秒
  动作：TTS "检查增益状态"
```

插件还可以注册自定义触发器类型。

## 报告与历史

- 每场战斗自动存档（JSONL 格式）
- 支持导出为 JSON、CSV、HTML、XML
- SQLite 镜像用于快速查询历史记录
- 历史面板可回看任何一场战斗的完整数据

## 选择性记录

默认记录所有实体的数据。如果只关心特定目标，可以在设置中配置过滤策略：

- **仅自己** — 只记录自身数据
- **仅队伍** — 只记录队伍成员
- **包含列表** — 只记录指定实体
- **排除列表** — 排除指定实体

---

## 插件开发

### 插件结构

```
my_plugin/
├── plugin.json        — 插件清单
├── plugin.py          — 入口文件
├── requirements.txt   — 第三方依赖（可选）
├── vendor/            — 手动放置的纯 Python 包（可选）
├── assets/            — 数据文件
└── web/               — HTML 面板
```

用户安装的插件放在 `user_plugins/` 目录，软件更新时不会被覆盖。

### plugin.json 清单

```json
{
  "id": "my_game_plugin",
  "name": "我的游戏插件",
  "version": "1.0.0",
  "entry": "plugin.py",
  "enabled": true,
  "game_ids": ["my_game"],
  "permissions": ["engine_access"],
  "settings_schema": {
    "tts_enabled": {"type": "boolean", "default": true},
    "refresh_interval": {"type": "number", "default": 1.0}
  }
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `id` | 是 | 唯一标识符，建议与文件夹同名 |
| `name` | 否 | 显示名称 |
| `version` | 否 | 版本号 |
| `entry` | 否 | 入口文件，默认 `plugin.py` |
| `enabled` | 否 | 是否默认启用 |
| `game_ids` | 否 | 适配的游戏 ID |
| `requires` | 否 | 依赖的其他插件 ID |
| `permissions` | 否 | 声明的权限（`engine_access`、`input_control`） |
| `settings_schema` | 否 | 插件设置字段定义，自动生成设置界面 |
| `mcpServers` | 否 | 向 AI 编辑器暴露的 MCP 工具 |

### 插件生命周期

```
平台启动 → 发现插件 → on_load(ctx) → on_enable() → [运行中] → on_disable() → on_unload()
```

完整的入口示例：

```python
def on_load(ctx):
    """插件加载。ctx 是 PluginContext，提供所有平台能力。"""
    # 安装第三方依赖（如果有 requirements.txt）
    ctx.ensure_requirements(install=True)

    # 订阅伤害事件，每次有人造成伤害时回调
    ctx.on_damage(handle_damage)

    # 订阅 Boss 状态变化
    ctx.on_boss(handle_boss_state)

    # 注册一个菜单项
    ctx.register_menu_category('我的工具', '🔧', build_my_menu, priority=20)

    # 注册一个 HUD 面板
    ctx.register_ui_panel('my_panel', {
        'title': '自定义面板',
        'width': 300,
        'height': 200,
    }, render=render_panel, on_action=handle_action)

def on_enable():
    """插件被用户启用。"""

def on_disable():
    """插件被用户禁用。"""

def on_unload():
    """插件卸载，事件订阅和 sys.path 自动清理。"""


def handle_damage(event):
    """处理伤害事件。"""
    dmg = event['payload']
    if dmg.get('is_critical') and dmg['damage'] > 100000:
        print(f"暴击 {dmg['damage']}!")

def handle_boss_state(event):
    """Boss 状态变化。"""
    boss = event['payload']
    if boss.get('hp_pct', 1.0) < 0.1:
        ctx.toast("Boss 即将倒下！", level="warning")

def render_panel(ctx):
    """返回面板内容，平台在所有 UI 表面上渲染。"""
    snap = ctx.get_snapshot()
    if not snap:
        return {"text": "等待数据..."}
    return {
        "text": f"DPS: {snap.get('total_dps', 0):,.0f}",
        "bars": [{"label": e['name'], "value": e['dps']}
                 for e in snap.get('entities', [])[:5]]
    }
```

### PluginContext API

#### 事件

```python
# 订阅任意主题
ctx.subscribe('damage', callback)
ctx.subscribe('boss_state', callback)

# 快捷订阅
ctx.on_damage(callback)       # 伤害
ctx.on_boss(callback)         # Boss 状态
ctx.on_snapshot(callback)     # DPS 快照刷新
ctx.on_encounter_finalized(callback)  # 战斗结束

# 发布自定义事件
ctx.emit('my_custom_topic', {"key": "value"})

# 主动获取数据
snap = ctx.get_snapshot()              # 最新 DPS 快照
events = ctx.recent_events('damage', limit=50)  # 最近 50 条伤害事件
```

#### UI

```python
# 注册 HUD 面板（Tk 和 WebView 双端自动渲染）
ctx.register_ui_panel('panel_id', meta_dict, render=fn, on_action=fn)

# 注册菜单分类
ctx.register_menu_category('分类名', '图标', builder_fn, priority=10)

# 弹出独立窗口
ctx.open_window('panel_id', width=400, height=300)

# 请求面板重绘
ctx.request_redraw('panel_id')

# 通知和 Toast
ctx.notify("标题", "内容", duration_s=5)
ctx.toast("操作成功", level="info")  # level: info / warning / error
```

#### 渲染钩子

```python
# 在指定 UI 表面上叠加自定义渲染
ctx.register_render_hook('dps_overlay', my_render_hook)
ctx.set_overlay('dps_overlay', overlay_data)
```

#### 引擎句柄

受信任的插件可以通过 `ctx.engine` 访问平台内部组件：

```python
# 获取 DPS 追踪器
tracker = ctx.engine.get('dps_tracker')

# 获取游戏状态管理器
state = ctx.engine.get('game_state')
snapshot = state.snapshot()

# 获取触发器引擎
triggers = ctx.engine.get('trigger_engine')
```

常用句柄：`game_state`、`dps_tracker`、`encounter_manager`、`trigger_engine`、`packet_bridge`、`memory_bridge`、`auto_key_engine`、`boss_raid_engine`、`settings`、`history_store`。

#### 设置

```python
# 读写插件设置（自动持久化）
sound_on = ctx.get_setting('tts_enabled', default=True)
ctx.set_setting('tts_enabled', False)

# 批量设置默认值
ctx.set_defaults({'tts_enabled': True, 'refresh_interval': 1.0})
```

#### 触发器

```python
# 注册自定义触发器类型
ctx.register_trigger_type('low_hp_warning', {
    'name': 'HP 过低提醒',
    'description': '自身 HP 低于阈值时触发',
}, handler=check_low_hp)

def check_low_hp(event, config):
    """返回 True 表示触发。"""
    if event['topic'] != 'self_state':
        return False
    hp_pct = event['payload'].get('hp_pct', 1.0)
    return hp_pct < config.get('threshold', 0.3)
```

#### 快捷键

```python
# 注册快捷键（CTRL+组合键，避免与主界面冲突）
ctx.register_hotkey('ctrl+shift+d', toggle_dps_panel, label='切换 DPS 面板')
```

#### 数据源

```python
# 注册自定义数据源
ctx.register_data_source('my_source', {
    'name': '自定义数据源',
    'description': '从自定义协议读取数据',
}, start=start_capture, stop=stop_capture)
```

### 第三方依赖

在插件目录放一个 `requirements.txt`：

```
numpy>=1.24
protobuf>=4.24
```

在 `on_load` 开头调用 `ctx.ensure_requirements(install=True)`，平台会自动把缺失的包安装到插件的 `libs/` 目录。也可以手动把纯 Python 包放到 `vendor/` 目录。卸载插件时 `sys.path` 自动清理。

### 向 AI 编辑器暴露工具

插件可以让 AI 编辑器调用游戏相关的工具：

```python
# 方式一：在 plugin.json 声明
# "mcpServers": {"my_tools": {"transport": "internal", "name": "My Tools"}}

# 方式二：代码注册
from ai_editor.mcp_client import InternalMcpProvider

provider = InternalMcpProvider("my_plugin")
provider.add_tool(
    "get_boss_hp",
    "读取当前 Boss 血量",
    {"type": "object", "properties": {}},
    handler=lambda: {"hp": state.boss_current_hp, "max": state.boss_total_hp}
)
```

### 安装插件

用户安装插件只需三步：

1. 拿到 `.zip` 插件包
2. 在 ACT 设置面板点击「导入插件」，选择 `.zip` 文件
3. 插件自动解压到 `user_plugins/`，启用即用

也可以手动把插件文件夹放到 `user_plugins/` 目录，重启后生效。

### 注意事项

- 插件运行在主进程内，事件回调应保持轻量，耗时操作请开后台线程
- 事件总线会隔离回调异常——单个插件崩溃不会影响其他插件
- 快捷键用 `CTRL+` 组合，避免和平台的 F5-F12 功能键冲突
- 双端 UI（Tk + WebView）的面板内容必须保持一致
