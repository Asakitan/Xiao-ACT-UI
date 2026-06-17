# 插件 UI 面板开发指南

> 配套文档：`PLUGIN_SDK.md`（插件生命周期与完整 API）、`ACT_PLATFORM.md`（平台架构）。

本文面向**插件开发者**，介绍如何用声明式 UI 为你的插件添加可视面板。你只需要描述面板长什么样，平台会自动在 Tk 和 WebView 两个渲染器上画出一致的界面。

---

## 快速上手：一个完整的插件面板

以下是一个最小但完整的插件，包含一个带状态显示和按钮交互的面板。

**plugin.json**
```json
{
  "id": "my_counter",
  "name": "计数器",
  "version": "1.0.0",
  "entry": "plugin.py",
  "enabled": true,
  "capabilities": [
    {"id": "ui_panels", "title": "计数器面板", "actions": ["reset"]}
  ]
}
```

**plugin.py**
```python
_ctx = None
_count = 0

def on_load(ctx):
    global _ctx
    _ctx = ctx

    # 1. 注册面板：告诉平台你的面板长什么样、怎么响应交互
    ctx.register_ui_panel(
        "counter",                              # 面板 ID（插件内唯一）
        {
            "title": "计数器",                   # 显示标题
            "description": "一个简单的点击计数器",
            "width": 360, "height": 240,         # 首选窗口尺寸
        },
        render=_render,                          # 渲染函数
        on_action=_on_action,                    # 交互回调
    )

def _render(_payload=None):
    """每次面板需要重绘时调用，返回 UI 描述树。"""
    ui = _ctx.ui
    return ui.panel("计数器", [
        ui.section("状态", [
            ui.kv("当前计数", str(_count)),
            ui.bar("进度", pct=min(_count / 100, 1.0), color="cyan",
                   caption=f"{_count}/100"),
            ui.badge("已完成" if _count >= 100 else "进行中",
                     "ok" if _count >= 100 else "accent"),
        ]),
        ui.row([
            ui.button("+1", action="increment", style="primary"),
            ui.button("重置", action="reset", style="danger"),
        ]),
    ])

def _on_action(action_id, payload=None):
    """用户点击按钮时调用。action_id 就是按钮的 action 字符串。"""
    global _count
    if action_id == "increment":
        _count += 1
    elif action_id == "reset":
        _count = 0
    _ctx.request_redraw("counter")  # 通知平台重新拉取渲染
    return _render()                # 也可以直接返回新 spec 立即重绘

def on_enable(ctx):
    pass

def on_disable(ctx):
    pass
```

这个例子在 Tk 和 WebView 中效果一致——你不需要写任何 HTML 或 Tk 代码。

---

## 核心概念

### 工作原理

```
插件 render()  →  UI Spec (纯 dict)  →  平台归一化  →  Tk 渲染器 / WebView 渲染器
                                              ↑                   ↓
                                        安全裁剪             屏幕上的 UI
                                        (深度/节点数/文本长度限制)
```

1. 平台调用你的 `render()` 函数获取一棵 UI 描述树
2. 描述树经过归一化和安全裁剪
3. 两个渲染器各自从同一棵树渲染出原生 UI

你不需要关心当前运行在哪个渲染器上——写一次 spec，两端都能画。

### 面板注册

