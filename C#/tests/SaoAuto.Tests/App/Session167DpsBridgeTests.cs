using System.Collections.Immutable;
using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.App;

/// <summary>
/// S167 — Pin <see cref="DpsBridge"/>: reset invokes the supplied
/// hook, last-report returns a formatted string + numeric metadata,
/// entity-detail returns a live skill breakdown when a tracker is wired,
/// missing snapshot returns <c>{error:"no_report"}</c>, null
/// delegates return <c>{error:"dps_unavailable"}</c> (matching the
/// runner case where the packet runtime never started), dispose
/// unregisters commands.
/// </summary>
public class Session167DpsBridgeTests
{
    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    private static DpsSnapshot SampleSnapshot() => new(
        Active: true,
        TotalDamage: 12345,
        Dps: 678,
        TotalHeal: 100,
        Hps: 5,
        DurationSeconds: 18.2,
        Rows: ImmutableArray<DpsEntitySnapshot>.Empty);

    [Fact]
    public void ResetCallsHookAndReportsTrue()
    {
        var calls = 0;
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, () => calls++, null);
        var reply = router.Dispatch(Cmd(BridgeCommands.ResetCombat));
        Assert.Equal(1, calls);
        Assert.True(reply!.Payload!["reset"]!.GetValue<bool>());
    }

    [Fact]
    public void ResetWithoutHookReturnsUnavailable()
    {
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, null, null);
        var reply = router.Dispatch(Cmd(BridgeCommands.ResetCombat));
        Assert.Equal("dps_unavailable", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void LastReportReturnsFormattedSnapshot()
    {
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, null, () => SampleSnapshot());
        var reply = router.Dispatch(Cmd(BridgeCommands.ShowLastDpsReport));
        var payload = reply!.Payload!;
        var report = payload["report"]!.GetValue<string>();
        Assert.Contains("DPS Report", report);
        Assert.Equal(12345, payload["total_damage"]!.GetValue<long>());
        Assert.Equal(18.2, payload["duration_s"]!.GetValue<double>(), 1);
    }

    [Fact]
    public void LastReportWithNullSnapshotReturnsNoReport()
    {
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, null, () => null);
        var reply = router.Dispatch(Cmd(BridgeCommands.ShowLastDpsReport));
        Assert.Equal("no_report", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void LastReportWithoutProviderReturnsUnavailable()
    {
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, null, null);
        var reply = router.Dispatch(Cmd(BridgeCommands.ShowLastDpsReport));
        Assert.Equal("dps_unavailable", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void EntityDetailWithoutTrackerReturnsUnavailable()
    {
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, null, null);
        var reply = router.Dispatch(Cmd(BridgeCommands.DpsEntityDetail,
            new JsonObject { ["uid"] = 11 }));
        Assert.Equal("dps_unavailable", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void EntityDetailReturnsLiveSkillBreakdown()
    {
        var now = DateTimeOffset.Parse("2026-06-11T01:00:00Z");
        var tracker = new DpsTracker(() => now);
        tracker.RecordDamage(
            entityUuid: 11,
            entityName: "Asuna",
            amount: 700,
            professionId: 7,
            isSelf: true,
            skillId: 42,
            skillName: "Star Slash",
            isCrit: true);
        tracker.RecordHeal(
            entityUuid: 11,
            entityName: "Asuna",
            amount: 50,
            professionId: 7,
            isSelf: true,
            skillId: 77,
            skillName: "Pulse Heal");
        now = now.AddSeconds(2);

        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, null, null, tracker: tracker);
        var reply = router.Dispatch(Cmd(BridgeCommands.DpsEntityDetail,
            new JsonObject { ["uid"] = "11" }));

        var payload = reply!.Payload!;
        Assert.Equal(11L, payload["uid"]!.GetValue<long>());
        Assert.Equal("Asuna", payload["name"]!.GetValue<string>());
        Assert.Equal(700L, payload["damage_total"]!.GetValue<long>());
        Assert.Equal(50L, payload["heal_total"]!.GetValue<long>());
        Assert.True(payload["is_self"]!.GetValue<bool>());
        Assert.Equal(1, payload["damage_hits"]!.GetValue<int>());
        Assert.Equal(1.0, payload["crit_rate"]!.GetValue<double>());
        Assert.Equal(700L, payload["max_hit"]!.GetValue<long>());

        var skills = payload["skills"]!.AsArray();
        Assert.Equal(2, skills.Count);
        var damageSkill = skills.First(s => s!["skill_id"]!.GetValue<int>() == 42)!;
        Assert.Equal("Star Slash", damageSkill["name"]!.GetValue<string>());
        Assert.Equal("Star Slash", damageSkill["skill_name"]!.GetValue<string>());
        Assert.Equal(700L, damageSkill["total"]!.GetValue<long>());
        Assert.Equal(1, damageSkill["crit_hits"]!.GetValue<int>());
    }

    [Fact]
    public void DisposeUnregistersBoth()
    {
        var router = new BridgeRouter();
        var bridge = new DpsBridge(router, () => { }, () => null);
        Assert.Contains(BridgeCommands.ResetCombat, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ShowLastDpsReport, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DpsEntityDetail, router.RegisteredCommands);
        bridge.Dispose();
        Assert.DoesNotContain(BridgeCommands.ResetCombat, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ShowLastDpsReport, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.DpsEntityDetail, router.RegisteredCommands);
    }

    [Fact]
    public void NullRouterThrows()
    {
        Assert.Throws<ArgumentNullException>(() => new DpsBridge(null!, null, null));
    }
}
