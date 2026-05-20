using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.Hosting;

/// <summary>
/// S196 — Shared HUD geometry formula used by both
/// <see cref="WebViewHostWindow"/> and <see cref="EntityHostWindow"/>.
/// 1:1 port of Python's <c>sao_webview.py</c> lines 3282–3318 +
/// <c>_calc_hud_target</c> (line 3589).
///
/// <list type="bullet">
///   <item>width = <c>0.75 * monitor_width</c></item>
///   <item>height = <c>500</c> (fixed, matches Python's
///   non-fullscreen <c>hp_h</c>)</item>
///   <item>x = <c>monitor_left + hud_offset_x * monitor_width</c>
///   (default offset 0.04)</item>
///   <item>y = <c>monitor_top + monitor_height - 500</c>
///   (anchored to the bottom of the *full* monitor — overlays the
///   taskbar so a fullscreen game shows beneath the HUD)</item>
/// </list>
///
/// Uses the full primary screen rect (<c>PrimaryScreenWidth/Height</c>),
/// not the work area, so the HUD covers the Windows taskbar in
/// borderless / fullscreen-game scenarios.
/// </summary>
public static class HudGeometry
{
    public const double HudHeight = 500;
    public const double DefaultOffsetX = 0.04;

    public readonly record struct Bounds(double X, double Y, double W, double H);

    public static Bounds Compute(SettingsManager? settings = null, ILogger? logger = null)
    {
        var log = logger ?? NullLogger.Instance;
        try
        {
            var sw = System.Windows.SystemParameters.PrimaryScreenWidth;
            var sh = System.Windows.SystemParameters.PrimaryScreenHeight;
            var offsetX = settings?.Get<double?>(SettingsKeys.HudOffsetX) ?? DefaultOffsetX;
            if (offsetX < -0.5 || offsetX > 0.95) offsetX = DefaultOffsetX;

            var bounds = new Bounds(
                X: sw * offsetX,
                Y: sh - HudHeight,
                W: sw * 0.75,
                H: HudHeight);
            log.LogInformation(
                "HUD geometry: monitor={Sw}x{Sh}; hud={Hw}x{Hh} @ ({X},{Y}) offset_x={OffsetX}",
                sw, sh, bounds.W, bounds.H, bounds.X, bounds.Y, offsetX);
            return bounds;
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "HudGeometry.Compute failed; falling back to 1920x1080 defaults");
            // Sane fallback so the window still appears somewhere visible.
            return new Bounds(X: 76, Y: 580, W: 1440, H: HudHeight);
        }
    }

    public static void Apply(System.Windows.Window window, SettingsManager? settings = null, ILogger? logger = null)
    {
        if (window is null) throw new ArgumentNullException(nameof(window));
        var b = Compute(settings, logger);
        window.WindowStartupLocation = System.Windows.WindowStartupLocation.Manual;
        window.Width = b.W;
        window.Height = b.H;
        window.Left = b.X;
        window.Top = b.Y;
        // After the window is shown, force the geometry and topmost via
        // Win32 SetWindowPos so we (a) beat the Windows shell taskbar in
        // z-order and (b) place the window in *physical* pixels so the
        // taskbar work-area constraint doesn't clip the bottom of our
        // HUD. WPF's high-level Top/Height respect the work area in
        // some scenarios; Python's sao_webview.py works around this with
        // the same SetWindowPos dance (sao_webview.py lines 4296-4360).
        window.SourceInitialized += (_, _) => ForceGeometryAndTopmost(window, b);
        window.Loaded += (_, _) => ForceGeometryAndTopmost(window, b);
    }

    private static readonly IntPtr HWND_TOPMOST = new(-1);
    private const uint SWP_NOACTIVATE = 0x0010;
    private const uint SWP_SHOWWINDOW = 0x0040;

    private static void ForceGeometryAndTopmost(System.Windows.Window window, Bounds b)
    {
        try
        {
            var hwnd = new System.Windows.Interop.WindowInteropHelper(window).Handle;
            if (hwnd == IntPtr.Zero) return;
            // Convert WPF DIPs → physical pixels via the window's
            // PresentationSource DPI matrix. On 100% DPI this is a no-op;
            // on 150/200% it scales correctly.
            var source = System.Windows.PresentationSource.FromVisual(window);
            double sx = 1.0, sy = 1.0;
            if (source?.CompositionTarget is { } ct)
            {
                sx = ct.TransformToDevice.M11;
                sy = ct.TransformToDevice.M22;
            }
            int px = (int)Math.Round(b.X * sx);
            int py = (int)Math.Round(b.Y * sy);
            int pw = (int)Math.Round(b.W * sx);
            int ph = (int)Math.Round(b.H * sy);
            SetWindowPos(hwnd, HWND_TOPMOST, px, py, pw, ph,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        catch { /* swallow — best effort */ }
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    [return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
}
