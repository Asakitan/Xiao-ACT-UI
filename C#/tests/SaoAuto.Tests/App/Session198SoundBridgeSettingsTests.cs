using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.App;

public class Session198SoundBridgeSettingsTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _settingsPath;

    public Session198SoundBridgeSettingsTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s198-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _settingsPath = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_settingsPath, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    private SoundCatalog Catalog() => new(_workDir);

    [Fact]
    public void ConstructorAppliesPersistedSoundSettings()
    {
        File.WriteAllText(_settingsPath, """{ "sound_enabled": false, "sound_volume": 250 }""");
        var settings = new SettingsManager(_settingsPath);
        var player = new NullSoundPlayer { Enabled = true, VolumePct = 70 };
        var router = new BridgeRouter();

        using var bridge = new SoundBridge(router, player, Catalog(), settings);

        Assert.False(player.Enabled);
        Assert.Equal(100, player.VolumePct);
    }

    [Fact]
    public void SetSoundEnabledUpdatesPlayerAndPersists()
    {
        var settings = new SettingsManager(_settingsPath);
        var player = new NullSoundPlayer { Enabled = true };
        var router = new BridgeRouter();
        using var bridge = new SoundBridge(router, player, Catalog(), settings);

        var reply = router.Dispatch(Cmd(BridgeCommands.SetSoundEnabled, new JsonObject
        {
            ["enabled"] = false,
        }));

        Assert.False(player.Enabled);
        Assert.False(reply!.Payload!["enabled"]!.GetValue<bool>());
        var reloaded = new SettingsManager(_settingsPath);
        Assert.False(reloaded.GetBool(SettingsKeys.SoundEnabled, defaultValue: true));
    }

    [Fact]
    public void SetSoundVolumeClampsUpdatesPlayerAndPersists()
    {
        var settings = new SettingsManager(_settingsPath);
        var player = new NullSoundPlayer { VolumePct = 70 };
        var router = new BridgeRouter();
        using var bridge = new SoundBridge(router, player, Catalog(), settings);

        var reply = router.Dispatch(Cmd(BridgeCommands.SetSoundVolume, new JsonObject
        {
            ["volume"] = 130,
        }));

        Assert.Equal(100, player.VolumePct);
        Assert.Equal(100, reply!.Payload!["volume"]!.GetValue<int>());
        var reloaded = new SettingsManager(_settingsPath);
        Assert.Equal(100, reloaded.GetInt(SettingsKeys.SoundVolume, defaultValue: 70));
    }

    [Fact]
    public void DisposeUnregistersSoundSettingCommands()
    {
        var router = new BridgeRouter();
        var bridge = new SoundBridge(router, new NullSoundPlayer(), Catalog());
        Assert.Contains(BridgeCommands.SetSoundEnabled, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetSoundVolume, router.RegisteredCommands);

        bridge.Dispose();

        Assert.DoesNotContain(BridgeCommands.SetSoundEnabled, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetSoundVolume, router.RegisteredCommands);
    }
}
