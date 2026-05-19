using SaoAuto.Overlay;
using Xunit;

namespace SaoAuto.Tests.Overlay;

public class SessionRosterTests
{
    private static SessionRow R(string uid, string name, long fp, bool self, string fpStr = "1.0K")
        => new(uid, name, fp, fpStr, self);

    [Fact]
    public void RowsSignature_skips_null_and_keeps_order()
    {
        var rows = new[] { R("u1", "A", 100, false), null!, R("u2", "B", 200, true) };
        var sig = SessionRoster.RowsSignature(rows);
        Assert.Equal(2, sig.Count);
        Assert.Equal(("u1", "A", 100L, false), sig[0]);
        Assert.Equal(("u2", "B", 200L, true), sig[1]);
    }

    [Fact]
    public void SelfUid_returns_first_self_or_empty()
    {
        Assert.Equal("", SessionRoster.SelfUid(null));
        Assert.Equal("u2", SessionRoster.SelfUid(new[] { R("u1", "A", 1, false), R("u2", "B", 2, true) }));
        Assert.Equal("", SessionRoster.SelfUid(new[] { R("u1", "A", 1, false) }));
    }

    [Theory]
    [InlineData(0, 10, 5, 0)]
    [InlineData(-3, 10, 5, 0)]
    [InlineData(7, 10, 5, 5)]
    [InlineData(100, 10, 5, 5)]
    [InlineData(0, 3, 5, 0)] // total < visible → max_first clamps to 0
    public void ClampFirstIndex_obeys_bounds(int first, int total, int vc, int expected)
        => Assert.Equal(expected, SessionRoster.ClampFirstIndex(first, total, vc));

    [Theory]
    [InlineData(4L, 0L, -3)]
    [InlineData(5L, 0L, 3)]
    [InlineData(0L, 120L, -1)]
    [InlineData(0L, -120L, 1)]
    [InlineData(0L, 240L, -2)]
    [InlineData(0L, 60L, -1)] // small wheel still resolves to ±1
    public void ScrollDelta_handles_x11_and_windows(long num, long raw, int expected)
        => Assert.Equal(expected, SessionRoster.ScrollDelta(num, raw));

    [Fact]
    public void VisibleRows_clamps_window_and_uses_short_name()
    {
        var rows = new[]
        {
            R("u1", "AAAAAAAAAAAAAAAAAAAA", 1, false, "1K"), // 20 chars → trims to 13 + …
            R("u2", "B", 2, true, "2K"),
            R("", "C", 3, false, ""),
        };
        var visible = SessionRoster.VisibleRows(rows, first: 0, visibleCount: 5);
        Assert.Equal(3, visible.Count);
        Assert.EndsWith("\u2026", visible[0].ShortName);
        Assert.True(visible[1].IsSelf);
        Assert.Equal("--", visible[2].Uid);
        Assert.Equal("--", visible[2].FightPower);
    }

    [Fact]
    public void OpenAnimGeometry_t_clamps_and_height_at_least_one()
    {
        var a = SessionRoster.OpenAnimGeometry(panelH: 100, elapsed: 0.0, duration: 1.0);
        Assert.Equal(0.0, a.T);
        Assert.True(a.Height >= 1);
        Assert.True(a.HighlightOn); // t<0.55

        var b = SessionRoster.OpenAnimGeometry(panelH: 100, elapsed: 1.0, duration: 1.0);
        Assert.Equal(1.0, b.T);
        Assert.Equal(100, b.Height);
        Assert.Equal(0, b.Offset);
        Assert.False(b.HighlightOn);

        var c = SessionRoster.OpenAnimGeometry(panelH: 0, elapsed: 0.0, duration: 0.0);
        Assert.Equal(1.0, c.T);
        Assert.Equal(1, c.Height); // clamped
    }

    [Fact]
    public void SaoFxCoords_uses_panel_id_phase_offset()
    {
        var c0 = SessionRoster.SaoFxCoords(0.0, panelId: 0, width: 100);
        var c17 = SessionRoster.SaoFxCoords(0.0, panelId: 17, width: 100);
        // panel_id 0 and 17 → 17 % 17 == 0 → identical coords
        Assert.Equal(c0, c17);
        var c1 = SessionRoster.SaoFxCoords(0.0, panelId: 1, width: 100);
        Assert.NotEqual(c0, c1);
    }

    [Fact]
    public void ScanX_within_padded_range()
    {
        int x = SessionRoster.ScanX(width: 100, now: 0.0);
        Assert.InRange(x, 10, 90);
    }
}

