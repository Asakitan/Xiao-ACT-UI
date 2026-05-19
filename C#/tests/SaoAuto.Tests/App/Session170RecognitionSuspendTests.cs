using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Startup;
using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.App;

/// <summary>
/// S170 — Pin <see cref="RecognitionLifecycle"/>'s new
/// <c>Suspend</c>/<c>Resume</c> hooks plus the corresponding
/// <see cref="RecognitionTickHost.Suspended"/> gate. While suspended,
/// <c>RunOnceAsync</c> skips engine ticks (no projection), and
/// <see cref="RecognitionLifecycle.IsActive"/> reports <c>false</c>
/// so the HUD reflects vision being paused.
/// </summary>
public class Session170RecognitionSuspendTests
{
    private sealed class StubEnumerator : IWindowEnumerator
    {
        public IEnumerable<WindowCandidate> Enumerate() => Array.Empty<WindowCandidate>();
        public bool IsAlive(IntPtr hwnd) => false;
        public WindowCandidate? Probe(IntPtr hwnd) => null;
    }

    private sealed class StubCapture : IFrameCapture
    {
        public CapturedFrame? Capture() => null;
        public void Dispose() { }
    }

    private static RecognitionTickHost BuildStubHost(GameStateManager states)
    {
        var locator = new WindowLocator(new StubEnumerator());
        Func<WindowCandidate?> windowProvider = () => locator.FindGameWindow();
        var engine = new RecognitionTickEngine(new StubCapture(), windowProvider, default);
        return new RecognitionTickHost(engine, states);
    }

    [Fact]
    public void SuspendFlipsIsActiveFalse()
    {
        var states = new GameStateManager();
        using var lifecycle = RecognitionLifecycle.Start(
            () => BuildStubHost(states), NullLogger.Instance, CancellationToken.None);
        Assert.True(lifecycle.IsActive);
        lifecycle.Suspend();
        Assert.False(lifecycle.IsActive);
    }

    [Fact]
    public void ResumeRestoresActive()
    {
        var states = new GameStateManager();
        using var lifecycle = RecognitionLifecycle.Start(
            () => BuildStubHost(states), NullLogger.Instance, CancellationToken.None);
        lifecycle.Suspend();
        lifecycle.Resume();
        Assert.True(lifecycle.IsActive);
    }

    [Fact]
    public void SuspendResumeAreIdempotent()
    {
        var states = new GameStateManager();
        using var lifecycle = RecognitionLifecycle.Start(
            () => BuildStubHost(states), NullLogger.Instance, CancellationToken.None);
        lifecycle.Suspend();
        lifecycle.Suspend();
        Assert.False(lifecycle.IsActive);
        lifecycle.Resume();
        lifecycle.Resume();
        Assert.True(lifecycle.IsActive);
    }

    [Fact]
    public void SuspendOnInactiveLifecycleIsNoOp()
    {
        using var lifecycle = RecognitionLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            NullLogger.Instance, CancellationToken.None);
        lifecycle.Suspend();
        lifecycle.Resume();
        Assert.False(lifecycle.IsActive);
    }

    [Fact]
    public void HostSuspendedSkipsEngineTick()
    {
        var states = new GameStateManager();
        var host = BuildStubHost(states);
        // Tick once unsuspended to exercise the projection path.
        var unsuspendedSleep = host.RunOnceAsync();
        Assert.True(unsuspendedSleep >= 10);
        host.Suspended = true;
        var suspendedSleep = host.RunOnceAsync();
        // Suspended branch ignores NextFps and falls back to the 1s floor.
        Assert.True(suspendedSleep >= 1000, $"expected >=1000ms suspended sleep, got {suspendedSleep}");
        host.Suspended = false;
        var resumedSleep = host.RunOnceAsync();
        Assert.True(resumedSleep >= 10);
    }

    [Fact]
    public void HostSuspendedReturnsAtLeastMinSleep()
    {
        var states = new GameStateManager();
        var host = BuildStubHost(states);
        host.Suspended = true;
        var sleepMs = host.RunOnceAsync();
        Assert.True(sleepMs >= 10, $"expected >=10ms, got {sleepMs}");
    }
}
