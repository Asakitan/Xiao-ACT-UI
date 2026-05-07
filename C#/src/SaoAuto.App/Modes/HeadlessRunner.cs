using Microsoft.Extensions.Logging;
using SaoAuto.App.Startup;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Logging;
using SaoAuto.Core.State;

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

        // S96/S97 — wire the recognition pipeline via the shared
        // best-effort lifecycle. A failure here (no game window, GDI
        // init issue) must not block headless from publishing cache.
        using var recognition = RecognitionLifecycle.Start(
            () => RecognitionPipelineBootstrap.Build(_settings, _states, _log),
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
