using System.Runtime.InteropServices;

namespace SaoAuto.Core.Vision;

/// <summary>
/// Live <see cref="IFrameCapture"/> using GDI <c>BitBlt</c> + <c>PrintWindow</c>.
/// Mirrors the Python `recognition._capture_via_gdi` path. Returns
/// <c>null</c> when the source window is gone or the capture call fails.
/// </summary>
public sealed class GdiFrameCapture : IFrameCapture
{
    private readonly Func<WindowCandidate?> _windowProvider;
    private bool _disposed;

    public GdiFrameCapture(Func<WindowCandidate?> windowProvider)
    {
        _windowProvider = windowProvider ?? throw new ArgumentNullException(nameof(windowProvider));
    }

    public CapturedFrame? Capture()
    {
        if (_disposed) return null;
        var window = _windowProvider();
        if (window is null || !window.Value.LooksLikeGameWindow) return null;

        var hwnd = window.Value.Hwnd;
        if (hwnd == IntPtr.Zero) return null;
        var width = window.Value.Width;
        var height = window.Value.Height;
        if (width <= 0 || height <= 0) return null;

        var hdcSrc = NativeMethods.GetDC(hwnd);
        if (hdcSrc == IntPtr.Zero) return null;
        var hdcMem = IntPtr.Zero;
        var hBmp = IntPtr.Zero;
        var hOld = IntPtr.Zero;
        try
        {
            hdcMem = NativeMethods.CreateCompatibleDC(hdcSrc);
            hBmp = NativeMethods.CreateCompatibleBitmap(hdcSrc, width, height);
            if (hdcMem == IntPtr.Zero || hBmp == IntPtr.Zero) return null;
            hOld = NativeMethods.SelectObject(hdcMem, hBmp);

            var ok = NativeMethods.PrintWindow(hwnd, hdcMem, NativeMethods.PW_CLIENTONLY | NativeMethods.PW_RENDERFULLCONTENT);
            if (!ok)
            {
                ok = NativeMethods.BitBlt(hdcMem, 0, 0, width, height, hdcSrc, 0, 0, NativeMethods.SRCCOPY);
            }
            if (!ok) return null;

            // Read pixels via GetDIBits.
            var stride = width * 4;
            var pixels = new byte[stride * height];
            var bmi = new NativeMethods.BITMAPINFO
            {
                bmiHeader = new NativeMethods.BITMAPINFOHEADER
                {
                    biSize = (uint)Marshal.SizeOf<NativeMethods.BITMAPINFOHEADER>(),
                    biWidth = width,
                    biHeight = -height, // top-down
                    biPlanes = 1,
                    biBitCount = 32,
                    biCompression = NativeMethods.BI_RGB,
                },
            };
            var copied = NativeMethods.GetDIBits(hdcMem, hBmp, 0, (uint)height, pixels, ref bmi, NativeMethods.DIB_RGB_COLORS);
            if (copied <= 0) return null;
            return new CapturedFrame(width, height, stride, pixels);
        }
        finally
        {
            if (hdcMem != IntPtr.Zero)
            {
                if (hOld != IntPtr.Zero) NativeMethods.SelectObject(hdcMem, hOld);
                NativeMethods.DeleteDC(hdcMem);
            }
            if (hBmp != IntPtr.Zero) NativeMethods.DeleteObject(hBmp);
            NativeMethods.ReleaseDC(hwnd, hdcSrc);
        }
    }

    public void Dispose() => _disposed = true;

    private static class NativeMethods
    {
        internal const int PW_CLIENTONLY = 0x00000001;
        internal const int PW_RENDERFULLCONTENT = 0x00000002;
        internal const uint SRCCOPY = 0x00CC0020;
        internal const uint BI_RGB = 0;
        internal const uint DIB_RGB_COLORS = 0;

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

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        public struct BITMAPINFO
        {
            public BITMAPINFOHEADER bmiHeader;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)]
            public uint[] bmiColors;
        }

        [DllImport("user32.dll")]
        public static extern IntPtr GetDC(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);

        [DllImport("gdi32.dll", SetLastError = true)]
        public static extern IntPtr CreateCompatibleDC(IntPtr hdc);

        [DllImport("gdi32.dll", SetLastError = true)]
        public static extern IntPtr CreateCompatibleBitmap(IntPtr hdc, int cx, int cy);

        [DllImport("gdi32.dll")]
        public static extern IntPtr SelectObject(IntPtr hdc, IntPtr h);

        [DllImport("gdi32.dll")]
        public static extern bool DeleteObject(IntPtr ho);

        [DllImport("gdi32.dll")]
        public static extern bool DeleteDC(IntPtr hdc);

        [DllImport("gdi32.dll")]
        public static extern bool BitBlt(
            IntPtr hdc, int x, int y, int cx, int cy,
            IntPtr hdcSrc, int x1, int y1, uint rop);

        [DllImport("user32.dll")]
        public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint flags);

        [DllImport("gdi32.dll")]
        public static extern int GetDIBits(IntPtr hdc, IntPtr hbm, uint start, uint cLines,
            byte[] lpvBits, ref BITMAPINFO lpbmi, uint usage);
    }
}
