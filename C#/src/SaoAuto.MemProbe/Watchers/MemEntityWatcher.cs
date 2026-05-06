using System.Buffers.Binary;
using System.Collections.Concurrent;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.MemProbe.Watchers;

/// <summary>
/// Per-entity-type read recipe. Mirrors Python <c>EntityReadConfig</c>.
/// FLAT layout (<see cref="AttrSlotOff"/> &lt; 0): all fields read from
/// the obj body. NESTED layout (<see cref="AttrSlotOff"/> &gt;= 0): the
/// obj body holds uuid + a ptr at <see cref="AttrSlotOff"/> to the attr
/// object; fields whose name starts with <c>"attr."</c> read from there.
/// </summary>
public sealed class EntityReadConfig
{
    public ulong KlassPtr { get; init; }
    /// <summary>(name, off, width)</summary>
    public IReadOnlyList<(string Name, int Off, int Width)> FieldSpecs { get; init; } = Array.Empty<(string, int, int)>();
    public int BodySize { get; init; } = 0x200;
    public string Name { get; init; } = "monster";
    public int AttrSlotOff { get; init; } = -1;
    public int AttrBodySize { get; init; } = 0x200;
    /// <summary>logical name → 'i32'|'i64'|'f32'|'f64' (default uint passthrough).</summary>
    public IReadOnlyDictionary<string, string> FieldEncodings { get; init; } = new Dictionary<string, string>();
}

public sealed class EntityState
{
    public ulong Uuid { get; set; }
    public ulong ObjAddr { get; set; }
    public long Hp { get; set; }
    public long MaxHp { get; set; }
    public int IsDead { get; set; }
    public int ProfessionId { get; set; }
    public long MaxExtinction { get; set; }
    public long Extinction { get; set; }
    public double LastSeenTs { get; set; }
    public (long, long, int, long, long) LastEmitSig { get; set; }
}

public sealed record EntityUpdate
{
    public ulong Uuid { get; init; }
    public long MaxHp { get; init; }
    public long Hp { get; init; }
    public long MaxExtinction { get; init; }
    public bool IsDead { get; init; }
    public int ProfessionId { get; init; }
}

/// <summary>
/// Monster / nearby-player tracker. Two cadences:
/// fast-path (100 ms) increment-reads HP/extinction/is_dead for known
/// objs; discovery-path (1 s) re-scans private regions for the configured
/// <see cref="EntityReadConfig.KlassPtr"/> values to detect new entities.
/// Mirrors <c>mem_probe.entity_watcher.MemEntityWatcher</c>.
/// </summary>
public sealed class MemEntityWatcher : IDisposable
{
    public TimeSpan FastInterval { get; init; } = TimeSpan.FromMilliseconds(100);
    public TimeSpan DiscoveryInterval { get; init; } = TimeSpan.FromSeconds(1);
    public TimeSpan DropAfter { get; init; } = TimeSpan.FromSeconds(3);
    public int MaxRegionSize { get; init; } = 256 * 1024 * 1024;

    public Action<EntityUpdate>? OnMonsterUpdate { get; set; }
    public Action<string, string>? OnStatusChange { get; set; }

    private readonly IMemorySource _pm;
    private readonly IReadOnlyList<EntityReadConfig> _configs;
    private readonly Dictionary<ulong, EntityReadConfig> _byKlass;
    private readonly ConcurrentDictionary<ulong, EntityState> _entities = new();
    private readonly object _objsLock = new();
    private Dictionary<ulong, ulong> _knownObjs = new();   // obj → klass
    private readonly ILogger _log;

    private CancellationTokenSource? _cts;
    private Thread? _thread;
    private DateTimeOffset _lastDiscovery = DateTimeOffset.MinValue;
    private int _failCount, _tickCount;

