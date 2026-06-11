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
    public void SetEntityRolePromotesAndDemotesTrackedEntities()
    {
        var router = new BridgeRouter();
        var engine = new BossRaidEngine();
        using var bridge = new BossRaidRuntimeBridge(router, engine);
        engine.Start(new[] { new RaidPhase(0, "P1", 30) });
        engine.OnDamageEvent(0xABC, 100, true, true, false, false, false, "First");
        engine.OnDamageEvent(0xDEF, 50, true, true, false, false, false, "Second");

        var promote = router.Dispatch(Cmd(BridgeCommands.RaidSetEntityRole, new JsonObject
        {
            ["uuid"] = 0xDEF,
            ["role"] = "boss",
        }))!.Payload!;

        Assert.True(promote["ok"]!.GetValue<bool>());
        Assert.Equal("set_entity_role", promote["command"]!.GetValue<string>());
        Assert.Equal(0xDEF, promote["uuid"]!.GetValue<long>());
        Assert.Equal("boss", promote["role"]!.GetValue<string>());
        Assert.Equal(0xDEF, engine.BossUuid);
        var entities = engine.Entities;
        Assert.Equal("enemy", entities.Single(e => e.Uuid == 0xABC).Role);
        Assert.Equal("boss", entities.Single(e => e.Uuid == 0xDEF).Role);
        Assert.Equal("boss", promote["entities"]!.AsArray()[1]!["role"]!.GetValue<string>());

        var demote = router.Dispatch(Cmd(BridgeCommands.RaidSetEntityRole, new JsonObject
        {
            ["uuid"] = 0xDEF,
            ["role"] = "enemy",
        }))!.Payload!;

        Assert.True(demote["ok"]!.GetValue<bool>());
        Assert.Equal(0, engine.BossUuid);
        Assert.All(engine.Entities, entity => Assert.Equal("enemy", entity.Role));
    }

    [Fact]
    public void SetEntityRoleRejectsUnknownEntity()
    {
        var router = new BridgeRouter();
        var engine = new BossRaidEngine();
        using var bridge = new BossRaidRuntimeBridge(router, engine);
        engine.Start(new[] { new RaidPhase(0, "P1", 30) });

        var reply = router.Dispatch(Cmd(BridgeCommands.RaidSetEntityRole, new JsonObject
        {
            ["uuid"] = 0xABC,
            ["role"] = "boss",
        }));
        var payload = reply!.Payload!;

        Assert.False(payload["ok"]!.GetValue<bool>());
        Assert.Equal("entity_not_found", payload["error"]!.GetValue<string>());
        Assert.Equal(0xABC, payload["uuid"]!.GetValue<long>());
    }

    [Fact]
    public void DisposeUnregistersCommands()
    {
        var router = new BridgeRouter();
        var engine = new BossRaidEngine();
        var bridge = new BossRaidRuntimeBridge(router, engine);

        Assert.Contains(BridgeCommands.RaidNextPhase, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RaidReset, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RaidSetEntityRole, router.RegisteredCommands);

        bridge.Dispose();

        Assert.DoesNotContain(BridgeCommands.RaidNextPhase, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RaidReset, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RaidSetEntityRole, router.RegisteredCommands);
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
        Assert.Contains(BridgeCommands.RaidSetEntityRole, lifecycle.Router.RegisteredCommands);

        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.RaidReset));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());

        lifecycle.Dispose();

        Assert.DoesNotContain(BridgeCommands.RaidNextPhase, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RaidReset, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RaidSetEntityRole, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachAfterDisposeThrows()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.Dispose();

        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachBossRaidRuntime(new BossRaidEngine()));
    }
}
