using SaoAuto.Core.Configuration;
using System.Diagnostics;
using System.Reflection;

namespace SaoAuto.Tests.Configuration;

public class AppVersionTests
{
    [Fact]
    public void LabelMatchesVersion()
    {
        Assert.Equal("5.0.0", AppVersion.Version);
        Assert.Equal("v5.0.0", AppVersion.Label);
    }

    [Fact]
    public void AssemblyVersionMatchesProductVersion()
    {
        var assembly = typeof(AppVersion).Assembly;
        var fileVersion = FileVersionInfo.GetVersionInfo(assembly.Location);
        var informationalVersion = assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
            .InformationalVersion;

        Assert.Equal(new Version(5, 0, 0, 0), assembly.GetName().Version);
        Assert.Equal("5.0.0.0", fileVersion.FileVersion);
        Assert.Equal(AppVersion.Version, informationalVersion);
    }

    [Theory]
    [InlineData("4.9.9", "5.0.0", -1)]
    [InlineData("5.0.0", "5.0.0", 0)]
    [InlineData("5.0.0-a", "5.0.0", 1)]
    [InlineData("5.0.0-a", "5.0.0-b", -1)]
    [InlineData("v5.0.0", "5.0.0", 0)]
    public void SuffixOrderingPutsSuffixAfterPlainVersion(string left, string right, int expectedSign)
    {
        var actual = AppVersion.Compare(left, right);
        Assert.Equal(expectedSign, Math.Sign(actual));
    }
}
