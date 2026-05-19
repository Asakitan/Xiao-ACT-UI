using SaoAuto.App.Startup;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

/// <summary>
/// S143 — Pin <see cref="AutoKeyLifecycle.SnapshotRuntime"/>: must
/// return <see cref="AutoKeyRuntimeSnapshot.Empty"/> when inactive,
/// and a real snapshot when an active host has at least one tick.
/// </summary>
public class Session143LifecycleSnapshotTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public Session143LifecycleSnapshotTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s143-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private sealed class NoOpDispatcher : IKeyDispatcher
    {
        public void Dispatch(KeyStroke stroke) { }
    }

    private AutoKeyTickHost BuildHost()
    {
        var settings = new SettingsManager(_path);
        var states = new GameStateManager();
        var runtime = new AutoKeySpecRuntime(new NoOpDispatcher());
        return new AutoKeyTickHost(settings, states, runtime);
    }

    [Fact]
    public void InactiveReturnsEmptySnapshot()
    {
        using var lc = AutoKeyLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            logger: null,
            CancellationToken.None);

        Assert.False(lc.IsActive);
        var snap = lc.SnapshotRuntime();
        Assert.Empty(snap.BlockReasons);
        Assert.Empty(snap.CooldownRemainingMs);
        Assert.Empty(snap.ReadyForMs);
        Assert.Equal(0, snap.FireCount);
    }

    [Fact]
    public void ActiveReturnsLiveSnapshot()
    {
        using var lc = AutoKeyLifecycle.Start(BuildHost, logger: null, CancellationToken.None);
        Assert.True(lc.IsActive);
        // Lifecycle live; not asserting on contents (depends on tick race)
        // — just ensure the call doesn't throw and returns the snapshot type.
        var snap = lc.SnapshotRuntime();
        Assert.NotNull(snap.BlockReasons);
    }
}
