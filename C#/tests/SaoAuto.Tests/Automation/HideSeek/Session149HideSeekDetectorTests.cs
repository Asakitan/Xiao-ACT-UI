using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.Automation.HideSeek;

/// <summary>
/// S149 — Smoke tests for <see cref="HideSeekDetector.TryDetect"/>
/// end-to-end (ROI crop → mask → grayscale → match → screen-space
/// click). The CV components have their own tests; these only pin the
/// glue and the screen-space coordinate translation.
/// </summary>
public class Session149HideSeekDetectorTests
{
    private static HideSeekFrame MakeBgrFrame(int w, int h, int clientLeft = 0, int clientTop = 0)
        => new(new byte[w * h * 3], w, h, w * 3, 3, clientLeft, clientTop);

    [Fact]
    public void EmptyFrameReturnsNull()
    {
        var step = HideSeekSteps.Default[2]; // Confirm-2 (sqdiff enabled)
        var frame = MakeBgrFrame(800, 600);
        var tpl = new HideSeekTemplate("3.png", new byte[] { 50 }, 1, 1);

        Assert.Null(HideSeekDetector.TryDetect(step, frame, tpl));
    }

    [Fact]
    public void DetectsConfirm2PatchViaSqDiffFallback()
    {
        // Confirm-2 ROI on an 800×600 frame:
        //   roiX = int(0.812 * 800) = 649
        //   roiY = int(0.816 * 600) = 489
        //   roiW = int(0.161 * 800) = 128
        //   roiH = int(0.069 * 600) = 41
        var step = HideSeekSteps.Default[2];
        var frame = MakeBgrFrame(800, 600, clientLeft: 100, clientTop: 50);

        // Paint a single dark-gray pixel at offset (5,3) within the ROI.
        int roiAbsX = 649 + 5;
        int roiAbsY = 489 + 3;
        int p = (roiAbsY * 800 + roiAbsX) * 3;
        frame.Pixels[p] = 50;
        frame.Pixels[p + 1] = 50;
        frame.Pixels[p + 2] = 50;

        var tpl = new HideSeekTemplate("3.png", new byte[] { 50 }, 1, 1);
        var hit = HideSeekDetector.TryDetect(step, frame, tpl);

        Assert.NotNull(hit);
        // Screen-space click = clientLeft + roiX + match.X + tw/2
        //                    = 100 + 649 + 5 + 0 = 754
        Assert.Equal(754, hit!.Value.ClickX);
        Assert.Equal(50 + 489 + 3 + 0, hit!.Value.ClickY);
        Assert.StartsWith("sqdiff", hit!.Value.Method);
    }

    [Fact]
    public void ColorOutsideAllStepMasksReturnsNullWhenSqDiffDisabled()
    {
        // Confirm-1 (step 1) — sqdiff disabled. Paint a red pixel that
        // matches neither dark- nor light-gray; NCC also fails on a
        // 1×1 constant template, so the detector reports nothing.
        var step = HideSeekSteps.Default[1];
        var frame = MakeBgrFrame(800, 600);
        int roiX = (int)(step.Roi.X * 800);
        int roiY = (int)(step.Roi.Y * 600);
        int p = ((roiY + 2) * 800 + roiX + 2) * 3;
        frame.Pixels[p] = 0; frame.Pixels[p + 1] = 0; frame.Pixels[p + 2] = 255; // pure red

        var tpl = new HideSeekTemplate("2.png", new byte[] { 50 }, 1, 1);
        Assert.Null(HideSeekDetector.TryDetect(step, frame, tpl));
    }

    [Fact]
    public void OversizeTemplateIsResizedAndStillSearched()
    {
        // Hand a template that's larger than the ROI; ResizeTemplateIfTooLarge
        // shrinks it. With a constant template the matcher won't find a hit,
        // so detector returns null — but the call must not throw.
        var step = HideSeekSteps.Default[1];
        var frame = MakeBgrFrame(800, 600);
        var big = new byte[1000 * 1000];
        var tpl = new HideSeekTemplate("2.png", big, 1000, 1000);

        Assert.Null(HideSeekDetector.TryDetect(step, frame, tpl));
    }
}
