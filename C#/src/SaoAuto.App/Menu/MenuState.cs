namespace SaoAuto.App.Menu;

/// <summary>
/// Lifecycle state of the SAO popup menu. Mirrors Python's
/// `_state` field on the legacy menu controller.
/// </summary>
public enum MenuState
{
    Closed,
    Opening,
    Open,
    Closing,
}

/// <summary>
/// Public API of the SAO popup menu. The menu is a long-lived singleton
/// owned by the entity host; both the entity and webview shells route
/// menu commands through this interface.
/// </summary>
public interface IMenuController
{
    MenuState State { get; }
    bool Visible { get; }
    void Open();
    void Close();
    void Toggle();
    void RefreshChildMenus();
    void RefreshChildMenu(string id);
    event Action<MenuState>? StateChanged;
}
