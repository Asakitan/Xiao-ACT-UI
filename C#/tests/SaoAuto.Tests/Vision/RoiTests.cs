using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

public class RoiTests
{
    [Fact]
    public void ToPixelsRoundsToNearest()
    {
        var roi = new Roi(0.1, 0.2, 0.3, 0.4);
        var rect = roi.ToPixels(1000, 800);
        Assert.Equal(100, rect.X);
        Assert.Equal(160, rect.Y);
        Assert.Equal(300, rect.W);
        Assert.Equal(320, rect.H);
    }

    [Fact]
    public void ToScreenPixelsAddsWindowOrigin()
    {
        var roi = new Roi(0.5, 0.5, 0.1, 0.1);
        var window = new WindowCandidate(IntPtr.Zero, "t", null, 100, 200, 1100, 1000);
        var rect = roi.ToScreenPixels(window);
        Assert.Equal(100 + 500, rect.X);
        Assert.Equal(200 + 400, rect.Y);
        Assert.Equal(100, rect.W);
        Assert.Equal(80, rect.H);
    }
}
