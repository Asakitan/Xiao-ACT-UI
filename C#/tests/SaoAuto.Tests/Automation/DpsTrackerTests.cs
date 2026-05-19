using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

public class DpsTrackerTests
{
    [Fact]
    public void EmptyTrackerSnapshotsInactive()
    {
        var tracker = new DpsTracker();
        var snap = tracker.Snapshot();
        Assert.False(snap.Active);
        Assert.Empty(snap.Rows);
    }

    [Fact]
    public void DamageAccumulatesAcrossEntities()
    {
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var tracker = new DpsTracker(() => clock.Now);

        tracker.RecordDamage(1, "Self", 1000, professionId: 12, isSelf: true);
        clock.Advance(TimeSpan.FromSeconds(2));
        tracker.RecordDamage(2, "Mate", 500, professionId: 4, isSelf: false);

        var snap = tracker.Snapshot();
        Assert.True(snap.Active);
        Assert.Equal(1500, snap.TotalDamage);
        Assert.Equal(2, snap.Rows.Length);
        // Sorted by damage descending → Self first.
        Assert.Equal(1000, snap.Rows[0].Damage);
        Assert.Equal("Self", snap.Rows[0].EntityName);
    }

    [Fact]
    public void DpsDividesByElapsedSeconds()
    {
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var tracker = new DpsTracker(() => clock.Now);
        tracker.RecordDamage(1, "Self", 1000, 12, true);
        clock.Advance(TimeSpan.FromSeconds(5));
        tracker.RecordDamage(1, "Self", 4000, 12, true);

        var snap = tracker.Snapshot();
        Assert.Equal(5000, snap.TotalDamage);
        Assert.Equal(1000, snap.Dps); // 5000 / 5s
    }

    [Fact]
    public void HealsTrackedSeparately()
    {
        var tracker = new DpsTracker();
        tracker.RecordHeal(1, "Healer", 800, 5, false);
        tracker.RecordDamage(1, "Healer", 200, 5, false);
        var snap = tracker.Snapshot();
        Assert.Equal(200, snap.TotalDamage);
        Assert.Equal(800, snap.TotalHeal);
    }

    [Fact]
    public void ResetClearsState()
    {
        var tracker = new DpsTracker();
        tracker.RecordDamage(1, "x", 100, 1, false);
        tracker.Reset();
        Assert.False(tracker.Snapshot().Active);
    }

    [Fact]
    public void HealAloneActivatesEncounter()
    {
        // Wired via EncounterTracker (S52): a heal-only encounter is Active.
        var tracker = new DpsTracker();
        tracker.RecordHeal(1, "Healer", 500, 5, false);
        var snap = tracker.Snapshot();
        Assert.True(snap.Active);
        Assert.Equal(0, snap.TotalDamage);
        Assert.Equal(500, snap.TotalHeal);
    }

    [Fact]
    public void DurationFloorsToMinElapsedOnSingleEvent()
    {
        // EncounterTracker.MinElapsedSeconds = 0.001 → Snapshot reports 0.001
        // when start == end (single event) instead of dividing by zero.
        var tracker = new DpsTracker();
        tracker.RecordDamage(1, "self", 100, 7, true);
        var snap = tracker.Snapshot();
        Assert.True(snap.DurationSeconds >= 0.001);
        Assert.True(snap.DurationSeconds < 0.01);
    }

    [Fact]
    public void IdleGapDoesNotCloseEncounterUntilFinalize()
    {
        // Python parity (per S50): only an explicit reset / FinalizeIfIdle
        // closes the window. Snapshots between events still report Active=true.
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var tracker = new DpsTracker(() => clock.Now);
        tracker.RecordDamage(1, "self", 100, 7, true);
        clock.Advance(TimeSpan.FromMinutes(2));
        var snap = tracker.Snapshot();
        Assert.True(snap.Active);
    }

    private sealed class TestClock
    {
        public DateTimeOffset Now { get; private set; }
        public TestClock(DateTimeOffset s) => Now = s;
        public void Advance(TimeSpan d) => Now = Now.Add(d);
    }
}
