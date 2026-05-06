using System.Runtime.InteropServices;
using SaoAuto.Overlay.Rendering;

namespace SaoAuto.Overlay.Windows;

/// <summary>
/// Live <c>UpdateLayeredWindow</c> presenter. Owns a top-level Win32
/// window (no WPF chrome) and renders a top-down premultiplied BGRA
/// <see cref="FrameBuffer"/> straight onto it. Mirrors Python's
/// <c>gpu_overlay_window.GpuOverlayWindow</c> / ULW path.
///
/// The window has WS_EX_LAYERED + WS_EX_TOPMOST set; click-through
/// can be toggled at runtime via <see cref="SetClickThrough"/>.
/// </summary>
public sealed class LayeredOverlayWindow : IDisposable
{
    private const string WindowClassName = "SaoAuto.LayeredOverlay";
    private static int _classRegistered;
    private static readonly NativeMethods.WndProc StaticWndProc = WndProc;

    private IntPtr _hwnd;
    private bool _disposed;

    public IntPtr Handle => _hwnd;
    public int X { get; private set; }
    public int Y { get; private set; }
    public int Width { get; private set; }
    public int Height { get; private set; }
    public bool ClickThrough { get; private set; }

    public LayeredOverlayWindow(string title, int x, int y, int width, int height,
        bool clickThrough = true, bool topmost = true)
    {
        EnsureClass();
        var exStyle = NativeMethods.WS_EX_LAYERED;
        if (topmost) exStyle |= NativeMethods.WS_EX_TOPMOST;
        if (clickThrough) exStyle |= NativeMethods.WS_EX_TRANSPARENT | NativeMethods.WS_EX_NOACTIVATE;

        _hwnd = NativeMethods.CreateWindowEx(
            exStyle,
            WindowClassName,
            title,
            NativeMethods.WS_POPUP,
            x, y, width, height,
            IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (_hwnd == IntPtr.Zero)
        {
            throw new InvalidOperationException(
                $"CreateWindowEx failed (err={Marshal.GetLastWin32Error()})");
        }
        X = x; Y = y; Width = width; Height = height;
        ClickThrough = clickThrough;
    }

    public void Show()
    {
        if (_hwnd == IntPtr.Zero) return;
        NativeMethods.ShowWindow(_hwnd, NativeMethods.SW_SHOWNOACTIVATE);
    }

    public void Hide()
    {
        if (_hwnd == IntPtr.Zero) return;
        NativeMethods.ShowWindow(_hwnd, NativeMethods.SW_HIDE);
    }

    public void SetClickThrough(bool enabled)
    {
        if (_hwnd == IntPtr.Zero) return;
        var ex = NativeMethods.GetWindowLongPtr(_hwnd, NativeMethods.GWL_EXSTYLE).ToInt64();
        if (enabled)
        {
            ex |= NativeMethods.WS_EX_TRANSPARENT | NativeMethods.WS_EX_NOACTIVATE;
        }
        else
        {
            ex &= ~(long)(NativeMethods.WS_EX_TRANSPARENT | NativeMethods.WS_EX_NOACTIVATE);
        }
        NativeMethods.SetWindowLongPtr(_hwnd, NativeMethods.GWL_EXSTYLE, new IntPtr(ex));
        ClickThrough = enabled;
    }

    public void RaiseTopmost()
    {
        if (_hwnd == IntPtr.Zero) return;
        NativeMethods.SetWindowPos(_hwnd, NativeMethods.HWND_TOPMOST, 0, 0, 0, 0,
            NativeMethods.SWP_NOMOVE | NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOACTIVATE);
    }

    public void MoveTo(int x, int y)
    {
        if (_hwnd == IntPtr.Zero) return;
        NativeMethods.SetWindowPos(_hwnd, IntPtr.Zero, x, y, 0, 0,
            NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_NOZORDER);
        X = x; Y = y;
    }

    /// <summary>
    /// Present a top-down premultiplied BGRA buffer. Width/height must
    /// match this window's dimensions or the call is rejected.
    /// </summary>
    public bool Present(FrameBuffer buffer, byte alpha = 255)
    {
        if (_hwnd == IntPtr.Zero) return false;
        if (buffer.Width != Width || buffer.Height != Height) return false;

        var hdcScreen = NativeMethods.GetDC(IntPtr.Zero);
        var hdcMem = NativeMethods.CreateCompatibleDC(hdcScreen);
        var hOld = IntPtr.Zero;
        var hBmp = IntPtr.Zero;
        try
        {
            var bmi = new NativeMethods.BITMAPINFO
            {
                bmiHeader = new NativeMethods.BITMAPINFOHEADER
                {
                    biSize = (uint)Marshal.SizeOf<NativeMethods.BITMAPINFOHEADER>(),
                    biWidth = Width,
                    biHeight = -Height, // top-down
                    biPlanes = 1,
                    biBitCount = 32,
                    biCompression = NativeMethods.BI_RGB,
                },
            };
            hBmp = NativeMethods.CreateDIBSection(hdcMem, ref bmi, NativeMethods.DIB_RGB_COLORS,
                out var bits, IntPtr.Zero, 0);
            if (hBmp == IntPtr.Zero || bits == IntPtr.Zero) return false;

            Marshal.Copy(buffer.Pixels, 0, bits, buffer.Pixels.Length);
            hOld = NativeMethods.SelectObject(hdcMem, hBmp);

            var pos = new NativeMethods.POINT { X = X, Y = Y };
            var size = new NativeMethods.SIZE { Cx = Width, Cy = Height };
            var src = new NativeMethods.POINT { X = 0, Y = 0 };
            var blend = new NativeMethods.BLENDFUNCTION
            {
                BlendOp = NativeMethods.AC_SRC_OVER,
                BlendFlags = 0,
                SourceConstantAlpha = alpha,
                AlphaFormat = NativeMethods.AC_SRC_ALPHA,
            };

            return NativeMethods.UpdateLayeredWindow(_hwnd, hdcScreen,
                ref pos, ref size, hdcMem, ref src,
                0, ref blend, NativeMethods.ULW_ALPHA);
        }
        finally
        {
            if (hOld != IntPtr.Zero) NativeMethods.SelectObject(hdcMem, hOld);
            if (hBmp != IntPtr.Zero) NativeMethods.DeleteObject(hBmp);
            NativeMethods.DeleteDC(hdcMem);
            NativeMethods.ReleaseDC(IntPtr.Zero, hdcScreen);
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_hwnd != IntPtr.Zero)
        {
            NativeMethods.DestroyWindow(_hwnd);
            _hwnd = IntPtr.Zero;
        }
    }

    private static void EnsureClass()
    {
        if (Interlocked.Exchange(ref _classRegistered, 1) == 1) return;
        var wc = new NativeMethods.WNDCLASSEX
        {
            cbSize = (uint)Marshal.SizeOf<NativeMethods.WNDCLASSEX>(),
            style = 0,
            lpfnWndProc = StaticWndProc,
            hInstance = IntPtr.Zero,
            lpszClassName = WindowClassName,
        };
        NativeMethods.RegisterClassEx(ref wc);
    }

    private static IntPtr WndProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        return NativeMethods.DefWindowProc(hwnd, msg, wParam, lParam);
    }

