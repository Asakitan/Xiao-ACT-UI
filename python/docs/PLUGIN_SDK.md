# 插件开发指南

本文档面向希望为 SAO ACT UI 编写插件的开发者。涵盖从零创建插件到发布分发的全过程。

---

## 快速开始

一个最小插件只需要两个文件：

```
my_plugin/
├── plugin.json
└── plugin.py
```

**plugin.json**:
```json
{
  "id": "my_plugin",
  "name": "我的插件",
  "entry": "plugin.py",
  "enabled": true
}
```

**plugin.py**:
```python
_ctx = None

def on_load(ctx):
    global _ctx
    _ctx = ctx
    ctx.register_ui_panel(
        "hello",
        {"title": "Hello World"},
        render=_render,
        on_action=_on_action,
    )
    ctx.log("my_plugin loaded")

def on_enable():
    pass

def on_disable():
    pass

def on_unload():
    pass

def _render(_payload=None):
    ui = _ctx.ui
    return ui.panel("Hello World", [
        ui.text("这是我的第一个插件"),
        ui.button("点我", action="greet", style="primary"),
    ])

def _on_action(action_id, payload=None):
    if action_id == "greet":
        _ctx.toast("你好！")
    return _render()
```

把 `my_plugin/` 文件夹放入 `user_plugins/` 目录，重启程序即可加载。也可以打成 `.zip` 包通过一键导入安装。

---

## 插件目录结构

```
my_plugin/
├── plugin.json          — 清单（必须）
├── plugin.py            — 入口模块（必须）
├── requirements.txt     — 第三方依赖（可选）
├── vendor/              — 手动放置的纯 Python 包（可选，离线优先）
├── libs/                — pip 自动安装目标（开发时自动创建）
├── assets/              — 数据文件（图片、JSON、模板等）
├── web/                 — HTML 面板文件
├── engines/             — 插件自带的引擎模块
├── mem/                 — 内存读取器
├── net/                 — 网络抓包模块
├── panels/              — Tk 面板
└── cython/              — 编译加速模块（.pyd）
```

内置插件放在 `plugins/`，用户安装的插件放在 `user_plugins/`。`user_plugins/` 目录在程序更新时不会被覆盖。

---

## plugin.json 清单

完整字段说明：

```json
{
  "id": "my_plugin",
  "name": "我的插件",
  "version": "1.0.0",
  "entry": "plugin.py",
  "description": "插件功能简述",
  "enabled": true,
  "game_ids": ["star_resonance"],
  "requires": ["other_plugin"],
  "permissions": ["engine_access", "input_control"],
  "capabilities": [
    "plugin_manager",
    {
      "id": "ui_panels",
      "title": "主面板",
      "description": "面板功能描述",
      "actions": ["toggle", "refresh"],
      "width": 600,
      "height": 400,
      "hidden": false,
      "primary": true
    }
  ],
  "settings_schema": {
    "sound_enabled": {
      "type": "boolean",
      "default": true,
      "description": "是否启用音效"
    },
    "refresh_interval": {
      "type": "integer",
      "default": 5,
      "description": "刷新间隔（秒）"
    },
    "detail_mode": {
      "type": "string",
      "default": "seconds",
      "enum": ["seconds", "minute"],
      "enum_labels": {
        "seconds": "秒级刷新",
        "minute": "分钟刷新"
      },
      "description": "刷新细节"
    },
    "api_key": {
      "type": "string",
      "default": "",
      "description": "API 密钥"
    }
  },
  "locales": {
    "en-US": {
      "name": "My Plugin",
      "description": "Brief plugin description",
      "capabilities": {
        "ui_panels": {
          "title": "Main Panel",
          "description": "Panel feature description"
        }
      },
      "sao_menu": {
        "name": "My Tool",
        "script_label": "My Tool",
        "actions": {
          "script.state": {"label": "Read state"}
        }
      },
      "settings_schema": {
        "sound_enabled": {
          "description": "Enable sound effects"
        },
        "detail_mode": {
          "description": "Refresh detail",
          "enum_labels": {
            "seconds": "Seconds",
            "minute": "Minute"
          }
        }
      }
    }
  }
}
```

