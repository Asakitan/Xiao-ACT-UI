using SaoAuto.Overlay.SkillFx;

namespace SaoAuto.Tests.Overlay;

public class CySkillFxTests
{
    [Fact]
    public void CircleSdfZeroOnRing()
    {
        Assert.Equal(0.0, CySkillFx.CircleSdf(10, 0, 10), 6);
    }

    [Fact]
    public void CircleSdfNegativeInside()
    {
        Assert.True(CySkillFx.CircleSdf(0, 0, 10) < 0);
    }

    [Fact]
    public void RingSdfZeroOnCenterline()
    {
        // dx=10, dy=0, radius=10, halfWidth=2 → |0| - 2 = -2 (inside ring band)
        Assert.Equal(-2.0, CySkillFx.RingSdf(10, 0, 10, 2), 6);
    }

    [Theory]
    [InlineData(0.0, 1.0, 0.5, 0.5)]
    [InlineData(0.0, 1.0, -1.0, 0.0)]
    [InlineData(0.0, 1.0, 2.0, 1.0)]
    public void SmoothStepClamps(double e0, double e1, double x, double expected)
    {
        var actual = CySkillFx.SmoothStep(e0, e1, x);
        if (expected is 0.0 or 1.0)
        {
            Assert.Equal(expected, actual, 4);
        }
        else
        {
            Assert.InRange(actual, 0.0, 1.0);
        }
    }

    [Fact]
    public void RingAlphaPeaksOnRing()
    {
        // Inside ring band → alpha 1.
        Assert.Equal(1.0, CySkillFx.RingAlpha(10, 0, 10, 2), 4);
        // Far outside → alpha 0.
        Assert.Equal(0.0, CySkillFx.RingAlpha(20, 0, 10, 2, aaPixels: 1), 4);
    }

    [Fact]
    public void GlowAlphaDecaysWithDistance()
    {
        var center = CySkillFx.GlowAlpha(0, 0, radius: 0, sigma: 5);
        var far = CySkillFx.GlowAlpha(20, 0, radius: 0, sigma: 5);
        Assert.True(center > far);
        Assert.Equal(1.0, center, 4);
    }

    [Fact]
    public void BeamAlphaInsideBarIsOne()
    {
        Assert.Equal(1.0, CySkillFx.BeamAlpha(0, halfWidth: 5), 4);
        Assert.Equal(0.0, CySkillFx.BeamAlpha(10, halfWidth: 5, aaPixels: 1), 4);
    }
}
