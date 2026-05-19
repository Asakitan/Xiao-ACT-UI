using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

public class StaminaBarRecognizerTests
{
    [Fact]
    public void SampleHorizontalFillCountsMatchingPixels()
    {
        // 10x4 frame, fill the centre row half-and-half: blue then black.
        var stride = 10 * 4;
        var pixels = new byte[stride * 4];
        for (var x = 0; x < 5; x++)
        {
            var off = (2 * stride) + x * 4;
            pixels[off] = 255; pixels[off + 1] = 0; pixels[off + 2] = 0; pixels[off + 3] = 255;
        }
        var frame = new CapturedFrame(10, 4, stride, pixels);
        var region = new RectI(0, 1, 10, 3);
        // Predicate: blue channel >= 128
        var fill = StaminaBarRecognizer.SampleHorizontalFill(frame, region, (b, _, _) => b >= 128);
        Assert.Equal(0.5, fill, 6);
    }

    [Fact]
    public void EmptyRegionReturnsNan()
    {
        var frame = new CapturedFrame(10, 4, 40, new byte[160]);
        var fill = StaminaBarRecognizer.SampleHorizontalFill(
            frame, new RectI(0, 0, 0, 0), (_, _, _) => true);
        Assert.True(double.IsNaN(fill));
    }

    [Theory]
    [InlineData(0.50, 0.50)]
    [InlineData(0.91, 0.91)]   // unclamped 91-97 (Python rule)
    [InlineData(0.97, 0.97)]
    [InlineData(0.98, 1.00)]   // clamp at 98%+
    [InlineData(1.05, 1.00)]
    [InlineData(-0.1, 0.00)]
    public void NormalizeStaminaPercentFollowsPythonClamp(double input, double expected)
    {
        Assert.Equal(expected, StaminaBarRecognizer.NormalizeStaminaPercent(input), 6);
    }

    [Fact]
    public void NaNNormalizesToZero()
    {
        Assert.Equal(0.0, StaminaBarRecognizer.NormalizeStaminaPercent(double.NaN));
    }
}