```python
ctx.register_ui_panel(panel_id, metadata, render, on_action)
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `panel_id` | `str` | 插件内唯一标识，如 `"settings"`、`"monitor"` |
| `metadata` | `dict` | 面板元数据（见下表） |
| `render` | `callable` | `render(payload) -> ui_spec`，返回面板内容 |
| `on_action` | `callable` | `on_action(action_id, payload) -> result`，处理按钮点击 |

**metadata 字段：**

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `title` | `str` | `""` | 面板显示名 |
| `description` | `str` | `""` | 面板说明 |
| `width` | `int` | 平台默认 | 首选窗口宽度 (px) |
| `height` | `int` | 平台默认 | 首选窗口高度 (px) |
| `min_width` | `int` | 0 | 最小窗口宽度 |
| `min_height` | `int` | 0 | 最小窗口高度 |
| `hidden` | `bool` | `False` | 隐藏面板，不在面板列表出现，由主面板通过 `ctx.open_window()` 呼出 |
| `primary` | `bool` | `False` | 主面板，启动时自动打开 |

### 多面板插件

一个插件可以注册多个面板。典型模式是一个主面板 + 若干子面板：

```python
def on_load(ctx):
    ctx.register_ui_panel("main", {"title": "我的插件", "primary": True},
                          render=_render_main, on_action=_on_main)
    ctx.register_ui_panel("settings", {"title": "设置", "hidden": True},
                          render=_render_settings, on_action=_on_settings)

def _on_main(action_id, payload):
    if action_id == "open_settings":
        _ctx.open_window("settings", width=500, height=400)
```

---

## 节点类型详解

### 容器节点

容器用来组织布局，可以嵌套（最大深度 8 层）。

#### `panel(title, children)`

面板的根容器。顶层 panel 会被解包——标题升为面板标题，children 成为面板内容。

```python
ui.panel("我的面板", [
    ui.text("Hello"),
    ui.kv("版本", "1.0"),
])
```

#### `section(title, children, accent="cyan")`

带标题和左侧色条的分组框。适合把面板分成几个视觉区域。

```
┌─ 状态 ────────────────────────┐
│▋ HP:  2400/3000               │
│▋ MP:  800/1200                │
└───────────────────────────────┘
```

```python
ui.section("状态", [
    ui.kv("HP", "2400/3000"),
    ui.kv("MP", "800/1200"),
], accent="ok")       # 左侧色条颜色，默认 cyan
```

`accent` 可选值：`cyan`(默认), `gold`, `ok`, `warn`, `bad`, `accent`, `heal`

#### `card(title, children)`

和 section 类似的带框容器，但没有色条。适合信息卡片。

```python
ui.card("玩家信息", [
    ui.kv("名字", "Alice"),
    ui.kv("等级", "60"),
])
```

#### `row(children, align="left")`

水平排列子节点。按钮组、状态徽章行等横向布局都用它。

```
[ 启动 ] [ 停止 ] [ 重置 ]
```

```python
ui.row([
    ui.button("启动", action="start", style="primary"),
    ui.button("停止", action="stop", style="danger"),
    ui.button("重置", action="reset"),
])
```

`align`：`left`(默认), `center`, `right`

#### `group(children)`

无框无标题的逻辑分组。纯粹用于组织子节点，不产生额外视觉元素。

---

### 显示节点

#### `text(text, style="value", align="left")`

纯文本。

```
这是一段说明文字
```

```python
ui.text("这是一段说明文字", style="muted")
ui.text("重要提示", style="bad", align="center")
```

**style 可选值及视觉效果：**

| style | 说明 | 效果 |
|-------|------|------|
| `title` | 标题 | 大号、高亮 |
| `subtitle` | 副标题 | 略小、次高亮 |
| `value` | 默认值 | 正常亮度 |
| `label` | 标签 | 偏暗的说明文字 |
| `muted` | 弱化 | 最暗的灰色文字 |
| `ok` | 正常/成功 | 绿色 |
| `warn` | 警告 | 金黄色 |
| `bad` | 危险/错误 | 红色 |
| `gold` | 金色高亮 | 金色 |
| `accent` | 强调 | 青色 (cyan) |
| `mono` | 等宽 | 等宽字体 |

#### `title(text)`

`text(text, style="title")` 的快捷写法。

#### `kv(label, value, style="value")`

键值对——左侧标签、右侧值。状态面板最常用的节点。

```
HP        2400/3000
MP        800/1200
职业      剑士
```

```python
ui.kv("HP", f"{hp}/{max_hp}", style="ok"),
ui.kv("MP", f"{mp}/{max_mp}", style="accent"),
ui.kv("职业", role_name),
```

#### `bar(label, pct, color="cyan", caption="")`

进度条。`pct` 范围 0.0~1.0。

```
HP  ████████████░░░░░░░░  75%  2400/3200
```

```python
ui.bar("HP", pct=hp/max_hp, color="ok", caption=f"{hp}/{max_hp}")
ui.bar("Boss", pct=boss_pct, color="bad", caption=f"{boss_pct:.0%}")
```

`color` 可选值：`cyan`(默认), `gold`, `ok`(绿), `warn`(黄), `bad`(红), `accent`, `heal`

#### `badge(text, style="muted")`

内联标签/徽章，带圆角边框。适合状态标记。

```
[运行中]  [MEM]  [已断开]
```

```python
ui.badge("运行中", "ok"),
ui.badge("MEM", "accent"),
ui.badge("已断开", "bad"),
```

`style` 可选值：`muted`, `ok`, `warn`, `bad`, `gold`, `accent`

#### `divider()`

水平分割线。

```python
ui.divider()
```

#### `spacer(size=8)`

空白间距，单位 px，范围 0~64。

```python
ui.spacer(16)
```

---

### 交互节点

#### `button(label, action, style="default", payload=None, disabled=False)`

按钮。点击后触发 `on_action(action, payload)` 回调。

```python
ui.button("启动", action="start", style="primary")
ui.button("删除", action="delete", style="danger", disabled=not can_delete)
ui.button("导出", action="export", payload={"format": "csv"})
```

**style 视觉效果：**

| style | 说明 |
|-------|------|
| `default` | 普通按钮，灰色边框 |
| `primary` | 主要操作，青色/蓝色高亮 |
| `danger` | 危险操作，红色 |
| `ghost` | 幽灵按钮，无边框，hover 才显示 |

**payload** 是附加数据，会原样传给 `on_action`。

#### `input(id, value="", placeholder="", input_type="text", width=0)`

文本输入框。用户输入的值在按钮点击时通过 `payload["inputs"][id]` 传递。

```python
ui.section("搜索", [
    ui.input("keyword", placeholder="输入关键词..."),
    ui.input("min_dps", value="1000", input_type="number", width=100),
    ui.button("搜索", action="search", style="primary"),
])
```

| 参数 | 说明 |
|------|------|
| `id` | 输入框标识，在 `payload["inputs"]` 中作为 key |
| `value` | 默认值 |
| `placeholder` | 占位提示文字 |
| `input_type` | `"text"`, `"number"`, `"password"` |
| `width` | 固定宽度 (px)；0 表示自动填满 |

**获取用户输入：**

```python
def _on_action(action_id, payload):
    if action_id == "search":
        keyword = payload.get("inputs", {}).get("keyword", "")
        min_dps = payload.get("inputs", {}).get("min_dps", "0")
        # ... 使用 keyword 和 min_dps
