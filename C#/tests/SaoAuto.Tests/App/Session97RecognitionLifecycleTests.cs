using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Startup;
using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.App;

/// <summary>
/// S97 — Pin <see cref="RecognitionLifecycle"/>. The lifecycle wraps
/// the build/start/stop/dispose dance shared by HeadlessRunner and
/// UiRunner. Construction must never throw; dispose must be
/// idempotent and swallow shutdown errors.
/// </summary>
public class Session97RecognitionLifecycleTests
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
        var enumerator = new StubEnumerator();
        var locator = new WindowLocator(enumerator);
        Func<WindowCandidate?> windowProvider = () => locator.FindGameWindow();
        var capture = new StubCapture();
        var engine = new RecognitionTickEngine(capture, windowProvider, default);
        return new RecognitionTickHost(engine, states);
    }

    [Fact]
    public void NullFactory_Throws()
    {
        Assert.Throws<ArgumentNullException>(() =>
            RecognitionLifecycle.Start(null!, NullLogger.Instance, CancellationToken.None));
    }

    [Fact]
    public void FactoryThrows_LifecycleInactive_NoThrow()
    {
        using var lifecycle = RecognitionLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            NullLogger.Instance,
            CancellationToken.None);
        Assert.False(lifecycle.IsActive);
    }

    [Fact]
    public void FactorySucceeds_LifecycleActive()
    {
        var states = new GameStateManager();
        using var lifecycle = RecognitionLifecycle.Start(
            () => BuildStubHost(states),
            NullLogger.Instance,
            CancellationToken.None);
        Assert.True(lifecycle.IsActive);
    }

    [Fact]
    public void Dispose_IsIdempotent()
    {
        var states = new GameStateManager();
        var lifecycle = RecognitionLifecycle.Start(
            () => BuildStubHost(states),
            NullLogger.Instance,
            CancellationToken.None);
        lifecycle.Dispose();
        lifecycle.Dispose(); // must not throw
    }

    [Fact]
    public void Dispose_OnInactive_IsNoOp()
    {
        var lifecycle = RecognitionLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            NullLogger.Instance,
            CancellationToken.None);
        lifecycle.Dispose();
        Assert.False(lifecycle.IsActive);
    }
}
