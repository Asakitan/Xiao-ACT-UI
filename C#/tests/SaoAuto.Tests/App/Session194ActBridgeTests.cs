using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

/// <summary>
/// S194 — Native WebView2 ACT command bridge. The shared ACT HTML pages call
/// act.* commands through pywebview-shim.js; C# should register those names
/// and return structured unavailability until a native ACT backend is wired.
/// </summary>
public class Session194ActBridgeTests
{
    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void RegistersAllShimActCommandsAndUnregistersOnDispose()
    {
        var router = new BridgeRouter();
        var bridge = new ActBridge(router);

        foreach (var commandName in ActBridge.CommandNames)
        {
            Assert.Contains(commandName, router.RegisteredCommands);
        }

        bridge.Dispose();

        foreach (var commandName in ActBridge.CommandNames)
        {
            Assert.DoesNotContain(commandName, router.RegisteredCommands);
        }
    }

    [Fact]
    public void MissingBackendReturnsStructuredUnavailable()
    {
        var router = new BridgeRouter();
        using var bridge = new ActBridge(router);

        var reply = router.Dispatch(Cmd(BridgeCommands.ActTimelineStatus, new JsonObject { ["limit"] = 80 }));
        var payload = reply!.Payload!;

        Assert.False(payload["ok"]!.GetValue<bool>());
        Assert.Equal("act_unavailable", payload["error"]!.GetValue<string>());
        Assert.Equal(BridgeCommands.ActTimelineStatus, payload["command"]!.GetValue<string>());
        Assert.Equal("timeline", payload["surface"]!.GetValue<string>());
    }

    [Fact]
    public void DelegateBackendReceivesCommandAndPayload()
    {
        var router = new BridgeRouter();
        using var bridge = new ActBridge(router, (name, payload) => new JsonObject
        {
            ["ok"] = true,
            ["command"] = name,
            ["limit"] = payload?["limit"]?.GetValue<int>() ?? 0,
        });

        var reply = router.Dispatch(Cmd(BridgeCommands.ActGraphStatus, new JsonObject { ["limit"] = 120 }));
        var payload = reply!.Payload!;

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal(BridgeCommands.ActGraphStatus, payload["command"]!.GetValue<string>());
        Assert.Equal(120, payload["limit"]!.GetValue<int>());
    }

    [Fact]
    public void NullRouterThrows()
    {
        Assert.Throws<ArgumentNullException>(() => new ActBridge(null!));
    }

    [Fact]
    public void LifecycleAttachActRegistersAndDisposes()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachAct();

        Assert.Contains(BridgeCommands.ActSourcesHealth, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ActAggregateStatus, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ActSkillStatus, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ActDeathRecapStatus, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ActRenderApplyHooks, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ActRenderOverlays, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ActTriggersStatus, lifecycle.Router.RegisteredCommands);

        lifecycle.Dispose();

        Assert.DoesNotContain(BridgeCommands.ActSourcesHealth, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ActSkillStatus, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ActDeathRecapStatus, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ActRenderOverlays, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachActIsIdempotentAndThrowsAfterDispose()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachAct();
        lifecycle.AttachAct((name, _) => new JsonObject { ["command"] = name });

        Assert.Contains(BridgeCommands.ActReportStatus, lifecycle.Router.RegisteredCommands);

        lifecycle.Dispose();
        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachAct());
    }
}
