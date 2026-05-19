using System.Text.Json;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

[Collection("BossRaidProfileFactories")]
public class BossRaidProfileIoTests : IDisposable
{
    private readonly Func<string, string> _origId = BossRaidProfile.NewIdFactory;
    private readonly Func<string> _origNow = BossRaidProfile.UtcNowIsoFactory;
    private readonly Func<DateTime> _origLocal = BossRaidProfileIo.LocalNowFactory;
    private readonly string _tempDir;
    private int _idCounter;

    public BossRaidProfileIoTests()
    {
        _tempDir = Path.Combine(Path.GetTempPath(), "sao_brio_" + Guid.NewGuid().ToString("N")[..8]);
        BossRaidProfile.NewIdFactory = prefix => $"{prefix}_fake{++_idCounter:D3}";
        BossRaidProfile.UtcNowIsoFactory = () => "2026-05-06T12:34:56Z";
        BossRaidProfileIo.LocalNowFactory = () => new DateTime(2026, 5, 6, 13, 5, 7);
    }

    public void Dispose()
    {
        BossRaidProfile.NewIdFactory = _origId;
        BossRaidProfile.UtcNowIsoFactory = _origNow;
        BossRaidProfileIo.LocalNowFactory = _origLocal;
        if (Directory.Exists(_tempDir)) Directory.Delete(_tempDir, recursive: true);
    }

    [Fact]
    public void EnsureExportDirCreatesDirectory()
    {
        var dir = Path.Combine(_tempDir, "exports");
        Assert.False(Directory.Exists(dir));
        var returned = BossRaidProfileIo.EnsureExportDir(dir);
        Assert.True(Directory.Exists(dir));
        Assert.Equal(dir, returned);
    }

    [Fact]
    public void ExportProfileToDefaultPathWritesStampedFile()
    {
        var profile = BossRaidProfile.MakeDefaultProfile() with { ProfileName = "火 Boss" };
        var path = BossRaidProfileIo.ExportProfileToDefaultPath(profile, _tempDir);

        Assert.True(File.Exists(path));
        Assert.Contains("火_Boss_20260506_130507.json", path);
        var content = File.ReadAllText(path);
        Assert.Contains("\"schema_version\"", content);
        Assert.Contains("火 Boss", content);
    }

    [Fact]
    public void ImportProfileFromPathReadsAndRegeneratesIds()
    {
        var dir = Directory.CreateDirectory(_tempDir).FullName;
        var path = Path.Combine(dir, "in.json");
        File.WriteAllText(path,
            "{\"schema_version\":1,\"profile\":{\"id\":\"orig\",\"profile_name\":\"X\"," +
            "\"remote_id\":\"r1\",\"source\":\"downloaded\",\"phases\":[" +
            "{\"id\":\"p_old\",\"timelines\":[{\"id\":\"tl_old\",\"label\":\"a\"}]}]}}");

        var p = BossRaidProfileIo.ImportProfileFromPath(path);
        Assert.NotEqual("orig", p.Id);
        Assert.StartsWith("boss_fake", p.Id);
        Assert.Null(p.RemoteId);
        Assert.Equal("local", p.Source);
        Assert.Equal("2026-05-06T12:34:56Z", p.CreatedAt);
        Assert.Equal("2026-05-06T12:34:56Z", p.UpdatedAt);
        Assert.NotEqual("p_old", p.Phases[0].Id);
        Assert.NotEqual("tl_old", p.Phases[0].Timelines[0].Id);
    }

    [Fact]
    public void ImportProfileAcceptsBareProfileObject()
    {
        // No "profile" wrapper — Python falls back to treating root as profile.
        var dir = Directory.CreateDirectory(_tempDir).FullName;
        var path = Path.Combine(dir, "bare.json");
        File.WriteAllText(path, "{\"profile_name\":\"Bare\",\"phases\":[]}");
        var p = BossRaidProfileIo.ImportProfileFromPath(path);
        Assert.Equal("Bare", p.ProfileName);
        Assert.Single(p.Phases);
    }

    [Fact]
    public void BuildBossRaidStateExposesActiveProfileFields()
    {
        var c = BossRaidProfile.DefaultConfig();
        var p = BossRaidProfile.MakeDefaultProfile() with { Id = "a", ProfileName = "Alpha" };
        c = BossRaidProfile.UpsertProfile(c, p, activate: true);

        var view = BossRaidProfileIo.BuildBossRaidState(c);
        Assert.Equal("a", view.ActiveProfileId);
        Assert.Equal("Alpha", view.ActiveProfileName);
        Assert.Equal(1, view.LocalProfileCount);
        Assert.NotNull(view.ActiveProfile);
        Assert.Equal("a", view.ActiveProfile!.Id);
        Assert.Single(view.Profiles);
        Assert.Single(view.ProfilesFull);
    }

    [Fact]
    public void BuildBossRaidStateFallsBackToDefaultServerUrl()
    {
        var c = BossRaidProfile.DefaultConfig() with { ServerUrl = "" };
        var view = BossRaidProfileIo.BuildBossRaidState(c);
        Assert.Equal(BossRaidProfile.DefaultServerUrl, view.ServerUrl);
    }

    [Fact]
    public void RoundTripExportThenImportYieldsEquivalentProfile()
    {
        var src = BossRaidProfile.NormalizeProfile(JsonDocument.Parse(
            "{\"profile_name\":\"R\",\"phases\":[{\"timelines\":[{\"label\":\"go\"}]}]}").RootElement);
        var path = BossRaidProfileIo.ExportProfileToDefaultPath(src, _tempDir);
        var imported = BossRaidProfileIo.ImportProfileFromPath(path);
        Assert.Equal(src.ProfileName, imported.ProfileName);
        Assert.Equal(src.Phases.Count, imported.Phases.Count);
        Assert.Equal("go", imported.Phases[0].Timelines[0].Label);
        Assert.NotEqual(src.Id, imported.Id); // import always regenerates
    }
}