public class CyUiHelpersExtraTests
{
    [Fact]
    public void HpFmtInt_uses_invariant_commas_and_bankers()
    {
        Assert.Equal("1,234", CyUiHelpers.HpFmtInt(1234.0));
        Assert.Equal("0", CyUiHelpers.HpFmtInt(0.5));   // banker's: 0.5 → 0
        Assert.Equal("2", CyUiHelpers.HpFmtInt(1.5));   // banker's: 1.5 → 2
        Assert.Equal("1,000,000", CyUiHelpers.HpFmtInt(1_000_000.0));
    }

    [Fact]
    public void HpStageScreenX_combines_window_and_stage_offsets()
    {
        int x = CyUiHelpers.HpStageScreenX(screenW: 1920, windowLeftPct: 0.1, hudVwPct: 0.5, stageLeftPct: 0.2);
        // 1920 * 0.1 + 1920 * 0.5 * 0.2 = 192 + 192 = 384
        Assert.Equal(384, x);
    }

    [Fact]
    public void HpLayoutMetrics_basic_shape()
    {
        var lay = CyUiHelpers.HpLayoutMetrics(
            screenW: 1920, hudVwPct: 0.4, stageWidthPct: 0.5, shadowGutter: 12.0,
            coverW: 100, boxW: 80, staW: 60);
        Assert.True(lay.StageW > 0);
        Assert.True(lay.PanelW > 0);
        Assert.True(lay.IdW > 0);
        Assert.Equal(lay.BoxX, lay.StaX); // Python: sta_x = box_x
    }

    [Theory]
    [InlineData(0.0, 0)]
    [InlineData(0.5, 50)]
    [InlineData(1.0, 100)]
    [InlineData(-0.5, 0)]   // clamps
    [InlineData(1.5, 100)]  // clamps
    public void MixRgba_clamps_and_lerps_each_channel(double t, int expectedR)
    {
        var (r, g, b, a) = CyUiHelpers.MixRgba((0, 0, 0, 0), (100, 100, 100, 100), t);
        Assert.Equal(expectedR, r);
        Assert.Equal(expectedR, g);
        Assert.Equal(expectedR, b);
        Assert.Equal(expectedR, a);
    }

    [Fact]
    public void OffsetPoly_translates_each_point()
    {
        var pts = new[] { (1, 2), (3, 4), (-1, -2) };
        var moved = CyUiHelpers.OffsetPoly(pts, dx: 10, dy: 20);
        Assert.Equal(3, moved.Count);
        Assert.Equal((11, 22), moved[0]);
        Assert.Equal((13, 24), moved[1]);
        Assert.Equal((9, 18), moved[2]);
    }

    private sealed record Mob(long Hp, long MaxHp, double LastTs);

    [Fact]
    public void SortRecentMonsters_orders_by_hp_pct_desc_then_ts_desc()
    {
        var mobs = new[]
        {
            new Mob(50, 100, 5.0),   // pct=0.5  ts=5
            new Mob(80, 100, 1.0),   // pct=0.8  ts=1
            new Mob(50, 100, 9.0),   // pct=0.5  ts=9 (newer → first among 0.5s)
        };
        var sorted = CyUiHelpers.SortRecentMonsters(mobs,
            m => m.Hp, m => m.MaxHp, m => m.LastTs);
        Assert.Equal(80, sorted[0].Hp);
        Assert.Equal(9.0, sorted[1].LastTs); // newer 0.5 first
        Assert.Equal(5.0, sorted[2].LastTs);
    }

    [Fact]
    public void SortRecentMonsters_handles_zero_max_hp()
    {
        var mobs = new[] { new Mob(0, 0, 0), new Mob(10, 0, 0) };
        var sorted = CyUiHelpers.SortRecentMonsters(mobs,
            m => m.Hp, m => m.MaxHp, m => m.LastTs);
        // hp=10 maxHp=0 → maxHp=hp=10 → pct=1.0 wins
        Assert.Equal(10, sorted[0].Hp);
    }

    [Fact]
    public void DpsRoundHalfUpInt_matches_helper()
    {
        Assert.Equal(0L, CyUiHelpers.DpsRoundHalfUpInt(-1.0));
        Assert.Equal(1L, CyUiHelpers.DpsRoundHalfUpInt(0.5));
        Assert.Equal(2L, CyUiHelpers.DpsRoundHalfUpInt(1.5));
        Assert.Equal(0L, CyUiHelpers.DpsRoundHalfUpInt(0.0));
    }
}
