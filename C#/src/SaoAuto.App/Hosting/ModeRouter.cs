using SaoAuto.Core.Configuration;

namespace SaoAuto.App.Hosting;

/// <summary>
/// Pure decision module. Given a normalized requested UI mode, returns the host
/// chain to attempt in order. Mirrors <c>main.run_ui</c>:
/// <list type="bullet">
///   <item>requested <c>entity</c>: try entity, then webview. (No second entity attempt.)</item>
///   <item>requested <c>webview</c> (or any other value): try webview, then entity.</item>
/// </list>
/// Any final fallback to headless mode is the runner's responsibility, not the chain's.
/// </summary>
public static class ModeRouter
{
    public static IReadOnlyList<string> BuildChain(string? requestedMode)
    {
        var normalized = UiMode.Normalize(requestedMode);
        return normalized == UiMode.Entity
            ? new[] { UiMode.Entity, UiMode.WebView }
            : new[] { UiMode.WebView, UiMode.Entity };
    }
}
