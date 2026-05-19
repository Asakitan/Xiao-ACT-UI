using SaoAuto.Overlay;

namespace SaoAuto.Tests.Overlay;

public class SkillFxLayoutTests
{
    private static SkillFxClientRect Client1080() =>
        new(Left: 0, Top: 0, Right: 1920, Bottom: 1080);

    [Fact]
    public void Returns_null_when_client_rect_is_null()
    {
        var result = CyUiHelpers.ComputeSkillFxLayout(null,
            new[] { new SkillFxInputSlot(1, 800, 900, 50, 50) });
        Assert.Null(result);
    }

    [Fact]
    public void Returns_null_when_no_slots_and_no_fallback()
    {
        var result = CyUiHelpers.ComputeSkillFxLayout(Client1080(), null);
        Assert.Null(result);
    }

    [Fact]
    public void Returns_null_when_slots_filtered_out_and_no_fallback()
    {
        // Index <= 0 OR w/h <= 0 → filtered.
        var bad = new[]
        {
            new SkillFxInputSlot(0, 100, 100, 50, 50),
            new SkillFxInputSlot(2, 100, 100, 0, 50),
            new SkillFxInputSlot(3, 100, 100, 50, -1),
        };
        Assert.Null(CyUiHelpers.ComputeSkillFxLayout(Client1080(), bad));
    }

    [Fact]
    public void Falls_back_when_primary_empty()
    {
        var result = CyUiHelpers.ComputeSkillFxLayout(
            Client1080(),
            slotRects: Array.Empty<SkillFxInputSlot>(),
            fallbackSlotRects: new[] { new SkillFxInputSlot(1, 800, 900, 50, 50) });
        Assert.NotNull(result);
        Assert.Single(result!.Slots);
        Assert.Equal(1, result.Slots[0].Index);
    }

    [Fact]
    public void Window_size_meets_minimum_floors()
    {
        // A tiny single slot should still produce width >= 420, height >= 220.
        var result = CyUiHelpers.ComputeSkillFxLayout(
            new SkillFxClientRect(0, 0, 200, 200),
            new[] { new SkillFxInputSlot(1, 50, 50, 30, 30) });
        Assert.NotNull(result);
        Assert.True(result!.Window.W >= 420);
        Assert.True(result.Window.H >= 220);
    }

    [Fact]
    public void Window_x_clamped_to_zero_when_pad_left_exceeds_min_x()
    {
        // min_x = 10, pad_left = max(96, round(1920*0.055)=106) = 106 → 10 - 106 = -96 → clamp 0.
        var result = CyUiHelpers.ComputeSkillFxLayout(
            Client1080(),
            new[] { new SkillFxInputSlot(1, 10, 100, 50, 50) });
        Assert.NotNull(result);
        Assert.Equal(0, result!.Window.X);
    }

    [Fact]
    public void Window_y_uses_client_top_clamped_at_zero()
    {
        // client_top = -50 → clamp to 0.
        var result = CyUiHelpers.ComputeSkillFxLayout(
            new SkillFxClientRect(0, -50, 1920, 1030),
            new[] { new SkillFxInputSlot(1, 800, 100, 50, 50) });
        Assert.NotNull(result);
        Assert.Equal(0, result!.Window.Y);
    }

    [Fact]
    public void Padding_x_final_is_max_of_three_paddings()
    {
        // For 1920x1080: pad_x = max(18, round(23.04)) = 23, pad_left = max(96, 106) = 106,
        // pad_right = max(84, round(84.48)) = 85. Final = 106.
        var result = CyUiHelpers.ComputeSkillFxLayout(
            Client1080(),
            new[] { new SkillFxInputSlot(1, 800, 900, 50, 50) });
        Assert.NotNull(result);
        Assert.Equal(106, result!.Viewport.PaddingX);
        Assert.Equal(18, result.Viewport.PaddingY);  // max(18, round(17.28)=17) = 18
    }

