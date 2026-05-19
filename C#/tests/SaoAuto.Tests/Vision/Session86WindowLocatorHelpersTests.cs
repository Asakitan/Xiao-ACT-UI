using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S86 — WindowLocator helpers (GetRect / GetSize / RoiToScreenBbox).
/// Pure projections atop the existing FindGameWindow path.
/// </summary>
public class Session86WindowLocatorHelpersTests
{
    private sealed class FakeEnumerator : IWindowEnumerator
    {
        private readonly WindowCandidate[] _candidates;
        public FakeEnumerator(params WindowCandidate[] cs) => _candidates = cs;
        public IEnumerable<WindowCandidate> Enumerate() => _candidates;
        public bool IsAlive(IntPtr hwnd) => _candidates.Any(c => c.Hwnd == hwnd);
        public WindowCandidate? Probe(IntPtr hwnd)
        {
            foreach (var c in _candidates) if (c.Hwnd == hwnd) return c;
            return null;
        }
    }

    private static WindowCandidate Win(string title, int l = 100, int t = 50, int w = 1920, int h = 1080) =>
        new((IntPtr)1, title, "starresonance.exe", l, t, l + w, t + h);

    [Fact]
    public void GetRect_NullWhenNoWindow()
    {
        var loc = new WindowLocator(new FakeEnumerator(), titleKeywords: new[] { "Game" });
        Assert.Null(loc.GetRect());
    }

    [Fact]
    public void GetRect_ReturnsClientScreenRect()
    {
        var loc = new WindowLocator(new FakeEnumerator(Win("Star Resonance Game")),
            titleKeywords: new[] { "Resonance" });
        var r = loc.GetRect();
        Assert.NotNull(r);
        Assert.Equal(new RectI(100, 50, 1920, 1080), r!.Value);
    }

    [Fact]
    public void GetSize_NullWhenNoWindow()
    {
        var loc = new WindowLocator(new FakeEnumerator(), titleKeywords: new[] { "X" });
        Assert.Null(loc.GetSize());
    }

    [Fact]
    public void GetSize_ReturnsWidthHeight()
    {
        var loc = new WindowLocator(new FakeEnumerator(Win("X game", w: 800, h: 600)),
            titleKeywords: new[] { "game" });
        var size = loc.GetSize();
        Assert.NotNull(size);
        Assert.Equal((800, 600), size!.Value);
    }

    [Fact]
    public void RoiToScreenBbox_InstanceUsesCachedWindow()
    {
        var loc = new WindowLocator(new FakeEnumerator(Win("Star game", l: 100, t: 50, w: 1000, h: 500)),
            titleKeywords: new[] { "game" });
        // ROI(0.1, 0.2, 0.3, 0.4) → x=100+100=200, y=50+100=150, x2=200+300=500, y2=150+200=350
        var b = loc.RoiToScreenBbox(new Roi(0.1, 0.2, 0.3, 0.4));
        Assert.NotNull(b);
        Assert.Equal((200, 150, 500, 350), b!.Value);
    }

    [Fact]
    public void RoiToScreenBbox_NullWhenNoWindow()
    {
        var loc = new WindowLocator(new FakeEnumerator(), titleKeywords: new[] { "x" });
        Assert.Null(loc.RoiToScreenBbox(new Roi(0, 0, 1, 1)));
    }

    [Fact]
    public void RoiToScreenBbox_StaticOverload_PureProjection()
    {
        var b = WindowLocator.RoiToScreenBbox(new Roi(0.5, 0.5, 0.25, 0.25),
            left: 0, top: 0, right: 800, bottom: 600);
        Assert.Equal((400, 300, 600, 450), b);
    }

    [Fact]
    public void RoiToScreenBbox_StaticOverload_NonZeroOrigin()
    {
        var b = WindowLocator.RoiToScreenBbox(new Roi(0, 0, 1, 1),
            left: 50, top: 25, right: 850, bottom: 625);
        Assert.Equal((50, 25, 850, 625), b);
    }
}
