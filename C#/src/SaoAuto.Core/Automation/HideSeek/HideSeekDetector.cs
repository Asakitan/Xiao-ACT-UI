namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// S149 — One step's detection pipeline. Mirrors the per-step body
/// of Python's <c>_try_detect_step</c> but drops the multi-scale +
/// debug-save fallbacks. Pure: frame in, optional screen-space click
/// position out.
///
/// Pipeline:
///   1. Crop ROI from the frame using <see cref="HideSeekStep.Roi"/>.
///   2. Build a colour mask via <see cref="HideSeekColorMask.Build"/>.
///   3. Convert ROI to grayscale + zero out non-mask pixels.
///   4. Convert template to grayscale + apply its own colour mask.
///   5. Match (NCC; fallback to SQDIFF when <c>step.UseSqDiff</c>).
///   6. If a hit, return screen-space click coordinates pointing at
///      the centre of the matched template.
/// </summary>
public static class HideSeekDetector
{
    public readonly record struct DetectResult(int ClickX, int ClickY, double Confidence, string Method);

    public static DetectResult? TryDetect(
        HideSeekStep step,
        HideSeekFrame frame,
        HideSeekTemplate template)
    {
        ArgumentNullException.ThrowIfNull(frame);
        ArgumentNullException.ThrowIfNull(template);

        // ROI math: relative coords against the captured client area.
        int roiX = Math.Max(0, (int)(step.Roi.X * frame.Width));
        int roiY = Math.Max(0, (int)(step.Roi.Y * frame.Height));
        int roiW = Math.Max(1, (int)(step.Roi.Width * frame.Width));
        int roiH = Math.Max(1, (int)(step.Roi.Height * frame.Height));
        int roiX2 = Math.Min(frame.Width, roiX + roiW);
        int roiY2 = Math.Min(frame.Height, roiY + roiH);
        roiW = roiX2 - roiX;
        roiH = roiY2 - roiY;
        if (roiW <= 0 || roiH <= 0) return null;

        // Crop ROI bytes into a tight buffer.
        var roiPixels = CropTight(frame.Pixels, frame.Stride, frame.Channels, roiX, roiY, roiW, roiH);

        // Build masks.
        var roiMask = HideSeekColorMask.Build(roiPixels, roiW, roiH, roiW * frame.Channels, frame.Channels, step.Colors);
        var roiGray = HideSeekTemplateMatcher.ToGrayscale(roiPixels, roiW, roiH, roiW * frame.Channels, frame.Channels);
        ApplyMask(roiGray, roiMask);

        // Template may need to be resized down to fit ROI.
        var sized = HideSeekTemplateMatcher.ResizeTemplateIfTooLarge(
            template.Grayscale, template.Width, template.Height, roiW, roiH);
        if (sized is null) return null;
        var (tplGray, tplW, tplH) = sized.Value;

        // The template colour mask is built from the *grayscale* template,
        // not the original BGR — the engine has historically only stored
        // grayscale templates in this port. Apply a degenerate mask by
        // skipping the bitwise-and on the template side; the matcher's
        // statistics still work because the NCC formula is mean-centred.

        var primary = HideSeekTemplateMatcher.MatchNcc(
            roiGray, roiW, roiH, tplGray, tplW, tplH, HideSeekSteps.MatchThreshold);
        string method = "ncc";
        var match = primary;

        if (!primary.Found)
        {
            // Raw NCC against unmasked ROI grayscale — recovers when the
            // mask is too aggressive (text vs. background contrast).
            var rawGray = HideSeekTemplateMatcher.ToGrayscale(
                roiPixels, roiW, roiH, roiW * frame.Channels, frame.Channels);
            var raw = HideSeekTemplateMatcher.MatchNcc(
                rawGray, roiW, roiH, tplGray, tplW, tplH, HideSeekSteps.MatchThreshold);
            if (raw.Found)
            {
                match = raw;
                method = "raw";
            }
            else if (step.UseSqDiff)
            {
                var sq = HideSeekTemplateMatcher.MatchSqDiff(
                    rawGray, roiW, roiH, tplGray, tplW, tplH, HideSeekSteps.SqDiffThreshold);
                if (sq.Found)
                {
                    match = new HideSeekTemplateMatcher.MatchResult(sq.X, sq.Y, 1.0 - sq.Score, true);
                    method = $"sqdiff({sq.Score:F4})";
                }
            }
        }

        if (!match.Found) return null;

        int clickX = frame.ClientLeft + roiX + match.X + tplW / 2;
        int clickY = frame.ClientTop + roiY + match.Y + tplH / 2;
        return new DetectResult(clickX, clickY, match.Score, method);
    }

    private static byte[] CropTight(byte[] src, int srcStride, int channels, int x, int y, int w, int h)
    {
        var dst = new byte[w * h * channels];
        int srcStart = y * srcStride + x * channels;
        int dstStride = w * channels;
        for (int row = 0; row < h; row++)
            Buffer.BlockCopy(src, srcStart + row * srcStride, dst, row * dstStride, dstStride);
        return dst;
    }

    private static void ApplyMask(byte[] gray, byte[] mask)
    {
        // gray and mask are same width×height — zero gray where mask is 0.
        for (int i = 0; i < gray.Length; i++)
            if (mask[i] == 0) gray[i] = 0;
    }
}
