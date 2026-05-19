namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// S146 — pure C# port of <c>hide_seek_engine.py</c>'s
/// <c>_mask_in_range</c> colour-filter pre-pass. Given a BGR or BGRA
/// pixel buffer, returns a single-channel mask where each output byte
/// is 255 when the source pixel is within tolerance of any of the
/// supplied colour centres, else 0. A final 3×3 rectangular dilation
/// (1 iteration) is applied to join nearby pixels, matching Python's
/// <c>cv2.dilate</c> step.
///
/// Kept free of OpenCV so Core does not pick up a heavyweight CV
/// dependency for what is ultimately a few hundred multiplications
/// per ROI.
/// </summary>
public static class HideSeekColorMask
{
    /// <summary>
    /// Build the inRange-then-dilate mask.
    /// </summary>
    /// <param name="pixels">Source pixel buffer (BGR or BGRA).</param>
    /// <param name="width">Width in pixels.</param>
    /// <param name="height">Height in pixels.</param>
    /// <param name="stride">Bytes per row (must be ≥ width × channels).</param>
    /// <param name="channels">3 (BGR) or 4 (BGRA).</param>
    /// <param name="colors">Colour centres + per-channel tolerance.</param>
    /// <returns>Tight row-major width×height mask (255 / 0).</returns>
    public static byte[] Build(
        ReadOnlySpan<byte> pixels,
        int width,
        int height,
        int stride,
        int channels,
        IReadOnlyList<HideSeekColor> colors)
    {
        if (width <= 0 || height <= 0) return Array.Empty<byte>();
        if (channels != 3 && channels != 4)
            throw new ArgumentOutOfRangeException(nameof(channels), "channels must be 3 (BGR) or 4 (BGRA).");
        if (stride < width * channels)
            throw new ArgumentOutOfRangeException(nameof(stride), "stride is shorter than width × channels.");
        if (pixels.Length < stride * (height - 1) + width * channels)
            throw new ArgumentException("pixels buffer is shorter than declared geometry.", nameof(pixels));
        ArgumentNullException.ThrowIfNull(colors);

        var raw = new byte[width * height];
        if (colors.Count == 0) return raw;

        Span<byte> lowB = stackalloc byte[colors.Count];
        Span<byte> highB = stackalloc byte[colors.Count];
        Span<byte> lowG = stackalloc byte[colors.Count];
        Span<byte> highG = stackalloc byte[colors.Count];
        Span<byte> lowR = stackalloc byte[colors.Count];
        Span<byte> highR = stackalloc byte[colors.Count];

        for (int i = 0; i < colors.Count; i++)
        {
            var c = colors[i];
            lowB[i] = (byte)Math.Max(0, c.B - c.Tolerance);
            highB[i] = (byte)Math.Min(255, c.B + c.Tolerance);
            lowG[i] = (byte)Math.Max(0, c.G - c.Tolerance);
            highG[i] = (byte)Math.Min(255, c.G + c.Tolerance);
            lowR[i] = (byte)Math.Max(0, c.R - c.Tolerance);
            highR[i] = (byte)Math.Min(255, c.R + c.Tolerance);
        }

        for (int y = 0; y < height; y++)
        {
            int srcRow = y * stride;
            int dstRow = y * width;
            for (int x = 0; x < width; x++)
            {
                int srcIdx = srcRow + x * channels;
                byte b = pixels[srcIdx];
                byte g = pixels[srcIdx + 1];
                byte r = pixels[srcIdx + 2];
                for (int i = 0; i < colors.Count; i++)
                {
                    if (b >= lowB[i] && b <= highB[i]
                        && g >= lowG[i] && g <= highG[i]
                        && r >= lowR[i] && r <= highR[i])
                    {
                        raw[dstRow + x] = 255;
                        break;
                    }
                }
            }
        }

        return Dilate3x3(raw, width, height);
    }

    /// <summary>
    /// 3×3 rectangular dilation, single iteration. Matches OpenCV's
    /// <c>cv2.dilate(mask, getStructuringElement(MORPH_RECT, (3,3)))</c>:
    /// output pixel = 255 if any of the 9 source pixels in the
    /// surrounding window is non-zero.
    /// </summary>
    public static byte[] Dilate3x3(ReadOnlySpan<byte> mask, int width, int height)
    {
        if (mask.Length != width * height)
            throw new ArgumentException("mask length does not match width × height.", nameof(mask));
        var output = new byte[width * height];
        for (int y = 0; y < height; y++)
        {
            int y0 = Math.Max(0, y - 1);
            int y1 = Math.Min(height - 1, y + 1);
            for (int x = 0; x < width; x++)
            {
                int x0 = Math.Max(0, x - 1);
                int x1 = Math.Min(width - 1, x + 1);
                byte hit = 0;
                for (int yy = y0; yy <= y1 && hit == 0; yy++)
                {
                    int row = yy * width;
                    for (int xx = x0; xx <= x1; xx++)
                    {
                        if (mask[row + xx] != 0) { hit = 255; break; }
                    }
                }
                output[y * width + x] = hit;
            }
        }
        return output;
    }
}
