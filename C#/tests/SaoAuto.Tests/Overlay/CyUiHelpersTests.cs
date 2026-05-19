using SaoAuto.Overlay;

namespace SaoAuto.Tests.Overlay;

public class CyUiHelpersTests
{
    [Fact]
    public void BreathOffsetsHasRequestedCount()
    {
        var values = CyUiHelpers.BreathOffsets(0.0, count: 8, periodSeconds: 2.0, amplitude: 1.0);
        Assert.Equal(8, values.Length);
        foreach (var v in values) Assert.InRange(v, -1.0, 1.0);
    }

    [Fact]
    public void BreathOffsetsAtTimeZeroFirstSampleIsZero()
    {
        var values = CyUiHelpers.BreathOffsets(0.0, count: 4, periodSeconds: 2.0, amplitude: 1.0);
        Assert.Equal(0.0, values[0], 6);
    }

    [Theory]
    [InlineData(60, 0, "60")]
    [InlineData(60, 12, "60(+12)")]
    [InlineData(0, 0, "0")]
    [InlineData(-1, 5, "0(+5)")]
    public void FormatLevelTextMatchesPython(int lv, int extra, string expected)
    {
        Assert.Equal(expected, CyUiHelpers.FormatLevelText(lv, extra));
    }

    [Fact]
    public void NormalizeWatchedSlotsSorts_DedupesAndDropsNonPositive()
    {
        var raw = new[] { 3, 0, -1, 7, 3, 5 };
        Assert.Equal(new[] { 3, 5, 7 }, CyUiHelpers.NormalizeWatchedSkillSlots(raw));
    }

    [Theory]
    [InlineData(0.0, true)]
    [InlineData(0.001, true)]
    [InlineData(0.1, false)]
    public void IsDeadStateAtThreshold(double pct, bool expected)
    {
        Assert.Equal(expected, CyUiHelpers.IsDeadState(pct));
    }

    [Theory]
    [InlineData(0L, 0L)]
    [InlineData(123L, 123L)]
    [InlineData("456", 456L)]
    [InlineData("not-a-number", 0L)]
    [InlineData(null, 0L)]
    public void SessionIntParsesCommonShapes(object? input, long expected)
    {
        Assert.Equal(expected, CyUiHelpers.SessionInt(input));
    }

    [Theory]
    [InlineData(1234L, "1234")]
    [InlineData(9999L, "9999")]
    [InlineData(10_000L, "1万")]
    [InlineData(123_456L, "12万")]
    public void FormatSessionPower(long power, string expected)
    {
        Assert.Equal(expected, CyUiHelpers.FormatSessionPower(power));
    }

    [Theory]
    [InlineData(0.0, 0)]
    [InlineData(0.4, 0)]
    [InlineData(0.5, 1)]
    [InlineData(1.5, 2)]
    [InlineData(-0.5, 0)]
    [InlineData(123.45, 123)]
    public void RoundHalfUpNonnegMatchesPython(double v, long expected)
        => Assert.Equal(expected, CyUiHelpers.RoundHalfUpNonneg(v));

    [Theory]
    [InlineData(0.5, 0)]   // banker's rounds 0.5 → 0
    [InlineData(1.5, 2)]
    [InlineData(2.5, 2)]
    [InlineData(-1.5, -2)]
    public void RoundEvenMatchesPythonBuiltin(double v, long expected)
        => Assert.Equal(expected, CyUiHelpers.RoundEven(v));

    [Theory]
    [InlineData(1.234, 2, "1.23")]
    [InlineData(1.235, 2, "1.24")]
    [InlineData(0.0, 3, "0.000")]
    [InlineData(9.99, 1, "10.0")]
    [InlineData(42.0, 0, "42")]
    [InlineData(-3.0, 2, "0.00")]
    public void DpsToFixedHalfUpFormatsNonNeg(double v, int digits, string expected)
        => Assert.Equal(expected, CyUiHelpers.DpsToFixedHalfUp(v, digits));

