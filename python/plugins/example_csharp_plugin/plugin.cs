// plugin.cs — C# 插件示例
//
// C# 插件通过 pythonnet 宿主 .NET CLR 运行。
// 两种模式:
//   1. 源码模式 (.cs): 自动使用 dotnet/csc 编译为 .dll 后加载
//   2. 程序集模式 (.dll): 直接加载预编译的 .NET 程序集
//
// ctx 作为 dynamic 对象传入, 可直接调用平台全部 SDK:
//   ctx.log("message");
//   ctx.subscribe("topic", callback);
//   ctx.register_ui_panel(id, metadata, render, on_action);
//   ctx.get_setting(key, default);
//   ctx.notify(title, message);
//
// 需要: pip install pythonnet + .NET 6.0+ 运行时
// 编译需要: .NET SDK (dotnet 命令) 或 csc.exe

using System;
using System.Collections.Generic;

public class Plugin
{
    private dynamic _ctx;
    private int _clickCount = 0;

    public void OnLoad(dynamic ctx)
    {
        _ctx = ctx;
        ctx.log("C# 插件已加载!");

        ctx.set_defaults(new Dictionary<string, object> {
            {"greeting", "你好"}
        });

        ctx.register_ui_panel("cs_demo",
            new Dictionary<string, object> {
                {"title", "C# Demo"},
                {"description", "C# 语言插件示例"}
            },
            new Func<object, object>(RenderPanel),
            new Func<string, object, object>(OnPanelAction)
        );
    }

    public object RenderPanel(object payload)
    {
        string greeting = (string)(_ctx.get_setting("greeting", "你好") ?? "你好");
        return _ctx.ui.panel("C# Plugin", new object[] {
            _ctx.ui.section("状态", new object[] {
                _ctx.ui.kv("语言", "C# (.NET via pythonnet)"),
                _ctx.ui.kv("问候语", greeting),
                _ctx.ui.kv("点击次数", _clickCount.ToString()),
            }),
            _ctx.ui.section("操作", new object[] {
                _ctx.ui.row(new object[] {
                    _ctx.ui.button("问候", "greet", "primary"),
                    _ctx.ui.button("计数 +1", "count"),
                }),
            }),
        });
    }

    public object OnPanelAction(string actionId, object payload)
    {
        if (actionId == "greet")
        {
            string greeting = (string)(_ctx.get_setting("greeting", "你好") ?? "你好");
            _ctx.notify("C# Plugin", greeting + ", 来自 C#!", 3.0);
        }
        else if (actionId == "count")
        {
            _clickCount++;
            _ctx.request_redraw("cs_demo");
        }

        return new Dictionary<string, object> { {"ok", true} };
    }

    public void OnEnable()
    {
        _ctx?.log("C# 插件已启用");
    }

    public void OnDisable()
    {
        _ctx?.log("C# 插件已禁用");
    }

    public void OnUnload()
    {
        _ctx?.log("C# 插件已卸载");
    }
}
