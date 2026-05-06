using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.MemProbe.Watchers;

public sealed class SceneReadConfig
{
    public ulong ObjAddr { get; init; }
    public int SceneIdOff { get; init; } = -1;
    public int DungeonIdOff { get; init; } = -1;
    public int LayerOff { get; init; } = -1;
}

/// <summary>
/// Scene/dungeon transition kinds emitted by <see cref="MemSceneWatcher"/>.
/// </summary>
public sealed record SceneEvent
{
    public string Kind { get; init; } = "hard";       // "hard" | "soft"
    public string Reason { get; init; } = "";          // dungeon_enter | dungeon_leave | layer_change | scene_restart
    public bool PreserveCombat { get; init; }
    public bool ResetOnNextDamage { get; init; }
    public int DungeonId { get; init; }
    public int SceneId { get; init; }
    public int FromSceneId { get; init; }
}

/// <summary>
/// Polls SceneManager.scene_id / dungeon_id / layer (500 ms cadence) and
/// classifies transitions. Mirrors
/// <c>mem_probe.scene_watcher.MemSceneWatcher</c>.
/// </summary>
public sealed class MemSceneWatcher : IDisposable
{
    public TimeSpan PollInterval { get; init; } = TimeSpan.FromMilliseconds(500);

    public Action<SceneEvent>? OnSceneChange { get; set; }
    public Action<string, string>? OnStatusChange { get; set; }

    private readonly IMemorySource _pm;
    private readonly SceneReadConfig _cfg;
    private readonly ILogger _log;

    private CancellationTokenSource? _cts;
    private Thread? _thread;
    private int _lastSceneId = -1, _lastDungeonId = -1, _lastLayer = -1;
    private int _failCount, _tickCount;

    public MemSceneWatcher(IMemorySource pm, SceneReadConfig cfg, ILogger<MemSceneWatcher>? logger = null)
    {
        _pm = pm; _cfg = cfg; _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public void Start()
    {
        if (_thread is { IsAlive: true }) return;
        _cts = new CancellationTokenSource();
        var ct = _cts.Token;
        _thread = new Thread(() => Loop(ct)) { Name = "mem-scene-watcher", IsBackground = true };
        _thread.Start();
    }

    public void Stop(TimeSpan? timeout = null)
    {
        _cts?.Cancel();
        if (_thread is { } t && t != Thread.CurrentThread) t.Join(timeout ?? TimeSpan.FromSeconds(1));
        _thread = null;
    }

    private void Loop(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                _tickCount++;
                var state = ReadState();
                if (state is null) _failCount++;
                else { _failCount = 0; DetectChange(state.Value); }
            }
            catch (Exception ex) { _log.LogWarning(ex, "[MemScene] tick"); _failCount++; }
            try { Task.Delay(PollInterval, ct).Wait(ct); } catch { }
        }
    }

    private (int Scene, int Dungeon, int Layer)? ReadState()
    {
        if (_cfg.ObjAddr == 0) return null;
        int s = 0, d = 0, l = 0;
        if (_cfg.SceneIdOff >= 0) s = _pm.ReadI32(_cfg.ObjAddr + (ulong)_cfg.SceneIdOff) ?? 0;
        if (_cfg.DungeonIdOff >= 0) d = _pm.ReadI32(_cfg.ObjAddr + (ulong)_cfg.DungeonIdOff) ?? 0;
        if (_cfg.LayerOff >= 0) l = _pm.ReadI32(_cfg.ObjAddr + (ulong)_cfg.LayerOff) ?? 0;
        return (s, d, l);
    }

    public void DetectChange((int Scene, int Dungeon, int Layer) state)
    {
        var (sceneId, dungeonId, layer) = state;
        if (_lastSceneId < 0) { _lastSceneId = sceneId; _lastDungeonId = dungeonId; _lastLayer = layer; return; }
        if (sceneId == _lastSceneId && dungeonId == _lastDungeonId && layer == _lastLayer) return;

        var prevD = _lastDungeonId; var prevS = _lastSceneId;
        SceneEvent ev;
        if (prevD == 0 && dungeonId != 0)
            ev = new SceneEvent { Kind = "hard", Reason = "dungeon_enter", DungeonId = dungeonId, SceneId = sceneId };
        else if (prevD != 0 && dungeonId == 0)
            ev = new SceneEvent { Kind = "hard", Reason = "dungeon_leave", DungeonId = prevD, SceneId = sceneId };
        else if (prevD == dungeonId && prevS != sceneId)
            ev = new SceneEvent { Kind = "soft", Reason = "layer_change", PreserveCombat = true, DungeonId = dungeonId, SceneId = sceneId, FromSceneId = prevS };
        else
            ev = new SceneEvent { Kind = "hard", Reason = "scene_restart", DungeonId = dungeonId, SceneId = sceneId };

        _lastSceneId = sceneId; _lastDungeonId = dungeonId; _lastLayer = layer;
        try { OnSceneChange?.Invoke(ev); } catch (Exception ex) { _log.LogWarning(ex, "OnSceneChange"); }
    }

    public object Health() => new { alive = _thread?.IsAlive ?? false, tick_count = _tickCount, fail_count = _failCount, scene_id = _lastSceneId, dungeon_id = _lastDungeonId, layer = _lastLayer };
    public void Dispose() { Stop(); _cts?.Dispose(); }
}