    public MemEntityWatcher(IMemorySource pm, IReadOnlyList<EntityReadConfig> configs, ILogger<MemEntityWatcher>? logger = null)
    {
        _pm = pm; _configs = configs;
        _byKlass = configs.ToDictionary(c => c.KlassPtr, c => c);
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public IReadOnlyDictionary<ulong, EntityState> Entities() =>
        _entities.ToDictionary(kv => kv.Key, kv => kv.Value);

    public void Start()
    {
        if (_thread is { IsAlive: true }) return;
        _cts = new CancellationTokenSource();
        var ct = _cts.Token;
        _thread = new Thread(() => Loop(ct)) { Name = "mem-entity-watcher", IsBackground = true };
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
                var now = DateTimeOffset.UtcNow;
                if (now - _lastDiscovery >= DiscoveryInterval) { _lastDiscovery = now; DiscoveryPass(); }
                FastPass();
                DropStale(now);
            }
            catch (Exception ex) { _log.LogWarning(ex, "[MemEntity] tick"); _failCount++; }
            try { Task.Delay(FastInterval, ct).Wait(ct); } catch { }
        }
    }

    private void DiscoveryPass()
    {
        var fresh = new Dictionary<ulong, ulong>();
        foreach (var cfg in _configs)
        {
            try
            {
                foreach (var addr in ScanKlass(cfg.KlassPtr)) fresh[addr] = cfg.KlassPtr;
            }
            catch (Exception ex) { _log.LogWarning(ex, "[MemEntity] discovery {Klass:X}", cfg.KlassPtr); }
        }
        lock (_objsLock) _knownObjs = fresh;
    }

    public IEnumerable<ulong> ScanKlass(ulong klassPtr)
    {
        foreach (var region in _pm.IterRegions(onlyReadable: true, onlyPrivate: true))
        {
            if (region.Size > (ulong)MaxRegionSize) continue;
            var buf = _pm.ReadBytes(region.Base, (int)region.Size);
            if (buf is null) continue;
            foreach (var off in CyMemScan.FindAlignedU64(buf, klassPtr)) yield return region.Base + (ulong)off;
        }
    }

    public void FastPass()
    {
        Dictionary<ulong, ulong> snapshot;
        lock (_objsLock) snapshot = new Dictionary<ulong, ulong>(_knownObjs);
        if (snapshot.Count == 0) return;

        var byKlass = new Dictionary<ulong, List<ulong>>();
        foreach (var (obj, kp) in snapshot)
            (byKlass.TryGetValue(kp, out var lst) ? lst : (byKlass[kp] = new List<ulong>())).Add(obj);

        foreach (var (kp, objs) in byKlass)
        {
            if (!_byKlass.TryGetValue(kp, out var cfg)) continue;
            var objSpecs = new List<(string Name, int Off, int Width)>();
            var attrSpecs = new List<(string Name, int Off, int Width)>();
            foreach (var (n, o, w) in cfg.FieldSpecs)
            {
                if (n.StartsWith("attr.", StringComparison.Ordinal)) attrSpecs.Add((n[5..], o, w));
                else objSpecs.Add((n, o, w));
            }

            foreach (var obj in objs)
            {
                var blob = _pm.ReadBytes(obj, cfg.BodySize);
                if (blob is null) continue;
                var fields = new Dictionary<string, long>();
                var objVals = CyMemScan.UnpackStructFields(blob, objSpecs.Select(s => (s.Off, s.Width)).ToList());
                for (var i = 0; i < objSpecs.Count; i++)
                    fields[objSpecs[i].Name] = Reinterpret(cfg, objSpecs[i].Name, objVals[i], objSpecs[i].Width);

                if (attrSpecs.Count > 0 && cfg.AttrSlotOff >= 0)
                {
                    if (cfg.AttrSlotOff + 8 > blob.Length) continue;
                    var attrPtr = BinaryPrimitives.ReadUInt64LittleEndian(blob.AsSpan(cfg.AttrSlotOff, 8));
                    if (attrPtr is < 0x10000UL or > 0x7FFF_FFFF_FFFFUL) continue;
                    var attrBlob = _pm.ReadBytes(attrPtr, cfg.AttrBodySize);
                    if (attrBlob is null) continue;
                    var attrVals = CyMemScan.UnpackStructFields(attrBlob, attrSpecs.Select(s => (s.Off, s.Width)).ToList());
                    for (var i = 0; i < attrSpecs.Count; i++)
                        fields[attrSpecs[i].Name] = Reinterpret(cfg, attrSpecs[i].Name, attrVals[i], attrSpecs[i].Width);
                }

                UpdateEntity(obj, fields);
            }
        }
    }

    private static long Reinterpret(EntityReadConfig cfg, string name, ulong rawUint, int width)
    {
        if (cfg.FieldEncodings.TryGetValue(name, out var enc))
        {
            if (enc == "f32" && width == 4)
            {
                Span<byte> b = stackalloc byte[4];
                BinaryPrimitives.WriteUInt32LittleEndian(b, (uint)rawUint);
                return (long)BitConverter.ToSingle(b);
            }
            if (enc == "f64" && width == 8)
            {
                Span<byte> b = stackalloc byte[8];
                BinaryPrimitives.WriteUInt64LittleEndian(b, rawUint);
                return (long)BitConverter.ToDouble(b);
            }
        }
        return (long)rawUint;
    }

    private void UpdateEntity(ulong obj, Dictionary<string, long> fields)
    {
        var uuid = (ulong)fields.GetValueOrDefault("uuid", 0);
        if (uuid == 0) return;
        var hp = fields.GetValueOrDefault("hp", 0);
        var maxHp = fields.GetValueOrDefault("max_hp", 0);
        var isDead = (int)fields.GetValueOrDefault("is_dead", 0);
        var profession = (int)fields.GetValueOrDefault("profession_id", 0);
        var maxExt = fields.GetValueOrDefault("max_extinction", 0);
        var ext = fields.GetValueOrDefault("extinction", 0);
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0;

        var state = _entities.GetOrAdd(uuid, _ => new EntityState { Uuid = uuid, ObjAddr = obj });
        bool emit;
        lock (state)
        {
            state.ObjAddr = obj;
            state.Hp = hp; state.MaxHp = maxHp; state.IsDead = isDead;
            state.ProfessionId = profession; state.MaxExtinction = maxExt; state.Extinction = ext;
            state.LastSeenTs = now;
            var sig = (hp, maxHp, isDead, maxExt, ext);
            emit = !sig.Equals(state.LastEmitSig);
            state.LastEmitSig = sig;
        }
        if (emit && OnMonsterUpdate is not null)
        {
            try
            {
                OnMonsterUpdate(new EntityUpdate
                {
                    Uuid = uuid,
                    MaxHp = maxHp,
                    Hp = hp,
                    MaxExtinction = maxExt,
                    IsDead = isDead != 0,
                    ProfessionId = profession,
                });
            }
            catch (Exception ex) { _log.LogWarning(ex, "OnMonsterUpdate"); }
        }
    }

    private void DropStale(DateTimeOffset now)
    {
        var nowSec = now.ToUnixTimeMilliseconds() / 1000.0;
        var threshold = DropAfter.TotalSeconds;
        var toDrop = new List<(ulong Uuid, EntityState State)>();
        foreach (var (uuid, state) in _entities)
        {
            if (nowSec - state.LastSeenTs > threshold) toDrop.Add((uuid, state));
        }
        foreach (var (uuid, state) in toDrop)
        {
            _entities.TryRemove(uuid, out _);
            if (state.IsDead == 0 && OnMonsterUpdate is not null)
            {
                try
                {
                    OnMonsterUpdate(new EntityUpdate
                    {
                        Uuid = uuid, MaxHp = state.MaxHp, Hp = 0,
                        MaxExtinction = state.MaxExtinction, IsDead = true, ProfessionId = state.ProfessionId,
                    });
                }
                catch { }
            }
        }
    }

    public object Health() => new
    {
        alive = _thread?.IsAlive ?? false, tick_count = _tickCount, fail_count = _failCount,
        n_entities = _entities.Count, n_known_objs = _knownObjs.Count, n_klass_configs = _configs.Count,
    };

    public void Dispose() { Stop(); _cts?.Dispose(); }
}