```

> 渲染器会保护用户正在编辑的输入框——即使面板重绘，只要用户仍在输入，文本不会被服务端的值覆盖。

---

### 数据节点

#### `table(columns, rows, highlight_key="", title="")`

数据表格。

```
 名字        DPS       占比
 Alice       12,345    45.2%   ← 高亮行（金色加粗）
 Bob          8,901    32.6%
 Carol        6,078    22.2%
```

```python
ui.table(
    columns=[
        {"key": "name", "title": "名字"},
        {"key": "dps",  "title": "DPS",  "align": "right"},
        {"key": "pct",  "title": "占比", "align": "right"},
    ],
    rows=[
        {"name": "Alice", "dps": "12,345", "pct": "45.2%", "is_self": True},
        {"name": "Bob",   "dps": "8,901",  "pct": "32.6%"},
        {"name": "Carol", "dps": "6,078",  "pct": "22.2%"},
    ],
    highlight_key="is_self",   # 该字段为 truthy 的行高亮显示
    title="DPS 排行",          # 可选表头标题
)
```

**columns** 支持两种写法：
```python
# 完整写法
columns=[{"key": "name", "title": "名字", "align": "left"}]

# 简写——key 和 title 相同
columns=["name", "dps", "pct"]
```

每列 `align`：`left`(默认), `center`, `right`。

限制：最多 200 行、16 列。

---

### 画布节点（高级）

#### `canvas(width, height, ops=[], bg="body")`

自由绘图画布，用于钢琴键盘、波形图等需要自定义绘制的场景。

```python
ui.canvas(width=300, height=100, bg="body", ops=[
    # 矩形
    {"op": "rect", "x": 10, "y": 10, "w": 80, "h": 40,
     "fill": "accent", "outline": "border"},
    # 椭圆
    {"op": "oval", "x": 100, "y": 10, "w": 40, "h": 40,
     "fill": "ok"},
    # 线条
    {"op": "line", "x1": 0, "y1": 80, "x2": 300, "y2": 80,
     "fill": "muted", "width": 1},
    # 文字
    {"op": "text", "x": 150, "y": 50, "text": "Hello",
     "fill": "title", "size": 14, "anchor": "center", "bold": True},
])
```

**绘图操作：**

| 操作 | 参数 |
|------|------|
| `rect` | `x, y, w, h, fill, outline, width` |
| `oval` | `x, y, w, h, fill, outline, width` |
| `line` | `x1, y1, x2, y2, fill, width` |
| `text` | `x, y, text, fill, size, anchor, bold` |

**颜色**可以是主题色标记（`accent`, `ok`, `warn`, `bad`, `gold`, `title`, `value`, `muted`, `mono`, `body`, `border`, `sep` 等）或十六进制值（`#ff0000`, `#0af`）。

