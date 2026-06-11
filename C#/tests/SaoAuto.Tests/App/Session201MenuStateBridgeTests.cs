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
    public void DisposeUnregistersCommands()
    {
        var router = new BridgeRouter();
        var settings = NewSettings();
        var bridge = new MenuStateBridge(router, settings);

        Assert.Contains(BridgeCommands.GetAutoKeyState, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.GetBossRaidState, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBossRaidEnabled, router.RegisteredCommands);

        bridge.Dispose();

        Assert.DoesNotContain(BridgeCommands.GetAutoKeyState, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.GetBossRaidState, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBossRaidEnabled, router.RegisteredCommands);
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

        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.GetAutoKeyState));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());

        lifecycle.Dispose();

        Assert.DoesNotContain(BridgeCommands.GetAutoKeyState, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.GetBossRaidState, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SetBossRaidEnabled, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachAfterDisposeThrows()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.Dispose();

        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachMenuState(NewSettings()));
    }
}
