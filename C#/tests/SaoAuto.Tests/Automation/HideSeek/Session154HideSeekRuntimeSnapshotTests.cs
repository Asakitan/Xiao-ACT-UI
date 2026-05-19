using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Startup;
using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.Automation.HideSeek;

/// <summary>
/// S154 — Pin <see cref="HideSeekRuntimeSnapshot"/> on tick host and
/// lifecycle: empty snapshot when inactive, populated snapshot reflects
/// IsRunning / TickCount / LastFiredStep / LastError after ticks.
/// </summary>
public class Session154HideSeekRuntimeSnapshotTests
{
    private sealed class StubFrames : IHideSeekFrameProvider
    {
        public HideSeekFrame? Capture() =>
            new(new byte[12], 2, 2, 6, 3, 0, 0);
    }

    private sealed class StubInput : IHideSeekInput
    {
        public void Click(int x, int y, bool alt) { }
    }

    private static HideSeekStateMachine MakeMachine(bool throwOnTick = false, bool hit = true)
    {
        var steps = HideSeekSteps.Default;
        var templates = steps.ToDictionary(s => s.ImageFile,
            s => new HideSeekTemplate(s.ImageFile, new byte[] { 0 }, 1, 1));
        return new HideSeekStateMachine(
            steps, templates, new StubFrames(), new StubInput(),
            detect: (step, _, _) =>
            {
                if (throwOnTick) throw new InvalidOperationException("boom");
                return hit
                    ? new HideSeekDetector.DetectResult(10, 20, 0.9, "stub")
                    : null;
            });
    }

    [Fact]
    public void EmptySnapshotMatchesInactiveDefaults()
    {
        var empty = HideSeekRuntimeSnapshot.Empty;
        Assert.False(empty.IsRunning);
        Assert.Equal(0L, empty.TickCount);
        Assert.Equal(-1, empty.LastFiredStep);
        Assert.Equal("inactive", empty.LastError);
    }

    [Fact]
    public void TickHostSnapshotReflectsTickResult()
    {
        var host = new HideSeekTickHost(MakeMachine(hit: true), intervalMs: 10);
        host.TickOnce();
        var snap = host.SnapshotRuntime();
        Assert.Equal(1L, snap.TickCount);
        Assert.Equal(0, snap.LastFiredStep);
        Assert.Equal("", snap.LastError);
        Assert.False(snap.IsRunning); // pump not started
    }

    [Fact]
    public void TickHostSnapshotCarriesLastError()
    {
        var host = new HideSeekTickHost(MakeMachine(throwOnTick: true), intervalMs: 10);
        host.TickOnce();
        var snap = host.SnapshotRuntime();
        Assert.Equal(-1, snap.LastFiredStep);
        Assert.Contains("boom", snap.LastError);
    }

    [Fact]
    public void LifecycleSnapshotEmptyWhenFactoryThrows()
    {
        using var lc = HideSeekLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            NullLogger.Instance,
            CancellationToken.None);
        var snap = lc.SnapshotRuntime();
        Assert.Equal(HideSeekRuntimeSnapshot.Empty, snap);
        Assert.False(snap.IsRunning);
        Assert.Equal("inactive", snap.LastError);
    }
}
