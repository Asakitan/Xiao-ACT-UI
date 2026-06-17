using System.Net;
using System.Text;
using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

public sealed class AutoKeyCloudContributorTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public AutoKeyCloudContributorTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-autokey-cloud-contributor-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { }
    }

    private SettingsManager NewSettings() => new(_path);

    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    private static AutoKeyCloudClient NewClient(FakeHandler handler, string baseUrl = "http://example.com")
        => new(baseUrl, new HttpClient(handler));

    [Fact]
    public void AttachContributorRegistersAutoKeyCloudCommands()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var handler = new FakeHandler();

        lifecycle.AttachContributor(new AutoKeyCloudContributor(_ => NewClient(handler)), NewSettings());

        Assert.Contains(BridgeCommands.SearchAutoKeyScripts, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.GetAutoKeyScript, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.IssueAutoKeyUploadToken, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.UploadAutoKeyScript, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetAutoKeyServerUrl, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AutoKeyCloudContributorRoutesSearchThroughInjectedFactory()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var settings = NewSettings();
        var handler = new FakeHandler
        {
            Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent("{\"results\":[{\"id\":\"r1\"}]}", Encoding.UTF8, "application/json"),
            },
        };
        var factoryCalls = 0;
        AutoKeyCloudClient Factory(SettingsManager _)
        {
            factoryCalls++;
            return NewClient(handler);
        }

        lifecycle.AttachContributor(new AutoKeyCloudContributor(Factory), settings);
        var reply = lifecycle.Router.Dispatch(Cmd(
            BridgeCommands.SearchAutoKeyScripts,
            new JsonObject { ["query"] = new JsonObject { ["q"] = "healer", ["page"] = "2" } }));

        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("r1", reply.Payload["data"]!["results"]![0]!["id"]!.GetValue<string>());
        Assert.Equal(2, factoryCalls);
        Assert.Contains("q=healer", handler.Requests.Single().RequestUri!.AbsoluteUri);
        Assert.Contains("page=2", handler.Requests.Single().RequestUri!.AbsoluteUri);
    }

    [Fact]
    public void SetServerUrlPersistsAndNextCommandUsesFactoryFromSettings()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var settings = NewSettings();
        var handler = new FakeHandler
        {
            Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent("{\"results\":[]}", Encoding.UTF8, "application/json"),
            },
        };
        var seenBaseUrls = new List<string>();
        AutoKeyCloudClient Factory(SettingsManager currentSettings)
        {
            var client = AutoKeyCloudClient.FromSettings(currentSettings, new HttpClient(handler));
            seenBaseUrls.Add(client.BaseUrl);
            return client;
        }

        lifecycle.AttachContributor(new AutoKeyCloudContributor(Factory), settings);
        var setReply = lifecycle.Router.Dispatch(Cmd(
            BridgeCommands.SetAutoKeyServerUrl,
            new JsonObject { ["url"] = "http://cloud.local:1234/" }));
        var searchReply = lifecycle.Router.Dispatch(Cmd(
            BridgeCommands.SearchAutoKeyScripts,
            new JsonObject { ["query"] = new JsonObject { ["q"] = "after-url-change" } }));

        Assert.True(setReply!.Payload!["ok"]!.GetValue<bool>());
        Assert.True(searchReply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal("http://cloud.local:1234/", setReply.Payload["server_url"]!.GetValue<string>());
        Assert.Contains("http://cloud.local:1234", seenBaseUrls);
        Assert.StartsWith("http://cloud.local:1234/api/scripts", handler.Requests.Single().RequestUri!.AbsoluteUri);
        using var reloaded = AutoKeyCloudClient.FromSettings(settings, new HttpClient(handler));
        Assert.Equal("http://cloud.local:1234", reloaded.BaseUrl);
    }

    [Fact]
    public void AutoKeyCloudContributorAttachmentCanBeDisposed()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var handler = new FakeHandler();
        var attachment = lifecycle.AttachContributor(new AutoKeyCloudContributor(_ => NewClient(handler)), NewSettings());

        attachment.Dispose();

        Assert.DoesNotContain(BridgeCommands.SearchAutoKeyScripts, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.GetAutoKeyScript, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetAutoKeyServerUrl, lifecycle.Router.RegisteredCommands);
    }

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
}
