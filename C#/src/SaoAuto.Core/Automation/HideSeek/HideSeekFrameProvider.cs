using SaoAuto.Core.Vision;

namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// S152 — Adapter from the existing <see cref="IFrameCapture"/> +
/// <see cref="WindowLocator"/> pair (used by recognition) to the
/// <see cref="IHideSeekFrameProvider"/> contract. Returns null when
/// the game window can't be located or the capture surface is offline,
/// which the state machine treats as "waiting". The reuse keeps
/// HideSeek and recognition on the *same* screen-grab pipeline —
/// no need for a second GDI / DXGI seam.
/// </summary>
public sealed class HideSeekFrameProvider : IHideSeekFrameProvider
{
    private readonly IFrameCapture _capture;
    private readonly WindowLocator _locator;

    public HideSeekFrameProvider(IFrameCapture capture, WindowLocator locator)
    {
        _capture = capture ?? throw new ArgumentNullException(nameof(capture));
        _locator = locator ?? throw new ArgumentNullException(nameof(locator));
    }

    public HideSeekFrame? Capture()
    {
        var win = _locator.FindGameWindow();
        if (win is null) return null;
        var frame = _capture.Capture();
        if (frame is null) return null;
        // CapturedFrame is BGRA32 (channels = 4). HideSeekFrame just
        // forwards the buffer; the detector knows to honour the
        // channel count when building masks / grayscale.
        return new HideSeekFrame(
            Pixels: frame.Pixels,
            Width: frame.Width,
            Height: frame.Height,
            Stride: frame.Stride,
            Channels: 4,
            ClientLeft: win.Value.Left,
            ClientTop: win.Value.Top);
    }
}
