using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.Configuration;

public class AppVersionTests
{
    [Fact]
    public void LabelMatchesVersion()
    {
        Assert.Equal("3.1.1", AppVersion.Version);
        Assert.Equal("v3.1.1", AppVersion.Label);
    }

    [Theory]
    [InlineData("2.1.0", "2.1.1", -1)]
    [InlineData("2.1.1", "2.1.1", 0)]
    [InlineData("2.1.1-a", "2.1.1", 1)]
    [InlineData("2.1.1-a", "2.1.1-b", -1)]
    [InlineData("v2.1.1", "2.1.1", 0)]
    public void SuffixOrderingPutsSuffixAfterPlainVersion(string left, string right, int expectedSign)
    {
        var actual = AppVersion.Compare(left, right);
        Assert.Equal(expectedSign, Math.Sign(actual));
    }
}
