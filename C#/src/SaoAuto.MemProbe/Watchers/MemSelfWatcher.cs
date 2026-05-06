using System.Buffers.Binary;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.MemProbe.Watchers;

/// <summary>
/// SELF polling thread + on_self_update callback. Reads CharSerialize /
/// UserFightAttr / substructs at 50 ms cadence; emits
/// <see cref="OnSelfUpdate"/> throttled to 80 ms (matches Python TCP-path
/// node). Uses persisted offsets so it works even when an IL2CPP dump.cs
/// is stale. Mirrors <c>mem_probe.self_watcher.MemSelfWatcher</c>.
/// </summary>
public sealed class MemSelfWatcher : IDisposable
{
    public TimeSpan PollInterval { get; init; } = TimeSpan.FromMilliseconds(50);
    public int EmitThrottleMs { get; init; } = 80;
    public TimeSpan LifeCheckInterval { get; init; } = TimeSpan.FromSeconds(5);
    public int MaxConsecutiveFails { get; init; } = 20;

    public Action<SelfSnapshot>? OnSelfUpdate { get; set; }
    public Action<string, string>? OnStatusChange { get; set; }
    public Action? OnRelocateNeeded { get; set; }

    private readonly IMemorySource _pm;
    private readonly SelfReadConfig _cfg;
    private readonly ILogger _log;
    private readonly object _lock = new();

    private CancellationTokenSource? _cts;
    private Thread? _thread;
    private SelfSnapshot? _last;
    private (ulong, long, long, int, int, long, int) _lastEmitSig;
    private double _lastEmitMs;
    private int _failCount;
    private int _tickCount;

