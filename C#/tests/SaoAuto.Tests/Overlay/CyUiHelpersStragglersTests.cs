using SaoAuto.Overlay;

namespace SaoAuto.Tests.Overlay;

public class CyUiHelpersStragglersTests
{
    // ── boss_monster_usable ──────────────────────────────────────────

    [Theory]
    [InlineData(100, 200, false, true, false)]   // alive, hp+max>0
    [InlineData(0, 200, false, true, false)]     // alive, max>0 only
    [InlineData(50, 0, false, true, false)]      // alive, hp>0 only
    [InlineData(0, 0, false, false, false)]      // alive, both zero → unusable
    [InlineData(0, 200, true, false, false)]     // dead, hp=0 → unusable, no revive
    [InlineData(50, 200, true, true, true)]      // dead but hp>0 → revive + usable
    public void BossMonsterUsable_priority_chain(long hp, long max, bool isDead, bool usable, bool revive)
    {
        var (u, r) = CyUiHelpers.BossMonsterUsable(hp, max, isDead);
        Assert.Equal(usable, u);
        Assert.Equal(revive, r);
    }

    // ── player_panel_anim_size ──────────────────────────────────────

    [Theory]
    [InlineData(100, 50, 1.0, 100, 50)]
    [InlineData(100, 50, 0.5, 50, 25)]
    [InlineData(100, 50, 0.0, 1, 1)]    // clamp at 1px
    [InlineData(2, 2, 0.4, 1, 1)]       // 0.8 truncated → 0, then clamped to 1
    public void PlayerPanelAnimSize_clamps_to_one(int w, int h, double t, int eW, int eH)
    {
        var (rW, rH) = CyUiHelpers.PlayerPanelAnimSize(w, h, t);
        Assert.Equal(eW, rW);
        Assert.Equal(eH, rH);
    }

    // ── popup_max_child_rows ────────────────────────────────────────

    [Fact]
    public void PopupMaxChildRows_finds_largest_row_count()
    {
        IReadOnlyList<int> a = new[] { 1, 2 };
        IReadOnlyList<int> b = new[] { 1, 2, 3, 4 };
        IReadOnlyList<int> c = new[] { 1 };
        var dict = new Dictionary<string, IReadOnlyList<int>>
        {
            ["a"] = a, ["b"] = b, ["c"] = c,
        };
        Assert.Equal(4, CyUiHelpers.PopupMaxChildRows<string, int>(dict));
    }

    [Fact]
    public void PopupMaxChildRows_null_returns_zero()
    {
        Assert.Equal(0, CyUiHelpers.PopupMaxChildRows<string, int>(null));
    }

    [Fact]
    public void PopupMaxChildRows_empty_dict_returns_zero()
    {
        Assert.Equal(0, CyUiHelpers.PopupMaxChildRows<string, int>(
            new Dictionary<string, IReadOnlyList<int>>()));
    }

    // ── menu_bar_slot_index ─────────────────────────────────────────

    [Fact]
    public void MenuBarSlotIndex_in_range_returns_index()
    {
        Assert.Equal(2, CyUiHelpers.MenuBarSlotIndex(x: 10.0, y: 100.0, maxSize: 50, slot: 40, buttonCount: 5));
    }

    [Fact]
    public void MenuBarSlotIndex_out_of_x_range_returns_null()
    {
        Assert.Null(CyUiHelpers.MenuBarSlotIndex(x: 50.0, y: 0.0, maxSize: 50, slot: 40, buttonCount: 5));
        Assert.Null(CyUiHelpers.MenuBarSlotIndex(x: -1.0, y: 0.0, maxSize: 50, slot: 40, buttonCount: 5));
    }

    [Fact]
    public void MenuBarSlotIndex_y_past_last_button_returns_null()
    {
        Assert.Null(CyUiHelpers.MenuBarSlotIndex(x: 10.0, y: 200.0, maxSize: 50, slot: 40, buttonCount: 5));
    }

    [Fact]
    public void MenuBarSlotIndex_zero_buttons_or_slot_returns_null()
    {
        Assert.Null(CyUiHelpers.MenuBarSlotIndex(0, 0, 50, 40, 0));
        Assert.Null(CyUiHelpers.MenuBarSlotIndex(0, 0, 50, 0, 5));
    }

    // ── menu_bar_snapshot_sig ───────────────────────────────────────

    [Fact]
    public void MenuBarSnapshotSig_quantizes_size_and_hover()
    {
        // size=0.37 → 0.37*4+0.5 = 1.98 → 1 → 0.25
        // hover=0.123 → 0.123*20+0.5 = 2.96 → 2 → 0.10
        var snaps = new[]
        {
            new CyUiHelpers.MenuBarSnapshot(Size: 0.37, HoverT: 0.123, Active: true, Icon: "a"),
        };
        var sig = CyUiHelpers.MenuBarSnapshotSig(100, 40, snaps);
        Assert.Equal(0.25, sig.Buttons[0].SizeQ);
        Assert.Equal(0.10, sig.Buttons[0].HoverQ);
        Assert.True(sig.Buttons[0].Active);
        Assert.Equal("a", sig.Buttons[0].Icon);
        Assert.Equal(1, sig.Count);
    }

