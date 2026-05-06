namespace SaoAuto.Overlay.Rendering;

/// <summary>
/// Top-down premultiplied BGRA32 buffer fed to <c>UpdateLayeredWindow</c>.
/// Width/Height are in pixels, <see cref="Stride"/> is the byte distance between
/// rows. <see cref="ContentVersion"/> increments whenever the renderer mutates
/// pixel content; presenters use it to skip redundant uploads.
/// </summary>
public sealed class FrameBuffer
{
    public int Width { get; }
    public int Height { get; }
    public int Stride { get; }
    public byte[] Pixels { get; }
    public long ContentVersion { get; private set; }

    public int OffsetX { get; private set; }
    public int OffsetY { get; private set; }

    public FrameBuffer(int width, int height)
    {
        if (width <= 0 || height <= 0) throw new ArgumentOutOfRangeException();
        Width = width;
        Height = height;
        Stride = width * 4;
        Pixels = new byte[Stride * height];
    }

    public void BumpContentVersion() => ContentVersion++;

    public void SetOffset(int offsetX, int offsetY)
    {
        OffsetX = offsetX;
        OffsetY = offsetY;
    }

    public Span<byte> AsSpan() => Pixels;
    public Span<byte> Row(int y) => Pixels.AsSpan(y * Stride, Stride);
}
