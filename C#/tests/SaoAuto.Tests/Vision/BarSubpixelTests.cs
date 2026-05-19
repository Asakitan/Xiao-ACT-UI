using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

public class BarSubpixelTests
{
    [Fact]
    public void SubpixelCrossingInterpolatesBetweenSamples()
    {
        // s0=0.8 > thr=0.5 >= s1=0.2  →  frac = (0.8-0.5)/(0.8-0.2) = 0.5
        var score = new[] { 0.9, 0.8, 0.2, 0.1 };
        var x = BarSubpixel.SubpixelThresholdCrossing(score, 0.5, 1);
        Assert.Equal(1.5, x, 6);
    }

    [Fact]
    public void SubpixelCrossingFallsBackWhenNoCrossing()
    {
        var score = new[] { 0.9, 0.8, 0.7, 0.6 };
        // No drop below threshold after lastFilledIdx=3 → returns idx+1
        Assert.Equal(4, BarSubpixel.SubpixelThresholdCrossing(score, 0.5, 3));
    }

    [Fact]
    public void ConvolveSameMatchesNumpyMeanFilter()
    {
        var input = new[] { 1.0, 2, 3, 4, 5 };
        var output = BarSubpixel.ConvolveSame(input, 3);
        // numpy mean-3 same: [(0+1+2)/3, (1+2+3)/3, (2+3+4)/3, (3+4+5)/3, (4+5+0)/3]
        Assert.Equal(new[] { 1.0, 2, 3, 4, 3 }, output.Select(v => Math.Round(v, 6)));
    }

    [Fact]
    public void GradientEdgePctReturnsNullWhenFlat()
    {
        var flat = Enumerable.Repeat(0.5, 40).ToArray();
        Assert.Null(BarSubpixel.GradientEdgePct(flat, 40, 0.0));
    }

    [Fact]
    public void GradientEdgePctFindsApproximateEdge()
    {
        // Build a 40-wide score: filled (0.9) for first 20, empty (0.1) after.
        var score = new double[40];
        for (var i = 0; i < 40; i++) score[i] = i < 20 ? 0.9 : 0.1;
        var smooth = BarSubpixel.ConvolveSame(score, 5);
        var pct = BarSubpixel.GradientEdgePct(smooth, 40, dynamicRange: 0.8);
        Assert.NotNull(pct);
        Assert.InRange(pct!.Value, 0.40, 0.60);
    }

    [Fact]
    public void RowIndependentPctReturnsMedianAcrossRows()
    {
        // 4 rows × 20 cols, each row has fill 0.8 in first 12 cols, 0.0 after.
        // → per-row pct ≈ 12/20 = 0.6
        var rows = new double[4, 20];
        for (var r = 0; r < 4; r++)
            for (var c = 0; c < 20; c++)
                rows[r, c] = c < 12 ? 0.8 : 0.0;
        var pct = BarSubpixel.RowIndependentPct(rows, threshold: 0.5);
        Assert.NotNull(pct);
        Assert.InRange(pct!.Value, 0.55, 0.65);
    }

    [Fact]
    public void RowIndependentPctReturnsNullWhenTooFewRows()
    {
        var rows = new double[1, 20];
        Assert.Null(BarSubpixel.RowIndependentPct(rows, 0.5));
    }
}
