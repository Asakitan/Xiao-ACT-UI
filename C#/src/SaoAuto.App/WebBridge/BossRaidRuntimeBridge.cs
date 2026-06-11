using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Narrow runtime controls used by raid_editor.html. The bridge executes
/// real BossRaidEngine operations; if the engine is idle, operations are
/// no-op success just like the defensive Python overlay API.
/// </summary>
public sealed class BossRaidRuntimeBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly BossRaidEngine _engine;
    private readonly string[] _commands;
    private bool _disposed;

    public BossRaidRuntimeBridge(BridgeRouter router, BossRaidEngine engine)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _engine = engine ?? throw new ArgumentNullException(nameof(engine));
        _commands = new[] { BridgeCommands.RaidNextPhase, BridgeCommands.RaidReset };
        _router.Register(BridgeCommands.RaidNextPhase, _ => HandleNextPhase());
        _router.Register(BridgeCommands.RaidReset, _ => HandleReset());
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var command in _commands)
            _router.Unregister(command);
    }

    private JsonObject HandleNextPhase()
    {
        _engine.NextPhase();
        return Status("next_phase");
    }

    private JsonObject HandleReset()
    {
        _engine.Stop();
        return Status("reset");
    }

    private JsonObject Status(string command)
    {
        var phase = _engine.CurrentPhase;
        return new JsonObject
        {
            ["ok"] = true,
            ["command"] = command,
            ["running"] = _engine.Running,
            ["phase_idx"] = phase?.Index ?? -1,
            ["phase_name"] = phase?.Name ?? string.Empty,
            ["enrage_remaining_s"] = _engine.EnrageRemainingSeconds,
        };
    }
}