**anchor**（文字锚点）：`nw`, `n`, `ne`, `w`, `center`, `e`, `sw`, `s`, `se`。

限制：最大尺寸 4096x4096，最多 4000 个绘图操作。

---

## 用户交互处理

### 按钮 + 输入框联动

最常见的交互模式是输入框 + 按钮：用户填写输入框，点击按钮提交。

```python
def _render(_payload=None):
    ui = _ctx.ui
    return ui.panel("配置", [
        ui.section("阈值设置", [
            ui.kv("当前阈值", str(_threshold)),
            ui.input("new_threshold", value=str(_threshold),
                     input_type="number", placeholder="输入新阈值"),
        ]),
        ui.row([
            ui.button("应用", action="apply", style="primary"),
            ui.button("恢复默认", action="reset_default"),
        ]),
    ])

def _on_action(action_id, payload):
    global _threshold
    if action_id == "apply":
        inputs = payload.get("inputs", {})
        try:
            _threshold = int(inputs.get("new_threshold", _threshold))
        except ValueError:
            pass
    elif action_id == "reset_default":
        _threshold = 1000
    _ctx.request_redraw("config")
```

### 按钮携带数据

按钮可以通过 `payload` 携带额外数据，适合列表中每一项对应不同操作：

```python
for item in items:
    children.append(ui.row([
        ui.text(item["name"]),
        ui.button("删除", action="delete", style="danger",
                  payload={"item_id": item["id"]}),
    ]))

# on_action 中
def _on_action(action_id, payload):
    if action_id == "delete":
        item_id = payload.get("item_id")
        _remove_item(item_id)
```

### 触发重绘

当数据变化需要更新面板时：

```python
# 方式 1：on_action 返回新的 spec，立即重绘
def _on_action(action_id, payload):
    _do_something()
    return _render()

# 方式 2：任意时刻请求重绘（异步数据更新时）
_ctx.request_redraw("my_panel", reason="data updated")
```

平台以约 700ms 间隔轮询重绘事件。

---

## 主题适配

面板自动跟随平台的暗色/亮色主题切换。

### 自动适配（大多数情况）

