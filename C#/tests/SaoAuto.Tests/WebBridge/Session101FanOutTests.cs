using SaoAuto.App.WebBridge;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

/// <summary>
/// S101 — Pin <see cref="GameStatePublisher"/>'s narrow-event fan-out.
/// On every <c>state.changed</c> emission, the publisher also fires
/// <see cref="BridgeEvents.HealthChanged"/> /
/// <see cref="BridgeEvents.StaminaChanged"/> /
/// <see cref="BridgeEvents.BurstReady"/> when the corresponding fields
/// differ from the previous emit. The first emit (initial snapshot)
/// fires all three so JS subscribers have a value to render.
/// </summary>
public class Session101FanOutTests
{
    private static (BridgeEventBroadcaster br, List<BridgeMessage> sink) NewSink()
    {
        var br = new BridgeEventBroadcaster();
        var sink = new List<BridgeMessage>();
        br.Posted += sink.Add;
        return (br, sink);
    }

    [Fact]
    public void StartEmitInitial_FiresStateChangedPlusAllNarrowChannels()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();

        var names = sink.Select(m => m.Name).ToList();
        Assert.Equal(6, sink.Count);
        Assert.Contains(BridgeEvents.GameStateChanged, names);
        Assert.Contains(BridgeEvents.HealthChanged, names);
        Assert.Contains(BridgeEvents.StaminaChanged, names);
        Assert.Contains(BridgeEvents.BurstReady, names);
        Assert.Contains(BridgeEvents.BossHpSnapshot, names);
        Assert.Contains(BridgeEvents.DpsSnapshot, names);
    }

    [Fact]
    public void GameStateChanged_FiresFirstAmongFanOut()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        Assert.Equal(BridgeEvents.GameStateChanged, sink[0].Name);
    }

    [Fact]
    public void HpChange_OnlyHealthFiresAlongsideStateChanged()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        sink.Clear();

        states.Update(s => s with { HpCurrent = 80, HpMax = 100, HpPct = 0.8 });

        var names = sink.Select(m => m.Name).ToList();
        Assert.Contains(BridgeEvents.GameStateChanged, names);
        Assert.Contains(BridgeEvents.HealthChanged, names);
        Assert.DoesNotContain(BridgeEvents.StaminaChanged, names);
        Assert.DoesNotContain(BridgeEvents.BurstReady, names);
    }

    [Fact]
    public void StaminaChange_OnlyStaminaFiresAlongsideStateChanged()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        sink.Clear();

        states.Update(s => s with { StaminaCurrent = 50, StaminaMax = 100, StaminaPct = 0.5 });

        var names = sink.Select(m => m.Name).ToList();
        Assert.Contains(BridgeEvents.GameStateChanged, names);
        Assert.Contains(BridgeEvents.StaminaChanged, names);
        Assert.DoesNotContain(BridgeEvents.HealthChanged, names);
        Assert.DoesNotContain(BridgeEvents.BurstReady, names);
    }

    [Fact]
    public void BurstReadyFlip_OnlyBurstFiresAlongsideStateChanged()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        sink.Clear();

        states.Update(s => s with { BurstReady = true });

        var names = sink.Select(m => m.Name).ToList();
        Assert.Contains(BridgeEvents.GameStateChanged, names);
        Assert.Contains(BridgeEvents.BurstReady, names);
        Assert.DoesNotContain(BridgeEvents.HealthChanged, names);
        Assert.DoesNotContain(BridgeEvents.StaminaChanged, names);
    }

    [Fact]
    public void UnrelatedFieldChange_NoNarrowChannelFires()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        sink.Clear();

        states.Update(s => s with { PlayerName = "Kirito" });

        var names = sink.Select(m => m.Name).ToList();
        Assert.Single(names);
        Assert.Equal(BridgeEvents.GameStateChanged, names[0]);
    }

    [Fact]
    public void IdenticalUpdate_NothingFires()
    {
        // Use a fixed clock so the auto-stamped CaptureTimestamp on
        // GameStateManager.Update doesn't drift between calls and
        // defeat the publisher's signature dedupe.
        var fixedTime = DateTimeOffset.FromUnixTimeMilliseconds(1_700_000_000_000);
        var states = new GameStateManager(clock: () => fixedTime);
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { PlayerName = "Asuna" });
        sink.Clear();

        // Re-apply identical content — payload signature unchanged → publisher dedupes.
        states.Update(s => s with { PlayerName = "Asuna" });
        Assert.Empty(sink);
    }

    [Fact]
    public void HpFanOut_PayloadShape()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { HpCurrent = 42, HpMax = 100, HpPct = 0.42 });

        var hp = sink.First(m => m.Name == BridgeEvents.HealthChanged).Payload!;
        Assert.Equal(42, hp["current"]?.GetValue<int>());
        Assert.Equal(100, hp["max"]?.GetValue<int>());
        Assert.Equal(0.42, hp["pct"]?.GetValue<double>());
    }

    [Fact]
    public void StaminaFanOut_PayloadShape()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { StaminaCurrent = 7, StaminaMax = 10, StaminaPct = 0.7 });

        var sta = sink.First(m => m.Name == BridgeEvents.StaminaChanged).Payload!;
        Assert.Equal(7, sta["current"]?.GetValue<int>());
        Assert.Equal(10, sta["max"]?.GetValue<int>());
        Assert.Equal(0.7, sta["pct"]?.GetValue<double>());
    }

    [Fact]
    public void BurstFanOut_PayloadShape()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { BurstReady = true });

        var burst = sink.First(m => m.Name == BridgeEvents.BurstReady).Payload!;
        Assert.True(burst["ready"]?.GetValue<bool>());
    }

    [Fact]
    public void BurstFlipBackToFalse_FiresAgain()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { BurstReady = true });
        states.Update(s => s with { BurstReady = false });

        var burstEvents = sink.Where(m => m.Name == BridgeEvents.BurstReady).ToList();
        Assert.Equal(2, burstEvents.Count);
        Assert.True(burstEvents[0].Payload!["ready"]?.GetValue<bool>());
        Assert.False(burstEvents[1].Payload!["ready"]?.GetValue<bool>());
    }

    [Fact]
    public void HpUnchangedAcrossUpdates_NoNarrowFire()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { HpCurrent = 50, HpMax = 100, HpPct = 0.5 });
        sink.Clear();

        // Same HP, different player name — only state.changed should fire.
        states.Update(s => s with { PlayerName = "Asuna" });

        Assert.Single(sink);
        Assert.Equal(BridgeEvents.GameStateChanged, sink[0].Name);
    }
}
