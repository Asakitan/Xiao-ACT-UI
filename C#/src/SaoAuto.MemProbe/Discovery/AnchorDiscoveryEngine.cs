using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.MemProbe.Discovery;

/// <summary>
/// Bootstrap loop for memory anchors. Mirrors the workflow of
/// <c>mem_probe/discover_entities.py</c> + <c>entity_discovery.py</c>:
///
///   1. Collect ground-truth (uuid, hp, max_hp) observations from
///      higher layers (PacketBridge usually wires this up).
///   2. Once enough distinct truths are seen, run a value-anchor scan
///      across all readable regions of the target process.
///   3. Return the candidate addresses; caller persists them to
///      <see cref="MemoryAnchorStore"/> when confident.
///
/// Heavy compute path: <see cref="CyMemScan.FindAlignedU64InSet"/>
/// (the C# port of the AVX2 Cython helper). The discovery engine only
/// owns the loop, the budgeting, and the truth-collection contract.
/// </summary>
public sealed class AnchorDiscoveryEngine
{
    private readonly object _gate = new();
    private readonly Dictionary<ulong, EntityTruth> _truths = new();
    private readonly int _minTruths;
    private readonly ILogger _log;

    public AnchorDiscoveryEngine(int minTruths = 2, ILogger<AnchorDiscoveryEngine>? logger = null)
    {
        if (minTruths < 1) throw new ArgumentOutOfRangeException(nameof(minTruths));
        _minTruths = minTruths;
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public int MinTruths => _minTruths;

    public int TruthCount
    {
        get { lock (_gate) return _truths.Count; }
    }

    public bool ReadyToScan
    {
        get { lock (_gate) return _truths.Count >= _minTruths; }
    }

    public IReadOnlyList<EntityTruth> SnapshotTruths()
    {
        lock (_gate) return _truths.Values.ToArray();
    }

    /// <summary>
    /// Record a ground-truth observation. Updates an existing entry only when
    /// the new max_hp grows (or hp grows under the same max_hp) — mirrors the
    /// Python <c>_MonsterCollector</c> logic: prefer the highest stable
    /// max_hp so the value-anchor scan locks onto a long-lived address.
    /// </summary>
    public void Observe(EntityTruth truth)
    {
        if (truth.Uuid == 0 || truth.MaxHp <= 0) return;
        lock (_gate)
        {
            if (!_truths.TryGetValue(truth.Uuid, out var prev))
            {
                _truths[truth.Uuid] = truth;
                return;
            }
            if (truth.MaxHp > prev.MaxHp)
            {
                _truths[truth.Uuid] = truth;
            }
            else if (truth.MaxHp == prev.MaxHp && truth.Hp > prev.Hp)
            {
                _truths[truth.Uuid] = truth;
            }
        }
    }

    public void Clear()
    {
        lock (_gate) _truths.Clear();
    }

    /// <summary>
    /// Run the value-anchor pass across the supplied memory source.
    /// Walks readable, private regions; for each region's bytes uses
    /// <see cref="CyMemScan.FindAlignedU64InSet"/> to find offsets where
    /// any known uuid appears, then validates HP / MaxHP at the
    /// canonical relative offsets (HP = uuid+0x10, MaxHP = uuid+0x18 —
    /// the most-frequently-seen layout from Python anchors.json).
    /// </summary>
    public DiscoveryResult Scan(IMemorySource source, int hpRelOffset = 0x10, int maxHpRelOffset = 0x18)
    {
        ArgumentNullException.ThrowIfNull(source);

        EntityTruth[] truths;
        lock (_gate) truths = _truths.Values.ToArray();

        if (truths.Length < _minTruths)
        {
            return new DiscoveryResult(truths.Length, Array.Empty<EntityHit>());
        }

        var truthByUuid = truths.ToDictionary(t => t.Uuid);
        var uuidSet = truths.Select(t => t.Uuid).ToArray();
        var hits = new List<EntityHit>();

        foreach (var region in source.IterRegions(onlyReadable: true, onlyPrivate: true))
        {
            var bytes = source.ReadBytes(region.Base, (int)Math.Min(region.Size, int.MaxValue));
            if (bytes is null || bytes.Length < 8) continue;

            var found = CyMemScan.FindAlignedU64InSet(bytes, uuidSet);
            foreach (var (offset, needle) in found)
            {
                if (!truthByUuid.TryGetValue(needle, out var truth)) continue;
                if (offset + maxHpRelOffset + 8 > bytes.Length) continue;

                var hpAddr = (ulong)(offset + hpRelOffset);
                var maxAddr = (ulong)(offset + maxHpRelOffset);
                if ((int)hpAddr + 8 > bytes.Length || (int)maxAddr + 8 > bytes.Length) continue;

                var observedHp = System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(bytes.AsSpan((int)hpAddr, 8));
                var observedMax = System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(bytes.AsSpan((int)maxAddr, 8));
                if (observedMax != truth.MaxHp) continue;
                if (observedHp != truth.Hp) continue;

                hits.Add(new EntityHit(region.Base, offset, needle, observedHp, observedMax));
            }
        }

        _log.LogInformation("[AnchorDiscovery] {Truths} truths, {Hits} hits", truths.Length, hits.Count);
        return new DiscoveryResult(truths.Length, hits);
    }
}
