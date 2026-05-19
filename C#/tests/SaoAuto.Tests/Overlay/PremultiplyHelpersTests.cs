using SaoAuto.Overlay.Rendering;

namespace SaoAuto.Tests.Overlay;

public class PremultiplyHelpersTests
{
    [Fact]
    public void OpaquePixelsAreUnchanged()
    {
        var pixels = new byte[] { 100, 150, 200, 255 };
        PremultiplyHelpers.PremultiplyInPlace(pixels);
        Assert.Equal(new byte[] { 100, 150, 200, 255 }, pixels);
    }

    [Fact]
    public void TransparentPixelsBecomeBlack()
    {
        var pixels = new byte[] { 100, 150, 200, 0 };
        PremultiplyHelpers.PremultiplyInPlace(pixels);
        Assert.Equal(new byte[] { 0, 0, 0, 0 }, pixels);
    }

    [Fact]
    public void HalfAlphaScalesChannels()
    {
        // alpha=128 → ~half. (200*128+127)/255 = 25727/255 = 100.886 → 100
        var pixels = new byte[] { 100, 200, 50, 128 };
        PremultiplyHelpers.PremultiplyInPlace(pixels);
        Assert.InRange(pixels[0], (byte)49, (byte)51);  // 100 * 128 / 255 ≈ 50
        Assert.InRange(pixels[1], (byte)99, (byte)101); // 200 * 128 / 255 ≈ 100
        Assert.InRange(pixels[2], (byte)24, (byte)26);  // 50 * 128 / 255 ≈ 25
        Assert.Equal((byte)128, pixels[3]);
    }

    [Fact]
    public void IsPremultipliedDetectsViolations()
    {
        Assert.True(PremultiplyHelpers.IsPremultiplied(new byte[] { 50, 50, 50, 100 }));
        Assert.False(PremultiplyHelpers.IsPremultiplied(new byte[] { 200, 50, 50, 100 }));
    }

    [Fact]
    public void NonAlignedBufferThrows()
    {
        Assert.Throws<ArgumentException>(() => PremultiplyHelpers.PremultiplyInPlace(new byte[] { 1, 2, 3 }));
    }
}
