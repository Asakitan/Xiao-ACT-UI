using System.Windows;

namespace SaoAuto.App.Hosting;

/// <summary>
/// Contract for the WPF UI shells. Hosts only build their main window; the
/// runner owns the single <see cref="System.Windows.Application"/> and decides
/// when to enter <see cref="System.Windows.Application.Run(Window)"/>. This keeps
/// fallback chains cheap — a host that throws while building its window does not
/// taint the application loop.
/// </summary>
public interface IUiHost
{
    /// <summary>One of <c>entity</c> / <c>webview</c>.</summary>
    string ModeName { get; }

    /// <summary>
    /// Cheap, non-throwing availability check. Used by the router to skip
    /// hosts whose runtime prerequisite is missing (e.g. WebView2 runtime not installed).
    /// </summary>
    bool IsAvailable { get; }

    /// <summary>
    /// Construct the host's primary window. May throw on construction failure;
    /// <see cref="ModeRouter"/> will treat that as a signal to fall back to the
    /// next host in the chain, matching <c>main.run_ui</c>.
    /// </summary>
    Window CreateMainWindow();
}
