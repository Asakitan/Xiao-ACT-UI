using System.Net;
using System.Text;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

/// <summary>
/// S178 — Pin <see cref="WebBridgeLifecycle.AttachAutoKeyCloud"/>.
/// Mirrors S172's updater attach: registers the four cloud commands,
/// idempotent on second call, unregisters on dispose, rejects null.
/// </summary>
public class Session178AutoKeyCloudAttachTests
{
    private sealed class FakeHandler : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request, CancellationToken cancellationToken)
            => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent("{}", Encoding.UTF8, "application/json"),
            });
    }

    private static AutoKeyCloudClient FreshClient()
    {
        var http = new HttpClient(new FakeHandler());
        return new AutoKeyCloudClient("http://example.com", http);
    }

    [Fact]
    public void AttachRegistersFourCommands()
    {
        var states = new GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        using var client = FreshClient();

        web.AttachAutoKeyCloud(client);

        Assert.Contains(BridgeCommands.SearchAutoKeyScripts, web.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.GetAutoKeyScript, web.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.IssueAutoKeyUploadToken, web.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.UploadAutoKeyScript, web.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachIsIdempotent()
    {
        var states = new GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        using var c1 = FreshClient();
        using var c2 = FreshClient();
        web.AttachAutoKeyCloud(c1);
        web.AttachAutoKeyCloud(c2); // must not throw, must not double-register
        Assert.Contains(BridgeCommands.SearchAutoKeyScripts, web.Router.RegisteredCommands);
    }

    [Fact]
    public void DisposeUnregistersCommands()
    {
        var states = new GameStateManager();
        var web = new WebBridgeLifecycle(states);
        using var client = FreshClient();
        web.AttachAutoKeyCloud(client);
        web.Dispose();
        Assert.DoesNotContain(BridgeCommands.SearchAutoKeyScripts, web.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachNullThrows()
    {
        var states = new GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        Assert.Throws<ArgumentNullException>(() => web.AttachAutoKeyCloud(null!));
    }

    [Fact]
    public void AttachAfterDisposeThrows()
    {
        var states = new GameStateManager();
        var web = new WebBridgeLifecycle(states);
        web.Dispose();
        using var client = FreshClient();
        Assert.Throws<ObjectDisposedException>(() => web.AttachAutoKeyCloud(client));
    }
}
