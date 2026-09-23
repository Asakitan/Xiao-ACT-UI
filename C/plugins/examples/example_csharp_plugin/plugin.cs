public class Plugin
{
    private static dynamic _ctx;

    public static void OnLoad(PluginContext ctx)
    {
        _ctx = ctx;
        ctx.log("C# 插件已加载 (原生 csmini)");
        var greeting = ctx.get_setting("greeting", "你好");
        ctx.set_setting("greeting", greeting);
        ctx.log(greeting);
    }

    public static void OnEnable()
    {
        _ctx.log("C# 插件已启用");
    }

    public static void OnDisable()
    {
        _ctx.log("C# 插件已禁用");
    }

    public static bool OnUnload()
    {
        _ctx.log("C# 插件已卸载");
        return true;
    }
}
