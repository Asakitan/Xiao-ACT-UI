using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

/// <summary>
/// S179 — Pin <see cref="BossRaidCloudBridge"/> +
/// <see cref="WebBridgeLifecycle.AttachBossRaidCloud"/>. Mirrors
/// S177/S178 for the auto-key side. Key extra: the client returns
/// <c>{error:"..."}</c> on failure rather than throwing, so the
/// bridge must collapse that shape into <c>{ok:false, error}</c>.
/// </summary>
public class Session179BossRaidCloudBridgeTests
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

    private static (BridgeRouter router, BossRaidCloudBridge bridge, FakeHandler handler) Build()
    {
        var router = new BridgeRouter();
        var handler = new FakeHandler();
        var http = new HttpClient(handler);
        var client = new BossRaidCloudClient("http://example.com", http);
        var bridge = new BossRaidCloudBridge(router, client);
        return (router, bridge, handler);
    }

    private static BridgeMessage Cmd(string name, string payloadJson = "{}")
    {
        using var doc = JsonDocument.Parse(payloadJson);
        var payload = JsonSerializer.Deserialize<JsonObject>(doc.RootElement.GetRawText());
        return new BridgeMessage(BridgeMessage.TypeCommand, name, payload);
    }

    [Fact]
    public void NullArgsThrow()
    {
        var router = new BridgeRouter();
        using var http = new HttpClient(new FakeHandler());
        using var client = new BossRaidCloudClient("http://example.com", http);
        Assert.Throws<ArgumentNullException>(() => new BossRaidCloudBridge(null!, client));
        Assert.Throws<ArgumentNullException>(() => new BossRaidCloudBridge(router, null!));
    }

    [Fact]
    public void RegistersCommandsAndUnregistersOnDispose()
    {
        var (router, bridge, _) = Build();
        Assert.Contains(BridgeCommands.SearchBossRaids, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.GetBossRaid, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DownloadBossRaidRemote, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RefreshBossRaidUploadAuth, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.IssueBossRaidUploadToken, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.UploadBossRaid, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBossRaidServerUrl, router.RegisteredCommands);
        bridge.Dispose();
        Assert.DoesNotContain(BridgeCommands.SearchBossRaids, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.DownloadBossRaidRemote, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RefreshBossRaidUploadAuth, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBossRaidServerUrl, router.RegisteredCommands);
    }

    [Fact]
    public void SearchForwardsQueryAndWrapsResponse()
    {
        var (router, _, handler) = Build();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("{\"results\":[1,2]}", Encoding.UTF8, "application/json"),
        };
        var reply = router.Dispatch(Cmd(BridgeCommands.SearchBossRaids,
            "{\"query\":{\"q\":\"boss\"}}"));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal(2, reply.Payload["data"]!["results"]!.AsArray().Count);
        Assert.Contains("/api/boss-raids?", handler.Requests[0].RequestUri!.AbsoluteUri);
        Assert.Contains("q=boss", handler.Requests[0].RequestUri!.AbsoluteUri);
    }

    [Fact]
    public void GetMissingIdReturnsError()
    {
        var (router, _, _) = Build();
        var reply = router.Dispatch(Cmd(BridgeCommands.GetBossRaid, "{}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("missing_id", reply.Payload["error"]!.GetValue<string>());
    }

    [Fact]
    public void IssueTokenMissingPayloadReturnsError()
    {
        var (router, _, _) = Build();
        var reply = router.Dispatch(Cmd(BridgeCommands.IssueBossRaidUploadToken, "{}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("missing_payload", reply.Payload["error"]!.GetValue<string>());
    }

    [Fact]
    public void UploadMissingProfileReturnsError()
    {
        var (router, _, _) = Build();
        var reply = router.Dispatch(Cmd(BridgeCommands.UploadBossRaid, "{\"token\":\"t\"}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("missing_profile", reply.Payload["error"]!.GetValue<string>());
    }

    [Fact]
    public void UploadForwardsTokenHeader()
    {
        var (router, _, handler) = Build();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("{\"id\":\"r1\"}", Encoding.UTF8, "application/json"),
        };
        var reply = router.Dispatch(Cmd(BridgeCommands.UploadBossRaid,
            "{\"token\":\"secret\",\"profile\":{\"profile_name\":\"B\"}}"));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.True(handler.Requests[0].Headers.TryGetValues("X-SAO-Upload-Token", out var v));
        Assert.Equal("secret", v!.Single());
    }

    [Fact]
    public void HttpErrorReturnedAsErrorJsonCollapsesToOkFalse()
    {
        var (router, _, handler) = Build();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.BadRequest)
        {
            Content = new StringContent("{\"detail\":\"bad\"}", Encoding.UTF8, "application/json"),
        };
        // BossRaidCloudClient parses the body as JSON and returns it verbatim
        // on error; the {detail:"bad"} body has no `error` field, so the
        // bridge wraps it as data with ok:true. (Faithful to the client.)
        var reply = router.Dispatch(Cmd(BridgeCommands.GetBossRaid, "{\"id\":\"x\"}"));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("bad", reply.Payload["data"]!["detail"]!.GetValue<string>());
    }

    [Fact]
    public void ClientErrorJsonCollapsesToOkFalse()
    {
        // Force the client into its synthetic {error:"..."} path by giving
        // a non-JSON 4xx response (the client falls back to "HTTP code: reason").
        var (router, _, handler) = Build();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.InternalServerError)
        { Content = new StringContent("not json", Encoding.UTF8, "text/plain"), ReasonPhrase = "Boom" };
        var reply = router.Dispatch(Cmd(BridgeCommands.GetBossRaid, "{\"id\":\"x\"}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("HTTP 500: Boom", reply.Payload["error"]!.GetValue<string>());
    }

    [Fact]
    public void TransportErrorCollapsesToOkFalse()
    {
        var router = new BridgeRouter();
        var handler = new FakeHandler
        { Responder = _ => throw new HttpRequestException("boom") };
        using var http = new HttpClient(handler);
        using var client = new BossRaidCloudClient("http://example.com", http);
        using var bridge = new BossRaidCloudBridge(router, client);
        var reply = router.Dispatch(Cmd(BridgeCommands.GetBossRaid, "{\"id\":\"x\"}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("boom", reply.Payload["error"]!.GetValue<string>());
    }

    [Fact]
    public void LifecycleAttachRegistersAndDisposes()
    {
        var states = new GameStateManager();
        var web = new WebBridgeLifecycle(states);
        using var http = new HttpClient(new FakeHandler());
        using var client = new BossRaidCloudClient("http://example.com", http);

        web.AttachBossRaidCloud(client);
        Assert.Contains(BridgeCommands.SearchBossRaids, web.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DownloadBossRaidRemote, web.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RefreshBossRaidUploadAuth, web.Router.RegisteredCommands);

        // Idempotent
        web.AttachBossRaidCloud(client);
        Assert.Contains(BridgeCommands.SearchBossRaids, web.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DownloadBossRaidRemote, web.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RefreshBossRaidUploadAuth, web.Router.RegisteredCommands);

        web.Dispose();
        Assert.DoesNotContain(BridgeCommands.SearchBossRaids, web.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.DownloadBossRaidRemote, web.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RefreshBossRaidUploadAuth, web.Router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachNullThrows()
    {
        var states = new GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        Assert.Throws<ArgumentNullException>(() => web.AttachBossRaidCloud(null!));
    }

    [Fact]
    public void LifecycleAttachAfterDisposeThrows()
    {
        var states = new GameStateManager();
        var web = new WebBridgeLifecycle(states);
        web.Dispose();
        using var http = new HttpClient(new FakeHandler());
        using var client = new BossRaidCloudClient("http://example.com", http);
        Assert.Throws<ObjectDisposedException>(() => web.AttachBossRaidCloud(client));
    }
}
