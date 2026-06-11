using System.Collections.Immutable;
using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

public class Session201MenuStateBridgeTests : IDisposable
{
    private readonly string _workDir;

    public Session201MenuStateBridgeTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s201-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(_workDir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { }
    }

    private SettingsManager NewSettings()
    {
        var path = Path.Combine(_workDir, $"settings-{Guid.NewGuid():N}.json");
        File.WriteAllText(path, "{}");
        return new SettingsManager(path);
    }

    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void AutoKeyStateReturnsFullPythonCompatibleMenuPayload()
    {
        var router = new BridgeRouter();
        var settings = NewSettings();
        var states = new GameStateManager();
        states.Replace(new GameState
        {
            PlayerId = "uid-1",
            PlayerName = "Asuna",
            ProfessionId = 7,
            ProfessionName = "Rapier",
        });
        var author = new AuthorSnapshot("uid-1", "Asuna", 7, "Rapier");
        var profile = AutoKeyProfileSpec.MakeDefaultProfile(author, newId: () => "fixed001")
            with
            {
                Id = "ak-1",
                ProfileName = "Auto Alpha",
                Actions = ImmutableArray.Create(
                    AutoKeyProfileSpec.MakeDefaultAction(3, newId: () => "act003")),
            };
        var config = AutoKeyProfileStore.UpsertProfile(
            AutoKeyProfileStore.DefaultConfig() with { Enabled = true },
            profile,
            activate: true);
        AutoKeyConfigLoader.Save(settings, config);
        settings.Save();

        using var bridge = new MenuStateBridge(router, settings, states);
        var reply = router.Dispatch(Cmd(BridgeCommands.GetAutoKeyState));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.True(state["enabled"]!.GetValue<bool>());
        Assert.Equal("ak-1", state["active_profile_id"]!.GetValue<string>());
        Assert.Equal("Auto Alpha", state["active_profile_name"]!.GetValue<string>());
        Assert.Equal(1, state["local_profile_count"]!.GetValue<int>());
        Assert.Equal(AutoKeyProfileSpec.DefaultServerUrl, state["server_url"]!.GetValue<string>());

        var profilesFull = state["profiles_full"]!.AsArray();
        Assert.Equal("Auto Alpha", profilesFull[0]!["profile_name"]!.GetValue<string>());
        Assert.Equal(3, profilesFull[0]!["actions"]!.AsArray()[0]!["slot_index"]!.GetValue<int>());
        Assert.Equal("ak-1", state["active_profile"]!["id"]!.GetValue<string>());
        Assert.Equal("Auto Alpha", state["profiles"]!.AsArray()[0]!["profile_name"]!.GetValue<string>());

        var identity = state["identity"]!.AsObject();
        Assert.True(identity["ready"]!.GetValue<bool>());
        Assert.Equal("uid-1", identity["player_uid"]!.GetValue<string>());
        Assert.Equal("packet", identity["source"]!.GetValue<string>());

        var uploadAuth = state["upload_auth"]!.AsObject();
        Assert.False(uploadAuth["ready"]!.GetValue<bool>());
        Assert.Equal("uid-1", uploadAuth["identity"]!["player_uid"]!.GetValue<string>());
        Assert.NotNull(state["last_remote_search"]!["query"]);
        Assert.NotNull(state["runtime"]);
    }

    [Fact]
    public void BossRaidStateKeepsTimelineTimeSAndIdentity()
    {
        var router = new BridgeRouter();
        var settings = NewSettings();
        var states = new GameStateManager();
        states.Replace(new GameState
        {
            PlayerId = "uid-2",
            PlayerName = "Kirito",
            ProfessionId = 9,
            ProfessionName = "Sword",
        });
        var profile = BossRaidProfile.NormalizeProfile(JsonDocument.Parse("""
        {
          "id": "br-1",
          "profile_name": "Boss Alpha",
          "phases": [
            {
              "id": "p1",
              "timelines": [
                { "id": "tl1", "time_s": 12.5, "label": "Move", "alert_type": "both" }
              ]
            }
          ]
        }
        """).RootElement);
        var config = BossRaidProfile.UpsertProfile(
            BossRaidProfile.DefaultConfig() with { Enabled = true },
            profile,
            activate: true);
        BossRaidConfigStore.Save(settings, config);

        using var bridge = new MenuStateBridge(router, settings, states);
        var reply = router.Dispatch(Cmd(BridgeCommands.GetBossRaidState));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.True(state["enabled"]!.GetValue<bool>());
        Assert.Equal("br-1", state["active_profile_id"]!.GetValue<string>());
        Assert.Equal("Boss Alpha", state["active_profile_name"]!.GetValue<string>());
        Assert.Equal(1, state["local_profile_count"]!.GetValue<int>());

        var timeline = state["profiles_full"]!.AsArray()[0]!["phases"]!.AsArray()[0]!["timelines"]!.AsArray()[0]!.AsObject();
        Assert.Equal(12.5, timeline["time_s"]!.GetValue<double>());
        Assert.False(timeline.ContainsKey("time_seconds"));
        Assert.Equal("br-1", state["active_profile"]!["id"]!.GetValue<string>());
        Assert.Equal("Boss Alpha", state["profiles"]!.AsArray()[0]!["profile_name"]!.GetValue<string>());

        var identity = state["identity"]!.AsObject();
        Assert.True(identity["ready"]!.GetValue<bool>());
        Assert.Equal("uid-2", identity["player_uid"]!.GetValue<string>());
        Assert.Equal("packet", identity["source"]!.GetValue<string>());

        var uploadAuth = state["upload_auth"]!.AsObject();
        Assert.False(uploadAuth["ready"]!.GetValue<bool>());
        Assert.Equal(BossRaidProfile.DefaultServerUrl, uploadAuth["server_url"]!.GetValue<string>());
        Assert.NotNull(state["last_remote_search"]!["query"]);
        Assert.NotNull(state["runtime"]);
    }