    private static class NativeMethods
    {
        internal const uint WS_POPUP = 0x80000000;
        internal const uint WS_EX_LAYERED = 0x00080000;
        internal const uint WS_EX_TOPMOST = 0x00000008;
        internal const uint WS_EX_TRANSPARENT = 0x00000020;
        internal const uint WS_EX_NOACTIVATE = 0x08000000;

        internal const int SW_SHOWNOACTIVATE = 4;
        internal const int SW_HIDE = 0;
        internal const int GWL_EXSTYLE = -20;

        internal static readonly IntPtr HWND_TOPMOST = new(-1);
        internal const uint SWP_NOMOVE = 0x0002;
        internal const uint SWP_NOSIZE = 0x0001;
        internal const uint SWP_NOACTIVATE = 0x0010;
        internal const uint SWP_NOZORDER = 0x0004;

        internal const uint BI_RGB = 0;
        internal const uint DIB_RGB_COLORS = 0;
        internal const uint ULW_ALPHA = 0x00000002;
        internal const byte AC_SRC_OVER = 0x00;
        internal const byte AC_SRC_ALPHA = 0x01;

        public delegate IntPtr WndProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);

        [StructLayout(LayoutKind.Sequential)]
        public struct POINT { public int X, Y; }

        [StructLayout(LayoutKind.Sequential)]
        public struct SIZE { public int Cx, Cy; }

