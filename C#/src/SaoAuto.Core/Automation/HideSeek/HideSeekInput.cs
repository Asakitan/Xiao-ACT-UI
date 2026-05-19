using System.Runtime.InteropServices;

namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// S148 — Mouse-click contract for the HideSeek state machine.
/// Production binds <see cref="Win32HideSeekInput"/>; tests bind a
/// recording fake. Coordinates are virtual-desktop screen pixels
/// (the matcher already converted ROI-relative hits to screen
/// space before reaching this layer).
/// </summary>
public interface IHideSeekInput
{
    /// <summary>
    /// Move the cursor to (<paramref name="screenX"/>,
    /// <paramref name="screenY"/>) and issue a left click. When
    /// <paramref name="altModifier"/> is true the click is bracketed
    /// by VK_MENU down / up, matching Python's <c>_alt_click</c>.
    /// </summary>
    void Click(int screenX, int screenY, bool altModifier);
}

/// <summary>
/// Low-level primitives behind the orchestrator. Split out so that
/// <see cref="HideSeekClickOrchestrator"/> can be unit-tested with a
/// recording fake — actual SendInput calls happen only in
/// <see cref="Win32HideSeekRawInput"/>.
/// </summary>
public interface IHideSeekRawInput
{
    /// <summary>Move to virtual-desktop absolute screen coordinates.</summary>
    void MouseMoveAbsolute(int screenX, int screenY);
    void MouseLeftDown();
    void MouseLeftUp();
    void KeyDown(ushort virtualKey);
    void KeyUp(ushort virtualKey);
}

/// <summary>
/// Pure sequencing layer. Mirrors Python's <c>_send_mouse_click</c>
/// and <c>_alt_click</c> ordering and timings (30/50/50/50 ms). The
/// sleep callback is injectable so tests can pin the call ordering
/// without actually sleeping.
/// </summary>
public sealed class HideSeekClickOrchestrator
{
    public const int MoveDwellMs = 30;
    public const int ClickHoldMs = 50;
    public const int AltBracketMs = 50;
    public const ushort VK_MENU = 0x12;

    private readonly IHideSeekRawInput _raw;
    private readonly Action<int>? _sleep;

    public HideSeekClickOrchestrator(IHideSeekRawInput raw, Action<int>? sleep = null)
    {
        _raw = raw ?? throw new ArgumentNullException(nameof(raw));
        _sleep = sleep;
    }

    public void Click(int screenX, int screenY, bool altModifier)
    {
        if (altModifier)
        {
            _raw.KeyDown(VK_MENU);
            _sleep?.Invoke(AltBracketMs);
        }

        _raw.MouseMoveAbsolute(screenX, screenY);
        _sleep?.Invoke(MoveDwellMs);

        _raw.MouseLeftDown();
        _sleep?.Invoke(ClickHoldMs);
        _raw.MouseLeftUp();

        if (altModifier)
        {
            _sleep?.Invoke(AltBracketMs);
            _raw.KeyUp(VK_MENU);
        }
    }
}

/// <summary>
/// Live <c>SendInput</c>-backed click implementation. Translates a
/// screen-pixel coordinate into the virtual-desktop normalised
/// 0-65535 packet that <c>MOUSEEVENTF_ABSOLUTE | VIRTUALDESK</c>
/// expects, then dispatches via the shared orchestrator.
/// </summary>
public sealed class Win32HideSeekInput : IHideSeekInput
{
    private readonly HideSeekClickOrchestrator _orchestrator;

    public Win32HideSeekInput()
    {
        _orchestrator = new HideSeekClickOrchestrator(new Win32HideSeekRawInput(), Thread.Sleep);
    }

    public void Click(int screenX, int screenY, bool altModifier)
        => _orchestrator.Click(screenX, screenY, altModifier);
}

/// <summary>
/// Live raw-input wrapper. All PInvoke lives here so unit tests can
/// substitute a recording fake without touching user32.
/// </summary>
public sealed class Win32HideSeekRawInput : IHideSeekRawInput
{
    public void MouseMoveAbsolute(int screenX, int screenY)
    {
        if (!TryGetVirtualDesktop(out int vx, out int vy, out int vw, out int vh))
            return;
        int x = Math.Max(vx, Math.Min(vx + Math.Max(1, vw) - 1, screenX));
        int y = Math.Max(vy, Math.Min(vy + Math.Max(1, vh) - 1, screenY));
        NativeMethods.SetCursorPos(x, y);
        int absX = (int)((x - vx) * 65535L / Math.Max(1, vw - 1));
        int absY = (int)((y - vy) * 65535L / Math.Max(1, vh - 1));
        SendMouse(absX, absY, NativeMethods.MOUSEEVENTF_ABSOLUTE | NativeMethods.MOUSEEVENTF_VIRTUALDESK | NativeMethods.MOUSEEVENTF_MOVE);
    }

