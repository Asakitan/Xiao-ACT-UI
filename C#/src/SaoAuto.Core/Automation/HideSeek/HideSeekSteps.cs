namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// S145 — HideSeek (躲猫猫) step shape. Pure data, mirrors the entry
/// shape of <c>hide_seek_engine.py</c>'s <c>STEPS</c> list (lines
/// 105–146): one ROI + colour filter set + template image + click
/// modifier per step in the sequence.
///
/// CV matcher and Win32 input live in later sessions; this is the
/// data-side first slice so the table can be referenced and tested
/// without dragging OpenCV or user32 into Core yet.
/// </summary>
public readonly record struct HideSeekStep(
    string Name,
    string ImageFile,
    HideSeekRoi Roi,
    IReadOnlyList<HideSeekColor> Colors,
    bool AltClick,
    bool UseSqDiff);

/// <summary>
/// Relative ROI on a 1920×1080 reference frame. All fields in [0,1].
/// Produced by <see cref="HideSeekRoi.FromBottomRight"/> to match
/// Python's <c>_br_to_rel</c> helper.
/// </summary>
public readonly record struct HideSeekRoi(double X, double Y, double Width, double Height)
{
    public const int ReferenceWidth = 1920;
    public const int ReferenceHeight = 1080;

    public static HideSeekRoi FromBottomRight(int brX, int brY, int width, int height)
        => new(
            (brX - width) / (double)ReferenceWidth,
            (brY - height) / (double)ReferenceHeight,
            width / (double)ReferenceWidth,
            height / (double)ReferenceHeight);
}

/// <summary>
/// BGR colour centre + tolerance used by HideSeek's colour-filter
/// pre-pass. Tolerance is the per-channel absolute distance allowed
/// during the mask build.
/// </summary>
public readonly record struct HideSeekColor(byte B, byte G, byte R, int Tolerance);

/// <summary>
/// Canonical step palette + sequence. Mirrors the module-level
/// <c>_WHITE</c> / <c>_DARK_GRAY</c> / <c>_LIGHT_GRAY</c> constants
/// and <c>STEPS</c> list from <c>hide_seek_engine.py</c>.
/// </summary>
public static class HideSeekSteps
{
    public static readonly HideSeekColor White = new(255, 255, 255, 35);
    public static readonly HideSeekColor DarkGray = new(50, 50, 50, 35);
    public static readonly HideSeekColor LightGray = new(200, 200, 200, 55);

    public const double MatchThreshold = 0.70;
    public const double SqDiffThreshold = 0.10;

    public static readonly IReadOnlyList<HideSeekStep> Default = new HideSeekStep[]
    {
        new(
            Name: "Accept",
            ImageFile: "1.png",
            Roi: HideSeekRoi.FromBottomRight(1446, 650, 35, 100),
            Colors: new[] { White },
            AltClick: true,
            UseSqDiff: false),
        new(
            Name: "Confirm-1",
            ImageFile: "2.png",
            Roi: HideSeekRoi.FromBottomRight(1893, 1020, 260, 75),
            Colors: new[] { DarkGray, LightGray },
            AltClick: false,
            UseSqDiff: false),
        new(
            Name: "Confirm-2",
            ImageFile: "3.png",
            Roi: HideSeekRoi.FromBottomRight(1869, 956, 310, 75),
            Colors: new[] { DarkGray, LightGray },
            AltClick: false,
            UseSqDiff: true),
        new(
            Name: "Confirm-3",
            ImageFile: "4.png",
            Roi: HideSeekRoi.FromBottomRight(1115, 1005, 310, 75),
            Colors: new[] { DarkGray, LightGray },
            AltClick: false,
            UseSqDiff: true),
        new(
            Name: "Confirm-4",
            ImageFile: "5.png",
            Roi: HideSeekRoi.FromBottomRight(1115, 1005, 310, 75),
            Colors: new[] { DarkGray, LightGray },
            AltClick: false,
            UseSqDiff: true),
    };
}