| 字段 | 必填 | 默认值 | 说明 |
|------|:----:|--------|------|
| `id` | 是 | — | 插件唯一 ID，建议与文件夹同名。只允许字母、数字、`-`、`.`、`_` |
| `name` | 否 | 同 `id` | 显示名称 |
| `version` | 否 | `"0.1.0"` | 语义版本号 |
| `entry` | 否 | `"plugin.py"` | 入口文件，必须在插件目录内 |
| `description` | 否 | — | 在插件管理界面显示的说明文字 |
| `enabled` | 否 | `true` | 程序启动时是否自动加载。用户可在管理界面切换 |
| `game_ids` | 否 | — | 适配的游戏 ID 列表（如 `["star_resonance"]`） |
| `requires` | 否 | — | 依赖的其他插件 ID，平台会先加载依赖项 |
| `permissions` | 否 | — | 声明需要的权限标记 |
| `capabilities` | 否 | — | 声明插件提供的能力（面板、自动化等） |
| `settings_schema` | 否 | — | 插件设置字段定义，支持 `boolean`/`integer`/`string` 类型 |
| `locales`/`i18n`/`translations` | 否 | — | 按 locale 覆盖显示文本，支持 `en-US`、`zh-CN`、`zh` 等 locale key |

### manifest 本地化

平台启动和热刷新时会读取 `locales`（也兼容 `i18n`、`translations`）里的本地化覆盖。当前语言来自设置项 `act_plugin_locale`/`plugin_locale`/`ui_locale`/`locale` 等，未设置时默认 `zh-CN`。locale key 会正规化，例如 `en_US` 会作为 `en-US` 处理；`en-GB` 会按 `en` → `en-US` → `en-GB` 顺序合并，精确 locale 优先。

本地化覆盖只应用展示文本，不能改变插件行为。可覆盖字段包括：

- 顶层：`name`、`description`
- `sao_menu`：`name`、`category`、`title`、`label`、`script_label`、`toggle_label`、`enable_label`、`disable_label`
- `sao_menu.actions`：按 `id` 覆盖 `label`、`title`、`description`、`tooltip`
- `capabilities`：按 `id` 覆盖 `label`、`title`、`description`、`display_name`、`render_hint`
- `settings_schema`：按设置 key 覆盖 `label`、`title`、`description`、`help`、`tooltip`、`placeholder`、`unit`、`prefix`、`suffix`；设置项里的 `options`/`choices` 可按既有 `id`/`value` 覆盖选项 `label`、`title`、`description`、`help`、`tooltip`，`enum_labels`/`value_labels` 可按既有枚举值覆盖显示文案

不会从 locale 覆盖新增/删除 action、capability、setting 或 setting option，也不会覆盖 `enabled`、`default`、`type`、`enum`、`priority`、`surface`、`action_id`、坐标、权限、选项 `value` 等行为字段。这样菜单和设置可以即时切换语言，同时避免翻译文件改变插件加载、开关或权限语义。

### capabilities 详解

capabilities 数组中的每个元素可以是字符串或对象：

```json
"capabilities": [
  "plugin_manager",
  {
    "id": "ui_panels",
    "title": "DPS 面板",
    "actions": ["toggle_dps", "reset"],
    "width": 800,
    "height": 600,
    "min_width": 400,
    "min_height": 300,
    "hidden": false,
    "primary": true
  }
]
```

对象形式支持的字段：

| 字段 | 说明 |
|------|------|
| `id` | 能力 ID |
| `title` | 显示名称 |
| `description` | 说明文字 |
| `actions` | 面板支持的按钮动作列表 |
| `width` / `height` | 独立窗口默认尺寸（px） |
| `min_width` / `min_height` | 最小尺寸约束 |
| `hidden` | `true` 表示不在菜单列出，通过 `ctx.open_window()` 打开 |
| `primary` | `true` 表示这是插件的主面板 |

---

## 生命周期

```
安装 → 发现 → 加载(on_load) → 启用(on_enable) → [ 运行中 ] → 禁用(on_disable) → 卸载(on_unload)
```

### 各阶段说明

**安装**：将插件文件夹放入 `user_plugins/`，或通过 `.zip` 包一键导入。

**发现**：程序启动时扫描 `plugins/` 和 `user_plugins/`，读取每个目录下的 `plugin.json`。
热刷新也会重新读取清单；同版本的菜单、设置、本地化、权限等清单变化会发布 `plugin_lifecycle` 事件 `manifest_changed`，让菜单、设置面板和 overlay host 立即刷新，但不会因此加载仍处于禁用状态的脚本插件。

**加载 (on_load)**：平台按 `requires` 依赖顺序加载。`on_load(ctx)` 是插件初始化的入口，在这里注册面板、订阅事件、声明引擎。此时收到的 `ctx` 是 `PluginContext` 实例，贯穿插件的整个生命周期。

