# -*- coding: utf-8 -*-
# Python Plugin Example — 演示 SDK 调用。
#
# 语法和 Python 平台的插件完全一致 —— 目标是把主平台的插件目录直接搬过来
# 就能跑。ctx 是 CPython 里的 SDK proxy 对象, 手写 PyModuleDef 生成。


_click_count = 0
_timer_token = ""
_ctx = None


def on_load(ctx):
    global _ctx
    _ctx = ctx
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
    greeting = _ctx.get_setting("greeting", "你好")
    return _ctx.ui.panel("Python Plugin", [
        _ctx.ui.section("状态", [
            _ctx.ui.kv("语言", "Python 3.11+ (嵌入 CPython)"),
            _ctx.ui.kv("问候语", greeting),
            _ctx.ui.kv("点击次数", str(_click_count)),
        ]),
        _ctx.ui.row([
            _ctx.ui.button("问候", "greet", "primary"),
            _ctx.ui.button("计数 +1", "count"),
        ]),
    ])


def on_panel_action(action_id, payload):
    global _click_count
    if action_id == "greet":
        greeting = _ctx.get_setting("greeting", "你好")
        _ctx.notify("Python Plugin", f"{greeting}, 来自 Python!", 3.0)
    elif action_id == "count":
        _click_count += 1
        _ctx.request_redraw("python_demo")
    return {"ok": True}


def on_hotkey_greet():
    _ctx.notify("Python Plugin", "F6 触发!", 2.0)


def on_enable():
    _ctx.log("Python 插件已启用")


def on_disable():
    _ctx.log("Python 插件已禁用")


def on_unload():
    global _ctx
    _ctx.log("Python 插件已卸载")
    _ctx = None
