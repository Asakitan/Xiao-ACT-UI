using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.Automation.HideSeek;

/// <summary>
/// S146 — Pin <see cref="HideSeekColorMask"/> against Python
/// <c>_mask_in_range</c> behaviour: inRange across multiple colour
/// centres, then 3×3 rectangular dilation.
/// </summary>
public class Session146HideSeekColorMaskTests
{
    private static byte[] MakeBgr(params (byte b, byte g, byte r)[] pixels)
    {
        var buf = new byte[pixels.Length * 3];
        for (int i = 0; i < pixels.Length; i++)
        {
            buf[i * 3] = pixels[i].b;
            buf[i * 3 + 1] = pixels[i].g;
            buf[i * 3 + 2] = pixels[i].r;
        }
        return buf;
    }

    [Fact]
    public void WhitePixelMatchesWhiteCentre()
    {
        // 1×1 white pixel against the canonical White centre (tol 35).
        var pixels = MakeBgr((255, 255, 255));
        var mask = HideSeekColorMask.Build(pixels, 1, 1, stride: 3, channels: 3,
            new[] { HideSeekSteps.White });
        Assert.Equal(new byte[] { 255 }, mask);
    }

    [Fact]
    public void OutOfToleranceProducesZero()
    {
        // 1×1 pure red — far outside the White centre.
        var pixels = MakeBgr((0, 0, 255));
        var mask = HideSeekColorMask.Build(pixels, 1, 1, 3, 3,
            new[] { HideSeekSteps.White });
        Assert.Equal(new byte[] { 0 }, mask);
    }

    [Fact]
    public void MultipleColoursAreOrCombined()
    {
        // Pixel matches DarkGray only; mask must still be 255 when both
        // DarkGray and LightGray are supplied (OR semantics).
        var pixels = MakeBgr((50, 50, 50));
        // Use a 3×1 stripe so dilation doesn't pull in neighbours we don't expect.
        var rowOf3 = MakeBgr((50, 50, 50), (50, 50, 50), (50, 50, 50));
        var mask = HideSeekColorMask.Build(rowOf3, 3, 1, 9, 3,
            new[] { HideSeekSteps.DarkGray, HideSeekSteps.LightGray });
        Assert.Equal(new byte[] { 255, 255, 255 }, mask);
        _ = pixels;
    }

    [Fact]
    public void BgraStrideIsHonoured()
    {
        // 5×1 BGRA: one white pixel at index 0, rest red. Pre-dilate mask
        // is [1,0,0,0,0]; 3×3 dilation spreads it one cell → [1,1,0,0,0].
        // The far cells (idx 3, 4) must stay 0 — proves the BGRA channels
        // are being indexed correctly rather than rolling into the next pixel.
        var pixels = new byte[]
        {
            255, 255, 255, 255, // white
            0,   0,   255, 255, // red
            0,   0,   255, 255,
            0,   0,   255, 255,
            0,   0,   255, 255,
        };
        var mask = HideSeekColorMask.Build(pixels, 5, 1, stride: 20, channels: 4,
            new[] { HideSeekSteps.White });
        Assert.Equal(new byte[] { 255, 255, 0, 0, 0 }, mask);
    }

    [Fact]
    public void Dilation3x3SpreadsSinglePixel()
    {
        // A 3×3 mask with the centre pixel hot — after dilation every
        // cell should be 255 (centre is in every neighbour's 3×3 window).
        var input = new byte[9];
        input[4] = 255;
        var d = HideSeekColorMask.Dilate3x3(input, 3, 3);
        Assert.All(d, b => Assert.Equal((byte)255, b));
    }

    [Fact]
    public void DilationDoesNotWrapAcrossRows()
    {
        // 4×2: hot pixel at top-left only. After dilation the bottom-right
        // corner must stay 0.
        var input = new byte[8];
        input[0] = 255;
        var d = HideSeekColorMask.Dilate3x3(input, 4, 2);
        Assert.Equal((byte)255, d[0]);
        Assert.Equal((byte)255, d[1]);
        Assert.Equal((byte)255, d[4]); // row 1 col 0 — covered by 3x3
        Assert.Equal((byte)0, d[3]);   // row 0 col 3 — out of window
        Assert.Equal((byte)0, d[7]);   // row 1 col 3 — out of window
    }

    [Fact]
    public void EmptyColourListReturnsAllZeroMask()
    {
        var pixels = MakeBgr((255, 255, 255), (0, 0, 0));
        var mask = HideSeekColorMask.Build(pixels, 2, 1, 6, 3,
            Array.Empty<HideSeekColor>());
        Assert.Equal(new byte[] { 0, 0 }, mask);
    }

    [Fact]
    public void RejectsBadChannelCount()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            HideSeekColorMask.Build(new byte[2], 1, 1, 2, 2, new[] { HideSeekSteps.White }));
    }

    [Fact]
    public void RejectsShortStride()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            HideSeekColorMask.Build(new byte[3], 2, 1, stride: 3, channels: 3,
                new[] { HideSeekSteps.White }));
    }

    [Fact]
    public void RejectsTruncatedBuffer()
    {
        Assert.Throws<ArgumentException>(() =>
            HideSeekColorMask.Build(new byte[2], 1, 1, 3, 3, new[] { HideSeekSteps.White }));
    }
}
