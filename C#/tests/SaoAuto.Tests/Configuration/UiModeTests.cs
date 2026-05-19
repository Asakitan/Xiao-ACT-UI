using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.Configuration;

public class UiModeTests
{
    [Theory]
    [InlineData("sao", UiMode.Entity)]
    [InlineData("SAO", UiMode.Entity)]
    [InlineData("entity", UiMode.Entity)]
    [InlineData("Entity ", UiMode.Entity)]
    [InlineData("webview", UiMode.WebView)]
    [InlineData(" WEBVIEW ", UiMode.WebView)]
    public void NormalizesAliasesAndCanonicalValues(string input, string expected)
    {
        Assert.Equal(expected, UiMode.Normalize(input));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    public void DefaultsToWebViewWhenMissing(string? input)
    {
        Assert.Equal(UiMode.Default, UiMode.Normalize(input));
        Assert.Equal(UiMode.WebView, UiMode.Default);
    }

    [Fact]
    public void UnknownStringsArePassedThroughLowercased()
    {
        Assert.Equal("headless", UiMode.Normalize("Headless"));
    }
}
