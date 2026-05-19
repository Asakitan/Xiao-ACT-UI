using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.Automation.HideSeek;

/// <summary>
/// S145 — Pin the data-side port of <c>hide_seek_engine.py</c>'s
/// STEPS table, colour palette, and <c>_br_to_rel</c> helper.
/// </summary>
public class Session145HideSeekStepsTests
{
    [Fact]
    public void FromBottomRightMatchesPythonBrToRel()
    {
        // Python: _br_to_rel(1446, 650, 35, 100) →
        //   tl_x = (1446 - 35) / 1920 = 1411/1920
        //   tl_y = (650 - 100) / 1080 = 550/1080
        var roi = HideSeekRoi.FromBottomRight(1446, 650, 35, 100);

        Assert.Equal(1411.0 / 1920.0, roi.X, 10);
        Assert.Equal(550.0 / 1080.0, roi.Y, 10);
        Assert.Equal(35.0 / 1920.0, roi.Width, 10);
        Assert.Equal(100.0 / 1080.0, roi.Height, 10);
    }

    [Fact]
    public void ColourPaletteMatchesPython()
    {
        Assert.Equal((byte)255, HideSeekSteps.White.B);
        Assert.Equal((byte)255, HideSeekSteps.White.G);
        Assert.Equal((byte)255, HideSeekSteps.White.R);
        Assert.Equal(35, HideSeekSteps.White.Tolerance);

        Assert.Equal((byte)50, HideSeekSteps.DarkGray.B);
        Assert.Equal(35, HideSeekSteps.DarkGray.Tolerance);

        Assert.Equal((byte)200, HideSeekSteps.LightGray.B);
        Assert.Equal(55, HideSeekSteps.LightGray.Tolerance); // widened 35→55
    }

    [Fact]
    public void ThresholdsMatchPython()
    {
        Assert.Equal(0.70, HideSeekSteps.MatchThreshold);
        Assert.Equal(0.10, HideSeekSteps.SqDiffThreshold);
    }

    [Fact]
    public void DefaultSequenceHasFiveStepsInOrder()
    {
        var s = HideSeekSteps.Default;
        Assert.Equal(5, s.Count);
        Assert.Equal(new[] { "Accept", "Confirm-1", "Confirm-2", "Confirm-3", "Confirm-4" },
            s.Select(x => x.Name).ToArray());
        Assert.Equal(new[] { "1.png", "2.png", "3.png", "4.png", "5.png" },
            s.Select(x => x.ImageFile).ToArray());
    }

    [Fact]
    public void AcceptStepUsesAltClickAndWhiteOnly()
    {
        var accept = HideSeekSteps.Default[0];
        Assert.True(accept.AltClick);
        Assert.False(accept.UseSqDiff);
        Assert.Single(accept.Colors);
        Assert.Equal(HideSeekSteps.White, accept.Colors[0]);
    }

    [Fact]
    public void ConfirmStepsUseGrayPairAndNoAlt()
    {
        for (int i = 1; i <= 4; i++)
        {
            var step = HideSeekSteps.Default[i];
            Assert.False(step.AltClick);
            Assert.Equal(2, step.Colors.Count);
            Assert.Contains(HideSeekSteps.DarkGray, step.Colors);
            Assert.Contains(HideSeekSteps.LightGray, step.Colors);
        }
    }

    [Fact]
    public void SqDiffOnlyEnabledForConfirm2Through4()
    {
        var flags = HideSeekSteps.Default.Select(s => s.UseSqDiff).ToArray();
        Assert.Equal(new[] { false, false, true, true, true }, flags);
    }

    [Fact]
    public void Confirm3AndConfirm4ShareSameRoi()
    {
        var c3 = HideSeekSteps.Default[3].Roi;
        var c4 = HideSeekSteps.Default[4].Roi;
        Assert.Equal(c3, c4); // Python step 3 & 4 both use _br_to_rel(1115, 1005, 310, 75)
    }
}
