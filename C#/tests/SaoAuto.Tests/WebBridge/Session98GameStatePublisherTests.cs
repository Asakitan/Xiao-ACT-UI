using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

/// <summary>
/// S98 — Pin <see cref="GameStatePublisher"/>. Routes
/// <see cref="GameStateManager"/> updates through
/// <see cref="StateSnapshotPayload.ToDict"/> + emits
/// <see cref="BridgeEvents.GameStateChanged"/> via
/// <see cref="BridgeEventBroadcaster"/>. De-duplicates identical
/// payloads.
/// </summary>
public class Session98GameStatePublisherTests
{
    private static (BridgeEventBroadcaster br, List<BridgeMessage> sink) NewSink()
    {
        // S101: publisher now also fans out narrow events
        // (state.hp / state.stamina / state.burst_ready). These S98
        // tests pin only the canonical state.changed channel, so
        // filter the sink to that event name.
        var br = new BridgeEventBroadcaster();
        var sink = new List<BridgeMessage>();
        br.Posted += msg =>
        {
            if (msg.Name == BridgeEvents.GameStateChanged) sink.Add(msg);
        };
        return (br, sink);
    }

    [Fact]
    public void NullStates_Throws()
    {
        var (br, _) = NewSink();
        Assert.Throws<ArgumentNullException>(() => new GameStatePublisher(null!, br));
    }

    [Fact]
    public void NullBroadcaster_Throws()
    {
        var states = new GameStateManager();
        Assert.Throws<ArgumentNullException>(() => new GameStatePublisher(states, null!));
    }

    [Fact]
    public void Start_EmitInitial_PostsOneMessage()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        Assert.Single(sink);
        Assert.Equal(BridgeEvents.GameStateChanged, sink[0].Name);
        Assert.Equal(BridgeMessage.TypeEvent, sink[0].Type);
        Assert.NotNull(sink[0].Payload);
    }

    [Fact]
    public void Start_NoInitial_PostsNothing()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);
        Assert.Empty(sink);
    }

    [Fact]
    public void Update_FiresEmit()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);
        states.Update(s => s with { PlayerName = "Kirito" });
        Assert.Single(sink);
        var payload = sink[0].Payload!;
        Assert.Equal("Kirito", payload["player_name"]?.GetValue<string>());
    }

    [Fact]
    public void IdenticalUpdate_Deduped()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);
        states.Update(s => s with { PlayerName = "Asuna" });
        states.Update(s => s with { PlayerName = "Asuna" }); // identical → deduped
        Assert.Single(sink);
    }

    [Fact]
    public void DifferentUpdates_BothEmit()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);
        states.Update(s => s with { PlayerName = "Kirito" });
        states.Update(s => s with { PlayerName = "Asuna" });
        Assert.Equal(2, sink.Count);
    }

    [Fact]
    public void Start_Idempotent()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);
        pub.Start(emitInitial: false); // no-op
        states.Update(s => s with { PlayerName = "Klein" });
        Assert.Single(sink); // only one subscription
    }

    [Fact]
    public void Dispose_StopsEmit()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);
        pub.Dispose();
        states.Update(s => s with { PlayerName = "Lisbeth" });
        Assert.Empty(sink);
    }

    [Fact]
    public void Dispose_Idempotent()
    {
        var states = new GameStateManager();
        var (br, _) = NewSink();
        var pub = new GameStatePublisher(states, br);
        pub.Start();
        pub.Dispose();
        pub.Dispose(); // no throw
    }

    [Fact]
    public void StartAfterDispose_Throws()
    {
        var states = new GameStateManager();
        var (br, _) = NewSink();
        var pub = new GameStatePublisher(states, br);
        pub.Dispose();
        Assert.Throws<ObjectDisposedException>(() => pub.Start());
    }

    [Fact]
    public void Payload_IncludesFullSnapshotKeys()
    {
        // Spot-check that the routed payload is the 43-key
        // ToDict shape, not a hand-rolled subset.
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        var keys = sink[0].Payload!.Select(kv => kv.Key).ToList();
        Assert.Contains("player_name", keys);
        Assert.Contains("hp_pct", keys);
        Assert.Contains("stamina_pct", keys);
        Assert.Contains("recognition_ok", keys);
        Assert.Contains("burst_ready", keys);
    }
}
