using System.Runtime.InteropServices;
using SaoAuto.Core.Vision;

namespace SaoAuto.App.Startup;

/// <summary>
/// S138 — Foreground probe used by
/// <see cref="SaoAuto.Core.Automation.AutoKeyTickHost"/>. Compares
/// Win32 <c>GetForegroundWindow</c> to the HWND the
/// <see cref="WindowLocator"/> last resolved.
///
/// Lives in App so Core keeps its Win32-free boundary at the
/// dispatcher / probe seams (probes are injected as
/// <see cref="Func{Boolean}"/>).
/// </summary>
public static class GameForegroundProbe
{
    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    public static Func<bool> Bind(WindowLocator locator)
    {
        ArgumentNullException.ThrowIfNull(locator);
        return () =>
        {
            try
            {
                var candidate = locator.FindGameWindow();
                if (candidate is not { } c) return false;
                return c.Hwnd == GetForegroundWindow();
            }
            catch
            {
                return false;
            }
        };
    }
}
