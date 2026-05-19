using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S136 — AutoKeyConfigLoader is the settings-side counterpart of Python's
/// <c>load_auto_key_config</c>. These facts pin the missing-key default,
/// state-author derivation, round-trip via Save, and that the loader is the
/// only thing the App layer needs to bridge SettingsManager → AutoKeyConfig.
/// </summary>
public class AutoKeyConfigLoaderTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public AutoKeyConfigLoaderTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s136-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    [Fact]
    public void MissingNodeYieldsDefaultConfig()
    {
        var settings = new SettingsManager(_path);
        var cfg = AutoKeyConfigLoader.Load(settings);

        Assert.False(cfg.Enabled);
        Assert.Equal("", cfg.ActiveProfileId);
        Assert.Empty(cfg.Profiles);
        Assert.Equal(AutoKeyProfileSpec.DefaultServerUrl, cfg.ServerUrl);
    }

    [Fact]
    public void EnabledFlagAndServerUrlAreReadFromSettings()
    {
        File.WriteAllText(_path, """{"auto_key":{"enabled":true,"server_url":"https://example.test/api"}}""");
        var settings = new SettingsManager(_path);

        var cfg = AutoKeyConfigLoader.Load(settings);

        Assert.True(cfg.Enabled);
        Assert.Equal("https://example.test/api", cfg.ServerUrl);
    }

    [Fact]
    public void ProfilesAreNormalizedAndActiveIdResolves()
    {
        File.WriteAllText(_path, """
        {
          "auto_key": {
            "enabled": true,
            "active_profile_id": "p1",
            "profiles": [
              { "id": "p1", "profile_name": "Combat", "actions": [] },
              { "id": "p2", "profile_name": "Dungeon", "actions": [] }
            ]
          }
        }
        """);
        var settings = new SettingsManager(_path);

        var cfg = AutoKeyConfigLoader.Load(settings);

        Assert.Equal(2, cfg.Profiles.Length);
        Assert.Equal("p1", cfg.ActiveProfileId);
        Assert.NotNull(AutoKeyProfileStore.ActiveProfile(cfg));
        Assert.Equal("Combat", AutoKeyProfileStore.ActiveProfile(cfg)!.ProfileName);
    }

    [Fact]
    public void OrphanActiveIdFallsBackToFirstProfile()
    {
        File.WriteAllText(_path, """
        {
          "auto_key": {
            "active_profile_id": "missing",
            "profiles": [
              { "id": "p1", "profile_name": "Combat", "actions": [] }
            ]
          }
        }
        """);
        var settings = new SettingsManager(_path);

        var cfg = AutoKeyConfigLoader.Load(settings);

        Assert.Equal("p1", cfg.ActiveProfileId);
    }

    [Fact]
    public void AuthorFromStateMirrorsPythonHelper()
    {
        var state = new GameState
        {
            PlayerId = "uid42",
            PlayerName = "Kirito",
            ProfessionId = 7,
            ProfessionName = "Spellblade",
        };

        var author = AutoKeyConfigLoader.AuthorFromState(state);

        Assert.Equal("uid42", author.PlayerUid);
        Assert.Equal("Kirito", author.PlayerName);
        Assert.Equal(7, author.ProfessionId);
        Assert.Equal("Spellblade", author.ProfessionName);
    }

    [Fact]
    public void LoadWithStateAuthorStampsNewProfilesFromGameState()
    {
        // A profile with no author block AND missing id triggers
        // NormalizeProfile's authorFallback path.
        File.WriteAllText(_path, """
        {
          "auto_key": {
            "profiles": [
              { "profile_name": "Fresh", "actions": [] }
            ]
          }
        }
        """);
        var settings = new SettingsManager(_path);
        var state = new GameState
        {
            PlayerId = "uid99",
            PlayerName = "Asuna",
            ProfessionId = 3,
            ProfessionName = "Maker",
        };

        var cfg = AutoKeyConfigLoader.LoadWithStateAuthor(settings, state);

        Assert.Single(cfg.Profiles);
        var p = cfg.Profiles[0];
        Assert.Equal("uid99", p.AuthorSnapshot.PlayerUid);
        Assert.Equal("Asuna", p.AuthorSnapshot.PlayerName);
        Assert.Equal("Maker", p.AuthorSnapshot.ProfessionName);
        Assert.False(string.IsNullOrEmpty(p.Id));
    }

    [Fact]
    public void SaveRoundTripsThroughSettingsManager()
    {
        var settings = new SettingsManager(_path);
        var cfg = AutoKeyConfigLoader.Load(settings);
        var updated = cfg with { Enabled = true, ServerUrl = "https://saved.test/v1" };

        AutoKeyConfigLoader.Save(settings, updated);
        settings.Save();

        var reloaded = new SettingsManager(_path);
        var roundtrip = AutoKeyConfigLoader.Load(reloaded);

        Assert.True(roundtrip.Enabled);
        Assert.Equal("https://saved.test/v1", roundtrip.ServerUrl);
    }

    [Fact]
    public void ExplicitlyNullAutoKeyNodeStillYieldsDefault()
    {
        File.WriteAllText(_path, """{"auto_key":null}""");
        var settings = new SettingsManager(_path);

        var cfg = AutoKeyConfigLoader.Load(settings);

        Assert.False(cfg.Enabled);
        Assert.Empty(cfg.Profiles);
    }
}
