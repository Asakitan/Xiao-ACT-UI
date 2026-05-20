using System.Net;
using System.Text;
using System.Text.Json;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.App;

/// <summary>
/// S177 — Pin <see cref="AutoKeyCloudBridge"/>. Each of the four cloud
/// endpoints routes through <see cref="BridgeRouter"/>; replies are
/// uniformly <c>{ok, data?, error?}</c>; HTTP / transport failures show
/// up as <c>{ok:false, error}</c>, not as router-level exceptions.
/// </summary>
public class Session177AutoKeyCloudBridgeTests
{
    private sealed class FakeHandler : HttpMessageHandler
    {
        public List<HttpRequestMessage> Requests { get; } = new();
        public Func<HttpRequestMessage, HttpResponseMessage> Responder { get; set; } = _ =>
            new HttpResponseMessage(HttpStatusCode.OK)
            { Content = new StringContent("{}", Encoding.UTF8, "application/json") };

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request, CancellationToken cancellationToken)
        {
            Requests.Add(request);
            return Task.FromResult(Responder(request));
        }
    }

    private static (BridgeRouter router, AutoKeyCloudBridge bridge, FakeHandler handler) Build()
    {
        var router = new BridgeRouter();
        var handler = new FakeHandler();
        var http = new HttpClient(handler);
        var client = new AutoKeyCloudClient("http://example.com", http);
        var bridge = new AutoKeyCloudBridge(router, client);
        return (router, bridge, handler);
    }

    private static BridgeMessage Cmd(string name, string payloadJson = "{}")
    {
        using var doc = JsonDocument.Parse(payloadJson);
        var payload = JsonSerializer.Deserialize<System.Text.Json.Nodes.JsonObject>(doc.RootElement.GetRawText());
        return new BridgeMessage(BridgeMessage.TypeCommand, name, payload);
    }

    [Fact]
    public void NullArgsThrow()
    {
        using var http = new HttpClient(new FakeHandler());
        using var client = new AutoKeyCloudClient("http://example.com", http);
        var router = new BridgeRouter();
        Assert.Throws<ArgumentNullException>(() => new AutoKeyCloudBridge(null!, client));
        Assert.Throws<ArgumentNullException>(() => new AutoKeyCloudBridge(router, null!));
    }

    [Fact]
    public void RegistersFourCommandsAndUnregistersOnDispose()
    {
        var (router, bridge, _) = Build();
        Assert.Contains(BridgeCommands.SearchAutoKeyScripts, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.GetAutoKeyScript, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.IssueAutoKeyUploadToken, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.UploadAutoKeyScript, router.RegisteredCommands);
        bridge.Dispose();
        Assert.DoesNotContain(BridgeCommands.SearchAutoKeyScripts, router.RegisteredCommands);
    }

    [Fact]
    public void SearchForwardsQueryAndWrapsResponse()
    {
        var (router, _, handler) = Build();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("{\"results\":[1,2,3]}", Encoding.UTF8, "application/json"),
        };
        var reply = router.Dispatch(Cmd(BridgeCommands.SearchAutoKeyScripts,
            "{\"query\":{\"q\":\"x\",\"page\":\"2\"}}"));
        Assert.NotNull(reply?.Payload);
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        var data = reply.Payload["data"]!.AsObject();
        var url = handler.Requests[0].RequestUri!.AbsoluteUri;
        Assert.Contains("q=x", url);
        Assert.Contains("page=2", url);
        Assert.Equal(3, data["results"]!.AsArray().Count);
    }

    [Fact]
    public void GetMissingIdReturnsError()
    {
        var (router, _, _) = Build();
        var reply = router.Dispatch(Cmd(BridgeCommands.GetAutoKeyScript, "{}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("missing_id", reply.Payload["error"]!.GetValue<string>());
    }

    [Fact]
    public void GetForwardsIdAndWraps()
    {
        var (router, _, handler) = Build();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("{\"id\":\"abc\",\"profile_name\":\"P\"}", Encoding.UTF8, "application/json"),
        };
        var reply = router.Dispatch(Cmd(BridgeCommands.GetAutoKeyScript, "{\"id\":\"abc\"}"));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("P", reply.Payload["data"]!["profile_name"]!.GetValue<string>());
        Assert.EndsWith("/api/scripts/abc", handler.Requests[0].RequestUri!.AbsoluteUri);
    }

    [Fact]
    public void IssueTokenMissingPayloadReturnsError()
    {
        var (router, _, _) = Build();
        var reply = router.Dispatch(Cmd(BridgeCommands.IssueAutoKeyUploadToken, "{}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("missing_payload", reply.Payload["error"]!.GetValue<string>());
    }

    [Fact]
    public void IssueTokenForwardsBody()
    {
        var (router, _, handler) = Build();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("{\"token\":\"tk\"}", Encoding.UTF8, "application/json"),
        };
        var reply = router.Dispatch(Cmd(BridgeCommands.IssueAutoKeyUploadToken,
            "{\"payload\":{\"player_uid\":\"u\"}}"));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("tk", reply.Payload["data"]!["token"]!.GetValue<string>());
    }

    [Fact]
    public void UploadMissingProfileReturnsError()
    {
        var (router, _, _) = Build();
        var reply = router.Dispatch(Cmd(BridgeCommands.UploadAutoKeyScript,
            "{\"token\":\"t\"}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("missing_profile", reply.Payload["error"]!.GetValue<string>());
    }

    [Fact]
    public void UploadForwardsTokenHeaderAndProfile()
    {
        var (router, _, handler) = Build();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("{\"id\":\"r1\"}", Encoding.UTF8, "application/json"),
        };
        var reply = router.Dispatch(Cmd(BridgeCommands.UploadAutoKeyScript,
            "{\"token\":\"secret\",\"profile\":{\"profile_name\":\"X\"}}"));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("r1", reply.Payload["data"]!["id"]!.GetValue<string>());
        Assert.True(handler.Requests[0].Headers.TryGetValues("X-SAO-Upload-Token", out var values));
        Assert.Equal("secret", values!.Single());
    }

    [Fact]
    public void HttpErrorBecomesOkFalse()
    {
        var (router, _, handler) = Build();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.BadRequest)
        {
            Content = new StringContent("{\"detail\":\"nope\"}", Encoding.UTF8, "application/json"),
        };
        var reply = router.Dispatch(Cmd(BridgeCommands.GetAutoKeyScript, "{\"id\":\"x\"}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("nope", reply.Payload["error"]!.GetValue<string>());
        Assert.Null(reply.Payload["data"]);
    }

    [Fact]
    public void TransportErrorBecomesOkFalse()
    {
        var handler = new FakeHandler { Responder = _ => throw new HttpRequestException("boom") };
        using var http = new HttpClient(handler);
        using var client = new AutoKeyCloudClient("http://example.com", http);
        var router = new BridgeRouter();
        using var bridge = new AutoKeyCloudBridge(router, client);

        var reply = router.Dispatch(Cmd(BridgeCommands.GetAutoKeyScript, "{\"id\":\"x\"}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("boom", reply.Payload["error"]!.GetValue<string>());
    }
}
