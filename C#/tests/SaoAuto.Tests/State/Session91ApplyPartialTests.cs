using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

/// <summary>
/// S91 — Pins the composed <see cref="GameStateManager.ApplyPartial"/>
/// pipeline (rollbacks → pct clamps → caps → _prev* refresh) against
/// Python <c>game_state.GameStateManager.update</c> (game_state.py 250–355).
/// </summary>
public class Session91ApplyPartialTests
{
    private static GameStateManager New()
    {
        var t = 0.0;
        return new GameStateManager(
            clock: () => DateTimeOffset.FromUnixTimeMilliseconds((long)(t += 1) * 1000));
    }

    [Fact]
    public void Empty_Partial_LeavesStateUntouched_ButRefreshesTimestamp()
    {
        var m = New();
        m.Replace(new GameState { HpCurrent = 100, HpMax = 100 });
        var before = m.Snapshot;
        var after = m.ApplyPartial(new StatePartial());
        Assert.Equal(before.HpCurrent, after.HpCurrent);
        Assert.Equal(before.HpMax, after.HpMax);
        Assert.NotEqual(before.CaptureTimestamp, after.CaptureTimestamp);
    }

    [Fact]
    public void HpCurrent_Zero_WithLivePct_RollsBackToPrev()
    {
        var m = New();
        // Establish previous non-zero
        m.ApplyPartial(new StatePartial { HpCurrent = 750, HpMax = 1000, HpPct = 0.75 });
        // 0 incoming + pct still > 0.001 → rollback to 750
        var s = m.ApplyPartial(new StatePartial { HpCurrent = 0, HpPct = 0.5 });
        Assert.Equal(750, s.HpCurrent);
    }

    [Fact]
    public void HpCurrent_Zero_WhenPctIsZero_StaysZero()
    {
        var m = New();
        m.ApplyPartial(new StatePartial { HpCurrent = 800, HpMax = 1000, HpPct = 0.8 });
        var s = m.ApplyPartial(new StatePartial { HpCurrent = 0, HpPct = 0.0 });
        Assert.Equal(0, s.HpCurrent);
    }

    [Fact]
    public void HpCurrent_CappedToMax()
    {
        var m = New();
        var s = m.ApplyPartial(new StatePartial { HpCurrent = 9999, HpMax = 1000 });
        Assert.Equal(1000, s.HpCurrent);
    }

    [Fact]
    public void HpPct_ClampedToOne()
    {
        var m = New();
        var s = m.ApplyPartial(new StatePartial { HpPct = 1.5 });
        Assert.Equal(1.0, s.HpPct);
        s = m.ApplyPartial(new StatePartial { HpPct = -0.2 });
        Assert.Equal(0.0, s.HpPct);
    }

    [Fact]
    public void StaminaCurrent_Zero_WithStaPctExplicit_NoRollback()
    {
        var m = New();
        m.ApplyPartial(new StatePartial { StaminaCurrent = 60, StaminaMax = 100, StaminaPct = 0.6 });
        // pct is in same batch → skip rollback even when current=0
        var s = m.ApplyPartial(new StatePartial { StaminaCurrent = 0, StaminaPct = 0.0 });
        Assert.Equal(0, s.StaminaCurrent);
    }

    [Fact]
    public void StaminaCurrent_Zero_NoPct_MaxPositive_NoRollback()
    {
        var m = New();
        m.ApplyPartial(new StatePartial { StaminaCurrent = 60, StaminaMax = 100 });
        // stamina_max > 0 (effective) → rollback skipped
        var s = m.ApplyPartial(new StatePartial { StaminaCurrent = 0 });
        Assert.Equal(0, s.StaminaCurrent);
    }

    [Fact]
    public void StaminaCurrent_Zero_NoPct_NoMax_RollsBackToPrev()
    {
        var m = New();
        m.ApplyPartial(new StatePartial { StaminaCurrent = 50 }); // prev=50, max stays 0
        var s = m.ApplyPartial(new StatePartial { StaminaCurrent = 0 });
        Assert.Equal(50, s.StaminaCurrent);
    }

    [Fact]
    public void LevelBase_Zero_RollsBackToPrev()
    {
        var m = New();
        m.ApplyPartial(new StatePartial { LevelBase = 42 });
        var s = m.ApplyPartial(new StatePartial { LevelBase = 0 });
        Assert.Equal(42, s.LevelBase);
    }

    [Fact]
    public void LevelExtra_Zero_RollsBackToPrev()
    {
        var m = New();
        m.ApplyPartial(new StatePartial { LevelExtra = 7 });
        var s = m.ApplyPartial(new StatePartial { LevelExtra = 0 });
        Assert.Equal(7, s.LevelExtra);
    }

    [Fact]
    public void Replace_ReseedsPrevTrackers()
    {
        var m = New();
        m.Replace(new GameState
        {
            HpCurrent = 555, HpMax = 1000, HpPct = 0.555,
            StaminaCurrent = 33, LevelBase = 88, LevelExtra = 4,
        });
        // After Replace, sending zeros should roll back to the cached values.
        var s = m.ApplyPartial(new StatePartial
        {
            HpCurrent = 0, HpPct = 0.5,
            LevelBase = 0, LevelExtra = 0,
            StaminaCurrent = 0, // no pct, no max → rollback path
        });
        Assert.Equal(555, s.HpCurrent);
        Assert.Equal(88, s.LevelBase);
        Assert.Equal(4, s.LevelExtra);
        Assert.Equal(33, s.StaminaCurrent);
    }

    [Fact]
    public void NotifiesSubscribersOutsideLock()
    {
        var m = New();
        GameState? received = null;
        using var sub = m.Subscribe(s => received = s);
        var s = m.ApplyPartial(new StatePartial { HpCurrent = 10, HpMax = 10 });
        Assert.NotNull(received);
        Assert.Same(s, received);
    }

    [Fact]
    public void NullPartial_Throws()
    {
        var m = New();
        Assert.Throws<ArgumentNullException>(() => m.ApplyPartial(null!));
    }
}
