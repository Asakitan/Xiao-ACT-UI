using System.IO;
using System.Windows;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Web.WebView2.Core;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Logging;

namespace SaoAuto.App.Hosting;

public partial class WebViewHostWindow : Window
{
    private static readonly ILogger _log = SaoLog.For("webview");

    private CoreWebView2MessageBus? _bus;
    private bool _coreInitialised;

    public WebViewHostWindow(string? runtimeStatus = null)
    {
        InitializeComponent();
        if (!string.IsNullOrEmpty(runtimeStatus))
        {
            StatusText.Text = runtimeStatus;
        }
    }

    /// <summary>S196 — apply the Python-parity HUD geometry via the
    /// shared <see cref="HudGeometry"/> helper.</summary>
    public void ApplyHudGeometry(SaoAuto.Core.Configuration.SettingsManager? settings = null)
    {
        HudGeometry.Apply(this, settings, _log);
    }

    /// <summary>S184 — exposed after <see cref="EnsureWebViewAsync"/>
    /// completes successfully. The host wires this to a
    /// <see cref="BridgeHostAdapter"/> via
    /// <see cref="WebViewBridgeBinder.Bind"/>.</summary>
    public IWebMessageBus? MessageBus => _bus;

    /// <summary>Initialise the embedded <c>CoreWebView2</c> on the UI
    /// thread, navigate to <paramref name="startUrl"/> (or
    /// <c>about:blank</c> when null), and expose
    /// <see cref="MessageBus"/>. Idempotent.</summary>
    public async Task EnsureWebViewAsync(string? startUrl = null, string? userDataFolder = null)
    {
        if (_coreInitialised) return;
        try
        {
            // Use a per-user data folder so the cache survives between runs
            // without polluting the system-default LocalAppData layout.
            var folder = userDataFolder ?? DefaultUserDataFolder();
            CoreWebView2Environment? env = null;
            if (!string.IsNullOrEmpty(folder))
            {
                Directory.CreateDirectory(folder);
                env = await CoreWebView2Environment.CreateAsync(userDataFolder: folder).ConfigureAwait(true);
            }
            await WebView.EnsureCoreWebView2Async(env).ConfigureAwait(true);
            _bus = new CoreWebView2MessageBus(WebView.CoreWebView2, Dispatcher);
            _coreInitialised = true;
            StatusOverlay.Visibility = Visibility.Collapsed;
            WebView.CoreWebView2.NavigationCompleted += OnNavigationCompleted;
            // S187 — inject `window.bridge` shim before the page script runs.
            // The shim lives in web/bridge.js next to panel.html; if missing
            // (e.g. about:blank fallback), the shim slot is just absent and
            // pages running outside WebView2 are unaffected.
            await TryInjectBridgeScriptAsync().ConfigureAwait(true);
            var target = startUrl ?? "about:blank";
            _log.LogInformation("WebView2 navigating to {Url}", target);
            WebView.CoreWebView2.Navigate(target);
        }
        catch (Exception ex)
        {
            StatusText.Text = $"WebView2 init failed: {ex.Message}";
            throw;
        }
    }

    private async Task TryInjectBridgeScriptAsync()
    {
        // bridge.js must inject first (defines window.bridge); then
        // the shims/subscribers which all need window.bridge.
        // AddScriptToExecuteOnDocumentCreatedAsync runs in registration
        // order, before the page's inline scripts.
        await InjectScriptByNameAsync("bridge.js").ConfigureAwait(true);
        await InjectScriptByNameAsync("pywebview-shim.js").ConfigureAwait(true);
        await InjectScriptByNameAsync("hud-bootstrap.js").ConfigureAwait(true);
    }

