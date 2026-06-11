using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.App;

/// <summary>
/// S182 — Pin <see cref="AutoKeyCloudBridge"/>'s search-persistence
/// path. On a successful <c>autokey.cloud.search</c> reply, the
/// bridge writes the query + results + fetched_at to
/// <c>auto_key.last_remote_search</c>.
/// </summary>
public class Session182AutoKeyCloudPersistTests : IDisposable
{
    private readonly string _workDir;

    public Session182AutoKeyCloudPersistTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s182-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(_workDir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
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

    private (BridgeRouter router, AutoKeyCloudBridge bridge, SettingsManager settings) Build(
        string body, Func<DateTimeOffset> clock)
    {
        var router = new BridgeRouter();
        var handler = new FakeHandler
        {
            Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
            { Content = new StringContent(body, Encoding.UTF8, "application/json") },
        };
        var http = new HttpClient(handler);
        var client = new AutoKeyCloudClient("http://example.com", http);
        var settingsPath = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(settingsPath, "{}");
        var settings = new SettingsManager(settingsPath);
        var bridge = new AutoKeyCloudBridge(router, client, settings, clock);
        return (router, bridge, settings);
    }

    private static BridgeMessage Cmd(string name, string payloadJson) =>
        new(BridgeMessage.TypeCommand, name, JsonSerializer.Deserialize<JsonObject>(payloadJson));

    [Fact]
    public void SuccessfulSearchPersistsQueryAndResults()
    {
        var fixedNow = new DateTimeOffset(2026, 5, 20, 12, 34, 56, TimeSpan.Zero);
        var (router, bridge, settings) = Build(
            "{\"results\":[{\"id\":\"r1\"},{\"id\":\"r2\"}]}",
            () => fixedNow);
        using var _ = bridge;

        var reply = router.Dispatch(Cmd(BridgeCommands.SearchAutoKeyScripts,
            "{\"query\":{\"q\":\"fire\",\"page\":2,\"page_size\":25}}"));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());

        var reloaded = AutoKeyConfigLoader.Load(new SettingsManager(settings.Path));
        Assert.Equal("fire", reloaded.LastRemoteSearch.Query.Q);
        Assert.Equal(2, reloaded.LastRemoteSearch.Query.Page);
        Assert.Equal(25, reloaded.LastRemoteSearch.Query.PageSize);
        Assert.Equal(2, reloaded.LastRemoteSearch.Results.Length);
        Assert.Equal("2026-05-20T12:34:56Z", reloaded.LastRemoteSearch.FetchedAt);
        Assert.Equal(string.Empty, reloaded.LastRemoteSearch.Error);
    }

    [Fact]
    public void FailedSearchDoesNotPersist()
    {
        var fixedNow = new DateTimeOffset(2026, 5, 20, 12, 34, 56, TimeSpan.Zero);
        var (router, bridge, settings) = Build(
            "not-json-at-all",
            () => fixedNow);
        using var _ = bridge;

        var reply = router.Dispatch(Cmd(BridgeCommands.SearchAutoKeyScripts,
            "{\"query\":{\"q\":\"fire\"}}"));
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());

        var reloaded = AutoKeyConfigLoader.Load(new SettingsManager(settings.Path));
        Assert.Equal(string.Empty, reloaded.LastRemoteSearch.Query.Q);
        Assert.Equal(string.Empty, reloaded.LastRemoteSearch.FetchedAt);
    }

    [Fact]
    public void NoSettingsArgumentSkipsPersistence()
    {
        var router = new BridgeRouter();
        var handler = new FakeHandler
        {
            Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
            { Content = new StringContent("{\"results\":[]}", Encoding.UTF8, "application/json") },
        };
        var http = new HttpClient(handler);
        using var client = new AutoKeyCloudClient("http://example.com", http);
        // Pass no settings — exercise the existing 2-arg ctor path.
        using var bridge = new AutoKeyCloudBridge(router, client);
        var reply = router.Dispatch(Cmd(BridgeCommands.SearchAutoKeyScripts,
            "{\"query\":{\"q\":\"x\"}}"));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        // No persistence side effect to assert; merely that the call doesn't NRE.
    }

    [Fact]
    public void PersistedSearchPreservesExistingProfiles()
    {
        var fixedNow = new DateTimeOffset(2026, 5, 20, 0, 0, 0, TimeSpan.Zero);
        var (router, bridge, settings) = Build(
            "{\"results\":[]}",
            () => fixedNow);
        using var _ = bridge;

        // Seed an existing config with one profile + enabled=true.
        var seeded = AutoKeyProfileStore.DefaultConfig() with { Enabled = true };
        AutoKeyConfigLoader.Save(settings, seeded);
        settings.Save();

        router.Dispatch(Cmd(BridgeCommands.SearchAutoKeyScripts, "{\"query\":{\"q\":\"new\"}}"));

        var reloaded = AutoKeyConfigLoader.Load(new SettingsManager(settings.Path));
        Assert.True(reloaded.Enabled);
        Assert.Equal("new", reloaded.LastRemoteSearch.Query.Q);
    }

    [Fact]
    public void SetServerUrlPersistsAndNextSearchUsesUpdatedSettings()
    {
        var router = new BridgeRouter();
        var handler = new FakeHandler
        {
            Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
            { Content = new StringContent("{\"results\":[]}", Encoding.UTF8, "application/json") },
        };
        using var http = new HttpClient(handler);
        using var initialClient = new AutoKeyCloudClient("http://old.example", http);
        var settingsPath = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(settingsPath, "{}");
        var settings = new SettingsManager(settingsPath);
        using var bridge = new AutoKeyCloudBridge(
            router,
            initialClient,
            settings,
            clientFromSettings: sm => AutoKeyCloudClient.FromSettings(sm, http));

        var saved = router.Dispatch(Cmd(BridgeCommands.SetAutoKeyServerUrl,
            "{\"url\":\"http://new.example/\"}"));
        Assert.True(saved!.Payload!["ok"]!.GetValue<bool>());

        router.Dispatch(Cmd(BridgeCommands.SearchAutoKeyScripts, "{\"query\":{\"q\":\"after\"}}"));

        var reloaded = AutoKeyConfigLoader.Load(new SettingsManager(settings.Path));
        Assert.Equal("http://new.example/", reloaded.ServerUrl);
        Assert.StartsWith("http://new.example/api/scripts?", handler.Requests.Single().RequestUri!.AbsoluteUri);
    }
}
