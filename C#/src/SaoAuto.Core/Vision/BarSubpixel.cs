namespace SaoAuto.Core.Vision;

/// <summary>
/// Sub-pixel bar fill estimators ported from the math-only helpers in
/// <c>recognition.py</c> (<c>_subpixel_threshold_crossing</c>,
/// <c>_gradient_edge_pct</c>, <c>_row_independent_pct</c>). These are
/// pure float-array operations with no GDI / capture dependency, so they
/// can be unit-tested against synthetic scores.
/// </summary>
public static class BarSubpixel
{
    /// <summary>
    /// Linear interpolate the sub-pixel index where <paramref name="score"/>
    /// crosses <paramref name="threshold"/> just after
    /// <paramref name="lastFilledIdx"/>. Returns <c>lastFilledIdx + 1</c>
    /// when no clean crossing is found.
    /// </summary>
    public static double SubpixelThresholdCrossing(ReadOnlySpan<double> score, double threshold, int lastFilledIdx)
    {
        var effW = score.Length;
        if (effW == 0) return 0;
        if (lastFilledIdx >= effW - 1) return lastFilledIdx + 1;
        var s0 = score[lastFilledIdx];
        var s1 = score[Math.Min(lastFilledIdx + 1, effW - 1)];
        if (s0 > threshold && threshold >= s1 && s0 != s1)
        {
            var frac = (s0 - threshold) / (s0 - s1);
            return lastFilledIdx + frac;
        }
        return lastFilledIdx + 1;
    }

    /// <summary>
    /// Locate the sharpest negative gradient in <paramref name="smoothScore"/>
    /// (already smoothed) and return the sub-pixel boundary as a fraction
    /// of <paramref name="effW"/>. Returns null when no edge is sharp
    /// enough relative to <paramref name="dynamicRange"/>.
    /// </summary>
    public static double? GradientEdgePct(ReadOnlySpan<double> smoothScore, int effW, double dynamicRange)
    {
        if (effW <= 6) return null;
        if (smoothScore.Length < 2) return null;

        var gradient = new double[smoothScore.Length - 1];
        for (var i = 0; i < gradient.Length; i++) gradient[i] = smoothScore[i + 1] - smoothScore[i];
        if (gradient.Length < 5) return null;

        var smoothGrad = ConvolveSame(gradient, kernelWidth: 7);

        var sStart = Math.Max(2, (int)(effW * 0.03));
        var sEnd = Math.Max(sStart + 4, effW - 1 - Math.Max(3, (int)(effW * 0.02)));
        if (sEnd > smoothGrad.Length) sEnd = smoothGrad.Length;
        if (sStart >= sEnd) return null;

        var minIdx = sStart;
        var minVal = smoothGrad[sStart];
        for (var i = sStart + 1; i < sEnd; i++)
        {
            if (smoothGrad[i] < minVal) { minVal = smoothGrad[i]; minIdx = i; }
        }

        var gradThreshold = -Math.Max(0.008, dynamicRange * 0.08);
        if (minVal >= gradThreshold) return null;

        var midScore = (smoothScore[minIdx] + smoothScore[Math.Min(minIdx + 1, effW - 1)]) * 0.5;
        var jStart = Math.Max(0, minIdx - 2);
        var jEnd = Math.Min(effW - 1, minIdx + 4);
        for (var j = jStart; j <= jEnd; j++)
        {
            var s0 = smoothScore[j];
            var s1 = smoothScore[Math.Min(j + 1, effW - 1)];
            if (s0 >= midScore && midScore > s1 && s0 != s1)
            {
                var frac = (s0 - midScore) / (s0 - s1);
                return Math.Clamp((j + frac + 0.5) / effW, 0.0, 1.0);
            }
        }
        return Math.Clamp((minIdx + 1.0) / effW, 0.0, 1.0);
    }

    /// <summary>
    /// Per-row fill percentage (median across rows) — outlier-resistant
    /// estimator. <paramref name="rows"/>×<paramref name="effW"/> matrix
    /// of pre-computed scores in [0,1]. Returns null when fewer than 2
    /// usable rows.
    /// </summary>
    public static double? RowIndependentPct(double[,] rowScores, double threshold)
    {
        var nRows = rowScores.GetLength(0);
        var effW = rowScores.GetLength(1);
        if (nRows < 2 || effW <= 4) return null;

        var pcts = new List<double>(nRows);
        var rowBuf = new double[effW];
        for (var r = 0; r < nRows; r++)
        {
            for (var c = 0; c < effW; c++) rowBuf[c] = rowScores[r, c];
            var smoothed = ConvolveSame(rowBuf, kernelWidth: 3);
            var lastIdx = -1;
            for (var c = 0; c < effW; c++)
            {
                if (smoothed[c] >= threshold) lastIdx = c;
            }
            if (lastIdx < 0) continue;
            var pct = SubpixelThresholdCrossing(smoothed, threshold, lastIdx) / effW;
            pcts.Add(Math.Clamp(pct, 0.0, 1.0));
        }
        if (pcts.Count < 2) return null;
        pcts.Sort();
        var mid = pcts.Count / 2;
        return (pcts.Count % 2 == 0) ? (pcts[mid - 1] + pcts[mid]) * 0.5 : pcts[mid];
    }

    /// <summary>
    /// Same-mode convolution with a uniform-weight kernel (mean filter).
    /// Mirrors NumPy <c>np.convolve(x, np.ones(k)/k, mode="same")</c>.
    /// </summary>
    public static double[] ConvolveSame(ReadOnlySpan<double> input, int kernelWidth)
    {
        if (kernelWidth <= 1) return input.ToArray();
        var n = input.Length;
        var output = new double[n];
        var w = 1.0 / kernelWidth;
        var half = kernelWidth / 2;
        for (var i = 0; i < n; i++)
        {
            // numpy 'same' aligns the kernel center on output[i]; for even
            // kernels numpy uses center = k/2 (0-indexed).
            var sum = 0.0;
            for (var k = 0; k < kernelWidth; k++)
            {
                var idx = i + k - half;
                if (idx >= 0 && idx < n) sum += input[idx];
            }
            output[i] = sum * w;
        }
        return output;
    }
}