**启用 (on_enable)**：加载完成后立即调用。可以在这里启动后台任务。

**运行中**：插件正常工作，响应事件、渲染面板、处理用户操作。

**禁用 (on_disable)**：用户手动禁用或程序关闭时调用。停止后台任务，释放资源。

**卸载 (on_unload)**：插件完全卸载。平台会自动清理事件订阅、定时器、`sys.path` 修改。在这里做额外清理（关闭窗口、断开连接等）。

### 错误处理

- `on_load` 抛异常 = 加载失败，插件不会进入运行状态
- `on_disable` / `on_unload` 的异常会被捕获并记录，不影响卸载流程
- 事件回调的异常被隔离，不影响其他订阅者
- 连续失败达到上限（默认 3 次）后，插件会被自动禁用

---

## PluginContext API

`ctx` 是插件与平台交互的唯一通道。以下按使用场景分组。

### 属性

```python
ctx.plugin_id    # 插件 ID（只读）
ctx.path         # 插件目录路径
ctx.web_path     # 插件 web/ 目录路径
ctx.assets_path  # 插件 assets/ 目录路径
ctx.ui           # 声明式 UI 构建器
ctx.engine       # 引擎访问桥
ctx.event_bus    # 事件总线
ctx.owner        # 宿主对象（如果可用）
ctx.mem          # 内存扫描门面（hybrid/auto/memory 模式下可用）
```

### 事件订阅

订阅平台事件，当事件发生时回调函数会被调用。所有订阅方法返回一个 token 字符串，用于取消订阅。

```python
# 通用订阅
token = ctx.subscribe("damage", on_damage_event)
token = ctx.subscribe("boss", on_boss_event)

# 快捷方法
token = ctx.on_damage(callback)              # 伤害事件
token = ctx.on_heal(callback)                # 治疗事件
token = ctx.on_skill(callback)               # 技能事件
token = ctx.on_boss(callback)                # Boss 事件
token = ctx.on_snapshot(callback)            # ACT 实时快照（高频）
token = ctx.on_encounter_finalized(callback) # 战斗结束汇总

# 装饰器写法
@ctx.on("damage")
def handle_damage(event):
    payload = event["payload"]
    actor = payload.get("actor", "")
    damage = payload.get("damage", 0)
    ctx.log(f"{actor} dealt {damage}")

# 一次性订阅（触发一次后自动取消）
token = ctx.subscribe_once("encounter_finalized", on_first_clear)

# 取消订阅
ctx.unsubscribe(token)
```

**可用事件主题**：

| 主题 | 说明 |
|------|------|
| `damage` | 伤害事件 |
| `heal` | 治疗事件 |
| `skill` | 技能释放 |
| `boss` | Boss 机制 / 阶段 |
| `boss_state` | Boss 血量 / 状态 |
| `monster` | 怪物状态 |
| `scene` | 场景 / 地图切换 |
| `dungeon` | 副本状态 |
| `self_state` | 玩家自身状态变化 |
| `encounter_started` | 战斗开始 |
| `encounter_updated` | 战斗进行中更新 |
| `encounter_finalized` | 战斗结束汇总 |
| `act_snapshot` | 实时数据快照（高频，不保留历史） |
| `trigger_fired` | 触发器 / 告警事件 |
| `raw_packet` | 原始网络 / 内存数据 |
| `parsed_event` | 通用解析事件 |

**事件结构**：

```python
{
    "schema_version": 1,
    "id": "事件唯一ID",
    "topic": "damage",
    "observed_at": 1234567890.123,       # Unix 时间戳
    "source": {
        "name": "解析器名",
        "kind": "parser",                # parser / plugin / trigger
        "game_id": "star_resonance",
        "confidence": 0.95               # 0.0 - 1.0
    },
    "payload": { ... }                   # 事件数据（按主题不同）
}
```

### 事件发布与查询

```python
# 发布自定义事件（source_name 自动设为插件 ID）
ctx.emit("my_custom_event", {"key": "value"})

# 获取最新的 ACT 快照
snapshot = ctx.get_snapshot()

# 用点号路径取快照中的值
dps = ctx.snapshot_value("encounter.dps", default=0)

# 查询历史事件
events = ctx.recent_events(limit=20, topic="damage")
```

### UI 面板

注册一个声明式 UI 面板，可在面板管理器中显示或弹出独立窗口。

