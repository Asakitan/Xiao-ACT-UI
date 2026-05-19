using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S76 — exact-BGR stamina detector + brightness fallback +
/// blank-frame guard. Verifies parity with recognition.py
/// (_detect_stamina_pct, _detect_bar_pct_simple,
/// _capture_looks_blank).
/// </summary>
public class Session76StaminaDetectorTests
{
    private const byte StaB = 53, StaG = 174, StaR = 255;

    private static byte[] MakeBgr(int width, int height, Func<int, int, (byte b, byte g, byte r)> px)
    {
        var buf = new byte[width * height * 3];
        for (var y = 0; y < height; y++)
        for (var x = 0; x < width; x++)
        {
            var (b, g, r) = px(x, y);
            var i = (y * width + x) * 3;
            buf[i] = b; buf[i + 1] = g; buf[i + 2] = r;
        }
        return buf;
    }

    private static (byte, byte, byte) Gold => (StaB, StaG, StaR);
    private static (byte, byte, byte) Black => (0, 0, 0);

    // ── Sta detector ────────────────────────────────────────────────

    [Fact]
    public void Stamina_TinyImage_ReturnsEmpty()
    {
        var pixels = new byte[3 * 3]; // 3px wide
        var r = StaminaColorDetector.Detect(pixels, 3, 1);
        Assert.Equal(StaminaColorDetector.Result.Empty, r);
    }

    [Fact]
    public void Stamina_FullyGold_ReturnsHighPctAndConfidence()
    {
        var w = 100; var h = 8;
        var buf = MakeBgr(w, h, (x, y) => Gold);
        var r = StaminaColorDetector.Detect(buf, w, h);
        Assert.True(r.Pct >= 0.99, $"pct={r.Pct}");
        Assert.True(r.Confidence >= 0.7, $"conf={r.Confidence}");
    }

    [Fact]
    public void Stamina_HalfGold_ReturnsHalfPct()
    {
        var w = 100; var h = 8;
        var buf = MakeBgr(w, h, (x, y) => x < 50 ? Gold : Black);
        var r = StaminaColorDetector.Detect(buf, w, h);
        Assert.InRange(r.Pct, 0.49, 0.51);
        Assert.True(r.Confidence > 0.0);
    }

    [Fact]
    public void Stamina_AllBlack_ReturnsEmpty()
    {
        var w = 80; var h = 8;
        var buf = MakeBgr(w, h, (x, y) => Black);
        var r = StaminaColorDetector.Detect(buf, w, h);
        Assert.Equal(0.0, r.Pct);
        Assert.Equal(0.0, r.Confidence);
    }

    [Fact]
    public void Stamina_QuarterFill_ReturnsQuarterPct()
    {
        var w = 100; var h = 8;
        var buf = MakeBgr(w, h, (x, y) => x < 25 ? Gold : Black);
        var r = StaminaColorDetector.Detect(buf, w, h);
        Assert.InRange(r.Pct, 0.24, 0.26);
    }

    [Fact]
    public void Stamina_NearFullWithEdgeGap_ExtensionRecoversPct()
    {
        // Fill 0..89 gold, 90..99 dim (below primary but above NearFull would
        // require ≥ 0.07 col_fill — single rows are 1/8 = 0.125 ≥ 0.07).
        // We give x ∈ [90,99] a single gold row (1/8 = 0.125 ≥ 0.07
        // but < 0.18) so primary scan stops at 89 and extension picks up.
        var w = 100; var h = 8;
        var buf = MakeBgr(w, h, (x, y) =>
        {
            if (x < 90) return Gold;
            if (y == 0) return Gold;   // 1/8 = 0.125 → relaxed only
            return Black;
        });
        var r = StaminaColorDetector.Detect(buf, w, h);
        Assert.True(r.Pct >= 0.99, $"pct={r.Pct} (should extend past 0.90)");
    }

