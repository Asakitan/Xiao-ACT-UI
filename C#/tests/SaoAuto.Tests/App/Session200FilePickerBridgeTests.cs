using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

public class Session200FilePickerBridgeTests : IDisposable
{
    private readonly string _workDir;

    public Session200FilePickerBridgeTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s200-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        Directory.CreateDirectory(Path.Combine(_workDir, "Beta"));
        Directory.CreateDirectory(Path.Combine(_workDir, "alpha"));
        Directory.CreateDirectory(Path.Combine(_workDir, ".hidden"));
        File.WriteAllText(Path.Combine(_workDir, "zeta.json"), "{}");
        File.WriteAllText(Path.Combine(_workDir, "Alpha.json"), "{}{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { }
    }

    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void BrowseDirListsSortedVisibleDirsAndFiles()
    {
        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir);

        var reply = router.Dispatch(Cmd(BridgeCommands.BrowseDir, new JsonObject { ["path"] = _workDir }));
        var payload = reply!.Payload!;

        Assert.Equal(Path.GetFullPath(_workDir), payload["current"]!.GetValue<string>());
        Assert.NotNull(payload["parent"]);

        var dirs = payload["dirs"]!.AsArray();
        Assert.Equal(2, dirs.Count);
        Assert.Equal("alpha", dirs[0]!["name"]!.GetValue<string>());
        Assert.Equal("Beta", dirs[1]!["name"]!.GetValue<string>());