    [Theory]
    [InlineData(999.0, "999")]
    [InlineData(1234.0, "1.2K")]
    [InlineData(99_999.0, "100.0K")]
    [InlineData(123_456.0, "123K")]
    [InlineData(1_234_567.0, "1.2M")]
    [InlineData(12_345_678.0, "12M")]
    public void DpsFmtNumSplitsAtKAndM(double v, string expected)
        => Assert.Equal(expected, CyUiHelpers.DpsFmtNum(v));

    [Theory]
    [InlineData(0.0, "")]
    [InlineData(-5.0, "")]
    [InlineData(1234.0, "1.2K")]
    [InlineData(2_345_678.0, "2.35M")]
    public void DpsFmtFpEmptyOnZeroAndUsesTwoDecimalsAtM(double v, string expected)
        => Assert.Equal(expected, CyUiHelpers.DpsFmtFp(v));

    [Theory]
    [InlineData(0.0, "00:00")]
    [InlineData(59.0, "00:59")]
    [InlineData(75.0, "01:15")]
    [InlineData(3661.0, "61:01")]
    [InlineData(-12.0, "00:00")]
    public void DpsFmtTimePadsMinutesAndSeconds(double s, string expected)
        => Assert.Equal(expected, CyUiHelpers.DpsFmtTime(s));

    [Theory]
    [InlineData(500.0, "500")]
    [InlineData(15_000.0, "15.0K")]
    [InlineData(1_500_000.0, "1.50M")]
    [InlineData(2_500_000_000.0, "2.50B")]
    public void BossHpFmtHpSplitsAtKMB(double v, string expected)
        => Assert.Equal(expected, CyUiHelpers.BossHpFmtHp(v));

    [Theory]
    [InlineData(0.0, 0.0)]
    [InlineData(1.0, 1.0)]
    [InlineData(0.5, 0.875)]
    [InlineData(-0.5, 0.0)]
    [InlineData(2.0, 1.0)]
    public void EaseOutCubicClampsAndEases(double t, double expected)
        => Assert.Equal(expected, CyUiHelpers.EaseOutCubic(t), 6);

    [Theory]
    [InlineData(0.0, 10.0, 0.0, 0.0)]
    [InlineData(0.0, 10.0, 1.0, 10.0)]
    [InlineData(0.0, 10.0, 0.25, 2.5)]
    [InlineData(0.0, 10.0, -1.0, 0.0)]
    [InlineData(0.0, 10.0, 2.0, 10.0)]
    public void LerpClampedClampsT(double a, double b, double t, double expected)
        => Assert.Equal(expected, CyUiHelpers.LerpClamped(a, b, t), 6);

    [Fact]
    public void CubicOpenRevealReturnsOneOnZeroDuration()
        => Assert.Equal(1.0, CyUiHelpers.CubicOpenReveal(0.0, 0.0));

    [Fact]
    public void CubicOpenRevealMatchesEaseOutCubic()
        => Assert.Equal(CyUiHelpers.EaseOutCubic(0.5), CyUiHelpers.CubicOpenReveal(1.0, 2.0), 6);

    [Fact]
    public void ScanPhaseStaysWithinUnitInterval()
    {
        for (double t = 0.0; t <= 10.0; t += 0.37)
        {
            var p = CyUiHelpers.ScanPhase(t);
            Assert.InRange(p, 0.0, 1.0);
        }
    }

    [Fact]
    public void PanelFloatOffsetsZeroAtZeroPhase()
    {
        var (dx, dy) = CyUiHelpers.PanelFloatOffsets(0.0, 0.0, 5.0);
        Assert.Equal(0, dx);
        // 5*sin(1.2) ≈ 4.66 → truncates to 4
        Assert.Equal(4, dy);
    }

    [Theory]
    [InlineData(null, "--")]
    [InlineData("", "--")]
    [InlineData("    ", "--")]
    [InlineData("Kirito", "Kirito")]
    [InlineData("12345678901234", "12345678901234")]
    [InlineData("123456789012345", "1234567890123\u2026")]
    public void ShortSessionNameTrimsAndEllipsizes(string? name, string expected)
        => Assert.Equal(expected, CyUiHelpers.ShortSessionName(name));
}
