using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

public class AutoKeyTriggerTests
{
    [Fact]
    public void HpBelowFiresWhenPctAtOrBelow()
    {
        var trigger = new HpBelowTrigger(0.4);
        var ctx = new AutoKeyContext(0.3, 1.0, false, 0, true, DateTimeOffset.UtcNow);
        Assert.True(AutoKeyTriggers.ShouldFire(trigger, ctx));
    }

    [Fact]
    public void HpBelowDoesNotFireAboveThreshold()
    {
        var trigger = new HpBelowTrigger(0.4);
        var ctx = new AutoKeyContext(0.5, 1.0, false, 0, true, DateTimeOffset.UtcNow);
        Assert.False(AutoKeyTriggers.ShouldFire(trigger, ctx));
    }

    [Fact]
    public void BurstReadyFiresOnFlag()
    {
        var trigger = new BurstReadyTrigger();
        var ctx = new AutoKeyContext(1.0, 1.0, true, 0, true, DateTimeOffset.UtcNow);
        Assert.True(AutoKeyTriggers.ShouldFire(trigger, ctx));
    }

    [Fact]
    public void BossPhaseFiresOnExactMatch()
    {
        var trigger = new BossPhaseTrigger(2);
        Assert.True(AutoKeyTriggers.ShouldFire(trigger, MakeCtx(bossPhase: 2)));
        Assert.False(AutoKeyTriggers.ShouldFire(trigger, MakeCtx(bossPhase: 1)));
    }

    [Fact]
    public void StaminaBelowRespectsThreshold()
    {
        var trigger = new StaminaBelowTrigger(0.5);
        Assert.True(AutoKeyTriggers.ShouldFire(trigger, MakeCtx(stamina: 0.4)));
        Assert.False(AutoKeyTriggers.ShouldFire(trigger, MakeCtx(stamina: 0.6)));
    }

    private static AutoKeyContext MakeCtx(double hp = 1.0, double stamina = 1.0, bool burst = false, int bossPhase = 0)
        => new(hp, stamina, burst, bossPhase, true, DateTimeOffset.UtcNow);
}

public class AutoKeyCooldownGateTests
{
    [Fact]
    public void FirstFireAlwaysSucceeds()
    {
        var gate = new AutoKeyCooldownGate();
        Assert.True(gate.TryFire("a", 100, DateTimeOffset.UtcNow));
    }

    [Fact]
    public void RepeatFireWithinCooldownIsRejected()
    {
        var gate = new AutoKeyCooldownGate();
        var t = DateTimeOffset.UtcNow;
        Assert.True(gate.TryFire("a", 100, t));
        Assert.False(gate.TryFire("a", 100, t.AddMilliseconds(50)));
    }

    [Fact]
    public void FireAfterCooldownSucceeds()
    {
        var gate = new AutoKeyCooldownGate();
        var t = DateTimeOffset.UtcNow;
        gate.TryFire("a", 100, t);
        Assert.True(gate.TryFire("a", 100, t.AddMilliseconds(150)));
    }

    [Fact]
    public void DistinctActionsHaveIndependentCooldowns()
    {
        var gate = new AutoKeyCooldownGate();
        var t = DateTimeOffset.UtcNow;
        gate.TryFire("a", 100, t);
        Assert.True(gate.TryFire("b", 100, t));
    }
}