    private async Task InjectScriptByNameAsync(string fileName)
    {
        var path = ResolveWebAssetPath(fileName);
        if (path is null)
        {
            _log.LogWarning("{File} not found; injection skipped", fileName);
            return;
        }
        try
        {
            var script = await File.ReadAllTextAsync(path).ConfigureAwait(true);
            await WebView.CoreWebView2.AddScriptToExecuteOnDocumentCreatedAsync(script)
                .ConfigureAwait(true);
            _log.LogInformation("{File} injected ({Bytes} bytes)", fileName, script.Length);
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "{File} injection failed; continuing", fileName);
        }
    }

    private static string? ResolveWebAssetPath(string fileName)
    {
        var binDir = AppContext.BaseDirectory;
        string[] candidates =
        {
            Path.Combine(binDir, "web", fileName),
            Path.Combine(binDir, "..", "..", "..", "..", "..", "..", "web", fileName),
            Path.Combine(binDir, "..", "..", "..", "..", "..", "web", fileName),
        };
        foreach (var c in candidates)
        {
            try
            {
                var full = Path.GetFullPath(c);
                if (File.Exists(full)) return full;
            }
            catch { /* ignore */ }
        }
        return null;
    }

    private async void OnNavigationCompleted(object? sender, CoreWebView2NavigationCompletedEventArgs e)
    {
        try
        {
            if (!e.IsSuccess)
            {
                _log.LogWarning("WebView2 navigation failed: status={Status}", e.WebErrorStatus);
                return;
            }
            // Smoke probe: read document title + body byte length so the
            // log proves the page actually parsed (rather than just
            // 'navigation completed' which is true for blank pages too).
            var title = await WebView.CoreWebView2.ExecuteScriptAsync(
                "document.title").ConfigureAwait(true);
            var bodyLen = await WebView.CoreWebView2.ExecuteScriptAsync(
                "document.body && document.body.innerHTML.length").ConfigureAwait(true);
            var bridgeVer = await WebView.CoreWebView2.ExecuteScriptAsync(
                "(window.bridge && window.bridge.version) || null").ConfigureAwait(true);
            var hudVer = await WebView.CoreWebView2.ExecuteScriptAsync(
                "(window.__hudBootstrap && window.__hudBootstrap.version) || null").ConfigureAwait(true);
            var pyShim = await WebView.CoreWebView2.ExecuteScriptAsync(
                "(window.__pywebviewShim && window.__pywebviewShim.version) || null").ConfigureAwait(true);
            _log.LogInformation("WebView2 navigation ok: title={Title} body-bytes={Bytes} bridge={Bridge} hud={Hud} pyshim={Pyshim}",
                title, bodyLen, bridgeVer, hudVer, pyShim);

            // S188 — round-trip smoke: kick off a `bridge.cmd` from JS,
            // stash the result on a window slot, then poll-read it from
            // C# after a brief delay. (ExecuteScriptAsync does NOT await
            // a Promise — it JSON-stringifies the script's last
            // expression, which for `async () => …` is a Promise that
            // serialises to `{}`. Going via a window slot keeps the
            // synchronous serialisation but defers the read until after
            // the reply has been delivered.)
            const string kickoffScript =
                "(function(){\n" +
                "  window.__bridge_smoke__ = { state: 'pending' };\n" +
                "  if (!window.bridge) { window.__bridge_smoke__ = { state: 'no_bridge' }; return; }\n" +
                "  window.bridge.cmd('recognition.status', {}).then(\n" +
                "    function(r){ window.__bridge_smoke__ = { state: 'ok', reply: r }; },\n" +
                "    function(e){ window.__bridge_smoke__ = { state: 'err', error: String(e) }; }\n" +
                "  );\n" +
                "})()";
            await WebView.CoreWebView2.ExecuteScriptAsync(kickoffScript).ConfigureAwait(true);
            // Brief poll-read: typical reply is sub-millisecond, but
            // give the dispatcher a few ticks to deliver.
            await Task.Delay(150).ConfigureAwait(true);
            var smokeReply = await WebView.CoreWebView2.ExecuteScriptAsync(
                "JSON.stringify(window.__bridge_smoke__)").ConfigureAwait(true);
            _log.LogInformation("bridge round-trip smoke: recognition.status reply={Reply}", smokeReply);
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "WebView2 NavigationCompleted probe failed");
        }
    }

    protected override void OnClosed(EventArgs e)
    {
        if (_coreInitialised && WebView.CoreWebView2 is not null)
        {
            WebView.CoreWebView2.NavigationCompleted -= OnNavigationCompleted;
        }
        _bus?.Dispose();
        _bus = null;
        base.OnClosed(e);
    }

    private static string DefaultUserDataFolder()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        return string.IsNullOrEmpty(local)
            ? Path.Combine(Path.GetTempPath(), "SaoAuto", "WebView2")
            : Path.Combine(local, "SaoAuto", "WebView2");
    }
}