```python
ctx.register_ui_panel(
    "my_panel",                           # 面板 ID
    {                                      # 元信息
        "title": "我的面板",
        "description": "面板功能说明",
        "width": 600, "height": 400,       # 独立窗口尺寸
    },
    render=my_render,                      # 渲染回调
    on_action=my_action,                   # 按钮动作回调
)
```

**渲染回调**：返回一个 UI spec 字典。平台在需要重绘时调用。

```python
def my_render(payload=None):
    ui = _ctx.ui
    return ui.panel("监控面板", [
        ui.section("状态", [
            ui.row([
                ui.kv("DPS", "12,345"),
                ui.badge("运行中", "ok"),
            ]),
            ui.bar("血量", pct=0.75, color="ok", caption="75%"),
        ]),
        ui.divider(),
        ui.row([
            ui.button("开始", action="start", style="primary"),
            ui.button("停止", action="stop", style="danger"),
            ui.button("重置", action="reset"),
        ]),
    ])
```

**动作回调**：处理面板按钮点击。返回新的 spec 字典可立即重绘面板。

```python
def my_action(action_id, payload=None):
    if action_id == "start":
        start_monitoring()
    elif action_id == "stop":
        stop_monitoring()
    elif action_id == "reset":
        reset_data()
    return my_render()  # 返回新界面
```

### UI 构建器 (ctx.ui)

所有方法返回纯字典（JSON 安全），由平台的 Tk 或 WebView 渲染器解读。

**容器**：

```python
ui.panel(title, children)              # 顶层容器
ui.section(title, children, accent)    # 分区（带标题和强调色）
ui.card(title, children)               # 卡片（带边框）
ui.row(children, align)                # 水平行（align: left/center/right）
ui.group(children)                     # 分组（无装饰）
```

**文本与数据**：

```python
ui.text(text, style, align)            # 文本（style 见下表）
ui.title(text)                         # 标题文本
ui.kv(label, value, style)             # 键值对
ui.badge(text, style)                  # 徽章标签
ui.bar(label, pct, color, caption)     # 进度条（pct: 0.0-1.0）
ui.divider()                           # 分隔线
ui.spacer(size)                        # 空白（0-64 px）
```

**交互**：

```python
ui.button(label, action, style, payload, disabled)  # 按钮
ui.input(id, value, placeholder, input_type, width)  # 输入框
ui.table(columns, rows, highlight_key, title)        # 表格
```

**画布**：

```python
ui.canvas(width, height, ops, bg)      # 画布容器
ui.rect(x, y, w, h, fill, outline)     # 矩形
ui.oval(x, y, w, h, fill, outline)     # 椭圆
ui.line(x1, y1, x2, y2, fill, width)   # 线段
ui.ctext(x, y, text, fill, size, anchor, bold)  # 文字
```

**文本样式 (style)**：`value` / `title` / `subtitle` / `label` / `muted` / `ok` / `warn` / `bad` / `gold` / `accent` / `mono`

**进度条颜色 (color)**：`cyan` / `gold` / `ok` / `warn` / `bad` / `accent` / `heal`

**表格用法**：

```python
ui.table(
    columns=[
        {"key": "name", "title": "名称"},
        {"key": "dps", "title": "DPS", "align": "right"},
        {"key": "pct", "title": "占比", "align": "right"},
    ],
    rows=[
        {"name": "玩家A", "dps": "12,345", "pct": "45%"},
        {"name": "玩家B", "dps": "8,901", "pct": "33%"},
    ],
    title="伤害排行",
)
```

**安全限制**：嵌套深度 <= 8，总节点 <= 400，表格行 <= 200，表格列 <= 16，文本长度 <= 4000 字符。

### 窗口与通知

```python
# 弹出独立窗口显示某个面板
ctx.open_window("my_panel", width=600, height=400)

# 请求面板重绘
ctx.request_redraw("my_panel")

# 持久通知（横幅式，需手动关闭或超时）
ctx.notify("警告", "Boss 即将释放大招", duration_s=10.0)

# 关闭持久通知
ctx.dismiss_notify()

# 临时 Toast（自动消失）
ctx.toast("操作完成")
```

### 渲染钩子与叠层

拦截平台已有的渲染表面，或在其上叠加自定义绘制。