        var files = payload["files"]!.AsArray();
        Assert.Equal(2, files.Count);
        Assert.Equal("Alpha.json", files[0]!["name"]!.GetValue<string>());
        Assert.Equal(4L, files[0]!["size"]!.GetValue<long>());
        Assert.Equal("zeta.json", files[1]!["name"]!.GetValue<string>());
    }

    [Fact]
    public void StartAutoKeyImportPickerReturnsFileModeBrowser()
    {
        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir);

        var reply = router.Dispatch(Cmd(BridgeCommands.StartAutoKeyImportPicker));
        var payload = reply!.Payload!;
        var browser = payload["browser"]!.AsObject();

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("file", browser["mode"]!.GetValue<string>());
        Assert.Equal(Path.GetFullPath(_workDir), browser["current"]!.GetValue<string>());
        Assert.NotEmpty(browser["files"]!.AsArray());
    }

    [Fact]
    public void StartBossRaidImportPickerReturnsFileModeBrowser()
    {
        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir);

        var reply = router.Dispatch(Cmd(BridgeCommands.StartBossRaidImportPicker));
        var payload = reply!.Payload!;
        var browser = payload["browser"]!.AsObject();

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("file", browser["mode"]!.GetValue<string>());
        Assert.Equal(Path.GetFullPath(_workDir), browser["current"]!.GetValue<string>());
        Assert.NotEmpty(browser["files"]!.AsArray());
    }

    [Fact]
    public void MissingPathReturnsPickerConsumableEmptyBrowser()
    {
        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir);
        var missing = Path.Combine(_workDir, "missing");

        var reply = router.Dispatch(Cmd(BridgeCommands.BrowseDir, new JsonObject { ["path"] = missing }));
        var payload = reply!.Payload!;

        Assert.Equal(Path.GetFullPath(missing), payload["current"]!.GetValue<string>());
        Assert.NotNull(payload["error"]);
        Assert.Empty(payload["dirs"]!.AsArray());
        Assert.Empty(payload["files"]!.AsArray());
    }

    [Fact]
    public void SelectFolderReturnsPythonCompatibleUnsupportedMessage()
    {
        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir);

        var reply = router.Dispatch(Cmd(BridgeCommands.SelectFolder, new JsonObject { ["path"] = _workDir }));
        var payload = reply!.Payload!;

        Assert.False(payload["ok"]!.GetValue<bool>());
        Assert.Equal("Folder selection not used here", payload["message"]!.GetValue<string>());
        Assert.Equal(_workDir, payload["path"]!.GetValue<string>());
    }

    [Fact]
    public void DisposeUnregistersCommands()
    {
        var router = new BridgeRouter();
        var bridge = new FilePickerBridge(router, () => _workDir);

        Assert.Contains(BridgeCommands.BrowseDir, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SelectFile, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SelectFolder, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StartAutoKeyImportPicker, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StartBossRaidImportPicker, router.RegisteredCommands);

        bridge.Dispose();

        Assert.DoesNotContain(BridgeCommands.BrowseDir, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SelectFile, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SelectFolder, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.StartAutoKeyImportPicker, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.StartBossRaidImportPicker, router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachRegistersAndDisposesFilePickerCommands()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachFilePicker(() => _workDir);
        lifecycle.AttachFilePicker(() => _workDir);

        Assert.Contains(BridgeCommands.BrowseDir, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SelectFile, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SelectFolder, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StartAutoKeyImportPicker, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StartBossRaidImportPicker, lifecycle.Router.RegisteredCommands);

        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.StartAutoKeyImportPicker));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());

        lifecycle.Dispose();

        Assert.DoesNotContain(BridgeCommands.BrowseDir, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SelectFile, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SelectFolder, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.StartAutoKeyImportPicker, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.StartBossRaidImportPicker, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachAfterDisposeThrows()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.Dispose();

        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachFilePicker(() => _workDir));
    }

    [Fact]
    public void SelectFileImportsAutoKeyProfileAndReturnsFreshState()
    {
        var settingsPath = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(settingsPath, "{}");
        var settings = new SettingsManager(settingsPath);
        var states = new GameStateManager();
        states.Replace(new GameState
        {
            PlayerId = "uid-ak",
            PlayerName = "Sinon",
            ProfessionId = 4,
            ProfessionName = "Bow",
        });
        var importPath = Path.Combine(_workDir, "auto-key-import.json");
        File.WriteAllText(importPath,
            "{\"schema_version\":1,\"profile\":{\"profile_name\":\"Imported AK\",\"actions\":[{\"slot_index\":2}]}}");

        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir, settings, states);

        var reply = router.Dispatch(Cmd(BridgeCommands.SelectFile, new JsonObject
        {
            ["path"] = importPath,
            ["consumer"] = "auto_key",
        }));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("auto_key", payload["consumer"]!.GetValue<string>());
        Assert.Equal("Imported AK", state["profiles_full"]!.AsArray()[0]!["profile_name"]!.GetValue<string>());
        Assert.Equal("uid-ak", state["identity"]!["player_uid"]!.GetValue<string>());

        var reloaded = AutoKeyConfigLoader.Load(new SettingsManager(settingsPath));
        Assert.Single(reloaded.Profiles);
        Assert.Equal("Imported AK", reloaded.Profiles[0].ProfileName);
    }

    [Fact]
    public void SelectFileImportsBossRaidProfileAndPreservesTimeS()
    {
        var settingsPath = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(settingsPath, "{}");
        var settings = new SettingsManager(settingsPath);
        var importPath = Path.Combine(_workDir, "boss-raid-import.json");
        File.WriteAllText(importPath, """
        {
          "schema_version": 1,
          "profile": {
            "profile_name": "Imported Raid",
            "phases": [
              { "timelines": [ { "time_s": 7.5, "label": "Stack" } ] }
            ]
          }
        }
        """);

        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir, settings, new GameStateManager());

        var reply = router.Dispatch(Cmd(BridgeCommands.SelectFile, new JsonObject
        {
            ["path"] = importPath,
            ["consumer"] = "boss_raid",
        }));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();
        var timeline = state["profiles_full"]!.AsArray()[0]!["phases"]!.AsArray()[0]!["timelines"]!.AsArray()[0]!.AsObject();

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("boss_raid", payload["consumer"]!.GetValue<string>());
        Assert.Equal("Imported Raid", state["profiles_full"]!.AsArray()[0]!["profile_name"]!.GetValue<string>());
        Assert.Equal(7.5, timeline["time_s"]!.GetValue<double>());
        Assert.False(timeline.ContainsKey("time_seconds"));

        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settingsPath));
        Assert.Single(reloaded.Profiles);
        Assert.Equal("Imported Raid", reloaded.Profiles[0].ProfileName);
    }

    [Fact]
    public void BossRaidImportPickerSetsPendingConsumerForSelectFile()
    {
        var settingsPath = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(settingsPath, "{}");
        var settings = new SettingsManager(settingsPath);
        var importPath = Path.Combine(_workDir, "boss-raid-pending-import.json");
        File.WriteAllText(importPath, """
        {
          "schema_version": 1,
          "profile": {
            "profile_name": "Pending Raid",
            "phases": [
              { "timelines": [ { "time_s": 11.0, "label": "Spread" } ] }
            ]
          }
        }
        """);

        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir, settings, new GameStateManager());

        router.Dispatch(Cmd(BridgeCommands.StartBossRaidImportPicker));
        var reply = router.Dispatch(Cmd(BridgeCommands.SelectFile, new JsonObject
        {
            ["path"] = importPath,
        }));
        var payload = reply!.Payload!;

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("boss_raid", payload["consumer"]!.GetValue<string>());
        Assert.Equal("Pending Raid", payload["state"]!["profiles_full"]!.AsArray()[0]!["profile_name"]!.GetValue<string>());
    }

    [Fact]
    public void SelectFileWithoutSettingsReturnsPythonConsumableError()
    {
        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir);

        var reply = router.Dispatch(Cmd(BridgeCommands.SelectFile, new JsonObject
        {
            ["path"] = Path.Combine(_workDir, "zeta.json"),
            ["consumer"] = "auto_key",
        }));

        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Contains("not available", reply.Payload["message"]!.GetValue<string>());
    }
}
