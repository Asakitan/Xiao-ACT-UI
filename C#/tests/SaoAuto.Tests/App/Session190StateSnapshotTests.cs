using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

/// <summary>
/// S190 — Pin the <c>state.snapshot</c> pull command. A HUD page
/// opening mid-run dispatches this; the router replies with the same
/// payload shape <c>state.changed</c> events emit, so freshly-loaded
/// pages can paint immediately.
/// </summary>
public class Session190StateSnapshotTests
{
    [Fact]
    public void Lifecycle_Start_RegistersSnapshotCommand()
    {
        var states = new GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        web.Start(emitInitial: false);
        Assert.Contains(BridgeCommands.StateSnapshot, web.Router.RegisteredCommands);
    }

    [Fact]
    public void SnapshotCommand_ReturnsCurrentPayload()
    {
        var states = new GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        web.Start(emitInitial: false);

        var reply = web.Router.Dispatch(
            new BridgeMessage(BridgeMessage.TypeCommand, BridgeCommands.StateSnapshot, null));
        Assert.NotNull(reply);
        Assert.Equal(BridgeMessage.TypeReply, reply!.Type);
        Assert.Equal(BridgeCommands.StateSnapshot, reply.Name);
        var payload = reply.Payload;
        Assert.NotNull(payload);
        // The dict-shape mirrors StateSnapshotPayload.ToDict — minimum
        // contract: includes the canonical HP keys.
        Assert.True(payload!.ContainsKey("hp_current"));
        Assert.True(payload.ContainsKey("hp_max"));
        Assert.True(payload.ContainsKey("hp_pct"));
    }

    [Fact]
    public void SnapshotCommand_ReflectsMutations()
    {
        var states = new GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        web.Start(emitInitial: false);
        states.Update(s => s with { HpCurrent = 555, HpMax = 1000, HpPct = 0.555 });

        var reply = web.Router.Dispatch(
            new BridgeMessage(BridgeMessage.TypeCommand, BridgeCommands.StateSnapshot, null));
        var payload = reply!.Payload!;
        Assert.Equal(555, payload["hp_current"]!.GetValue<int>());
        Assert.Equal(1000, payload["hp_max"]!.GetValue<int>());
    }
}
