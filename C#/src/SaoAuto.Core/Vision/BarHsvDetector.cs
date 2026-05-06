namespace SaoAuto.Core.Vision;

/// <summary>
/// HSV colour gate for bar detection. Mirrors the
/// <c>color_cfg</c> dict shape passed to
/// <c>recognition._detect_bar_pct</c> — only the hue/saturation
/// floors used by that function. Values follow OpenCV's H:[0,180]
/// S:[0,255] convention.
/// </summary>
public readonly record struct BarColorConfig(double HMin, double HMax, double SMin);

/// <summary>
/// S77 — HSV-aware bar fill detector. Bit-faithful port of
/// <c>recognition._detect_bar_pct</c> (lines 575–746): mean-blur
/// (bilateral substitute) → BGR→HSV → vertical/horizontal padding
/// → hue-aware mask → per-column scoring with hue-bonus penalty
/// → smoothed reference percentiles → two-tier hue presence gate
/// → full-bar / near-full hue-edge walk → three estimates
/// (gradient sub-pixel, per-row median, threshold sub-pixel)
/// combined via weighted average. Sub-pixel helpers reused from
/// <see cref="BarSubpixel"/> (S38).
///
/// No OpenCvSharp dep — bilateral filter is replaced with a 3x3
/// mean blur (intentional simplification flagged in the
/// session-77 handoff). All other math is pure scalar.
/// </summary>
public static class BarHsvDetector
{
    public readonly record struct Result(double Pct, double Confidence)
    {
        public static readonly Result Empty = new(0.0, 0.0);
    }

