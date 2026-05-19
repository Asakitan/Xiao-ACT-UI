using SaoAuto.Overlay;

namespace SaoAuto.Tests.Overlay;

public class PopupGeometryTests
{
    [Theory]
    [InlineData(0, 7, 0)]
    [InlineData(3, 7, 3)]
    [InlineData(10, 7, 7)]
    [InlineData(-2, 7, 0)]
    [InlineData(5, -1, 0)]
    public void VisibleCountClampsAndCaps(int menu, int max, int expected)
        => Assert.Equal(expected, PopupGeometry.VisibleCount(menu, max));

    [Theory]
    [InlineData(0, 7, 70, 70)]   // visible=0 → forced to 1 → 70
    [InlineData(3, 7, 70, 210)]
    [InlineData(10, 7, 70, 490)]
    public void ColumnHeightUsesMinOneVisible(int menu, int max, int slot, int expected)
        => Assert.Equal(expected, PopupGeometry.ColumnHeight(menu, max, slot));

    [Fact]
    public void ContentShiftAtFullAlphaIsZero()
    {
        var (dx, dy) = PopupGeometry.ContentShift(1.0);
        Assert.Equal(0, dx);
        Assert.Equal(0, dy);
    }

    [Fact]
    public void ContentShiftAtZeroAlphaPullsLeftAndDown()
    {
        var (dx, dy) = PopupGeometry.ContentShift(0.0);
        // -((1.0)*14 + 0.5) → -14
        Assert.Equal(-14, dx);
        // ((1.0)*10 + 0.5) → 10
        Assert.Equal(10, dy);
    }

    [Fact]
    public void OriginsAppliesShiftToBothColumns()
    {
        var (menu, child) = PopupGeometry.Origins(0.5, hudPad: 24, menuX: 24, childX: 119);
        // shift at alpha=0.5: dx = -((0.5)*14+0.5)= -7, dy = ((0.5)*10+0.5)= 5
        Assert.Equal((24 - 7, 24 + 5), menu);
        Assert.Equal((119 - 7, 24 + 5), child);
    }

    [Fact]
    public void ContentSizeUsesLargerOfMenuAndChildHeight()
    {
        // menuCount=3, max=7, slot=70 → menu_h=210
        // childCount=2, rowStride=47 → child_h=94 → inner_h=210
        var (w, h) = PopupGeometry.ContentSize(3, 2, 7, 70, 47, 70, 25, 267);
        Assert.Equal(70 + 25 + 267, w);
        Assert.Equal(210, h);
    }

    [Fact]
    public void ContentSizeMinimumOne()
    {
        var (_, h) = PopupGeometry.ContentSize(0, 0, 0, 0, 47, 0, 0, 0);
        Assert.Equal(1, h);
    }

    [Fact]
    public void WindowSizeAddsPadAndRespectsReservedAndMenuFloor()
    {
        // baseline content (3,2,7,70,...) → (362, 210)
        // reservedRows=6 * 47 = 282 → ih = 282
        // pad=24 each side
        var (w, h) = PopupGeometry.WindowSize(3, 2, 6, 7, 70, 47, 70, 25, 267, 24);
        Assert.Equal(362 + 48, w);
        Assert.Equal(282 + 48, h);
    }

    [Fact]
    public void MenuButtonFrameClampsAndRoundsUp()
    {
        var (sizeF, sizePx, ox, oy) = PopupGeometry.MenuButtonFrame(54.5, 70.0, 70.0, 2);
        Assert.Equal(54.5, sizeF);
        Assert.Equal(55, sizePx);
        // ox = round((70-54.5)/2) = round(7.75) = 8
        Assert.Equal(8, ox);
        // oy = round(2*70 + 7.75) = round(147.75) = 148
        Assert.Equal(148, oy);
    }

    [Fact]
    public void MenuButtonFrameUpperBoundClampsToMaxSize()
    {
        var (sizeF, sizePx, _, _) = PopupGeometry.MenuButtonFrame(99.0, 70.0, 70.0, 0);
        Assert.Equal(70.0, sizeF);
        Assert.Equal(70, sizePx);
    }

    [Fact]
    public void MenuHitRectsTilesVerticallyByVisibleCount()
    {
        var rects = PopupGeometry.MenuHitRects(menuCount: 3, maxVisible: 7, slot: 70, xOff: 10, yOff: 20);
        Assert.Equal(3, rects.Count);
        Assert.Equal(new PopupGeometry.HitRect(10, 20,    80,  90,  0), rects[0]);
        Assert.Equal(new PopupGeometry.HitRect(10, 90,    80, 160,  1), rects[1]);
        Assert.Equal(new PopupGeometry.HitRect(10, 160,   80, 230,  2), rects[2]);
    }

    [Fact]
    public void ChildHeightZeroForEmpty()
    {
        Assert.Equal(0, PopupGeometry.ChildHeight(0, 47));
        Assert.Equal(141, PopupGeometry.ChildHeight(3, 47));
    }

    [Fact]
    public void ChildHitRectsUseAnimWWhenAvailableElseTarget()
    {
        var anim = new[] { 100, 200 };
        var rects = PopupGeometry.ChildHitRects(
            childCount: 3, rowAnimW: anim,
            xOff: 5, yOff: 10, listX: 27,
            rowStride: 47, rowH: 36, targetRowW: 240);

        Assert.Equal(3, rects.Count);
        Assert.Equal(new PopupGeometry.HitRect(32, 10,  132, 46, 0), rects[0]);
        Assert.Equal(new PopupGeometry.HitRect(32, 57,  232, 93, 1), rects[1]);
        // 3rd row falls back to targetRowW=240
        Assert.Equal(new PopupGeometry.HitRect(32, 104, 272, 140, 2), rects[2]);
    }