如果你只使用 `ctx.ui` 构建面板，**不需要做任何额外工作**——所有节点的颜色会自动跟随主题。这包括：
- 文本颜色（通过 style token 如 `title`, `muted`, `ok` 等）
- 进度条颜色
- 徽章颜色
- 背景色和边框色

### 颜色系统

面板中所有颜色都通过**语义色标记**（token）指定，不使用硬编码颜色值：

| Token | 亮色模式 | 暗色模式 | 用途 |
|-------|---------|---------|------|
| `title` | 深色文字 | 亮白色 | 标题、强调 |
| `value` | 正常文字 | 正常亮色 | 默认内容 |
| `label` | 偏暗 | 偏暗蓝灰 | 说明标签 |
| `muted` | 灰色 | 暗灰蓝 | 次要信息 |
| `ok` | 绿色 | 亮绿 | 正常/成功 |
| `warn` | 金黄 | 亮金 | 警告 |
| `bad` | 红色 | 亮粉红 | 错误/危险 |
| `gold` | 金色 | 亮金 | 高亮/自身 |
| `accent` / `cyan` | 青色 | 青色 | 品牌强调色 |

### Canvas 画布中的颜色

画布节点同样优先使用 token：

```python
# 推荐：使用 token，自动适配主题
{"op": "rect", "fill": "accent", "outline": "border"}
{"op": "text", "fill": "title", "size": 12}

# 也可以用十六进制，但不会跟随主题切换
{"op": "rect", "fill": "#ff0000"}
```

画布专用 token：`body`（背景色）、`border`（边框色）、`sep`（分隔线色）、`grid`（网格色）、`header`（表头色）、`transparent`。

### Web 端 CSS 变量（高级）

如果你通过渲染钩子注入自定义 HTML，可以引用平台的 CSS 自定义属性：

```css
/* 这些变量在亮色/暗色主题下自动切换 */
var(--act-text)          /* 正文颜色 */
var(--act-muted)         /* 弱化文字 */
var(--act-card-bg)       /* 卡片背景 */
var(--act-edge)          /* 边框 */
var(--act-cyan)          /* 品牌强调色 */
var(--act-gold)          /* 金色 */
var(--act-ok)            /* 绿色 */
var(--act-danger)        /* 红色 */
var(--act-viewport-bg)   /* 页面底色 */
var(--act-shadow)        /* 阴影 */
```

暗色主题通过 `[data-act-theme="dark"]` 属性选择器自动切换所有变量。

---

## 渲染钩子与叠加层（高级）

除了注册独立面板，插件还可以在已有的 UI 上叠加内容或拦截渲染。

### 叠加层 (Overlay)

在任意 UI surface 上方叠加一层你的内容，不影响原有渲染：

```python
# 在某个 surface 上叠加显示
_ctx.set_overlay("dps_table", ctx.ui.panel("提醒", [
    ctx.ui.badge("PVP 模式", "warn"),
]))

# 清除叠加
_ctx.clear_overlay("dps_table")
```

### 渲染钩子 (Render Hook)

拦截 surface 的渲染流程，修改数据或完全接管绘制：

```python
def _my_hook(surface, payload):
    # 修改 payload 中的数据
    payload["custom_field"] = "injected"
    return payload

token = _ctx.register_render_hook("some_surface", _my_hook, priority=10.0)

# 不再需要时取消
# ctx.unregister_hook(token)
```

如果要完全接管某个 surface 的渲染：

```python
from act_platform.render_hooks import OVERRIDE_KEY

def _takeover_hook(surface, payload):
    payload[OVERRIDE_KEY] = _ctx.ui.panel("替代内容", [
        _ctx.ui.text("这个 surface 被插件接管了"),
    ])
    return payload
```

---

## 菜单注册

在平台菜单中添加你的插件菜单项：

```python
ctx.register_menu_category("我的工具", "⚙", _build_menu, priority=10.0)

def _build_menu():
    return [
        {"icon": "▶", "label": "启动监控", "command": _start_monitor},
        {"icon": "⏹", "label": "停止监控", "command": _stop_monitor},
        {"icon": "⚙", "label": "打开设置", "command": lambda: _ctx.open_window("settings")},
    ]
```

