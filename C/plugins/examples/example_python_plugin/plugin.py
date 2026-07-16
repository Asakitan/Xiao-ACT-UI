# -*- coding: utf-8 -*-
# Python Plugin Example — 演示 SDK 调用。
#
# 语法和 Python 平台的插件完全一致 —— 目标是把主平台的插件目录直接搬过来
# 就能跑。ctx 是 CPython 里的 SDK proxy 对象, 手写 PyModuleDef 生成。


_click_count = 0
_timer_token = ""


def on_load(ctx):
    ctx.log("Python 插件已加载 (C++ 宿主)")
    ctx.set_defaults({"greeting": "你好"})
    ctx.register_ui_panel(
        "python_demo",
        {"title": "Python Demo", "description": "Python 语言插件示例"},
        render_panel, on_panel_action,
    )
    # 快捷键 F6 (可在设置里改)
    ctx.register_hotkey("greet", on_hotkey_greet, "F6", "Python 示例问候")


def render_panel(payload):
    global _click_count
    greeting = ctx.get_setting("greeting", "你好")
    return ctx.ui.panel("Python Plugin", [
        ctx.ui.section("状态", [
            ctx.ui.kv("语言", "Python 3.11+ (嵌入 CPython)"),
            ctx.ui.kv("问候语", greeting),
            ctx.ui.kv("点击次数", str(_click_count)),
        ]),
        ctx.ui.row([
            ctx.ui.button("问候", "greet", "primary"),
            ctx.ui.button("计数 +1", "count"),
        ]),
    ])


def on_panel_action(action_id, payload):
    global _click_count
    if action_id == "greet":
        greeting = ctx.get_setting("greeting", "你好")
        ctx.notify("Python Plugin", f"{greeting}, 来自 Python!", 3.0)
    elif action_id == "count":
        _click_count += 1
        ctx.request_redraw("python_demo")
    return {"ok": True}


def on_hotkey_greet():
    ctx.notify("Python Plugin", "F6 触发!", 2.0)


def on_enable():
    ctx.log("Python 插件已启用")


def on_disable():
    ctx.log("Python 插件已禁用")


def on_unload():
    ctx.log("Python 插件已卸载")
