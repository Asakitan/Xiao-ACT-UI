using SaoAuto.App.Startup;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Updater;

namespace SaoAuto.Tests.App;

/// <summary>
/// S172 — Pin <see cref="UpdaterLifecycle"/> + <see
/// cref="WebBridgeLifecycle.AttachUpdater"/>. Mirrors S169's attach
/// suite: settings-gated startup, null delegates when inactive, real
/// delegates when active, bridge registers the three updater commands.
/// </summary>
public class Session172UpdaterLifecycleTests
{
    private static string FreshSettingsPath()
        => Path.Combine(Path.GetTempPath(), $"saoauto-s172-{Guid.NewGuid():N}.json");

    [Fact]
    public void DisabledByDefaultIsInactive()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        using var life = UpdaterLifecycle.Start(settings);
        Assert.False(life.IsActive);
        Assert.Null(life.Check);
        Assert.Null(life.Download);
        Assert.Null(life.Apply);
        Assert.NotNull(life.StateMachine);
    }

    [Fact]
    public void EnabledStartsActiveWithDelegates()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        settings.Set(SettingsKeys.UpdateCheckEnabled, true);
        var clientCalls = 0;
        var loopCtor = 0;
        using var life = UpdaterLifecycle.Start(
            settings,
            clientFactory: () => { clientCalls++; return new HttpUpdateClient(); },
            loopFactory: (opts, client, machine) =>
            {
                loopCtor++;
                return new UpdateClientLoop(opts, client, machine);
            });
        Assert.True(life.IsActive);
        Assert.NotNull(life.Check);
        Assert.NotNull(life.Download);
        Assert.NotNull(life.Apply);
        Assert.NotNull(life.Skip);
        Assert.Equal(1, clientCalls);
        Assert.Equal(1, loopCtor);
    }

    [Fact]
    public async Task SkipPersistsLatestVersionAndClearsState()
    {
        var path = FreshSettingsPath();
        var settings = new SettingsManager(path);
        settings.Set(SettingsKeys.UpdateCheckEnabled, true);
        using var life = UpdaterLifecycle.Start(
            settings,
            loopFactory: (opts, client, machine) => new UpdateClientLoop(opts, client, machine));
        var manifest = new UpdateManifest(
            Version: "9.9.9",
            Channel: "stable",
            Target: "windows-x64",
            PackageUrl: "http://example/pkg.zip",
            PackageSha256: "",
            PackageSize: 1,
            Kind: UpdatePackageKind.Full,
            Notes: null,
            PublishedAt: DateTimeOffset.UtcNow);
        life.StateMachine.CheckCompleted(manifest, "1.0.0");

        var skipped = await life.Skip!(CancellationToken.None);

        Assert.True(skipped);
        Assert.Equal(UpdaterStatus.NoUpdate, life.StateMachine.Snapshot.Status);
        var reloaded = new SettingsManager(path);
        Assert.Equal("9.9.9", reloaded.GetString(SettingsKeys.UpdateSkippedVersion));
    }

    [Fact]
    public void FactoryFailureSwallowsAndStaysInactive()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        settings.Set(SettingsKeys.UpdateCheckEnabled, true);
        using var life = UpdaterLifecycle.Start(
            settings,
            clientFactory: () => throw new InvalidOperationException("boom"));
        Assert.False(life.IsActive);
        Assert.Null(life.Check);
    }

    [Fact]
    public void DisposeIsIdempotent()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        var life = UpdaterLifecycle.Start(settings);
        life.Dispose();
        life.Dispose();
    }

    [Fact]
    public void AttachUpdaterRegistersCommands()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        var states = new SaoAuto.Core.State.GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        using var life = UpdaterLifecycle.Start(settings);
        web.AttachUpdater(life);
        Assert.Contains(BridgeCommands.CheckUpdate, web.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DownloadUpdate, web.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ApplyUpdate, web.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SkipUpdate, web.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachUpdaterBroadcastsStateChanged()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        var states = new SaoAuto.Core.State.GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        using var life = UpdaterLifecycle.Start(settings);
        var captured = new List<BridgeMessage>();
        web.Broadcaster.Posted += msg => captured.Add(msg);
        web.AttachUpdater(life);
        life.StateMachine.BeginCheck();
        Assert.Contains(captured, m =>
            m.Type == BridgeMessage.TypeEvent && m.Name == BridgeEvents.UpdaterStatus);
    }

    [Fact]
    public void AttachUpdaterIsIdempotent()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        var states = new SaoAuto.Core.State.GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        using var life = UpdaterLifecycle.Start(settings);
        web.AttachUpdater(life);
        web.AttachUpdater(life); // must not throw
        Assert.Contains(BridgeCommands.CheckUpdate, web.Router.RegisteredCommands);
    }

    [Fact]
    public void NullSettingsThrows()
    {
        Assert.Throws<ArgumentNullException>(() => UpdaterLifecycle.Start(null!));
    }

    [Fact]
    public void AttachUpdaterNullThrows()
    {
        var states = new SaoAuto.Core.State.GameStateManager();
        using var web = new WebBridgeLifecycle(states);
        Assert.Throws<ArgumentNullException>(() => web.AttachUpdater(null!));
    }
}
