using SaoAuto.Core.State;

namespace SaoAuto.Overlay.Panels;

/// <summary>
/// Pure geometry helpers shared across panel layouts. The concrete pixel
/// positions match Python's `web/hp.html`, `web/boss_hp.html`, `web/dps.html`
/// constants.
/// </summary>
public static class PanelGeometry
{
    /// <summary>HP bar fill rectangle for a given percentage and full-bar size.</summary>
    public static RectI HpBarFill(RectI fullBar, double pct)
    {
        var clamped = Math.Clamp(pct, 0.0, 1.0);
        var fillWidth = (int)Math.Round(fullBar.W * clamped);
        return new RectI(fullBar.X, fullBar.Y, fillWidth, fullBar.H);
    }

    /// <summary>
    /// BossHP bar split into two layers: the main HP rectangle plus an
    /// optional shield overlay on the right edge.
    /// </summary>
    public static (RectI Hp, RectI? Shield) BossHpBarFill(RectI fullBar, double hpPct, double shieldPct)
    {
        var hp = HpBarFill(fullBar, hpPct);
        if (shieldPct <= 0.0001) return (hp, null);
        var shieldWidth = (int)Math.Round(fullBar.W * Math.Clamp(shieldPct, 0.0, 1.0));
        var shield = new RectI(hp.X + hp.W - shieldWidth, hp.Y, shieldWidth, hp.H);
        return (hp, shield);
    }

    /// <summary>Stack DPS rows top-to-bottom with a fixed row height + spacing.</summary>
    public static IReadOnlyList<RectI> StackRows(RectI container, int rowCount, int rowHeight, int spacing)
    {
        if (rowCount <= 0 || rowHeight <= 0) return Array.Empty<RectI>();
        var result = new RectI[rowCount];
        for (var i = 0; i < rowCount; i++)
        {
            var y = container.Y + i * (rowHeight + spacing);
            result[i] = new RectI(container.X, y, container.W, rowHeight);
        }
        return result;
    }
}
