using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Menu;
using SaoAuto.App.Startup;

namespace SaoAuto.App.Hotkeys;

/// <summary>
/// S160 — Binds a single <see cref="HotkeyBinding"/> to
/// <see cref="HideSeekLifecycle.ToggleSuspend"/>. Encapsulates the
/// register/listen/unregister dance so the runner site stays one
/// line. Idempotent dispose. Trigger callbacks for *other* hotkey
/// ids are ignored.
///
/// Default binding is <c>Ctrl+Alt+H</c> (VK_H = 0x48) — pick lands
/// in the runner site; this class doesn't hard-code it.
/// </summary>
public sealed class HideSeekHotkeyBinding : IDisposable
{
    private readonly IGlobalHotkeyService _hotkeys;
    private readonly HideSeekLifecycle _lifecycle;
    private readonly ILogger _log;
    private readonly Action<int> _onTriggered;
    private readonly int _id;
    private bool _disposed;

    /// <summary>Fires after every toggle with the new suspended state.</summary>
    public event Action<bool>? Toggled;

    public HideSeekHotkeyBinding(
        IGlobalHotkeyService hotkeys,
        HideSeekLifecycle lifecycle,
        HotkeyBinding binding,
        ILogger<HideSeekHotkeyBinding>? logger = null)
    {
        _hotkeys = hotkeys ?? throw new ArgumentNullException(nameof(hotkeys));
        _lifecycle = lifecycle ?? throw new ArgumentNullException(nameof(lifecycle));
        _log = (ILogger?)logger ?? NullLogger.Instance;
        _id = _hotkeys.Register(binding);
        _onTriggered = OnTriggered;
        _hotkeys.Triggered += _onTriggered;
    }

    private void OnTriggered(int id)
    {
        if (id != _id) return;
        bool nowSuspended;
        try { nowSuspended = _lifecycle.ToggleSuspend(); }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[HideSeekHotkeyBinding] toggle threw");
            return;
        }
        _log.LogInformation("HideSeek hotkey toggled; suspended={Suspended}", nowSuspended);
        try { Toggled?.Invoke(nowSuspended); }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[HideSeekHotkeyBinding] Toggled subscriber threw");
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _hotkeys.Triggered -= _onTriggered;
        try { _hotkeys.Unregister(_id); } catch { /* swallow on shutdown */ }
    }
}
