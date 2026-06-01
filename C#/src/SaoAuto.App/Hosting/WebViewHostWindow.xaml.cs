using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
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

    /// <summary>
    /// Bug fix (user 2026-06-01, fourth pass): WebView click-through + black frame.
    ///
    /// History recap: pass-3 tried DwmExtendFrameIntoClientArea(MARGINS=-1)
    /// to apply the "sheet-of-glass" Aero effect — but that DWM technique
    /// only chroma-shows-through where the WPF window paints alpha=0
    /// pixels, and with AllowsTransparency=False the WPF backdrop is fully
    /// opaque. Result: a 1-2px DWM resize border surrounded the window
    /// (black-frame artifact) and the page still appeared off because of
    /// the half-transparent overlay tile.
    ///
    /// Correct fix mirrors the Python implementation at
    /// sao_webview.py:115-163 — apply WS_EX_LAYERED then
    /// SetLayeredWindowAttributes(hwnd, COLORREF=RGB(1,0,1),
    /// LWA_COLORKEY). The OS compositor chroma-keys every pixel of that
    /// exact color into a hole; everything else paints normally. Crucially
    /// we do NOT set WS_EX_TRANSPARENT, so mouse input still hits the
    /// WebView2 child HWND for clicks on visible UI. The XAML root uses
    /// Background=#FF010001 (the magic color) and inner Grid uses x:Null
    /// to keep WPF hit-test out of the way.
    ///
    /// We also strip WS_THICKFRAME|WS_CAPTION|WS_BORDER and OR-in WS_POPUP
    /// on GWL_STYLE, then SetWindowPos(SWP_FRAMECHANGED) to flush — this
    /// kills the residual 1-2px DWM resize frame that WindowStyle=None
    /// alone doesn't remove.
    ///
    /// EntityHostWindow keeps AllowsTransparency=True because it has no
    /// native child HWND — its pure-WPF HP-bar content needs per-pixel
    /// alpha for the anti-aliased glow.
    /// </summary>
    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        try
        {
            var hwnd = new WindowInteropHelper(this).Handle;
            if (hwnd == IntPtr.Zero) return;

            // 1) Apply color-key chroma-transparency via WS_EX_LAYERED + LWA_COLORKEY.
            //    COLORREF byte order is 0x00BBGGRR, so RGB(1,0,1) → 0x00010001.
            const int GWL_EXSTYLE = -20;
            const int GWL_STYLE = -16;
            const int WS_EX_LAYERED = 0x00080000;
            const uint LWA_COLORKEY = 0x00000001;
            const uint COLORREF_KEY = 0x00010001; // BGR of RGB(1, 0, 1)

            var ex = NativeMethods.GetWindowLong(hwnd, GWL_EXSTYLE);
            var newEx = ex | WS_EX_LAYERED;
            NativeMethods.SetWindowLong(hwnd, GWL_EXSTYLE, newEx);
            var keyOk = NativeMethods.SetLayeredWindowAttributes(hwnd, COLORREF_KEY, 0, LWA_COLORKEY);
            _log.LogInformation(
                "WebViewHost color-key applied hwnd={Hwnd} ex=0x{Ex:X8} key=0x{Key:X6} ok={Ok}",
                hwnd, newEx, COLORREF_KEY, keyOk);

            // 2) Strip residual chrome: drop WS_CAPTION|WS_THICKFRAME|WS_BORDER,
            //    OR-in WS_POPUP, then SWP_FRAMECHANGED so the DWM resizes the
            //    non-client area to zero. Without this, even WindowStyle=None
            //    leaves a 1-2px shadow/resize ring around the window.
            const uint WS_CAPTION = 0x00C00000;
            const uint WS_THICKFRAME = 0x00040000;
            const uint WS_BORDER = 0x00800000;
            const uint WS_POPUP = 0x80000000;
            const uint SWP_NOMOVE = 0x0002;
            const uint SWP_NOSIZE = 0x0001;
            const uint SWP_NOZORDER = 0x0004;
            const uint SWP_FRAMECHANGED = 0x0020;

            var style = (uint)NativeMethods.GetWindowLong(hwnd, GWL_STYLE);
            var newStyle = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER)) | WS_POPUP;
            NativeMethods.SetWindowLong(hwnd, GWL_STYLE, unchecked((int)newStyle));
            NativeMethods.SetWindowPos(hwnd, IntPtr.Zero, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "WebViewHost color-key / frame-strip failed; window will still show but may show chrome");
        }
    }

    private static class NativeMethods
    {
        [DllImport("user32.dll", SetLastError = true)]
        public static extern int GetWindowLong(IntPtr hWnd, int nIndex);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern int SetWindowLong(IntPtr hWnd, int nIndex, int dwNewLong);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetLayeredWindowAttributes(IntPtr hWnd, uint crKey, byte bAlpha, uint dwFlags);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
    }

    /// <summary>S196 — apply the Python-parity HUD geometry via the
    /// shared <see cref="HudGeometry"/> helper.</summary>
    public void ApplyHudGeometry(SaoAuto.Core.Configuration.SettingsManager? settings = null)
    {
        // WebView profile is the canonical 500 px HUD.
        HudGeometry.Apply(this, HudGeometry.Profile.WebView, settings, _log);
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
        // Bug fix (user 2026-06-01, fourth pass): the previous candidate list
        // only had 5/6-dotdot variants — the bin/Release/net8.0-windows/
        // win-x64/publish layout needs 4 or 7. Without bridge.js loading the
        // panel buttons look "dead" with zero log signal beyond the WebView2
        // navigation OK line, which is exactly what the user reported.
        var binDir = AppContext.BaseDirectory;
        string[] candidates =
        {
            Path.Combine(binDir, "web", fileName),
            Path.Combine(binDir, "..", "..", "..", "..", "web", fileName),
            Path.Combine(binDir, "..", "..", "..", "..", "..", "web", fileName),
            Path.Combine(binDir, "..", "..", "..", "..", "..", "..", "web", fileName),
            Path.Combine(binDir, "..", "..", "..", "..", "..", "..", "..", "web", fileName),
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
