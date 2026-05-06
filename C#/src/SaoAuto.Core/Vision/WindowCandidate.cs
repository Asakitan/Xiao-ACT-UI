namespace SaoAuto.Core.Vision;

/// <summary>
/// Candidate window seen by <see cref="IWindowEnumerator"/>. Coordinates are
/// the screen-space client-area rectangle (after <c>GetClientRect</c> +
/// <c>ClientToScreen</c>), matching Python's <c>_get_client_rect_screen</c>.
/// </summary>
public readonly record struct WindowCandidate(
    IntPtr Hwnd,
    string Title,
    string? ProcessName,
    int Left,
    int Top,
    int Right,
    int Bottom)
{
    public int Width => Right - Left;
    public int Height => Bottom - Top;

    public bool LooksLikeGameWindow =>
        Width >= GameWindowConfig.MinClientSide && Height >= GameWindowConfig.MinClientSide;

    public override string ToString() =>
        $"\"{Title}\" [{ProcessName ?? "?"}] {Left},{Top}->{Right},{Bottom} ({Width}x{Height})";
}

public interface IWindowEnumerator
{
    IEnumerable<WindowCandidate> Enumerate();
    bool IsAlive(IntPtr hwnd);
    WindowCandidate? Probe(IntPtr hwnd);
}
