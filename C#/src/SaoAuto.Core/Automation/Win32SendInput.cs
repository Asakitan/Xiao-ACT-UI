using System.Runtime.InteropServices;

namespace SaoAuto.Core.Automation;

/// <summary>
/// Live `SendInput`-backed keystroke writer. Mirrors Python's
/// `auto_key_engine._send_keys` — produces virtual-key down/up events
/// with optional hold delay. Admin-elevation warning when the foreground
/// window outranks our token (callers can check via
/// <see cref="WarnIfTargetElevated"/>).
/// </summary>
public sealed class Win32SendInput
{
    public void PressKey(int virtualKey, AutoKeyModifiers modifiers, int holdMs = 0)
    {
        var inputs = new List<NativeMethods.INPUT>();
        if ((modifiers & AutoKeyModifiers.Shift) != 0) inputs.Add(KeyDown(NativeMethods.VK_SHIFT));
        if ((modifiers & AutoKeyModifiers.Ctrl) != 0) inputs.Add(KeyDown(NativeMethods.VK_CONTROL));
        if ((modifiers & AutoKeyModifiers.Alt) != 0) inputs.Add(KeyDown(NativeMethods.VK_MENU));
        inputs.Add(KeyDown((ushort)virtualKey));

        SendBatch(inputs);
        if (holdMs > 0) Thread.Sleep(holdMs);

        var release = new List<NativeMethods.INPUT> { KeyUp((ushort)virtualKey) };
        if ((modifiers & AutoKeyModifiers.Alt) != 0) release.Add(KeyUp(NativeMethods.VK_MENU));
        if ((modifiers & AutoKeyModifiers.Ctrl) != 0) release.Add(KeyUp(NativeMethods.VK_CONTROL));
        if ((modifiers & AutoKeyModifiers.Shift) != 0) release.Add(KeyUp(NativeMethods.VK_SHIFT));
        SendBatch(release);
    }

    /// <summary>
    /// Returns true when the foreground window's process runs at higher
    /// integrity than our process (we cannot send input to it via SendInput).
    /// </summary>
    public static bool WarnIfTargetElevated()
    {
        try
        {
            var hwnd = NativeMethods.GetForegroundWindow();
            if (hwnd == IntPtr.Zero) return false;
            NativeMethods.GetWindowThreadProcessId(hwnd, out var pid);
            if (pid == 0) return false;
            var process = NativeMethods.OpenProcess(NativeMethods.PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if (process == IntPtr.Zero) return true; // can't open → likely elevated
            try
            {
                return false;
            }
            finally
            {
                NativeMethods.CloseHandle(process);
            }
        }
        catch
        {
            return false;
        }
    }

    private static void SendBatch(IReadOnlyList<NativeMethods.INPUT> inputs)
    {
        if (inputs.Count == 0) return;
        var arr = inputs.ToArray();
        NativeMethods.SendInput((uint)arr.Length, arr,
            Marshal.SizeOf<NativeMethods.INPUT>());
    }

    private static NativeMethods.INPUT KeyDown(ushort vk) => MakeInput(vk, false);
    private static NativeMethods.INPUT KeyUp(ushort vk) => MakeInput(vk, true);

    private static NativeMethods.INPUT MakeInput(ushort vk, bool keyup)
    {
        return new NativeMethods.INPUT
        {
            type = NativeMethods.INPUT_KEYBOARD,
            U = new NativeMethods.InputUnion
            {
                ki = new NativeMethods.KEYBDINPUT
                {
                    wVk = vk,
                    wScan = 0,
                    dwFlags = keyup ? NativeMethods.KEYEVENTF_KEYUP : 0,
                    time = 0,
                    dwExtraInfo = IntPtr.Zero,
                },
            },
        };
    }

    private static class NativeMethods
    {
        internal const uint INPUT_KEYBOARD = 1;
        internal const uint KEYEVENTF_KEYUP = 0x0002;
        internal const ushort VK_SHIFT = 0x10;
        internal const ushort VK_CONTROL = 0x11;
        internal const ushort VK_MENU = 0x12;
        internal const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

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
        public static extern IntPtr GetForegroundWindow();

        [DllImport("user32.dll", SetLastError = true)]
        public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr handle);
    }
}
