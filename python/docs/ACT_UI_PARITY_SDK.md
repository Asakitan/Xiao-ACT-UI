# ACT UI 对等 SDK

> 版本 `5.0.0`。配套文档：`ACT_PLATFORM.md`、`PLUGIN_SDK.md`。

## 概述

SAO ACT UI 提供三套 UI 表面（WebView、Entity/Tk、AI Editor）。插件通过声明式 API 注册面板，平台负责在所有表面上渲染。

## 对等原则

1. 任何 ACT 功能在所有 UI 表面都能访问同一个共享后端
2. 插件用 `ctx.register_ui_panel()` 注册，不直接操作 Tk/HTML
3. 声明式 UI spec（`ctx.ui`）定义面板内容，平台分别用 Tk 和 HTML 渲染
4. 面板动作通过 `on_action(action, payload)` 回调统一处理

## 声明式 UI

```python
def render_my_panel(payload):
    return ctx.ui.panel("My Panel", [
        ctx.ui.section("Status", [
            ctx.ui.kv("HP", f"{hp}/{max_hp}"),
            ctx.ui.bar("HP", pct=hp/max_hp, color="ok"),
            ctx.ui.button("Reset", action="reset", style="danger"),
        ]),
        ctx.ui.table(columns=["Name", "DPS"], rows=rows),
    ])
```

### 节点类型

**容器：** `panel`、`section`、`card`、`row`、`group`

**叶子：** `text`、`kv`、`bar`、`badge`、`divider`、`spacer`、`button`、`input`、`table`、`canvas`

### 限制

| 限制 | 值 |
|------|---|
| MAX_DEPTH | 8 |
| MAX_NODES | 400 |
| MAX_TABLE_ROWS | 200 |
| MAX_TEXT_LEN | 4000 |

## 面板注册

```python
ctx.register_ui_panel(
    "my_dps",
    {
        "title": "DPS Monitor",
        "description": "实时DPS面板",
        "width": 400, "height": 300,
        "hidden": False,     # True = 不独立列出，由主面板呼出
        "primary": False,    # True = 启动时自动打开
    },
    render=render_my_dps,
    on_action=on_dps_action,
)
```

### 渲染器

```python
def render_my_dps(payload):
    """payload 包含面板状态，返回 ui spec dict。"""
    return ctx.ui.panel("DPS", [...])
```

### 动作处理

```python
def on_dps_action(action, payload):
    """action = 按钮的 action 字符串，payload 含 inputs 等。"""
    if action == "reset":
        reset_dps()
        ctx.request_redraw("my_dps")
```

## 渲染钩子

插件可拦截/叠加任意 UI surface 的渲染：

```python
ctx.register_render_hook("dps_overlay", my_hook)
ctx.set_overlay("hp_overlay", overlay_data)
```

## 菜单注册

```python
ctx.register_menu_category('Boss', '⚔', build_boss_items, priority=20)

def build_boss_items():
    return [
        {'icon': '◆', 'label': 'Boss血条: ON', 'command': toggle_boss_hp},
        {'icon': '⚙', 'label': 'Raid配置', 'command': open_raid_editor},
    ]
```

## 独立窗口

```python
ctx.open_window("my_dps", width=800, height=600)
```

平台在 Tk 模式下打开 Toplevel，WebView 模式下打开新 webview 窗口。

## 主题

面板自动跟随 ACT 主题（dark/light）。通过 `panel_themes.act` 设置切换。

| 模式 | Tk | WebView | AI Editor |
|------|---|---------|-----------|
| Dark | `_SAO_PANEL_*` 常量 | `act_panel_theme.css` | `[data-theme="dark"]` |
| Light | `_apply_sao_panel_palette('light')` | CSS 变量切换 | `[data-theme="light"]` |

## 星痕共鸣面板清单

参考插件注册的面板：

| 面板 | 类型 | 说明 |
|------|------|------|
| DPS | overlay | 实时DPS表 |
| BossHP | overlay | Boss血条 |
| HP | overlay | 自身HP |
| Alert | overlay | 提醒弹窗 |
| SkillFX | overlay | 技能特效 |
| BuffMon | overlay | Buff监视 |
| AutoKey | panel | 自动按键配置 |
| BossRaid | panel | Boss机制编辑器 |
| Commander | panel | 指挥面板 |
| TriggerTimer | panel | 触发器管理 |
| DeathRecap | panel | 死亡回放 |
| CombatantDrilldown | panel | 战斗者钻取 |
| SkillDrilldown | panel | 技能钻取 |
| GraphTimeseries | panel | 时间线图表 |

## 验证

```bash
python -m tools.act_ui_parity
python -m unittest tools.act_ui_parity_selftest
```
