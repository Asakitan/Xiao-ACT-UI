namespace SaoAuto.Core.Vision;

/// <summary>
/// S76 — exact-BGR colour matching detector for the stamina bar.
/// Bit-faithful port of <c>_detect_stamina_pct</c>
/// (recognition.py 486–572): per-pixel Euclidean distance to the
/// gold reference colour, column-wise fill ratio, rightmost
/// filled column, near-full hue extension + tail confidence
/// boost. Pure scalar math, no OpenCV.
/// </summary>
public static class StaminaColorDetector
{
    /// <summary>Reference gold colour (BGR = #FFAE35 → 53, 174, 255).</summary>
    public const float StaB = 53f;
    public const float StaG = 174f;
    public const float StaR = 255f;

    /// <summary>Max Euclidean distance to count as "gold".</summary>
    public const float DistThreshold = 50f;

    /// <summary>Min fraction of rows matching per column for primary fill.</summary>
    public const double ColFillThreshold = 0.18;

    /// <summary>Relaxed threshold for rightmost columns near 100%.</summary>
    public const double ColFillNearFull = 0.07;

    public readonly record struct Result(double Pct, double Confidence)
    {
        public static readonly Result Empty = new(0.0, 0.0);
    }

    /// <summary>
    /// <paramref name="bgr"/> is row-major BGR (3 bytes/pixel,
    /// stride = width*3). Returns (pct, confidence).
    /// </summary>
    public static Result Detect(ReadOnlySpan<byte> bgr, int width, int height)
    {
        if (height < 2 || width < 4 || bgr.Length < width * height * 3)
            return Result.Empty;

        // col_fill[x] = fraction of rows whose pixel matches gold
        Span<int> matchCount = stackalloc int[0];
        var colMatch = width <= 4096 ? stackalloc int[width] : new int[width];
        var rowStride = width * 3;
        var totalMatch = 0;
        var thresholdSq = DistThreshold * DistThreshold;

        for (var y = 0; y < height; y++)
        {
            var row = bgr.Slice(y * rowStride, rowStride);
            for (var x = 0; x < width; x++)
            {
                var i = x * 3;
                float db = row[i] - StaB;
                float dg = row[i + 1] - StaG;
                float dr = row[i + 2] - StaR;
                if ((db * db + dg * dg + dr * dr) <= thresholdSq)
                {
                    colMatch[x]++;
                    totalMatch++;
                }
            }
        }

        var totalPixels = (double)width * height;
        var overallMatch = totalMatch / totalPixels;
        var confidence = Math.Min(1.0, overallMatch / 0.20);

        // Per-column fill ratios
        Span<double> colFill = width <= 4096 ? stackalloc double[width] : new double[width];
        var maxColFill = 0.0;
        for (var x = 0; x < width; x++)
        {
            colFill[x] = colMatch[x] / (double)height;
            if (colFill[x] > maxColFill) maxColFill = colFill[x];
        }

        if (overallMatch < 0.03 && maxColFill < ColFillNearFull)
            return Result.Empty;

        // Primary scan — rightmost column above ColFillThreshold
        var rightmost = -1;
        for (var x = width - 1; x >= 0; x--)
        {
            if (colFill[x] >= ColFillThreshold) { rightmost = x; break; }
        }

        if (rightmost < 0)
        {
            // Relaxed near-full path: contiguous (gap≤3) span from x=0..1
            return RelaxedNearFull(colFill, width, confidence);
        }

        rightmost += 1;
        var pct = Math.Clamp(rightmost / (double)width, 0.0, 1.0);
        var bodyFill = MeanRange(colFill, 0, rightmost);
        confidence = Math.Max(confidence, Math.Min(1.0, bodyFill / 0.24));

        // Near-full extension: contiguous relaxed columns past rightmost
        if (pct >= 0.78 && confidence >= 0.40)
        {
            var ext = ExtendContiguous(colFill, rightmost, width);
            if (ext > rightmost)
            {
                rightmost = ext;
                pct = Math.Clamp(rightmost / (double)width, 0.0, 1.0);
            }
        }

        // Near-full confidence boost — preserve measured pct, raise conf
        if (pct >= 0.91)
        {
            var tailW = Math.Max(3, (int)Math.Round(width * 0.09));
            var tailStart = Math.Max(0, width - tailW);
            var tailFill = MeanRange(colFill, tailStart, width);
            if (tailFill >= ColFillNearFull * 0.80)
                confidence = Math.Max(confidence, Math.Min(1.0, 0.72 + tailFill * 0.28));
        }
        return new Result(pct, confidence);
    }

