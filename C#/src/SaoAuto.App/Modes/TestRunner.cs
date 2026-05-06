using Microsoft.Extensions.Logging;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Logging;
using SaoAuto.Core.State;

namespace SaoAuto.App.Modes;

/// <summary>
/// Mirrors <c>main.run_test</c>: locate the game window once, execute one capture,
/// print results. Recognition/window code is not wired yet (Plan §6) so we report
/// a controlled "not implemented" status while preserving exit-code semantics.
/// </summary>
public sealed class TestRunner
{
    private readonly SettingsManager _settings;
    private readonly GameStateManager _states;
    private readonly ILogger _log;

    public TestRunner(SettingsManager settings, GameStateManager states, ILogger? logger = null)
    {
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _states = states ?? throw new ArgumentNullException(nameof(states));
        _log = logger ?? SaoLog.For("test");
    }

    public Task<int> RunAsync(CancellationToken cancellationToken)
    {
        Console.WriteLine("==================================================");
        Console.WriteLine($"  SaoAuto {AppVersion.Label} - Recognition test");
        Console.WriteLine("==================================================");

        GameStateCache.Load(_settings, _states);
        var snap = _states.Snapshot;

        Console.WriteLine();
        Console.WriteLine("Cached state from settings.json:");
        Console.WriteLine($"  player_name : {snap.PlayerName}");
        Console.WriteLine($"  level       : {snap.LevelText}");
        Console.WriteLine($"  player_id   : {snap.PlayerId}");
        Console.WriteLine($"  hp          : {snap.HpText} ({snap.HpPct:P0})");
        Console.WriteLine($"  stamina_pct : {snap.StaminaPct:P1}");
        Console.WriteLine($"  profession  : {snap.ProfessionId} {snap.ProfessionName}");
        Console.WriteLine();

        Console.WriteLine("Recognition pipeline: not implemented in this build.");
        Console.WriteLine("Vision/window capture lands in Session 6 (see plan-saoAutoCsharpPort.prompt.md).");
        _log.LogInformation("Test run completed (recognition pipeline pending Session 6)");
        return Task.FromResult(0);
    }
}
