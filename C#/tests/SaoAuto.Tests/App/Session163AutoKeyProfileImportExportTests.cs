using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.App;

/// <summary>
/// S163 — Pin <see cref="AutoKeyProfileBridge"/>'s
/// <c>autokey.profile.export</c> and <c>autokey.profile.import</c>
/// commands. Export returns the schema-versioned envelope JSON for an
/// existing profile and a structured error for missing ids. Import
/// parses that same envelope, assigns a fresh id, upserts it, and
/// optionally activates; malformed input returns an error instead of
/// throwing. Round-tripping export→import yields a *new* profile id,
/// matching the local-only re-import semantics in
/// <see cref="AutoKeyProfileStore.ImportProfile"/>.
/// </summary>
public class Session163AutoKeyProfileImportExportTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public Session163AutoKeyProfileImportExportTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s163-" + Guid.NewGuid().ToString("N"));
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
                new JsonObject { ["slot_index"] = 1, ["press_mode"] = "tap" },
            },
        };

    private static (AutoKeyProfileService svc, BridgeRouter router, AutoKeyProfileBridge bridge) NewBridge(string path)
    {
        var svc = new AutoKeyProfileService(new SettingsManager(path));
        var router = new BridgeRouter();
        var bridge = new AutoKeyProfileBridge(svc, router);
        return (svc, router, bridge);
    }

    [Fact]
    public void ExportReturnsEnvelopeJsonForExistingProfile()
    {
        var (svc, router, bridge) = NewBridge(_path);
        using var _ = bridge;
        router.Dispatch(Cmd(BridgeCommands.UpsertAutoKeyProfile,
            new JsonObject { ["profile"] = MinimalProfileJson("p1", "Original"), ["activate"] = true }));

        var reply = router.Dispatch(Cmd(BridgeCommands.ExportAutoKeyProfile,
            new JsonObject { ["id"] = "p1" }));

        Assert.NotNull(reply);
        Assert.Equal("p1", reply!.Payload!["id"]!.GetValue<string>());
        Assert.Equal("Original", reply.Payload!["profile_name"]!.GetValue<string>());
        var json = reply.Payload!["json"]!.GetValue<string>();
        Assert.Contains("\"schema_version\"", json);
        Assert.Contains("\"profile\"", json);
        Assert.Contains("Original", json);
    }

    [Fact]
    public void ExportMissingIdReturnsError()
    {
        var (svc, router, bridge) = NewBridge(_path);
        using var _ = bridge;
        var reply = router.Dispatch(Cmd(BridgeCommands.ExportAutoKeyProfile,
            new JsonObject { ["id"] = "ghost" }));
        Assert.Equal("not_found", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void ExportWithoutIdReturnsError()
    {
        var (svc, router, bridge) = NewBridge(_path);
        using var _ = bridge;
        var reply = router.Dispatch(Cmd(BridgeCommands.ExportAutoKeyProfile, new JsonObject()));
        Assert.Equal("missing_id", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void ImportInvalidJsonReturnsError()
    {
        var (svc, router, bridge) = NewBridge(_path);
        using var _ = bridge;
        var reply = router.Dispatch(Cmd(BridgeCommands.ImportAutoKeyProfile,
            new JsonObject { ["json"] = "{not valid" }));
        Assert.Equal("invalid_json", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void ImportMissingJsonReturnsError()
    {
        var (svc, router, bridge) = NewBridge(_path);
        using var _ = bridge;
        var reply = router.Dispatch(Cmd(BridgeCommands.ImportAutoKeyProfile, new JsonObject()));
        Assert.Equal("missing_json", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void ExportImportRoundTripCreatesNewProfile()
    {
        var (svc, router, bridge) = NewBridge(_path);
        using var _ = bridge;
        router.Dispatch(Cmd(BridgeCommands.UpsertAutoKeyProfile,
            new JsonObject { ["profile"] = MinimalProfileJson("p1", "Original"), ["activate"] = true }));

        var exportReply = router.Dispatch(Cmd(BridgeCommands.ExportAutoKeyProfile,
            new JsonObject { ["id"] = "p1" }));
        var exportedJson = exportReply!.Payload!["json"]!.GetValue<string>();

        var importReply = router.Dispatch(Cmd(BridgeCommands.ImportAutoKeyProfile,
            new JsonObject { ["json"] = exportedJson, ["activate"] = false }));

        Assert.NotNull(importReply);
        var newId = importReply!.Payload!["id"]!.GetValue<string>();
        Assert.NotEqual("p1", newId);
        Assert.Equal("Original", importReply.Payload!["profile_name"]!.GetValue<string>());
        Assert.False(importReply.Payload!["activated"]!.GetValue<bool>());
        Assert.Equal("p1", importReply.Payload!["active_id"]!.GetValue<string>());
        Assert.Equal(2, importReply.Payload!["total"]!.GetValue<int>());
    }

    [Fact]
    public void ImportWithActivateFlipsActiveId()
    {
        var (svc, router, bridge) = NewBridge(_path);
        using var _ = bridge;
        router.Dispatch(Cmd(BridgeCommands.UpsertAutoKeyProfile,
            new JsonObject { ["profile"] = MinimalProfileJson("p1", "Original"), ["activate"] = true }));
        var exported = router.Dispatch(Cmd(BridgeCommands.ExportAutoKeyProfile,
            new JsonObject { ["id"] = "p1" }))!.Payload!["json"]!.GetValue<string>();

        var reply = router.Dispatch(Cmd(BridgeCommands.ImportAutoKeyProfile,
            new JsonObject { ["json"] = exported, ["activate"] = true }));

        var newId = reply!.Payload!["id"]!.GetValue<string>();
        Assert.True(reply.Payload!["activated"]!.GetValue<bool>());
        Assert.Equal(newId, reply.Payload!["active_id"]!.GetValue<string>());
    }

    [Fact]
    public void DisposeUnregistersImportExport()
    {
        var (svc, router, bridge) = NewBridge(_path);
        Assert.Contains(BridgeCommands.ExportAutoKeyProfile, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ImportAutoKeyProfile, router.RegisteredCommands);
        bridge.Dispose();
        Assert.DoesNotContain(BridgeCommands.ExportAutoKeyProfile, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ImportAutoKeyProfile, router.RegisteredCommands);
    }
}