---

## 独立窗口

将面板弹出为独立窗口：

```python
ctx.open_window("my_panel", width=800, height=600)
```

平台根据当前渲染模式自动选择 Tk Toplevel 或 WebView 窗口。

---

## 安全限制

插件输出的 UI spec 经过平台归一化，自动裁剪以防止恶意或错误的 spec 影响性能：

| 限制 | 值 |
|------|---|
| 最大嵌套深度 | 8 层 |
| 最大节点数 | 400 |
| 表格最大行数 | 200 |
| 表格最大列数 | 16 |
| 文本最大长度 | 4000 字符 |
| 标题最大长度 | 200 字符 |
| 输入框值最大长度 | 2000 字符 |
| 画布最大尺寸 | 4096 x 4096 px |
| 画布最大操作数 | 4000 |

超出限制的部分会被静默截断，不会抛出异常。

---

## 常用模式

### 定时刷新面板

```python
def on_load(ctx):
    ctx.register_ui_panel("live", {...}, render=_render, on_action=_on_action)
    ctx.set_interval(_tick, 1000)  # 每秒刷新

def _tick():
    _update_data()
    _ctx.request_redraw("live")
```

### 条件渲染

```python
def _render(_payload=None):
    ui = _ctx.ui
    children = []
    if _is_connected:
        children.append(ui.badge("已连接", "ok"))
        children.append(ui.kv("延迟", f"{_latency}ms"))
    else:
        children.append(ui.badge("未连接", "bad"))
        children.append(ui.button("重连", action="reconnect", style="primary"))
    return ui.panel("网络状态", children)
```

### 动态表格

```python
def _render(_payload=None):
    ui = _ctx.ui
    rows = [
        {"name": p.name, "dps": f"{p.dps:,.0f}", "is_self": p.is_self}
        for p in _players
    ]
    return ui.panel("DPS", [
        ui.table(
            columns=[
                {"key": "name", "title": "名字"},
                {"key": "dps", "title": "DPS", "align": "right"},
            ],
            rows=rows,
            highlight_key="is_self",
        ),
    ])
```

---

## API 速查

### ctx.ui 构建器

| 方法 | 用途 |
|------|------|
| `ui.panel(title, children)` | 面板根容器 |
| `ui.section(title, children, accent)` | 带色条的分组框 |
| `ui.card(title, children)` | 信息卡片 |
| `ui.row(children, align)` | 水平排列 |
| `ui.group(children)` | 逻辑分组 |
| `ui.text(text, style, align)` | 文本 |
| `ui.title(text)` | 标题文本 |
| `ui.kv(label, value, style)` | 键值对 |
| `ui.bar(label, pct, color, caption)` | 进度条 |
| `ui.badge(text, style)` | 徽章标签 |
| `ui.divider()` | 分割线 |
| `ui.spacer(size)` | 空白间距 |
| `ui.button(label, action, style, payload, disabled)` | 按钮 |
| `ui.input(id, value, placeholder, input_type, width)` | 输入框 |
| `ui.table(columns, rows, highlight_key, title)` | 数据表格 |
| `ui.canvas(width, height, ops, bg)` | 自由画布 |

### ctx 面板相关方法

| 方法 | 用途 |
|------|------|
| `ctx.register_ui_panel(id, meta, render, on_action)` | 注册面板 |
| `ctx.request_redraw(panel_id, reason)` | 请求重绘 |
| `ctx.open_window(panel_id, width, height)` | 弹出独立窗口 |
| `ctx.register_render_hook(surface, callback, priority)` | 注册渲染钩子 |
| `ctx.set_overlay(surface, spec)` | 设置叠加层 |
| `ctx.clear_overlay(surface)` | 清除叠加层 |
| `ctx.register_menu_category(name, icon, builder, priority)` | 注册菜单 |
