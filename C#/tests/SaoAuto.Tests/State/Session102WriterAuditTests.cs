using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

/// <summary>
/// S102 — Pin the new <see cref="GameStateManager.ApplyPartial(StatePartial,
/// Func{GameState, GameState})"/> overload AND the migrated packet writers
/// (ApplyHealth/ApplyStamina/ApplyRevive/ApplyContainerSync) so callers
/// run the rollback/clamp/cap pipeline while still flipping the
/// non-tracked sibling fields (PacketActive, StaminaOffline, InCombat,
/// PlayerName, etc.) inside the SAME atomic snapshot.
/// </summary>
public class Session102WriterAuditTests
{
    [Fact]
    public void ApplyPartialWithExtra_NullExtra_Throws()
    {
        var mgr = new GameStateManager();
        Assert.Throws<ArgumentNullException>(() =>
            mgr.ApplyPartial(new StatePartial(), null!));
    }

    [Fact]
    public void ApplyPartialWithExtra_NullPartial_Throws()
    {
        var mgr = new GameStateManager();
        Assert.Throws<ArgumentNullException>(() =>
            mgr.ApplyPartial(null!, s => s));
    }

    [Fact]
    public void ApplyPartialWithExtra_ReturnsNull_Throws()
    {
        var mgr = new GameStateManager();
        Assert.Throws<InvalidOperationException>(() =>
            mgr.ApplyPartial(new StatePartial(), _ => null!));
    }

    [Fact]
    public void ApplyPartialWithExtra_AppliesPartialAndExtraInOneSnapshot()
    {
        var mgr = new GameStateManager();
        var emissions = new List<GameState>();
        using var sub = mgr.Subscribe(emissions.Add);

        var partial = new StatePartial { HpCurrent = 80, HpMax = 100, HpPct = 0.8 };
        mgr.ApplyPartial(partial, s => s with { PacketActive = true, PlayerName = "Kirito" });

        // Single emission, both partial + extra observable.
        Assert.Single(emissions);
        var snap = emissions[0];
        Assert.Equal(80, snap.HpCurrent);
        Assert.Equal(100, snap.HpMax);
        Assert.Equal(0.8, snap.HpPct);
        Assert.True(snap.PacketActive);
        Assert.Equal("Kirito", snap.PlayerName);
    }

    [Fact]
    public void ApplyPartialWithExtra_CapStillRunsAfterExtra()
    {
        // Extra mutate sets HpCurrent above HpMax — cap step (post-extra)
        // should still pin it back to HpMax.
        var mgr = new GameStateManager();
        var partial = new StatePartial { HpMax = 50 };
        var snap = mgr.ApplyPartial(partial, s => s with { HpCurrent = 999 });
        Assert.Equal(50, snap.HpCurrent);
        Assert.Equal(50, snap.HpMax);
    }

    [Fact]
    public void ApplyHealthMutator_FiresSingleAtomicSnapshot()
    {
        var mgr = new GameStateManager();
        var emissions = new List<GameState>();
        using var sub = mgr.Subscribe(emissions.Add);

        StateMutators.Apply(mgr, new HealthEvent(75, 100, 0.75, TimestampSeconds: 0.0));

        Assert.Single(emissions);
        Assert.Equal(75, emissions[0].HpCurrent);
        Assert.True(emissions[0].PacketActive);
    }

    [Fact]
    public void ApplyStaminaMutator_FiresSingleAtomicSnapshot()
    {
        var mgr = new GameStateManager();
        var emissions = new List<GameState>();
        using var sub = mgr.Subscribe(emissions.Add);

        StateMutators.Apply(mgr, new StaminaEvent(
            StaminaCurrent: 60.0, StaminaMax: 100, StaminaPct: 0.6,
            Offline: true, TimestampSeconds: 0.0));

        Assert.Single(emissions);
        Assert.Equal(60, emissions[0].StaminaCurrent);
        Assert.Equal(100, emissions[0].StaminaMax);
        Assert.Equal(0.6, emissions[0].StaminaPct);
        Assert.True(emissions[0].StaminaOffline);
    }

    [Fact]
    public void ApplyReviveMutator_FlipsInCombatAndHpPctTogether()
    {
        var mgr = new GameStateManager();
        mgr.Update(s => s with { InCombat = true, HpPct = 0.0 });
        var emissions = new List<GameState>();
        using var sub = mgr.Subscribe(emissions.Add);

        StateMutators.Apply(mgr, new ReviveEvent(Uuid: 0, TimestampSeconds: 0.0));

        Assert.Single(emissions);
        Assert.False(emissions[0].InCombat);
        Assert.Equal(1.0, emissions[0].HpPct);
    }

    [Fact]
    public void ApplyContainerSyncMutator_FiresSingleAtomicSnapshot()
    {
        var mgr = new GameStateManager();
        var emissions = new List<GameState>();
        using var sub = mgr.Subscribe(emissions.Add);

        StateMutators.Apply(mgr, new ContainerSyncEvent(
            CharId: 1, Name: "Asuna", Level: 50, FightPoint: 12345,
            CurHp: 800, MaxHp: 1000, Energy: 0f, TimestampSeconds: 0.0));

        Assert.Single(emissions);
        var s = emissions[0];
        Assert.Equal("Asuna", s.PlayerName);
        Assert.Equal(50, s.LevelBase);
        Assert.Equal(12345, s.FightPoint);
        Assert.Equal(800, s.HpCurrent);
        Assert.Equal(1000, s.HpMax);
        Assert.True(s.PacketActive);
    }

    [Fact]
    public void HealthMutator_RollbackPipelineRunsOnZero()
    {
        // First health event seeds prev tracker; second with HpCurrent=0
        // (and pct>0) should roll back to HpMax, not stay at 0.
        var mgr = new GameStateManager();
        StateMutators.Apply(mgr, new HealthEvent(80, 100, 0.8, 0.0));
        StateMutators.Apply(mgr, new HealthEvent(0, 100, 0.5, 0.0));

        // RollbackHpCurrent fires when current==0 + pct>0 + hp_max>0 +
        // previous>0, restoring to previous (80), not the incoming 0.
        Assert.Equal(80, mgr.Snapshot.HpCurrent);
    }

    [Fact]
    public void StaminaMutator_PctClamped()
    {
        var mgr = new GameStateManager();
        StateMutators.Apply(mgr, new StaminaEvent(
            StaminaCurrent: 50.0, StaminaMax: 100, StaminaPct: 1.5,
            Offline: false, TimestampSeconds: 0.0));
        Assert.Equal(1.0, mgr.Snapshot.StaminaPct);
    }
}
