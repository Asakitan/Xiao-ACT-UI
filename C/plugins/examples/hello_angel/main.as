// main.as — hello_angel 生命周期示例插件
//
// 演示 angel_host 加载 AngelScript 插件全流程:
//   1. on_load()  — 调 log_info, register_ui_panel("AS Hello"), register_hotkey("greet", "F9")
//   2. on_tick()  — 每 tick 计一次 tick 数
//   3. on_unload() — 打日志 + 让宿主释放引擎侧的 wrap_callback
//
// 用**自由函数**风格, 不依赖 dictionary / PluginContext@ 复杂类型绑定 —
// angel_host 侧只暴露 3 个 C 桥 (log_info / register_ui_panel / register_hotkey),
// 每个签名都是 (const string &in) → void, 保证跨 SDK 绑定层最小面.

int tick_count = 0;
int total_ticks_seen = 0;

void on_load()
{
    log_info("hello_angel: on_load fired");
    register_ui_panel("AS Hello");
    register_hotkey("greet_hotkey", "F9");
    log_info("hello_angel: registered UI + hotkey");
}

void on_tick()
{
    tick_count += 1;
    total_ticks_seen = tick_count;
    if (tick_count == 1) {
        log_info("hello_angel: first tick");
    }
}

void on_unload()
{
    log_info("hello_angel: on_unload fired");
}
