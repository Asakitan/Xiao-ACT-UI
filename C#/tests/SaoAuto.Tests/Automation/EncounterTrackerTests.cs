using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

public class EncounterTrackerTests
{
    [Fact]
    public void New_tracker_is_inactive()
    {
        var t = new EncounterTracker();
        Assert.False(t.Active);
        Assert.Equal(0.0, t.StartedAt);
        Assert.Equal(0.0, t.EndedAt);
        Assert.Equal(EncounterTracker.MinElapsedSeconds, t.ElapsedSeconds);
    }

    [Fact]
    public void OnDamage_opens_window_on_first_event()
    {
        var t = new EncounterTracker();
        t.OnDamage(100, now: 10.0);
        Assert.True(t.Active);
        Assert.Equal(10.0, t.StartedAt);
        Assert.Equal(10.0, t.EndedAt);
        Assert.Equal(100, t.TotalDamage);
    }

    [Fact]
    public void OnHeal_alone_opens_window_and_marks_active()
    {
        var t = new EncounterTracker();
        t.OnHeal(50, now: 5.0);
        Assert.True(t.Active);
        Assert.Equal(50, t.TotalHeal);
        Assert.Equal(0, t.TotalDamage);
    }

    [Fact]
    public void Subsequent_events_advance_end_only()
    {
        var t = new EncounterTracker();
        t.OnDamage(100, 10.0);
        t.OnDamage(200, 15.0);
        t.OnHeal(50, 20.0);
        Assert.Equal(10.0, t.StartedAt);
        Assert.Equal(20.0, t.EndedAt);
        Assert.Equal(300, t.TotalDamage);
        Assert.Equal(50, t.TotalHeal);
    }

    [Fact]
    public void Zero_or_negative_amounts_are_ignored()
    {
        var t = new EncounterTracker();
        t.OnDamage(0, 10.0);
        t.OnDamage(-5, 10.0);
        t.OnHeal(0, 10.0);
        Assert.False(t.Active);
        Assert.Equal(0.0, t.StartedAt);
    }

    [Fact]
    public void ElapsedSeconds_returns_min_floor_when_window_closed()
    {
        var t = new EncounterTracker();
        Assert.Equal(0.001, t.ElapsedSeconds);
    }

    [Fact]
    public void ElapsedSeconds_returns_min_floor_when_start_equals_end()
    {
        var t = new EncounterTracker();
        t.OnDamage(100, 10.0);
        // Single event → end == start → raw elapsed = 0 → floor 0.001
        Assert.Equal(0.001, t.ElapsedSeconds);
    }

    [Fact]
    public void ElapsedSeconds_returns_real_span_when_above_floor()
    {
        var t = new EncounterTracker();
        t.OnDamage(100, 10.0);
        t.OnDamage(100, 15.5);
        Assert.Equal(5.5, t.ElapsedSeconds, 3);
    }

    [Fact]
    public void Reset_clears_all_state()
    {
        var t = new EncounterTracker();
        t.OnDamage(100, 10.0);
        t.OnHeal(50, 11.0);
        t.Reset();
        Assert.False(t.Active);
        Assert.Equal(0.0, t.StartedAt);
        Assert.Equal(0.0, t.EndedAt);
        Assert.Equal(0, t.TotalDamage);
        Assert.Equal(0, t.TotalHeal);
    }

    [Fact]
    public void Snapshot_projects_python_field_layout()
    {
        var t = new EncounterTracker();
        t.OnDamage(1000, 10.0);
        t.OnDamage(1000, 20.0);
        var snap = t.Snapshot();
        Assert.True(snap.Active);
        Assert.Equal(10.0, snap.StartedAt);
        Assert.Equal(20.0, snap.EndedAt);
        Assert.Equal(10.0, snap.ElapsedSeconds);   // round(10.0, 1) → 10.0
        Assert.Equal(2000, snap.TotalDamage);
        Assert.Equal(0, snap.TotalHeal);
        Assert.Equal(200, snap.Dps);                // 2000 / 10 = 200
        Assert.Equal(0, snap.Hps);
    }

    [Fact]
    public void Snapshot_dps_uses_truncation_like_python_int_cast()
    {
        var t = new EncounterTracker();
        t.OnDamage(7, 10.0);
        t.OnDamage(0, 13.0);   // ignored — but won't advance EndedAt either
        // Need a real 2nd event to push end forward.
        t.OnDamage(7, 13.0);
        // elapsed = 3.0, total damage = 14, dps = int(14/3) = 4
        var snap = t.Snapshot();
        Assert.Equal(4, snap.Dps);
    }

    [Fact]
    public void Snapshot_total_damage_override_for_boss_focused_dps()
    {
        // Python uses `display_damage = total_damage_boss if (boss_uuid && total_damage_boss>0) else total_damage`.
        // The C# tracker doesn't own the boss split; callers pass it in.
        var t = new EncounterTracker();
        t.OnDamage(1000, 10.0);
        t.OnDamage(1000, 20.0);
        var snap = t.Snapshot(totalDamageOverride: 600);
        Assert.Equal(600, snap.TotalDamage);
        Assert.Equal(60, snap.Dps); // 600/10 = 60
    }

    [Fact]
    public void Snapshot_when_inactive_marks_active_false()
    {
        var t = new EncounterTracker();
        var snap = t.Snapshot();
        Assert.False(snap.Active);
        // Inactive: total damage 0 → dps 0 even though elapsed floor is 0.001.
        Assert.Equal(0, snap.Dps);
    }

    [Fact]
    public void Snapshot_inactive_elapsed_rounds_to_zero()
    {
        var t = new EncounterTracker();
        var snap = t.Snapshot();
        // round(0.001, 1) = 0.0 (Python parity).
        Assert.Equal(0.0, snap.ElapsedSeconds);
        Assert.Equal(0, snap.Dps);
        Assert.Equal(0, snap.Hps);
    }
}
