using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S78 — HSV slot-mask measurer. Verifies parity with
/// <c>skill_recognition._measure_slot</c> + <c>_compare_to_baseline</c>
/// for the deterministic shapes (mask geometry, size guards, full-bright
/// vs dark slots, baseline drift detection).
/// </summary>
public class Session78SkillSlotMeasurerTests
{
    private static byte[] MakeBgr(int w, int h, Func<int, int, (byte b, byte g, byte r)> px)
    {
        var buf = new byte[w * h * 3];
        for (var y = 0; y < h; y++)
        for (var x = 0; x < w; x++)
        {
            var (b, g, r) = px(x, y);
            var i = (y * w + x) * 3;
            buf[i] = b; buf[i + 1] = g; buf[i + 2] = r;
        }
        return buf;
    }

    private static (double cx, double cy, double ro, double ri) Geometry(int w, int h)
        => (w / 2.0, h * 0.46,
            Math.Max(6.0, Math.Min(w, h) * 0.47),
            Math.Max(4.0, Math.Min(w, h) * 0.30));

    // ── Mask geometry ───────────────────────────────────────────────

    [Fact]
    public void BuildMasks_GeneratesNonEmptyConcentricRegions()
    {
        Assert.True(SkillSlotHsvMeasurer.BuildMasks(40, 40, out var icon, out var inner, out var ring));
        var iconCount = icon.Count(b => b);
        var innerCount = inner.Count(b => b);
        var ringCount = ring.Count(b => b);
        Assert.True(iconCount > innerCount);
        Assert.True(innerCount > 0);
        Assert.True(ringCount > 0);
        Assert.Equal(iconCount, innerCount + ringCount);
        // Ring is the difference set — no overlap with inner.
        for (var i = 0; i < ring.Length; i++) if (ring[i]) Assert.False(inner[i]);
    }

    [Fact]
    public void BuildMasks_TinyDimensions_ReturnsFalse()
    {
        Assert.False(SkillSlotHsvMeasurer.BuildMasks(4, 4, out _, out _, out _));
    }

    // ── Size guard on Measure ───────────────────────────────────────

    [Fact]
    public void Measure_TinyImage_ReturnsNull()
    {
        var buf = new byte[7 * 7 * 3];
        Assert.Null(SkillSlotHsvMeasurer.Measure(buf, 7, 7));
    }

    [Fact]
    public void Measure_ShortBuffer_ReturnsNull()
    {
        var buf = new byte[10];
        Assert.Null(SkillSlotHsvMeasurer.Measure(buf, 100, 100));
    }

    // ── Bright (ready-like) slot ────────────────────────────────────

    [Fact]
    public void Measure_BrightWhiteSlot_HighReadyMetrics()
    {
        var w = 32; var h = 32;
        var buf = MakeBgr(w, h, (x, y) => ((byte)240, (byte)240, (byte)240));
        var m = SkillSlotHsvMeasurer.Measure(buf, w, h);
        Assert.NotNull(m);
        Assert.True(m!.InnerVMean > 200, $"InnerV={m.InnerVMean}");
        Assert.True(m.BrightRatio > 0.95, $"bright={m.BrightRatio}");
        Assert.Equal(0.0, m.DarkRatio);
        Assert.Equal(0.0, m.ShadowRatio);
        Assert.True(m.ReadyScore > 0.4, $"ready={m.ReadyScore}");
    }

    // ── Dark (cooldown-like) slot ───────────────────────────────────

    [Fact]
    public void Measure_BlackSlot_HighDarkRatios()
    {
        var w = 32; var h = 32;
        var buf = MakeBgr(w, h, (x, y) => ((byte)0, (byte)0, (byte)0));
        var m = SkillSlotHsvMeasurer.Measure(buf, w, h);
        Assert.NotNull(m);
        Assert.Equal(0.0, m!.InnerVMean);
        Assert.Equal(1.0, m.DarkRatio);
        Assert.Equal(1.0, m.ShadowRatio);
        Assert.Equal(1.0, m.GrayDarkRatio);
        Assert.Equal(1.0, m.DimRatio);
        Assert.Equal(0.0, m.BrightRatio);
        Assert.Equal(0.0, m.RingRatio);
    }

