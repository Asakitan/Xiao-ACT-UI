using SaoAuto.App.Startup;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Updater;

namespace SaoAuto.Tests.App;

/// <summary>
/// S173 — Pin the <c>update_auto_poll</c> opt-in. Default off keeps
/// <see cref="UpdateClientLoop.Start"/> from running; opt-in true
/// kicks the pump so background checks happen on the configured
/// <see cref="UpdateClientLoopOptions.PollInterval"/>.
/// </summary>
public class Session173UpdaterAutoPollTests
{
    private static string FreshSettingsPath()
        => Path.Combine(Path.GetTempPath(), $"saoauto-s173-{Guid.NewGuid():N}.json");

    private static UpdateClientLoopOptions FastPollOptions() =>
        new(
            BaseUrl: "http://localhost:1",
            Channel: "stable",
            Target: "windows-x64",
            CurrentVersion: "0.0.0",
            StagingDirectory: Path.Combine(Path.GetTempPath(), "saoauto-s173-staging"),
            PollInterval: TimeSpan.FromHours(1))
        {
            InitialDelay = TimeSpan.FromHours(1), // never actually ticks
            AutoDownload = false,
            AutoApply = false,
        };

    [Fact]
    public void DefaultDoesNotStartBackgroundPoll()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        settings.Set(SettingsKeys.UpdateCheckEnabled, true);
        using var life = UpdaterLifecycle.Start(
            settings,
            optionsFactory: FastPollOptions,
            clientFactory: () => new HttpUpdateClient());
        Assert.True(life.IsActive);
        Assert.False(life.IsBackgroundPollRunning);
    }

    [Fact]
    public void AutoPollSettingStartsLoop()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        settings.Set(SettingsKeys.UpdateCheckEnabled, true);
        settings.Set(SettingsKeys.UpdateAutoPoll, true);
        using var life = UpdaterLifecycle.Start(
            settings,
            optionsFactory: FastPollOptions,
            clientFactory: () => new HttpUpdateClient());
        Assert.True(life.IsBackgroundPollRunning);
    }

    [Fact]
    public void DisposeStopsBackgroundPoll()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        settings.Set(SettingsKeys.UpdateCheckEnabled, true);
        settings.Set(SettingsKeys.UpdateAutoPoll, true);
        var life = UpdaterLifecycle.Start(
            settings,
            optionsFactory: FastPollOptions,
            clientFactory: () => new HttpUpdateClient());
        Assert.True(life.IsBackgroundPollRunning);
        life.Dispose();
        Assert.False(life.IsBackgroundPollRunning);
    }

    [Fact]
    public void AutoPollWithoutCheckEnabledStaysInactive()
    {
        var settings = new SettingsManager(FreshSettingsPath());
        // UpdateCheckEnabled is false (default); AutoPoll alone should not bring
        // the lifecycle up — the gate is the check-enabled key.
        settings.Set(SettingsKeys.UpdateAutoPoll, true);
        using var life = UpdaterLifecycle.Start(settings);
        Assert.False(life.IsActive);
        Assert.False(life.IsBackgroundPollRunning);
    }
}
