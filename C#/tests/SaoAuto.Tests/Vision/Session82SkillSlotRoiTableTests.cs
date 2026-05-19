using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S82 — Skill-slot ROI table tests. Pin the 1920×1080 anchor table
/// (slot 1..9 + stamina_bar_visual) and the proportional scaling /
/// caching behaviour of <see cref="SkillSlotRoiTable"/>.
/// </summary>
public class Session82SkillSlotRoiTableTests
{
    [Fact]
    public void Spec_ReturnsCalibratedTable()
    {
        var s = SkillSlotRoiTable.GetSpec("skill_slot_1");
        Assert.NotNull(s);
        Assert.Equal(720, s!.Value.Right);
        Assert.Equal(1003, s.Value.Bottom);
        Assert.Equal(52, s.Value.Width);
        Assert.Equal(85, s.Value.Height);

        var st = SkillSlotRoiTable.GetSpec("stamina_bar_visual");
        Assert.NotNull(st);
        Assert.Equal(250, st!.Value.Width);

        Assert.Null(SkillSlotRoiTable.GetSpec("does_not_exist"));
    }

    [Fact]
    public void GetVisualRectBbox_BaseSize_PreservesAnchors()
    {
        // At 1920×1080 the bbox = (right-w, bottom-h, right, bottom).
        var bbox = SkillSlotRoiTable.GetVisualRectBbox("skill_slot_1", 0, 0, 1920, 1080);
        Assert.NotNull(bbox);
        Assert.Equal((720 - 52, 1003 - 85, 720, 1003), bbox!.Value);
    }

    [Fact]
    public void GetVisualRectBbox_ScaledClient_ProportionalScaling()
    {
        // Half-size client (960×540) → half-size bbox.
        var bbox = SkillSlotRoiTable.GetVisualRectBbox("skill_slot_5", 0, 0, 960, 540);
        Assert.NotNull(bbox);
        var width = bbox!.Value.X2 - bbox.Value.X1;
        var height = bbox.Value.Y2 - bbox.Value.Y1;
        Assert.InRange(width, 22, 24);   // 45/2 rounds
        Assert.InRange(height, 43, 44);  // 87/2 rounds
    }

    [Fact]
    public void GetVisualRectBbox_NonZeroOrigin_OffsetsCorrectly()
    {
        var bbox1 = SkillSlotRoiTable.GetVisualRectBbox("skill_slot_1", 0, 0, 1920, 1080);
        var bbox2 = SkillSlotRoiTable.GetVisualRectBbox("skill_slot_1", 100, 50, 100 + 1920, 50 + 1080);
        Assert.NotNull(bbox1);
        Assert.NotNull(bbox2);
        Assert.Equal(bbox1!.Value.X1 + 100, bbox2!.Value.X1);
        Assert.Equal(bbox1.Value.Y1 + 50, bbox2.Value.Y1);
    }

    [Fact]
    public void GetVisualRectBbox_UnknownName_ReturnsNull()
    {
        Assert.Null(SkillSlotRoiTable.GetVisualRectBbox("nope", 0, 0, 1920, 1080));
    }

    [Fact]
    public void GetSkillSlotRects_BaseSize_NineSlots()
    {
        SkillSlotRoiTable.ClearCache();
        var rects = SkillSlotRoiTable.GetSkillSlotRects(0, 0, 1920, 1080);
        Assert.Equal(9, rects.Count);
        for (var i = 0; i < 9; i++)
        {
            Assert.Equal(i + 1, rects[i].Index);
            Assert.Equal(i + 1, rects[i].VisualIndex);
            Assert.True(rects[i].Width > 0);
            Assert.True(rects[i].Height > 0);
        }
    }

    [Fact]
    public void GetSkillSlotRects_Cached_SameInstance()
    {
        SkillSlotRoiTable.ClearCache();
        var a = SkillSlotRoiTable.GetSkillSlotRects(0, 0, 1920, 1080);
        var b = SkillSlotRoiTable.GetSkillSlotRects(0, 0, 1920, 1080);
        Assert.Same(a, b);
    }

    [Fact]
    public void GetSkillSlotClientRects_HalfSize_HalfDimensions()
    {
        SkillSlotRoiTable.ClearCache();
        var full = SkillSlotRoiTable.GetSkillSlotClientRects(1920, 1080);
        var half = SkillSlotRoiTable.GetSkillSlotClientRects(960, 540);
        Assert.Equal(9, full.Count);
        Assert.Equal(9, half.Count);
        // Each half-size rect should be ~half of the full-size one (within rounding).
        for (var i = 0; i < 9; i++)
        {
            Assert.InRange(half[i].W, full[i].W / 2 - 1, full[i].W / 2 + 1);
            Assert.InRange(half[i].H, full[i].H / 2 - 1, full[i].H / 2 + 1);
        }
    }

    [Fact]
    public void GetSkillSlotClientRects_ZeroSize_EmptyList()
    {
        SkillSlotRoiTable.ClearCache();
        var rects = SkillSlotRoiTable.GetSkillSlotClientRects(0, 0);
        Assert.Empty(rects);
    }

    [Fact]
    public void SkillBarRoi_UnionOfSlotAnchors()
    {
        var roi = SkillSlotRoiTable.SkillBarRoi;
        // Left should be slot_1.left = (720-52)/1920 = 668/1920 ≈ 0.348
        Assert.InRange(roi.X, 0.347, 0.349);
        // Right edge = slot_9.right = 1177/1920 ≈ 0.613
        Assert.InRange(roi.X + roi.W, 0.612, 0.614);
        Assert.True(roi.H > 0);
    }

    [Fact]
    public void GetVisualRectClientRect_ZeroSize_ReturnsNull()
    {
        Assert.Null(SkillSlotRoiTable.GetVisualRectClientRect("skill_slot_1", 0, 1080));
    }

    [Fact]
    public void GetSkillSlotVisualIndex_IdentityMapping()
    {
        for (var i = 1; i <= 9; i++)
            Assert.Equal(i, SkillSlotRoiTable.GetSkillSlotVisualIndex(i));
    }
}
