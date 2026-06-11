using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

public class Session202BossRaidRuntimeBridgeTests
{
    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void NextPhaseAdvancesRunningEngine()
    {
        var router = new BridgeRouter();
        var engine = new BossRaidEngine();
        using var bridge = new BossRaidRuntimeBridge(router, engine);
        engine.Start(new[]
        {
            new RaidPhase(0, "P1", 30),
            new RaidPhase(1, "P2", 30),
        });

        var reply = router.Dispatch(Cmd(BridgeCommands.RaidNextPhase));
        var payload = reply!.Payload!;

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("next_phase", payload["command"]!.GetValue<string>());
        Assert.True(payload["running"]!.GetValue<bool>());
        Assert.Equal(1, payload["phase_idx"]!.GetValue<int>());
        Assert.Equal("P2", payload["phase_name"]!.GetValue<string>());
        Assert.Equal("P2", engine.CurrentPhase!.Name);
    }

    [Fact]
    public void ResetStopsEngine()
    {
        var router = new BridgeRouter();
        var engine = new BossRaidEngine();
        using var bridge = new BossRaidRuntimeBridge(router, engine);
        engine.Start(new[] { new RaidPhase(0, "P1", 30) });

        var reply = router.Dispatch(Cmd(BridgeCommands.RaidReset));
        var payload = reply!.Payload!;

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("reset", payload["command"]!.GetValue<string>());
        Assert.False(payload["running"]!.GetValue<bool>());
        Assert.Equal(-1, payload["phase_idx"]!.GetValue<int>());
        Assert.Equal(string.Empty, payload["phase_name"]!.GetValue<string>());
        Assert.False(engine.Running);
    }

    [Fact]
    public void IdleNextPhaseIsNoopSuccess()
    {
        var router = new BridgeRouter();
        var engine = new BossRaidEngine();
        using var bridge = new BossRaidRuntimeBridge(router, engine);

        var reply = router.Dispatch(Cmd(BridgeCommands.RaidNextPhase));
        var payload = reply!.Payload!;

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("next_phase", payload["command"]!.GetValue<string>());
        Assert.False(payload["running"]!.GetValue<bool>());
        Assert.Equal(-1, payload["phase_idx"]!.GetValue<int>());
        Assert.Equal(string.Empty, payload["phase_name"]!.GetValue<string>());
    }

    [Fact]
    public void DisposeUnregistersCommands()
    {
        var router = new BridgeRouter();
        var engine = new BossRaidEngine();
        var bridge = new BossRaidRuntimeBridge(router, engine);

        Assert.Contains(BridgeCommands.RaidNextPhase, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RaidReset, router.RegisteredCommands);

        bridge.Dispose();

        Assert.DoesNotContain(BridgeCommands.RaidNextPhase, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RaidReset, router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachRegistersAndDisposesRuntimeCommands()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var engine = new BossRaidEngine();

        lifecycle.AttachBossRaidRuntime(engine);
        lifecycle.AttachBossRaidRuntime(engine);

        Assert.Contains(BridgeCommands.RaidNextPhase, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RaidReset, lifecycle.Router.RegisteredCommands);

        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.RaidReset));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());

        lifecycle.Dispose();

        Assert.DoesNotContain(BridgeCommands.RaidNextPhase, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RaidReset, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachAfterDisposeThrows()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.Dispose();

        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachBossRaidRuntime(new BossRaidEngine()));
    }
}
