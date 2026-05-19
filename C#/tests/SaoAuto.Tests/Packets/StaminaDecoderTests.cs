using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class StaminaDecoderTests
{
    [Theory]
    [InlineData(1, true)]
    [InlineData(900, true)]
    [InlineData(1300, true)]
    [InlineData(1301, false)]
    [InlineData(0, false)]
    [InlineData(-1, false)]
    [InlineData(99999, false)]
    public void IsSaneAttrStaminaMaxFollowsPythonHeuristic(long value, bool expected)
    {
        Assert.Equal(expected, StaminaDecoder.IsSaneAttrStaminaMax(value));
    }

    [Fact]
    public void DecodeDirtyPrefersF32WhenPercentageWithMaxKnown()
    {
        // 0.25 (25% of stamina) is preferred when staminaMax > 0
        var result = StaminaDecoder.DecodeDirtyEnergyValue(rawU32: 9999, rawF32: 0.25f, staminaMax: 1000);
        Assert.NotNull(result);
        Assert.Equal(0.25, result!.Value, 4);
    }

    [Fact]
    public void DecodeDirtyAcceptsF32InAbsoluteRange()
    {
        var result = StaminaDecoder.DecodeDirtyEnergyValue(rawU32: 0, rawF32: 500.0f, staminaMax: 0);
        Assert.NotNull(result);
        Assert.Equal(500.0, result!.Value, 2);
    }

    [Fact]
    public void DecodeDirtyAcceptsZeroF32()
    {
        var result = StaminaDecoder.DecodeDirtyEnergyValue(rawU32: 0, rawF32: 0.0f, staminaMax: 0);
        Assert.NotNull(result);
        Assert.Equal(0.0, result!.Value);
    }

    [Fact]
    public void DecodeDirtyFallsBackToU32WhenF32OutOfRange()
    {
        var result = StaminaDecoder.DecodeDirtyEnergyValue(rawU32: 750, rawF32: float.NaN, staminaMax: 0);
        Assert.NotNull(result);
        Assert.Equal(750.0, result!.Value);
    }

    [Fact]
    public void DecodeDirtyRejectsBothOutOfRange()
    {
        var result = StaminaDecoder.DecodeDirtyEnergyValue(rawU32: 1_000_000, rawF32: float.PositiveInfinity, staminaMax: 0);
        Assert.Null(result);
    }

    [Fact]
    public void DecodeDirtyMaxAllowedScalesWithStaminaMax()
    {
        // staminaMax=1300 → max_allowed=1560 (1300*1.2)... but Python uses max(staminaMax*1.2, 20000)
        // so cap stays at 20000. A value of 19000 should still be accepted via u32.
        var result = StaminaDecoder.DecodeDirtyEnergyValue(rawU32: 19000, rawF32: float.NaN, staminaMax: 1300);
        Assert.NotNull(result);
        Assert.Equal(19000.0, result!.Value);
    }

    [Theory]
    [InlineData(0, 0)]
    [InlineData(7, 7)]
    [InlineData(-1, 0)]
    [InlineData(-99, 0)]
    [InlineData(150, 150)]
    public void NormalizeSeasonMedalLevelClampsBelowZero(long input, int expected)
    {
        Assert.Equal(expected, StaminaDecoder.NormalizeSeasonMedalLevel(input));
    }
}
