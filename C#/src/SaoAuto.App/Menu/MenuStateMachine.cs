namespace SaoAuto.App.Menu;

/// <summary>
/// Pure menu lifecycle state machine. The entity host wires this to the WPF
/// open / close animations; the webview bridge wires this to the JS animation
/// hooks. Logic here is shared so transitions stay coherent across the two
/// shells.
/// </summary>
public sealed class MenuStateMachine : IMenuController
{
    private readonly object _gate = new();
    private MenuState _state = MenuState.Closed;

    public event Action<MenuState>? StateChanged;
    public event Action? OpenAnimationRequested;
    public event Action? CloseAnimationRequested;

    public MenuState State
    {
        get { lock (_gate) { return _state; } }
    }

    public bool Visible => State is MenuState.Open or MenuState.Opening;

    public void Open()
    {
        Action? raise = null;
        bool changed = false;
        lock (_gate)
        {
            if (_state is MenuState.Open or MenuState.Opening) return;
            _state = MenuState.Opening;
            changed = true;
            raise = () => OpenAnimationRequested?.Invoke();
        }
        if (changed) RaiseStateChanged();
        raise?.Invoke();
    }

    public void Close()
    {
        Action? raise = null;
        bool changed = false;
        lock (_gate)
        {
            if (_state is MenuState.Closed or MenuState.Closing) return;
            _state = MenuState.Closing;
            changed = true;
            raise = () => CloseAnimationRequested?.Invoke();
        }
        if (changed) RaiseStateChanged();
        raise?.Invoke();
    }

    public void Toggle()
    {
        if (State is MenuState.Closed or MenuState.Closing) Open();
        else Close();
    }

    /// <summary>Called by the animation driver when an opening transition completes.</summary>
    public void OnOpenAnimationCompleted()
    {
        bool changed = false;
        lock (_gate)
        {
            if (_state == MenuState.Opening) { _state = MenuState.Open; changed = true; }
        }
        if (changed) RaiseStateChanged();
    }

    /// <summary>Called by the animation driver when a closing transition completes.</summary>
    public void OnCloseAnimationCompleted()
    {
        bool changed = false;
        lock (_gate)
        {
            if (_state == MenuState.Closing) { _state = MenuState.Closed; changed = true; }
        }
        if (changed) RaiseStateChanged();
    }

    public void RefreshChildMenus() { /* shell-specific; no-op in pure state machine */ }
    public void RefreshChildMenu(string id) { _ = id; /* same */ }

    public void Reset()
    {
        bool changed = false;
        lock (_gate)
        {
            if (_state != MenuState.Closed) { _state = MenuState.Closed; changed = true; }
        }
        if (changed) RaiseStateChanged();
    }

    private void RaiseStateChanged()
    {
        try { StateChanged?.Invoke(State); }
        catch { /* swallow */ }
    }
}
