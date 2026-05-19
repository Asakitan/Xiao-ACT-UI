using SaoAuto.Overlay.Rendering;

namespace SaoAuto.Tests.Overlay;

public class CyPixelsExtrasTests
{
    [Fact]
    public void AlphaBlitOpaqueSrcOverwritesDst()
    {
        var dst = new byte[16]; // 4 pixels
        var src = new byte[16];
        for (var i = 0; i < 4; i++)
        {
            src[i * 4] = 100;
            src[i * 4 + 1] = 150;
            src[i * 4 + 2] = 200;
            src[i * 4 + 3] = 255;
        }
        CyPixelsExtras.AlphaBlit(dst, src, width: 4, height: 1, dstStride: 16, srcStride: 16);
        Assert.Equal(100, dst[0]);
        Assert.Equal(150, dst[1]);
        Assert.Equal(200, dst[2]);
        Assert.Equal(255, dst[3]);
    }

    [Fact]
    public void AlphaBlitTransparentSrcLeavesDstUnchanged()
    {
        var dst = new byte[] { 50, 60, 70, 255 };
        var src = new byte[] { 200, 200, 200, 0 };
        CyPixelsExtras.AlphaBlit(dst, src, width: 1, height: 1, dstStride: 4, srcStride: 4);
        Assert.Equal(50, dst[0]);
        Assert.Equal(60, dst[1]);
        Assert.Equal(70, dst[2]);
        Assert.Equal(255, dst[3]);
    }

    [Fact]
    public void ClearRectFillsBoundedArea()
    {
        var pixels = new byte[10 * 10 * 4];
        CyPixelsExtras.ClearRect(pixels, 10, 10, 40, 2, 2, 3, 3, 0xFF, 0x00, 0x00, 0x80);
        // (3, 3) is inside the cleared rect — read its BGRA.
        var off = 3 * 40 + 3 * 4;
        Assert.Equal(0xFF, pixels[off]);
        Assert.Equal(0x00, pixels[off + 1]);
        Assert.Equal(0x00, pixels[off + 2]);
        Assert.Equal(0x80, pixels[off + 3]);
    }

    [Fact]
    public void ClearRectClipsOutOfBounds()
    {
        var pixels = new byte[4 * 4 * 4];
        CyPixelsExtras.ClearRect(pixels, 4, 4, 16, -10, -10, 100, 100, 0xFF, 0xFF, 0xFF, 0xFF);
        // Whole buffer should be filled with 0xFF.
        for (var i = 0; i < pixels.Length; i++) Assert.Equal(0xFF, pixels[i]);
    }

    [Fact]
    public void ScaleNearestUpsamplesEvenly()
    {
        // 2x1 source, scale to 4x1 → each src pixel duplicates twice.
        var src = new byte[]
        {
            10, 20, 30, 255,
            40, 50, 60, 255,
        };
        var dst = CyPixelsExtras.ScaleNearest(src, srcW: 2, srcH: 1, dstW: 4, dstH: 1);
        Assert.Equal(10, dst[0]);
        Assert.Equal(10, dst[4]);  // duplicate
        Assert.Equal(40, dst[8]);
        Assert.Equal(40, dst[12]); // duplicate
    }
}
