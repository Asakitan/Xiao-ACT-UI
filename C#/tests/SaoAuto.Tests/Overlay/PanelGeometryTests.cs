using SaoAuto.Core.State;
using SaoAuto.Overlay.Panels;

namespace SaoAuto.Tests.Overlay;

public class PanelGeometryTests
{
    [Fact]
    public void HpBarFillScalesWithPercentage()
    {
        var bar = new RectI(0, 0, 100, 10);
        Assert.Equal(0, PanelGeometry.HpBarFill(bar, 0.0).W);
        Assert.Equal(50, PanelGeometry.HpBarFill(bar, 0.5).W);
        Assert.Equal(100, PanelGeometry.HpBarFill(bar, 1.0).W);
        Assert.Equal(100, PanelGeometry.HpBarFill(bar, 1.5).W); // clamped
        Assert.Equal(0, PanelGeometry.HpBarFill(bar, -0.1).W); // clamped
    }

    [Fact]
    public void BossHpBarFillAddsShieldRightAligned()
    {
        var bar = new RectI(100, 50, 200, 16);
        var (hp, shield) = PanelGeometry.BossHpBarFill(bar, hpPct: 0.5, shieldPct: 0.25);
        Assert.Equal(100, hp.W);
        Assert.NotNull(shield);
        Assert.Equal(50, shield!.Value.W);
        // Shield abuts the right edge of the HP fill.
        Assert.Equal(hp.X + hp.W - shield.Value.W, shield.Value.X);
    }

    [Fact]
    public void NoShieldWhenShieldPctZero()
    {
        var bar = new RectI(0, 0, 100, 10);
        var (_, shield) = PanelGeometry.BossHpBarFill(bar, 0.5, 0);
        Assert.Null(shield);
    }

    [Fact]
    public void StackRowsLaysOutTopToBottom()
    {
        var container = new RectI(10, 20, 200, 100);
        var rows = PanelGeometry.StackRows(container, rowCount: 3, rowHeight: 20, spacing: 4);
        Assert.Equal(3, rows.Count);
        Assert.Equal(new RectI(10, 20, 200, 20), rows[0]);
        Assert.Equal(new RectI(10, 44, 200, 20), rows[1]);
        Assert.Equal(new RectI(10, 68, 200, 20), rows[2]);
    }
}
