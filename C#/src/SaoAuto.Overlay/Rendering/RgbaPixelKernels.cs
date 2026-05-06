namespace SaoAuto.Overlay.Rendering;

/// <summary>
/// RGBA/BGRA pixel kernels ported from <c>_sao_cy_pixels.pyx</c> — the
/// rounded + floor premultiply variants, the alpha-mask / region / scalar
/// multipliers, and the two procedural textures (HP scanline, hgrad bar).
/// All buffers are top-down with stride = width * 4. Returns fresh
/// <see cref="byte"/>[] like the Python <c>cpdef bytes</c> contracts.
/// </summary>
public static class RgbaPixelKernels
{
    private const string ChannelMsg = "expected RGBA input with at least 4 channels";

    /// <summary>RGBA → premultiplied BGRA with rounded division (Python <c>premultiply_bgra_ndarray</c>).</summary>
    public static byte[] PremultiplyRgbaToBgraRounded(ReadOnlySpan<byte> rgba, int height, int width)
    {
        int expected = checked(height * width * 4);
        if (rgba.Length < expected) throw new ArgumentException(ChannelMsg);
        var dst = new byte[expected];
        for (int i = 0; i < expected; i += 4)
        {
            byte a = rgba[i + 3];
            dst[i]     = (byte)((rgba[i + 2] * a + 127) / 255);
            dst[i + 1] = (byte)((rgba[i + 1] * a + 127) / 255);
            dst[i + 2] = (byte)((rgba[i]     * a + 127) / 255);
            dst[i + 3] = a;
        }
        return dst;
    }

    /// <summary>RGBA → premultiplied BGRA with floor division and optional master-alpha (Python <c>premultiply_bgra_bytes_floor</c>).</summary>
    public static byte[] PremultiplyRgbaToBgraFloor(ReadOnlySpan<byte> rgba, int height, int width, double masterAlpha = 1.0)
    {
        if (height < 0 || width < 0) throw new ArgumentException("height and width must be non-negative");
        int expected = checked(height * width * 4);
        if (rgba.Length != expected) throw new ArgumentException("RGBA byte length does not match height * width * 4");

        bool applyMaster = masterAlpha < 0.999;
        uint mul = 255;
        if (applyMaster)
        {
            if (masterAlpha <= 0.0) mul = 0;
            else if (masterAlpha >= 1.0) mul = 255;
            else mul = (uint)(masterAlpha * 255.0);
        }

        var dst = new byte[expected];
        for (int i = 0; i < expected; i += 4)
        {
            uint a = rgba[i + 3];
            if (applyMaster) a = (a * mul) / 255;
            dst[i]     = (byte)((rgba[i + 2] * a) / 255);
            dst[i + 1] = (byte)((rgba[i + 1] * a) / 255);
            dst[i + 2] = (byte)((rgba[i]     * a) / 255);
            dst[i + 3] = (byte)a;
        }
        return dst;
    }

    /// <summary>RGBA → RGBA with alpha channel scalar-multiplied (Python <c>multiply_alpha_rgba_ndarray_floor</c>).</summary>
    public static byte[] MultiplyAlphaRgba(ReadOnlySpan<byte> rgba, int height, int width, double alpha)
    {
        int expected = checked(height * width * 4);
        if (rgba.Length < expected) throw new ArgumentException(ChannelMsg);
        uint mul = 255;
        if (alpha < 0.999)
        {
            if (alpha <= 0.0) mul = 0;
            else if (alpha >= 1.0) mul = 255;
            else mul = (uint)(alpha * 255.0);
        }
        var dst = new byte[expected];
        for (int i = 0; i < expected; i += 4)
        {
            dst[i]     = rgba[i];
            dst[i + 1] = rgba[i + 1];
            dst[i + 2] = rgba[i + 2];
            dst[i + 3] = (byte)((rgba[i + 3] * mul) / 255);
        }
        return dst;
    }

    /// <summary>RGBA clipped by an L mask (Python <c>multiply_alpha_mask_rgba_ndarray_floor</c>).</summary>
    public static byte[] MultiplyAlphaMaskRgba(ReadOnlySpan<byte> rgba, ReadOnlySpan<byte> mask, int height, int width)
    {
        int expected = checked(height * width * 4);
        if (rgba.Length < expected) throw new ArgumentException(ChannelMsg);
        if (mask.Length < height * width)
            throw new ArgumentException("mask shape must match RGBA height and width");
        var dst = new byte[expected];
        int mi = 0;
        for (int i = 0; i < expected; i += 4, mi++)
        {
            byte ma = mask[mi];
            dst[i]     = rgba[i];
            dst[i + 1] = rgba[i + 1];
            dst[i + 2] = rgba[i + 2];
            dst[i + 3] = (byte)((rgba[i + 3] * ma) / 255);
        }
        return dst;
    }

