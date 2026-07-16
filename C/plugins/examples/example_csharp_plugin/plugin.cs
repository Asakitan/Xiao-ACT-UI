// plugin.cs — C# 插件示例
//
// hostfxr 嵌入 .NET 8 runtime, Roslyn 内存编译。ctx 是 dynamic (走 native
// P/Invoke, 免大量 wrapper 类), 姿势和 Python 一致。

using System;
using System.Collections.Generic;

public class Plugin
{
    private dynamic _ctx;
    private int _clickCount = 0;
    private string _timerToken = "";
    private bool _timerRunning = false;

    public void OnLoad(dynamic ctx)
    {
        _ctx = ctx;
        ctx.log("C# 插件已加载! (hostfxr 宿主)");
        ctx.set_defaults(new Dictionary<string, object> { { "greeting", "你好" } });

        ctx.register_ui_panel(
            "csharp_demo",
            new Dictionary<string, object> {
                { "title", "C# Demo" },
                { "description", "C# 语言插件示例" }
            },
            new Func<object, object>(RenderPanel),
            new Func<string, object, object>(OnPanelAction)
        );

        ctx.subscribe("damage", new Action<object>(evt => {
            _ctx.log("C# 收到伤害事件");
        }));
    }

    public object RenderPanel(object payload)
    {
        string greeting = (string)_ctx.get_setting("greeting", "你好");
        string statusText = _timerRunning ? "运行中" : "停止";

        return _ctx.ui.panel("C# Plugin", new object[] {
            _ctx.ui.section("状态", new object[] {
                _ctx.ui.kv("语言", "C# (hostfxr + Roslyn)"),
                _ctx.ui.kv("问候语", greeting),
                _ctx.ui.kv("点击次数", _clickCount.ToString()),
                _ctx.ui.kv("定时器", statusText),
            }),
            _ctx.ui.section("操作", new object[] {
                _ctx.ui.row(new object[] {
                    _ctx.ui.button("问候", "greet", "primary"),
                    _ctx.ui.button("计数 +1", "count"),
                    _ctx.ui.button("定时器开关", "timer_toggle"),
                }),
            }),
        });
    }

    public object OnPanelAction(string actionId, object payload)
    {
        if (actionId == "greet")
        {
            string greeting = (string)_ctx.get_setting("greeting", "你好");
            _ctx.notify("C# Plugin", $"{greeting}, 来自 C#!", 3.0);
        }
        else if (actionId == "count")
        {
            _clickCount += 1;
            _ctx.request_redraw("csharp_demo");
        }
        else if (actionId == "timer_toggle")
        {
            if (_timerRunning)
            {
                _ctx.clear_timer(_timerToken);
                _timerRunning = false;
            }
            else
            {
                _timerToken = (string)_ctx.set_interval(new Action(() => {
                    _clickCount += 1;
                    _ctx.request_redraw("csharp_demo");
                }), 2.0);
                _timerRunning = true;
            }
            _ctx.request_redraw("csharp_demo");
        }
        return new Dictionary<string, object> { { "ok", true } };
    }

    public void OnEnable()
    {
        _ctx.log("C# 插件已启用");
    }

    public void OnDisable()
    {
        if (_timerRunning)
        {
            _ctx.clear_timer(_timerToken);
            _timerRunning = false;
        }
        _ctx.log("C# 插件已禁用");
    }

    public void OnUnload()
    {
        _ctx.log("C# 插件已卸载");
    }
}
