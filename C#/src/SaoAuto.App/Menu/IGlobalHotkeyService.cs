namespace SaoAuto.App.Menu;

/// <summary>
/// Global hotkey contract. The live Win32 implementation lands in Session 8b
/// (P/Invoke `RegisterHotKey`); tests can substitute a fake.
/// </summary>
public interface IGlobalHotkeyService : IDisposable
{
    int Register(HotkeyBinding binding);
    void Unregister(int id);
    event Action<int>? Triggered;
}

[Flags]
public enum HotkeyModifiers
{
    None = 0,
    Alt = 1,
    Ctrl = 2,
    Shift = 4,
    Win = 8,
}

public readonly record struct HotkeyBinding(HotkeyModifiers Modifiers, int VirtualKey);
