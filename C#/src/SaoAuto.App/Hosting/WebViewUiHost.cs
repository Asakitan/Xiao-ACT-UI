using System.Windows;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.Hosting;

public sealed class WebViewUiHost : IUiHost
{
    public string ModeName => UiMode.WebView;

    public bool IsAvailable => WebView2Probe.IsRuntimeAvailable();

    public Window CreateMainWindow()
    {
        WebView2Probe.IsRuntimeAvailable(out var status);
        return new WebViewHostWindow($"WebView2 runtime: {status}");
    }
}
