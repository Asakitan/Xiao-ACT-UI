using System.Diagnostics;
using System.Runtime.InteropServices;

namespace SaoAuto.App.Startup;

/// <summary>
/// Per-process startup hooks that must run before any window is created.
/// Mirrors Python <c>main._early_dpi_aware</c> and <c>_elevate_process_priority</c>.
/// </summary>
public static class EarlyProcessSetup
{
    private static int _ran;

    /// <summary>Idempotent. Safe to call from multiple entry points.</summary>
    public static void Run()
    {
        if (Interlocked.Exchange(ref _ran, 1) == 1) return;
        EnableDpiAwareness();
        ElevateProcessPriorityAboveNormal();
    }

    public static bool EnableDpiAwareness()
    {
        try
        {
            // SetProcessDpiAwarenessContext (Win10 1703+) — PerMonitorV2.
            if (NativeMethods.SetProcessDpiAwarenessContext(NativeMethods.PerMonitorV2))
            {
                return true;
            }
        }
        catch (DllNotFoundException) { /* fall through */ }
        catch (EntryPointNotFoundException) { /* fall through */ }

        try
        {
            if (NativeMethods.SetProcessDpiAwareness(2) == 0)
            {
                return true;
            }
        }
        catch (DllNotFoundException) { /* fall through */ }
        catch (EntryPointNotFoundException) { /* fall through */ }

        try
        {
            return NativeMethods.SetProcessDPIAware();
        }
        catch
        {
            return false;
        }
    }

    public static bool ElevateProcessPriorityAboveNormal()
    {
        try
        {
            using var p = Process.GetCurrentProcess();
            p.PriorityClass = ProcessPriorityClass.AboveNormal;
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static class NativeMethods
    {
        internal static readonly IntPtr PerMonitorV2 = new(-4);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetProcessDpiAwarenessContext(IntPtr value);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetProcessDPIAware();

        [DllImport("shcore.dll", SetLastError = true)]
        internal static extern int SetProcessDpiAwareness(int value);
    }
}
