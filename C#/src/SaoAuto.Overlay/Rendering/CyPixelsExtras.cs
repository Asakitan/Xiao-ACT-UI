namespace SaoAuto.Overlay.Rendering;

/// <summary>
/// BGRA32 helpers ported from <c>_sao_cy_pixels.pyx</c>: in-place alpha
/// blend, rectangle clear, simple nearest-neighbor scale. Operates over
/// top-down BGRA buffers used by <see cref="FrameBuffer"/>.
/// </summary>
public static class CyPixelsExtras
{
    /// <summary>
    /// Blend <paramref name="src"/> over <paramref name="dst"/> in place.
    /// Both buffers are top-down BGRA32 with the same width/height. Mirrors
    /// the Cython `alpha_blit` helper: standard "source over" with
    /// pre-multiplied alpha on both sides.
    /// </summary>
    public static void AlphaBlit(
        Span<byte> dst, ReadOnlySpan<byte> src, int width, int height, int dstStride, int srcStride)
    {
        if (dst.Length < dstStride * height || src.Length < srcStride * height)
        {
            throw new ArgumentException("AlphaBlit buffer underflow");
        }
        for (var y = 0; y < height; y++)
        {
            var dstRow = dst.Slice(y * dstStride, width * 4);
            var srcRow = src.Slice(y * srcStride, width * 4);
            for (var x = 0; x < width * 4; x += 4)
            {
                var sa = srcRow[x + 3];
                if (sa == 0) continue;
                if (sa == 255)
                {
                    dstRow[x] = srcRow[x];
                    dstRow[x + 1] = srcRow[x + 1];
                    dstRow[x + 2] = srcRow[x + 2];
                    dstRow[x + 3] = srcRow[x + 3];
                    continue;
                }
                // dst_premul = dst + src * (1 - sa/255)
                var inv = 255 - sa;
                dstRow[x]     = (byte)(srcRow[x]     + (dstRow[x]     * inv + 127) / 255);
                dstRow[x + 1] = (byte)(srcRow[x + 1] + (dstRow[x + 1] * inv + 127) / 255);
                dstRow[x + 2] = (byte)(srcRow[x + 2] + (dstRow[x + 2] * inv + 127) / 255);
                dstRow[x + 3] = (byte)(srcRow[x + 3] + (dstRow[x + 3] * inv + 127) / 255);
            }
        }
    }

    /// <summary>Fill a rectangle inside <paramref name="dst"/> with a BGRA color.</summary>
    public static void ClearRect(Span<byte> dst, int width, int height, int stride,
        int x, int y, int w, int h, byte b, byte g, byte r, byte a)
    {
        if (w <= 0 || h <= 0) return;
        var x0 = Math.Max(0, x);
        var y0 = Math.Max(0, y);
        var x1 = Math.Min(width, x + w);
        var y1 = Math.Min(height, y + h);
        if (x0 >= x1 || y0 >= y1) return;
        for (var py = y0; py < y1; py++)
        {
            var row = dst.Slice(py * stride, width * 4);
            for (var px = x0; px < x1; px++)
            {
                var off = px * 4;
                row[off] = b;
                row[off + 1] = g;
                row[off + 2] = r;
                row[off + 3] = a;
            }
        }
    }

    /// <summary>Nearest-neighbor scale of a BGRA bitmap.</summary>
    public static byte[] ScaleNearest(ReadOnlySpan<byte> src, int srcW, int srcH, int dstW, int dstH)
    {
        if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        {
            return Array.Empty<byte>();
        }
        var dst = new byte[dstW * dstH * 4];
        var srcStride = srcW * 4;
        var dstStride = dstW * 4;
        for (var y = 0; y < dstH; y++)
        {
            var sy = y * srcH / dstH;
            for (var x = 0; x < dstW; x++)
            {
                var sx = x * srcW / dstW;
                var srcOff = sy * srcStride + sx * 4;
                var dstOff = y * dstStride + x * 4;
                dst[dstOff] = src[srcOff];
                dst[dstOff + 1] = src[srcOff + 1];
                dst[dstOff + 2] = src[srcOff + 2];
                dst[dstOff + 3] = src[srcOff + 3];
            }
        }
        return dst;
    }
}
