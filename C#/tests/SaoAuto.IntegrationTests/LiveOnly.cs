namespace SaoAuto.IntegrationTests;

/// <summary>
/// Marker constants for end-to-end smokes that require a live host
/// resource (Npcap driver, GDI desktop, WebView2 runtime, etc.).
/// Tests guarded by <see cref="LiveOnlyReason"/> are committed as
/// `[Fact(Skip = LiveOnly.Reason)]` so the suite documents the gap
/// without breaking CI on machines that lack the dependency.
/// </summary>
internal static class LiveOnly
{
    public const string Reason =
        "Live-only smoke: requires Npcap / desktop session / WebView2 / Win32 hotkey loop. " +
        "Run manually after the matching live wire-up session lands.";
}
