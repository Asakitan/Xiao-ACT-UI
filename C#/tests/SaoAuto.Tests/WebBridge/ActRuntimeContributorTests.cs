using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

public sealed class ActRuntimeContributorTests : IDisposable
{
    private readonly string _workDir;

    public ActRuntimeContributorTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-act-runtime-contributor-" + Guid.NewGuid().ToString("N"));
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
    public void AttachContributorRegistersActCommands()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachContributor(new ActRuntimeContributor(), NewSettings());

        foreach (var commandName in ActBridge.CommandNames)
        {
            Assert.Contains(commandName, lifecycle.Router.RegisteredCommands);
        }
    }

    [Fact]
    public void ActRuntimeContributorRoutesEmptyActStateThroughNativeRuntime()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachContributor(new ActRuntimeContributor(), NewSettings());

        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.ActPluginsStatus));
        var payload = reply!.Payload!;

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("csharp-native", payload["source"]!.GetValue<string>());
        Assert.Equal(0, payload["plugin_count"]!.GetValue<int>());
        Assert.Empty(payload["plugins"]!.AsArray());
        Assert.Empty(payload["ui_panels"]!.AsArray());
        Assert.NotEqual("act_unavailable", payload["error"]?.GetValue<string>());
    }

    [Fact]
    public void ActRuntimeContributorPreservesUnsupportedActReplies()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachContributor(new ActRuntimeContributor(), NewSettings());

        var reply = lifecycle.Router.Dispatch(Cmd(
            BridgeCommands.ActPluginsEnable,
            new JsonObject { ["plugin_id"] = "demo" }));
        var payload = reply!.Payload!;

        Assert.False(payload["ok"]!.GetValue<bool>());
        Assert.Equal("unsupported", payload["error"]!.GetValue<string>());
        Assert.Equal(BridgeCommands.ActPluginsEnable, payload["command"]!.GetValue<string>());
    }

    [Fact]
    public void ActRuntimeContributorPassesLifecycleStateManagerToRuntime()
    {
        var states = new GameStateManager();
        states.Update(s => s with
        {
            SelfUuid = 42,
            PlayerId = "kirito-uid",
            PlayerName = "Kirito",
        });
        using var lifecycle = new WebBridgeLifecycle(states);
        lifecycle.AttachContributor(new ActRuntimeContributor(), NewSettings());

        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.ActMemSelf));
        var self = reply!.Payload!["self"]!;

        Assert.Equal("42", self["uid"]!.GetValue<string>());
        Assert.Equal("kirito-uid", self["player_uid"]!.GetValue<string>());
        Assert.Equal("Kirito", self["player_name"]!.GetValue<string>());
        Assert.Equal("gamestate", self["source"]!.GetValue<string>());
    }

    [Fact]
    public void ActRuntimeContributorAttachmentCanBeDisposed()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var attachment = lifecycle.AttachContributor(new ActRuntimeContributor(), NewSettings());

        attachment.Dispose();

        foreach (var commandName in ActBridge.CommandNames)
        {
            Assert.DoesNotContain(commandName, lifecycle.Router.RegisteredCommands);
        }
    }
}
