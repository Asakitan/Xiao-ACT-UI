using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

public class DpsTrackerReportingTests
{
    [Fact]
    public void SkillBreakdownAggregatesPerSkillTotalsAndCrits()
    {
        var dps = new DpsTracker();
        dps.RecordDamage(1, "self", 100, 7, true, skillId: 42, skillName: "fireball");
        dps.RecordDamage(1, "self", 200, 7, true, skillId: 42, skillName: "fireball", isCrit: true);
        dps.RecordDamage(1, "self",  50, 7, true, skillId: 99, skillName: "shock");

        var rows = dps.SkillBreakdown(1);
        Assert.Equal(2, rows.Count);
        var fb = rows.Single(r => r.SkillId == 42);
        Assert.Equal(300, fb.Total);
        Assert.Equal(2, fb.Hits);
        Assert.Equal(1, fb.CritHits);
        Assert.Equal(0.5, fb.CritRate);
        Assert.Equal(200, fb.MaxHit);
    }

    [Fact]
    public void FinalizeIfIdleReturnsNullBeforeThreshold()
    {
        var now = DateTimeOffset.UtcNow;
        var dps = new DpsTracker(() => now);
        dps.RecordDamage(1, "self", 500, 7, true);
        Assert.Null(dps.FinalizeIfIdle(TimeSpan.FromSeconds(5)));
    }

    [Fact]
    public void FinalizeIfIdleStoresLastReportAndClearsLive()
    {
        var now = DateTimeOffset.UtcNow;
        var dps = new DpsTracker(() => now);
        dps.RecordDamage(1, "self", 500, 7, true, skillId: 1, skillName: "slash");
        now = now.AddSeconds(10);

        var report = dps.FinalizeIfIdle(TimeSpan.FromSeconds(5));
        Assert.NotNull(report);
        Assert.Equal("idle_timeout", report!.ReportReason);
        Assert.Equal(500, report.TotalDamage);
        Assert.Single(report.Rows);
        Assert.Single(report.Rows[0].Skills);
        Assert.Same(report, dps.LastReport);
        // Live state was reset.
        var live = dps.Snapshot();
        Assert.False(live.Active);
    }

    [Fact]
    public void PollOverlayStateReturnsFadeOutSnapshotWhenIdleFinalizes()
    {
        var now = DateTimeOffset.UtcNow;
        var dps = new DpsTracker(() => now);
        dps.RecordDamage(1, "self", 500, 7, true);
        now = now.AddSeconds(10);

        var poll = dps.PollOverlayState(TimeSpan.FromSeconds(5));

        Assert.False(poll.HasLive);
        Assert.True(poll.ShouldFadeOut);
        Assert.True(poll.HasReport);
        Assert.Equal(500, poll.Snapshot.TotalDamage);
        Assert.Same(poll.Snapshot, dps.LastReport);
    }

    [Fact]
    public void FormatReportProducesHeaderAndPerEntityRows()
    {
        var now = DateTimeOffset.UtcNow;
        var dps = new DpsTracker(() => now);
        dps.RecordDamage(1, "self", 800, 7, true);
        dps.RecordDamage(2, "ally", 200, 7, false);
        now = now.AddSeconds(10);
        var snap = dps.Snapshot();
        var text = DpsTracker.FormatReport(snap);

        Assert.Contains("DPS Report", text);
        Assert.Contains("Total damage:", text);
        Assert.Contains("self", text);
        Assert.Contains("ally", text);
        Assert.Contains("80.0%", text); // self share
    }

    [Fact]
    public void FormatReportIncludesReasonWhenFinalized()
    {
        var now = DateTimeOffset.UtcNow;
        var dps = new DpsTracker(() => now);
        dps.RecordDamage(1, "self", 100, 7, true);
        now = now.AddSeconds(20);
        var report = dps.FinalizeIfIdle(TimeSpan.FromSeconds(5), reason: "boss_killed");
        Assert.NotNull(report);
        var text = DpsTracker.FormatReport(report!);
        Assert.Contains("Reason:", text);
        Assert.Contains("boss_killed", text);
    }
}
