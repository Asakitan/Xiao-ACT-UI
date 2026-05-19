using System.Collections.Immutable;
using SaoAuto.App.Startup;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

/// <summary>
/// S138 — Pin <see cref="AutoKeyLifecycle"/>. Same shape as
/// <see cref="Session97RecognitionLifecycleTests"/>: factory throws
/// must not propagate, dispose must be idempotent, status pass-through
/// reflects the host when active.
/// </summary>
public class Session138AutoKeyLifecycleTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public Session138AutoKeyLifecycleTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s138-" + Guid.NewGuid().ToString("N"));
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
    public void NullFactory_Throws()
    {
        Assert.Throws<ArgumentNullException>(() =>
            AutoKeyLifecycle.Start(null!, null, CancellationToken.None));
    }

    [Fact]
    public void FactoryThrowing_LifecycleStaysInactive()
    {
        using var lc = AutoKeyLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            logger: null,
            CancellationToken.None);

        Assert.False(lc.IsActive);
        Assert.Equal("inactive", lc.LastReason);
        Assert.Equal("", lc.LastActiveProfileId);
        Assert.Null(lc.LastFiredActionId);
        Assert.Equal(0, lc.TickCount);
    }

    [Fact]
    public void DisposeIsIdempotent()
    {
        var lc = AutoKeyLifecycle.Start(BuildHost, logger: null, CancellationToken.None);
        lc.Dispose();
        lc.Dispose(); // no throw
    }

    [Fact]
    public void StatusSurfaceReflectsActiveHost()
    {
        using var lc = AutoKeyLifecycle.Start(BuildHost, logger: null, CancellationToken.None);

        Assert.True(lc.IsActive);
        // S174 — `StartAsync` schedules the pump on a Task.Run, so by the
        // time the test reads LastReason the first tick may already have
        // landed and rewritten it. Race-fix: assert the surface produces a
        // value from the known AutoKeyTickHost.LastReason vocabulary
        // (covers `init` *and* any post-first-tick state) instead of
        // pinning to the pre-tick "init" string.
        var validReasons = new HashSet<string>
        {
            "init", "dead", "recognition-off", "background", "idle", "fired",
        };
        Assert.True(
            validReasons.Contains(lc.LastReason)
                || lc.LastReason.StartsWith("error:", StringComparison.Ordinal),
            $"unexpected LastReason={lc.LastReason}");
    }
}
