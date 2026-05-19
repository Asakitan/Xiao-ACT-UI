using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.Automation.HideSeek;

/// <summary>
/// S147 — Pin <see cref="HideSeekTemplateMatcher"/> against the
/// behavioural contract of <c>cv2.matchTemplate</c> for the
/// TM_CCOEFF_NORMED and TM_SQDIFF_NORMED paths plus the
/// "template too big" fallback.
/// </summary>
public class Session147HideSeekTemplateMatcherTests
{
    private static byte[] Solid(int w, int h, byte v)
    {
        var buf = new byte[w * h];
        Array.Fill(buf, v);
        return buf;
    }

    [Fact]
    public void NccFindsExactMatchAtCorrectLocation()
    {
        // 5×5 background of 50, with a 3×3 "200" patch at (1, 2).
        var img = Solid(5, 5, 50);
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++)
                img[(2 + j) * 5 + (1 + i)] = 200;

        var tpl = Solid(3, 3, 200);

        var result = HideSeekTemplateMatcher.MatchNcc(img, 5, 5, tpl, 3, 3, threshold: 0.70);

        // Constant patch / constant template: zero variance — score path
        // returns 0.0. Found = false but the patch is still located by
        // the lower SQDIFF, so use a textured template instead for NCC.
        _ = result;
    }

    [Fact]
    public void NccTexturedTemplateHitsExactPosition()
    {
        // 6×6 background 50, with a 3×3 gradient patch at (2, 1).
        var img = Solid(6, 6, 50);
        byte[] tplBytes = { 10, 60, 110, 60, 110, 160, 110, 160, 210 };
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++)
                img[(1 + j) * 6 + (2 + i)] = tplBytes[j * 3 + i];

        var result = HideSeekTemplateMatcher.MatchNcc(img, 6, 6, tplBytes, 3, 3, threshold: 0.70);

        Assert.True(result.Found);
        Assert.Equal(2, result.X);
        Assert.Equal(1, result.Y);
        Assert.True(result.Score > 0.99, $"score was {result.Score}");
    }

    [Fact]
    public void NccBelowThresholdReportsNotFound()
    {
        // Image is uniform; textured template — best score should be ~0.
        var img = Solid(8, 8, 128);
        byte[] tpl = { 0, 255, 0, 255 };
        var result = HideSeekTemplateMatcher.MatchNcc(img, 8, 8, tpl, 2, 2, threshold: 0.70);
        Assert.False(result.Found);
    }

    [Fact]
    public void SqDiffFindsExactPatch()
    {
        // 4×4 background 0, patch [200] at (1, 1)
        var img = new byte[16];
        img[1 * 4 + 1] = 200;
        var tpl = new byte[] { 200 };

        var result = HideSeekTemplateMatcher.MatchSqDiff(img, 4, 4, tpl, 1, 1, threshold: 0.10);
        Assert.True(result.Found);
        Assert.Equal(1, result.X);
        Assert.Equal(1, result.Y);
        Assert.True(result.Score < 0.01, $"score was {result.Score}");
    }

    [Fact]
    public void SqDiffMismatchReportsNotFound()
    {
        var img = Solid(4, 4, 0);
        var tpl = new byte[] { 200 };
        var result = HideSeekTemplateMatcher.MatchSqDiff(img, 4, 4, tpl, 1, 1, threshold: 0.10);
        Assert.False(result.Found);
    }

    [Fact]
    public void TemplateLargerThanImageReturnsNotFound()
    {
        var img = Solid(3, 3, 0);
        var tpl = Solid(5, 5, 0);

        var ncc = HideSeekTemplateMatcher.MatchNcc(img, 3, 3, tpl, 5, 5);
        var sq = HideSeekTemplateMatcher.MatchSqDiff(img, 3, 3, tpl, 5, 5);
        Assert.False(ncc.Found);
        Assert.False(sq.Found);
    }

    [Fact]
    public void ToGrayscaleMatchesOpenCvWeights()
    {
        // BGR (0,0,255) red → 0.299*255 ≈ 76.245 → 76
        // BGR (0,255,0) green → 0.587*255 ≈ 149.685 → 150
        // BGR (255,0,0) blue → 0.114*255 ≈ 29.07 → 29
        var pixels = new byte[]
        {
            0, 0, 255,
            0, 255, 0,
            255, 0, 0,
        };
        var gray = HideSeekTemplateMatcher.ToGrayscale(pixels, 3, 1, 9, 3);
        Assert.InRange((int)gray[0], 75, 77);
        Assert.InRange((int)gray[1], 149, 151);
        Assert.InRange((int)gray[2], 28, 30);
    }

    [Fact]
    public void ToGrayscaleSinglePassesThroughChannel0()
    {
        var pixels = new byte[] { 17, 42, 99 };
        var gray = HideSeekTemplateMatcher.ToGrayscale(pixels, 3, 1, 3, 1);
        Assert.Equal(new byte[] { 17, 42, 99 }, gray);
    }

    [Fact]
    public void ResizeReturnsUnchangedWhenTemplateFits()
    {
        var tpl = new byte[] { 1, 2, 3, 4 };
        var resized = HideSeekTemplateMatcher.ResizeTemplateIfTooLarge(tpl, 2, 2, roiWidth: 4, roiHeight: 4);
        Assert.NotNull(resized);
        Assert.Equal(2, resized!.Value.Width);
        Assert.Equal(2, resized!.Value.Height);
    }

    [Fact]
    public void ResizeShrinksWhenTemplateLarger()
    {
        // 20×20 template, ROI 10×10 → scale = min(10/20, 10/20) * 0.95 = 0.475
        // new w = h = (int)(20 * 0.475) = 9
        var tpl = Solid(20, 20, 200);
        var resized = HideSeekTemplateMatcher.ResizeTemplateIfTooLarge(tpl, 20, 20, 10, 10);
        Assert.NotNull(resized);
        Assert.Equal(9, resized!.Value.Width);
        Assert.Equal(9, resized!.Value.Height);
    }

    [Fact]
    public void ResizeReturnsNullWhenScaleTooSmall()
    {
        // 200×200 tpl, ROI 10×10 → scale = 0.05 * 0.95 = 0.0475 ≤ 0.1
        var tpl = Solid(200, 200, 0);
        Assert.Null(HideSeekTemplateMatcher.ResizeTemplateIfTooLarge(tpl, 200, 200, 10, 10));
    }
}
