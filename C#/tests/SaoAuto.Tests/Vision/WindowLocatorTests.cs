using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

public class WindowLocatorTests
{
    [Fact]
    public void TitleKeywordMatchesCaseInsensitively()
    {
        var enumerator = new FakeEnumerator(
            new WindowCandidate((IntPtr)1, "Star Resonance Online", "x.exe", 0, 0, 1920, 1080),
            new WindowCandidate((IntPtr)2, "Notepad", "notepad.exe", 0, 0, 800, 600));

        var locator = new WindowLocator(enumerator,
            titleKeywords: new[] { "star" }, processNames: Array.Empty<string>());
        var found = locator.FindGameWindow();

        Assert.NotNull(found);
        Assert.Equal((IntPtr)1, found!.Value.Hwnd);
    }

    [Fact]
    public void ChineseKeywordMatches()
    {
        var enumerator = new FakeEnumerator(
            new WindowCandidate((IntPtr)1, "星痕共鸣 - 客户端", "star.exe", 0, 0, 1920, 1080));
        var locator = new WindowLocator(enumerator);
        Assert.NotNull(locator.FindGameWindow());
    }

    [Fact]
    public void ProcessNameMatchesEvenWhenTitleDoesNot()
    {
        var enumerator = new FakeEnumerator(
            new WindowCandidate((IntPtr)1, "Untitled", "Star.EXE", 0, 0, 1920, 1080));
        var locator = new WindowLocator(enumerator,
            titleKeywords: new[] { "no-such-keyword" });
        Assert.NotNull(locator.FindGameWindow());
    }

    [Fact]
    public void TooSmallWindowsAreRejected()
    {
        var enumerator = new FakeEnumerator(
            new WindowCandidate((IntPtr)1, "Star", "star.exe", 0, 0, 100, 100));
        var locator = new WindowLocator(enumerator);
        Assert.Null(locator.FindGameWindow());
    }

    [Fact]
    public void ReturnsNullWhenNoMatch()
    {
        var enumerator = new FakeEnumerator(
            new WindowCandidate((IntPtr)1, "Notepad", "notepad.exe", 0, 0, 800, 600),
            new WindowCandidate((IntPtr)2, "Calculator", "calc.exe", 0, 0, 800, 600));
        var locator = new WindowLocator(enumerator);
        Assert.Null(locator.FindGameWindow());
    }

    [Fact]
    public void CachedHandleAvoidsRepeatedEnumeration()
    {
        var enumerator = new FakeEnumerator(
            new WindowCandidate((IntPtr)1, "Star", "star.exe", 0, 0, 1920, 1080));
        var locator = new WindowLocator(enumerator);

        var first = locator.FindGameWindow();
        var second = locator.FindGameWindow();

        Assert.NotNull(first);
        Assert.NotNull(second);
        Assert.Equal(1, enumerator.EnumerateCallCount);
        Assert.True(enumerator.ProbeCallCount >= 1);
    }

    private sealed class FakeEnumerator : IWindowEnumerator
    {
        private readonly WindowCandidate[] _candidates;
        public int EnumerateCallCount { get; private set; }
        public int ProbeCallCount { get; private set; }

        public FakeEnumerator(params WindowCandidate[] candidates) => _candidates = candidates;

        public IEnumerable<WindowCandidate> Enumerate()
        {
            EnumerateCallCount++;
            return _candidates;
        }

        public bool IsAlive(IntPtr hwnd) => _candidates.Any(c => c.Hwnd == hwnd);

        public WindowCandidate? Probe(IntPtr hwnd)
        {
            ProbeCallCount++;
            foreach (var c in _candidates)
            {
                if (c.Hwnd == hwnd) return c;
            }
            return null;
        }
    }
}
