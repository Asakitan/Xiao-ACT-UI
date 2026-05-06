using SaoAuto.Core.State;

namespace SaoAuto.Core.Vision;

/// <summary>
/// Percentage region-of-interest. Mirrors Python's <c>{'x','y','w','h'}</c>
/// dict in <c>config.DEFAULT_ROI</c>. All four fields are normalized to
/// <c>[0.0, 1.0]</c> against the client-area width/height.
/// </summary>
public readonly record struct Roi(double X, double Y, double W, double H)
{
    public RectI ToPixels(int clientWidth, int clientHeight)
    {
        var x = (int)Math.Round(X * clientWidth);
        var y = (int)Math.Round(Y * clientHeight);
        var w = (int)Math.Round(W * clientWidth);
        var h = (int)Math.Round(H * clientHeight);
        return new RectI(x, y, w, h);
    }

    public RectI ToScreenPixels(WindowCandidate window)
    {
        var pixels = ToPixels(window.Width, window.Height);
        return new RectI(window.Left + pixels.X, window.Top + pixels.Y, pixels.W, pixels.H);
    }
}
