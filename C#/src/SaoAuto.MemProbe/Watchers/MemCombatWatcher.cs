using System.Buffers.Binary;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.MemProbe.Watchers;

public sealed class CombatReadConfig
{
    public ulong SelfAttrObj { get; init; }
    public int InCombatOff { get; init; } = -1;
    public int BuffListOff { get; init; } = -1;
    public int BuffListCountOff { get; init; } = 24;
    public int BuffListArrayOff { get; init; } = 16;
    public int BuffArrayElemsOff { get; init; } = 32;
    public int BuffStructSize { get; init; } = 0x30;
    public int BuffIdFieldOff { get; init; } = 16;
}

public sealed record BossEvent
{
    public int EventType { get; init; }
    public ulong HostUuid { get; init; }
    public int BuffId { get; init; }
    public ulong SourceUid { get; init; }
}

/// <summary>
/// Polls SELF in_combat (50 ms) and per-entity buff lists (200 ms),
/// diffing buff_id sets to fire boss events. Mirrors
/// <c>mem_probe.combat_watcher.MemCombatWatcher</c>. Tracked buff types
/// (47 shield_broken, 51 super_armor_broken, 58 enter_breaking,
/// 88 into_fracture_state) match the TCP path.
/// </summary>
public sealed class MemCombatWatcher : IDisposable
{
    public TimeSpan CombatPollInterval { get; init; } = TimeSpan.FromMilliseconds(50);
    public TimeSpan BuffPollInterval { get; init; } = TimeSpan.FromMilliseconds(200);
    public static readonly HashSet<int> TrackedBuffTypes = new() { 47, 51, 58, 88 };

    public Action<bool>? OnCombatChange { get; set; }
    public Action<BossEvent>? OnBossEvent { get; set; }
    public Action<string, string>? OnStatusChange { get; set; }

    private readonly IMemorySource _pm;
    private readonly CombatReadConfig _cfg;
    private readonly Func<IReadOnlyDictionary<ulong, EntityState>>? _entityProvider;
    private readonly Dictionary<ulong, HashSet<int>> _buffsPerEntity = new();
    private readonly ILogger _log;

    private CancellationTokenSource? _cts;
    private Thread? _thread;
    private bool? _lastInCombat;
    private DateTimeOffset _lastBuffPoll = DateTimeOffset.MinValue;
    private int _failCount, _tickCount;

    public MemCombatWatcher(IMemorySource pm, CombatReadConfig cfg,
        Func<IReadOnlyDictionary<ulong, EntityState>>? entityProvider = null,
        ILogger<MemCombatWatcher>? logger = null)
    {
        _pm = pm; _cfg = cfg; _entityProvider = entityProvider;
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public void Start()
    {
        if (_thread is { IsAlive: true }) return;
        _cts = new CancellationTokenSource();
        var ct = _cts.Token;
        _thread = new Thread(() => Loop(ct)) { Name = "mem-combat-watcher", IsBackground = true };
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
                PollInCombat();
                var now = DateTimeOffset.UtcNow;
                if (now - _lastBuffPoll >= BuffPollInterval) { _lastBuffPoll = now; PollBuffs(); }
            }
            catch (Exception ex) { _log.LogWarning(ex, "[MemCombat] tick"); _failCount++; }
            try { Task.Delay(CombatPollInterval, ct).Wait(ct); } catch { }
        }
    }

    public void PollInCombat()
    {
        if (_cfg.SelfAttrObj == 0 || _cfg.InCombatOff < 0) return;
        var v = _pm.ReadI32(_cfg.SelfAttrObj + (ulong)_cfg.InCombatOff);
        if (v is null) return;
        var inCombat = v.Value != 0;
        if (inCombat == _lastInCombat) return;
        _lastInCombat = inCombat;
        try { OnCombatChange?.Invoke(inCombat); } catch (Exception ex) { _log.LogWarning(ex, "OnCombatChange"); }
    }

    public void PollBuffs()
    {
        if (_entityProvider is null || _cfg.BuffListOff < 0) return;
        IReadOnlyDictionary<ulong, EntityState> entities;
        try { entities = _entityProvider(); } catch { return; }
        foreach (var (uuid, state) in entities)
        {
            try { PollEntityBuffs(uuid, state); } catch { }
        }
    }

    private void PollEntityBuffs(ulong uuid, EntityState state)
    {
        var attrObj = state.ObjAddr;
        if (attrObj == 0) return;
        var listPtr = _pm.ReadU64(attrObj + (ulong)_cfg.BuffListOff);
        if (listPtr is null or < 0x10000UL or > 0x7FFF_FFFF_FFFFUL) return;
        var header = _pm.ReadBytes(listPtr.Value, 0x30);
        if (header is null || header.Length < 32) return;
        var count = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(_cfg.BuffListCountOff, 4));
        var current = new HashSet<int>();
        if (count > 0 && count <= 256)
        {
            var arrPtr = BinaryPrimitives.ReadUInt64LittleEndian(header.AsSpan(_cfg.BuffListArrayOff, 8));
            if (arrPtr is >= 0x10000UL and <= 0x7FFF_FFFF_FFFFUL)
            {
                var totalBytes = _cfg.BuffArrayElemsOff + count * _cfg.BuffStructSize;
                var arrBlob = _pm.ReadBytes(arrPtr, totalBytes);
                if (arrBlob is not null && arrBlob.Length >= totalBytes)
                {
                    for (var i = 0; i < count; i++)
                    {
                        var b = _cfg.BuffArrayElemsOff + i * _cfg.BuffStructSize;
                        if (b + _cfg.BuffIdFieldOff + 4 > arrBlob.Length) break;
                        current.Add(BinaryPrimitives.ReadInt32LittleEndian(arrBlob.AsSpan(b + _cfg.BuffIdFieldOff, 4)));
                    }
                }
            }
        }

        if (!_buffsPerEntity.TryGetValue(uuid, out var last)) last = new HashSet<int>();
        foreach (var bid in current)
        {
            if (!last.Contains(bid) && TrackedBuffTypes.Contains(bid))
            {
                try
                {
                    OnBossEvent?.Invoke(new BossEvent { EventType = bid, HostUuid = uuid, BuffId = bid });
                }
                catch (Exception ex) { _log.LogWarning(ex, "OnBossEvent"); }
            }
        }
        _buffsPerEntity[uuid] = current;
    }

    public object Health() => new
    {
        alive = _thread?.IsAlive ?? false, tick_count = _tickCount, fail_count = _failCount,
        in_combat = _lastInCombat, n_entities_buffs = _buffsPerEntity.Count,
    };
    public void Dispose() { Stop(); _cts?.Dispose(); }
}
