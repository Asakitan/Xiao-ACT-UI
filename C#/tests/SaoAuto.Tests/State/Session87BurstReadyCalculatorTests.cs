using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

/// <summary>
/// S87 — Pins parity for <see cref="BurstReadyCalculator.Compute"/> against
/// Python <c>game_state.compute_burst_ready</c>.
/// </summary>
public class Session87BurstReadyCalculatorTests
{
    private static SkillSlot Slot(
        int index,
        SkillSlotState state = SkillSlotState.Cooldown,
        bool active = false,
        int chargeCount = 0,
        int remainingMs = 1000,
        double cooldownPct = 1.0) =>
        new()
        {
            Index = index,
            State = state,
            Active = active,
            ChargeCount = chargeCount,
            RemainingMs = remainingMs,
            CooldownPct = cooldownPct,
        };

    [Fact]
    public void NullSlots_False()
    {
        Assert.False(BurstReadyCalculator.Compute(null, new[] { 1 }));
    }

    [Fact]
    public void NullWatched_False()
    {
        Assert.False(BurstReadyCalculator.Compute(new[] { Slot(1) }, null));
    }

    [Fact]
    public void EmptyWatched_False()
    {
        Assert.False(BurstReadyCalculator.Compute(new[] { Slot(1, state: SkillSlotState.Ready) }, Array.Empty<int>()));
    }

    [Fact]
    public void WatchedAllNonPositive_False()
    {
        // Python filters out watched ids <= 0; if nothing remains, returns False.
        Assert.False(BurstReadyCalculator.Compute(new[] { Slot(1) }, new[] { 0, -1 }));
    }

    [Fact]
    public void NoWatchedSlotsPresentInSnapshot_False()
    {
        // Watch slot 5 but live snapshot only has slot 1 → matched empty → False.
        Assert.False(BurstReadyCalculator.Compute(new[] { Slot(1, state: SkillSlotState.Ready) }, new[] { 5 }));
    }

    [Fact]
    public void StateReady_CountsAsReady()
    {
        Assert.True(BurstReadyCalculator.Compute(
            new[] { Slot(1, state: SkillSlotState.Ready) }, new[] { 1 }));
    }

    [Fact]
    public void StateActive_CountsAsReady()
    {
        Assert.True(BurstReadyCalculator.Compute(
            new[] { Slot(1, state: SkillSlotState.Active) }, new[] { 1 }));
    }

    [Fact]
    public void ActiveFlag_CountsAsReady()
    {
        Assert.True(BurstReadyCalculator.Compute(
            new[] { Slot(1, active: true) }, new[] { 1 }));
    }

    [Fact]
    public void ChargeCountPositive_CountsAsReady()
    {
        Assert.True(BurstReadyCalculator.Compute(
            new[] { Slot(1, chargeCount: 1) }, new[] { 1 }));
    }

    [Fact]
    public void RemainingMsAtBoundary_ReadyAt120NotAt121()
    {
        Assert.True(BurstReadyCalculator.Compute(
            new[] { Slot(1, remainingMs: 120) }, new[] { 1 }));
        Assert.False(BurstReadyCalculator.Compute(
            new[] { Slot(1, remainingMs: 121) }, new[] { 1 }));
    }

    [Fact]
    public void CooldownPctAtBoundary_ReadyAt002NotAbove()
    {
        Assert.True(BurstReadyCalculator.Compute(
            new[] { Slot(1, remainingMs: 1000, cooldownPct: 0.02) }, new[] { 1 }));
        Assert.False(BurstReadyCalculator.Compute(
            new[] { Slot(1, remainingMs: 1000, cooldownPct: 0.03) }, new[] { 1 }));
    }

    [Fact]
    public void AllWatchedReady_True_OneNotReady_False()
    {
        var slots = new[]
        {
            Slot(1, state: SkillSlotState.Ready),
            Slot(2, chargeCount: 2),
            Slot(3, state: SkillSlotState.Cooldown, remainingMs: 5000, cooldownPct: 0.5),
        };
        Assert.True(BurstReadyCalculator.Compute(new[] { slots[0], slots[1] }, new[] { 1, 2 }));
        // Slot 3 is on cooldown → watching {1,2,3} should fail.
        Assert.False(BurstReadyCalculator.Compute(slots, new[] { 1, 2, 3 }));
    }

    [Fact]
    public void DuplicateWatchedIndices_DeDupedLikePython()
    {
        // Python builds a set; C# port mirrors that.
        Assert.True(BurstReadyCalculator.Compute(
            new[] { Slot(1, state: SkillSlotState.Ready) }, new[] { 1, 1, 1 }));
    }

    [Fact]
    public void UnwatchedNotReadySlot_IgnoredEvenIfPresent()
    {
        var slots = new[]
        {
            Slot(1, state: SkillSlotState.Ready),
            Slot(2, state: SkillSlotState.Cooldown, remainingMs: 9000, cooldownPct: 0.9),
        };
        Assert.True(BurstReadyCalculator.Compute(slots, new[] { 1 }));
    }
}
