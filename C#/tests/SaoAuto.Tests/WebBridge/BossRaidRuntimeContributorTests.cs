using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

public sealed class BossRaidRuntimeContributorTests : IDisposable
{
    private readonly string _workDir;

    public BossRaidRuntimeContributorTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-bossraid-runtime-contributor-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { }
    }

    private SettingsManager NewSettings()
    {
        var path = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(path, "{}");
        return new SettingsManager(path);
    }

    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void AttachContributorRegistersBossRaidRuntimeCommands()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachContributor(new BossRaidRuntimeContributor(new BossRaidEngine()), NewSettings());

        Assert.Contains(BridgeCommands.StartBossRaid, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StopBossRaid, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RaidNextPhase, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RaidReset, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RaidSetEntityRole, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void BossRaidRuntimeContributorRoutesThroughRuntimeBridge()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var engine = new BossRaidEngine();
        var settings = NewSettings();
        var profile = BossRaidProfile.MakeDefaultProfile() with
        {
            Id = "br-contributor-start",
            ProfileName = "Contributor Boss",
            BossTotalHp = 1200,
            EnrageTimeS = 45,
            Phases = new[]
            {
                new RaidProfilePhase(
                    "phase-contributor",
                    "Contributor Phase",
                    new RaidPhaseTrigger("manual", 0),
                    Array.Empty<RaidTimelineEntry>()),
            },
        };
        BossRaidConfigStore.Save(
            settings,
            BossRaidProfile.UpsertProfile(BossRaidProfile.DefaultConfig() with { Enabled = false }, profile, activate: true));

        lifecycle.AttachContributor(new BossRaidRuntimeContributor(engine), settings);
        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.StartBossRaid));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();
        var runtime = state["runtime"]!.AsObject();
        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.True(payload["running"]!.GetValue<bool>());
        Assert.True(engine.Running);
        Assert.True(reloaded.Enabled);
        Assert.Equal("Contributor Phase", payload["phase_name"]!.GetValue<string>());
        Assert.Equal("Contributor Boss", runtime["profile_name"]!.GetValue<string>());
        Assert.Equal("Contributor Boss", state["active_profile_name"]!.GetValue<string>());
    }

    [Fact]
    public void BossRaidRuntimeContributorAttachmentCanBeDisposed()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var attachment = lifecycle.AttachContributor(new BossRaidRuntimeContributor(new BossRaidEngine()), NewSettings());

        attachment.Dispose();

        Assert.DoesNotContain(BridgeCommands.StartBossRaid, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.StopBossRaid, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RaidNextPhase, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RaidReset, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RaidSetEntityRole, lifecycle.Router.RegisteredCommands);
    }
}
