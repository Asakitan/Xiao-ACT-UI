namespace SaoAuto.Core.Vision;

/// <summary>
/// Game-window discovery defaults ported from <c>config.GAME_WINDOW_KEYWORDS</c>
/// / <c>GAME_PROCESS_NAMES</c>. Override via <see cref="WindowLocator"/> ctor.
/// </summary>
public static class GameWindowConfig
{
    public static readonly IReadOnlyList<string> DefaultTitleKeywords = new[]
    {
        "Star",
        "星痕共鸣",
    };

    public static readonly IReadOnlyList<string> DefaultProcessNames = new[]
    {
        "star.exe",
    };

    /// <summary>Minimum client-area side length for a window to count as a game window.</summary>
    public const int MinClientSide = 200;
}
