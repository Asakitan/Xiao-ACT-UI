using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S175 — Pin <see cref="BossRaidConfigStore"/>. Mirrors Python's
/// <c>load_boss_raid_config</c> / <c>save_boss_raid_config</c>: load
/// returns a normalized config (defaults when the key is missing or
/// junk); save runs the payload through normalization, writes back
/// under <c>boss_raid</c>, and persists. Snake-case round-trip must
/// preserve <c>time_s</c> exactly (no naming-policy drift).
/// </summary>
[Collection("BossRaidProfileFactories")]
public class Session175BossRaidConfigStoreTests : IDisposable
{
    private readonly Func<string, string> _origId = BossRaidProfile.NewIdFactory;
    private readonly Func<string> _origNow = BossRaidProfile.UtcNowIsoFactory;
    private readonly string _workDir;
    private int _idCounter;

    public Session175BossRaidConfigStoreTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s175-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(_workDir);
        BossRaidProfile.NewIdFactory = prefix => $"{prefix}_fake{++_idCounter:D3}";
        BossRaidProfile.UtcNowIsoFactory = () => "2026-05-20T00:00:00Z";
    }

    public void Dispose()
    {
        BossRaidProfile.NewIdFactory = _origId;
        BossRaidProfile.UtcNowIsoFactory = _origNow;
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private SettingsManager NewSettings(string? body = null)
    {
        var path = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(path, body ?? "{}");
        return new SettingsManager(path);
    }

    [Fact]
    public void NullSettingsThrows()
    {
        Assert.Throws<ArgumentNullException>(() => BossRaidConfigStore.Load(null!));
        Assert.Throws<ArgumentNullException>(() => BossRaidConfigStore.Save(null!, BossRaidProfile.DefaultConfig()));
    }

    [Fact]
    public void LoadMissingKeyReturnsDefault()
    {
        var settings = NewSettings();
        var config = BossRaidConfigStore.Load(settings);
        var def = BossRaidProfile.DefaultConfig();
        Assert.Equal(def.Enabled, config.Enabled);
        Assert.Equal(def.ActiveProfileId, config.ActiveProfileId);
        Assert.Equal(def.ServerUrl, config.ServerUrl);
        Assert.Empty(config.Profiles);
    }

    [Fact]
    public void LoadJunkKeyReturnsDefault()
    {
        var settings = NewSettings("{ \"boss_raid\": \"not-an-object\" }");
        var config = BossRaidConfigStore.Load(settings);
        Assert.False(config.Enabled);
        Assert.Empty(config.Profiles);
    }

    [Fact]
    public void SaveRoundTripsThroughSettings()
    {
        var settings = NewSettings();
        var seed = BossRaidProfile.DefaultConfig() with { Enabled = true };
        // Manufacture a profile via the normal Normalize path so IDs/timestamps line up.
        using var raw = JsonDocument.Parse(
            "{\"profile_name\":\"Test\",\"phases\":[{\"name\":\"P1\",\"timelines\":[{\"time_s\":42.5,\"label\":\"go\"}]}]}");
        var profile = BossRaidProfile.NormalizeProfile(raw.RootElement);
        seed = BossRaidProfile.UpsertProfile(seed, profile, activate: true);

        var saved = BossRaidConfigStore.Save(settings, seed);
        Assert.True(saved.Enabled);
        Assert.Single(saved.Profiles);

        // Reload from disk via a fresh SettingsManager to prove durability.
        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));
        Assert.True(reloaded.Enabled);
        Assert.Single(reloaded.Profiles);
        Assert.Equal(profile.Id, reloaded.ActiveProfileId);
        Assert.Equal(42.5, reloaded.Profiles[0].Phases[0].Timelines[0].TimeSeconds);
    }

    [Fact]
    public void SavePersistsPythonCompatibleKeys()
    {
        var settings = NewSettings();
        var seed = BossRaidProfile.DefaultConfig() with { Enabled = true };
        using var raw = JsonDocument.Parse(
            "{\"profile_name\":\"T\",\"phases\":[{\"name\":\"P1\",\"timelines\":[{\"time_s\":7.25,\"repeat_interval_s\":3}]}]}");
        var profile = BossRaidProfile.NormalizeProfile(raw.RootElement);
        seed = BossRaidProfile.UpsertProfile(seed, profile, activate: true);
        BossRaidConfigStore.Save(settings, seed);

        var onDisk = File.ReadAllText(settings.Path);
        Assert.Contains("\"time_s\"", onDisk);
        Assert.DoesNotContain("\"time_seconds\"", onDisk);
        Assert.Contains("\"enrage_time_s\"", onDisk);
        Assert.Contains("\"author_snapshot\"", onDisk);
    }

    [Fact]
    public void LoadHonorsStateSnapshotAuthor()
    {
        var settings = NewSettings(
            "{ \"boss_raid\": { \"profiles\": [ { \"profile_name\": \"A\" } ] } }");
        using var snap = JsonDocument.Parse(
            "{\"player_uid\":\"u1\",\"player_name\":\"Sora\",\"profession_id\":3,\"profession_name\":\"Mage\"}");
        var config = BossRaidConfigStore.Load(settings, snap.RootElement);
        Assert.Single(config.Profiles);
        Assert.Equal("u1", config.Profiles[0].AuthorSnapshot.PlayerUid);
        Assert.Equal("Sora", config.Profiles[0].AuthorSnapshot.PlayerName);
    }

    [Fact]
    public void SaveDoesNotClobberSiblingSettings()
    {
        var settings = NewSettings("{ \"unrelated\": 42 }");
        BossRaidConfigStore.Save(settings, BossRaidProfile.DefaultConfig());
        var onDisk = File.ReadAllText(settings.Path);
        Assert.Contains("\"unrelated\"", onDisk);
        Assert.Contains("\"boss_raid\"", onDisk);
    }
}
