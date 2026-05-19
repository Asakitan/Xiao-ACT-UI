using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.App;

/// <summary>
/// S166 — Pin <see cref="BuffMonBridge"/>'s
/// <c>buffmon.get_enabled</c> / <c>buffmon.set_enabled</c> commands:
/// settings round-trip, no-op detection via <c>changed:false</c>,
/// disk persistence, dispose unregisters.
/// </summary>
public class Session166BuffMonBridgeTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public Session166BuffMonBridgeTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s166-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void GetDefaultsToFalseOnFreshSettings()
    {
        var sm = new SettingsManager(_path);
        var router = new BridgeRouter();
        using var bridge = new BuffMonBridge(sm, router);
        var reply = router.Dispatch(Cmd(BridgeCommands.GetBuffMonEnabled));
        Assert.False(reply!.Payload!["enabled"]!.GetValue<bool>());
    }

    [Fact]
    public void SetTrueFlipsFlagAndReportsChanged()
    {
        var sm = new SettingsManager(_path);
        var router = new BridgeRouter();
        using var bridge = new BuffMonBridge(sm, router);
        var reply = router.Dispatch(Cmd(BridgeCommands.SetBuffMonEnabled,
            new JsonObject { ["enabled"] = true }));
        Assert.True(reply!.Payload!["enabled"]!.GetValue<bool>());
        Assert.True(reply.Payload!["changed"]!.GetValue<bool>());
        Assert.True(sm.GetBool(SettingsKeys.BuffMonEnabled));
    }

    [Fact]
    public void SetSameValueReportsChangedFalse()
    {
        var sm = new SettingsManager(_path);
        sm.Set(SettingsKeys.BuffMonEnabled, true);
        sm.Save();
        var router = new BridgeRouter();
        using var bridge = new BuffMonBridge(sm, router);
        var reply = router.Dispatch(Cmd(BridgeCommands.SetBuffMonEnabled,
            new JsonObject { ["enabled"] = true }));
        Assert.True(reply!.Payload!["enabled"]!.GetValue<bool>());
        Assert.False(reply.Payload!["changed"]!.GetValue<bool>());
    }

    [Fact]
    public void SetPersistsAcrossManagerInstances()
    {
        var sm1 = new SettingsManager(_path);
        var router = new BridgeRouter();
        var bridge = new BuffMonBridge(sm1, router);
        router.Dispatch(Cmd(BridgeCommands.SetBuffMonEnabled,
            new JsonObject { ["enabled"] = true }));
        bridge.Dispose();

        var sm2 = new SettingsManager(_path);
        Assert.True(sm2.GetBool(SettingsKeys.BuffMonEnabled));
    }

    [Fact]
    public void SetWithoutPayloadDefaultsToFalse()
    {
        var sm = new SettingsManager(_path);
        sm.Set(SettingsKeys.BuffMonEnabled, true);
        sm.Save();
        var router = new BridgeRouter();
        using var bridge = new BuffMonBridge(sm, router);
        var reply = router.Dispatch(Cmd(BridgeCommands.SetBuffMonEnabled, null));
        Assert.False(reply!.Payload!["enabled"]!.GetValue<bool>());
        Assert.True(reply.Payload!["changed"]!.GetValue<bool>());
    }

    [Fact]
    public void DisposeUnregistersBoth()
    {
        var sm = new SettingsManager(_path);
        var router = new BridgeRouter();
        var bridge = new BuffMonBridge(sm, router);
        Assert.Contains(BridgeCommands.SetBuffMonEnabled, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.GetBuffMonEnabled, router.RegisteredCommands);
        bridge.Dispose();
        Assert.DoesNotContain(BridgeCommands.SetBuffMonEnabled, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.GetBuffMonEnabled, router.RegisteredCommands);
    }

    [Fact]
    public void NullArgsThrow()
    {
        var sm = new SettingsManager(_path);
        var router = new BridgeRouter();
        Assert.Throws<ArgumentNullException>(() => new BuffMonBridge(null!, router));
        Assert.Throws<ArgumentNullException>(() => new BuffMonBridge(sm, null!));
    }
}
