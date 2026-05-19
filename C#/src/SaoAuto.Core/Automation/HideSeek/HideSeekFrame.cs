namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// One captured frame of the game's client area, in BGR or BGRA.
/// <see cref="ClientLeft"/> / <see cref="ClientTop"/> are screen-space
/// offsets of the client area's top-left corner; they exist so the
/// state machine can convert ROI-relative match positions back to
/// absolute screen coordinates for the click layer.
/// </summary>
public sealed record HideSeekFrame(
    byte[] Pixels,
    int Width,
    int Height,
    int Stride,
    int Channels,
    int ClientLeft,
    int ClientTop);

/// <summary>
/// Capture source plugged into the state machine. Returns null when
/// the game window is missing or capture failed — the engine treats
/// that as "waiting" and retries next tick.
/// </summary>
public interface IHideSeekFrameProvider
{
    HideSeekFrame? Capture();
}

/// <summary>
/// One template image preconverted to grayscale + dimensions.
/// Loading is the caller's job (asset file vs. user override is an
/// app-layer concern); the engine only needs the byte buffer.
/// </summary>
public sealed record HideSeekTemplate(string Name, byte[] Grayscale, int Width, int Height);
