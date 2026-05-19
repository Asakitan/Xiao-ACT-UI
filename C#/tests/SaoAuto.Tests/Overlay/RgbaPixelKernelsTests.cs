using SaoAuto.Overlay.Rendering;
using Xunit;

namespace SaoAuto.Tests.Overlay;

public class RgbaPixelKernelsTests
{
    private static byte[] Rgba(params byte[] bytes) => bytes;

    [Fact]
    public void PremultiplyRounded_swaps_channels_and_rounds()
    {
        // Single pixel R=200, G=100, B=50, A=128
        // Premul: ch * 128 + 127 / 255
        // R: (200*128+127)/255 = 25727/255 = 100
        // G: (100*128+127)/255 = 12927/255 = 50
        // B: (50*128+127)/255  = 6527/255  = 25
        var src = Rgba(200, 100, 50, 128);
        var dst = RgbaPixelKernels.PremultiplyRgbaToBgraRounded(src, 1, 1);
        Assert.Equal(25, dst[0]);  // B
        Assert.Equal(50, dst[1]);  // G
        Assert.Equal(100, dst[2]); // R
        Assert.Equal(128, dst[3]); // A
    }

    [Fact]
    public void PremultiplyRounded_alpha_255_preserves()
    {
        var src = Rgba(200, 100, 50, 255);
        var dst = RgbaPixelKernels.PremultiplyRgbaToBgraRounded(src, 1, 1);
        Assert.Equal(50, dst[0]);
        Assert.Equal(100, dst[1]);
        Assert.Equal(200, dst[2]);
        Assert.Equal(255, dst[3]);
    }

    [Fact]
    public void PremultiplyFloor_uses_floor_division()
    {
        // R=100, A=128 → 100*128/255 = 12800/255 = 50 (floor)
        // Rounded would be (100*128+127)/255 = 50 also, so pick a tighter case
        // R=10, A=10 → floor: 100/255 = 0; rounded: 227/255 = 0 — same
        // R=100, A=200 → floor: 20000/255 = 78; rounded: 20127/255 = 78 — same
        // R=1, A=128 → floor: 128/255 = 0; rounded: 255/255 = 1 — DIFFER
        var src = Rgba(1, 0, 0, 128);
        var floorDst = RgbaPixelKernels.PremultiplyRgbaToBgraFloor(src, 1, 1);
        var roundDst = RgbaPixelKernels.PremultiplyRgbaToBgraRounded(src, 1, 1);
        Assert.Equal(0, floorDst[2]);   // floor R-into-position-2
        Assert.Equal(1, roundDst[2]);   // rounded R-into-position-2
    }

    [Fact]
    public void PremultiplyFloor_master_alpha_clamps()
    {
        var src = Rgba(255, 255, 255, 200);
        var dstZero = RgbaPixelKernels.PremultiplyRgbaToBgraFloor(src, 1, 1, masterAlpha: 0.0);
        Assert.Equal(0, dstZero[3]); // master 0 → alpha 0
        var dstHalf = RgbaPixelKernels.PremultiplyRgbaToBgraFloor(src, 1, 1, masterAlpha: 0.5);
        // mul = (uint)(0.5*255) = 127; a = (200*127)/255 = 25400/255 = 99
        Assert.Equal(99, dstHalf[3]);
        var dstFull = RgbaPixelKernels.PremultiplyRgbaToBgraFloor(src, 1, 1, masterAlpha: 1.0);
        Assert.Equal(200, dstFull[3]); // no master apply (>= 0.999)
    }

    [Fact]
    public void PremultiplyFloor_rejects_size_mismatch()
    {
        Assert.Throws<ArgumentException>(() =>
            RgbaPixelKernels.PremultiplyRgbaToBgraFloor(Rgba(1, 2, 3), 1, 1));
        Assert.Throws<ArgumentException>(() =>
            RgbaPixelKernels.PremultiplyRgbaToBgraFloor(Rgba(0, 0, 0, 0), -1, 1));
    }

    [Fact]
    public void MultiplyAlphaRgba_keeps_rgb_scales_alpha()
    {
        var src = Rgba(50, 100, 200, 128);
        var dst = RgbaPixelKernels.MultiplyAlphaRgba(src, 1, 1, 0.5);
        Assert.Equal(50, dst[0]);
        Assert.Equal(100, dst[1]);
        Assert.Equal(200, dst[2]);
        // mul=127; a=(128*127)/255 = 16256/255 = 63
        Assert.Equal(63, dst[3]);
    }

