using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S89 — Pins parity for <see cref="LegacyProfileMigrator.Migrate"/> against
/// Python <c>character_profile._migrate_legacy_profile_once</c>.
/// </summary>
public class Session89LegacyProfileMigratorTests : IDisposable
{
    private readonly string _tempDir;

    public Session89LegacyProfileMigratorTests()
    {
        _tempDir = Path.Combine(Path.GetTempPath(), $"sao_legacy_{Guid.NewGuid():N}");
        Directory.CreateDirectory(_tempDir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_tempDir, recursive: true); } catch { /* best-effort */ }
    }

    private SettingsManager NewSettings() =>
        new(Path.Combine(_tempDir, "settings.json"));

    private string WriteLegacy(object payload)
    {
        var path = Path.Combine(_tempDir, "player_profile.json");
        File.WriteAllText(path, System.Text.Json.JsonSerializer.Serialize(payload));
        return path;
    }

    [Fact]
    public void NoLegacyFile_ReturnsNotFound()
    {
        var s = NewSettings();
        var path = Path.Combine(_tempDir, "missing.json");
        var r = LegacyProfileMigrator.Migrate(s, path);
        Assert.False(r.LegacyFileFound);
        Assert.False(r.SettingsChanged);
        Assert.False(r.LegacyFileDeleted);
        Assert.Null(r.Error);
    }

    [Fact]
    public void NonObjectLegacy_NoChangesNoDelete()
    {
        var path = Path.Combine(_tempDir, "player_profile.json");
        File.WriteAllText(path, "[1,2,3]");
        var s = NewSettings();
        var r = LegacyProfileMigrator.Migrate(s, path);
        Assert.True(r.LegacyFileFound);
        Assert.False(r.SettingsChanged);
        Assert.False(r.LegacyFileDeleted);
        Assert.True(File.Exists(path));
    }

    [Fact]
    public void IdentityFields_FillEmptyCacheSlots()
    {
        var path = WriteLegacy(new
        {
            username = "Alice",
            profession = "雷影剑士",
            level = 42,
            uid = "u-12345",
            xp = 9999,
            songs_played = 3,
            play_time = 180.5,
        });

        var s = NewSettings();
        var r = LegacyProfileMigrator.Migrate(s, path);

        Assert.True(r.LegacyFileFound);
        Assert.True(r.SettingsChanged);
        Assert.True(r.LegacyFileDeleted);
        Assert.False(File.Exists(path));

        var cache = s.Get<JsonObject>(CharacterProfileStore.GameCacheKey)!;
        Assert.Equal("Alice", cache["player_name"]!.GetValue<string>());
        Assert.Equal("雷影剑士", cache["profession_name"]!.GetValue<string>());
        Assert.Equal("u-12345", cache["player_id"]!.GetValue<string>());
        Assert.Equal(42, cache["level_base"]!.GetValue<int>());

        var stats = s.Get<JsonObject>(CharacterProfileStore.PlayerStatsKey)!;
        Assert.Equal(9999, stats["xp"]!.GetValue<int>());
        Assert.Equal(3, stats["songs_played"]!.GetValue<int>());
        Assert.Equal(180.5, stats["play_time"]!.GetValue<double>());
    }

    [Fact]
    public void IdentityFields_DoNotOverwriteExisting()
    {
        var s = NewSettings();
        s.Set(CharacterProfileStore.GameCacheKey, new JsonObject
        {
            ["player_name"] = "LiveName",
            ["profession_name"] = "LiveProf",
            ["level_base"] = 99,
            ["player_id"] = "live-uid",
        });

        var path = WriteLegacy(new
        {
            username = "Alice", profession = "雷影剑士",
            level = 42, uid = "u-12345",
        });

        LegacyProfileMigrator.Migrate(s, path);

        var cache = s.Get<JsonObject>(CharacterProfileStore.GameCacheKey)!;
        Assert.Equal("LiveName", cache["player_name"]!.GetValue<string>());
        Assert.Equal("LiveProf", cache["profession_name"]!.GetValue<string>());
        Assert.Equal(99, cache["level_base"]!.GetValue<int>());
        Assert.Equal("live-uid", cache["player_id"]!.GetValue<string>());
    }

    [Fact]
    public void LevelZero_IgnoredEvenWhenSlotEmpty()
    {
        var path = WriteLegacy(new { username = "Bob", level = 0 });
        var s = NewSettings();
        LegacyProfileMigrator.Migrate(s, path);

        var cache = s.Get<JsonObject>(CharacterProfileStore.GameCacheKey)!;
        Assert.Null(cache["level_base"]);
        Assert.Equal("Bob", cache["player_name"]!.GetValue<string>());
    }

    [Fact]
    public void StatsFields_DoNotOverwriteExistingKeys()
    {
        var s = NewSettings();
        s.Set(CharacterProfileStore.PlayerStatsKey, new JsonObject
        {
            ["xp"] = 5000,
            // play_time intentionally absent → should be picked up from legacy.
        });
        var path = WriteLegacy(new { xp = 9999, songs_played = 7, play_time = 99.9 });

        LegacyProfileMigrator.Migrate(s, path);

        var stats = s.Get<JsonObject>(CharacterProfileStore.PlayerStatsKey)!;
        Assert.Equal(5000, stats["xp"]!.GetValue<int>());          // preserved
        Assert.Equal(7, stats["songs_played"]!.GetValue<int>());    // pulled from legacy
        Assert.Equal(99.9, stats["play_time"]!.GetValue<double>()); // pulled from legacy
    }

    [Fact]
    public void EmptyLegacy_StillDeletesFile()
    {
        // No useful fields → no merge happens, but file is still removed
        // so a misformed legacy doesn't trigger the migration on every launch.
        var path = WriteLegacy(new { });
        var s = NewSettings();
        var r = LegacyProfileMigrator.Migrate(s, path);

        Assert.True(r.LegacyFileFound);
        Assert.False(r.SettingsChanged);
        Assert.True(r.LegacyFileDeleted);
        Assert.False(File.Exists(path));
    }

    [Fact]
    public void SecondCall_AfterDeletion_IsNoop()
    {
        var path = WriteLegacy(new { username = "Carol" });
        var s = NewSettings();
        LegacyProfileMigrator.Migrate(s, path);

        // Second invocation finds no file → no-op.
        var r2 = LegacyProfileMigrator.Migrate(s, path);
        Assert.False(r2.LegacyFileFound);
        Assert.False(r2.SettingsChanged);
    }

    [Fact]
    public void MalformedJson_ReturnsErrorWithoutCrashing()
    {
        var path = Path.Combine(_tempDir, "player_profile.json");
        File.WriteAllText(path, "{not valid json");
        var s = NewSettings();
        var r = LegacyProfileMigrator.Migrate(s, path);

        Assert.True(r.LegacyFileFound);
        Assert.False(r.SettingsChanged);
        Assert.False(r.LegacyFileDeleted);
        Assert.NotNull(r.Error);
        Assert.True(File.Exists(path));
    }
}
