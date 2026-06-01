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
    // F1/F3: WebView and Entity hosts share the same offset but use different
    // height profiles. WebView is the canonical 500px high HUD; Entity host
    // (post-port) will use a slimmer 320px profile per Python's parity at
    // sao_gui_hp.py:898-902. Until Entity overlay panels port natively, both
    // profiles fall through to HudHeight.
    public const double EntityHudHeight = HudHeight;
    public const double WebViewHudHeight = HudHeight;

    public enum Profile
    {
        WebView = 0,
        Entity = 1,
    }

    public readonly record struct Bounds(double X, double Y, double W, double H);

    public static Bounds Compute(SettingsManager? settings = null, ILogger? logger = null)
        => Compute(Profile.WebView, settings, logger, gameWindowHandle: IntPtr.Zero);

    /// <summary>
    /// F1/F3/F4 monitor-aware compute. When <paramref name="gameWindowHandle"/>
    /// is non-zero, the HUD is anchored to the monitor containing that hwnd
    /// (mirrors Python's game-window-aware HudGeometry in sao_webview.py).
    /// Falls back to <see cref="System.Windows.SystemParameters.PrimaryScreenWidth"/>
    /// when the lookup fails. Different <see cref="Profile"/> values select
    /// per-host height / offset tuning.
    /// </summary>
    public static Bounds Compute(
        Profile profile,
        SettingsManager? settings = null,
        ILogger? logger = null,
        IntPtr gameWindowHandle = default)
    {
        var log = logger ?? NullLogger.Instance;
        try
        {
            double monLeft = 0, monTop = 0;
            double monWidth = System.Windows.SystemParameters.PrimaryScreenWidth;
            double monHeight = System.Windows.SystemParameters.PrimaryScreenHeight;
            // F1: monitor-aware — when we have a game window, resolve which
            // monitor contains it and use that monitor's rect. Falls back to
            // the primary screen on lookup failure.
            if (gameWindowHandle != IntPtr.Zero
                && TryGetMonitorRect(gameWindowHandle, out var rect))
            {
                monLeft = rect.Left;
                monTop = rect.Top;
                monWidth = rect.Right - rect.Left;
                monHeight = rect.Bottom - rect.Top;
            }

            var offsetX = settings?.Get<double?>(SettingsKeys.HudOffsetX) ?? DefaultOffsetX;
            var height = profile switch
            {
                Profile.Entity => EntityHudHeight,
                _ => WebViewHudHeight,
            };

            var bounds = new Bounds(
                X: monLeft + monWidth * offsetX,
                Y: monTop + monHeight - height,
                W: monWidth * 0.75,
                H: height);
            log.LogInformation(
                "HUD geometry [{Profile}]: monitor=({ML},{MT})+{MW}x{MH}; hud={Hw}x{Hh} @ ({X},{Y}) offset_x={OffsetX}",
                profile, monLeft, monTop, monWidth, monHeight,
                bounds.W, bounds.H, bounds.X, bounds.Y, offsetX);
            return bounds;
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "HudGeometry.Compute failed; falling back to 1920x1080 defaults");
            // Sane fallback so the window still appears somewhere visible.
            return new Bounds(X: 76, Y: 580, W: 1440, H: HudHeight);
        }
    }

    /// <summary>F1 helper: resolve the monitor rect containing the given
    /// window handle. Returns false when the hwnd is bad or MonitorFromWindow
    /// fails. Output is in physical pixels (matches the SetWindowPos path).</summary>
    private static bool TryGetMonitorRect(IntPtr hwnd, out RECT rect)
    {
        rect = default;
        try
        {
            const uint MONITOR_DEFAULTTONEAREST = 2;
            var hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (hMonitor == IntPtr.Zero) return false;
            var info = new MONITORINFO { cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<MONITORINFO>() };
            if (!GetMonitorInfo(hMonitor, ref info)) return false;
            rect = info.rcMonitor;
            return true;
        }
        catch
        {
            return false;
        }
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct MONITORINFO
    {
        public uint cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint dwFlags);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    [return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
    private static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO lpmi);

    public static void Apply(System.Windows.Window window, SettingsManager? settings = null, ILogger? logger = null)
        => Apply(window, Profile.WebView, settings, logger, gameWindowHandle: IntPtr.Zero);

    /// <summary>
    /// F1/F3 multi-monitor / Entity-vs-WebView profile-aware Apply.
    /// When <paramref name="gameWindowHandle"/> is non-zero, the HUD
    /// anchors to the monitor containing that window. Callers that
    /// know which profile they are (EntityHostWindow / WebViewHostWindow)
    /// should pass it so the per-host height profile applies.
    /// </summary>
    public static void Apply(
        System.Windows.Window window,
        Profile profile,
        SettingsManager? settings = null,
        ILogger? logger = null,
        IntPtr gameWindowHandle = default)
    {
        if (window is null) throw new ArgumentNullException(nameof(window));
        var b = Compute(profile, settings, logger, gameWindowHandle);
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

    // Extended window style bits — mirrors Python sao_gui_hp.py:898-902
    // which uses WS_EX_TOOLWINDOW (no taskbar entry + no Alt-Tab) and
    // WS_EX_NOACTIVATE (don't steal focus on click) for the HUD host.
    // NOTE: WS_EX_TRANSPARENT (0x20) is intentionally NOT set — Python
    // toggles it per-frame via cursor-hit-region logic in
    // sao_webview.py:4115-4191. Setting it statically would make the HUD
    // permanently un-clickable. WS_EX_LAYERED (0x80000) is set per host:
    // EntityHostWindow gets it via AllowsTransparency=True in XAML;
    // WebViewHostWindow does NOT — it uses DWM composition instead
    // (see Bug fix (user 2026-06-01) note in WebViewHostWindow.xaml.cs).
    private const int GWL_EXSTYLE = -20;
    private const int WS_EX_TOOLWINDOW = 0x00000080;
    private const int WS_EX_NOACTIVATE = 0x08000000;

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

            // Bug fix (user 2026-06-01): WebView click-through.
            // Apply extended-style bits so the HUD behaves like Python's
            // tk overlay: no taskbar entry, no Alt-Tab, no focus-steal.
            // WS_EX_TOOLWINDOW + WS_EX_NOACTIVATE merge is correct for
            // both hosts and stays unchanged here.
            //
            // Click-through hit-test strategy now differs per host:
            //   - WebViewHostWindow: uses DWM composition
            //     (DwmExtendFrameIntoClientArea, called in
            //     WebViewHostWindow.OnSourceInitialized) — NOT WS_EX_LAYERED.
            //     Reason: a native WebView2 child HWND breaks the
            //     layered-window alpha hit-test contract (its DComp
            //     surface never participates in UpdateLayeredWindow's
            //     alpha plane, so OS hit-test would treat every WebView2
            //     pixel as transparent and clicks would fall through).
            //     DWM composition keeps the HWND non-layered so Windows
            //     hit-tests against the real HWND tree while pixels with
            //     alpha=0 still appear transparent via composition.
            //   - EntityHostWindow: keeps AllowsTransparency=True (which
            //     sets WS_EX_LAYERED) because its content is pure WPF
            //     with no child HWND — per-pixel alpha works correctly
            //     and is needed for the SAO HP-bar anti-aliased glow.
            // WS_EX_TRANSPARENT is intentionally NOT merged on either
            // host — Python toggles it per-frame via cursor-hit-region
            // logic; statically setting it makes the HUD permanently
            // un-clickable.
            var ex = GetWindowLong(hwnd, GWL_EXSTYLE);
            int merged = ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            if (merged != ex)
            {
                SetWindowLong(hwnd, GWL_EXSTYLE, merged);
            }
        }
        catch { /* swallow — best effort */ }
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    [return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);

    [System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
    private static extern int GetWindowLong(IntPtr hWnd, int nIndex);

    [System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "SetWindowLongW")]
    private static extern int SetWindowLong(IntPtr hWnd, int nIndex, int dwNewLong);
}