    private static Result RelaxedNearFull(ReadOnlySpan<double> colFill, int width, double confidence)
    {
        // Find first relaxed col; require x ≤ 1 (contiguous from left)
        var first = -1;
        for (var x = 0; x < width; x++)
        {
            if (colFill[x] >= ColFillNearFull) { first = x; break; }
        }
        if (first < 0 || first > 1)
            return new Result(0.0, confidence * 0.5);

        var rightmost = first;
        var prev = first;
        for (var x = first + 1; x < width; x++)
        {
            if (colFill[x] < ColFillNearFull) continue;
            if (x - prev <= 3) { rightmost = x; prev = x; }
            else break;
        }

        if ((rightmost + 1) < (int)Math.Round(width * 0.85))
            return new Result(0.0, confidence * 0.5);

        var pct = Math.Clamp((rightmost + 1) / (double)width, 0.0, 1.0);
        var bodyFill = MeanRange(colFill, 0, rightmost + 1);
        confidence = Math.Max(confidence, Math.Min(1.0, 0.60 + bodyFill * 0.40));
        return new Result(pct, confidence);
    }

    private static int ExtendContiguous(ReadOnlySpan<double> colFill, int rightmost, int width)
    {
        // Search [rightmost..] for relaxed cols; first must be ≤ 2 past rightmost
        var first = -1;
        for (var x = rightmost; x < width; x++)
        {
            if (colFill[x] >= ColFillNearFull) { first = x; break; }
        }
        if (first < 0 || (first - rightmost) > 2) return rightmost - 1;

        var ext = first;
        var prev = first;
        for (var x = first + 1; x < width; x++)
        {
            if (colFill[x] < ColFillNearFull) continue;
            if (x - prev <= 3) { ext = x; prev = x; }
            else break;
        }
        return ext + 1;   // +1 so caller's pct = ext/width matches rightmost+1
    }

    private static double MeanRange(ReadOnlySpan<double> v, int start, int endExclusive)
    {
        if (endExclusive <= start) return 0.0;
        var sum = 0.0;
        for (var i = start; i < endExclusive; i++) sum += v[i];
        return sum / (endExclusive - start);
    }
}

/// <summary>
/// S76 — fallback brightness-threshold bar detector.
/// Bit-faithful port of <c>_detect_bar_pct_simple</c>
/// (recognition.py 749–759): sample 3-row band around vertical
/// midline, compute per-column mean brightness, threshold at
/// max(45.0, mean*0.68), rightmost above-threshold column.
/// </summary>
public static class BarBrightnessDetector
{
    /// <summary>
    /// <paramref name="bgr"/> is row-major BGR. Returns 0..1 fill.
    /// </summary>
    public static double Detect(ReadOnlySpan<byte> bgr, int width, int height)
    {
        if (width < 2 || height < 2 || bgr.Length < width * height * 3) return 0.0;
        var rowStride = width * 3;
        var midY = height / 2;
        var startY = Math.Max(0, midY - 1);
        var endY = Math.Min(height, midY + 2);
        var rows = endY - startY;
        if (rows <= 0) return 0.0;

        Span<double> brightness = width <= 4096 ? stackalloc double[width] : new double[width];
        var globalSum = 0.0;
        for (var x = 0; x < width; x++)
        {
            var sum = 0.0;
            for (var y = startY; y < endY; y++)
            {
                var i = y * rowStride + x * 3;
                sum += bgr[i] + bgr[i + 1] + bgr[i + 2];
            }
            // mean over (rows × 3 channels)
            brightness[x] = sum / (rows * 3.0);
            globalSum += brightness[x];
        }
        var mean = globalSum / width;
        var threshold = Math.Max(45.0, mean * 0.68);

        var rightmost = -1;
        for (var x = width - 1; x >= 0; x--)
        {
            if (brightness[x] >= threshold) { rightmost = x; break; }
        }
        if (rightmost < 0) return 0.0;
        return Math.Clamp((rightmost + 1) / (double)width, 0.0, 1.0);
    }
}

/// <summary>
/// S76 — port of <c>_capture_looks_blank</c> (recognition.py
/// 106–112): a frame is "blank" when every channel ≤ 2 and
/// per-channel std ≤ 1.0 (capture failed / window minimised).
/// </summary>
public static class BlankFrameDetector
{
    public static bool LooksBlank(ReadOnlySpan<byte> pixels)
    {
        if (pixels.Length == 0) return true;
        byte max = 0;
        long sum = 0;
        for (var i = 0; i < pixels.Length; i++)
        {
            var b = pixels[i];
            if (b > max) max = b;
            sum += b;
        }
        if (max > 2) return false;
        var mean = sum / (double)pixels.Length;
        var sqSum = 0.0;
        for (var i = 0; i < pixels.Length; i++)
        {
            var d = pixels[i] - mean;
            sqSum += d * d;
        }
        var std = Math.Sqrt(sqSum / pixels.Length);
        return std <= 1.0;
    }
}