    [Fact]
    public void MenuBarSnapshotSig_null_snapshots_returns_empty_sig()
    {
        var sig = CyUiHelpers.MenuBarSnapshotSig(100, 40, null);
        Assert.Equal(0, sig.Count);
        Assert.Empty(sig.Buttons);
        Assert.Equal(100, sig.StripW);
    }

    // ── build_batch_sig ─────────────────────────────────────────────

    [Fact]
    public void BuildBatchSig_sorts_lex_by_tuple_order()
    {
        var batch = new[]
        {
            new CyUiHelpers.RosterBatchRow("u2", "B", "Knight", 100, 60),
            new CyUiHelpers.RosterBatchRow("u1", "A", "Mage", 200, 70),
        };
        var sig = CyUiHelpers.BuildBatchSig(batch);
        Assert.Equal("u1", sig[0].Uid);
        Assert.Equal("u2", sig[1].Uid);
    }

    [Fact]
    public void BuildBatchSig_empty_returns_empty()
    {
        Assert.Empty(CyUiHelpers.BuildBatchSig(null));
        Assert.Empty(CyUiHelpers.BuildBatchSig(Array.Empty<CyUiHelpers.RosterBatchRow>()));
    }

    [Fact]
    public void BuildBatchSig_value_equality_for_dedup()
    {
        var a = CyUiHelpers.BuildBatchSig(new[]
        {
            new CyUiHelpers.RosterBatchRow("u1", "A", "X", 1, 1),
        });
        var b = CyUiHelpers.BuildBatchSig(new[]
        {
            new CyUiHelpers.RosterBatchRow("u1", "A", "X", 1, 1),
        });
        Assert.Equal(a[0], b[0]); // record value equality
    }

    // ── build_boss_bar_sig ──────────────────────────────────────────

    [Fact]
    public void BuildBossBarSig_rounds_pct_to_three_decimals()
    {
        var data = new CyUiHelpers.BossBarSigInput(
            Active: true, HpPct: 0.4567, HpSource: "tcp",
            CurrentHp: 100, TotalHp: 200,
            ShieldActive: false, ShieldPct: 0.0,
            BreakingStage: 0, HasBreakData: false,
            ExtinctionPct: 0.0, Extinction: 0, MaxExtinction: 0,
            StopBreakingTicking: false, InOverdrive: false, Invincible: false,
            BossName: "Boss");
        var add = new[]
        {
            new CyUiHelpers.BossBarAdditionalInput(
                Name: "Add", HpPct: 0.66666, ExtinctionPct: 0.12345,
                HasBreakData: true, BreakingStage: 2,
                ShieldActive: true, ShieldPct: 0.7777),
        };
        var sig = CyUiHelpers.BuildBossBarSig(data, add);
        Assert.Equal(0.667, sig.Additional[0].HpPct);
        Assert.Equal(0.123, sig.Additional[0].ExtinctionPct);
        Assert.Equal(0.778, sig.Additional[0].ShieldPct);
        Assert.Equal(0.4567, sig.HpPct);            // top-level NOT rounded (Python parity)
        Assert.Equal("Boss", sig.BossName);
    }

    [Fact]
    public void BuildBossBarSig_null_additional_returns_empty_list()
    {
        var data = new CyUiHelpers.BossBarSigInput(
            true, 0.5, "tcp", 1, 1, false, 0, 0, false, 0, 0, 0,
            false, false, false, "Boss");
        var sig = CyUiHelpers.BuildBossBarSig(data, null);
        Assert.Empty(sig.Additional);
    }

    [Fact]
    public void BuildBossBarSig_field_equality_for_dedup()
    {
        var d1 = new CyUiHelpers.BossBarSigInput(
            true, 0.5, "tcp", 1, 1, false, 0, 0, false, 0, 0, 0,
            false, false, false, "Boss");
        var d2 = new CyUiHelpers.BossBarSigInput(
            true, 0.5, "tcp", 1, 1, false, 0, 0, false, 0, 0, 0,
            false, false, false, "Boss");
        var s1 = CyUiHelpers.BuildBossBarSig(d1, null);
        var s2 = CyUiHelpers.BuildBossBarSig(d2, null);
        // List instances differ by reference; compare scalar fields and additional length.
        Assert.Equal(s1.Active, s2.Active);
        Assert.Equal(s1.HpPct, s2.HpPct);
        Assert.Equal(s1.HpSource, s2.HpSource);
        Assert.Equal(s1.BossName, s2.BossName);
        Assert.Equal(s1.Additional.Count, s2.Additional.Count);
    }
}