        [StructLayout(LayoutKind.Sequential)]
        public struct BLENDFUNCTION
        {
            public byte BlendOp;
            public byte BlendFlags;
            public byte SourceConstantAlpha;
            public byte AlphaFormat;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct BITMAPINFOHEADER
        {
            public uint biSize;
            public int biWidth;
            public int biHeight;
            public ushort biPlanes;
            public ushort biBitCount;
            public uint biCompression;
            public uint biSizeImage;
            public int biXPelsPerMeter;
            public int biYPelsPerMeter;
            public uint biClrUsed;
            public uint biClrImportant;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct BITMAPINFO
        {
            public BITMAPINFOHEADER bmiHeader;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)]
            public uint[] bmiColors;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct WNDCLASSEX
        {
            public uint cbSize;
            public uint style;
            [MarshalAs(UnmanagedType.FunctionPtr)] public WndProc lpfnWndProc;
            public int cbClsExtra;
            public int cbWndExtra;
            public IntPtr hInstance;
            public IntPtr hIcon;
            public IntPtr hCursor;
            public IntPtr hbrBackground;
            [MarshalAs(UnmanagedType.LPWStr)] public string? lpszMenuName;
            [MarshalAs(UnmanagedType.LPWStr)] public string lpszClassName;
            public IntPtr hIconSm;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern ushort RegisterClassEx([In] ref WNDCLASSEX lpwcx);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateWindowEx(
            uint dwExStyle, string lpClassName, string? lpWindowName,
            uint dwStyle, int x, int y, int nWidth, int nHeight,
            IntPtr hWndParent, IntPtr hMenu, IntPtr hInstance, IntPtr lpParam);

        [DllImport("user32.dll")]
        public static extern bool DestroyWindow(IntPtr hwnd);

        [DllImport("user32.dll")]
        public static extern bool ShowWindow(IntPtr hwnd, int nCmdShow);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool SetWindowPos(IntPtr hwnd, IntPtr hwndAfter,
            int x, int y, int cx, int cy, uint uFlags);

        [DllImport("user32.dll")]
        public static extern IntPtr GetWindowLongPtr(IntPtr hwnd, int nIndex);

        [DllImport("user32.dll")]
        public static extern IntPtr SetWindowLongPtr(IntPtr hwnd, int nIndex, IntPtr dwNewLong);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr DefWindowProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern IntPtr GetDC(IntPtr hwnd);

        [DllImport("user32.dll")]
        public static extern int ReleaseDC(IntPtr hwnd, IntPtr hdc);

        [DllImport("gdi32.dll")]
        public static extern IntPtr CreateCompatibleDC(IntPtr hdc);

        [DllImport("gdi32.dll")]
        public static extern bool DeleteDC(IntPtr hdc);

        [DllImport("gdi32.dll")]
        public static extern IntPtr CreateDIBSection(IntPtr hdc, ref BITMAPINFO bmi, uint usage,
            out IntPtr ppvBits, IntPtr hSection, uint offset);

        [DllImport("gdi32.dll")]
        public static extern IntPtr SelectObject(IntPtr hdc, IntPtr h);

        [DllImport("gdi32.dll")]
        public static extern bool DeleteObject(IntPtr ho);

        [DllImport("user32.dll")]
        public static extern bool UpdateLayeredWindow(IntPtr hwnd, IntPtr hdcDst,
            ref POINT pptDst, ref SIZE psize, IntPtr hdcSrc, ref POINT pptSrc,
            int crKey, ref BLENDFUNCTION pblend, uint dwFlags);
    }
}
