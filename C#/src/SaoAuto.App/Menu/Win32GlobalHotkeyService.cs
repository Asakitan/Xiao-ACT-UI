using System.Runtime.InteropServices;

namespace SaoAuto.App.Menu;

/// <summary>
/// Live `RegisterHotKey`-backed implementation of
/// <see cref="IGlobalHotkeyService"/>. Requires a message-only window
/// to receive `WM_HOTKEY`; the host wires <see cref="ProcessMessage"/>
/// to the WPF dispatcher's `ComponentDispatcher.ThreadFilterMessage`.
/// </summary>
public sealed class Win32GlobalHotkeyService : IGlobalHotkeyService
{
    private readonly Dictionary<int, HotkeyBinding> _bindings = new();
    private int _nextId = 0xB000;
    private bool _disposed;

    public event Action<int>? Triggered;

    public int Register(HotkeyBinding binding)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(Win32GlobalHotkeyService));
        var id = ++_nextId;
        var modifiers = TranslateModifiers(binding.Modifiers);
        if (!NativeMethods.RegisterHotKey(IntPtr.Zero, id, modifiers, (uint)binding.VirtualKey))
        {
            throw new InvalidOperationException(
                $"RegisterHotKey failed (err={Marshal.GetLastWin32Error()}, modifiers={binding.Modifiers}, vk=0x{binding.VirtualKey:X})");
        }
        _bindings[id] = binding;
        return id;
    }

    public void Unregister(int id)
    {
        if (_bindings.Remove(id))
        {
            NativeMethods.UnregisterHotKey(IntPtr.Zero, id);
        }
    }

    /// <summary>
    /// Process a `WM_HOTKEY` message. Hosts route here from their
    /// dispatcher hook. Returns true when the id matched a registered binding.
    /// </summary>
    public bool ProcessMessage(uint message, IntPtr wParam)
    {
        if (message != NativeMethods.WM_HOTKEY) return false;
        var id = wParam.ToInt32();
        if (!_bindings.ContainsKey(id)) return false;
        try { Triggered?.Invoke(id); } catch { /* swallow */ }
        return true;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var id in _bindings.Keys.ToArray())
        {
            NativeMethods.UnregisterHotKey(IntPtr.Zero, id);
        }
        _bindings.Clear();
    }

    private static uint TranslateModifiers(HotkeyModifiers mods)
    {
        uint result = 0;
        if ((mods & HotkeyModifiers.Alt) != 0) result |= NativeMethods.MOD_ALT;
        if ((mods & HotkeyModifiers.Ctrl) != 0) result |= NativeMethods.MOD_CONTROL;
        if ((mods & HotkeyModifiers.Shift) != 0) result |= NativeMethods.MOD_SHIFT;
        if ((mods & HotkeyModifiers.Win) != 0) result |= NativeMethods.MOD_WIN;
        return result;
    }

    private static class NativeMethods
    {
        internal const uint MOD_ALT = 0x0001;
        internal const uint MOD_CONTROL = 0x0002;
        internal const uint MOD_SHIFT = 0x0004;
        internal const uint MOD_WIN = 0x0008;
        internal const uint WM_HOTKEY = 0x0312;

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool RegisterHotKey(IntPtr hwnd, int id, uint modifiers, uint vk);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool UnregisterHotKey(IntPtr hwnd, int id);
    }
}
