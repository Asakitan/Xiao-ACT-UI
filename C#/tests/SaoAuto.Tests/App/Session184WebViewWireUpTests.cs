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
        var isolated = Path.Combine(_workDir, "isolated", "bin");
        Directory.CreateDirectory(isolated);

        var url = UiRunner.ResolveHudIndexUrl(NullLogger.Instance, isolated);

        Assert.Null(url);
    }

    [Fact]
    public void ResolveHudIndexUrl_UsesCSharpLocalWebOnly()
    {
        var bin = Path.Combine(_workDir, "standalone", "bin");
        var web = Path.Combine(bin, "web");
        Directory.CreateDirectory(web);
        File.WriteAllText(Path.Combine(web, "panel.html"), "<html></html>");

        var pythonWeb = Path.Combine(_workDir, "sao_auto", "web");
        Directory.CreateDirectory(pythonWeb);
        File.WriteAllText(Path.Combine(pythonWeb, "hp.html"), "<html>python</html>");

        var url = UiRunner.ResolveHudIndexUrl(NullLogger.Instance, bin);

        Assert.NotNull(url);
        Assert.EndsWith("panel.html", url, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("sao_auto", url!, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void AppProjectDoesNotReferencePythonRuntimeResources()
    {
        var project = LocateAppProjectFile();
        var text = File.ReadAllText(project);

        Assert.DoesNotContain("Include=\"..\\..\\..\\python", text, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("Include=\"../../../python", text, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Include=\"web\\**\\*.*\"", text, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Include=\"plugins\\**\\*.*\"", text, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Include=\"shaders\\**\\*.*\"", text, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Include=\"drivers\\**\\*.*\"", text, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void CSharpWebResourcesExposePython50BridgeShimContracts()
    {
        var project = LocateAppProjectFile();
        var appDir = Path.GetDirectoryName(project)!;
        var bridge = File.ReadAllText(Path.Combine(appDir, "web", "bridge.js"));
        var shim = File.ReadAllText(Path.Combine(appDir, "web", "pywebview-shim.js"));

        Assert.Contains("bridge.cmd", bridge, StringComparison.Ordinal);
        Assert.Contains("bridge.on", bridge, StringComparison.Ordinal);
        Assert.Contains("bridge.ready", bridge, StringComparison.Ordinal);
        Assert.Contains("type: 'command'", bridge, StringComparison.Ordinal);
        Assert.Contains("window.chrome.webview.addEventListener('message'", bridge, StringComparison.Ordinal);

        Assert.Contains("get_data_source_health", shim, StringComparison.Ordinal);
        Assert.Contains("act.sources.health", shim, StringComparison.Ordinal);
        Assert.Contains("get_plugin_status", shim, StringComparison.Ordinal);
        Assert.Contains("act.plugins.status", shim, StringComparison.Ordinal);
        Assert.Contains("get_mem_scope_status", shim, StringComparison.Ordinal);
        Assert.Contains("act.mem_scope.status", shim, StringComparison.Ordinal);
        Assert.Contains("trigger_export_presets", shim, StringComparison.Ordinal);
        Assert.Contains("act_trigger_export_presets", shim, StringComparison.Ordinal);
        Assert.Contains("license_status", shim, StringComparison.Ordinal);
        Assert.Contains("csharp-standalone-3", shim, StringComparison.Ordinal);
    }

    [Fact]
    public void CSharpStandaloneResourcesMirrorPythonPackagedDataDirectories()
    {
        var project = LocateAppProjectFile();
        var appDir = Path.GetDirectoryName(project)!;
        var repoRoot = Path.GetFullPath(Path.Combine(appDir, "..", "..", ".."));
        var pythonDir = Path.Combine(repoRoot, "python");

        AssertPackagedFilesExist(Path.Combine(pythonDir, "web"), Path.Combine(appDir, "web"));
        AssertPackagedFilesExist(Path.Combine(pythonDir, "plugins"), Path.Combine(appDir, "plugins"));
        AssertPackagedFilesExist(Path.Combine(pythonDir, "shaders"), Path.Combine(appDir, "shaders"));
        AssertPackagedFilesExist(Path.Combine(pythonDir, "drivers"), Path.Combine(appDir, "drivers"));

        Assert.True(File.Exists(Path.Combine(appDir, "web", "fonts", "SAOUI.ttf")));
        Assert.True(File.Exists(Path.Combine(appDir, "web", "fonts", "ZhuZiAYuanJWD.ttf")));
        Assert.True(File.Exists(Path.Combine(appDir, "drivers", "a.dat")));
        Assert.True(File.Exists(Path.Combine(appDir, "shaders", "skillfx.frag")));
        Assert.True(File.Exists(Path.Combine(appDir, "plugins", "star_resonance_plugin", "plugin.json")));
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

    private static string LocateAppProjectFile()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (var i = 0; i < 12 && dir is not null; i++, dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "src", "SaoAuto.App", "SaoAuto.App.csproj");
            if (File.Exists(candidate)) return candidate;
        }

        throw new FileNotFoundException("Could not locate SaoAuto.App.csproj from test output directory.");
    }

    private static void AssertPackagedFilesExist(string sourceDir, string targetDir)
    {
        Assert.True(Directory.Exists(sourceDir), $"Missing source directory: {sourceDir}");
        Assert.True(Directory.Exists(targetDir), $"Missing target directory: {targetDir}");

        var missing = Directory.EnumerateFiles(sourceDir, "*", SearchOption.AllDirectories)
            .Select(path => Path.GetRelativePath(sourceDir, path))
            .Where(rel => !File.Exists(Path.Combine(targetDir, rel)))
            .Order(StringComparer.OrdinalIgnoreCase)
            .Take(10)
            .ToArray();

        Assert.Empty(missing);
    }
}
