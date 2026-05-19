using Microsoft.Extensions.Logging;
using SaoAuto.App.Startup;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Logging;
using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.App.Modes;

/// <summary>
/// Mirrors <c>main.run_headless</c>: no HUD, prints unified state every tick until cancelled.
/// Real automation/recognition wiring lands in Sessions 5–6; for now we publish whatever
/// snapshot the cache hydrated and tick until the cancellation token fires.
/// </summary>
public sealed class HeadlessRunner
{
    private readonly SettingsManager _settings;
    private readonly GameStateManager _states;
    private readonly ILogger _log;

    public HeadlessRunner(SettingsManager settings, GameStateManager states, ILogger? logger = null)
    {
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _states = states ?? throw new ArgumentNullException(nameof(states));
        _log = logger ?? SaoLog.For("headless");
    }

    public async Task<int> RunAsync(CancellationToken cancellationToken)
    {
        Console.WriteLine("==================================================");
        Console.WriteLine($"  SaoAuto {AppVersion.Label} — Headless mode");
        Console.WriteLine("  Press Ctrl+C to exit");
        Console.WriteLine("==================================================");

        GameStateCache.Load(_settings, _states);
        using var sub = _states.Subscribe(PrintStateLine);

        // Force one print of the cached snapshot so the operator sees something immediately.
        PrintStateLine(_states.Snapshot);

        using var packets = PacketLifecycle.Start(
            _settings, _states, _log, cancellationToken);

        // S96/S97 — wire the recognition pipeline via the shared
        // best-effort lifecycle. A failure here (no game window, GDI
        // init issue) must not block headless from publishing cache.
        // S138 — bundle exposes the locator so AutoKey can share the
        // foreground probe without re-discovering the window.
        WindowLocator? sharedLocator = null;
        using var recognition = RecognitionLifecycle.Start(
            () =>
            {
                var bundle = RecognitionPipelineBootstrap.BuildBundle(_settings, _states, _log);
                sharedLocator = bundle.Locator;
                return bundle.Host;
            },
            _log,
            cancellationToken);

        // S137/S138 — wire the AutoKey tick host via the shared
        // best-effort lifecycle. Foreground probe derives from the
        // recognition pipeline's WindowLocator when available.
        using var autoKey = AutoKeyLifecycle.Start(
            () => new AutoKeyTickHost(
                _settings,
                _states,
                new AutoKeySpecRuntime(new SendInputKeyDispatcher()),
                foregroundProbe: sharedLocator is null ? null : GameForegroundProbe.Bind(sharedLocator)),
            _log,
            cancellationToken);

        // S153 — HideSeek tick host. Factory throws when no locator is
        // available or assets are missing; HideSeekLifecycle logs and
        // continues so headless still runs.
        using var hideSeek = HideSeekLifecycle.Start(
            () => HideSeekBootstrap.Build(
                sharedLocator ?? throw new InvalidOperationException("HideSeek: no shared locator"),
                ResourcePathResolver.ForCurrentProcess(),
                log: _log),
            _log,
            cancellationToken);

        try
        {
            await Task.Delay(Timeout.Infinite, cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // expected on Ctrl+C
        }

        Console.WriteLine();
        Console.WriteLine("Exited cleanly.");
        _log.LogInformation("Headless run finished");
        return 0;
    }

    private static void PrintStateLine(GameState state)
    {
        if (state.RecognitionOk || state.PacketActive || state.HpMax > 0)
        {
            Console.Write(
                $"\r[{state.LevelText}] {state.PlayerName}  HP:{state.HpText} ({state.HpPct:P0})  STA:{state.StaminaText} ({state.StaminaPct:P0})  ID:{state.PlayerId}".PadRight(120));
        }
        else
        {
            Console.Write($"\rWaiting for data… ({state.ErrorMsg})".PadRight(120));
        }
    }
}
