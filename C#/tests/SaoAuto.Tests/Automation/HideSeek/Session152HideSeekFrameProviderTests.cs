using SaoAuto.Core.Automation.HideSeek;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Automation.HideSeek;

/// <summary>
/// S152 — Pin <see cref="HideSeekFrameProvider"/>: wires
/// <see cref="WindowLocator"/> + <see cref="IFrameCapture"/> into the
/// state machine's frame contract. Returns null when either side
/// gives up; otherwise forwards pixels + ClientLeft/Top.
/// </summary>
public class Session152HideSeekFrameProviderTests
{
    private sealed class FakeEnumerator : IWindowEnumerator
    {
        public WindowCandidate? Window;
        public IEnumerable<WindowCandidate> Enumerate()
            => Window is { } w ? new[] { w } : Array.Empty<WindowCandidate>();
        public bool IsAlive(IntPtr hwnd) => Window is { } w && w.Hwnd == hwnd;
        public WindowCandidate? Probe(IntPtr hwnd) => Window;
    }

    private sealed class FakeCapture : IFrameCapture
    {
        public CapturedFrame? Next;
        public CapturedFrame? Capture() => Next;
        public void Dispose() { }
    }

    private static WindowCandidate Win(int left, int top, int w = 800, int h = 600)
        => new(new IntPtr(0x1234), "Sword Art Online", "sao.exe", left, top, left + w, top + h);

    [Fact]
    public void ReturnsNullWhenWindowMissing()
    {
        var locator = new WindowLocator(new FakeEnumerator(), new[] { "Sword" });
        var cap = new FakeCapture { Next = new CapturedFrame(2, 2, 8, new byte[16]) };
        var prov = new HideSeekFrameProvider(cap, locator);
        Assert.Null(prov.Capture());
    }

    [Fact]
    public void ReturnsNullWhenCaptureOffline()
    {
        var en = new FakeEnumerator { Window = Win(100, 50) };
        var locator = new WindowLocator(en, new[] { "Sword" });
        var prov = new HideSeekFrameProvider(new FakeCapture { Next = null }, locator);
        Assert.Null(prov.Capture());
    }

    [Fact]
    public void ForwardsPixelsAndClientOffset()
    {
        var en = new FakeEnumerator { Window = Win(100, 50, 800, 600) };
        var locator = new WindowLocator(en, new[] { "Sword" });
        var pixels = new byte[800 * 600 * 4];
        pixels[0] = 7;
        var cap = new FakeCapture { Next = new CapturedFrame(800, 600, 800 * 4, pixels) };
        var prov = new HideSeekFrameProvider(cap, locator);

        var frame = prov.Capture();
        Assert.NotNull(frame);
        Assert.Equal(800, frame!.Width);
        Assert.Equal(600, frame.Height);
        Assert.Equal(800 * 4, frame.Stride);
        Assert.Equal(4, frame.Channels);
        Assert.Equal(100, frame.ClientLeft);
        Assert.Equal(50, frame.ClientTop);
        Assert.Same(pixels, frame.Pixels);
    }

    [Fact]
    public void NullArgsRejected()
    {
        var locator = new WindowLocator(new FakeEnumerator(), new[] { "x" });
        Assert.Throws<ArgumentNullException>(() => new HideSeekFrameProvider(null!, locator));
        Assert.Throws<ArgumentNullException>(() => new HideSeekFrameProvider(new FakeCapture(), null!));
    }
}