    [Fact]
    public void SetBossRaidEnabledPersistsAndReturnsFreshState()
    {
        var router = new BridgeRouter();
        var settings = NewSettings();
        BossRaidConfigStore.Save(settings, BossRaidProfile.DefaultConfig() with { Enabled = false });
        using var bridge = new MenuStateBridge(router, settings, new GameStateManager());

        var reply = router.Dispatch(Cmd(BridgeCommands.SetBossRaidEnabled, new JsonObject
        {
            ["enabled"] = true,
        }));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.True(payload["enabled"]!.GetValue<bool>());
        Assert.True(state["enabled"]!.GetValue<bool>());
        Assert.True(BossRaidConfigStore.Load(new SettingsManager(settings.Path)).Enabled);
    }

    [Fact]
    public void SetBossRaidActiveProfilePersistsAndReturnsFreshState()
    {
        var router = new BridgeRouter();
        var settings = NewSettings();
        var alpha = BossRaidProfile.MakeDefaultProfile() with { Id = "br-a", ProfileName = "Alpha" };
        var beta = BossRaidProfile.MakeDefaultProfile() with { Id = "br-b", ProfileName = "Beta" };
        var config = BossRaidProfile.UpsertProfile(BossRaidProfile.DefaultConfig(), alpha, activate: true);
        config = BossRaidProfile.UpsertProfile(config, beta);
        BossRaidConfigStore.Save(settings, config);
        using var bridge = new MenuStateBridge(router, settings, new GameStateManager());

        var reply = router.Dispatch(Cmd(BridgeCommands.SetBossRaidActiveProfile, new JsonObject
        {
            ["id"] = "br-b",
        }));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("br-b", state["active_profile_id"]!.GetValue<string>());
        Assert.Equal("Beta", state["active_profile_name"]!.GetValue<string>());
        Assert.Equal("br-b", BossRaidConfigStore.Load(new SettingsManager(settings.Path)).ActiveProfileId);
    }

    [Fact]
    public void CreateBossRaidProfilePersistsAndActivatesDefaultProfile()
    {
        var router = new BridgeRouter();
        var settings = NewSettings();
        using var bridge = new MenuStateBridge(router, settings, new GameStateManager());

        var reply = router.Dispatch(Cmd(BridgeCommands.CreateBossRaidProfile));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();
        var profileId = payload["profile_id"]!.GetValue<string>();
        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.False(string.IsNullOrWhiteSpace(profileId));
        Assert.Equal(profileId, state["active_profile_id"]!.GetValue<string>());
        Assert.Equal("New Boss Raid", state["active_profile_name"]!.GetValue<string>());
        Assert.Single(reloaded.Profiles);
        Assert.Equal(profileId, reloaded.ActiveProfileId);
    }

