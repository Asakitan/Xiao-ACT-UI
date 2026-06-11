using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;

namespace SaoAuto.Tests.App;

public class Session195LegacyUiBridgeTests
{
    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void RegistersShimUiCommandsAndUnregistersOnDispose()
    {
        var router = new BridgeRouter();
        var bridge = new LegacyUiBridge(router);
        var commands = new[]
        {
            BridgeCommands.SetHitRegions,
            BridgeCommands.BossHpHitRegions,
            BridgeCommands.NotifyHpHitRegionsReady,
            BridgeCommands.ExitApplication,
            BridgeCommands.SetPanelVisible,
            BridgeCommands.GetPanelThemes,
            BridgeCommands.SetPanelTheme,
            BridgeCommands.ToggleMenu,
            BridgeCommands.ContextAction,
            BridgeCommands.MenuAction,
            BridgeCommands.FetchLeaderboard,
            BridgeCommands.WindowDrag,
            BridgeCommands.SetCtxMenuActive,
            BridgeCommands.ClosePanel,
            BridgeCommands.PanelAction,
        };

        foreach (var command in commands)
        {
            Assert.Contains(command, router.RegisteredCommands);
            var reply = router.Dispatch(Cmd(command));
            Assert.Equal(BridgeMessage.TypeReply, reply!.Type);
            Assert.Equal(command, reply.Name);
            Assert.NotEqual("unknown_command", reply.Payload?["error"]?.GetValue<string>());
        }

        bridge.Dispose();

        foreach (var command in commands)
        {
            Assert.DoesNotContain(command, router.RegisteredCommands);
        }
    }

    [Fact]
    public void ExitActionRunsForExitCommandAndActionAliases()
    {
        var exits = 0;
        var router = new BridgeRouter();
        using var bridge = new LegacyUiBridge(router, exitAction: () => exits++);

        router.Dispatch(Cmd(BridgeCommands.ExitApplication));
        router.Dispatch(Cmd(BridgeCommands.ContextAction, new JsonObject { ["action"] = "exit" }));
        router.Dispatch(Cmd(BridgeCommands.MenuAction, new JsonObject { ["action"] = "exit" }));

        Assert.Equal(3, exits);
    }

    [Fact]
    public void GetPanelThemesReturnsPageConsumableDefaults()
    {
        var router = new BridgeRouter();
        using var bridge = new LegacyUiBridge(router);

        var reply = router.Dispatch(Cmd(BridgeCommands.GetPanelThemes));
        var payload = reply!.Payload!;

        Assert.Equal("dark", payload["dps"]!.GetValue<string>());
        Assert.Equal("dark", payload["hp"]!.GetValue<string>());
        Assert.Equal("dark", payload["bosshp"]!.GetValue<string>());
        Assert.Equal("dark", payload["skillfx"]!.GetValue<string>());
        Assert.Equal("dark", payload["alert"]!.GetValue<string>());
        Assert.Equal("dark", payload["act"]!.GetValue<string>());
    }
}
