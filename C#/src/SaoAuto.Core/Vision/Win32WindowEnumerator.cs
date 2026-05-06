using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace SaoAuto.Core.Vision;

/// <summary>
/// <see cref="IWindowEnumerator"/> backed by Win32 <c>EnumWindows</c> +
/// <c>GetClientRect</c>/<c>ClientToScreen</c>. Caches the
/// <see cref="EnumWindowsProc"/> delegate so the native trampoline does not
/// get GC'd mid-callback (mirrors the Python regression that was fixed in
/// <c>window_locator._ENUM_CB</c>).
/// </summary>
public sealed class Win32WindowEnumerator : IWindowEnumerator
{
    public IEnumerable<WindowCandidate> Enumerate()
    {
        var bucket = new List<WindowCandidate>(64);
        var proc = new EnumWindowsProc(EnumProc);
        var handle = GCHandle.Alloc(bucket);
        try
        {
            NativeMethods.EnumWindows(proc, GCHandle.ToIntPtr(handle));
        }
        finally
        {
            handle.Free();
        }
        return bucket;

        bool EnumProc(IntPtr hwnd, IntPtr lparam)
        {
            try
            {
                if (!NativeMethods.IsWindowVisible(hwnd)) return true;
                var probed = TryProbe(hwnd);
                if (probed is { } candidate)
                {
                    bucket.Add(candidate);
                }
            }
            catch
            {
                // best effort
            }
            return true;
        }
    }

    public bool IsAlive(IntPtr hwnd) =>
        NativeMethods.IsWindow(hwnd) && NativeMethods.IsWindowVisible(hwnd);

    public WindowCandidate? Probe(IntPtr hwnd) => TryProbe(hwnd);

    private static WindowCandidate? TryProbe(IntPtr hwnd)
    {
        var clientRect = GetClientRectScreen(hwnd);
        if (clientRect is null) return null;

        var (left, top, right, bottom) = clientRect.Value;
        if (right - left < 100 || bottom - top < 100) return null;

        var title = GetWindowText(hwnd);
        var process = TryGetProcessName(hwnd);
        return new WindowCandidate(hwnd, title, process, left, top, right, bottom);
    }

    private static (int Left, int Top, int Right, int Bottom)? GetClientRectScreen(IntPtr hwnd)
    {
        if (!NativeMethods.GetClientRect(hwnd, out var rect)) return null;
        var w = rect.Right;
        var h = rect.Bottom;
        if (w < 100 || h < 100) return null;

        var pt = new NativeMethods.POINT { X = 0, Y = 0 };
        if (!NativeMethods.ClientToScreen(hwnd, ref pt)) return null;

        return (pt.X, pt.Y, pt.X + w, pt.Y + h);
    }

    private static string GetWindowText(IntPtr hwnd)
    {
        var len = NativeMethods.GetWindowTextLength(hwnd);
        if (len <= 0) return string.Empty;
        var sb = new StringBuilder(len + 1);
        NativeMethods.GetWindowTextW(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    private static string? TryGetProcessName(IntPtr hwnd)
    {
        try
        {
            NativeMethods.GetWindowThreadProcessId(hwnd, out var pid);
            if (pid == 0) return null;
            using var process = Process.GetProcessById((int)pid);
            return process.ProcessName + ".exe";
        }
        catch
        {
            return null;
        }
    }

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lparam);

    private static class NativeMethods
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct RECT
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct POINT
        {
            public int X;
            public int Y;
        }

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool IsWindow(IntPtr hWnd);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern int GetWindowTextLength(IntPtr hWnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    }
}
