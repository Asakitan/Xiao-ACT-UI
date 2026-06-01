using System.IO;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Extensions.Logging;
using Microsoft.Web.WebView2.Core;
using SaoAuto.Core.Logging;

namespace SaoAuto.App.Hosting;

/// <summary>
/// Bug fix (user 2026-06-01, fifth pass): SAOLinkStart via WebView2 + WebGL.
///
/// The splash hosts a WebView2 that loads <c>web/linkstart.html</c> — a 1:1
/// WebGL2 port of the Python <c>sao_theme.SAOLinkStart</c> moderngl tunnel.
/// This code-behind owns:
///   1. WebView2 init + navigation to linkstart.html (file:// URI resolved
///      from the same candidate ladder the HUD host uses).
///   2. A WebMessage listener: the page posts <c>linkstart_done</c> when the
///      9-second sequence finishes → we close.
///   3. A 10.5s safety timeout backstop so the splash can never hang the
///      host chain even if WebView2 fails to init or the page never posts.
///
/// No OpenTK / GLWpfControl dependency: removing it eliminated the
/// black-screen / crash failure mode and the per-GPU driver baggage.
/// </summary>
public partial class SaoSplashWindow : Window
{
    private static readonly ILogger _log = SaoLog.For("splash");
    private static readonly TimeSpan SafetyTimeout = TimeSpan.FromSeconds(10.5);

    private DispatcherTimer? _safetyTimer;
    private bool _closing;

    public SaoSplashWindow()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Closed += OnClosed;
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        // Safety net first: even if WebView2 init throws or the page never
        // signals completion, dismiss after 10.5s (9s sequence + buffer).
        _safetyTimer = new DispatcherTimer { Interval = SafetyTimeout };
        _safetyTimer.Tick += (_, _) =>
        {
            if (_closing) return;
            _log.LogWarning(
                "splash safety timeout ({Seconds:0.0}s) — linkstart_done never received; forcing close",
                SafetyTimeout.TotalSeconds);
            SafeClose();
        };
        _safetyTimer.Start();

        try
        {
            var url = ResolveLinkStartUrl();
            if (url is null)
            {
                _log.LogWarning("linkstart.html not found; skipping splash");
                SafeClose();
                return;
            }

            var userData = DefaultUserDataFolder();
            CoreWebView2Environment? env = null;
            if (!string.IsNullOrEmpty(userData))
            {
                Directory.CreateDirectory(userData);
                env = await CoreWebView2Environment.CreateAsync(userDataFolder: userData)
                    .ConfigureAwait(true);
            }
            await SplashWeb.EnsureCoreWebView2Async(env).ConfigureAwait(true);

            var core = SplashWeb.CoreWebView2;
            // The page signals completion via window.chrome.webview.postMessage.
            core.WebMessageReceived += OnWebMessage;
            // Hide WebView2 default context menu / dev affordances on the splash.
            core.Settings.AreDefaultContextMenusEnabled = false;
            core.Settings.IsZoomControlEnabled = false;
            core.Settings.AreBrowserAcceleratorKeysEnabled = false;

            _log.LogInformation("splash navigating to {Url}", url);
            core.Navigate(url);
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "splash WebView2 init failed; closing");
            SafeClose();
        }
    }

    private void OnWebMessage(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        string msg;
        try { msg = e.TryGetWebMessageAsString(); }
        catch { msg = string.Empty; }
        if (string.Equals(msg, "linkstart_done", StringComparison.Ordinal))
        {
            _log.LogInformation("splash received linkstart_done; closing");
            // Marshal to UI thread (WebMessageReceived already fires there,
            // but be defensive).
            Dispatcher.BeginInvoke(new Action(SafeClose));
        }
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        _closing = true;
        try { _safetyTimer?.Stop(); } catch { /* ignore */ }
        _safetyTimer = null;
        try
        {
            if (SplashWeb.CoreWebView2 is not null)
            {
                SplashWeb.CoreWebView2.WebMessageReceived -= OnWebMessage;
            }
            SplashWeb.Dispose();
        }
        catch { /* ignore */ }
    }

    /// <summary>
    /// Resolve <c>web/linkstart.html</c> to a file:// URL. Mirrors the
    /// candidate ladder used by <see cref="WebViewHostWindow"/> /
    /// <c>UiRunner.ResolveHudIndexUrl</c> so the splash works from both the
    /// published layout (bin/web) and the dev tree (../../../web).
    /// </summary>
    private static string? ResolveLinkStartUrl()
    {
        var binDir = AppContext.BaseDirectory;
        var candidates = new[]
        {
            Path.Combine(binDir, "web", "linkstart.html"),
            Path.Combine(binDir, "..", "..", "..", "web", "linkstart.html"),
            Path.Combine(binDir, "..", "..", "..", "..", "web", "linkstart.html"),
            Path.Combine(binDir, "..", "..", "..", "..", "..", "web", "linkstart.html"),
            Path.Combine(binDir, "..", "..", "..", "..", "..", "..", "web", "linkstart.html"),
        };
        foreach (var c in candidates)
        {
            try
            {
                var full = Path.GetFullPath(c);
                if (File.Exists(full))
                {
                    return new Uri(full).AbsoluteUri;
                }
            }
            catch { /* try next */ }
        }
        // Project-anchored upward walk: find any ancestor sao_auto/web/linkstart.html.
        try
        {
            var dir = new DirectoryInfo(binDir);
            for (int i = 0; i < 12 && dir is not null; i++, dir = dir.Parent)
            {
                var probe = Path.Combine(dir.FullName, "sao_auto", "web", "linkstart.html");
                if (File.Exists(probe)) return new Uri(Path.GetFullPath(probe)).AbsoluteUri;
                var probe2 = Path.Combine(dir.FullName, "web", "linkstart.html");
                if (File.Exists(probe2)) return new Uri(Path.GetFullPath(probe2)).AbsoluteUri;
            }
        }
        catch { /* ignore */ }
        return null;
    }

    private static string DefaultUserDataFolder()
    {
        try
        {
            var baseDir = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            return Path.Combine(baseDir, "SaoAuto", "WebView2");
        }
        catch
        {
            return string.Empty;
        }
    }

    private void SafeClose()
    {
        if (_closing) return;
        _closing = true;
        try
        {
            _safetyTimer?.Stop();
            _safetyTimer = null;
            Close();
        }
        catch (Exception ex)
        {
            _log.LogDebug(ex, "splash close threw; ignoring");
        }
    }
}
