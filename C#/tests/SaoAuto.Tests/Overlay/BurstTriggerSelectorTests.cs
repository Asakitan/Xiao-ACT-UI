using SaoAuto.Overlay;
using Xunit;

namespace SaoAuto.Tests.Overlay;

public class BurstTriggerSelectorTests
{
    private static BurstSlot S(int idx, string state, double cd = 1.0, bool edge = false)
        => new(idx, state, cd, edge);

    [Fact]
    public void Empty_inputs_return_zero()
    {
        Assert.Equal(0, BurstTriggerSelector.Pick(null, null, 0));
        Assert.Equal(0, BurstTriggerSelector.Pick(Array.Empty<BurstSlot>(), null, 0));
    }

    [Fact]
    public void Watched_defaults_to_slot_one_when_empty()
    {
        var slots = new[] { S(1, "ready"), S(2, "ready") };
        Assert.Equal(1, BurstTriggerSelector.Pick(slots, null, 0));
        Assert.Equal(1, BurstTriggerSelector.Pick(slots, Array.Empty<int>(), 0));
    }

    [Fact]
    public void Edge_wins_over_everything()
    {
        var slots = new[]
        {
            S(1, "ready"),
            S(2, "cooldown", 0.5, edge: true),
            S(3, "active"),
        };
        Assert.Equal(2, BurstTriggerSelector.Pick(slots, new[] { 1, 2, 3 }, 3));
    }

    [Fact]
    public void Prev_still_ok_beats_first_ready()
    {
        var slots = new[] { S(1, "ready"), S(2, "ready") };
        Assert.Equal(2, BurstTriggerSelector.Pick(slots, new[] { 1, 2 }, 2));
    }

    [Fact]
    public void First_ready_beats_first_active()
    {
        var slots = new[] { S(1, "active"), S(2, "ready") };
        Assert.Equal(2, BurstTriggerSelector.Pick(slots, new[] { 1, 2 }, 0));
    }

    [Fact]
    public void First_active_beats_first_low_cd()
    {
        var slots = new[] { S(1, "cooldown", 0.0), S(2, "active", 0.5) };
        // slot 1: cd=0 makes it low_cd, but slot 2 is active → active wins
        Assert.Equal(2, BurstTriggerSelector.Pick(slots, new[] { 1, 2 }, 0));
    }

    [Fact]
    public void First_low_cd_falls_back_when_no_ready_or_active()
    {
        var slots = new[] { S(1, "cooldown", 0.5), S(2, "cooldown", 0.01) };
        Assert.Equal(2, BurstTriggerSelector.Pick(slots, new[] { 1, 2 }, 0));
    }

    [Fact]
    public void Edge_skipped_when_slot_not_in_watched()
    {
        var slots = new[] { S(2, "cooldown", 0.5, edge: true), S(1, "ready") };
        Assert.Equal(1, BurstTriggerSelector.Pick(slots, new[] { 1 }, 0));
    }

    [Theory]
    [InlineData(0.02, true)]
    [InlineData(0.0201, false)]
    [InlineData(0.0, true)]
    public void Low_cd_threshold_is_inclusive_at_002(double cd, bool shouldPick)
    {
        var slots = new[] { S(1, "cooldown", cd) };
        var picked = BurstTriggerSelector.Pick(slots, new[] { 1 }, 0);
        Assert.Equal(shouldPick ? 1 : 0, picked);
    }

    [Fact]
    public void State_string_is_trimmed_and_lowercased()
    {
        var slots = new[] { S(1, "  READY  ") };
        Assert.Equal(1, BurstTriggerSelector.Pick(slots, new[] { 1 }, 0));
    }

    [Fact]
    public void Non_positive_index_is_skipped()
    {
        var slots = new[] { S(0, "ready"), S(-1, "ready"), S(2, "ready") };
        Assert.Equal(2, BurstTriggerSelector.Pick(slots, new[] { 2 }, 0));
    }

    [Fact]
    public void Prev_slot_not_ready_does_not_short_circuit()
    {
        var slots = new[] { S(1, "ready"), S(2, "cooldown", 0.5) };
        // prev=2 is not ready → falls through to first_ready=1
        Assert.Equal(1, BurstTriggerSelector.Pick(slots, new[] { 1, 2 }, 2));
    }
}
