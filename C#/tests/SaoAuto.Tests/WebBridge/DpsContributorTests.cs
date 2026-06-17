using System.Collections.Immutable;
using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

public sealed class DpsContributorTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public DpsContributorTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-dps-contributor-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { }
    }

    private SettingsManager NewSettings() => new(_path);

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
    public void AttachContributorRegistersDpsCommands()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachContributor(new DpsContributor(() => { }, () => null), NewSettings());

        Assert.Contains(BridgeCommands.ResetCombat, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ShowLastDpsReport, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DpsEntityDetail, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DpsToggleEnabled, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void DpsContributorRoutesResetAndReportThroughDpsBridge()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var calls = 0;

        lifecycle.AttachContributor(new DpsContributor(() => calls++, () => SampleSnapshot()), NewSettings());

        var reset = lifecycle.Router.Dispatch(Cmd(BridgeCommands.ResetCombat));
        var report = lifecycle.Router.Dispatch(Cmd(BridgeCommands.ShowLastDpsReport));

        Assert.Equal(1, calls);
        Assert.True(reset!.Payload!["reset"]!.GetValue<bool>());
        Assert.Contains("DPS Report", report!.Payload!["report"]!.GetValue<string>());
        Assert.Equal(12345L, report.Payload!["total_damage"]!.GetValue<long>());
    }

    [Fact]
    public void DpsContributorRoutesEntityDetailThroughInjectedTracker()
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
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachContributor(new DpsContributor(tracker: tracker), NewSettings());
        var reply = lifecycle.Router.Dispatch(Cmd(
            BridgeCommands.DpsEntityDetail,
            new JsonObject { ["uid"] = "11" }));

        var payload = reply!.Payload!;
        Assert.Equal(11L, payload["uid"]!.GetValue<long>());
        Assert.Equal("Asuna", payload["name"]!.GetValue<string>());
        Assert.Equal(700L, payload["damage_total"]!.GetValue<long>());
        Assert.Equal(1, payload["damage_hits"]!.GetValue<int>());
    }

    [Fact]
    public void DpsContributorRoutesToggleThroughContextSettings()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var settings = NewSettings();

        lifecycle.AttachContributor(new DpsContributor(), settings);
        var reply = lifecycle.Router.Dispatch(Cmd(
            BridgeCommands.DpsToggleEnabled,
            new JsonObject { ["enabled"] = false }));

        Assert.False(reply!.Payload!["enabled"]!.GetValue<bool>());
        Assert.False(settings.Get<bool?>(SettingsKeys.DpsEnabled) ?? true);
    }

    [Fact]
    public void DpsContributorPreservesNullDelegateBehavior()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachContributor(new DpsContributor(), NewSettings());
        var reset = lifecycle.Router.Dispatch(Cmd(BridgeCommands.ResetCombat));
        var report = lifecycle.Router.Dispatch(Cmd(BridgeCommands.ShowLastDpsReport));
        var detail = lifecycle.Router.Dispatch(Cmd(
            BridgeCommands.DpsEntityDetail,
            new JsonObject { ["uid"] = 11 }));

        Assert.Equal("dps_unavailable", reset!.Payload!["error"]!.GetValue<string>());
        Assert.Equal("dps_unavailable", report!.Payload!["error"]!.GetValue<string>());
        Assert.Equal("dps_unavailable", detail!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void DpsContributorAttachmentCanBeDisposed()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var attachment = lifecycle.AttachContributor(new DpsContributor(() => { }, () => null), NewSettings());

        attachment.Dispose();

        Assert.DoesNotContain(BridgeCommands.ResetCombat, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ShowLastDpsReport, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.DpsEntityDetail, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.DpsToggleEnabled, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void NullContextThrows()
    {
        var contributor = new DpsContributor();

        Assert.Throws<ArgumentNullException>(() => contributor.Attach(null!));
    }
}