    public static Result Detect(ReadOnlySpan<byte> bgr, int width, int height, BarColorConfig cfg)
    {
        if (height < 2 || width < 4 || bgr.Length < width * height * 3)
            return Result.Empty;

        var blurred = MeanBlur3x3Bgr(bgr, width, height);
        BgrToHsvInPlace(blurred, width * height);

        // hsv layout: same buffer, interpreted as H/S/V per 3-byte pixel.
        var yPad = Math.Max(1, (int)Math.Round(height * 0.12));
        var ySampleStart = yPad;
        var ySampleEnd = Math.Max(yPad + 1, height - yPad);
        if (ySampleEnd <= ySampleStart) { ySampleStart = 0; ySampleEnd = height; }

        var xPad = Math.Max(2, (int)Math.Round(width * 0.018));
        var x1 = Math.Min(Math.Max(0, xPad), Math.Max(0, width - 8));
        var x2 = Math.Max(x1 + 8, width - xPad);
        var effW = x2 - x1;
        if (effW <= 4) return Result.Empty;

        var rows = ySampleEnd - ySampleStart;
        var meanHue = new double[effW];
        var meanSat = new double[effW];
        var meanVal = new double[effW];
        var hueCoverage = new double[effW];

        var sFloor = Math.Max(18.0, cfg.SMin * 0.42);

        for (var x = 0; x < effW; x++)
        {
            double hSum = 0, sSum = 0, vSum = 0;
            var maskHits = 0;
            for (var y = ySampleStart; y < ySampleEnd; y++)
            {
                var pi = (y * width + (x + x1)) * 3;
                var hh = blurred[pi];      // H
                var ss = blurred[pi + 1];  // S
                var vv = blurred[pi + 2];  // V
                hSum += hh;
                sSum += ss;
                vSum += vv;
                if (hh >= cfg.HMin && hh <= cfg.HMax && ss >= sFloor) maskHits++;
            }
            meanHue[x] = hSum / rows;
            meanSat[x] = sSum / rows;
            meanVal[x] = vSum / rows;
            hueCoverage[x] = maskHits / (double)rows;
        }

        // Reference hue from leftmost 12%
        var leftRefWidth = Math.Max(6, (int)Math.Round(effW * 0.12));
        var leftHues = new double[leftRefWidth];
        Array.Copy(meanHue, 0, leftHues, 0, leftRefWidth);
        var fillHueRef = Percentile(leftHues, 55);

        // col_score with hue-bonus penalty for hue_coverage < 0.12
        var colScore = new double[effW];
        for (var x = 0; x < effW; x++)
        {
            var hueDelta = Math.Abs(meanHue[x] - fillHueRef);
            hueDelta = Math.Min(hueDelta, 180.0 - hueDelta);
            var hueBonus = 1.0 - Math.Clamp(hueDelta / 24.0, 0.0, 1.0);
            var raw = 0.78 * (meanVal[x] / 255.0) + 0.22 * (meanSat[x] / 255.0);
            colScore[x] = hueCoverage[x] >= 0.12 ? raw : raw * (0.72 + 0.18 * hueBonus);
        }
        var smoothScore = BarSubpixel.ConvolveSame(colScore, kernelWidth: 5);

        // Tier-1 hue presence
        var overallHueCov = Mean(hueCoverage);
        if (overallHueCov < 0.08) return Result.Empty;

        // Tier-2: leftmost 10% anchor
        var leftAnchorW = Math.Max(4, (int)Math.Round(effW * 0.10));
        var leftAnchorHue = MeanRange(hueCoverage, 0, leftAnchorW);

        var smoothHue = BarSubpixel.ConvolveSame(hueCoverage, kernelWidth: 5);

        // References + dynamic range
        var refWidth = Math.Max(6, (int)Math.Round(effW * 0.12));
        var leftSlice = Slice(smoothScore, 0, refWidth);
        var rightSlice = Slice(smoothScore, Math.Max(0, effW - refWidth), effW);
        var leftRef = Percentile(leftSlice, 84);
        var rightRef = Percentile(rightSlice, 62);
        var dynamicRange = Math.Max(0.0, leftRef - rightRef);

        // Full-bar shortcut
        var tailW = Math.Max(3, (int)Math.Round(effW * 0.05));
        var tailHue = MeanRange(smoothHue, effW - tailW, effW);
        var headHue = MeanRange(smoothHue, 0, tailW);
        if (tailHue >= 0.40 && headHue >= 0.40 && dynamicRange <= 0.06)
            return new Result(1.0, 1.0);

        // Near-full hue-edge walk
        if (leftAnchorHue >= 0.35 && dynamicRange <= 0.12)
        {
            const double hueDropThr = 0.25;
            var edgeCol = effW;
            var stop = Math.Max(0, (int)(effW * 0.60));
            for (var i = effW - 1; i > stop; i--)
            {
                if (smoothHue[i] < hueDropThr) { edgeCol = i + 1; break; }
            }
            var nearPct = Math.Min(1.0, edgeCol / (double)effW);
            if (edgeCol >= effW)
                return new Result(1.0, Math.Min(1.0, dynamicRange / 0.18 + 0.50));
            return new Result(Math.Max(0.0, nearPct),
                Math.Min(1.0, Math.Max(0.30, dynamicRange / 0.14)));
        }

        var threshold = Math.Max(0.20, Math.Max(rightRef + dynamicRange * 0.54, leftRef * 0.71));

        // Method 1: gradient sub-pixel
        var gradientPct = BarSubpixel.GradientEdgePct(smoothScore, effW, dynamicRange);

        // Method 2: per-row median (build per-row score matrix)
        double? rowMedianPct = null;
        if (rows >= 2)
        {
            var rowScores = new double[rows, effW];
            for (var r = 0; r < rows; r++)
            {
                for (var x = 0; x < effW; x++)
                {
                    var pi = ((ySampleStart + r) * width + (x + x1)) * 3;
                    var hh = blurred[pi];
                    var ss = blurred[pi + 1];
                    var vv = blurred[pi + 2];
                    var hueDelta = Math.Abs(hh - fillHueRef);
                    hueDelta = Math.Min(hueDelta, 180.0 - hueDelta);
                    var hueBonus = 1.0 - Math.Clamp(hueDelta / 24.0, 0.0, 1.0);
                    var raw = 0.78 * (vv / 255.0) + 0.22 * (ss / 255.0);
                    var inMask = hh >= cfg.HMin && hh <= cfg.HMax && ss >= sFloor;
                    rowScores[r, x] = inMask ? raw : raw * (0.72 + 0.18 * hueBonus);
                }
            }
            rowMedianPct = BarSubpixel.RowIndependentPct(rowScores, threshold);
        }

        // Method 3: threshold sub-pixel
        var filled = new bool[effW];
        for (var x = 0; x < effW; x++) filled[x] = smoothScore[x] >= threshold;
        if (effW >= 3)
        {
            var copy = (bool[])filled.Clone();
            for (var x = 1; x < effW - 1; x++) filled[x] = copy[x] | (copy[x - 1] & copy[x + 1]);
        }
        double thresholdPct;
        var lastIdx = -1;
        for (var x = effW - 1; x >= 0; x--) { if (filled[x]) { lastIdx = x; break; } }
        if (lastIdx < 0) thresholdPct = 0.0;
        else
        {
            var sub = BarSubpixel.SubpixelThresholdCrossing(smoothScore, threshold, lastIdx);
            thresholdPct = Math.Clamp(sub / effW, 0.0, 1.0);
        }

        // Combine
        var confidence = Math.Min(1.0, dynamicRange / 0.18);
        var estimates = new List<double>(3);
        var weights = new List<double>(3);
        if (gradientPct.HasValue) { estimates.Add(gradientPct.Value); weights.Add(0.45); }
        if (rowMedianPct.HasValue) { estimates.Add(rowMedianPct.Value); weights.Add(0.30); }
        estimates.Add(thresholdPct);
        weights.Add(estimates.Count > 1 ? 0.25 : 1.0);

        double pct;
        if (estimates.Count >= 2)
        {
            double maxE = double.MinValue, minE = double.MaxValue;
            foreach (var e in estimates) { if (e > maxE) maxE = e; if (e < minE) minE = e; }
            var spread = maxE - minE;
            if (spread <= 0.03 && gradientPct.HasValue)
            {
                pct = gradientPct.Value;
                confidence = Math.Min(1.0, confidence * 1.1);
            }
            else
            {
                var totalW = 0.0;
                var sum = 0.0;
                for (var i = 0; i < estimates.Count; i++) { sum += estimates[i] * weights[i]; totalW += weights[i]; }
                pct = sum / totalW;
            }
        }
        else
        {
            pct = estimates[0];
        }

        pct = Math.Clamp(pct, 0.0, 1.0);
        var hueScale = Math.Min(1.0, overallHueCov / 0.30);
        var anchorScale = Math.Min(1.0, leftAnchorHue / 0.30);
        confidence = Math.Clamp(confidence * hueScale * anchorScale, 0.0, 1.0);
        return new Result(pct, confidence);
    }

