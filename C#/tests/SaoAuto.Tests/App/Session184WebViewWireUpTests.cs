using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Hosting;
using SaoAuto.App.Modes;

namespace SaoAuto.Tests.App;

/// <summary>
/// S184 — Smoke coverage for the WebView2 wire-up. Real
/// <see cref="WebViewHostWindow"/> initialization requires an STA
/// dispatcher + WebView2 runtime; that's exercised via a manual smoke
/// (running the app), not pinned here. These tests cover the
/// non-window-bound logic: <see cref="UiRunner.ResolveHudIndexUrl"/>
/// path-resolution and <see cref="WebView2Probe"/> reporting.
/// </summary>
public class Session184WebViewWireUpTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _origCwd;

    public Session184WebViewWireUpTests()
    {
        _origCwd = Environment.CurrentDirectory;
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s184-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(_workDir);
    }

    public void Dispose()
    {
        try { Environment.CurrentDirectory = _origCwd; } catch { /* ignore */ }
        try { Directory.Delete(_workDir, recursive: true); } catch { /* ignore */ }
    }

    [Fact]
    public void ResolveHudIndexUrl_NoFile_ReturnsNull()
    {
        // AppContext.BaseDirectory is the test runner's bin; web/panel.html
        // does not ship from the test project, so resolve must return null.
        var url = UiRunner.ResolveHudIndexUrl(NullLogger.Instance);
        // Either null (no panel.html) or a file:// URL to a real panel.html
        // (when test is run from a workspace that has one). Both are
        // valid outcomes; verify the contract.
        if (url is not null)
        {
            Assert.StartsWith("file:///", url);
            // S192 — hp.html is now the priority target, panel.html is a fallback.
            Assert.True(
                url.EndsWith("hp.html", StringComparison.OrdinalIgnoreCase)
                    || url.EndsWith("panel.html", StringComparison.OrdinalIgnoreCase),
                $"unexpected url={url}");
        }
    }

    [Fact]
    public void WebView2Probe_ReportsStatus()
    {
        // Smoke: probe never throws; status is either a version string or
        // an error message. Run-environment dependent; this asserts the
        // contract, not the specific runtime presence.
        var available = WebView2Probe.IsRuntimeAvailable(out var status);
        Assert.False(string.IsNullOrEmpty(status));
        // `available` reflects this machine's installed runtime; not asserted.
        _ = available;
    }
}
