using SaoAuto.Core.Automation;

namespace SaoAuto.App.Startup;

/// <summary>
/// S137 — App-layer adapter exposing <see cref="Win32SendInput"/> as
/// an <see cref="IKeyDispatcher"/>. Lives in App so Core stays
/// Win32-free at the dispatcher boundary too (the Win32 call itself
/// is in Core already, but the contract glue belongs with the App
/// wiring).
/// </summary>
public sealed class SendInputKeyDispatcher : IKeyDispatcher
{
    private readonly Win32SendInput _sink;

    public SendInputKeyDispatcher(Win32SendInput? sink = null)
    {
        _sink = sink ?? new Win32SendInput();
    }

    public void Dispatch(KeyStroke stroke)
    {
        if (stroke is null) return;
        _sink.PressKey(stroke.VirtualKey, stroke.Modifiers, stroke.HoldMs);
    }
}
