using SaoAuto.App.Hosting;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.Hosting;

public class ModeRouterTests
{
    [Fact]
    public void EntityRequestedTriesEntityThenWebView()
    {
        var chain = ModeRouter.BuildChain(UiMode.Entity);
        Assert.Equal(new[] { UiMode.Entity, UiMode.WebView }, chain);
    }

    [Theory]
    [InlineData(UiMode.WebView)]
    [InlineData("WEBVIEW")]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("unrecognized")]
    public void NonEntityRequestedTriesWebViewFirstThenEntity(string? requested)
    {
        var chain = ModeRouter.BuildChain(requested);
        Assert.Equal(2, chain.Count);
        Assert.Equal(UiMode.WebView, chain[0]);
        Assert.Equal(UiMode.Entity, chain[1]);
    }

    [Fact]
    public void LegacySaoAliasMapsToEntityChain()
    {
        // Python normalizes ui_mode=sao to entity before deciding the chain.
        var chain = ModeRouter.BuildChain("sao");
        Assert.Equal(new[] { UiMode.Entity, UiMode.WebView }, chain);
    }

    [Fact]
    public void ChainNeverContainsHeadless()
    {
        // Headless is the runner's final-final fallback, not part of the chain.
        Assert.DoesNotContain("headless", ModeRouter.BuildChain(UiMode.Entity));
        Assert.DoesNotContain("headless", ModeRouter.BuildChain(UiMode.WebView));
    }
}
