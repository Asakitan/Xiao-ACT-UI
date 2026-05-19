using System.Collections.Immutable;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S144 — Pin <see cref="AutoKeyProfileService"/>: every mutator
/// must persist through <see cref="SettingsManager.Save"/> so a fresh
/// reload sees the change. Concurrent calls must serialise without
/// corrupting the on-disk config.
/// </summary>
public class Session144AutoKeyProfileServiceTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public Session144AutoKeyProfileServiceTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s144-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private AutoKeyProfileSpecRecord Profile(string id, string name = "p")
        => new(id, 1, name, "", 0, "", "local", null, "", "",
               AuthorSnapshot.Empty,
               new AutoKeyEngineConfig(50, false, true),
               ImmutableArray<AutoKeyActionSpec>.Empty);

    private AutoKeyProfileService MakeService()
        => new(new SettingsManager(_path));

    private AutoKeyConfig ReloadFromDisk()
        => AutoKeyConfigLoader.Load(new SettingsManager(_path));

    [Fact]
    public void UpsertPersistsThroughReload()
    {
        var svc = MakeService();
        svc.Upsert(Profile("p1", "First"), activate: true);

        var reloaded = ReloadFromDisk();
        Assert.Single(reloaded.Profiles);
        Assert.Equal("p1", reloaded.ActiveProfileId);
        Assert.Equal("First", reloaded.Profiles[0].ProfileName);
    }

    [Fact]
    public void DeletePersistsAndPromotesActive()
    {
        var svc = MakeService();
        svc.Upsert(Profile("p1"), activate: true);
        svc.Upsert(Profile("p2"));

        svc.Delete("p1");

        var reloaded = ReloadFromDisk();
        Assert.Single(reloaded.Profiles);
        Assert.Equal("p2", reloaded.Profiles[0].Id);
        Assert.Equal("p2", reloaded.ActiveProfileId);
    }

    [Fact]
    public void CloneReturnsClonedAndPersists()
    {
        var svc = MakeService();
        svc.Upsert(Profile("p1", "Base"), activate: true);

        var clone = svc.Clone("p1");

        Assert.NotNull(clone);
        Assert.NotEqual("p1", clone!.Id);
        var reloaded = ReloadFromDisk();
        Assert.Equal(2, reloaded.Profiles.Length);
        Assert.Equal("p1", reloaded.ActiveProfileId); // clone does not activate
    }

    [Fact]
    public void CloneOfMissingReturnsNull()
    {
        var svc = MakeService();
        Assert.Null(svc.Clone("missing"));
    }

    [Fact]
    public void SetActiveSwitchesPersistedId()
    {
        var svc = MakeService();
        svc.Upsert(Profile("p1"), activate: true);
        svc.Upsert(Profile("p2"));

        svc.SetActive("p2");

        Assert.Equal("p2", ReloadFromDisk().ActiveProfileId);
    }

    [Fact]
    public void SetActiveIgnoresMissingId()
    {
        var svc = MakeService();
        svc.Upsert(Profile("p1"), activate: true);

        svc.SetActive("nope");

        Assert.Equal("p1", ReloadFromDisk().ActiveProfileId);
    }

    [Fact]
    public void SetEnabledFlipsPersistedFlag()
    {
        var svc = MakeService();
        Assert.False(ReloadFromDisk().Enabled);

        svc.SetEnabled(true);

        Assert.True(ReloadFromDisk().Enabled);
    }

    [Fact]
    public void ConcurrentUpsertsAllPersist()
    {
        var svc = MakeService();
        Parallel.For(0, 32, i => svc.Upsert(Profile($"p{i}")));

        var reloaded = ReloadFromDisk();
        Assert.Equal(32, reloaded.Profiles.Length);
    }
}
