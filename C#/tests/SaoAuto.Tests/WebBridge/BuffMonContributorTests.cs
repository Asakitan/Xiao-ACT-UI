using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

public sealed class BuffMonContributorTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public BuffMonContributorTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-buffmon-contributor-" + Guid.NewGuid().ToString("N"));
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

    [Fact]
    public void AttachContributorRegistersBuffMonCommands()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachContributor(new BuffMonContributor(), NewSettings());

        Assert.Contains(BridgeCommands.GetBuffMonEnabled, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBuffMonEnabled, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void BuffMonContributorRoutesThroughBuffMonBridge()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var settings = NewSettings();

        lifecycle.AttachContributor(new BuffMonContributor(), settings);
        var reply = lifecycle.Router.Dispatch(Cmd(
            BridgeCommands.SetBuffMonEnabled,
            new JsonObject { ["enabled"] = true }));

        Assert.True(reply!.Payload!["enabled"]!.GetValue<bool>());
        Assert.True(settings.GetBool(SettingsKeys.BuffMonEnabled));
    }

    [Fact]
    public void BuffMonContributorAttachmentCanBeDisposed()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var attachment = lifecycle.AttachContributor(new BuffMonContributor(), NewSettings());

        attachment.Dispose();

        Assert.DoesNotContain(BridgeCommands.GetBuffMonEnabled, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBuffMonEnabled, lifecycle.Router.RegisteredCommands);
    }
}
