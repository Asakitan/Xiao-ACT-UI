using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S180 — Pin <see cref="AutoKeyCloudClient.FromSettings"/> /
/// <see cref="BossRaidCloudClient.FromSettings"/>. Both honour the
/// <c>server_url</c> field inside the per-feature config blob; when
/// missing or blank they fall back to the canonical default. Trailing
/// slashes are stripped (matches the regular constructor).
/// </summary>
public class Session180CloudFromSettingsTests : IDisposable
{
    private readonly string _workDir;

    public Session180CloudFromSettingsTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s180-" + Guid.NewGuid().ToString("N")[..8]);
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
    public void AutoKey_NullSettings_Throws()
    {
        Assert.Throws<ArgumentNullException>(() => AutoKeyCloudClient.FromSettings(null!));
    }

    [Fact]
    public void AutoKey_MissingKey_UsesDefault()
    {
        var settings = NewSettings();
        using var client = AutoKeyCloudClient.FromSettings(settings);
        Assert.Equal(AutoKeyCloudClient.DefaultServerUrl, client.BaseUrl);
    }

    [Fact]
    public void AutoKey_CustomUrl_Honored()
    {
        var settings = NewSettings("{ \"auto_key\": { \"server_url\": \"http://my-mirror:9000/\" } }");
        using var client = AutoKeyCloudClient.FromSettings(settings);
        Assert.Equal("http://my-mirror:9000", client.BaseUrl);
    }

    [Fact]
    public void AutoKey_BlankUrl_FallsBackToDefault()
    {
        var settings = NewSettings("{ \"auto_key\": { \"server_url\": \"   \" } }");
        using var client = AutoKeyCloudClient.FromSettings(settings);
        Assert.Equal(AutoKeyCloudClient.DefaultServerUrl, client.BaseUrl);
    }

    [Fact]
    public void BossRaid_NullSettings_Throws()
    {
        Assert.Throws<ArgumentNullException>(() => BossRaidCloudClient.FromSettings(null!));
    }

    [Fact]
    public void BossRaid_MissingKey_UsesDefault()
    {
        var settings = NewSettings();
        using var client = BossRaidCloudClient.FromSettings(settings);
        Assert.Equal(BossRaidProfile.DefaultServerUrl, client.BaseUrl);
    }

    [Fact]
    public void BossRaid_CustomUrl_Honored()
    {
        var settings = NewSettings("{ \"boss_raid\": { \"server_url\": \"http://mirror.example/\" } }");
        using var client = BossRaidCloudClient.FromSettings(settings);
        Assert.Equal("http://mirror.example", client.BaseUrl);
    }

    [Fact]
    public void BossRaid_BlankUrl_FallsBackToDefault()
    {
        var settings = NewSettings("{ \"boss_raid\": { \"server_url\": \"\" } }");
        using var client = BossRaidCloudClient.FromSettings(settings);
        Assert.Equal(BossRaidProfile.DefaultServerUrl, client.BaseUrl);
    }
}
