namespace SaoAuto.Core.Vision;

/// <summary>
/// S78 — HSV slot-mask measurer. Bit-faithful port of
/// <c>skill_recognition._measure_slot</c> + <c>_compare_to_baseline</c>
/// (skill_recognition.py lines 36–173). Produces the
/// <see cref="SkillSlotMetrics"/> + <see cref="SkillBaselineComparison"/>
/// records consumed by <see cref="SkillStateClassifier"/> (S39).
///
/// Reuses S77 helpers: <see cref="BarHsvDetector.MeanBlur3x3Bgr"/>
/// (substitute for OpenCV's <c>gaussian_blur((3,3),0)</c>) and
/// <see cref="BarHsvDetector.BgrToHsvInPlace"/>.
///
/// <para>Constants from <c>config.BAR_COLORS["skill_cooldown"]</c>:
/// <c>v_max_dark = 80</c>, <c>s_max_gray = 40</c>.</para>
/// </summary>
public static class SkillSlotHsvMeasurer
{
    public const byte VMaxDark = 80;
    public const byte SMaxGray = 40;

    public static SkillSlotMetrics? Measure(ReadOnlySpan<byte> bgr, int width, int height)
    {
        if (width < 8 || height < 8 || bgr.Length < width * height * 3) return null;

        var hsv = BarHsvDetector.MeanBlur3x3Bgr(bgr, width, height);
        BarHsvDetector.BgrToHsvInPlace(hsv, width * height);

        if (!BuildMasks(width, height, out var iconMask, out var innerMask, out var ringMask))
            return null;

        long iconV = 0, iconS = 0, innerV = 0, innerS = 0;
        int iconTotal = 0, innerTotal = 0, ringTotal = 0;
        int cyanRingHits = 0, brightInner = 0, darkInner = 0;
        int grayDarkInner = 0, dimInner = 0, warmInner = 0, shadowIcon = 0;

        for (var y = 0; y < height; y++)
        {
            for (var x = 0; x < width; x++)
            {
                var idx = y * width + x;
                var pi = idx * 3;
                byte hh = hsv[pi], ss = hsv[pi + 1], vv = hsv[pi + 2];

                var inIcon = iconMask[idx];
                var inInner = innerMask[idx];
                var inRing = ringMask[idx];
                if (!inIcon) continue;

                iconTotal++;
                iconV += vv;
                iconS += ss;
                var dark = vv <= VMaxDark;
                if (dark) shadowIcon++;

                if (inInner)
                {
                    innerTotal++;
                    innerV += vv;
                    innerS += ss;
                    if (dark) darkInner++;
                    if (dark && ss <= SMaxGray) grayDarkInner++;
                    if (vv <= 120) dimInner++;
                    if (vv >= 175) brightInner++;
                    if (hh >= 5 && hh <= 35 && ss >= 55 && vv >= 30) warmInner++;
                }

                if (inRing)
                {
                    ringTotal++;
                    if (hh >= 72 && hh <= 126 && ss >= 30 && vv >= 100) cyanRingHits++;
                }
            }
        }

        if (iconTotal == 0 || innerTotal == 0 || ringTotal == 0) return null;

        var iconTotalD = (double)iconTotal;
        var innerTotalD = (double)innerTotal;
        var ringTotalD = (double)ringTotal;

        var innerVMean = innerV / innerTotalD;
        var innerSMean = innerS / innerTotalD;
        var iconVMean = iconV / iconTotalD;
        var iconSMean = iconS / iconTotalD;
        var ringRatio = cyanRingHits / ringTotalD;
        var brightRatio = brightInner / innerTotalD;
        var darkRatio = darkInner / innerTotalD;
        var grayDarkRatio = grayDarkInner / innerTotalD;
        var dimRatio = dimInner / innerTotalD;
        var warmRatio = warmInner / innerTotalD;
        var shadowRatio = shadowIcon / iconTotalD;
        var iconScore = 0.74 * (iconVMean / 255.0) + 0.26 * (iconSMean / 255.0);
        var readyScore =
            (innerVMean / 255.0) * 0.38
            + ringRatio * 0.28
            + brightRatio * 0.16
            + (iconVMean / 255.0) * 0.12
            + Math.Max(0.0, 1.0 - darkRatio) * 0.06;

        return new SkillSlotMetrics(
            InnerVMean: innerVMean,
            InnerSMean: innerSMean,
            IconVMean: iconVMean,
            IconSMean: iconSMean,
            RingRatio: ringRatio,
            BrightRatio: brightRatio,
            DarkRatio: darkRatio,
            GrayDarkRatio: grayDarkRatio,
            DimRatio: dimRatio,
            WarmRatio: warmRatio,
            ShadowRatio: shadowRatio,
            IconScore: iconScore,
            ReadyScore: readyScore);
    }