    [Fact]
    public void ChildHitRectsClampZeroWidthToOne()
    {
        var rects = PopupGeometry.ChildHitRects(1, new[] { 0 }, 0, 0, 0, 47, 36, 100);
        Assert.Equal(0 + 1, rects[0].X2);
    }

    [Fact]
    public void PickHitMatchesRegionThenBackgroundThenNull()
    {
        var rect = new PopupGeometry.HitRect(0, 0, 50, 50, 7);
        var regions = new List<(PopupGeometry.HitRect, string, int)>
        {
            (rect, "menu", 7),
        };
        Assert.Equal(("menu", 7), PopupGeometry.PickHit(regions, (0, 0, 100, 100), 10, 10));
        Assert.Equal(("background", -1), PopupGeometry.PickHit(regions, (0, 0, 100, 100), 60, 60));
        Assert.Null(PopupGeometry.PickHit(regions, (0, 0, 100, 100), 200, 200));
    }

    [Fact]
    public void HudDynamicFrameProducesScanRangeAndDotTravel()
    {
        var f = PopupGeometry.HudDynamicFrame(260, 180, phase: 0.0,
            plateePad: 16, hudMargin: 6, bracketLen: 16);
        Assert.Equal(10, f.Cx1);
        Assert.Equal(10, f.Cy1);
        Assert.Equal(282, f.Cx2);
        Assert.Equal(202, f.Cy2);
        // scan_pos = 0 → scanY = cy1
        Assert.Equal(10, f.ScanY);
        // dot left/right at phase=0: sin(0)=0 → (0+1)/2=0.5; sin(π)=0 → 0.5
        // dotTravel = 202-10-32 = 160 → +80
        Assert.Equal(10 + 16 + 80, f.DotYL);
        Assert.Equal(10 + 16 + 80, f.DotYR);
    }

    [Fact]
    public void TickDtFirstTickReturns60thAndClamps()
    {
        var (dt, now) = PopupGeometry.TickDt(123.456, lastTickT: 0.0);
        Assert.Equal(1.0 / 60.0, dt, 6);
        Assert.Equal(123.456, now);

        // Big jump → clamp to 0.10
        (dt, _) = PopupGeometry.TickDt(124.0, lastTickT: 100.0);
        Assert.Equal(0.10, dt, 6);

        // Negative → clamp to 0
        (dt, _) = PopupGeometry.TickDt(50.0, lastTickT: 100.0);
        Assert.Equal(0.0, dt);
    }

    [Fact]
    public void FadeAlphaInChoosesDefaultDuration()
    {
        var (alpha, done, t) = PopupGeometry.FadeAlpha(
            tickNow: 0.225, fadeT0: 0.0, fadeDuration: 0.0, fadeTarget: 1.0);
        // dur = 0.45 (fade-in default), t = 0.5
        Assert.Equal(0.5, t, 6);
        Assert.Equal(0.5, alpha, 6);
        Assert.False(done);
    }

    [Fact]
    public void FadeAlphaOutInvertsAndCompletes()
    {
        var (alpha, done, t) = PopupGeometry.FadeAlpha(
            tickNow: 1.0, fadeT0: 0.0, fadeDuration: 0.30, fadeTarget: 0.0);
        Assert.Equal(1.0, t);
        Assert.Equal(0.0, alpha);
        Assert.True(done);
    }

    [Fact]
    public void ChildPhaseStepFadeoutAdvancesAndCompletes()
    {
        var (ph, t, done) = PopupGeometry.ChildPhaseStep("fadeout", fadeT: 0.95, dt: 0.05);
        Assert.Equal("fadeout", ph);
        Assert.Equal(1.0, t);
        Assert.True(done);
    }

    [Fact]
    public void ChildPhaseStepFadeinTransitionsToIdle()
    {
        var (ph, t, done) = PopupGeometry.ChildPhaseStep("fadein", fadeT: 0.0005, dt: 0.0);
        // fade_t -= 0/0.22 = 0 → ft stays 0.0005 → > 0.001 false → completed
        // Actually: ft = 0.0005, then 0.0005 -= 0 → 0.0005; 0.0005 <= 0.001 → completed
        Assert.Equal("idle", ph);
        Assert.Equal(0.0, t);
        Assert.True(done);
    }

    [Fact]
    public void ChildPhaseStepIdleIsNoOp()
    {
        var (ph, t, done) = PopupGeometry.ChildPhaseStep("idle", fadeT: 0.5, dt: 0.1);
        Assert.Equal("idle", ph);
        Assert.Equal(0.5, t);
        Assert.False(done);
    }

    [Fact]
    public void MaxChildRowsReturnsLargestOrZero()
    {
        Assert.Equal(0, PopupGeometry.MaxChildRows<int>(null));
        var menus = new IReadOnlyCollection<int>[]
        {
            new[] { 1, 2 },
            new[] { 1, 2, 3, 4 },
            new[] { 9 },
        };
        Assert.Equal(4, PopupGeometry.MaxChildRows(menus));
    }
}
