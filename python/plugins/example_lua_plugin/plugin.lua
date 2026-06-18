-- plugin.lua — Lua 插件示例
--
-- 所有平台 SDK 方法都可通过 ctx 对象调用 (冒号语法):
--   ctx:log("message")
--   ctx:subscribe("damage", callback)
--   ctx:register_ui_panel(id, meta, render, on_action)
--   ctx:set_interval(callback, seconds)
--   ctx:get_setting(key, default)
--   ctx:notify(title, message)
--   ... 与 Python 插件完全一致
--
-- Lua table 自动转换为 Python dict/list, 反之亦然。
-- 需要安装 lupa: pip install lupa

local _ctx = nil
local click_count = 0

function on_load(ctx)
    _ctx = ctx
    ctx:log("Lua 插件已加载!")

    ctx:set_defaults({greeting = "你好"})

    ctx:register_ui_panel("lua_demo",
        {title = "Lua Demo", description = "Lua 语言插件示例"},
        render_panel,
        on_panel_action
    )

    ctx:subscribe("damage", function(event)
        ctx:log("Lua 收到伤害事件: " .. tostring(event.amount or 0))
    end)
end

function render_panel(payload)
    local greeting = _ctx:get_setting("greeting", "你好")
    return _ctx.ui.panel("Lua Plugin", {
        _ctx.ui.section("状态", {
            _ctx.ui.kv("语言", "Lua (via lupa)"),
            _ctx.ui.kv("问候语", greeting),
            _ctx.ui.kv("点击次数", tostring(click_count)),
        }),
        _ctx.ui.section("操作", {
            _ctx.ui.row({
                _ctx.ui.button("问候", "greet", "primary"),
                _ctx.ui.button("计数 +1", "count"),
            }),
        }),
    })
end

function on_panel_action(action_id, payload)
    if action_id == "greet" then
        local greeting = _ctx:get_setting("greeting", "你好")
        _ctx:notify("Lua Plugin", greeting .. ", 来自 Lua!", 3.0)
    elseif action_id == "count" then
        click_count = click_count + 1
        _ctx:request_redraw("lua_demo")
    end
    return {ok = true}
end

function on_enable()
    if _ctx then _ctx:log("Lua 插件已启用") end
end

function on_disable()
    if _ctx then _ctx:log("Lua 插件已禁用") end
end

function on_unload()
    if _ctx then _ctx:log("Lua 插件已卸载") end
end
