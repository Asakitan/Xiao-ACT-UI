using SaoAuto.Overlay.Panels;

namespace SaoAuto.Tests.Overlay;

public class CompactNumberFormatterTests
{
    [Theory]
    [InlineData(0, "0")]
    [InlineData(999, "999")]
    [InlineData(1_000, "1K")]
    [InlineData(1_234, "1.23K")]
    [InlineData(9_999, "10K")]
    [InlineData(10_000, "10K")]
    [InlineData(123_456, "123K")]
    [InlineData(1_500_000, "1.5M")]
    [InlineData(15_000_000, "15M")]
    [InlineData(1_500_000_000L, "1.5G")]
    public void FormatProducesPythonShape(long value, string expected)
    {
        Assert.Equal(expected, CompactNumberFormatter.Format(value));
    }

    [Fact]
    public void NegativeValuesPreserveSign()
    {
        Assert.Equal("-1.5K", CompactNumberFormatter.Format(-1500));
    }

    [Theory]
    [InlineData(0.0, "0%")]
    [InlineData(0.5, "50%")]
    [InlineData(1.0, "100%")]
    [InlineData(1.5, "100%")]
    [InlineData(-0.1, "0%")]
    public void FormatPercentClampsAndRounds(double pct, string expected)
    {
        Assert.Equal(expected, CompactNumberFormatter.FormatPercent(pct));
    }

    [Fact]
    public void FormatPercentWithDecimals()
    {
        Assert.Equal("12.50%", CompactNumberFormatter.FormatPercent(0.125, decimals: 2));
    }
}
