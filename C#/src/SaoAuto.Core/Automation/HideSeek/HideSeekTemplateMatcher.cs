namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// S147 — Port of <c>hide_seek_engine.py</c>'s template-matching half.
/// Re-implements <c>cv2.matchTemplate</c> with TM_CCOEFF_NORMED and
/// TM_SQDIFF_NORMED for the small ROI + template sizes HideSeek
/// actually uses (templates are ROI-sized, so only a handful of
/// window positions per call). Pure C#, no OpenCV.
///
/// Grayscale conversion matches OpenCV's BGR→GRAY weights
/// (0.114·B + 0.587·G + 0.299·R rounded to int).
/// </summary>
public static class HideSeekTemplateMatcher
{
    /// <summary>Best-match position and confidence score.</summary>
    public readonly record struct MatchResult(int X, int Y, double Score, bool Found);

    /// <summary>
    /// Convert a BGR (or BGRA) byte buffer to a tight width×height
    /// 8-bit grayscale array using OpenCV's BT.601 weights.
    /// </summary>
    public static byte[] ToGrayscale(
        ReadOnlySpan<byte> pixels,
        int width,
        int height,
        int stride,
        int channels)
    {
        if (channels != 1 && channels != 3 && channels != 4)
            throw new ArgumentOutOfRangeException(nameof(channels), "channels must be 1, 3, or 4.");
        if (stride < width * channels)
            throw new ArgumentOutOfRangeException(nameof(stride), "stride is shorter than width × channels.");
        if (pixels.Length < stride * (height - 1) + width * channels)
            throw new ArgumentException("pixels buffer is shorter than declared geometry.", nameof(pixels));

        var gray = new byte[width * height];
        if (channels == 1)
        {
            for (int y = 0; y < height; y++)
            {
                int src = y * stride;
                int dst = y * width;
                pixels.Slice(src, width).CopyTo(gray.AsSpan(dst, width));
            }
            return gray;
        }

        for (int y = 0; y < height; y++)
        {
            int srcRow = y * stride;
            int dstRow = y * width;
            for (int x = 0; x < width; x++)
            {
                int s = srcRow + x * channels;
                byte b = pixels[s];
                byte g = pixels[s + 1];
                byte r = pixels[s + 2];
                // OpenCV: Y = 0.114*B + 0.587*G + 0.299*R (8-bit fixed-point)
                int yVal = (b * 1868 + g * 9617 + r * 4899 + 8192) >> 14;
                gray[dstRow + x] = (byte)yVal;
            }
        }
        return gray;
    }

    /// <summary>
    /// TM_CCOEFF_NORMED: slide template across image, return best
    /// (x, y, score) with score in [-1, 1] (higher = better match).
    /// <paramref name="threshold"/> drives the <c>Found</c> flag.
    /// </summary>
    public static MatchResult MatchNcc(
        ReadOnlySpan<byte> img, int iw, int ih,
        ReadOnlySpan<byte> tpl, int tw, int th,
        double threshold = 0.70)
    {
        if (!ValidateGeometry(img, iw, ih, tpl, tw, th))
            return new MatchResult(0, 0, 0.0, false);

        int n = tw * th;
        long tSum = 0, tSqSum = 0;
        for (int j = 0; j < th; j++)
            for (int i = 0; i < tw; i++)
            {
                byte v = tpl[j * tw + i];
                tSum += v;
                tSqSum += v * v;
            }
        double tMean = tSum / (double)n;
        double tVar = tSqSum - tSum * (double)tSum / n; // Σ T'^2

        double bestScore = -2.0;
        int bestX = 0, bestY = 0;
        int xMax = iw - tw, yMax = ih - th;
        for (int y = 0; y <= yMax; y++)
        {
            for (int x = 0; x <= xMax; x++)
            {
                long iSum = 0, iSqSum = 0, crossSum = 0;
                for (int j = 0; j < th; j++)
                {
                    int srcRow = (y + j) * iw + x;
                    int tplRow = j * tw;
                    for (int i = 0; i < tw; i++)
                    {
                        byte iv = img[srcRow + i];
                        byte tv = tpl[tplRow + i];
                        iSum += iv;
                        iSqSum += iv * iv;
                        crossSum += iv * tv;
                    }
                }
                double iVar = iSqSum - iSum * (double)iSum / n;
                double denom = Math.Sqrt(tVar * iVar);
                double numer = crossSum - tMean * iSum;
                double score = denom > 1e-12 ? numer / denom : 0.0;
                if (score > bestScore)
                {
                    bestScore = score;
                    bestX = x;
                    bestY = y;
                }
            }
        }
        return new MatchResult(bestX, bestY, bestScore, bestScore >= threshold);
    }