    [Fact]
    public void Callout_size_uses_proportional_minimums()
    {
        // 1920x1080 → callout_w = max(440, round(556.8)=557) = 557, callout_h = max(128, round(124.2)=124) = 128.
        var result = CyUiHelpers.ComputeSkillFxLayout(
            Client1080(),
            new[] { new SkillFxInputSlot(1, 800, 900, 50, 50) });
        Assert.NotNull(result);
        Assert.Equal(557, result!.Viewport.Callout.W);
        Assert.Equal(128, result.Viewport.Callout.H);
    }

    [Fact]
    public void Callout_y_equals_callout_margin_y()
    {
        // 1920x1080 → callout_margin_y = max(24, round(43.2)=43) = 43.
        var result = CyUiHelpers.ComputeSkillFxLayout(
            Client1080(),
            new[] { new SkillFxInputSlot(1, 800, 900, 50, 50) });
        Assert.NotNull(result);
        Assert.Equal(43, result!.Viewport.Callout.Y);
    }

    [Fact]
    public void Callout_x_clamped_to_callout_margin_x_when_negative()
    {
        // Force tiny window where width - callout_w - margin would be < margin.
        // Use a client where width derives small. 600x400 client, single slot at (10, 50, 30, 30).
        // pad_left = max(96, round(33)) = 96 → win_x = 0; width = 600 - 0 + max(84, round(26.4)=26)=84 = 684.
        // callout_w = max(440, round(174)=174)=440, callout_margin_x = max(28, round(13.2)=13)=28.
        // callout_x raw = 684 - 440 - 28 = 216 ≥ 28 → not clamped here.
        // Make it tiny: client 200x200, single slot.
        var result = CyUiHelpers.ComputeSkillFxLayout(
            new SkillFxClientRect(0, 0, 200, 200),
            new[] { new SkillFxInputSlot(1, 50, 50, 30, 30) });
        Assert.NotNull(result);
        // width floors to 420, callout_w=max(440,..)=440, callout_margin_x=max(28,..)=28.
        // raw = 420 - 440 - 28 = -48 < 28 → clamp to 28.
        Assert.Equal(28, result!.Viewport.Callout.X);
    }

    [Fact]
    public void Slots_translated_into_window_local_coords()
    {
        // pad_left = 106 for 1920w. Slot at x=800,y=100 → win_x=0 (800-106 then clamp not needed: 694).
        // Actually min_x=800, win_x=800-106=694. local x = 800-694 = 106. local y = 100 - 0 = 100.
        var result = CyUiHelpers.ComputeSkillFxLayout(
            Client1080(),
            new[] { new SkillFxInputSlot(7, 800, 100, 50, 50) });
        Assert.NotNull(result);
        Assert.Equal(694, result!.Window.X);
        var s0 = result.Slots[0];
        Assert.Equal(7, s0.Index);
        Assert.Equal(106, s0.X);
        Assert.Equal(100, s0.Y);
        Assert.Equal(50, s0.W);
        Assert.Equal(50, s0.H);
    }

    [Fact]
    public void Slots_sorted_by_index_ascending()
    {
        var result = CyUiHelpers.ComputeSkillFxLayout(
            Client1080(),
            new[]
            {
                new SkillFxInputSlot(7, 800, 200, 50, 50),
                new SkillFxInputSlot(2, 600, 200, 50, 50),
                new SkillFxInputSlot(4, 700, 200, 50, 50),
            });
        Assert.NotNull(result);
        Assert.Equal(new[] { 2, 4, 7 }, result!.Slots.Select(s => s.Index).ToArray());
    }

    [Fact]
    public void MinX_and_MaxY_aggregated_across_all_slots()
    {
        // Two slots: leftmost at x=300, deepest at y+h=400.
        var result = CyUiHelpers.ComputeSkillFxLayout(
            Client1080(),
            new[]
            {
                new SkillFxInputSlot(1, 300, 100, 50, 50),  // left edge=300, bottom=150
                new SkillFxInputSlot(2, 800, 350, 50, 50),  // bottom=400
            });
        Assert.NotNull(result);
        // win_x = 300 - 106 = 194.
        Assert.Equal(194, result!.Window.X);
        // win_y = 0; height = (400 - 0) + pad_y(18) = 418 → floored to 420? 418 < 220? no, 418 > 220 → 418.
        Assert.Equal(418, result.Window.H);
    }
}