    // ── BGR→HSV per-pixel (OpenCV convention: H ∈ [0,180]) ──────────

    public static byte[] MeanBlur3x3Bgr(ReadOnlySpan<byte> src, int w, int h)
    {
        var dst = new byte[src.Length];
        var rowStride = w * 3;
        for (var y = 0; y < h; y++)
        {
            for (var x = 0; x < w; x++)
            {
                int b = 0, g = 0, r = 0, n = 0;
                for (var dy = -1; dy <= 1; dy++)
                {
                    var yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (var dx = -1; dx <= 1; dx++)
                    {
                        var xx = x + dx;
                        if (xx < 0 || xx >= w) continue;
                        var i = yy * rowStride + xx * 3;
                        b += src[i]; g += src[i + 1]; r += src[i + 2]; n++;
                    }
                }
                var di = y * rowStride + x * 3;
                dst[di] = (byte)(b / n);
                dst[di + 1] = (byte)(g / n);
                dst[di + 2] = (byte)(r / n);
            }
        }
        return dst;
    }

    public static void BgrToHsvInPlace(byte[] buf, int pixelCount)
    {
        for (var p = 0; p < pixelCount; p++)
        {
            var i = p * 3;
            int b = buf[i], g = buf[i + 1], r = buf[i + 2];
            var max = Math.Max(r, Math.Max(g, b));
            var min = Math.Min(r, Math.Min(g, b));
            var v = max;
            var delta = max - min;
            byte s = max == 0 ? (byte)0 : (byte)Math.Min(255, (int)Math.Round(delta * 255.0 / max));
            double h;
            if (delta == 0) h = 0.0;
            else if (max == r) h = 60.0 * (((g - b) / (double)delta) % 6.0);
            else if (max == g) h = 60.0 * (((b - r) / (double)delta) + 2.0);
            else h = 60.0 * (((r - g) / (double)delta) + 4.0);
            if (h < 0) h += 360.0;
            // OpenCV halves H to fit in uint8: H ∈ [0,180]
            var h8 = (byte)Math.Min(180, (int)Math.Round(h / 2.0));
            buf[i] = h8;
            buf[i + 1] = s;
            buf[i + 2] = (byte)v;
        }
    }

    private static double Mean(double[] v)
    {
        if (v.Length == 0) return 0.0;
        var s = 0.0;
        for (var i = 0; i < v.Length; i++) s += v[i];
        return s / v.Length;
    }

    private static double MeanRange(double[] v, int start, int endExclusive)
    {
        if (endExclusive <= start) return 0.0;
        if (start < 0) start = 0;
        if (endExclusive > v.Length) endExclusive = v.Length;
        var s = 0.0;
        for (var i = start; i < endExclusive; i++) s += v[i];
        return s / (endExclusive - start);
    }

    private static double[] Slice(double[] v, int start, int endExclusive)
    {
        if (start < 0) start = 0;
        if (endExclusive > v.Length) endExclusive = v.Length;
        if (endExclusive <= start) return Array.Empty<double>();
        var r = new double[endExclusive - start];
        Array.Copy(v, start, r, 0, r.Length);
        return r;
    }

    /// <summary>Linear-interpolation percentile (matches numpy default).</summary>
    public static double Percentile(double[] v, double pct)
    {
        if (v.Length == 0) return 0.0;
        var sorted = (double[])v.Clone();
        Array.Sort(sorted);
        if (sorted.Length == 1) return sorted[0];
        var rank = pct / 100.0 * (sorted.Length - 1);
        var lo = (int)Math.Floor(rank);
        var hi = (int)Math.Ceiling(rank);
        if (lo == hi) return sorted[lo];
        var frac = rank - lo;
        return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
    }
}