    public MemSelfWatcher(IMemorySource pm, SelfReadConfig cfg, ILogger<MemSelfWatcher>? logger = null)
    {
        _pm = pm;
        _cfg = cfg;
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public void Start()
    {
        if (_thread is { IsAlive: true }) return;
        _cts = new CancellationTokenSource();
        var ct = _cts.Token;
        _thread = new Thread(() => Loop(ct)) { Name = "mem-self-watcher", IsBackground = true };
        _thread.Start();
    }

    public void Stop(TimeSpan? timeout = null)
    {
        _cts?.Cancel();
        if (_thread is { } t && t != Thread.CurrentThread) t.Join(timeout ?? TimeSpan.FromSeconds(1));
        _thread = null;
    }

    public SelfSnapshot? Latest() { lock (_lock) return _last; }

    private void Loop(CancellationToken ct)
    {
        var lastLifeCheck = DateTimeOffset.UtcNow;
        while (!ct.IsCancellationRequested)
        {
            try
            {
                _tickCount++;
                var snap = ReadSnapshot();
                if (snap is null)
                {
                    _failCount++;
                    if (_failCount >= MaxConsecutiveFails)
                    {
                        Notify("error", $"SELF read failed {_failCount} times");
                        try { OnRelocateNeeded?.Invoke(); } catch (Exception ex) { _log.LogWarning(ex, "relocate cb"); }
                        _failCount = 0;
                    }
                }
                else
                {
                    _failCount = 0;
                    MaybeEmit(snap);
                }

                var now = DateTimeOffset.UtcNow;
                if (now - lastLifeCheck >= LifeCheckInterval)
                {
                    lastLifeCheck = now;
                    if (!ValidateAlive())
                    {
                        Notify("error", "SELF refs no longer alive");
                        try { OnRelocateNeeded?.Invoke(); } catch (Exception ex) { _log.LogWarning(ex, "relocate cb"); }
                    }
                }
            }
            catch (Exception ex) { _log.LogWarning(ex, "[MemSelf] tick"); }
            try { Task.Delay(PollInterval, ct).Wait(ct); } catch { }
        }
    }

    private SelfSnapshot? ReadSnapshot()
    {
        var charBlob = _pm.ReadBytes(_cfg.CharObj, 0x300);
        if (charBlob is null) return null;
        var span = (ReadOnlySpan<byte>)charBlob;

        ulong uid = 0;
        if (_cfg.UidOff >= 0 && _cfg.UidOff + 8 <= span.Length)
            uid = BinaryPrimitives.ReadUInt64LittleEndian(span.Slice(_cfg.UidOff, 8));
        if (_cfg.CharId != 0 && uid != _cfg.CharId) return null;

        var attrObj = _cfg.AttrObj;
        if (_cfg.AttrSlotOff >= 0 && _cfg.AttrSlotOff + 8 <= span.Length)
            attrObj = BinaryPrimitives.ReadUInt64LittleEndian(span.Slice(_cfg.AttrSlotOff, 8));

        long curHp = 0, maxHp = 0;
        if (attrObj != 0 && _cfg.CurHpOff >= 0 && _cfg.MaxHpOff >= 0)
        {
            var attrBlob = _pm.ReadBytes(attrObj, 0x100);
            if (attrBlob is not null)
            {
                curHp = ReadLittleEndian(attrBlob, _cfg.CurHpOff, _cfg.HpWidth);
                maxHp = ReadLittleEndian(attrBlob, _cfg.MaxHpOff, _cfg.HpWidth);
            }
        }

        var level = ReadSubstructField(charBlob, _cfg.RoleLevelSlotOff, _cfg.RoleLevelFieldOff, 4);
        var professionId = ReadSubstructField(charBlob, _cfg.ProfessionListSlotOff, _cfg.ProfessionFieldOff, 4);
        var energyMax = ReadSubstructField(charBlob, _cfg.EnergyItemSlotOff, _cfg.EnergyFieldOff, 4);
        var fightPoint = ReadSubstructField(charBlob, _cfg.CharBaseSlotOff, _cfg.FightPointFieldOff, 4);

        var snap = new SelfSnapshot
        {
            Uid = uid,
            Uuid = uid,
            Level = (int)level,
            Hp = curHp,
            MaxHp = maxHp,
            ProfessionId = (int)professionId,
            FightPoint = fightPoint,
            StaminaMax = (int)energyMax,
            EnergyTotal = (int)energyMax,
            Timestamp = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0,
        };
        lock (_lock) _last = snap;
        return snap;
    }

    private long ReadSubstructField(byte[] charBlob, int slotOff, int fieldOff, int width)
    {
        if (slotOff < 0 || fieldOff < 0 || slotOff + 8 > charBlob.Length) return 0;
        var subPtr = BinaryPrimitives.ReadUInt64LittleEndian(charBlob.AsSpan(slotOff, 8));
        if (subPtr is < 0x10000UL or > 0x7FFF_FFFF_FFFFUL) return 0;
        var blob = _pm.ReadBytes(subPtr + (ulong)fieldOff, width);
        if (blob is null) return 0;
        return ReadLittleEndian(blob, 0, width);
    }

    private static long ReadLittleEndian(byte[] buf, int off, int width)
    {
        if (off < 0 || off + width > buf.Length) return 0;
        return width switch
        {
            1 => buf[off],
            2 => BinaryPrimitives.ReadUInt16LittleEndian(buf.AsSpan(off, 2)),
            4 => BinaryPrimitives.ReadUInt32LittleEndian(buf.AsSpan(off, 4)),
            8 => (long)BinaryPrimitives.ReadUInt64LittleEndian(buf.AsSpan(off, 8)),
            _ => 0,
        };
    }

    private bool ValidateAlive()
    {
        if (_pm.ReadBytes(_cfg.CharObj, 8) is null) return false;
        var uidBytes = _pm.ReadBytes(_cfg.CharObj + (ulong)Math.Max(_cfg.UidOff, 0), 8);
        if (uidBytes is null) return false;
        var uid = BinaryPrimitives.ReadUInt64LittleEndian(uidBytes);
        if (_cfg.CharId != 0 && uid != _cfg.CharId) return false;
        return true;
    }

    private void MaybeEmit(SelfSnapshot snap)
    {
        if (OnSelfUpdate is null) return;
        var sig = (snap.Uid, snap.Hp, snap.MaxHp, snap.Level, snap.ProfessionId, snap.FightPoint, snap.StaminaMax);
        var nowMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (sig.Equals(_lastEmitSig) && (nowMs - _lastEmitMs) < EmitThrottleMs) return;
        _lastEmitSig = sig;
        _lastEmitMs = nowMs;
        try { OnSelfUpdate(snap); } catch (Exception ex) { _log.LogWarning(ex, "OnSelfUpdate"); }
    }

    private void Notify(string status, string error)
    {
        if (OnStatusChange is null) return;
        try { OnStatusChange(status, error); } catch { }
    }

    public object Health() => new
    {
        alive = _thread?.IsAlive ?? false,
        tick_count = _tickCount,
        fail_count = _failCount,
        char_obj = $"0x{_cfg.CharObj:X}",
        attr_obj = $"0x{_cfg.AttrObj:X}",
        char_id = _cfg.CharId,
    };

    public void Dispose() { Stop(); _cts?.Dispose(); }

    /// <summary>Build a config from <c>anchors.json</c>'s <c>smart_locator</c> block.</summary>
    public static SelfReadConfig? FromAnchors(MemoryAnchorStore anchors)
    {
        var sl = anchors.SmartLocator;
        // v2 nested
        var selfAnchor = anchors.GetV2Anchor("self");
        if (selfAnchor is not null)
        {
            var subs = selfAnchor["substructs"]?.AsObject();
            int Slot(string n) => subs?[n]?.AsObject()?["slot_off"] is { } v ? MemoryAnchorStore.GetInt(v) : -1;
            int Fld(string n) => subs?[n]?.AsObject()?["discovered_field_off"] is { } v ? MemoryAnchorStore.GetInt(v) : -1;
            ulong attr = 0;
            if (subs?["user_fight_attr"]?.AsObject() is { } ufa)
                attr = MemoryAnchorStore.ParseHex(ufa["obj_addr"]);
            return new SelfReadConfig
            {
                CharObj = MemoryAnchorStore.ParseHex(selfAnchor["obj_addr"]),
                UidOff = MemoryAnchorStore.GetInt(selfAnchor["uid_off"]),
                AttrSlotOff = MemoryAnchorStore.GetInt(selfAnchor["attr_slot_off"]),
                CharBaseSlotOff = Slot("char_base"),
                RoleLevelSlotOff = Slot("role_level"),
                ProfessionListSlotOff = Slot("profession_list"),
                EnergyItemSlotOff = Slot("energy_item"),
                SeasonMedalSlotOff = Slot("season_medal_info"),
                AttrObj = attr,
                CurHpOff = MemoryAnchorStore.GetInt(selfAnchor["cur_hp_off"]),
                MaxHpOff = MemoryAnchorStore.GetInt(selfAnchor["max_hp_off"]),
                HpWidth = MemoryAnchorStore.GetInt(selfAnchor["hp_width"], 8),
                RoleLevelFieldOff = Fld("role_level"),
                ProfessionFieldOff = Fld("profession_list"),
                EnergyFieldOff = Fld("energy_item"),
                FightPointFieldOff = Fld("char_base"),
                CharId = (ulong)Math.Max(MemoryAnchorStore.GetInt(sl["known_uid"], 0), 0),
            };
        }
        // v1 flat
        var charObj = MemoryAnchorStore.ParseHex(sl["last_self_obj"]);
        if (charObj == 0) return null;
        return new SelfReadConfig
        {
            CharObj = charObj,
            UidOff = MemoryAnchorStore.GetInt(sl["last_uid_off"]),
            AttrSlotOff = MemoryAnchorStore.GetInt(sl["last_attr_slot_off"]),
            AttrObj = MemoryAnchorStore.ParseHex(sl["last_user_fight_attr"]),
            CurHpOff = MemoryAnchorStore.GetInt(sl["last_cur_hp_off"]),
            MaxHpOff = MemoryAnchorStore.GetInt(sl["last_max_hp_off"]),
            HpWidth = MemoryAnchorStore.GetInt(sl["last_hp_width"], 8),
            CharId = (ulong)Math.Max(MemoryAnchorStore.GetInt(sl["known_uid"], 0), 0),
        };
    }
}