    /// <summary>RGBA copy with alpha multiplied inside the given clamped rects (Python <c>multiply_alpha_regions_rgba_bytes</c>).</summary>
    public static byte[] MultiplyAlphaRegionsRgba(
        ReadOnlySpan<byte> rgba, int height, int width,
        IEnumerable<(int X0, int Y0, int X1, int Y1)>? rects, double alpha)
    {
        int expected = checked(height * width * 4);
        if (rgba.Length < expected) throw new ArgumentException(ChannelMsg);
        uint mul = 255;
        if (alpha < 0.999)
        {
            if (alpha <= 0.0) mul = 0;
            else if (alpha >= 1.0) mul = 255;
            else mul = (uint)(alpha * 255.0);
        }
        var dst = new byte[expected];
        rgba.Slice(0, expected).CopyTo(dst);
        if (rects is null) return dst;

        foreach (var rect in rects)
        {
            int rx0 = rect.X0, ry0 = rect.Y0, rx1 = rect.X1, ry1 = rect.Y1;
            if (rx0 < 0) rx0 = 0; else if (rx0 > width) rx0 = width;
            if (ry0 < 0) ry0 = 0; else if (ry0 > height) ry0 = height;
            if (rx1 < rx0) rx1 = rx0; else if (rx1 > width) rx1 = width;
            if (ry1 < ry0) ry1 = ry0; else if (ry1 > height) ry1 = height;
            if (rx1 <= rx0 || ry1 <= ry0) continue;
            for (int yy = ry0; yy < ry1; yy++)
            {
                int pos = (yy * width + rx0) * 4 + 3;
                for (int xx = rx0; xx < rx1; xx++)
                {
                    dst[pos] = (byte)((dst[pos] * mul) / 255);
                    pos += 4;
                }
            }
        }
        return dst;
    }

    /// <summary>HP scanline texture — every 3rd row gets white pixels with the given alpha (Python <c>scanline_texture_rgba_bytes</c>).</summary>
    public static byte[] ScanlineTextureRgba(int width, int height, uint alpha = 10)
    {
        if (width <= 0 || height <= 0) return new byte[4];
        byte a = (byte)(alpha > 255 ? 255 : alpha);
        var dst = new byte[width * height * 4];
        for (int y = 0; y < height; y++)
        {
            if (y % 3 != 2) continue;
            int pos = y * width * 4;
            for (int x = 0; x < width; x++)
            {
                dst[pos]     = 255;
                dst[pos + 1] = 255;
                dst[pos + 2] = 255;
                dst[pos + 3] = a;
                pos += 4;
            }
        }
        return dst;
    }

    /// <summary>HP/STA horizontal gradient bar with vertical shading (Python <c>hgrad_bar_rgba_bytes</c>).</summary>
    public static byte[] HorizontalGradientBarRgba(
        int width, int height, (int R, int G, int B, int A) ca, (int R, int G, int B, int A) cb)
    {
        if (width <= 0 || height <= 0) return new byte[4];
        var dst = new byte[width * height * 4];
        int pos = 0;
        for (int y = 0; y < height; y++)
        {
            double shade = height > 1
                ? 1.02 + (0.88 - 1.02) * ((double)y / (height - 1))
                : 1.02;
            for (int x = 0; x < width; x++)
            {
                double tx = width > 1 ? (double)x / (width - 1) : 0.0;
                int rr = (int)((ca.R + (cb.R - ca.R) * tx) * shade);
                int gg = (int)((ca.G + (cb.G - ca.G) * tx) * shade);
                int bl = (int)((ca.B + (cb.B - ca.B) * tx) * shade);
                int al = (int)(ca.A + (cb.A - ca.A) * tx);
                if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
                if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
                if (bl < 0) bl = 0; else if (bl > 255) bl = 255;
                if (al < 0) al = 0; else if (al > 255) al = 255;
                dst[pos]     = (byte)rr;
                dst[pos + 1] = (byte)gg;
                dst[pos + 2] = (byte)bl;
                dst[pos + 3] = (byte)al;
                pos += 4;
            }
        }
        return dst;
    }
}
