using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.Configuration;

public class SettingsManagerTests : IDisposable
{
    private static readonly string FixturePath = Path.Combine(
        AppContext.BaseDirectory, "Fixtures", "settings.json");

    private readonly string _workDir;
    private readonly string _settingsPath;

    public SettingsManagerTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _settingsPath = Path.Combine(_workDir, "settings.json");
        File.Copy(FixturePath, _settingsPath);
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    [Fact]
    public void RoundTripPreservesEveryKnownAndUnknownKey()
    {
        var original = JsonNode.Parse(File.ReadAllText(_settingsPath))!.AsObject();

        var manager = new SettingsManager(_settingsPath);
        manager.Save();

        var afterSave = JsonNode.Parse(File.ReadAllText(_settingsPath))!.AsObject();

        foreach (var (key, _) in original)
        {
            Assert.True(afterSave.ContainsKey(key), $"key '{key}' was dropped on save");
        }

        // game_cache is a deeply nested object; assert the inner identity field round-trips as-is.
        var origGameCache = original["game_cache"]!.AsObject();
        var nextGameCache = afterSave["game_cache"]!.AsObject();
        Assert.Equal(origGameCache["player_name"]!.GetValue<string>(), nextGameCache["player_name"]!.GetValue<string>());
        Assert.Equal(origGameCache["player_id"]!.GetValue<string>(), nextGameCache["player_id"]!.GetValue<string>());
    }

    [Fact]
    public void LegacyKeysArePrunedOnSave()
    {
        // Inject a legacy key the Python SettingsManager prunes on save.
        var fixture = JsonNode.Parse(File.ReadAllText(_settingsPath))!.AsObject();
        fixture["last_file"] = "C:/old/path.mid";
        fixture["chord_mode"] = true;
        File.WriteAllText(_settingsPath, fixture.ToJsonString());

        var manager = new SettingsManager(_settingsPath);
        Assert.Equal("C:/old/path.mid", manager.GetString("last_file"));
        manager.Save();

        var afterSave = JsonNode.Parse(File.ReadAllText(_settingsPath))!.AsObject();
        Assert.False(afterSave.ContainsKey("last_file"));
        Assert.False(afterSave.ContainsKey("chord_mode"));
        // Non-legacy keys still present.
        Assert.True(afterSave.ContainsKey("ui_mode"));
        Assert.True(afterSave.ContainsKey("buffmon_enabled"));
    }

    [Fact]
    public void UnknownKeysAddedAtRuntimeAreSavedAndReloaded()
    {
        var manager = new SettingsManager(_settingsPath);
        manager.Set("future_unknown_section", new { foo = 1, bar = "baz" });
        manager.Save();

        var reloaded = new SettingsManager(_settingsPath);
        var node = reloaded.Snapshot()["future_unknown_section"]!.AsObject();
        Assert.Equal(1, node["foo"]!.GetValue<int>());
        Assert.Equal("baz", node["bar"]!.GetValue<string>());
    }

    [Fact]
    public void NormalizedUiModeUsesUiModeNormalizer()
    {
        var manager = new SettingsManager(_settingsPath);
        Assert.Equal(UiMode.Entity, manager.NormalizedUiMode); // fixture has ui_mode=entity

        manager.Set("ui_mode", "sao");
        Assert.Equal(UiMode.Entity, manager.NormalizedUiMode);

        manager.Set("ui_mode", "webview");
        Assert.Equal(UiMode.WebView, manager.NormalizedUiMode);
    }

    [Fact]
    public void MissingFileLoadsAsEmptyAndCanBeSaved()
    {
        var freshPath = Path.Combine(_workDir, "fresh.json");
        Assert.False(File.Exists(freshPath));

        var manager = new SettingsManager(freshPath);
        manager.Set("ui_mode", "entity");
        manager.Save();

        Assert.True(File.Exists(freshPath));
        var roundTrip = new SettingsManager(freshPath);
        Assert.Equal("entity", roundTrip.GetString("ui_mode"));
    }

    [Fact]
    public void StaleTempFilesAreCleanedOnLoad()
    {
        var stale = Path.Combine(_workDir, "tmpDEADBEEF.tmp.json");
        File.WriteAllText(stale, "{}");

        _ = new SettingsManager(_settingsPath);

        Assert.False(File.Exists(stale));
    }
}
