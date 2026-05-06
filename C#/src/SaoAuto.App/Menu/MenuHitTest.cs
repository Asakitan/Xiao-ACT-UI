using SaoAuto.Core.State;

namespace SaoAuto.App.Menu;

/// <summary>
/// Pure hit-testing helpers for the SAO popup menu. Coordinates are in
/// screen pixels; tests can drive the helpers without a live WPF visual
/// tree. Mirrors Python's <c>ui_gpu.menu_hit.*</c> helpers.
/// </summary>
public static class MenuHitTest
{
    public static MenuHitRegion Test(int x, int y, MenuLayout layout)
    {
        if (Contains(layout.LeftWidget, x, y)) return MenuHitRegion.LeftWidget;
        if (Contains(layout.MenuButton, x, y)) return MenuHitRegion.MenuButton;
        for (var i = 0; i < layout.ChildRows.Count; i++)
        {
            if (Contains(layout.ChildRows[i], x, y))
            {
                return new MenuHitRegion(MenuHitKind.ChildRow, i);
            }
        }
        if (Contains(layout.Backdrop, x, y)) return MenuHitRegion.Backdrop;
        return MenuHitRegion.Outside;
    }

    public static bool Contains(RectI rect, int x, int y) =>
        x >= rect.X && y >= rect.Y && x < rect.X + rect.W && y < rect.Y + rect.H;
}

public sealed record MenuLayout(
    RectI LeftWidget,
    RectI MenuButton,
    IReadOnlyList<RectI> ChildRows,
    RectI Backdrop);

public enum MenuHitKind
{
    Outside,
    Backdrop,
    LeftWidget,
    MenuButton,
    ChildRow,
}

public readonly record struct MenuHitRegion(MenuHitKind Kind, int Index = -1)
{
    public static readonly MenuHitRegion Outside = new(MenuHitKind.Outside);
    public static readonly MenuHitRegion Backdrop = new(MenuHitKind.Backdrop);
    public static readonly MenuHitRegion LeftWidget = new(MenuHitKind.LeftWidget);
    public static readonly MenuHitRegion MenuButton = new(MenuHitKind.MenuButton);

    public static implicit operator MenuHitRegion(MenuHitKind kind) => new(kind);
}