    /// <summary>
    /// Compare current frame against a baseline. Both inputs must already
    /// share <paramref name="width"/>×<paramref name="height"/> — Python
    /// auto-resizes via <c>cv2.resize(INTER_AREA)</c>; the C# port
    /// requires the caller to pre-resize so this stays dependency-free.
    /// Returns null if dimensions or buffers are invalid.
    /// </summary>
    public static SkillBaselineComparison? Compare(
        ReadOnlySpan<byte> bgr,
        ReadOnlySpan<byte> baselineBgr,
        int width,
        int height)
    {
        if (width < 8 || height < 8) return null;
        var need = width * height * 3;
        if (bgr.Length < need || baselineBgr.Length < need) return null;

        var curHsv = BarHsvDetector.MeanBlur3x3Bgr(bgr, width, height);
        BarHsvDetector.BgrToHsvInPlace(curHsv, width * height);
        var baseHsv = BarHsvDetector.MeanBlur3x3Bgr(baselineBgr, width, height);
        BarHsvDetector.BgrToHsvInPlace(baseHsv, width * height);

        if (!BuildMasks(width, height, out var iconMask, out var innerMask, out _))
            return null;

        long curIconV = 0, curIconS = 0, baseIconV = 0, baseIconS = 0;
        long curInnerV = 0, baseInnerV = 0;
        int iconTotal = 0, innerTotal = 0;
        int darkenedHits = 0, restoredHits = 0;

        for (var i = 0; i < width * height; i++)
        {
            var pi = i * 3;
            byte cv = curHsv[pi + 2], cs = curHsv[pi + 1];
            byte bv = baseHsv[pi + 2], bs = baseHsv[pi + 1];

            if (iconMask[i])
            {
                iconTotal++;
                curIconV += cv;
                curIconS += cs;
                baseIconV += bv;
                baseIconS += bs;
            }
            if (innerMask[i])
            {
                innerTotal++;
                curInnerV += cv;
                baseInnerV += bv;
                if (cv + 14.0 < bv) darkenedHits++;
                if (Math.Abs(cv - bv) <= 16.0) restoredHits++;
            }
        }
        if (iconTotal == 0 || innerTotal == 0) return null;

        var baseIconVMean = Math.Max(1.0, baseIconV / (double)iconTotal);
        var baseIconSMean = Math.Max(1.0, baseIconS / (double)iconTotal);
        var curIconVMean = curIconV / (double)iconTotal;
        var curIconSMean = curIconS / (double)iconTotal;
        var baseScore = 0.74 * (baseIconVMean / 255.0) + 0.26 * (baseIconSMean / 255.0);
        var curScore = 0.74 * (curIconVMean / 255.0) + 0.26 * (curIconSMean / 255.0);

        var darkenedRatio = darkenedHits / (double)innerTotal;
        var restoredRatio = restoredHits / (double)innerTotal;
        var avgDeltaV = (curInnerV - baseInnerV) / (double)innerTotal;

        return new SkillBaselineComparison(
            IconVRatio: curIconVMean / baseIconVMean,
            IconSRatio: curIconSMean / baseIconSMean,
            ScoreRatio: curScore / Math.Max(0.05, baseScore),
            DarkenedRatio: darkenedRatio,
            RestoredRatio: restoredRatio,
            AvgDeltaV: avgDeltaV);
    }

    /// <summary>
    /// Build the three concentric circle masks used by the slot detector.
    /// Returns false when geometry collapses (any mask becomes empty).
    /// Mirrors Python's <c>_slot_masks</c>:
    /// cx = w/2, cy = h*0.46, outer radius = max(6, min(w,h)*0.47),
    /// inner radius = max(4, min(w,h)*0.30).
    /// </summary>
    public static bool BuildMasks(
        int width,
        int height,
        out bool[] iconMask,
        out bool[] innerMask,
        out bool[] ringMask)
    {
        iconMask = new bool[width * height];
        innerMask = new bool[width * height];
        ringMask = new bool[width * height];
        if (width < 8 || height < 8) return false;

        var cx = width / 2.0;
        var cy = height * 0.46;
        var ro = Math.Max(6.0, Math.Min(width, height) * 0.47);
        var ri = Math.Max(4.0, Math.Min(width, height) * 0.30);
        var ro2 = ro * ro;
        var ri2 = ri * ri;

        int iconHits = 0, innerHits = 0, ringHits = 0;
        for (var y = 0; y < height; y++)
        {
            var dy = y - cy;
            var dy2 = dy * dy;
            for (var x = 0; x < width; x++)
            {
                var dx = x - cx;
                var d2 = dx * dx + dy2;
                var idx = y * width + x;
                var inIcon = d2 <= ro2;
                var inInner = d2 <= ri2;
                iconMask[idx] = inIcon;
                innerMask[idx] = inInner;
                var inRing = inIcon && !inInner;
                ringMask[idx] = inRing;
                if (inIcon) iconHits++;
                if (inInner) innerHits++;
                if (inRing) ringHits++;
            }
        }
        return iconHits > 0 && innerHits > 0 && ringHits > 0;
    }
}