    [Fact]
    public void SaveBossRaidProfilePersistsNormalizedPayloadAndPreservesCreatedAt()
    {
        var router = new BridgeRouter();
        var settings = NewSettings();
        var original = BossRaidProfile.MakeDefaultProfile() with
        {
            Id = "br-save",
            ProfileName = "Before",
            CreatedAt = "2026-01-01T00:00:00Z",
        };
        var config = BossRaidProfile.UpsertProfile(BossRaidProfile.DefaultConfig(), original, activate: true);
        BossRaidConfigStore.Save(settings, config);
        using var bridge = new MenuStateBridge(router, settings, new GameStateManager());

        var reply = router.Dispatch(Cmd(BridgeCommands.SaveBossRaidProfile, new JsonObject
        {
            ["profile"] = JsonNode.Parse("""
            {
              "id": "br-save",
              "profile_name": "After",
              "phases": [
                {
                  "id": "p1",
                  "timelines": [
                    { "id": "tl1", "time_s": 8.5, "label": "Stack" }
                  ]
                }
              ]
            }
            """),
        }));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();
        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));
        var saved = BossRaidProfile.FindProfile(reloaded, "br-save")!;

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("br-save", payload["profile_id"]!.GetValue<string>());
        Assert.Equal("After", state["active_profile_name"]!.GetValue<string>());
        Assert.Equal("After", saved.ProfileName);
        Assert.Equal("2026-01-01T00:00:00Z", saved.CreatedAt);
        Assert.Equal(8.5, saved.Phases[0].Timelines[0].TimeSeconds);
    }

    [Fact]
    public void DeleteBossRaidProfilePersistsAndFallsBackActiveProfile()
    {
        var router = new BridgeRouter();
        var settings = NewSettings();
        var alpha = BossRaidProfile.MakeDefaultProfile() with { Id = "br-a", ProfileName = "Alpha" };
        var beta = BossRaidProfile.MakeDefaultProfile() with { Id = "br-b", ProfileName = "Beta" };
        var config = BossRaidProfile.UpsertProfile(BossRaidProfile.DefaultConfig(), alpha, activate: true);
        config = BossRaidProfile.UpsertProfile(config, beta);
        BossRaidConfigStore.Save(settings, config);
        using var bridge = new MenuStateBridge(router, settings, new GameStateManager());

        var reply = router.Dispatch(Cmd(BridgeCommands.DeleteBossRaidProfile, new JsonObject
        {
            ["id"] = "br-a",
        }));
        var payload = reply!.Payload!;
        var state = payload["state"]!.AsObject();
        var reloaded = BossRaidConfigStore.Load(new SettingsManager(settings.Path));

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("br-b", state["active_profile_id"]!.GetValue<string>());
        Assert.Equal("Beta", state["active_profile_name"]!.GetValue<string>());
        Assert.Single(reloaded.Profiles);
        Assert.Equal("br-b", reloaded.ActiveProfileId);
    }

    [Fact]
    public void ExportBossRaidProfileWritesSelectedProfileToExportDirectory()
    {
        var router = new BridgeRouter();
        var settings = NewSettings();
        var exportDir = Path.Combine(_workDir, "exports");
        var profile = BossRaidProfile.MakeDefaultProfile() with
        {
            Id = "br-export",
            ProfileName = "Export Boss",
        };
        var config = BossRaidProfile.UpsertProfile(BossRaidProfile.DefaultConfig(), profile, activate: true);
        BossRaidConfigStore.Save(settings, config);
        using var bridge = new MenuStateBridge(
            router,
            settings,
            new GameStateManager(),
            bossRaidExportDirProvider: () => exportDir);

        var reply = router.Dispatch(Cmd(BridgeCommands.ExportBossRaid, new JsonObject
        {
            ["id"] = "br-export",
        }));
        var payload = reply!.Payload!;
        var path = payload["path"]!.GetValue<string>();
        var content = File.ReadAllText(path);

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.True(path.StartsWith(exportDir, StringComparison.Ordinal));
        Assert.Contains("Export Boss", content);
        Assert.Contains("\"profile_name\"", content);
    }

    [Fact]
    public void DisposeUnregistersCommands()
    {
        var router = new BridgeRouter();
        var settings = NewSettings();
        var bridge = new MenuStateBridge(router, settings);

        Assert.Contains(BridgeCommands.GetAutoKeyState, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.GetBossRaidState, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBossRaidEnabled, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBossRaidActiveProfile, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.CreateBossRaidProfile, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SaveBossRaidProfile, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DeleteBossRaidProfile, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ExportBossRaid, router.RegisteredCommands);

        bridge.Dispose();

        Assert.DoesNotContain(BridgeCommands.GetAutoKeyState, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.GetBossRaidState, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBossRaidEnabled, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBossRaidActiveProfile, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.CreateBossRaidProfile, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SaveBossRaidProfile, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.DeleteBossRaidProfile, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ExportBossRaid, router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachRegistersAndDisposesMenuStateCommands()
    {
        var settings = NewSettings();
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachMenuState(settings);
        lifecycle.AttachMenuState(settings);

        Assert.Contains(BridgeCommands.GetAutoKeyState, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.GetBossRaidState, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBossRaidEnabled, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBossRaidActiveProfile, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.CreateBossRaidProfile, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SaveBossRaidProfile, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DeleteBossRaidProfile, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ExportBossRaid, lifecycle.Router.RegisteredCommands);

        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.GetAutoKeyState));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());

        lifecycle.Dispose();

        Assert.DoesNotContain(BridgeCommands.GetAutoKeyState, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.GetBossRaidState, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBossRaidEnabled, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBossRaidActiveProfile, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.CreateBossRaidProfile, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SaveBossRaidProfile, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.DeleteBossRaidProfile, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ExportBossRaid, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachAfterDisposeThrows()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.Dispose();

        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachMenuState(NewSettings()));
    }
}
