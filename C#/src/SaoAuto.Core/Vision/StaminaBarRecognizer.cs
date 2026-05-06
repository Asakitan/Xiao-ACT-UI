namespace SaoAuto.Core.Vision;

/// <summary>
/// Pixel-bar recognition helpers ported from <c>recognition.py</c>'s STA / HP
/// bar inspection. These are pure-math primitives over a captured frame —
/// they take a region of pixels and return the percentage of the bar that
/// passes the colour gate.
/// </summary>
public static class StaminaBarRecognizer
{
    /// <summary>
    /// Compute the fraction of horizontal samples whose pixel passes <paramref name="predicate"/>,
    /// scanning the centre row of <paramref name="region"/>. Mirrors Python's
    /// "scan one row, count matches" approach. Returns NaN if the region is empty.
    /// </summary>
    public static double SampleHorizontalFill(
        CapturedFrame frame,
        State.RectI region,
        Func<byte, byte, byte, bool> predicate)
    {
        if (region.W <= 0 || region.H <= 0) return double.NaN;
        var midY = region.Y + region.H / 2;
        var hits = 0;
        var samples = 0;
        for (var dx = 0; dx < region.W; dx++)
        {
            var x = region.X + dx;
            if (x < 0 || midY < 0) continue;
            var px = frame.GetPixel(x, midY);
            samples++;
            if (predicate(px.B, px.G, px.R)) hits++;
        }
        if (samples == 0) return double.NaN;
        return (double)hits / samples;
    }

    /// <summary>
    /// Stamina-acceptance rule from Python: percentages 0.91..0.97 must be
    /// returned UNCLAMPED at the low-level detector (so the acceptance stage
    /// can see they hover near full). Only 98%+ values get clamped to 1.0.
    /// </summary>
    public static double NormalizeStaminaPercent(double rawPercent)
    {
        if (double.IsNaN(rawPercent)) return 0.0;
        if (rawPercent < 0.0) return 0.0;
        if (rawPercent >= 0.98) return 1.0;
        return rawPercent;
    }
}
