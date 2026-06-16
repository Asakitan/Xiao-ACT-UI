using SaoAuto.Core.Updater;
using SaoAuto.UpdateHost;

namespace SaoAuto.Tests.Updater;

public class ManifestStoreTests : IDisposable
{
    private readonly string _path;

    public ManifestStoreTests()
    {
        _path = Path.Combine(Path.GetTempPath(), $"sao_manifests_{Guid.NewGuid():N}.json");
    }

    public void Dispose()
    {
        try { if (File.Exists(_path)) File.Delete(_path); } catch { }
    }

    private static UpdateManifest Make(string ver, string ch = "stable", string tg = "windows-x64") =>
        new(ver, ch, tg, $"https://example.com/{ver}.zip",
            "deadbeef", 1024, UpdatePackageKind.Full, $"notes-{ver}", DateTimeOffset.UnixEpoch.AddDays(1));

    [Fact]
    public void Publish_RoundTripsToDisk()
    {
        var s1 = new ManifestStore(_path);
        s1.Publish(Make("5.0.0"));
        s1.Publish(Make("5.0.1", ch: "beta"));

        Assert.True(File.Exists(_path));

        var s2 = new ManifestStore(_path);
        Assert.Equal("5.0.0", s2.Latest("stable", "windows-x64")!.Version);
        Assert.Equal("5.0.1", s2.Latest("beta", "windows-x64")!.Version);
        Assert.Null(s2.Latest("nope", "windows-x64"));
        Assert.Equal(2, s2.Summary().Count);
    }

    [Fact]
    public void Publish_OverwritesExistingChannelTarget()
    {
        var s = new ManifestStore(_path);
        s.Publish(Make("5.0.0"));
        s.Publish(Make("5.0.1"));
        Assert.Equal("5.0.1", s.Latest("stable", "windows-x64")!.Version);
        Assert.Single(s.Summary());
    }

    [Fact]
    public void Load_TolratesMissingFile()
    {
        var s = new ManifestStore(_path);
        Assert.Null(s.Latest("stable", "windows-x64"));
        Assert.Empty(s.Summary());
    }

    [Fact]
    public void Load_TolratesCorruptFile()
    {
        File.WriteAllText(_path, "}}}not json{{{");
        var s = new ManifestStore(_path);
        Assert.Empty(s.Summary());
        s.Publish(Make("9.9.9"));
        Assert.Equal("9.9.9", s.Latest("stable", "windows-x64")!.Version);
    }
}
