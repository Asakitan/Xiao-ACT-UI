namespace SaoAuto.Core.Vision;

/// <summary>
/// In-memory / file-backed <see cref="IFrameCapture"/> for tests and parity
/// replay. Reads a sequence of pre-captured frames; advances by one on each
/// <see cref="Capture"/> call, returning <c>null</c> after the sequence is
/// exhausted (mirrors a window-gone state).
/// </summary>
public sealed class FixtureFrameCapture : IFrameCapture
{
    private readonly IReadOnlyList<CapturedFrame> _frames;
    private int _index;

    public FixtureFrameCapture(IEnumerable<CapturedFrame> frames)
    {
        _frames = frames?.ToArray() ?? throw new ArgumentNullException(nameof(frames));
    }

    public static FixtureFrameCapture Single(CapturedFrame frame) => new(new[] { frame });

    public CapturedFrame? Capture()
    {
        if (_index >= _frames.Count) return null;
        return _frames[_index++];
    }

    public int Remaining => Math.Max(0, _frames.Count - _index);

    public void Dispose() { /* no unmanaged state */ }
}
