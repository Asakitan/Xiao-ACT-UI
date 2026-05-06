namespace SaoAuto.Overlay.SkillFx;

/// <summary>
/// SDF kernels ported from <c>_sao_cy_skillfx.pyx</c>. The CPU fallback for
/// the burst-ready ring + beam + glow when the GPU SDF shader is unavailable.
/// Live GPU path lives in <c>SaoAuto.Overlay/SkillFx/SkillFxOverlayRenderer.cs</c>
/// (Session 21).
/// </summary>
public static class CySkillFx
{
    /// <summary>Signed distance to a 2D point relative to a circle of <paramref name="radius"/>.</summary>
    public static double CircleSdf(double dx, double dy, double radius) =>
        Math.Sqrt(dx * dx + dy * dy) - radius;

    /// <summary>
    /// Ring SDF: positive outside the band, 0 on the ring centerline,
    /// negative inside. Half-thickness = <paramref name="halfWidth"/>.
    /// </summary>
    public static double RingSdf(double dx, double dy, double radius, double halfWidth) =>
        Math.Abs(CircleSdf(dx, dy, radius)) - halfWidth;

    /// <summary>
    /// Smooth-step gate (matches Python `_smoothstep`): linear blend that
    /// curves at endpoints. <paramref name="edge0"/> &lt; <paramref name="edge1"/>.
    /// </summary>
    public static double SmoothStep(double edge0, double edge1, double x)
    {
        if (edge0 >= edge1) return x < edge0 ? 0 : 1;
        var t = Math.Clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
        return t * t * (3 - 2 * t);
    }

    /// <summary>
    /// Compute the alpha for a pixel in the burst-ready ring at radius
    /// <paramref name="r"/>. Mirrors <c>ring_alpha</c>: smooth ramp
    /// across the ring's edges with a soft anti-alias band.
    /// </summary>
    public static double RingAlpha(double dx, double dy, double radius, double halfWidth, double aaPixels = 1.0)
    {
        var d = RingSdf(dx, dy, radius, halfWidth);
        // Inside the band → 1, outside by aaPixels → 0
        return 1.0 - SmoothStep(0, aaPixels, d);
    }

    /// <summary>
    /// Beam alpha along a vertical bar: full inside the bar, smooth fall-off
    /// at the bar edges. <paramref name="dx"/> is signed distance from the
    /// beam centerline.
    /// </summary>
    public static double BeamAlpha(double dx, double halfWidth, double aaPixels = 1.0)
    {
        var d = Math.Abs(dx) - halfWidth;
        return 1.0 - SmoothStep(0, aaPixels, d);
    }

    /// <summary>
    /// Glow alpha: gaussian-shape decay outward from the ring centerline.
    /// </summary>
    public static double GlowAlpha(double dx, double dy, double radius, double sigma)
    {
        var d = CircleSdf(dx, dy, radius);
        return Math.Exp(-(d * d) / (2 * sigma * sigma));
    }
}
