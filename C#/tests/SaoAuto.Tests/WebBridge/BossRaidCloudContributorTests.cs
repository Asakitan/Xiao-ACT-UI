using System.Net;
using System.Text;
using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

public sealed class BossRaidCloudContributorTests : IDisposable
{
    private sealed class FakeHandler : HttpMessageHandler
    {
        public List<HttpRequestMessage> Requests { get; } = new();
        public Func<HttpRequestMessage, HttpResponseMessage> Responder { get; set; } = _ =>
            new HttpResponseMessage(HttpStatusCode.OK)
            { Content = new StringContent("{}", Encoding.UTF8, "application/json") };

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            Requests.Add(request);
            return Task.FromResult(Responder(request));
        }
    }

    private readonly string _workDir;
    private readonly FakeHandler _handler = new();
    private readonly HttpClient _http;
    private readonly List<string> _factorySettingsPaths = new();

    public BossRaidCloudContributorTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-bossraid-cloud-contributor-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _http = new HttpClient(_handler);
    }

    public void Dispose()
    {
        _http.Dispose();
        try { Directory.Delete(_workDir, recursive: true); } catch { }
    }

    private SettingsManager NewSettings()
    {
        var path = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(path, "{}");
        return new SettingsManager(path);
    }

    private BossRaidCloudClient NewClient(SettingsManager settings)
    {
        _factorySettingsPaths.Add(settings.Path);
        return new BossRaidCloudClient("http://example.com", _http);
    }

    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void AttachContributorRegistersBossRaidCloudCommands()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachContributor(new BossRaidCloudContributor(NewClient), NewSettings());

        Assert.Contains(BridgeCommands.SearchBossRaids, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.GetBossRaid, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DownloadBossRaidRemote, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.RefreshBossRaidUploadAuth, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.IssueBossRaidUploadToken, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.UploadBossRaid, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBossRaidServerUrl, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void BossRaidCloudContributorRoutesThroughCloudBridgeFactory()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var settings = NewSettings();
        _handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("{\"results\":[{\"id\":\"r1\"}]}", Encoding.UTF8, "application/json"),
        };

        lifecycle.AttachContributor(new BossRaidCloudContributor(NewClient), settings);
        var reply = lifecycle.Router.Dispatch(Cmd(
            BridgeCommands.SearchBossRaids,
            new JsonObject { ["query"] = new JsonObject { ["q"] = "boss" } }));

        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Single(reply.Payload["results"]!.AsArray());
        Assert.Contains("/api/boss-raids?", _handler.Requests[0].RequestUri!.AbsoluteUri);
        Assert.Contains("q=boss", _handler.Requests[0].RequestUri!.AbsoluteUri);
        Assert.All(_factorySettingsPaths, path => Assert.Equal(settings.Path, path));
    }

    [Fact]
    public void BossRaidCloudContributorPassesContextStatesToCloudBridge()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachContributor(new BossRaidCloudContributor(NewClient), NewSettings());
        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.RefreshBossRaidUploadAuth));
        var identity = reply!.Payload!["upload_auth"]!["identity"]!.AsObject();

        Assert.False(reply.Payload["ok"]!.GetValue<bool>());
        Assert.Equal("packet", identity["source"]!.GetValue<string>());
        Assert.Empty(_handler.Requests);
    }

    [Fact]
    public void BossRaidCloudContributorAttachmentCanBeDisposed()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var attachment = lifecycle.AttachContributor(new BossRaidCloudContributor(NewClient), NewSettings());

        attachment.Dispose();

        Assert.DoesNotContain(BridgeCommands.SearchBossRaids, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.GetBossRaid, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.DownloadBossRaidRemote, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RefreshBossRaidUploadAuth, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.IssueBossRaidUploadToken, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.UploadBossRaid, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBossRaidServerUrl, lifecycle.Router.RegisteredCommands);
    }
}