```python
# 注册渲染钩子：在目标 surface 渲染前修改数据
hook_token = ctx.register_render_hook(
    "dps_panel",           # 要拦截的 surface 名
    my_hook_fn,            # 钩子函数
    priority=10.0,         # 数值越大越先执行
)

def my_hook_fn(surface, payload):
    """修改渲染数据或注入自定义内容。"""
    payload["custom_field"] = "injected"
    return payload

# 设置叠层（声明式 spec，绘制在 surface 之上）
ctx.set_overlay("dps_panel", ui.panel("Overlay", [...]))

# 清除叠层
ctx.clear_overlay("dps_panel")  # 清除指定 surface
ctx.clear_overlay()              # 清除全部
```

### 引擎访问 (ctx.engine)

通过 `ctx.engine` 访问平台和其他插件注册的引擎实例。

```python
# 获取引擎（不存在返回 None）
tracker = ctx.engine.get("dps_tracker")
bridge = ctx.engine.get("packet_bridge")

# 获取引擎（不存在抛 RuntimeError）
state = ctx.engine.require("game_state")

# 列出可用引擎
names = ctx.engine.available()

# 调用引擎方法
ctx.engine.call("dps_tracker", "reset")

# 注册自己的引擎供其他插件使用
ctx.register_engine("my_custom_engine", my_engine_instance)
```

**常见引擎名**：

| 名称 | 说明 |
|------|------|
| `owner` | 宿主对象 |
| `event_bus` | 事件总线 |
| `plugin_manager` | 插件管理器 |
| `settings` | 全局设置 |
| `game_state` | 游戏状态 |
| `state_manager` | 状态管理器 |
| `dps_tracker` | DPS 追踪器 |
| `history_store` | 历史记录 |
| `encounter_manager` | 战斗管理器 |
| `trigger_engine` | 触发器引擎 |
| `packet_bridge` | 数据包桥接 |
| `memory_bridge` | 内存桥接 |
| `auto_key_engine` | 自动按键引擎 |
| `boss_raid_engine` | Boss 机制引擎 |
| `window_locator` | 窗口定位器 |

### 设置读写

读写 `plugin.json` 中 `settings_schema` 声明的配置。值会持久化。

```python
# 设置默认值（通常在 on_load 中调用）
ctx.set_defaults({
    "sound_enabled": True,
    "refresh_interval": 5,
})

# 读取
enabled = ctx.get_setting("sound_enabled", default=True)

# 写入
ctx.set_setting("refresh_interval", 10)
```

### 定时器与线程

```python
# 重复定时器（后台线程执行）
token = ctx.set_interval(my_callback, seconds=5.0)

# 一次性延迟执行
token = ctx.set_timeout(my_callback, seconds=3.0)

# 取消定时器
ctx.clear_timer(token)

# 在 UI 线程执行（操作 Tk 控件时必须用这个）
ctx.run_on_ui(lambda: update_label("new text"))
```

### 快捷键

注册可由用户在设置中重新绑定的快捷键。

```python
ctx.register_hotkey(
    "toggle",                   # 快捷键 ID
    my_toggle_fn,               # 回调函数
    default_key="CTRL+F12",    # 默认键位
    label="开关我的功能",       # 在设置面板中显示的名称
)
```

注意：纯 `F5`-`F12` 键已被主 UI 占用，插件快捷键建议使用 `CTRL+` 组合键。

### 菜单注册

在平台菜单中添加分类入口。

```python
ctx.register_menu_category(
    "工具",                     # 分类名
    "🔧",                      # 图标
    build_menu,                 # 返回菜单项列表的函数
    priority=10,                # 排序权重
)

def build_menu():
    return [
        {"icon": "▶", "label": "启动", "command": start_fn},
        {"icon": "⏹", "label": "停止", "command": stop_fn},
    ]
```

### 依赖管理

在插件目录下放 `requirements.txt`，在 `on_load` 中调用：

```python
def on_load(ctx):
    ctx.ensure_requirements(install=True)
    # 之后就可以正常 import 了
    import numpy as np
```

**加载顺序**（从高到低优先级）：

1. `vendor/` -- 手动放置的纯 Python 包（离线 / 打包分发推荐）
2. `libs/` -- `pip install --target` 的安装目标（开发时自动下载）
3. 主程序的 site-packages（兜底）

**实践建议**：

- 发布时把依赖打入 `vendor/`，确保用户无需联网安装
- `libs/` 是开发时的缓存目录，不需要提交到版本控制
- 卸载时 `sys.path` 会自动恢复，不会污染其他插件

### 模块加载

加载插件目录内的 Python 模块。

