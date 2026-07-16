// plugin.as — AngelScript 插件示例
//
// C 风格静态类型, 真 asIScriptEngine 嵌入。ctx 是强类型的 PluginContext@ handle。

PluginContext@ ctx;
int click_count = 0;

void on_load(PluginContext@ c)
{
    @ctx = c;
    ctx.log("AngelScript 插件已加载! (C++ 宿主)");

    dictionary meta;
    meta["title"] = "AS Demo";
    meta["description"] = "AngelScript 语言插件示例";

    ctx.register_ui_panel("as_demo", meta,
        @render_panel, @on_panel_action);
}

dictionary@ render_panel(dictionary@ payload)
{
    string greeting = ctx.get_setting_str("greeting", "你好");

    array<UiNode@> statusItems = {
        ctx.ui_kv("语言", "AngelScript (asIScriptEngine)"),
        ctx.ui_kv("问候语", greeting),
        ctx.ui_kv("点击次数", "" + click_count),
    };

    array<UiNode@> buttons = {
        ctx.ui_button("问候", "greet", "primary"),
        ctx.ui_button("计数 +1", "count"),
    };

    array<UiNode@> sections = {
        ctx.ui_section("状态", statusItems),
        ctx.ui_row(buttons),
    };

    return ctx.ui_panel("AngelScript Plugin", sections);
}

dictionary@ on_panel_action(string action_id, dictionary@ payload)
{
    if (action_id == "greet")
    {
        string greeting = ctx.get_setting_str("greeting", "你好");
        ctx.notify("AS Plugin", greeting + ", 来自 AngelScript!", 3.0);
    }
    else if (action_id == "count")
    {
        click_count += 1;
        ctx.request_redraw("as_demo");
    }
    dictionary d;
    d["ok"] = true;
    return d;
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