    [Fact]
    public void MultiplyAlphaMaskRgba_clips_per_pixel()
    {
        // 2x1 image
        var src = Rgba(0, 0, 0, 200, 0, 0, 0, 200);
        var mask = new byte[] { 255, 0 };
        var dst = RgbaPixelKernels.MultiplyAlphaMaskRgba(src, mask, 1, 2);
        Assert.Equal(200, dst[3]); // mask 255 → unchanged
        Assert.Equal(0, dst[7]);   // mask 0 → cleared
    }

    [Fact]
    public void MultiplyAlphaRegions_only_touches_inside_rect()
    {
        // 3x1 image, alpha 200 each, rect covers x=[1,2)
        var src = Rgba(
            10, 20, 30, 200,
            40, 50, 60, 200,
            70, 80, 90, 200);
        var rects = new[] { (1, 0, 2, 1) };
        var dst = RgbaPixelKernels.MultiplyAlphaRegionsRgba(src, 1, 3, rects, 0.5);
        Assert.Equal(200, dst[3]);  // px0 untouched
        Assert.Equal(99, dst[7]);   // px1 alpha multiplied: 200*127/255=99
        Assert.Equal(200, dst[11]); // px2 untouched
        // RGB unchanged in all
        Assert.Equal(40, dst[4]);
    }

    [Fact]
    public void MultiplyAlphaRegions_clamps_oob_rect()
    {
        var src = Rgba(0, 0, 0, 100);
        var rects = new[] { (-5, -5, 100, 100) };
        var dst = RgbaPixelKernels.MultiplyAlphaRegionsRgba(src, 1, 1, rects, 0.0);
        Assert.Equal(0, dst[3]);
    }

    [Fact]
    public void ScanlineTexture_only_marks_every_third_row()
    {
        var dst = RgbaPixelKernels.ScanlineTextureRgba(width: 2, height: 6, alpha: 100);
        // y=0,1 empty; y=2 marked; y=3,4 empty; y=5 marked
        Assert.Equal(0, dst[0]);
        Assert.Equal(0, dst[1 * 2 * 4]);
        Assert.Equal(255, dst[2 * 2 * 4]);
        Assert.Equal(100, dst[2 * 2 * 4 + 3]);
        Assert.Equal(0, dst[3 * 2 * 4]);
        Assert.Equal(255, dst[5 * 2 * 4]);
    }

    [Fact]
    public void ScanlineTexture_zero_dim_returns_4_bytes()
    {
        Assert.Equal(4, RgbaPixelKernels.ScanlineTextureRgba(0, 10).Length);
        Assert.Equal(4, RgbaPixelKernels.ScanlineTextureRgba(10, 0).Length);
    }

    [Fact]
    public void HorizontalGradientBar_endpoints_match_input_colors_with_shading()
    {
        // 2x1, ca=(100,100,100,255), cb=(200,200,200,255)
        var dst = RgbaPixelKernels.HorizontalGradientBarRgba(
            width: 2, height: 1, ca: (100, 100, 100, 255), cb: (200, 200, 200, 255));
        // y=0 single row → shade=1.02
        // x=0: tx=0, rgb=100*1.02=102
        // x=1: tx=1, rgb=200*1.02=204
        Assert.Equal(102, dst[0]);
        Assert.Equal(204, dst[4]);
        Assert.Equal(255, dst[3]); // alpha keeps 255 (no shade applied to alpha)
    }

    [Fact]
    public void HorizontalGradientBar_zero_dim_returns_4_bytes()
    {
        Assert.Equal(4, RgbaPixelKernels.HorizontalGradientBarRgba(0, 5, default, default).Length);
    }

    [Fact]
    public void HorizontalGradientBar_clamps_overshoot_channels()
    {
        // shade at top = 1.02 → R=255*1.02=260 → clamps to 255
        var dst = RgbaPixelKernels.HorizontalGradientBarRgba(
            width: 1, height: 1, ca: (255, 255, 255, 255), cb: (255, 255, 255, 255));
        Assert.Equal(255, dst[0]);
        Assert.Equal(255, dst[1]);
        Assert.Equal(255, dst[2]);
        Assert.Equal(255, dst[3]);
    }
}
