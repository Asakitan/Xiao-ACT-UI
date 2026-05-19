using System.Text.Json.Nodes;
using SaoAuto.App.Startup;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.App;

/// <summary>
/// S90 — pins the App-startup wire-up that bridges
/// <see cref="ResourcePathResolver.LegacyProfile"/> →
/// <see cref="LegacyProfileMigrator"/>. The migrator itself is pinned by
/// <c>Session89LegacyProfileMigratorTests</c>; here we only check the
/// bootstrap helper anchors the right path and surfaces the result.
/// </summary>
public class Session90LegacyProfileBootstrapTests : IDisposable
{
    private readonly string _tempDir;

    public Session90LegacyProfileBootstrapTests()
    {
        _tempDir = Path.Combine(Path.GetTempPath(), $"sao_legacy_boot_{Guid.NewGuid():N}");
        Directory.CreateDirectory(_tempDir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_tempDir, recursive: true); } catch { /* best-effort */ }
    }

    private SettingsManager NewSettings() =>
        new(Path.Combine(_tempDir, "settings.json"));

    private ResourcePathResolver Resolver() => new(_tempDir, _tempDir);

    [Fact]
    public void LegacyProfile_IsAnchoredAtBaseDir()
    {
        var r = Resolver();
        Assert.Equal(Path.Combine(_tempDir, "player_profile.json"), r.LegacyProfile);
    }

    [Fact]
    public void Run_WithNoLegacyFile_ReturnsNotFound()
    {
        var s = NewSettings();
        var r = LegacyProfileBootstrap.Run(s, Resolver());
        Assert.False(r.LegacyFileFound);
        Assert.False(r.SettingsChanged);
        Assert.Null(r.Error);
    }

    [Fact]
    public void Run_WithFullLegacy_MergesAndDeletes()
    {
        var legacyPath = Path.Combine(_tempDir, "player_profile.json");
        File.WriteAllText(legacyPath, """
            {"username":"Alice","profession":"雷影剑士","level":42,"uid":"u-1","xp":777}
            """);

        var s = NewSettings();
        var r = LegacyProfileBootstrap.Run(s, Resolver());

        Assert.True(r.LegacyFileFound);
        Assert.True(r.SettingsChanged);
        Assert.True(r.LegacyFileDeleted);
        Assert.False(File.Exists(legacyPath));

        var cache = s.Get<JsonObject>(CharacterProfileStore.GameCacheKey)!;
        Assert.Equal("Alice", cache["player_name"]!.GetValue<string>());
        Assert.Equal(42, cache["level_base"]!.GetValue<int>());

        var stats = s.Get<JsonObject>(CharacterProfileStore.PlayerStatsKey)!;
        Assert.Equal(777, stats["xp"]!.GetValue<int>());
    }

    [Fact]
    public void Run_NullSettings_Throws()
    {
        Assert.Throws<ArgumentNullException>(() =>
            LegacyProfileBootstrap.Run(null!, Resolver()));
    }

    [Fact]
    public void Run_NullResolver_Throws()
    {
        Assert.Throws<ArgumentNullException>(() =>
            LegacyProfileBootstrap.Run(NewSettings(), null!));
    }
}
