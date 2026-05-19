using SaoAuto.App.WebBridge;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

/// <summary>
/// S99 — Pin <see cref="WebBridgeLifecycle"/>. Bundles the
/// <see cref="BridgeEventBroadcaster"/> + <see cref="GameStatePublisher"/>
/// so a UiRunner can own them via <c>using</c> without manually
/// composing the pair every time.
/// </summary>
public class Session99WebBridgeLifecycleTests
{
    [Fact]
    public void NullStates_Throws()
    {
        Assert.Throws<ArgumentNullException>(() => new WebBridgeLifecycle(null!));
    }

    [Fact]
    public void Construct_BroadcasterAvailable_PublisherInactive()
    {
        var states = new GameStateManager();
        using var lifecycle = new WebBridgeLifecycle(states);
        Assert.NotNull(lifecycle.Broadcaster);
        Assert.False(lifecycle.IsActive);
    }

    [Fact]
    public void Start_EmitInitial_PostsThroughBroadcaster()
    {
        var states = new GameStateManager();
        using var lifecycle = new WebBridgeLifecycle(states);
        var sink = new List<BridgeMessage>();
        lifecycle.Broadcaster.Posted += msg =>
        {
            // S101: filter to state.changed; publisher also fans out narrow channels.
            if (msg.Name == BridgeEvents.GameStateChanged) sink.Add(msg);
        };

        lifecycle.Start();

        Assert.True(lifecycle.IsActive);
        Assert.Single(sink);
        Assert.Equal(BridgeEvents.GameStateChanged, sink[0].Name);
    }

    [Fact]
    public void Start_NoInitial_PostsOnlyOnUpdate()
    {
        var states = new GameStateManager();
        using var lifecycle = new WebBridgeLifecycle(states);
        var sink = new List<BridgeMessage>();
        lifecycle.Broadcaster.Posted += msg =>
        {
            if (msg.Name == BridgeEvents.GameStateChanged) sink.Add(msg);
        };

        lifecycle.Start(emitInitial: false);
        Assert.Empty(sink);

        states.Update(s => s with { PlayerName = "Eugeo" });
        Assert.Single(sink);
    }

    [Fact]
    public void Dispose_StopsEmit_AndIsIdempotent()
    {
        var states = new GameStateManager();
        var lifecycle = new WebBridgeLifecycle(states);
        var sink = new List<BridgeMessage>();
        lifecycle.Broadcaster.Posted += msg =>
        {
            if (msg.Name == BridgeEvents.GameStateChanged) sink.Add(msg);
        };

        lifecycle.Start(emitInitial: false);
        lifecycle.Dispose();

        states.Update(s => s with { PlayerName = "Alice" });
        Assert.Empty(sink);

        lifecycle.Dispose(); // no throw
    }

    [Fact]
    public void StartAfterDispose_Throws()
    {
        var states = new GameStateManager();
        var lifecycle = new WebBridgeLifecycle(states);
        lifecycle.Dispose();
        Assert.Throws<ObjectDisposedException>(() => lifecycle.Start());
    }
}
