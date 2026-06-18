// plugin.as — AngelScript 插件示例
//
// C 风格语法, 类型声明可选, 所有平台 SDK 通过 ctx 对象调用:
//   ctx.log("message");
//   ctx.subscribe("damage", callback);
//   ctx.register_ui_panel(id, meta, render, on_action);
//   ctx.set_interval(callback, seconds);
//   ctx.ui_panel(title, children);   // ui 快捷方法
//   ctx.ui_text(text);
//   ctx.ui_button(label, action);
//   ctx.ui_kv(label, value);
//   ctx.ui_section(title, children);
//   ctx.ui_row(children);
//
// 使用纯 Python AngelScript 子集解释器, 零外部依赖。

int click_count = 0;

void on_load(PluginContext@ c)
{
    @ctx = c;
    ctx.log("AngelScript 插件已加载!");
    ctx.set_defaults({"greeting": "你好"});

    ctx.register_ui_panel("as_demo",
        {"title": "AS Demo", "description": "AngelScript 插件示例"},
        @render_panel, @on_panel_action);
}

dictionary@ render_panel(dictionary@ payload)
{
    string greeting = ctx.get_setting("greeting", "你好");
    return ctx.ui_panel("AngelScript Plugin", {
        ctx.ui_section("状态", {
            ctx.ui_kv("语言", "AngelScript (内置解释器)"),
            ctx.ui_kv("问候语", greeting),
            ctx.ui_kv("点击次数", tostring(click_count))
        }),
        ctx.ui_section("操作", {
            ctx.ui_row({
                ctx.ui_button("问候", "greet", "primary"),
                ctx.ui_button("计数 +1", "count")
            })
        })
    });
}

dictionary@ on_panel_action(string action_id, dictionary@ payload)
{
    if (action_id == "greet")
    {
        string greeting = ctx.get_setting("greeting", "你好");
        ctx.notify("AS Plugin", greeting + ", 来自 AngelScript!", 3.0);
    }
    if (action_id == "count")
    {
        click_count = click_count + 1;
        ctx.request_redraw("as_demo");
    }
    return {"ok": true};
}

void on_enable()
{
    ctx.log("AngelScript 插件已启用");
}

void on_disable()
{
    ctx.log("AngelScript 插件已禁用");
}

void on_unload()
{
    ctx.log("AngelScript 插件已卸载");
}

