using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

/// <summary>
/// S183 — Mirror of S182 for the boss-raid side. Successful
/// <c>bossraid.cloud.search</c> writes the query + results + UTC
/// timestamp into <c>boss_raid.last_remote_search</c>.
/// </summary>
public class Session183BossRaidCloudPersistTests : IDisposable
{
    private readonly string _workDir;

    public Session183BossRaidCloudPersistTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s183-" + Guid.NewGuid().ToString("N")[..8]);
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

    private (BridgeRouter router, BossRaidCloudBridge bridge, SettingsManager settings) Build(
        string body, Func<DateTimeOffset> clock)
    {
        var router = new BridgeRouter();
        var handler = new FakeHandler
        {
            Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
            { Content = new StringContent(body, Encoding.UTF8, "application/json") },
        };
        var http = new HttpClient(handler);
        var client = new BossRaidCloudClient("http://example.com", http);
        var settingsPath = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(settingsPath, "{}");
        var settings = new SettingsManager(settingsPath);
        var bridge = new BossRaidCloudBridge(router, client, settings, clock);
        return (router, bridge, settings);
    }

    private static BridgeMessage Cmd(string name, string payloadJson) =>
        new(BridgeMessage.TypeCommand, name, JsonSerializer.Deserialize<JsonObject>(payloadJson));

