using Microsoft.Web.WebView2.Core;

namespace SaoAuto.App.Hosting;

/// <summary>
/// Mirrors Python <c>sao_webview.is_webview_available</c>: cheap, non-throwing
/// "is the runtime installed?" probe so the mode router can fall back to entity
/// without touching a real WebView2 control.
/// </summary>
public static class WebView2Probe
{
    public static bool IsRuntimeAvailable(out string? versionOrError)
    {
        try
        {
            var version = CoreWebView2Environment.GetAvailableBrowserVersionString();
            if (string.IsNullOrEmpty(version))
            {
                versionOrError = "WebView2 runtime not detected";
                return false;
            }
            versionOrError = version;
            return true;
        }
        catch (Exception ex)
        {
            versionOrError = ex.Message;
            return false;
        }
    }

    public static bool IsRuntimeAvailable() => IsRuntimeAvailable(out _);
}
