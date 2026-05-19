using System.Collections.Immutable;
using System.Text.Json.Nodes;
using SaoAuto.App.Startup;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

/// <summary>
/// S156 — Pin <see cref="AutoKeyProfileBridge"/>: each registered
/// command edits the same on-disk config via the shared
/// <see cref="AutoKeyProfileService"/>; Dispose unregisters; the
/// lifecycle wraps the service for runner composition.
/// </summary>
public class Session156AutoKeyProfileBridgeTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public Session156AutoKeyProfileBridgeTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s156-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private static AutoKeyProfileSpecRecord Profile(string id, string name = "p")
        => new(id, 1, name, "", 0, "", "local", null, "", "",
               AuthorSnapshot.Empty,
               new AutoKeyEngineConfig(50, false, true),
               ImmutableArray<AutoKeyActionSpec>.Empty);

    private AutoKeyProfileService NewService()
        => new(new SettingsManager(_path));

    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void LifecycleConstructsService()
    {
        var settings = new SettingsManager(_path);
        using var lc = new AutoKeyProfileLifecycle(settings, null);
        Assert.NotNull(lc.Service);
        var cfg = lc.Service.Load();
        Assert.NotNull(cfg);
    }

    [Fact]
    public void SetEnabledRoundTrips()
    {
        var svc = NewService();
        var router = new BridgeRouter();
        using var bridge = new AutoKeyProfileBridge(svc, router);
        var reply = router.Dispatch(Cmd(BridgeCommands.SetAutoKeyEnabled,
            new JsonObject { ["enabled"] = true }));
        Assert.NotNull(reply);
        Assert.True(reply!.Payload!["enabled"]!.GetValue<bool>());
        Assert.True(svc.Load().Enabled);
    }

    [Fact]
    public void ListReturnsSummaries()
    {
        var svc = NewService();
        svc.Upsert(Profile("p1", "First"), activate: true);
        svc.Upsert(Profile("p2", "Second"));
        var router = new BridgeRouter();
        using var bridge = new AutoKeyProfileBridge(svc, router);

        var reply = router.Dispatch(Cmd(BridgeCommands.ListAutoKeyProfiles));
        Assert.NotNull(reply);
        var payload = reply!.Payload!;
        Assert.Equal("p1", payload["active_id"]!.GetValue<string>());
        var arr = payload["profiles"]!.AsArray();
        Assert.Equal(2, arr.Count);
        Assert.Equal("First", arr[0]!["name"]!.GetValue<string>());
    }

    [Fact]
    public void SetActiveAndDelete()
    {
        var svc = NewService();
        svc.Upsert(Profile("p1"), activate: true);
        svc.Upsert(Profile("p2"));
        var router = new BridgeRouter();
        using var bridge = new AutoKeyProfileBridge(svc, router);

        var sa = router.Dispatch(Cmd(BridgeCommands.SetAutoKeyActiveProfile,
            new JsonObject { ["id"] = "p2" }));
        Assert.True(sa!.Payload!["changed"]!.GetValue<bool>());
        Assert.Equal("p2", sa.Payload!["active_id"]!.GetValue<string>());

        var del = router.Dispatch(Cmd(BridgeCommands.DeleteAutoKeyProfile,
            new JsonObject { ["id"] = "p2" }));
        Assert.Equal(1, del!.Payload!["remaining"]!.GetValue<int>());
        Assert.Equal("p1", del.Payload!["active_id"]!.GetValue<string>());
    }

    [Fact]
    public void CloneReturnsClonedIdOrErrorOnMissing()
    {
        var svc = NewService();
        svc.Upsert(Profile("p1", "Base"), activate: true);
        var router = new BridgeRouter();
        using var bridge = new AutoKeyProfileBridge(svc, router);

        var ok = router.Dispatch(Cmd(BridgeCommands.CloneAutoKeyProfile,
            new JsonObject { ["id"] = "p1" }));
        Assert.NotNull(ok!.Payload!["cloned_id"]);
        Assert.NotEqual("p1", ok.Payload!["cloned_id"]!.GetValue<string>());

        var miss = router.Dispatch(Cmd(BridgeCommands.CloneAutoKeyProfile,
            new JsonObject { ["id"] = "missing" }));
        Assert.Equal("source_not_found", miss!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void DisposeUnregistersAllCommands()
    {
        var svc = NewService();
        var router = new BridgeRouter();
        var bridge = new AutoKeyProfileBridge(svc, router);
        Assert.Contains(BridgeCommands.ListAutoKeyProfiles, router.RegisteredCommands);
        bridge.Dispose();
        Assert.DoesNotContain(BridgeCommands.ListAutoKeyProfiles, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetAutoKeyEnabled, router.RegisteredCommands);
    }

    [Fact]
    public void NullArgsThrow()
    {
        var router = new BridgeRouter();
        var settings = new SettingsManager(_path);
        Assert.Throws<ArgumentNullException>(() => new AutoKeyProfileBridge(null!, router));
        Assert.Throws<ArgumentNullException>(() =>
            new AutoKeyProfileBridge(new AutoKeyProfileService(settings), null!));
        Assert.Throws<ArgumentNullException>(() => new AutoKeyProfileLifecycle(null!, null));
    }
}
