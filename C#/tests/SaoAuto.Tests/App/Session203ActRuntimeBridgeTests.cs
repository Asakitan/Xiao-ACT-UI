using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

public sealed class Session203ActRuntimeBridgeTests : IDisposable
{
    private readonly string _workDir;

    public Session203ActRuntimeBridgeTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s203-" + Guid.NewGuid().ToString("N")[..8]);
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
    public void NativeRuntimeReturnsPluginManagerCompatibleEmptyStatus()
    {
        var runtime = new ActRuntime(NewSettings(), new GameStateManager());

        var payload = runtime.Handle(BridgeCommands.ActPluginsStatus, null);

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("csharp-native", payload["source"]!.GetValue<string>());
        Assert.Equal(0, payload["plugin_count"]!.GetValue<int>());
        Assert.Equal(0, payload["active_count"]!.GetValue<int>());
        Assert.Empty(payload["plugins"]!.AsArray());
        Assert.Empty(payload["ui_panels"]!.AsArray());
        Assert.NotNull(payload["event_bus"]!["topics"]);
    }

    [Fact]
    public void NativeRuntimeSupportsPluginPanelAndRenderContracts()
    {
        var runtime = new ActRuntime(NewSettings(), new GameStateManager());

        var panels = runtime.Handle(BridgeCommands.ActPluginsUiPanels, null);
        var render = runtime.Handle(BridgeCommands.ActPluginsRenderUiPanel, new JsonObject
        {
            ["panel_id"] = "missing-panel",
        });

        Assert.True(panels["ok"]!.GetValue<bool>());
        Assert.Empty(panels["panels"]!.AsArray());
        Assert.False(render["ok"]!.GetValue<bool>());
        Assert.Equal("missing-panel", render["panel_id"]!.GetValue<string>());
        Assert.Equal(1, render["spec"]!["version"]!.GetValue<int>());
        Assert.Empty(render["spec"]!["nodes"]!.AsArray());
    }

    [Fact]
    public void NativeRuntimePassesRenderHookPayloadThrough()
    {
        var runtime = new ActRuntime(NewSettings(), new GameStateManager());

        var payload = runtime.Handle(BridgeCommands.ActRenderApplyHooks, new JsonObject
        {
            ["surface"] = "menu",
            ["payload"] = new JsonObject { ["title"] = "SAO" },
        });

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("menu", payload["surface"]!.GetValue<string>());
        Assert.Equal("SAO", payload["payload"]!["title"]!.GetValue<string>());
        Assert.Empty(payload["applied"]!.AsArray());
    }

    [Fact]
    public void LifecycleAttachActRuntimeReplacesUnavailableBackend()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachActRuntime(NewSettings(), new GameStateManager());

        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.ActPluginsUiPanels));
        var payload = reply!.Payload!;

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Empty(payload["panels"]!.AsArray());
        Assert.NotEqual("act_unavailable", payload["error"]?.GetValue<string>());
    }

    [Fact]
    public void UnsupportedNativeCommandsAreExplicitButNotUnavailable()
    {
        var runtime = new ActRuntime(NewSettings(), new GameStateManager());

        var payload = runtime.Handle(BridgeCommands.ActPluginsEnable, new JsonObject
        {
            ["plugin_id"] = "demo",
        });

        Assert.False(payload["ok"]!.GetValue<bool>());
        Assert.Equal("unsupported", payload["error"]!.GetValue<string>());
        Assert.Equal(BridgeCommands.ActPluginsEnable, payload["command"]!.GetValue<string>());
    }
}