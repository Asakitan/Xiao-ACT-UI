namespace SaoAuto.Core.Vision;

/// <summary>
/// Source of game-window screenshots. Implementations include
/// <see cref="FixtureFrameCapture"/> (file-backed, used by tests) and
/// the live GDI / WGC sources (Session 6b live wiring).
/// </summary>
public interface IFrameCapture : IDisposable
{
    /// <summary>
    /// Capture one frame. Returns <c>null</c> when the source is offline
    /// (e.g. window minimized, capture surface lost). Mirrors Python's
    /// "missing capture" path that propagates to <see cref="State.GameState.RecognitionOk"/> = false.
    /// </summary>
    CapturedFrame? Capture();
}

/// <summary>
/// Top-down BGRA32 pixel buffer. Width/Height are in pixels; <c>Stride</c> is
/// the byte distance between successive rows (allows for padded buffers).
/// </summary>
public sealed record CapturedFrame(int Width, int Height, int Stride, byte[] Pixels)
{
    public int ByteSize => Stride * Height;

    /// <summary>
    /// Read a BGRA32 pixel at logical (x, y). Returns <c>(0, 0, 0, 0)</c> when
    /// out of bounds — matches the Python "skip if outside ROI" behavior.
    /// </summary>
    public (byte B, byte G, byte R, byte A) GetPixel(int x, int y)
    {
        if ((uint)x >= (uint)Width || (uint)y >= (uint)Height) return default;
        var off = y * Stride + x * 4;
        return (Pixels[off], Pixels[off + 1], Pixels[off + 2], Pixels[off + 3]);
    }
}