    // ── Cyan ring detection ─────────────────────────────────────────

    [Fact]
    public void Measure_CyanRingPaint_RaisesRingRatio()
    {
        // Solid cyan covers entire icon → ring mask sees cyan everywhere.
        // Cyan BGR≈(255, 255, 0) → HSV H=90 S=255 V=255 (in OpenCV [0,180]).
        var w = 32; var h = 32;
        var buf = MakeBgr(w, h, (x, y) => ((byte)255, (byte)255, (byte)0));
        var m = SkillSlotHsvMeasurer.Measure(buf, w, h);
        Assert.NotNull(m);
        Assert.True(m!.RingRatio > 0.85, $"ring={m.RingRatio}");
    }

    // ── Warm (insufficient-energy-like) slot ────────────────────────

    [Fact]
    public void Measure_WarmOrangeSlot_RaisesWarmRatio()
    {
        // Orange #FFAE35 → BGR (53,174,255), H≈18, S≈204, V=255 → warm gate.
        var w = 32; var h = 32;
        var buf = MakeBgr(w, h, (x, y) => ((byte)53, (byte)174, (byte)255));
        var m = SkillSlotHsvMeasurer.Measure(buf, w, h);
        Assert.NotNull(m);
        Assert.True(m!.WarmRatio > 0.95, $"warm={m.WarmRatio}");
    }

    // ── Compare: baseline drift ─────────────────────────────────────

    [Fact]
    public void Compare_DarkenedFrameVsBrightBaseline_SignalsDarkening()
    {
        var w = 32; var h = 32;
        var bright = MakeBgr(w, h, (x, y) => ((byte)240, (byte)240, (byte)240));
        var darker = MakeBgr(w, h, (x, y) => ((byte)80, (byte)80, (byte)80));
        var c = SkillSlotHsvMeasurer.Compare(darker, bright, w, h);
        Assert.NotNull(c);
        Assert.True(c!.IconVRatio < 0.50, $"icon_v_ratio={c.IconVRatio}");
        Assert.True(c.ScoreRatio < 0.60, $"score_ratio={c.ScoreRatio}");
        Assert.True(c.DarkenedRatio > 0.95, $"darkened={c.DarkenedRatio}");
        Assert.True(c.AvgDeltaV < -100, $"delta={c.AvgDeltaV}");
    }

    [Fact]
    public void Compare_IdenticalFrames_FullRestoredRatio()
    {
        var w = 32; var h = 32;
        var buf = MakeBgr(w, h, (x, y) => ((byte)180, (byte)180, (byte)180));
        var c = SkillSlotHsvMeasurer.Compare(buf, buf, w, h);
        Assert.NotNull(c);
        Assert.Equal(1.0, c!.IconVRatio, 3);
        // ScoreRatio is ~0.998 not exactly 1.0 because Python floors
        // the baseline icon-S mean to 1.0 (max(1, ...)) while the
        // current icon-S mean stays 0 — same drift in both ports.
        Assert.InRange(c.ScoreRatio, 0.99, 1.001);
        Assert.Equal(0.0, c.DarkenedRatio);
        Assert.Equal(1.0, c.RestoredRatio);
        Assert.Equal(0.0, c.AvgDeltaV, 3);
    }

    [Fact]
    public void Compare_ShortBuffer_ReturnsNull()
    {
        var w = 32; var h = 32;
        var ok = new byte[w * h * 3];
        var bad = new byte[10];
        Assert.Null(SkillSlotHsvMeasurer.Compare(ok, bad, w, h));
        Assert.Null(SkillSlotHsvMeasurer.Compare(bad, ok, w, h));
    }
}