    /// <summary>
    /// TM_SQDIFF_NORMED: lower = more similar. Score in [0, ~1+].
    /// <paramref name="threshold"/> drives the <c>Found</c> flag
    /// (Found = score ≤ threshold).
    /// </summary>
    public static MatchResult MatchSqDiff(
        ReadOnlySpan<byte> img, int iw, int ih,
        ReadOnlySpan<byte> tpl, int tw, int th,
        double threshold = 0.10)
    {
        if (!ValidateGeometry(img, iw, ih, tpl, tw, th))
            return new MatchResult(0, 0, 1.0, false);

        long tSqSum = 0;
        for (int j = 0; j < th; j++)
            for (int i = 0; i < tw; i++)
            {
                byte v = tpl[j * tw + i];
                tSqSum += v * v;
            }

        double bestScore = double.PositiveInfinity;
        int bestX = 0, bestY = 0;
        int xMax = iw - tw, yMax = ih - th;
        for (int y = 0; y <= yMax; y++)
        {
            for (int x = 0; x <= xMax; x++)
            {
                long iSqSum = 0, crossSum = 0;
                for (int j = 0; j < th; j++)
                {
                    int srcRow = (y + j) * iw + x;
                    int tplRow = j * tw;
                    for (int i = 0; i < tw; i++)
                    {
                        byte iv = img[srcRow + i];
                        byte tv = tpl[tplRow + i];
                        iSqSum += iv * iv;
                        crossSum += iv * tv;
                    }
                }
                // Σ (T - I)^2 = Σ T^2 + Σ I^2 - 2 Σ T·I
                double sqDiff = tSqSum + iSqSum - 2.0 * crossSum;
                double denom = Math.Sqrt((double)tSqSum * iSqSum);
                double score = denom > 1e-12 ? sqDiff / denom : 1.0;
                if (score < bestScore)
                {
                    bestScore = score;
                    bestX = x;
                    bestY = y;
                }
            }
        }
        if (double.IsPositiveInfinity(bestScore)) bestScore = 1.0;
        return new MatchResult(bestX, bestY, bestScore, bestScore <= threshold);
    }

    /// <summary>
    /// Mirror of Python <c>_resize_template</c>: if the template is
    /// larger than the ROI in either dimension, scale it down by
    /// <c>min(rh/th, rw/tw) × 0.95</c>; return null when the scale
    /// drops to ≤ 0.1. Uses nearest-neighbour, adequate for the
    /// "template too big" fallback path.
    /// </summary>
    public static (byte[] Pixels, int Width, int Height)? ResizeTemplateIfTooLarge(
        ReadOnlySpan<byte> tpl, int tw, int th,
        int roiWidth, int roiHeight)
    {
        if (th <= roiHeight && tw <= roiWidth)
        {
            var copy = tpl.ToArray();
            return (copy, tw, th);
        }
        double scale = Math.Min(roiHeight / (double)th, roiWidth / (double)tw) * 0.95;
        if (scale <= 0.1) return null;
        int newW = Math.Max(1, (int)(tw * scale));
        int newH = Math.Max(1, (int)(th * scale));
        var output = new byte[newW * newH];
        for (int y = 0; y < newH; y++)
        {
            int sy = Math.Min(th - 1, (int)(y / scale));
            for (int x = 0; x < newW; x++)
            {
                int sx = Math.Min(tw - 1, (int)(x / scale));
                output[y * newW + x] = tpl[sy * tw + sx];
            }
        }
        return (output, newW, newH);
    }

    /// <summary>
    /// HS-01: multi-scale CCOEFF_NORMED fallback. When <see cref="MatchNcc"/>
    /// at the native template size returns below threshold, rescale the
    /// template by a fixed set of factors and pick the best score. Mirrors
    /// Python's multi-scale loop at hide_seek_engine.py:441-485 — used to
    /// recover detection when the ROI was captured at a different DPI /
    /// window scale than the bundled template.
    /// </summary>
    /// <param name="scales">Pre-sorted set of scale factors; default {0.85,
    /// 0.92, 1.08, 1.15} matches Python's tuning at hide_seek_engine.py:447.</param>
    public static MatchResult MatchNccMultiScale(
        ReadOnlySpan<byte> img, int iw, int ih,
        ReadOnlySpan<byte> tpl, int tw, int th,
        double threshold = 0.70,
        ReadOnlySpan<double> scales = default)
    {
        // Native size first — most matches succeed there with no rescale cost.
        var native = MatchNcc(img, iw, ih, tpl, tw, th, threshold);
        if (native.Found) return native;
        // Pre-default the scale set inside the body so the parameter can stay
        // as a ReadOnlySpan<double> (which can't carry an array default).
        Span<double> defaultScales = stackalloc double[] { 0.85, 0.92, 1.08, 1.15 };
        var actualScales = scales.IsEmpty ? defaultScales : scales;
        var best = native;
        // Heap-allocate the scratch buffer ONCE sized for the largest scale.
        var maxScale = 1.0;
        foreach (var s in actualScales)
        {
            if (s > maxScale) maxScale = s;
        }
        var maxW = Math.Max(tw, (int)(tw * maxScale + 1));
        var maxH = Math.Max(th, (int)(th * maxScale + 1));
        var scratch = new byte[maxW * maxH];
        foreach (var s in actualScales)
        {
            if (s <= 0.05) continue;
            var sw = Math.Max(1, (int)(tw * s));
            var sh = Math.Max(1, (int)(th * s));
            if (sw > iw || sh > ih) continue;
            // Nearest-neighbour rescale into scratch.
            for (int y = 0; y < sh; y++)
            {
                int sy = Math.Min(th - 1, (int)(y / s));
                for (int x = 0; x < sw; x++)
                {
                    int sx = Math.Min(tw - 1, (int)(x / s));
                    scratch[y * sw + x] = tpl[sy * tw + sx];
                }
            }
            var attempt = MatchNcc(img, iw, ih, scratch.AsSpan(0, sw * sh), sw, sh, threshold);
            if (attempt.Score > best.Score) best = attempt;
            if (best.Found) break; // Short-circuit on first passing scale.
        }
        return best;
    }

    private static bool ValidateGeometry(
        ReadOnlySpan<byte> img, int iw, int ih,
        ReadOnlySpan<byte> tpl, int tw, int th)
    {
        if (tw <= 0 || th <= 0 || iw <= 0 || ih <= 0) return false;
        if (tw > iw || th > ih) return false;
        if (img.Length < iw * ih) return false;
        if (tpl.Length < tw * th) return false;
        return true;
    }
}
