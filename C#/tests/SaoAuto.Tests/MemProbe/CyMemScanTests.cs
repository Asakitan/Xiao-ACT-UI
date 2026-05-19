using SaoAuto.MemProbe;

namespace SaoAuto.Tests.MemProbe;

public class CyMemScanTests
{
    [Fact]
    public void FindPatternFindsExactMatch()
    {
        var haystack = new byte[] { 0x10, 0x20, 0xAB, 0xCD, 0xEF, 0x30, 0x40 };
        var pattern = new byte?[] { 0xAB, 0xCD, 0xEF };
        Assert.Equal(2, CyMemScan.FindPattern(haystack, pattern));
    }

    [Fact]
    public void FindPatternHandlesWildcards()
    {
        var haystack = new byte[] { 0x10, 0x20, 0xAB, 0x99, 0xEF, 0x30 };
        var pattern = new byte?[] { 0xAB, null, 0xEF };
        Assert.Equal(2, CyMemScan.FindPattern(haystack, pattern));
    }

    [Fact]
    public void FindPatternMissReturnsMinusOne()
    {
        var haystack = new byte[] { 0x10, 0x20, 0x30 };
        var pattern = new byte?[] { 0xAB, 0xCD };
        Assert.Equal(-1, CyMemScan.FindPattern(haystack, pattern));
    }

    [Fact]
    public void FindPatternRespectsStartOffset()
    {
        var haystack = new byte[] { 0xAB, 0xCD, 0x00, 0xAB, 0xCD };
        var pattern = new byte?[] { 0xAB, 0xCD };
        Assert.Equal(0, CyMemScan.FindPattern(haystack, pattern, start: 0));
        Assert.Equal(3, CyMemScan.FindPattern(haystack, pattern, start: 1));
    }

    [Fact]
    public void FindAllPatternsReturnsEveryHit()
    {
        var haystack = new byte[] { 0xAB, 0xCD, 0x00, 0xAB, 0xCD, 0x00, 0xAB, 0xCD };
        var pattern = new byte?[] { 0xAB, 0xCD };
        Assert.Equal(new[] { 0, 3, 6 }, CyMemScan.FindAllPatterns(haystack, pattern));
    }

    [Fact]
    public void ParsePatternHandlesHexAndWildcards()
    {
        var pattern = CyMemScan.ParsePattern("AB ?? CD ?? ?? EF");
        Assert.Equal(6, pattern.Length);
        Assert.Equal((byte)0xAB, pattern[0]!.Value);
        Assert.Null(pattern[1]);
        Assert.Equal((byte)0xCD, pattern[2]!.Value);
        Assert.Null(pattern[3]);
        Assert.Null(pattern[4]);
        Assert.Equal((byte)0xEF, pattern[5]!.Value);
    }

    [Fact]
    public void ParsePatternThrowsOnInvalidToken()
    {
        Assert.Throws<ArgumentException>(() => CyMemScan.ParsePattern("AB GG CD"));
    }
}
