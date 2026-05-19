using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S72 — slot-readiness hysteresis gate. Verifies the leading-edge
/// timer + reset-on-not-ready behaviour from
/// <c>auto_key_engine._action_ready</c>.
/// </summary>
public class Session72ReadinessGateTests
{
    private static DateTimeOffset T(int ms)
        => new DateTimeOffset(2026, 1, 1, 0, 0, 0, TimeSpan.Zero).AddMilliseconds(ms);

    private static SlotReadiness Ready() => new("ready", false, 0, 999_999, 1.0);
    private static SlotReadiness ChargeReady() => new("", false, 2, 999_999, 1.0);
    private static SlotReadiness ColdSlot() => new("cooldown", false, 0, 5_000, 0.7);

    [Fact]
    public void IsReady_HonoursStateActiveChargeRemainingAndCooldownPct()
    {
        Assert.True(SlotReadiness.IsReady(new("READY", false, 0, 999_999, 1.0)));   // case-insensitive
        Assert.True(SlotReadiness.IsReady(new("active", false, 0, 999_999, 1.0)));
        Assert.True(SlotReadiness.IsReady(new("", true, 0, 999_999, 1.0)));         // active flag
        Assert.True(SlotReadiness.IsReady(new("", false, 1, 999_999, 1.0)));        // charges
        Assert.True(SlotReadiness.IsReady(new("", false, 0, 120, 1.0)));            // remaining 120ms
        Assert.True(SlotReadiness.IsReady(new("", false, 0, 999_999, 0.02)));       // cooldown_pct
        Assert.False(SlotReadiness.IsReady(new("cooldown", false, 0, 121, 0.021))); // just over both
        Assert.False(SlotReadiness.IsReady(SlotReadiness.NotReady));
    }

    [Fact]
    public void TryFire_ZeroDelay_FiresOnFirstReadyTick()
    {
        var gate = new AutoKeyReadinessGate();
        Assert.True(gate.TryFire("a1", Ready(), readyDelayMs: 0, T(0)));
    }

    [Fact]
    public void TryFire_DelayedReadiness_GatesUntilContinuous()
    {
        var gate = new AutoKeyReadinessGate();
        Assert.False(gate.TryFire("a1", Ready(), readyDelayMs: 200, T(0)));    // since = 0
        Assert.False(gate.TryFire("a1", Ready(), readyDelayMs: 200, T(199)));  // 1ms short
        Assert.True(gate.TryFire("a1", Ready(), readyDelayMs: 200, T(200)));   // exact boundary
    }

    [Fact]
    public void TryFire_NotReadyResetsTimer()
    {
        var gate = new AutoKeyReadinessGate();
        Assert.False(gate.TryFire("a1", Ready(), 200, T(0)));
        Assert.False(gate.TryFire("a1", ColdSlot(), 200, T(150)));      // drop → forget
        Assert.Null(gate.ReadySince("a1"));
        Assert.False(gate.TryFire("a1", Ready(), 200, T(160)));         // restart from 160
        Assert.False(gate.TryFire("a1", Ready(), 200, T(359)));
        Assert.True(gate.TryFire("a1", Ready(), 200, T(360)));
    }

    [Fact]
    public void TryFire_PerActionIsolation()
    {
        var gate = new AutoKeyReadinessGate();
        Assert.False(gate.TryFire("a1", Ready(), 100, T(0)));
        Assert.False(gate.TryFire("a2", Ready(), 100, T(50)));
        Assert.True(gate.TryFire("a1", Ready(), 100, T(100)));   // a1 ripens
        Assert.False(gate.TryFire("a2", Ready(), 100, T(100)));  // a2 still 50ms in
        Assert.True(gate.TryFire("a2", Ready(), 100, T(150)));
    }

    [Fact]
    public void TryFire_ChargeCountCountsAsReady()
    {
        var gate = new AutoKeyReadinessGate();
        Assert.True(gate.TryFire("a1", ChargeReady(), readyDelayMs: 0, T(0)));
    }

    [Fact]
    public void Forget_ClearsOneAction()
    {
        var gate = new AutoKeyReadinessGate();
        gate.TryFire("a1", Ready(), 500, T(0));
        gate.TryFire("a2", Ready(), 500, T(0));
        Assert.Equal(2, gate.TrackedCount);
        gate.Forget("a1");
        Assert.Equal(1, gate.TrackedCount);
        Assert.Null(gate.ReadySince("a1"));
        Assert.NotNull(gate.ReadySince("a2"));
    }

    [Fact]
    public void Reset_ClearsAll()
    {
        var gate = new AutoKeyReadinessGate();
        gate.TryFire("a1", Ready(), 500, T(0));
        gate.TryFire("a2", Ready(), 500, T(0));
        gate.Reset();
        Assert.Equal(0, gate.TrackedCount);
    }

    [Fact]
    public void TryFire_NegativeDelayTreatedAsZero()
    {
        var gate = new AutoKeyReadinessGate();
        Assert.True(gate.TryFire("a1", Ready(), readyDelayMs: -5, T(0)));
    }

    [Fact]
    public void TryFire_EmptyActionIdRejected()
    {
        var gate = new AutoKeyReadinessGate();
        Assert.Throws<ArgumentException>(() => gate.TryFire("", Ready(), 0, T(0)));
    }
}