```python
# 加载插件内的引擎模块
module = ctx.load_local("my_engine.py")
engine = module.MyEngine()

# 加载子目录中的模块
parser = ctx.load_local("parsers/my_parser.py")
```

### 日志

```python
ctx.log("状态更新: DPS = 12345")
ctx.log(f"错误: {exc}")
```

日志写入插件独立的缓冲区，可在管理界面查看。

---

## 扩展类型

除了 UI 面板，插件还可以注册以下扩展：

```python
# 数据源（如自定义抓包方式）
ctx.register_data_source("my_source", {"title": "自定义数据源"},
                         start=start_capture, stop=stop_capture)

# 解析器适配器（TCP/内存包解析）
ctx.register_parser_adapter("my_parser", {"title": "自定义解析器"},
                            handler=parse_packet)

# 导出器（报告导出格式）
ctx.register_exporter("csv_export", {"title": "CSV 导出"},
                      handler=export_csv)

# 格式化器（Mini-Parse 格式化）
ctx.register_formatter("custom_fmt", {"title": "自定义格式"},
                       handler=format_fn)

# 触发器类型（告警规则）
ctx.register_trigger_type("hp_low", {"title": "血量过低"},
                          handler=check_hp)

# 报告视图
ctx.register_report_view("timeline", {"title": "时间线"},
                         handler=render_timeline)

# 倒计时器
ctx.register_timer("cd_timer", {"title": "技能冷却"},
                   handler=timer_tick)
```

---

## 完整示例

### 示例一：DPS 监控面板

一个订阅伤害事件、实时显示 DPS 排行的插件。

**plugin.json**:
```json
{
  "id": "dps_monitor",
  "name": "DPS 监控",
  "entry": "plugin.py",
  "enabled": true,
  "capabilities": [
    {
      "id": "ui_panels",
      "title": "DPS 监控",
      "actions": ["reset"],
      "width": 500,
      "height": 400,
      "primary": true
    }
  ]
}
```

**plugin.py**:
```python
import time
from collections import defaultdict

_ctx = None
_damage_total = defaultdict(int)
_start_time = 0.0

def on_load(ctx):
    global _ctx, _start_time
    _ctx = ctx
    _start_time = time.time()

    ctx.register_ui_panel(
        "dps_monitor",
        {"title": "DPS 监控", "width": 500, "height": 400},
        render=_render,
        on_action=_on_action,
    )

    @ctx.on("damage")
    def on_damage(event):
        p = event["payload"]
        actor = p.get("actor", "未知")
        dmg = p.get("damage", 0)
        _damage_total[actor] += dmg
        ctx.request_redraw("dps_monitor")

    ctx.log("DPS 监控已加载")

def on_unload():
    _damage_total.clear()

def _render(payload=None):
    ui = _ctx.ui
    elapsed = max(time.time() - _start_time, 1.0)
    ranked = sorted(_damage_total.items(), key=lambda x: x[1], reverse=True)
    top_dmg = ranked[0][1] if ranked else 1

    rows = []
    for name, total in ranked[:10]:
        dps = total / elapsed
        rows.append({
            "name": name,
            "total": f"{total:,.0f}",
            "dps": f"{dps:,.0f}",
        })

    return ui.panel("DPS 监控", [
        ui.kv("持续时间", f"{elapsed:.0f}s"),
        ui.table(
            columns=[
                {"key": "name", "title": "角色"},
                {"key": "total", "title": "总伤害", "align": "right"},
                {"key": "dps", "title": "DPS", "align": "right"},
            ],
            rows=rows,
        ),
        ui.button("重置", action="reset", style="danger"),
    ])

def _on_action(action_id, payload=None):
    if action_id == "reset":
        global _start_time
        _damage_total.clear()
        _start_time = time.time()
    return _render()
```

### 示例二：带子面板的复杂插件

展示主面板 + 多个子面板（弹出独立窗口）的模式。

**plugin.json**:
```json
{
  "id": "boss_helper",
  "name": "Boss 助手",
  "entry": "plugin.py",
  "enabled": true,
  "game_ids": ["star_resonance"],
  "requires": ["star_resonance"],
  "capabilities": [
    {
      "id": "ui_panels",
      "title": "Boss 助手",
      "actions": ["open_timeline", "open_settings", "toggle_alert"],
      "primary": true
    }
  ],
  "settings_schema": {
    "alert_enabled": {
      "type": "boolean",
      "default": true,
      "description": "Boss 技能预警"
    },
    "alert_advance_s": {
      "type": "integer",
      "default": 3,
      "description": "提前预警秒数"
    }
  }
}
```