    [Fact]
    public void SuccessfulSearchPersistsQueryAndResults()
    {
        var fixedNow = new DateTimeOffset(2026, 5, 20, 6, 0, 0, TimeSpan.Zero);
        var (router, bridge, settings) = Build(
            "{\"results\":[{\"id\":\"b1\"},{\"id\":\"b2\"},{\"id\":\"b3\"}]}",
            () => fixedNow);
        using var _ = bridge;

        var reply = router.Dispatch(Cmd(BridgeCommands.SearchBossRaids,
            "{\"query\":{\"q\":\"dragon\",\"page\":3,\"page_size\":50}}"));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.Equal(3, reply.Payload["results"]!.AsArray().Count);
        Assert.Equal(3, reply.Payload["state"]!["last_remote_search"]!["results"]!.AsArray().Count);

        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));
        Assert.Equal("dragon", reloaded.LastRemoteSearch.Query.Q);
        Assert.Equal(3, reloaded.LastRemoteSearch.Query.Page);
        Assert.Equal(50, reloaded.LastRemoteSearch.Query.PageSize);
        Assert.Equal(3, reloaded.LastRemoteSearch.Results.Count);
        Assert.Equal("2026-05-20T06:00:00Z", reloaded.LastRemoteSearch.FetchedAt);
    }

    [Fact]
    public void SearchReturnsItemsAsFrontendResultsAndState()
    {
        var fixedNow = new DateTimeOffset(2026, 5, 20, 6, 0, 0, TimeSpan.Zero);
        var (router, bridge, settings) = Build(
            "{\"items\":[{\"id\":\"b1\",\"profile_name\":\"Dragon\"}]}",
            () => fixedNow);
        using var _ = bridge;

        var reply = router.Dispatch(Cmd(BridgeCommands.SearchBossRaids,
            "{\"query\":{\"q\":\"dragon\"}}"));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();
        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Single(payload["results"]!.AsArray());
        Assert.Equal("Dragon", state["last_remote_search"]!["results"]!.AsArray()[0]!["profile_name"]!.GetValue<string>());
        Assert.Single(reloaded.LastRemoteSearch.Results);
    }

    [Fact]
    public void FailedSearchDoesNotPersist()
    {
        var fixedNow = new DateTimeOffset(2026, 5, 20, 0, 0, 0, TimeSpan.Zero);
        var (router, bridge, settings) = Build(
            "not-json",
            () => fixedNow);
        using var _ = bridge;

        var reply = router.Dispatch(Cmd(BridgeCommands.SearchBossRaids,
            "{\"query\":{\"q\":\"fail\"}}"));
        // Faithfully: the BossRaidCloudClient's `invalid JSON: ...` path
        // surfaces as an InvalidOperationException; the bridge collapses
        // to ok:false.
        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());

        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));
        Assert.Equal(string.Empty, reloaded.LastRemoteSearch.Query.Q);
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
        using var client = new BossRaidCloudClient("http://example.com", http);
        using var bridge = new BossRaidCloudBridge(router, client);
        var reply = router.Dispatch(Cmd(BridgeCommands.SearchBossRaids,
            "{\"query\":{\"q\":\"x\"}}"));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        // No persistence side effect to verify; merely that the call doesn't NRE.
    }

    [Fact]
    public void PersistedSearchPreservesExistingProfiles()
    {
        var fixedNow = new DateTimeOffset(2026, 5, 20, 0, 0, 0, TimeSpan.Zero);
        var (router, bridge, settings) = Build(
            "{\"results\":[]}",
            () => fixedNow);
        using var _ = bridge;

        var seeded = BossRaidProfile.DefaultConfig() with { Enabled = true };
        BossRaidConfigStore.Save(settings, seeded);

        router.Dispatch(Cmd(BridgeCommands.SearchBossRaids, "{\"query\":{\"q\":\"after-seed\"}}"));

        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));
        Assert.True(reloaded.Enabled);
        Assert.Equal("after-seed", reloaded.LastRemoteSearch.Query.Q);
    }

    [Fact]
    public void DownloadRemotePersistsProfileAndReturnsMenuState()
    {
        var fixedNow = new DateTimeOffset(2026, 5, 20, 0, 0, 0, TimeSpan.Zero);
        var (router, bridge, settings) = Build(
            "{\"id\":\"remote-1\",\"profile\":{\"id\":\"remote-profile\",\"profile_name\":\"Remote Boss\",\"boss_total_hp\":900,\"phases\":[]}}",
            () => fixedNow);
        using var _ = bridge;

        var reply = router.Dispatch(Cmd(BridgeCommands.DownloadBossRaidRemote,
            "{\"id\":\"remote-1\"}"));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();
        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));
        var saved = Assert.Single(reloaded.Profiles);

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("remote-profile", payload["profile_id"]!.GetValue<string>());
        Assert.Equal("remote-1", payload["remote_id"]!.GetValue<string>());
        Assert.Equal("Remote Boss", saved.ProfileName);
        Assert.Equal("downloaded", saved.Source);
        Assert.Equal("remote-1", saved.RemoteId);
        Assert.Equal(1, state["local_profile_count"]!.GetValue<int>());
        Assert.Equal("Remote Boss", state["profiles"]!.AsArray()[0]!["profile_name"]!.GetValue<string>());
    }

    [Fact]
    public void RefreshUploadAuthIssuesTokenAndReturnsMaskedState()
    {
        var router = new BridgeRouter();
        var handler = new FakeHandler
        {
            Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent(
                    "{\"token\":\"abcdefghijkl\",\"expires_at\":\"2026-05-21T00:00:00Z\",\"mode\":\"test\"}",
                    Encoding.UTF8,
                    "application/json"),
            },
        };
        using var http = new HttpClient(handler);
        using var client = new BossRaidCloudClient("http://example.com", http);
        var settingsPath = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(settingsPath, "{}");
        var settings = new SettingsManager(settingsPath);
        var states = new GameStateManager();
        states.Replace(new GameState
        {
            PlayerId = "u1",
            PlayerName = "Asuna",
            ProfessionId = 7,
            ProfessionName = "Rapier",
        });
        using var bridge = new BossRaidCloudBridge(router, client, settings, states: states);

        var reply = router.Dispatch(Cmd(BridgeCommands.RefreshBossRaidUploadAuth,
            "{\"force\":true}"));
        var payload = reply!.Payload!;
        var auth = payload["upload_auth"]!.AsObject();
        var stateAuth = payload["state"]!["upload_auth"]!.AsObject();

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.True(auth["ready"]!.GetValue<bool>());
        Assert.Equal("abcd...ijkl", auth["token_masked"]!.GetValue<string>());
        Assert.Equal(string.Empty, auth["token"]!.GetValue<string>());
        Assert.Equal("test", auth["mode"]!.GetValue<string>());
        Assert.Equal("u1", stateAuth["identity"]!["player_uid"]!.GetValue<string>());
    }

    [Fact]
    public void UploadProfileRefreshesTokenAndPersistsRemoteId()
    {
        var router = new BridgeRouter();
        var handler = new FakeHandler
        {
            Responder = req =>
            {
                if (req.RequestUri!.AbsolutePath.EndsWith("/api/upload-token/issue", StringComparison.Ordinal))
                {
                    return new HttpResponseMessage(HttpStatusCode.OK)
                    {
                        Content = new StringContent(
                            "{\"token\":\"upload-token\",\"expires_at\":\"2026-05-21T00:00:00Z\",\"mode\":\"test\"}",
                            Encoding.UTF8,
                            "application/json"),
                    };
                }
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent("{\"id\":\"remote-upload\"}", Encoding.UTF8, "application/json"),
                };
            },
        };
        using var http = new HttpClient(handler);
        using var client = new BossRaidCloudClient("http://example.com", http);
        var settingsPath = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(settingsPath, "{}");
        var settings = new SettingsManager(settingsPath);
        var profile = BossRaidProfile.MakeDefaultProfile() with
        {
            Id = "br-upload",
            ProfileName = "Upload Boss",
            BossTotalHp = 1200,
        };
        BossRaidConfigStore.Save(
            settings,
            BossRaidProfile.UpsertProfile(BossRaidProfile.DefaultConfig() with { Enabled = true }, profile, activate: true));
        var states = new GameStateManager();
        states.Replace(new GameState
        {
            PlayerId = "u1",
            PlayerName = "Asuna",
            ProfessionId = 7,
            ProfessionName = "Rapier",
        });
        using var bridge = new BossRaidCloudBridge(router, client, settings, states: states);

        var reply = router.Dispatch(Cmd(BridgeCommands.UploadBossRaid,
            "{\"id\":\"br-upload\"}"));
        var payload = reply!.Payload!;
        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));
        var saved = BossRaidProfile.FindProfile(reloaded, "br-upload")!;

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("remote-upload", payload["remote_id"]!.GetValue<string>());
        Assert.Equal("uploaded", saved.Source);
        Assert.Equal("remote-upload", saved.RemoteId);
        Assert.Equal("remote-upload", payload["state"]!["active_profile"]!["remote_id"]!.GetValue<string>());
        Assert.Equal(2, handler.Requests.Count);
        Assert.True(handler.Requests[1].Headers.TryGetValues("X-SAO-Upload-Token", out var values));
        Assert.Equal("upload-token", values!.Single());
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
        using var initialClient = new BossRaidCloudClient("http://old.example", http);
        var settingsPath = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(settingsPath, "{}");
        var settings = new SettingsManager(settingsPath);
        using var bridge = new BossRaidCloudBridge(
            router,
            initialClient,
            settings,
            clientFromSettings: sm => BossRaidCloudClient.FromSettings(sm, http));

        var saved = router.Dispatch(Cmd(BridgeCommands.SetBossRaidServerUrl,
            "{\"url\":\"http://raid-new.example/\"}"));
        Assert.True(saved!.Payload!["ok"]!.GetValue<bool>());

        router.Dispatch(Cmd(BridgeCommands.SearchBossRaids, "{\"query\":{\"q\":\"after\"}}"));

        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));
        Assert.Equal("http://raid-new.example/", reloaded.ServerUrl);
        Assert.StartsWith("http://raid-new.example/api/boss-raids?", handler.Requests.Single().RequestUri!.AbsoluteUri);
    }
}
