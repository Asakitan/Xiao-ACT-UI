using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

public sealed class AutoKeyProfileContributorTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public AutoKeyProfileContributorTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-autokey-profile-contributor-" + Guid.NewGuid().ToString("N"));
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

    private static JsonObject MinimalProfileJson(string id, string name)
        => new()
        {
            ["id"] = id,
            ["profile_name"] = name,
            ["actions"] = new JsonArray
            {
                new JsonObject
                {
                    ["slot_index"] = 1,
                    ["press_mode"] = "tap",
                },
            },
        };

    [Fact]
    public void AttachContributorRegistersAutoKeyProfileCommands()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachContributor(new AutoKeyProfileContributor(), NewSettings());

        Assert.Contains(BridgeCommands.SetAutoKeyEnabled, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ListAutoKeyProfiles, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetAutoKeyActiveProfile, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DeleteAutoKeyProfile, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.CloneAutoKeyProfile, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.UpsertAutoKeyProfile, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ExportAutoKeyProfile, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ImportAutoKeyProfile, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AutoKeyProfileContributorRoutesThroughAutoKeyProfileBridge()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachContributor(new AutoKeyProfileContributor(), NewSettings());
        var upsert = lifecycle.Router.Dispatch(Cmd(
            BridgeCommands.UpsertAutoKeyProfile,
            new JsonObject
            {
                ["profile"] = MinimalProfileJson("p1", "First"),
                ["activate"] = true,
            }));
        var list = lifecycle.Router.Dispatch(Cmd(BridgeCommands.ListAutoKeyProfiles));

        Assert.Equal("p1", upsert!.Payload!["id"]!.GetValue<string>());
        Assert.True(upsert.Payload!["activated"]!.GetValue<bool>());
        Assert.Equal("p1", list!.Payload!["active_id"]!.GetValue<string>());
        var profiles = list.Payload!["profiles"]!.AsArray();
        Assert.Single(profiles);
        Assert.Equal("First", profiles[0]!["name"]!.GetValue<string>());
    }

    [Fact]
    public void AutoKeyProfileContributorAttachmentCanBeDisposed()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var attachment = lifecycle.AttachContributor(new AutoKeyProfileContributor(), NewSettings());

        attachment.Dispose();

        Assert.DoesNotContain(BridgeCommands.SetAutoKeyEnabled, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ListAutoKeyProfiles, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetAutoKeyActiveProfile, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.DeleteAutoKeyProfile, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.CloneAutoKeyProfile, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.UpsertAutoKeyProfile, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ExportAutoKeyProfile, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ImportAutoKeyProfile, lifecycle.Router.RegisteredCommands);
    }
}
