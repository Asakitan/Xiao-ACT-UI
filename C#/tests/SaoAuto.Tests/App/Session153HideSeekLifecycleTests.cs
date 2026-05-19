using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Startup;
using SaoAuto.Core.Automation.HideSeek;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.App;

/// <summary>
/// S153 — Pin <see cref="HideSeekLifecycle"/> and
/// <see cref="HideSeekBootstrap"/>: factory exceptions surface as
/// inactive lifecycle, missing assets throw from bootstrap, dispose
/// is idempotent.
/// </summary>
public class Session153HideSeekLifecycleTests
{
    private sealed class StubEnumerator : IWindowEnumerator
    {
        public IEnumerable<WindowCandidate> Enumerate() => Array.Empty<WindowCandidate>();
        public bool IsAlive(IntPtr hwnd) => false;
        public WindowCandidate? Probe(IntPtr hwnd) => null;
    }

    [Fact]
    public void NullFactoryThrows()
    {
        Assert.Throws<ArgumentNullException>(() =>
            HideSeekLifecycle.Start(null!, NullLogger.Instance, CancellationToken.None));
    }

    [Fact]
    public void FactoryThrowsKeepsLifecycleInactive()
    {
        using var lc = HideSeekLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            NullLogger.Instance,
            CancellationToken.None);
        Assert.False(lc.IsActive);
        Assert.Equal("inactive", lc.LastError);
        Assert.Equal(0, lc.TickCount);
        Assert.Equal(-1, lc.LastFiredStep);
    }

    [Fact]
    public void DisposeIsIdempotent()
    {
        var lc = HideSeekLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            NullLogger.Instance,
            CancellationToken.None);
        lc.Dispose();
        lc.Dispose(); // second dispose must not throw
    }

    [Fact]
    public void BootstrapThrowsWhenAssetsMissing()
    {
        var locator = new WindowLocator(new StubEnumerator());
        // Point at a directory that exists but lacks the PNGs.
        var paths = new ResourcePathResolver(Path.GetTempPath(), Path.GetTempPath());
        var ex = Assert.Throws<InvalidOperationException>(() =>
            HideSeekBootstrap.Build(locator, paths,
                decode: _ => null,
                log: NullLogger.Instance));
        Assert.Contains("no templates loaded", ex.Message);
    }

    [Fact]
    public void BootstrapBuildsHostWhenTemplatesDecode()
    {
        var locator = new WindowLocator(new StubEnumerator());
        var paths = new ResourcePathResolver(Path.GetTempPath(), Path.GetTempPath());
        using var host = HideSeekBootstrap.Build(locator, paths,
            decode: _ => new HideSeekTemplates.DecodedImage(new byte[4], 1, 1, 4, 4),
            log: NullLogger.Instance);
        Assert.NotNull(host);
        Assert.Equal(0, host.TickCount);
    }

    [Fact]
    public void BootstrapRejectsNullArgs()
    {
        var paths = new ResourcePathResolver(Path.GetTempPath(), Path.GetTempPath());
        Assert.Throws<ArgumentNullException>(() =>
            HideSeekBootstrap.Build(null!, paths));
        Assert.Throws<ArgumentNullException>(() =>
            HideSeekBootstrap.Build(new WindowLocator(new StubEnumerator()), null!));
    }
}