**plugin.py**:
```python
_ctx = None
_boss_name = ""
_boss_hp_pct = 1.0
_recent_skills = []

def on_load(ctx):
    global _ctx
    _ctx = ctx
    ctx.set_defaults({"alert_enabled": True, "alert_advance_s": 3})

    # 主面板
    ctx.register_ui_panel(
        "boss_main", {"title": "Boss 助手", "primary": True},
        render=_render_main, on_action=_on_action,
    )
    # 子面板（hidden，通过主面板按钮打开独立窗口）
    ctx.register_ui_panel(
        "boss_timeline",
        {"title": "技能时间线", "width": 700, "height": 500, "hidden": True},
        render=_render_timeline,
    )
    ctx.register_ui_panel(
        "boss_settings",
        {"title": "设置", "width": 400, "height": 300, "hidden": True},
        render=_render_settings, on_action=_on_settings_action,
    )

    ctx.on_boss(_on_boss)
    ctx.register_hotkey("toggle_alert", _toggle_alert,
                        default_key="CTRL+F9", label="Boss 预警开关")
    ctx.log("Boss 助手已加载")

def _on_boss(event):
    global _boss_name, _boss_hp_pct, _recent_skills
    p = event["payload"]
    _boss_name = p.get("name", _boss_name)
    _boss_hp_pct = p.get("hp_pct", _boss_hp_pct)
    skill = p.get("skill_name")
    if skill:
        _recent_skills.append(skill)
        _recent_skills = _recent_skills[-20:]
        if _ctx.get_setting("alert_enabled"):
            _ctx.notify("Boss 技能", skill, duration_s=4.0)
    _ctx.request_redraw("boss_main")

def _toggle_alert():
    current = _ctx.get_setting("alert_enabled", True)
    _ctx.set_setting("alert_enabled", not current)
    _ctx.toast("预警已" + ("关闭" if current else "开启"))
    _ctx.request_redraw("boss_main")

def _render_main(payload=None):
    ui = _ctx.ui
    alert_on = _ctx.get_setting("alert_enabled", True)
    return ui.panel("Boss 助手", [
        ui.section("Boss 状态", [
            ui.kv("名称", _boss_name or "无"),
            ui.bar("血量", pct=_boss_hp_pct, color="bad", caption=f"{_boss_hp_pct:.0%}"),
            ui.badge("预警开启" if alert_on else "预警关闭", "ok" if alert_on else "muted"),
        ]),
        ui.section("最近技能", [
            ui.text(" → ".join(_recent_skills[-5:]) or "暂无", style="mono"),
        ]),
        ui.divider(),
        ui.row([
            ui.button("技能时间线", action="open_timeline"),
            ui.button("设置", action="open_settings"),
            ui.button("预警" + ("关" if alert_on else "开"), action="toggle_alert",
                      style="danger" if alert_on else "primary"),
        ]),
    ])

def _on_action(action_id, payload=None):
    if action_id == "open_timeline":
        _ctx.open_window("boss_timeline", width=700, height=500)
    elif action_id == "open_settings":
        _ctx.open_window("boss_settings", width=400, height=300)
    elif action_id == "toggle_alert":
        _toggle_alert()
    return _render_main()

def _render_timeline(payload=None):
    ui = _ctx.ui
    rows = [{"idx": str(i+1), "skill": s}
            for i, s in enumerate(_recent_skills)]
    return ui.panel("技能时间线", [
        ui.table(
            columns=[
                {"key": "idx", "title": "#"},
                {"key": "skill", "title": "技能"},
            ],
            rows=rows,
        ),
    ])

def _render_settings(payload=None):
    ui = _ctx.ui
    return ui.panel("设置", [
        ui.kv("预警", "开启" if _ctx.get_setting("alert_enabled") else "关闭"),
        ui.kv("提前秒数", str(_ctx.get_setting("alert_advance_s", 3))),
        ui.row([
            ui.button("预警开", action="alert_on", style="primary"),
            ui.button("预警关", action="alert_off"),
        ]),
    ])

def _on_settings_action(action_id, payload=None):
    if action_id == "alert_on":
        _ctx.set_setting("alert_enabled", True)
    elif action_id == "alert_off":
        _ctx.set_setting("alert_enabled", False)
    _ctx.request_redraw("boss_main")
    return _render_settings()
```

### 示例三：使用引擎访问的插件

展示如何访问平台引擎（DPS 追踪器、内存桥接等）。