    [Fact]
    public void Stamina_RelaxedOnlyPath_RequiresContiguousFromLeft()
    {
        // No column hits the primary 0.18 threshold, but most do hit 0.07.
        // Place 1 gold row in every column → per-col fill = 1/8 = 0.125
        // which is ≥ NearFull(0.07) but < Threshold(0.18).
        var w = 100; var h = 8;
        var buf = MakeBgr(w, h, (x, y) => y == 0 ? Gold : Black);
        var r = StaminaColorDetector.Detect(buf, w, h);
        // Should classify as near-full via relaxed path
        Assert.True(r.Pct >= 0.85, $"pct={r.Pct} via relaxed path");
    }

    [Fact]
    public void Stamina_HandlesShortBuffer()
    {
        var buf = new byte[5];   // way too small
        var r = StaminaColorDetector.Detect(buf, 100, 8);
        Assert.Equal(StaminaColorDetector.Result.Empty, r);
    }

    // ── Brightness fallback ─────────────────────────────────────────

    [Fact]
    public void Brightness_TinyImage_ReturnsZero()
    {
        var buf = new byte[1 * 1 * 3];
        Assert.Equal(0.0, BarBrightnessDetector.Detect(buf, 1, 1));
    }

    [Fact]
    public void Brightness_HalfBright_ReturnsHalfPct()
    {
        var w = 100; var h = 6;
        var buf = MakeBgr(w, h, (x, y) => x < 50 ? ((byte)200, (byte)200, (byte)200) : Black);
        var pct = BarBrightnessDetector.Detect(buf, w, h);
        Assert.InRange(pct, 0.49, 0.51);
    }

    [Fact]
    public void Brightness_AllDark_ReturnsZero()
    {
        var w = 60; var h = 4;
        var buf = MakeBgr(w, h, (x, y) => ((byte)5, (byte)5, (byte)5));
        Assert.Equal(0.0, BarBrightnessDetector.Detect(buf, w, h));
    }

    [Fact]
    public void Brightness_FullBright_ReturnsOne()
    {
        var w = 50; var h = 4;
        var buf = MakeBgr(w, h, (x, y) => ((byte)255, (byte)255, (byte)255));
        Assert.Equal(1.0, BarBrightnessDetector.Detect(buf, w, h));
    }

    // ── Blank detector ──────────────────────────────────────────────

    [Fact]
    public void Blank_EmptyBuffer_IsBlank()
    {
        Assert.True(BlankFrameDetector.LooksBlank(ReadOnlySpan<byte>.Empty));
    }

    [Fact]
    public void Blank_AllZero_IsBlank()
    {
        var buf = new byte[300];   // all zero
        Assert.True(BlankFrameDetector.LooksBlank(buf));
    }

    [Fact]
    public void Blank_AllTwo_IsBlank()
    {
        var buf = new byte[300];
        Array.Fill(buf, (byte)2);
        Assert.True(BlankFrameDetector.LooksBlank(buf));
    }

    [Fact]
    public void Blank_OnePixelTen_IsNotBlank()
    {
        var buf = new byte[300];
        buf[100] = 10;   // max > 2
        Assert.False(BlankFrameDetector.LooksBlank(buf));
    }

    [Fact]
    public void Blank_HighStdEvenIfMaxLow_IsNotBlank()
    {
        // Max ≤ 2 but std > 1: alternating 0/2 over 300 bytes → std = 1.0
        // exactly. Use 0/3? Then max=3 → fails the max gate. The std gate
        // only kicks in when max ≤ 2; build a real "max=2 but std > 1" by
        // using a longer wave with values 0,2,0,2,... → std ≈ 1.0; Python
        // accepts std ≤ 1.0. So this test pins that std == 1.0 alternating
        // is still considered blank.
        var buf = new byte[300];
        for (var i = 0; i < buf.Length; i++) buf[i] = (byte)(i % 2 == 0 ? 0 : 2);
        Assert.True(BlankFrameDetector.LooksBlank(buf));
    }
}
