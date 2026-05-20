using System.Collections.Immutable;
using System.Text.Json;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S181 — Pin <see cref="AutoKeyConfig.LastRemoteSearch"/> round-trip
/// through <see cref="AutoKeyConfigLoader"/>. Matches Python's
/// <c>last_remote_search</c> structure in
/// <c>default_auto_key_config()</c> + <c>normalize_auto_key_config()</c>.
/// </summary>
public class Session181AutoKeyLastRemoteSearchTests : IDisposable
{
    private readonly string _workDir;

    public Session181AutoKeyLastRemoteSearchTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s181-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(_workDir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private SettingsManager NewSettings(string? body = null)
    {
        var path = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(path, body ?? "{}");
        return new SettingsManager(path);
    }

    [Fact]
    public void DefaultConfigHasEmptyRemoteSearch()
    {
        var d = AutoKeyProfileStore.DefaultConfig();
        Assert.NotNull(d.LastRemoteSearch);
        Assert.Equal(string.Empty, d.LastRemoteSearch.Query.Q);
        Assert.Equal(1, d.LastRemoteSearch.Query.Page);
        Assert.Equal(20, d.LastRemoteSearch.Query.PageSize);
        Assert.Empty(d.LastRemoteSearch.Results);
        Assert.Equal(string.Empty, d.LastRemoteSearch.Error);
        Assert.Equal(string.Empty, d.LastRemoteSearch.FetchedAt);
    }

    [Fact]
    public void NormalizeReadsRemoteSearchFields()
    {
        using var doc = JsonDocument.Parse("""
        {
          "last_remote_search": {
            "query": {
              "q": "fire",
              "profile_name": "pf",
              "player_uid": "uid1",
              "player_name": "Sora",
              "profession_name": "Mage",
              "page": 3,
              "page_size": 50
            },
            "results": [{"id":"r1"},{"id":"r2"}],
            "error": "",
            "fetched_at": "2026-05-20T00:00:00Z"
          }
        }
        """);
        var cfg = AutoKeyProfileStore.NormalizeConfig(doc.RootElement);
        var s = cfg.LastRemoteSearch;
        Assert.Equal("fire", s.Query.Q);
        Assert.Equal("Sora", s.Query.PlayerName);
        Assert.Equal(3, s.Query.Page);
        Assert.Equal(50, s.Query.PageSize);
        Assert.Equal(2, s.Results.Length);
        Assert.Equal("2026-05-20T00:00:00Z", s.FetchedAt);
    }

    [Fact]
    public void NormalizeClampsPageAndPageSize()
    {
        using var doc = JsonDocument.Parse("""
        {
          "last_remote_search": {
            "query": { "page": -5, "page_size": 999 }
          }
        }
        """);
        var cfg = AutoKeyProfileStore.NormalizeConfig(doc.RootElement);
        Assert.Equal(1, cfg.LastRemoteSearch.Query.Page);
        Assert.Equal(100, cfg.LastRemoteSearch.Query.PageSize);
    }

    [Fact]
    public void NormalizeJunkRemoteSearchYieldsDefault()
    {
        using var doc = JsonDocument.Parse("{ \"last_remote_search\": \"not-an-object\" }");
        var cfg = AutoKeyProfileStore.NormalizeConfig(doc.RootElement);
        Assert.Equal(AutoKeyRemoteSearch.Default, cfg.LastRemoteSearch);
    }

    [Fact]
    public void SaveAndReloadRoundTripsRemoteSearch()
    {
        var settings = NewSettings();
        var seed = AutoKeyProfileStore.DefaultConfig() with
        {
            LastRemoteSearch = new AutoKeyRemoteSearch(
                Query: new AutoKeyRemoteQuery(
                    Q: "burst",
                    ProfileName: "p",
                    PlayerUid: "u",
                    PlayerName: "n",
                    ProfessionName: "Mage",
                    Page: 2,
                    PageSize: 25),
                Results: ImmutableArray<JsonElement>.Empty,
                Error: "",
                FetchedAt: "2026-05-20T12:00:00Z"),
        };
        AutoKeyConfigLoader.Save(settings, seed);
        settings.Save();

        var reloaded = AutoKeyConfigLoader.Load(new SettingsManager(settings.Path));
        Assert.Equal("burst", reloaded.LastRemoteSearch.Query.Q);
        Assert.Equal(2, reloaded.LastRemoteSearch.Query.Page);
        Assert.Equal(25, reloaded.LastRemoteSearch.Query.PageSize);
        Assert.Equal("2026-05-20T12:00:00Z", reloaded.LastRemoteSearch.FetchedAt);
    }

    [Fact]
    public void SavePersistsPythonCompatibleKeys()
    {
        var settings = NewSettings();
        var seed = AutoKeyProfileStore.DefaultConfig() with
        {
            LastRemoteSearch = AutoKeyRemoteSearch.Default with
            {
                FetchedAt = "2026-05-20T00:00:00Z",
            },
        };
        AutoKeyConfigLoader.Save(settings, seed);
        settings.Save();

        var onDisk = File.ReadAllText(settings.Path);
        Assert.Contains("\"last_remote_search\"", onDisk);
        Assert.Contains("\"page_size\"", onDisk);
        Assert.Contains("\"fetched_at\"", onDisk);
        Assert.DoesNotContain("\"PageSize\"", onDisk);
    }
}
