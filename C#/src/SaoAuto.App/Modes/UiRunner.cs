using System.Windows;
using Microsoft.Extensions.Logging;
using SaoAuto.App.Hosting;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Logging;
using SaoAuto.Core.State;

namespace SaoAuto.App.Modes;

/// <summary>
/// Drives the WPF UI loop with the same fallback chain as <c>main.run_ui</c>:
/// try each host in <see cref="ModeRouter.BuildChain"/> order, fall back to
/// <see cref="HeadlessRunner"/> if every host throws or reports
/// <see cref="IUiHost.IsAvailable"/> = <c>false</c>.
/// </summary>
public sealed class UiRunner
{
    private readonly SettingsManager _settings;
    private readonly GameStateManager _states;
    private readonly IUiHostFactory _hostFactory;
    private readonly ILogger _log;

    public UiRunner(
        SettingsManager settings,
        GameStateManager states,
        IUiHostFactory? hostFactory = null,
        ILogger? logger = null)
    {
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _states = states ?? throw new ArgumentNullException(nameof(states));
        _hostFactory = hostFactory ?? new DefaultUiHostFactory();
        _log = logger ?? SaoLog.For("ui");
    }

    public int Run(CancellationToken cancellationToken)
    {
        var requested = _settings.NormalizedUiMode;
        var chain = ModeRouter.BuildChain(requested);
        _log.LogInformation("UI mode requested={Requested} chain=[{Chain}]", requested, string.Join(", ", chain));

        GameStateCache.Load(_settings, _states);

        var application = (System.Windows.Application.Current as App) ?? new App();
        application.DispatcherUnhandledException += (_, e) =>
        {
            _log.LogError(e.Exception, "WPF dispatcher unhandled exception");
        };

        // Honor cancellation by asking the dispatcher to shut down.
        using var ctRegistration = cancellationToken.Register(() =>
        {
            try
            {
                application.Dispatcher.Invoke(() =>
                {
                    application.Shutdown(130);
                });
            }
            catch
            {
                // dispatcher already torn down — fine
            }
        });

        foreach (var modeName in chain)
        {
            if (cancellationToken.IsCancellationRequested) break;
            IUiHost host;
            try
            {
                host = _hostFactory.Create(modeName);
            }
            catch (Exception ex)
            {
                _log.LogError(ex, "host factory failed for {Mode}", modeName);
                continue;
            }

            if (!host.IsAvailable)
            {
                _log.LogWarning("host {Mode} reports not available; trying next", modeName);
                continue;
            }

            Window window;
            try
            {
                window = host.CreateMainWindow();
            }
            catch (Exception ex)
            {
                _log.LogError(ex, "host {Mode} failed to build window; falling through", modeName);
                continue;
            }

            try
            {
                _log.LogInformation("starting {Mode} host window", modeName);
                return application.Run(window);
            }
            catch (Exception ex)
            {
                _log.LogError(ex, "host {Mode} crashed during Application.Run", modeName);
                // Application.Run is one-shot per Application instance; stop trying further hosts here
                // and fall to headless rather than corrupting the loop state.
                break;
            }
        }

        _log.LogWarning("all UI hosts failed; falling back to headless");
        return new HeadlessRunner(_settings, _states, SaoLog.For("headless"))
            .RunAsync(cancellationToken)
            .GetAwaiter()
            .GetResult();
    }
}