```python
_ctx = None

def on_load(ctx):
    global _ctx
    _ctx = ctx

    # 列出可用引擎
    ctx.log(f"可用引擎: {ctx.engine.available()}")

    # 获取 DPS 追踪器
    tracker = ctx.engine.get("dps_tracker")
    if tracker:
        ctx.log("DPS 追踪器可用")

    # 获取内存桥接（hybrid / memory 模式下可用）
    mem_bridge = ctx.engine.get("memory_bridge")
    if mem_bridge:
        ctx.log("内存桥接可用")

    # 注册自己的引擎，供其他插件调用
    ctx.register_engine("my_data_provider", MyDataProvider())

    # 订阅快照，用引擎数据增强
    ctx.on_snapshot(_on_snapshot)

def _on_snapshot(event):
    tracker = _ctx.engine.get("dps_tracker")
    if tracker is None:
        return
    # 使用追踪器的数据做自定义处理
    # ...

class MyDataProvider:
    """其他插件可以通过 ctx.engine.get("my_data_provider") 获取这个实例。"""
    def get_data(self):
        return {"example": True}
```

---

## 打包与分发

### ZIP 打包

将插件目录打成 `.zip`，用户通过一键导入安装。

```
my_plugin.zip
└── my_plugin/          # 或者直接在根目录放文件，两种都行
    ├── plugin.json
    ├── plugin.py
    ├── vendor/         # 离线依赖放这里
    └── assets/
```

两种 ZIP 结构都支持：
- 根目录直接包含 `plugin.json`
- 根目录下有一层子目录包含 `plugin.json`

安装后插件会被解压到 `user_plugins/{plugin_id}/`。如果同 ID 插件已存在，会被替换（旧版本的加载状态自动清理）。

### 离线依赖打包

如果插件依赖第三方库，建议打入 `vendor/` 目录：

```bash
pip install --target my_plugin/vendor numpy mido
```

这样用户无需联网即可使用。`vendor/` 中的包优先级高于在线安装。

---

## 故障排查

### 插件没有加载

1. 检查 `plugin.json` 是否存在且 JSON 格式正确
2. 确认 `id` 字段只包含字母、数字、`-`、`.`、`_`
3. 确认 `entry` 指向的文件存在且在插件目录内
4. 检查 `enabled` 是否为 `true`
5. 如果声明了 `requires`，确认依赖插件已加载
6. 查看程序日志，搜索插件 ID 相关的错误信息

### 插件加载后被自动禁用

连续失败 3 次后插件会被自动禁用。检查 `on_load` 中是否有未捕获的异常。常见原因：

- `import` 失败：依赖包缺失，需要在 `on_load` 开头调用 `ctx.ensure_requirements()`
- 访问了尚未就绪的引擎：在 `on_load` 中 `ctx.engine.get()` 可能返回 `None`，要做空值检查

### 面板不显示

1. 确认在 `on_load` 中调用了 `ctx.register_ui_panel()`
2. 确认 `render` 回调返回了有效的 UI spec 字典（用 `ctx.ui` 构建）
3. 确认面板 ID 在 capabilities 的 `actions` 中有声明（如果依赖按钮动作）
4. 如果面板是 `hidden: true`，需要通过 `ctx.open_window()` 打开

### 事件收不到

1. 确认订阅的主题名拼写正确（区分大小写）
2. 确认事件源在运行（如数据抓包已开启）
3. 回调函数是否抛了异常 -- 异常会被平台吞掉，但会计入失败统计
4. 用 `ctx.recent_events(topic="damage")` 检查是否有历史事件

### 依赖安装失败

1. 检查 `requirements.txt` 格式是否正确
2. 开发环境下确认 `pip` 可用
3. 尝试手动 `pip install --target vendor/ -r requirements.txt`
4. 打包分发时将依赖放入 `vendor/` 目录，不依赖在线安装

### 定时器或后台线程问题

- 定时器在后台线程执行，操作 UI 必须用 `ctx.run_on_ui()`
- `on_unload` 后平台会自动清理定时器，但如果自己开了 `threading.Thread`，需要在 `on_disable` / `on_unload` 中手动停止
- 回调要保持精简，重型工作开后台线程

### 快捷键冲突

- 主 UI 已占用 `F5`-`F12`，插件键位建议用 `CTRL+` 组合
- `register_hotkey` 会拒绝与已有快捷键冲突的键位
- 用户可在设置面板中重新绑定插件快捷键