    public void MouseLeftDown() => SendMouse(0, 0, NativeMethods.MOUSEEVENTF_LEFTDOWN);
    public void MouseLeftUp() => SendMouse(0, 0, NativeMethods.MOUSEEVENTF_LEFTUP);

    public void KeyDown(ushort virtualKey) => SendKey(virtualKey, up: false);
    public void KeyUp(ushort virtualKey) => SendKey(virtualKey, up: true);

    private static bool TryGetVirtualDesktop(out int x, out int y, out int w, out int h)
    {
        x = NativeMethods.GetSystemMetrics(NativeMethods.SM_XVIRTUALSCREEN);
        y = NativeMethods.GetSystemMetrics(NativeMethods.SM_YVIRTUALSCREEN);
        w = NativeMethods.GetSystemMetrics(NativeMethods.SM_CXVIRTUALSCREEN);
        h = NativeMethods.GetSystemMetrics(NativeMethods.SM_CYVIRTUALSCREEN);
        if (w <= 0 || h <= 0)
        {
            x = 0; y = 0;
            w = NativeMethods.GetSystemMetrics(0);
            h = NativeMethods.GetSystemMetrics(1);
        }
        return w > 0 && h > 0;
    }

    private static void SendMouse(int dx, int dy, uint flags)
    {
        var inputs = new NativeMethods.INPUT[1];
        inputs[0].type = NativeMethods.INPUT_MOUSE;
        inputs[0].U.mi = new NativeMethods.MOUSEINPUT
        {
            dx = dx, dy = dy, mouseData = 0, dwFlags = flags, time = 0, dwExtraInfo = IntPtr.Zero,
        };
        NativeMethods.SendInput(1, inputs, Marshal.SizeOf<NativeMethods.INPUT>());
    }

    private static void SendKey(ushort vk, bool up)
    {
        var inputs = new NativeMethods.INPUT[1];
        inputs[0].type = NativeMethods.INPUT_KEYBOARD;
        inputs[0].U.ki = new NativeMethods.KEYBDINPUT
        {
            wVk = vk, wScan = 0,
            dwFlags = up ? NativeMethods.KEYEVENTF_KEYUP : 0u,
            time = 0, dwExtraInfo = IntPtr.Zero,
        };
        NativeMethods.SendInput(1, inputs, Marshal.SizeOf<NativeMethods.INPUT>());
    }

    private static class NativeMethods
    {
        internal const uint INPUT_MOUSE = 0;
        internal const uint INPUT_KEYBOARD = 1;
        internal const uint MOUSEEVENTF_MOVE = 0x0001;
        internal const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
        internal const uint MOUSEEVENTF_LEFTUP = 0x0004;
        internal const uint MOUSEEVENTF_VIRTUALDESK = 0x4000;
        internal const uint MOUSEEVENTF_ABSOLUTE = 0x8000;
        internal const uint KEYEVENTF_KEYUP = 0x0002;
        internal const int SM_XVIRTUALSCREEN = 76;
        internal const int SM_YVIRTUALSCREEN = 77;
        internal const int SM_CXVIRTUALSCREEN = 78;
        internal const int SM_CYVIRTUALSCREEN = 79;

        [StructLayout(LayoutKind.Sequential)]
        public struct KEYBDINPUT
        {
            public ushort wVk;
            public ushort wScan;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct MOUSEINPUT
        {
            public int dx;
            public int dy;
            public uint mouseData;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct HARDWAREINPUT
        {
            public uint uMsg;
            public ushort wParamL;
            public ushort wParamH;
        }

        [StructLayout(LayoutKind.Explicit)]
        public struct InputUnion
        {
            [FieldOffset(0)] public MOUSEINPUT mi;
            [FieldOffset(0)] public KEYBDINPUT ki;
            [FieldOffset(0)] public HARDWAREINPUT hi;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct INPUT
        {
            public uint type;
            public InputUnion U;
        }

        [DllImport("user32.dll", SetLastError = true)]
        public static extern uint SendInput(uint nInputs,
            [MarshalAs(UnmanagedType.LPArray)] INPUT[] pInputs, int cbSize);

        [DllImport("user32.dll")]
        public static extern bool SetCursorPos(int x, int y);

        [DllImport("user32.dll")]
        public static extern int GetSystemMetrics(int nIndex);
    }
}
