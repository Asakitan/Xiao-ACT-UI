-- plugin.lua — Lua 插件示例
--
-- 真 Lua 5.4 C API 嵌入。ctx 是 userdata + metatable, 支持冒号方法调用:
--   ctx:log("msg")
--   ctx:register_ui_panel(id, meta, render, on_action)
--
-- 老 Python 平台 (via lupa) 的 ctx.log(...) 点号调用也兼容 (compat 层)。

local click_count = 0
local timer_token = ""
local timer_running = false

function on_load(ctx)
    ctx:log("Lua 插件已加载! (C++ 宿主)")
    ctx:set_defaults({greeting = "你好"})

    ctx:register_ui_panel("lua_demo",
        {title = "Lua Demo", description = "Lua 语言插件示例"},
        function(payload) return render_panel(ctx, payload) end,
        function(action_id, payload) return on_panel_action(ctx, action_id, payload) end
    )

    ctx:subscribe("damage", function(event)
        ctx:log("Lua 收到伤害事件")
    end)
end

function render_panel(ctx, payload)
    local greeting = ctx:get_setting("greeting", "你好")
    local status_text = timer_running and "运行中" or "停止"

    return ctx.ui.panel("Lua Plugin", {
        ctx.ui.section("状态", {
            ctx.ui.kv("语言", "Lua 5.4 (真 C API)"),
            ctx.ui.kv("问候语", greeting),
            ctx.ui.kv("点击次数", tostring(click_count)),
            ctx.ui.kv("定时器", status_text),
        }),
        ctx.ui.section("操作", {
            ctx.ui.row({
                ctx.ui.button("问候", "greet", "primary"),
                ctx.ui.button("计数 +1", "count"),
                ctx.ui.button("定时器开关", "timer_toggle"),
            }),
        }),
    })
end

function on_panel_action(ctx, action_id, payload)
    if action_id == "greet" then
        local greeting = ctx:get_setting("greeting", "你好")
        ctx:notify("Lua Plugin", greeting .. ", 来自 Lua!", 3.0)
    elseif action_id == "count" then
        click_count = click_count + 1
        ctx:request_redraw("lua_demo")
    elseif action_id == "timer_toggle" then
        if timer_running then
            ctx:clear_timer(timer_token)
            timer_running = false
        else
            timer_token = ctx:set_interval(function()
                click_count = click_count + 1
                ctx:request_redraw("lua_demo")
            end, 2.0)
            timer_running = true
        end
        ctx:request_redraw("lua_demo")
    end
    return {ok = true}
end

function on_enable(ctx)
    ctx:log("Lua 插件已启用")
end

function on_disable(ctx)
    ctx:log("Lua 插件已禁用")
end

function on_unload(ctx)
    ctx:log("Lua 插件已卸载")
end
