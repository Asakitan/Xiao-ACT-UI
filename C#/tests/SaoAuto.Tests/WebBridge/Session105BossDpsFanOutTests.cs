using SaoAuto.App.WebBridge;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

/// <summary>
/// S105 — Pin <see cref="GameStatePublisher"/>'s boss-side narrow
/// fan-out: <see cref="BridgeEvents.BossHpSnapshot"/> when any boss
/// HP / shield / overdrive / invincible / source field changes, and
/// <see cref="BridgeEvents.DpsSnapshot"/> when total damage or DPS
/// rate changes. Same first-emit-fires-all + dedupe-on-subsequent
/// pattern as S101.
/// </summary>
public class Session105BossDpsFanOutTests
{
    private static (BridgeEventBroadcaster br, List<BridgeMessage> sink) NewSink()
    {
        var br = new BridgeEventBroadcaster();
        var sink = new List<BridgeMessage>();
        br.Posted += sink.Add;
        return (br, sink);
    }

    [Fact]
    public void BossHpChange_FiresBossHpAlongsideStateChanged()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        sink.Clear();

        states.Update(s => s with
        {
            BossCurrentHp = 5000, BossTotalHp = 10000, BossHpEstPct = 0.5,
            BossHpSource = BossHpSource.Packet,
        });

        var names = sink.Select(m => m.Name).ToList();
        Assert.Contains(BridgeEvents.GameStateChanged, names);
        Assert.Contains(BridgeEvents.BossHpSnapshot, names);
        Assert.DoesNotContain(BridgeEvents.HealthChanged, names);
        Assert.DoesNotContain(BridgeEvents.StaminaChanged, names);
        Assert.DoesNotContain(BridgeEvents.BurstReady, names);
        Assert.DoesNotContain(BridgeEvents.DpsSnapshot, names);
    }

    [Fact]
    public void DpsChange_FiresDpsAlongsideStateChanged()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        sink.Clear();

        states.Update(s => s with { BossTotalDamage = 12345, BossDps = 678 });

        var names = sink.Select(m => m.Name).ToList();
        Assert.Contains(BridgeEvents.GameStateChanged, names);
        Assert.Contains(BridgeEvents.DpsSnapshot, names);
        Assert.DoesNotContain(BridgeEvents.BossHpSnapshot, names);
        Assert.DoesNotContain(BridgeEvents.HealthChanged, names);
    }

    [Fact]
    public void UnrelatedFieldChange_NoBossOrDpsFires()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        sink.Clear();

        states.Update(s => s with { PlayerName = "Asuna" });

        var names = sink.Select(m => m.Name).ToList();
        Assert.Single(names);
        Assert.Equal(BridgeEvents.GameStateChanged, names[0]);
    }

    [Fact]
    public void BossHpFanOut_PayloadShape()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with
        {
            BossCurrentHp = 3000, BossTotalHp = 9000, BossHpEstPct = 0.333,
            BossShieldActive = true, BossShieldPct = 0.42,
            BossBreakingStage = 2, BossInOverdrive = true, BossInvincible = false,
            BossHpSource = BossHpSource.Packet,
        });

        var bh = sink.First(m => m.Name == BridgeEvents.BossHpSnapshot).Payload!;
        Assert.Equal(3000, bh["current"]?.GetValue<int>());
        Assert.Equal(9000, bh["max"]?.GetValue<int>());
        Assert.Equal(0.333, bh["pct"]?.GetValue<double>());
        Assert.True(bh["shield_active"]?.GetValue<bool>());
        Assert.Equal(0.42, bh["shield_pct"]?.GetValue<double>());
        Assert.Equal(2, bh["breaking_stage"]?.GetValue<int>());
        Assert.True(bh["in_overdrive"]?.GetValue<bool>());
        Assert.False(bh["invincible"]?.GetValue<bool>());
        Assert.Equal("Packet", bh["source"]?.GetValue<string>());
    }

    [Fact]
    public void DpsFanOut_PayloadShape()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { BossTotalDamage = 99999, BossDps = 4321 });

        var dps = sink.First(m => m.Name == BridgeEvents.DpsSnapshot).Payload!;
        Assert.Equal(99999, dps["total_damage"]?.GetValue<int>());
        Assert.Equal(4321, dps["dps"]?.GetValue<int>());
    }

    [Fact]
    public void BossShieldFlip_AloneFiresBossHp()
    {
        // BossShieldActive flipping alone (HP unchanged) still triggers
        // a state.bosshp narrow event — the channel is "anything boss
        // HUD cares about", not just current/max.
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start();
        sink.Clear();

        states.Update(s => s with { BossShieldActive = true });

        Assert.Contains(sink, m => m.Name == BridgeEvents.BossHpSnapshot);
        Assert.DoesNotContain(sink, m => m.Name == BridgeEvents.DpsSnapshot);
    }

    [Fact]
    public void BossOverdriveFlip_AloneFiresBossHp()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { BossInOverdrive = true });

        Assert.Contains(sink, m => m.Name == BridgeEvents.BossHpSnapshot);
    }

    [Fact]
    public void BossInvincibleFlip_AloneFiresBossHp()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { BossInvincible = true });

        Assert.Contains(sink, m => m.Name == BridgeEvents.BossHpSnapshot);
    }

    [Fact]
    public void DpsOnlyTotalDamageMoves_StillFires()
    {
        // total_damage moving with dps unchanged still counts —
        // damage-tally subscribers need every cumulative bump.
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { BossTotalDamage = 100 });
        sink.Clear();
        states.Update(s => s with { BossTotalDamage = 200 });

        Assert.Contains(sink, m => m.Name == BridgeEvents.DpsSnapshot);
    }

    [Fact]
    public void BossHpUnchangedAcrossUpdates_NoBossFire()
    {
        var states = new GameStateManager();
        var (br, sink) = NewSink();
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { BossCurrentHp = 500, BossTotalHp = 1000 });
        sink.Clear();

        // Different field, boss HP/shield/overdrive untouched.
        states.Update(s => s with { PlayerName = "Asuna" });

        Assert.Single(sink);
        Assert.Equal(BridgeEvents.GameStateChanged, sink[0].Name);
    }
}
