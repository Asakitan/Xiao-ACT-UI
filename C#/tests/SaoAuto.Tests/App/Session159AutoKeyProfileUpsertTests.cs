using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.App;

/// <summary>
/// S159 — Pin <see cref="AutoKeyProfileBridge"/>'s
/// <c>autokey.profile.upsert</c> command. Verifies a JSON profile
/// payload normalizes through <see cref="AutoKeyProfileSpec.NormalizeProfile"/>
/// and lands in the same on-disk config the other CRUD commands see;
/// the <c>activate</c> flag flips active id; absent profile returns a
/// structured error; subsequent upserts of the same id overwrite.
/// </summary>
public class Session159AutoKeyProfileUpsertTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public Session159AutoKeyProfileUpsertTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s159-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private AutoKeyProfileService NewService() => new(new SettingsManager(_path));

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
    public void UpsertAddsProfileAndOptionallyActivates()
    {
        var svc = NewService();
        var router = new BridgeRouter();
        using var bridge = new AutoKeyProfileBridge(svc, router);

        var reply = router.Dispatch(Cmd(BridgeCommands.UpsertAutoKeyProfile,
            new JsonObject
            {
                ["profile"] = MinimalProfileJson("p1", "First"),
                ["activate"] = true,
            }));

        Assert.NotNull(reply);
        var payload = reply!.Payload!;
        Assert.Equal("p1", payload["id"]!.GetValue<string>());
        Assert.Equal("First", payload["profile_name"]!.GetValue<string>());
        Assert.True(payload["activated"]!.GetValue<bool>());
        Assert.Equal("p1", payload["active_id"]!.GetValue<string>());
        Assert.Equal(1, payload["total"]!.GetValue<int>());

        var cfg = svc.Load();
        Assert.Single(cfg.Profiles);
        Assert.Equal("p1", cfg.ActiveProfileId);
    }

    [Fact]
    public void UpsertWithoutActivateLeavesActiveAlone()
    {
        var svc = NewService();
        var router = new BridgeRouter();
        using var bridge = new AutoKeyProfileBridge(svc, router);

        router.Dispatch(Cmd(BridgeCommands.UpsertAutoKeyProfile,
            new JsonObject { ["profile"] = MinimalProfileJson("p1", "First"), ["activate"] = true }));
        var reply = router.Dispatch(Cmd(BridgeCommands.UpsertAutoKeyProfile,
            new JsonObject { ["profile"] = MinimalProfileJson("p2", "Second") }));

        Assert.False(reply!.Payload!["activated"]!.GetValue<bool>());
        Assert.Equal("p1", reply.Payload!["active_id"]!.GetValue<string>());
        Assert.Equal(2, reply.Payload!["total"]!.GetValue<int>());
    }

    [Fact]
    public void UpsertOverwritesSameId()
    {
        var svc = NewService();
        var router = new BridgeRouter();
        using var bridge = new AutoKeyProfileBridge(svc, router);

        router.Dispatch(Cmd(BridgeCommands.UpsertAutoKeyProfile,
            new JsonObject { ["profile"] = MinimalProfileJson("p1", "First") }));
        var reply = router.Dispatch(Cmd(BridgeCommands.UpsertAutoKeyProfile,
            new JsonObject { ["profile"] = MinimalProfileJson("p1", "Renamed") }));

        Assert.Equal(1, reply!.Payload!["total"]!.GetValue<int>());
        var cfg = svc.Load();
        Assert.Single(cfg.Profiles);
        Assert.Equal("Renamed", cfg.Profiles[0].ProfileName);
    }

    [Fact]
    public void UpsertWithoutProfileReturnsError()
    {
        var svc = NewService();
        var router = new BridgeRouter();
        using var bridge = new AutoKeyProfileBridge(svc, router);

        var reply = router.Dispatch(Cmd(BridgeCommands.UpsertAutoKeyProfile,
            new JsonObject { ["activate"] = true }));
        Assert.Equal("missing_profile", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void DisposeAlsoUnregistersUpsert()
    {
        var svc = NewService();
        var router = new BridgeRouter();
        var bridge = new AutoKeyProfileBridge(svc, router);
        Assert.Contains(BridgeCommands.UpsertAutoKeyProfile, router.RegisteredCommands);
        bridge.Dispose();
        Assert.DoesNotContain(BridgeCommands.UpsertAutoKeyProfile, router.RegisteredCommands);
    }
}
