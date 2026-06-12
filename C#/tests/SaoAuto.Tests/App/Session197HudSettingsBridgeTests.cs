using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.App;

public class Session197HudSettingsBridgeTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public Session197HudSettingsBridgeTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s197-" + Guid.NewGuid().ToString("N"));
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
    public void WatchedSlotsNormalizeAndPersist()
    {
        var settings = new SettingsManager(_path);
        var router = new BridgeRouter();
        using var bridge = new HudSettingsBridge(router, settings);

        var reply = router.Dispatch(Cmd(BridgeCommands.SetWatchedSlots, new JsonObject
        {
            ["slots"] = new JsonArray(3, 1, 3, 0, 10, "5"),
        }));

        var slots = reply!.Payload!["slots"]!.AsArray();
        Assert.Equal(new[] { 3, 1, 5 }, slots.Select(s => s!.GetValue<int>()).ToArray());

        var reloaded = new SettingsManager(_path);
        Assert.Equal(new[] { 3, 1, 5 }, WatchedSkillSlotsLoader.Load(reloaded));
    }

    [Fact]
    public void BurstEnabledPersistsBoolean()
    {
        var settings = new SettingsManager(_path);
        var router = new BridgeRouter();
        using var bridge = new HudSettingsBridge(router, settings);

        var reply = router.Dispatch(Cmd(BridgeCommands.SetBurstEnabled, new JsonObject
        {
            ["enabled"] = false,
        }));

        Assert.False(reply!.Payload!["enabled"]!.GetValue<bool>());
        var reloaded = new SettingsManager(_path);
        Assert.False(reloaded.GetBool(SettingsKeys.BurstEnabled, defaultValue: true));
    }

    [Fact]
    public void DisposeUnregistersCommands()
    {
        var router = new BridgeRouter();
        var bridge = new HudSettingsBridge(router, new SettingsManager(_path));
        Assert.Contains(BridgeCommands.SetWatchedSlots, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBurstEnabled, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBossBarMode, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetDpsFadeTimeout, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetDataSource, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetComponentSource, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SaveAutoKeyActions, router.RegisteredCommands);

        bridge.Dispose();

        Assert.DoesNotContain(BridgeCommands.SetWatchedSlots, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBurstEnabled, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBossBarMode, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetDpsFadeTimeout, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetDataSource, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetComponentSource, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SaveAutoKeyActions, router.RegisteredCommands);
    }

    [Fact]
    public void BossBarModeNormalizesAndPersists()
    {
        var settings = new SettingsManager(_path);
        var router = new BridgeRouter();
        using var bridge = new HudSettingsBridge(router, settings);

        var reply = router.Dispatch(Cmd(BridgeCommands.SetBossBarMode, new JsonObject
        {
            ["mode"] = "invalid",
        }));

        Assert.Equal("boss_raid", reply!.Payload!["mode"]!.GetValue<string>());
        var reloaded = new SettingsManager(_path);
        Assert.Equal("boss_raid", reloaded.GetString(SettingsKeys.BossBarMode));
    }

    [Fact]
    public void DpsFadeTimeoutClampsAndPersists()
    {
        var settings = new SettingsManager(_path);
        var router = new BridgeRouter();
        using var bridge = new HudSettingsBridge(router, settings);

        var reply = router.Dispatch(Cmd(BridgeCommands.SetDpsFadeTimeout, new JsonObject
        {
            ["seconds"] = -5,
        }));

        Assert.Equal(0, reply!.Payload!["seconds"]!.GetValue<int>());
        var reloaded = new SettingsManager(_path);
        Assert.Equal(0, reloaded.GetInt(SettingsKeys.DpsFadeTimeoutSeconds, defaultValue: 5));
    }

    [Fact]
    public void DataSourceNormalizesAndPersists()
    {
        var settings = new SettingsManager(_path);
        var router = new BridgeRouter();
        using var bridge = new HudSettingsBridge(router, settings);

        var reply = router.Dispatch(Cmd(BridgeCommands.SetDataSource, new JsonObject
        {
            ["mode"] = "bad-source",
        }));

        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("hybrid", reply.Payload["mode"]!.GetValue<string>());
        Assert.False(reply.Payload["live_reconfigured"]!.GetValue<bool>());
        var reloaded = new SettingsManager(_path);
        Assert.Equal("hybrid", reloaded.GetString(SettingsKeys.MemDataSource));
    }

    [Fact]
    public void ComponentSourceNormalizesAndPersists()
    {
        var settings = new SettingsManager(_path);
        var router = new BridgeRouter();
        using var bridge = new HudSettingsBridge(router, settings);

        var reply = router.Dispatch(Cmd(BridgeCommands.SetComponentSource, new JsonObject
        {
            ["component"] = "level",
            ["mode"] = "screen",
        }));

        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("level", reply.Payload["component"]!.GetValue<string>());
        Assert.Equal("vision", reply.Payload["mode"]!.GetValue<string>());
        Assert.False(reply.Payload["live_reconfigured"]!.GetValue<bool>());

        var reloaded = new SettingsManager(_path);
        var saved = reloaded.Get<JsonObject>(SettingsKeys.DataSourceMap)!;
        Assert.Equal("vision", saved["level"]!.GetValue<string>());
        Assert.Equal("vision", saved["stamina"]!.GetValue<string>());
        Assert.Equal("packet", saved["skills"]!.GetValue<string>());
        Assert.Equal("mixed", reloaded.GetString(SettingsKeys.DataSource));
    }

    [Fact]
    public void ComponentSourceRejectsUnknownComponent()
    {
        var router = new BridgeRouter();
        using var bridge = new HudSettingsBridge(router, new SettingsManager(_path));

        var reply = router.Dispatch(Cmd(BridgeCommands.SetComponentSource, new JsonObject
        {
            ["component"] = "boss",
            ["mode"] = "packet",
        }));

        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("bad_component", reply.Payload["error"]!.GetValue<string>());
        var reloaded = new SettingsManager(_path);
        Assert.Null(reloaded.Get<JsonObject?>(SettingsKeys.DataSourceMap));
    }

    [Fact]
    public void SaveAutoKeyActionsPersistsJsonArray()
    {
        var settings = new SettingsManager(_path);
        var router = new BridgeRouter();
        using var bridge = new HudSettingsBridge(router, settings);

        var reply = router.Dispatch(Cmd(BridgeCommands.SaveAutoKeyActions, new JsonObject
        {
            ["actions_json"] = "[{\"trigger_slot\":1,\"action_slot\":3}]",
        }));

        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal(1, reply.Payload["saved_count"]!.GetValue<int>());
        Assert.False(reply.Payload["live_reconfigured"]!.GetValue<bool>());

        var reloaded = new SettingsManager(_path);
        var saved = reloaded.Get<JsonArray>("autokey_burst_actions")!;
        Assert.Single(saved);
        Assert.Equal(3, saved[0]!["action_slot"]!.GetValue<int>());
    }

    [Fact]
    public void SaveAutoKeyActionsRejectsNonArrayJson()
    {
        var router = new BridgeRouter();
        using var bridge = new HudSettingsBridge(router, new SettingsManager(_path));

        var reply = router.Dispatch(Cmd(BridgeCommands.SaveAutoKeyActions, new JsonObject
        {
            ["actions_json"] = "{\"not\":\"array\"}",
        }));

        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("bad_payload", reply.Payload["error"]!.GetValue<string>());
    }
}
